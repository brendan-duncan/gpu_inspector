// Incoming messages from the inspector UI (ui_messages.h), dispatched to the modules that answer
// them: the Vulkan layer's HandleUiMessage (src/vulkan/src/layer.cpp) with D3D12 spellings.
//
// The handler runs on the transport's receiver thread while the application renders, so every
// answer comes from state the modules guard themselves (the tracker, the descriptor tracker, the
// shader editor) and the two that need the GPU (RequestImage, ReplaceShader) do their own
// synchronization inside.
#include "ui_messages.h"

#include "capture.h"
#include "common.h"
#include "descriptors.h"
#include "image_readback.h"
#include "json_parse.h"
#include "shader_edit.h"
#include "stacktrace.h"
#include "tracker.h"
#include "transport.h"
#include "validation.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace dxinsp {

namespace {

using vkinsp::JsonParser;
using vkinsp::JsonValue;

// Standard and URL-safe alphabets, padding and line breaks ignored (the UI sends standard base64).
bool DecodeBase64(const std::string& text, std::vector<uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    out.clear();
    out.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xff));
        }
    }
    return true;
}

const char* DescriptorKindName(DescriptorKind kind) {
    switch (kind) {
        case DescriptorKind::CBV: return "CBV";
        case DescriptorKind::SRV: return "SRV";
        case DescriptorKind::UAV: return "UAV";
        case DescriptorKind::Sampler: return "Sampler";
        case DescriptorKind::RTV: return "RTV";
        case DescriptorKind::DSV: return "DSV";
        default: return nullptr;
    }
}

void SendShaderReplaced(uint64_t pipeline, const std::string& stage, bool ok, const std::string& error,
                        const std::string& note, uint64_t replacement) {
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ShaderReplaced");
    w.Key("pipeline"); w.Uint(pipeline);
    w.Key("stage"); w.String(stage);
    w.Key("ok"); w.Boolean(ok);
    if (!error.empty()) { w.Key("error"); w.String(error); }
    if (!note.empty()) { w.Key("note"); w.String(note); }
    if (replacement) { w.Key("replacement"); w.Uint(replacement); }
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void HandleRequestBlob(const JsonValue& msg) {
    uint64_t id = (uint64_t)msg.GetNumber("id");
    uint32_t index = (uint32_t)msg.GetNumber("index");
    auto blob = Tracker::Get().GetBlob(id, index);
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ObjectBlob");
    w.Key("id"); w.Uint(id);
    w.Key("index"); w.Uint(index);
    w.Key("size"); w.Uint(blob ? blob->size() : 0);
    w.EndObject();
    if (blob) Transport::Get().SendBinary(std::move(w.str()), blob->data(), blob->size());
    else Transport::Get().SendJson(std::move(w.str()));
}

// The live contents of a descriptor heap for the Inspect panel, as an ObjectUpdate the UI merges
// into the object: the written slots as `bindings` in the shape of a capture's snapshot, one
// binding per slot (README.md, "Live requests").
void HandleRequestDescriptorSet(const JsonValue& msg) {
    constexpr uint32_t kMaxSlots = 4096;
    uint64_t id = (uint64_t)msg.GetNumber("id");
    TrackedObject obj;
    bool tracked = Tracker::Get().FindById(id, obj) && obj.type == "ID3D12DescriptorHeap";
    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("tracked"); w.Boolean(tracked);
    w.Key("bindings"); w.BeginArray();
    if (tracked) {
        auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(static_cast<uintptr_t>(obj.handle));
        DescriptorTracker& d = DescriptorTracker::Get();
        uint32_t count = d.WrittenCount(heap);
        if (count > kMaxSlots) count = kMaxSlots;
        std::vector<DescriptorRecord> slots = d.Slots(heap, 0, count);
        for (size_t slot = 0; slot < slots.size(); ++slot) {
            const char* kind = DescriptorKindName(slots[slot].kind);
            if (!kind) continue;
            w.BeginObject();
            w.Key("binding"); w.Uint(slot);
            w.Key("type"); w.String(kind);
            w.Key("descriptors"); w.BeginArray();
            WriteDescriptorRecord(w, slots[slot], 0);
            w.EndArray();
            w.EndObject();
        }
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void HandleCapture(const JsonValue& msg) {
    CaptureOptions o;
    o.frameCount = (uint32_t)msg.GetNumber("frameCount", 1);
    if (const JsonValue* v = msg.Get("atFrame")) { if (v->kind == JsonValue::Number && v->num >= 0) o.atFrame = (uint64_t)v->num; }
    if (const JsonValue* v = msg.Get("maxBufferSize")) { if (v->kind == JsonValue::Number) o.maxBufferSize = (uint64_t)v->num; }
    if (const JsonValue* v = msg.Get("maxBufferTotal")) { if (v->kind == JsonValue::Number) o.maxBufferTotal = (uint64_t)v->num; }
    if (const JsonValue* v = msg.Get("maxTextureSize")) { if (v->kind == JsonValue::Number) o.maxTextureSize = (uint64_t)v->num; }
    if (const JsonValue* v = msg.Get("maxImageTotal")) { if (v->kind == JsonValue::Number) o.maxImageTotal = (uint64_t)v->num; }
    o.captureTextures = msg.GetBool("captureTextures", true);
    o.captureBuffers = msg.GetBool("captureBuffers", true);
    o.captureImages = msg.GetBool("captureImages", true);
    o.profilePasses = msg.GetBool("profilePasses", true);
    o.stacktraces = msg.GetBool("stacktraces", false);
    o.overdraw = msg.GetBool("overdraw", false);
    // {texture, x, y, mip, layer}: the pixel to follow through the captured frame (pixel_history.cpp).
    if (const JsonValue* h = msg.Get("pixelHistory"); h != nullptr && h->kind == JsonValue::Object) {
        o.pixelHistory.enabled = true;
        o.pixelHistory.texture = (uint64_t)h->GetNumber("texture");
        o.pixelHistory.x = (uint32_t)h->GetNumber("x");
        o.pixelHistory.y = (uint32_t)h->GetNumber("y");
        o.pixelHistory.mip = (uint32_t)h->GetNumber("mip");
        o.pixelHistory.layer = (uint32_t)h->GetNumber("layer");
    }
    CaptureManager::Get().RequestCapture(o);
}

// Creation stacks of objects, symbolized: {stacks: [{id, frames}]}; `available` says whether the
// library captured any (the launch option).
void HandleRequestStacktraces(const JsonValue& msg) {
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("Stacktraces");
    w.Key("available"); w.Boolean(StackTracesEnabled());
    w.Key("stacks"); w.BeginArray();
    if (const JsonValue* ids = msg.Get("ids")) {
        for (const JsonValue& v : ids->arr) {
            if (v.kind != JsonValue::Number) continue;
            uint64_t id = (uint64_t)v.num;
            StackTrace stack = Tracker::Get().GetStack(id);
            if (stack.empty()) continue;
            w.BeginObject();
            w.Key("id"); w.Uint(id);
            w.Key("frames"); WriteStackFrames(w, Symbolize(stack));
            w.EndObject();
        }
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

// Symbolizes the addresses a capture's commands carry: {frames: [...]} in request order.
void HandleRequestSymbols(const JsonValue& msg) {
    StackTrace addresses;
    if (const JsonValue* list = msg.Get("addresses")) {
        for (const JsonValue& v : list->arr) {
            if (v.kind == JsonValue::String) addresses.push_back(strtoull(v.str.c_str(), nullptr, 0));
            else if (v.kind == JsonValue::Number) addresses.push_back((uint64_t)v.num);
        }
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("Symbols");
    w.Key("frames"); WriteStackFrames(w, Symbolize(addresses));
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

// {pipeline, stage, spirv: base64 DXBC/DXIL} (the field keeps its Vulkan name).
void HandleReplaceShader(const JsonValue& msg) {
    uint64_t pipeline = (uint64_t)msg.GetNumber("pipeline");
    std::string stage = msg.GetString("stage");
    std::vector<uint8_t> bytes;
    if (!DecodeBase64(msg.GetString("spirv"), bytes) || bytes.empty()) {
        SendShaderReplaced(pipeline, stage, false, "malformed bytecode payload (expected base64 DXBC/DXIL)", "", 0);
        return;
    }
    std::string error, note;
    uint64_t replacement = 0;
    bool ok = ShaderEditor::Get().Replace(pipeline, stage, bytes, error, replacement, note);
    SendShaderReplaced(pipeline, stage, ok, ok ? "" : error, note, ok ? replacement : 0);
}

void HandleRestoreShader(const JsonValue& msg) {
    uint64_t pipeline = (uint64_t)msg.GetNumber("pipeline");
    std::string stage = msg.GetString("stage");
    std::string error;
    bool ok = ShaderEditor::Get().Restore(pipeline, stage, error);
    SendShaderReplaced(pipeline, stage, ok, ok ? "" : error, "", 0);
}

void Dispatch(const std::string& text) {
    JsonValue msg;
    if (!JsonParser::Parse(text, msg)) {
        Log("bad message from UI: %s", text.c_str());
        return;
    }
    std::string action = msg.GetString("action");
    Log("ui message: %s", action.c_str());
    if (action == "Ping") {
        Transport::Get().SendJson("{\"action\":\"Pong\"}");
    } else if (action == "RequestSnapshot") {
        // A UI window that picked up an already-connected session rebuilds its object list.
        Tracker::Get().SendSnapshot();
        ValidationLog::Get().SendSnapshot();
    } else if (action == "RequestBlob") {
        HandleRequestBlob(msg);
    } else if (action == "RequestImage") {
        ReadBackImage((uint64_t)msg.GetNumber("id"), (uint32_t)msg.GetNumber("mip"), (uint32_t)msg.GetNumber("layer"));
    } else if (action == "RequestDescriptorSet") {
        HandleRequestDescriptorSet(msg);
    } else if (action == "Settings") {
        if (const JsonValue* v = msg.Get("recordAlways")) {
            if (v->kind == JsonValue::Boolean) CaptureManager::Get().SetRecordAlways(v->b);
        }
    } else if (action == "Capture") {
        HandleCapture(msg);
    } else if (action == "RequestStacktraces") {
        HandleRequestStacktraces(msg);
    } else if (action == "RequestSymbols") {
        HandleRequestSymbols(msg);
    } else if (action == "ReplaceShader") {
        HandleReplaceShader(msg);
    } else if (action == "RestoreShader") {
        HandleRestoreShader(msg);
    } else {
        Log("unhandled UI message: %s", action.c_str());
    }
}

std::once_flag g_startOnce;

}  // namespace

void StartTracking() {
    std::call_once(g_startOnce, [] {
        Transport& t = Transport::Get();
        // The object snapshot first, then the validation messages that refer to its objects.
        t.SetOnConnect([] {
            Tracker::Get().SendSnapshot();
            ValidationLog::Get().SendSnapshot();
        });
        t.SetOnDisconnect([] { Tracker::Get().OnDisconnect(); });
        t.SetMessageHandler(Dispatch);
        t.Start();
    });
}

}  // namespace dxinsp
