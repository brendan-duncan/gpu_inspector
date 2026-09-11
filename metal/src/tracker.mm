#include "tracker.h"

#include "json_writer.h"
#include "overdraw.h"
#include "stacktrace.h"
#include "swizzle.h"
#include "transport.h"
#include "ui_messages.h"
#include "validation.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

// The ARC weak-reference runtime, usable from manual reference counting but not declared by
// <objc/runtime.h>.
extern "C" id objc_loadWeakRetained(id *location);

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtlinsp {
namespace {

struct TrackedObject {
    uint64_t id = 0;
    // ::id because the `id` member below shadows the Objective-C type inside this scope.
    // The address is registered with the runtime, so it must not move: g_byId is node-based
    // (std::unordered_map), which keeps element addresses stable across rehashing.
    /** Weak slot, so a live object can be used later without keeping it alive. */
    __unsafe_unretained ::id weakSlot = nil;
    std::string type;   // the protocol the application sees: "MTLBuffer"
    std::string cmd;    // the selector that created it
    uint64_t parentId = 0;
    const void *pointer = nullptr;
    std::string label;
    std::string args;   // JSON descriptor, or empty
    /** Named payloads the UI can ask for: shader source, a metallib. */
    std::vector<std::pair<std::string, std::vector<uint8_t>>> blobs;
    /** Where the application created it: return addresses, when stack traces are on. */
    StackTrace stack;
    /** The latest ObjectUpdate fields per key (UpdateObject), replayed by a snapshot. */
    std::vector<std::pair<std::string, std::string>> updates;
};

/** `{"action":"ObjectUpdate","id":N,` + the fields of `argsJson` (an object). */
std::string ObjectUpdateMessage(uint64_t id, const std::string &argsJson) {
    std::string message = "{\"action\":\"ObjectUpdate\",\"id\":" + std::to_string(id);
    // The fields' object, less its braces; an empty object contributes nothing.
    if (argsJson.size() > 2) message += "," + argsJson.substr(1, argsJson.size() - 2);
    return message + "}";
}

std::mutex g_mutex;
uint64_t g_nextId = 1;
std::unordered_map<const void *, uint64_t> g_byPointer;
std::unordered_map<uint64_t, TrackedObject> g_byId;
// Insertion order, so a snapshot arrives parents-first and the UI never sees a child whose parent
// it has not been told about. Ids of objects since deleted stay in it until it is compacted.
std::vector<uint64_t> g_order;
bool g_live = false;

std::string LabelOf(id object) {
    if (![object respondsToSelector:@selector(label)]) return {};
    NSString *label = [object performSelector:@selector(label)];
    return label == nil ? std::string() : std::string(label.UTF8String);
}

std::string AddObjectMessage(const TrackedObject &o) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("AddObject");
    w.Key("id"); w.Uint(o.id);
    w.Key("parent"); w.Uint(o.parentId);
    w.Key("type"); w.String(o.type);
    w.Key("cmd"); w.String(o.cmd);
    w.Key("index"); w.Uint(0);
    w.Key("handle"); w.Pointer(o.pointer);
    w.Key("label"); if (o.label.empty()) w.Null(); else w.String(o.label);
    w.Key("args"); if (o.args.empty()) w.Null(); else w.Raw(o.args);
    if (!o.blobs.empty()) {
        w.Key("blobs"); w.BeginArray();
        for (const auto &[name, data] : o.blobs) {
            w.BeginObject();
            w.Key("name"); w.String(name);
            w.Key("size"); w.Uint(data.size());
            w.EndObject();
        }
        w.EndArray();
    }
    w.EndObject();
    return std::move(w.str());
}

/**
 * The end of a tracked object's life. Installed on every class that has had an instance tracked,
 * and called for every instance of it, tracked or not: the lookup is what tells them apart.
 *
 * Nothing is sent to the object — it is half destroyed — only its pointer is looked up. Always
 * forwarded, whatever the re-entry depth: a release can happen inside any other hook, and an
 * entry left behind here is the pointer-reuse bug this exists to prevent.
 */
void Replaced_dealloc(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    UntrackObject(self);
    // A render pipeline's kept descriptor and counting copies go with it (overdraw.h).
    ForgetRenderPipeline(self);
    ((void (*)(id, SEL))reentry.original())(self, _cmd);
}

/** Under g_mutex. Drops deleted ids from the snapshot order once they outnumber the live ones. */
void CompactOrder() {
    if (g_order.size() < 2 * g_byId.size() + 1024) return;
    std::vector<uint64_t> live;
    live.reserve(g_byId.size());
    for (uint64_t id : g_order) {
        if (g_byId.count(id) != 0) live.push_back(id);
    }
    g_order.swap(live);
}

}  // namespace

id LiveObject(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_byId.find(id);
    if (it == g_byId.end()) return nil;
    // Retained and autoreleased, so the caller can use it even if the application releases it in
    // the meantime.
    return [objc_loadWeakRetained(&it->second.weakSlot) autorelease];
}

uint64_t IdOf(id object) {
    if (object == nil) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_byPointer.find((__bridge const void *)object);
    return it == g_byPointer.end() ? 0 : it->second;
}

uint64_t AllocateId() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_nextId++;
}

uint64_t TrackObject(id object, const char *type, const char *cmd, id parent,
                     const std::string &argsJson) {
    if (object == nil) return 0;
    // The library's own staging buffers, read-back queue and counter buffers are not the
    // application's objects, and the UI should never see them.
    if (IsInternal()) return 0;
    const void *pointer = (__bridge const void *)object;
    // Before the lock: the addresses are cheap, the symbols are looked up only on request.
    // Two frames above this one are the hook and Track(); what remains starts at the
    // application's call, or at Metal's own frames when it made the object itself.
    StackTrace stack;
    if (StackTracesEnabled()) stack = CaptureStack(2);

    std::string message;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto existing = g_byPointer.find(pointer);
        if (existing != g_byPointer.end()) return existing->second;

        uint64_t parentId = 0;
        if (parent != nil) {
            auto it = g_byPointer.find((__bridge const void *)parent);
            if (it != g_byPointer.end()) parentId = it->second;
        }

        TrackedObject tracked;
        tracked.id = id = g_nextId++;
        tracked.type = type;
        tracked.cmd = cmd;
        tracked.parentId = parentId;
        tracked.pointer = pointer;
        tracked.args = argsJson;
        tracked.label = LabelOf(object);
        tracked.stack = std::move(stack);
        g_byPointer[pointer] = tracked.id;
        g_order.push_back(tracked.id);
        auto &stored = g_byId[tracked.id] = std::move(tracked);
        objc_storeWeak(&stored.weakSlot, object);
        if (g_live) message = AddObjectMessage(stored);
    }
    // Outside the lock: Hook has its own, and so does the send queue, and holding both is how
    // deadlocks start. Idempotent per class, so the cost is one lookup after the first instance.
    Hook(object_getClass(object), sel_registerName("dealloc"), (IMP)Replaced_dealloc);
    if (!message.empty()) Transport::Get().SendJson(std::move(message));
    return id;
}

void UntrackObject(id object) {
    if (object == nil) return;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_byPointer.find((__bridge const void *)object);
        if (it == g_byPointer.end()) return;
        const uint64_t id = it->second;
        g_byPointer.erase(it);
        auto tracked = g_byId.find(id);
        if (tracked != g_byId.end()) {
            // Unregisters the slot with the runtime before the memory holding it goes away.
            objc_storeWeak(&tracked->second.weakSlot, nil);
            g_byId.erase(tracked);
        }
        CompactOrder();
        if (!g_live) return;
        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("DeleteObjects");
        w.Key("ids"); w.BeginArray(); w.Uint(id); w.EndArray();
        w.EndObject();
        message = std::move(w.str());
    }
    Transport::Get().SendJson(std::move(message));
}

void AddBlob(id object, const char *name, const void *data, size_t size) {
    if (object == nil || data == nullptr || size == 0) return;
    const uint8_t *bytes = static_cast<const uint8_t *>(data);

    std::string message;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_byPointer.find((__bridge const void *)object);
        if (it == g_byPointer.end()) return;
        auto tracked = g_byId.find(it->second);
        if (tracked == g_byId.end()) return;
        tracked->second.blobs.emplace_back(name, std::vector<uint8_t>(bytes, bytes + size));
        if (!g_live) return;
        // The object was announced without this blob, so tell the UI its list has changed.
        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("ObjectBlobs");
        w.Key("id"); w.Uint(tracked->second.id);
        w.Key("blobs"); w.BeginArray();
        for (const auto &[blobName, blobData] : tracked->second.blobs) {
            w.BeginObject();
            w.Key("name"); w.String(blobName);
            w.Key("size"); w.Uint(blobData.size());
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        message = std::move(w.str());
    }
    Transport::Get().SendJson(std::move(message));
}

void SendBlob(uint64_t objectId, uint32_t index) {
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_byId.find(objectId);
        if (it != g_byId.end() && index < it->second.blobs.size()) {
            data = it->second.blobs[index].second;
        }
    }
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ObjectBlob");
    w.Key("id"); w.Uint(objectId);
    w.Key("index"); w.Uint(index);
    w.Key("size"); w.Uint(data.size());
    w.EndObject();
    Transport::Get().SendBinary(std::move(w.str()), std::move(data));
}

void SendStacktraces(const std::vector<uint64_t> &ids) {
    // The stacks are copied out under the lock and symbolized outside it: dladdr is slow.
    std::vector<std::pair<uint64_t, StackTrace>> stacks;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint64_t id : ids) {
            auto it = g_byId.find(id);
            if (it != g_byId.end() && !it->second.stack.empty()) stacks.emplace_back(id, it->second.stack);
        }
    }
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("Stacktraces");
    w.Key("available"); w.Boolean(StackTracesEnabled());
    w.Key("stacks"); w.BeginArray();
    for (const auto &[id, stack] : stacks) {
        w.BeginObject();
        w.Key("id"); w.Uint(id);
        w.Key("frames"); WriteStackFrames(w, Symbolize(stack));
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void TrackLabel(id object) {
    if (object == nil) return;
    const std::string label = LabelOf(object);
    if (label.empty()) return;

    std::string message;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_byPointer.find((__bridge const void *)object);
        if (it == g_byPointer.end()) return;
        auto tracked = g_byId.find(it->second);
        if (tracked == g_byId.end() || tracked->second.label == label) return;
        tracked->second.label = label;
        if (!g_live) return;

        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("ObjectSetLabel");
        w.Key("id"); w.Uint(tracked->second.id);
        w.Key("label"); w.String(label);
        w.EndObject();
        message = std::move(w.str());
    }
    Transport::Get().SendJson(std::move(message));
}

void UpdateObject(id object, const char *key, const std::string &argsJson) {
    if (object == nil || IsInternal()) return;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_byPointer.find((__bridge const void *)object);
        if (it == g_byPointer.end()) return;
        auto tracked = g_byId.find(it->second);
        if (tracked == g_byId.end()) return;
        auto &updates = tracked->second.updates;
        bool replaced = false;
        for (auto &u : updates) {
            if (u.first == key) { u.second = argsJson; replaced = true; break; }
        }
        if (!replaced) updates.emplace_back(key, argsJson);
        if (!g_live) return;
        message = ObjectUpdateMessage(tracked->second.id, argsJson);
    }
    Transport::Get().SendJson(std::move(message));
}

void SendSnapshot() {
    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        messages.reserve(g_byId.size());
        for (uint64_t id : g_order) {
            auto it = g_byId.find(id);
            if (it == g_byId.end()) continue;
            messages.push_back(AddObjectMessage(it->second));
            for (const auto &u : it->second.updates) messages.push_back(ObjectUpdateMessage(id, u.second));
        }
        g_live = true;
    }
    Log("sending snapshot of %zu objects", messages.size());
    for (auto &message : messages) Transport::Get().SendJson(std::move(message));
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_live = false;
}

void SendLeakReport() {
    std::string message;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<const TrackedObject *> leaked;
        uint64_t owner = 0;
        for (uint64_t id : g_order) {
            auto it = g_byId.find(id);
            if (it == g_byId.end()) continue;
            const TrackedObject &o = it->second;
            if (o.type == "MTLDevice") {
                if (owner == 0) owner = o.id;
                continue;
            }
            if (o.type == "MTLCommandQueue" || o.type == "MTLCommandBuffer") continue;
            leaked.push_back(&o);
        }
        if (leaked.empty()) {
            Log("no leaked objects at exit");
            return;
        }
        std::map<std::string, uint32_t> byType;
        for (const TrackedObject *o : leaked) byType[o->type]++;
        Log("%zu objects still alive at exit", leaked.size());
        if (!g_live) return;

        constexpr size_t kMaxListed = 2000;
        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("LeakReport");
        w.Key("owner"); w.Uint(owner);
        w.Key("ownerClass"); w.String("MTLDevice");
        w.Key("count"); w.Uint(leaked.size());
        w.Key("byType"); w.BeginObject();
        for (const auto &[name, n] : byType) { w.Key(name.c_str()); w.Uint(n); }
        w.EndObject();
        w.Key("objects"); w.BeginArray();
        size_t listed = 0;
        for (const TrackedObject *o : leaked) {
            if (listed++ >= kMaxListed) break;
            w.BeginObject();
            w.Key("id"); w.Uint(o->id);
            w.Key("class"); w.String(o->type);
            w.Key("name"); if (o->label.empty()) w.Null(); else w.String(o->label);
            w.Key("cmd"); w.String(o->cmd);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        message = std::move(w.str());
    }
    Transport::Get().SendJson(std::move(message));
}

void StartTracking() {
    StartUiMessages();
    Transport::Get().SetOnConnect([] {
        SendSnapshot();
        SendValidationSnapshot();
    });
    Transport::Get().SetOnDisconnect([] { OnDisconnect(); });
    Transport::Get().Start();
}

}  // namespace mtlinsp
