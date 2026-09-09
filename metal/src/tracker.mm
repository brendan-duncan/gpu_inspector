#include "tracker.h"

#include "json_writer.h"
#include "swizzle.h"
#include "transport.h"

#import <Foundation/Foundation.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtlinsp {
namespace {

struct TrackedObject {
    uint64_t id = 0;
    std::string type;   // the protocol the application sees: "MTLBuffer"
    std::string cmd;    // the selector that created it
    uint64_t parentId = 0;
    const void *pointer = nullptr;
    std::string label;
    std::string args;   // JSON descriptor, or empty
};

std::mutex g_mutex;
uint64_t g_nextId = 1;
std::unordered_map<const void *, uint64_t> g_byPointer;
std::unordered_map<uint64_t, TrackedObject> g_byId;
// Insertion order, so a snapshot arrives parents-first and the UI never sees a child whose parent
// it has not been told about.
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
    w.EndObject();
    return std::move(w.str());
}

}  // namespace

uint64_t IdOf(id object) {
    if (object == nil) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_byPointer.find((__bridge const void *)object);
    return it == g_byPointer.end() ? 0 : it->second;
}

uint64_t TrackObject(id object, const char *type, const char *cmd, id parent,
                     const std::string &argsJson) {
    if (object == nil) return 0;
    const void *pointer = (__bridge const void *)object;

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
        g_byPointer[pointer] = tracked.id;
        g_order.push_back(tracked.id);
        auto &stored = g_byId[tracked.id] = std::move(tracked);
        if (!g_live) return id;
        message = AddObjectMessage(stored);
    }
    // Outside the lock: the send queue has its own, and holding both is how deadlocks start.
    Transport::Get().SendJson(std::move(message));
    return id;
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

void SendSnapshot() {
    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        messages.reserve(g_order.size());
        for (uint64_t id : g_order) {
            auto it = g_byId.find(id);
            if (it != g_byId.end()) messages.push_back(AddObjectMessage(it->second));
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

void StartTracking() {
    Transport::Get().SetOnConnect([] { SendSnapshot(); });
    Transport::Get().SetOnDisconnect([] { OnDisconnect(); });
    Transport::Get().Start();
}

}  // namespace mtlinsp
