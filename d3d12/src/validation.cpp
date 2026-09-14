// Validation messages (validation.h): the debug layer's info queue as the UI's ValidationMessage
// stream, deduplicated the way vulkan/src/validation.cpp deduplicates the messenger's output.
//
// Two ways in. ID3D12InfoQueue1::RegisterMessageCallback (Windows 11, or the Agility SDK) calls
// back on the application's thread as the message is produced, inside the D3D12 call that caused
// it, so a message fired in a command list method the library is recording can be attached to
// that command. Without it the device's queue is drained at every present. Either way every
// message goes through OnMessage under one mutex.
#include "validation.h"

#include "capture.h"
#include "command_recorder.h"
#include "d3d12_enums.gen.h"
#include "tracker.h"
#include "transport.h"

#include <d3d12sdklayers.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dxinsp {

namespace {

// Unique messages kept per process; beyond this new ones are only counted.
constexpr size_t kMaxEntries = 2000;

struct Entry {
    uint64_t key = 0;
    std::string severity;
    std::vector<std::string> types;
    bool hasIdName = false;
    std::string idName;
    int64_t idNumber = 0;
    std::string message;
    uint64_t frame = 0;
    uint64_t count = 0;
    uint64_t cmdBufferId = 0;
    int64_t cmdSlot = -1;
    bool dirty = false;    // its count changed since the last flush
    bool resend = false;   // its command reference moved: goes out in full at the next flush
};

/** A device's info queue: the interface pointer only (see OnDeviceCreated), and how it delivers. */
struct DeviceQueue {
    ID3D12Device* device = nullptr;
    ID3D12InfoQueue* queue = nullptr;
    bool polled = false;
    DWORD cookie = 0;
};

const char* SeverityName(D3D12_MESSAGE_SEVERITY s) {
    switch (s) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
        case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
        case D3D12_MESSAGE_SEVERITY_INFO: return "info";
        default: return "verbose";
    }
}

// "D3D12_MESSAGE_CATEGORY_STATE_CREATION" -> "state_creation", the UI's `types` spelling.
std::string CategoryName(D3D12_MESSAGE_CATEGORY c) {
    const char* name = ToString_D3D12_MESSAGE_CATEGORY(c);
    if (!name) return std::to_string((int)c);
    std::string s = name;
    const char* prefix = "D3D12_MESSAGE_CATEGORY_";
    if (s.compare(0, strlen(prefix), prefix) == 0) s.erase(0, strlen(prefix));
    for (char& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

// The same message about the same objects every frame is one message counted; the text names
// the objects, so another object's is a message of its own.
uint64_t DedupeKey(int64_t id, const char* text) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](uint8_t b) { h ^= b; h *= 1099511628211ull; };
    for (size_t i = 0; i < sizeof(id); ++i) mix((uint8_t)((uint64_t)id >> (8 * i)));
    mix('|');
    for (const char* p = text; p && *p; ++p) mix((uint8_t)*p);
    return h;
}

// The log's state, a type of its own so the info queue's callback (which cannot name the
// class's private Impl) can be handed it as its context.
struct LogState {
    std::mutex mutex;
    std::vector<Entry> entries;
    std::unordered_map<uint64_t, size_t> byKey;   // dedupe hash -> index into entries
    uint64_t dropped = 0;
    bool droppedDirty = false;
    bool anyDirty = false;
    std::atomic<uint64_t> frame{0};

    std::mutex devicesMutex;
    std::vector<DeviceQueue> devices;
    std::once_flag enableOnce;

    std::string MessageJson(const Entry& e) const;
    void OnMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, const char* text, bool inCallback);
    void AddNote(const std::string& text);
    void Flush();
};

void __stdcall MessageCallback(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                               LPCSTR description, void* context) {
    static_cast<LogState*>(context)->OnMessage(category, severity, id, description, true);
}

}  // namespace

struct ValidationLog::Impl : LogState {};

// ---------------------------------------------------------------------------------------------
// Messages

std::string LogState::MessageJson(const Entry& e) const {
    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("ValidationMessage");
    w.Key("key"); w.Uint(e.key);
    w.Key("severity"); w.String(e.severity);
    w.Key("types"); w.BeginArray();
    for (auto& t : e.types) w.String(t);
    w.EndArray();
    w.Key("idName"); if (e.hasIdName) w.String(e.idName); else w.Null();
    w.Key("idNumber"); w.Int(e.idNumber);
    w.Key("message"); w.String(e.message);
    w.Key("frame"); w.Uint(e.frame);
    w.Key("count"); w.Uint(e.count);
    // D3D12 messages name their objects only in the text.
    w.Key("objects"); w.BeginArray(); w.EndArray();
    if (e.cmdBufferId && e.cmdSlot >= 0) {
        w.Key("command"); w.BeginObject();
        w.Key("commandBuffer"); w.Uint(e.cmdBufferId);
        w.Key("slot"); w.Uint((uint64_t)e.cmdSlot);
        w.EndObject();
    }
    w.EndObject();
    return std::move(w.str());
}

void LogState::OnMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                         const char* text, bool inCallback) {
    if (!text) text = "";
    uint64_t dedupe = DedupeKey((int64_t)id, text);
    // The command being recorded on this thread, when the message fired inside it during a
    // capture: the recording the capture will show. Only the callback runs inside the call;
    // a polled message has no thread of its own.
    uint64_t cmdBufferId = 0;
    uint32_t cmdSlot = 0;
    bool hasCommand = inCallback && CaptureManager::Get().IsCapturing() && CommandScope::Current(cmdBufferId, cmdSlot) && cmdBufferId;

    std::lock_guard<std::mutex> lock(mutex);
    auto it = byKey.find(dedupe);
    if (it != byKey.end()) {
        Entry& e = entries[it->second];
        e.count++;
        e.dirty = true;
        if (hasCommand && (cmdBufferId != e.cmdBufferId || (int64_t)cmdSlot != e.cmdSlot)) {
            e.cmdBufferId = cmdBufferId;
            e.cmdSlot = (int64_t)cmdSlot;
            e.resend = true;
        }
        anyDirty = true;
        return;
    }
    if (entries.size() >= kMaxEntries) {
        dropped++;
        droppedDirty = true;
        anyDirty = true;
        return;
    }
    Entry e;
    e.key = entries.size() + 1;
    e.severity = SeverityName(severity);
    e.types.push_back(CategoryName(category));
    if (const char* name = ToString_D3D12_MESSAGE_ID(id)) {
        e.hasIdName = true;
        e.idName = name;
    }
    e.idNumber = (int64_t)id;
    e.message = text;
    e.frame = frame.load(std::memory_order_relaxed);
    e.count = 1;
    if (hasCommand) {
        e.cmdBufferId = cmdBufferId;
        e.cmdSlot = (int64_t)cmdSlot;
    }
    byKey.emplace(dedupe, entries.size());
    entries.push_back(std::move(e));
    const Entry& added = entries.back();
    if (LogEnabled()) Log("validation %s: %s", added.severity.c_str(), added.message.c_str());
    if (Transport::Get().Connected()) Transport::Get().SendJson(MessageJson(added));
}

void LogState::AddNote(const std::string& text) {
    uint64_t dedupe = DedupeKey(0, text.c_str());
    std::lock_guard<std::mutex> lock(mutex);
    auto it = byKey.find(dedupe);
    if (it != byKey.end()) {
        entries[it->second].count++;
        entries[it->second].dirty = true;
        anyDirty = true;
        return;
    }
    if (entries.size() >= kMaxEntries) {
        dropped++;
        droppedDirty = true;
        anyDirty = true;
        return;
    }
    Entry e;
    e.key = entries.size() + 1;
    e.severity = "info";
    e.types.push_back("inspector");
    e.idNumber = 0;
    e.message = text;
    e.frame = frame.load(std::memory_order_relaxed);
    e.count = 1;
    byKey.emplace(dedupe, entries.size());
    entries.push_back(std::move(e));
    Log("note: %s", text.c_str());
    if (Transport::Get().Connected()) Transport::Get().SendJson(MessageJson(entries.back()));
}

void LogState::Flush() {
    if (!anyDirty || !Transport::Get().Connected()) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (!anyDirty) return;
    // Messages whose command reference moved go out in full; the rest as counts.
    for (Entry& e : entries) {
        if (!e.resend) continue;
        Transport::Get().SendJson(MessageJson(e));
        e.resend = false;
        e.dirty = false;
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ValidationCount");
    w.Key("counts"); w.BeginArray();
    for (Entry& e : entries) {
        if (!e.dirty) continue;
        w.BeginArray(); w.Uint(e.key); w.Uint(e.count); w.EndArray();
        e.dirty = false;
    }
    w.EndArray();
    if (droppedDirty) {
        w.Key("dropped"); w.Uint(dropped);
        droppedDirty = false;
    }
    w.EndObject();
    anyDirty = false;
    Transport::Get().SendJson(std::move(w.str()));
}

// ---------------------------------------------------------------------------------------------
// ValidationLog

ValidationLog& ValidationLog::Get() {
    static ValidationLog* instance = new ValidationLog();
    return *instance;
}

ValidationLog::Impl& ValidationLog::impl() {
    if (!_impl) _impl = new Impl();
    return *_impl;
}

bool ValidationLog::DebugLayerRequested() {
    static int requested = -1;
    if (requested < 0) requested = ConfigFlag("DXINSP_DEBUG_LAYER") ? 1 : 0;
    return requested == 1;
}

void ValidationLog::EnableDebugLayer() {
    if (!DebugLayerRequested()) return;
    std::call_once(impl().enableOnce, [] {
        // d3d12.dll is loaded already: the library loaded it to hook its exports.
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
        auto getDebugInterface = d3d12 ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
        if (!getDebugInterface) {
            LogAlways("debug layer: D3D12GetDebugInterface not found; validation messages will not be reported");
            return;
        }
        ScopedInternal internal;
        ComPtr<ID3D12Debug> debug;
        HRESULT hr = getDebugInterface(IID_PPV_ARGS(debug.put()));
        if (FAILED(hr) || !debug) {
            LogAlways("debug layer: D3D12GetDebugInterface failed (%s): is the Graphics Tools optional feature installed?", HrText(hr).c_str());
            return;
        }
        debug->EnableDebugLayer();
        LogAlways("debug layer enabled");
    });
}

void ValidationLog::OnDeviceCreated(ID3D12Device* device) {
    if (!device || !DebugLayerRequested()) return;
    Impl& i = impl();
    ScopedInternal internal;
    // The info queue is an interface of the device itself, and a reference of ours would keep
    // the application's last Release from reaching zero (which is how the library learns the
    // device is gone). So the reference is dropped at once and the pointer kept: it is valid
    // as long as the device is, and nothing touches it after OnDeviceReleased.
    ID3D12InfoQueue* queue = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
        Log("validation: the device has no ID3D12InfoQueue (debug layer not active)");
        return;
    }
    queue->Release();
    DeviceQueue dq;
    dq.device = device;
    dq.queue = queue;
    ID3D12InfoQueue1* queue1 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue1))) && queue1) {
        queue1->Release();
        HRESULT hr = queue1->RegisterMessageCallback(&MessageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, static_cast<LogState*>(&i), &dq.cookie);
        if (SUCCEEDED(hr)) {
            Log("validation: message callback registered");
        } else {
            Log("validation: RegisterMessageCallback failed (%s): the info queue is polled at every present", HrText(hr).c_str());
            dq.polled = true;
        }
    } else {
        Log("validation: no ID3D12InfoQueue1: the info queue is polled at every present");
        dq.polled = true;
    }
    std::lock_guard<std::mutex> lock(i.devicesMutex);
    i.devices.push_back(dq);
}

void ValidationLog::OnDeviceReleased(ID3D12Device* device) {
    if (!_impl) return;
    Impl& i = *_impl;
    // Called once the device's count reached zero: the queue is gone with it, so nothing is
    // unregistered or released here, only forgotten.
    std::lock_guard<std::mutex> lock(i.devicesMutex);
    for (size_t k = 0; k < i.devices.size(); ++k) {
        if (i.devices[k].device != device) continue;
        i.devices[k] = i.devices.back();
        i.devices.pop_back();
        break;
    }
}

void ValidationLog::Poll(uint64_t frame) {
    if (!_impl) return;
    Impl& i = *_impl;
    i.frame.store(frame, std::memory_order_relaxed);
    std::vector<ID3D12InfoQueue*> polled;
    {
        std::lock_guard<std::mutex> lock(i.devicesMutex);
        for (const DeviceQueue& dq : i.devices)
            if (dq.polled) polled.push_back(dq.queue);
    }
    if (!polled.empty()) {
        ScopedInternal internal;
        std::vector<uint8_t> buffer;
        for (ID3D12InfoQueue* queue : polled) {
            UINT64 count = queue->GetNumStoredMessages();
            for (UINT64 m = 0; m < count; ++m) {
                SIZE_T length = 0;
                if (FAILED(queue->GetMessage(m, nullptr, &length)) || length < sizeof(D3D12_MESSAGE)) continue;
                buffer.resize(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (FAILED(queue->GetMessage(m, message, &length))) continue;
                std::string text = message->pDescription ? std::string(message->pDescription, message->DescriptionByteLength) : std::string();
                while (!text.empty() && text.back() == '\0') text.pop_back();
                i.OnMessage(message->Category, message->Severity, message->ID, text.c_str(), false);
            }
            if (count) queue->ClearStoredMessages();
        }
    }
    i.Flush();
}

void ValidationLog::SendSnapshot() {
    Impl& i = impl();
    std::lock_guard<std::mutex> lock(i.mutex);
    for (Entry& e : i.entries) {
        Transport::Get().SendJson(i.MessageJson(e));
        e.dirty = false;
        e.resend = false;
    }
    if (i.dropped) {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("ValidationCount");
        w.Key("counts"); w.BeginArray(); w.EndArray();
        w.Key("dropped"); w.Uint(i.dropped);
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
        i.droppedDirty = false;
    }
    i.anyDirty = false;
}

void ValidationLog::Note(const std::string& text) {
    impl().AddNote(text);
}

}  // namespace dxinsp
