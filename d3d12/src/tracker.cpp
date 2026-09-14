#include "tracker.h"

#include "transport.h"

#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace dxinsp {

struct Tracker::Impl {
    std::shared_mutex mutex;
    uint64_t nextId = 1;
    std::unordered_map<uint64_t, TrackedObject> byId;
    std::unordered_map<uint64_t, uint64_t> byHandle;
    bool live = false;
};

Tracker& Tracker::Get() {
    static Tracker* instance = new Tracker();
    return *instance;
}

Tracker::Impl& Tracker::impl() {
    if (!_impl) _impl = new Impl();
    return *_impl;
}

static std::string AddObjectMessage(const TrackedObject& o) {
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("AddObject");
    w.Key("id"); w.Uint(o.id);
    w.Key("parent"); w.Uint(o.parentId);
    w.Key("type"); w.String(o.type);
    w.Key("cmd"); w.String(o.cmd);
    w.Key("index"); w.Uint(o.index);
    w.Key("handle"); w.Pointer((const void*)(uintptr_t)o.handle);
    w.Key("label"); if (o.label.empty()) w.Null(); else w.String(o.label);
    w.Key("args"); if (o.args.empty()) w.Null(); else w.Raw(o.args);
    if (!o.blobs.empty()) {
        w.Key("blobs"); w.BeginArray();
        for (auto& [name, data] : o.blobs) {
            w.BeginObject();
            w.Key("name"); w.String(name);
            w.Key("size"); w.Uint(data ? data->size() : 0);
            w.EndObject();
        }
        w.EndArray();
    }
    w.EndObject();
    return std::move(w.str());
}

uint64_t Tracker::Resolve(int, uint64_t handle) {
    if (!handle) return 0;
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto it = i.byHandle.find(handle);
    return it == i.byHandle.end() ? 0 : it->second;
}

uint64_t Tracker::AllocateId() {
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    return i.nextId++;
}

uint64_t Tracker::Track(void* object, const char* type, const char* cmd, void* parent, std::string args, uint32_t index) {
    if (!object || Internal()) return 0;
    Impl& i = impl();
    std::string message;
    uint64_t id;
    {
        std::unique_lock lock(i.mutex);
        auto existing = i.byHandle.find(Key(object));
        if (existing != i.byHandle.end()) return existing->second;
        TrackedObject o;
        o.id = id = i.nextId++;
        o.type = type;
        o.handle = Key(object);
        o.cmd = cmd;
        o.index = index;
        o.args = std::move(args);
        if (parent) {
            auto pit = i.byHandle.find(Key(parent));
            if (pit != i.byHandle.end()) o.parentId = pit->second;
        }
        if (StackTracesEnabled()) o.stack = CaptureStack(1);
        if (i.live) message = AddObjectMessage(o);
        i.byHandle[o.handle] = id;
        i.byId.emplace(id, std::move(o));
    }
    if (!message.empty()) Transport::Get().SendJson(std::move(message));
    Log("track %s %llu (%s)", type, (unsigned long long)id, cmd);
    return id;
}

static std::string DeleteMessage(const std::vector<uint64_t>& ids) {
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("DeleteObjects");
    w.Key("ids"); w.BeginArray();
    for (uint64_t id : ids) w.Uint(id);
    w.EndArray();
    w.EndObject();
    return std::move(w.str());
}

void Tracker::Untrack(void* object) {
    if (!object) return;
    Impl& i = impl();
    std::vector<uint64_t> removed;
    {
        std::unique_lock lock(i.mutex);
        auto it = i.byHandle.find(Key(object));
        if (it == i.byHandle.end()) return;
        removed.push_back(it->second);
        i.byId.erase(it->second);
        i.byHandle.erase(it);
    }
    if (i.live) Transport::Get().SendJson(DeleteMessage(removed));
    Log("untrack %llu", (unsigned long long)removed[0]);
}

void Tracker::UntrackWithChildren(void* parent) {
    if (!parent) return;
    Impl& i = impl();
    std::vector<uint64_t> removed;
    {
        std::unique_lock lock(i.mutex);
        auto it = i.byHandle.find(Key(parent));
        if (it == i.byHandle.end()) return;
        uint64_t parentId = it->second;
        for (auto& [id, o] : i.byId)
            if (o.parentId == parentId) removed.push_back(id);
        for (uint64_t id : removed) {
            auto oit = i.byId.find(id);
            if (oit != i.byId.end()) {
                i.byHandle.erase(oit->second.handle);
                i.byId.erase(oit);
            }
        }
        removed.push_back(parentId);
        i.byId.erase(parentId);
        i.byHandle.erase(it);
    }
    if (i.live) Transport::Get().SendJson(DeleteMessage(removed));
}

uint64_t Tracker::IdOf(const void* object) {
    return Resolve(0, Key(object));
}

void* Tracker::ObjectOf(uint64_t id) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto it = i.byId.find(id);
    return it == i.byId.end() ? nullptr : (void*)(uintptr_t)it->second.handle;
}

bool Tracker::Find(const void* object, TrackedObject& out) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto it = i.byHandle.find(Key(object));
    if (it == i.byHandle.end()) return false;
    auto oit = i.byId.find(it->second);
    if (oit == i.byId.end()) return false;
    out = oit->second;
    return true;
}

bool Tracker::FindById(uint64_t id, TrackedObject& out) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto oit = i.byId.find(id);
    if (oit == i.byId.end()) return false;
    out = oit->second;
    return true;
}

std::string Tracker::TypeOf(const void* object) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto it = i.byHandle.find(Key(object));
    if (it == i.byHandle.end()) return std::string();
    auto oit = i.byId.find(it->second);
    return oit == i.byId.end() ? std::string() : oit->second.type;
}

void Tracker::SetLabel(void* object, const std::string& label) {
    Impl& i = impl();
    uint64_t id = 0;
    {
        std::unique_lock lock(i.mutex);
        auto it = i.byHandle.find(Key(object));
        if (it == i.byHandle.end()) return;
        auto oit = i.byId.find(it->second);
        if (oit == i.byId.end() || oit->second.label == label) return;
        oit->second.label = label;
        id = it->second;
    }
    if (!i.live) return;
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ObjectSetLabel");
    w.Key("id"); w.Uint(id);
    w.Key("label"); w.String(label);
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void Tracker::Update(void* object, const std::string& key, const std::string& messageJson) {
    uint64_t id = IdOf(object);
    if (id) UpdateById(id, key, messageJson);
}

void Tracker::UpdateById(uint64_t id, const std::string& key, const std::string& messageJson) {
    Impl& i = impl();
    {
        std::unique_lock lock(i.mutex);
        auto oit = i.byId.find(id);
        if (oit == i.byId.end()) return;
        bool replaced = false;
        for (auto& u : oit->second.updates) {
            if (u.first == key) { u.second = messageJson; replaced = true; break; }
        }
        if (!replaced) oit->second.updates.emplace_back(key, messageJson);
    }
    if (i.live) Transport::Get().SendJson(messageJson);
}

void Tracker::AddBlob(void* object, const std::string& name, std::shared_ptr<std::vector<uint8_t>> blob) {
    Impl& i = impl();
    std::string message;
    {
        std::unique_lock lock(i.mutex);
        auto it = i.byHandle.find(Key(object));
        if (it == i.byHandle.end()) return;
        auto oit = i.byId.find(it->second);
        if (oit == i.byId.end()) return;
        oit->second.blobs.emplace_back(name, std::move(blob));
        if (i.live) {
            JsonWriter w;
            w.BeginObject();
            w.Key("action"); w.String("ObjectBlobs");
            w.Key("id"); w.Uint(it->second);
            w.Key("blobs"); w.BeginArray();
            for (auto& [n, data] : oit->second.blobs) {
                w.BeginObject();
                w.Key("name"); w.String(n);
                w.Key("size"); w.Uint(data ? data->size() : 0);
                w.EndObject();
            }
            w.EndArray();
            w.EndObject();
            message = std::move(w.str());
        }
    }
    if (!message.empty()) Transport::Get().SendJson(std::move(message));
}

std::shared_ptr<std::vector<uint8_t>> Tracker::GetBlob(uint64_t id, uint32_t index) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto oit = i.byId.find(id);
    if (oit == i.byId.end() || index >= oit->second.blobs.size()) return nullptr;
    return oit->second.blobs[index].second;
}

StackTrace Tracker::GetStack(uint64_t id) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto oit = i.byId.find(id);
    return oit == i.byId.end() ? StackTrace() : oit->second.stack;
}

void Tracker::SendSnapshot() {
    Impl& i = impl();
    std::vector<std::string> messages;
    {
        std::unique_lock lock(i.mutex);
        // Parents before children: ids are allocated in creation order, and an object's parent
        // always exists before it.
        std::map<uint64_t, const TrackedObject*> ordered;
        for (auto& [id, o] : i.byId) ordered[id] = &o;
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("Snapshot");
        w.Key("count"); w.Uint(ordered.size());
        w.EndObject();
        messages.push_back(std::move(w.str()));
        for (auto& [id, o] : ordered) {
            messages.push_back(AddObjectMessage(*o));
            for (auto& u : o->updates) messages.push_back(u.second);
        }
        i.live = true;
    }
    Transport& t = Transport::Get();
    for (auto& m : messages) t.SendJson(std::move(m));
    Log("snapshot sent: %zu objects", messages.size() - 1);
}

void Tracker::OnDisconnect() {
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    i.live = false;
}

void Tracker::SendLeakReport(void* owner) {
    Impl& i = impl();
    uint64_t ownerId = IdOf(owner);
    if (!ownerId) return;
    std::string ownerClass;
    std::map<std::string, uint32_t> byType;
    std::vector<const TrackedObject*> leaked;
    JsonWriter w;
    {
        std::shared_lock lock(i.mutex);
        auto oit = i.byId.find(ownerId);
        if (oit == i.byId.end()) return;
        ownerClass = oit->second.type;
        std::map<uint64_t, const TrackedObject*> ordered;
        for (auto& [id, o] : i.byId) {
            if (o.parentId != ownerId) continue;
            // What the application cannot release itself is not a leak.
            if (o.type == "ID3D12CommandQueue" && o.cmd == "D3D12CreateDevice") continue;
            ordered[id] = &o;
        }
        for (auto& [id, o] : ordered) {
            byType[o->type]++;
            leaked.push_back(o);
        }
        if (leaked.empty()) return;
        w.BeginObject();
        w.Key("action"); w.String("LeakReport");
        w.Key("owner"); w.Uint(ownerId);
        w.Key("ownerClass"); w.String(ownerClass);
        w.Key("count"); w.Uint(leaked.size());
        w.Key("byType"); w.BeginObject();
        for (auto& [type, n] : byType) { w.Key(type.c_str()); w.Uint(n); }
        w.EndObject();
        w.Key("objects"); w.BeginArray();
        size_t n = 0;
        for (const TrackedObject* o : leaked) {
            if (n++ >= 2000) break;
            w.BeginObject();
            w.Key("id"); w.Uint(o->id);
            w.Key("class"); w.String(o->type);
            w.Key("name"); if (o->label.empty()) w.Null(); else w.String(o->label);
            w.Key("cmd"); w.String(o->cmd);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    LogAlways("leak report: %zu objects still alive under %s %llu", leaked.size(), ownerClass.c_str(), (unsigned long long)ownerId);
    Transport::Get().SendJson(std::move(w.str()));
}

void WriteRef(JsonWriter& w, const void* object, const char* type) {
    if (!object) { w.Null(); return; }
    w.Handle(0, type, Key(object));
}

}  // namespace dxinsp
