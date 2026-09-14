#include "tracker.h"

#include <algorithm>
#include <map>

#include "layer.h"
#include "resources.h"
#include "transport.h"

namespace vkinsp {

Tracker& Tracker::Get() {
    static Tracker* instance = new Tracker();  // never destroyed: outlives static teardown order
    return *instance;
}

uint64_t Tracker::Resolve(int handleType, uint64_t handle) {
    if (handleType < 0 || handleType >= HT_Count) return 0;
    std::shared_lock lock(_mutex);
    auto& m = _byHandle[handleType];
    auto it = m.find(handle);
    return it == m.end() ? 0 : it->second;
}

JsonWriter& Tracker::BeginArgs() {
    thread_local JsonWriter writer;
    writer.Reset();
    writer.SetResolver(this);
    return writer;
}

void Tracker::EndArgs() {}

std::string Tracker::AddObjectMessage(const TrackedObject& o) const {
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("AddObject");
    w.Key("id"); w.Uint(o.id);
    w.Key("parent"); w.Uint(o.parentId);
    w.Key("type"); w.String(kHandleTypeNames[o.type]);
    w.Key("cmd"); w.String(kVkCommandNames[(int)o.cmd]);
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

uint64_t Tracker::OnCreate(HandleType type, uint64_t handle, HandleType parentType, uint64_t parentHandle,
                           VkCmdId cmd, uint32_t index, const std::string& args) {
    std::unique_lock lock(_mutex);
    auto& m = _byHandle[type];
    uint64_t parentId = 0;
    if (parentType < HT_Count) {
        auto pit = _byHandle[parentType].find(parentHandle);
        if (pit != _byHandle[parentType].end()) parentId = pit->second;
    }
    auto existing = m.find(handle);
    if (existing != m.end()) {
        auto it = _byId.find(existing->second);
        // The same object handed out again (swapchain images retrieved twice) keeps its record.
        // A handle under a *new* owner is a recycled one: a swapchain recreated with oldSwapchain
        // hands back its predecessor's images, and leaving those records under the old swapchain
        // would destroy them with it — taking the image views over them, which is how the live
        // swapchain's images and views went missing from a capture.
        if (it == _byId.end() || !parentId || it->second.parentId == parentId) return existing->second;
        TrackedObject& o = it->second;
        if (o.parentId) {
            auto old = _byId.find(o.parentId);
            if (old != _byId.end()) {
                auto& siblings = old->second.children;
                for (size_t i = 0; i < siblings.size(); ++i)
                    if (siblings[i] == o.id) { siblings[i] = siblings.back(); siblings.pop_back(); break; }
            }
        }
        o.parentId = parentId;
        o.cmd = cmd;
        o.index = index;
        o.args = args;
        _byId[parentId].children.push_back(o.id);
        if (_live) Transport::Get().SendJson(AddObjectMessage(o));
        return o.id;
    }

    TrackedObject o;
    o.id = _nextId++;
    o.type = type;
    o.handle = handle;
    o.cmd = cmd;
    o.index = index;
    o.args = args;
    if (StackTracesEnabled()) o.stack = CaptureStack(1);
    if (parentId) {
        o.parentId = parentId;
        _byId[parentId].children.push_back(o.id);
    }
    m[handle] = o.id;
    uint64_t id = o.id;
    if (_live) Transport::Get().SendJson(AddObjectMessage(o));
    _byId[id] = std::move(o);
    return id;
}

void Tracker::RemoveLocked(uint64_t id, std::vector<uint64_t>& removed) {
    auto it = _byId.find(id);
    if (it == _byId.end()) return;
    std::vector<uint64_t> children = std::move(it->second.children);
    for (uint64_t c : children) RemoveLocked(c, removed);
    it = _byId.find(id);
    if (it == _byId.end()) return;
    TrackedObject& o = it->second;
    auto& m = _byHandle[o.type];
    auto hit = m.find(o.handle);
    if (hit != m.end() && hit->second == id) m.erase(hit);
    if (o.parentId) {
        auto pit = _byId.find(o.parentId);
        if (pit != _byId.end()) {
            auto& pc = pit->second.children;
            for (size_t i = 0; i < pc.size(); ++i) {
                if (pc[i] == id) { pc[i] = pc.back(); pc.pop_back(); break; }
            }
        }
    }
    removed.push_back(id);
    ResourceRegistry::Get().OnDestroy(o.type, o.handle);
    _byId.erase(it);
}

static void SendDeleted(const std::vector<uint64_t>& ids) {
    if (ids.empty()) return;
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("DeleteObjects");
    w.Key("ids"); w.BeginArray();
    for (uint64_t id : ids) w.Uint(id);
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void Tracker::OnDestroy(HandleType type, uint64_t handle) {
    if (!handle) return;
    std::unique_lock lock(_mutex);
    auto& m = _byHandle[type];
    auto it = m.find(handle);
    if (it == m.end()) return;
    std::vector<uint64_t> removed;
    RemoveLocked(it->second, removed);
    if (_live) SendDeleted(removed);
}

void Tracker::OnDestroyChildren(HandleType type, uint64_t handle) {
    std::unique_lock lock(_mutex);
    auto& m = _byHandle[type];
    auto it = m.find(handle);
    if (it == m.end()) return;
    auto oit = _byId.find(it->second);
    if (oit == _byId.end()) return;
    std::vector<uint64_t> children = std::move(oit->second.children);
    oit->second.children.clear();
    std::vector<uint64_t> removed;
    for (uint64_t c : children) RemoveLocked(c, removed);
    if (_live) SendDeleted(removed);
}

void Tracker::SendLeakReport(HandleType type, uint64_t handle) {
    if (!handle) return;
    std::unique_lock lock(_mutex);
    auto& m = _byHandle[type];
    auto it = m.find(handle);
    if (it == m.end()) return;
    auto oit = _byId.find(it->second);
    if (oit == _byId.end()) return;
    const TrackedObject& owner = oit->second;

    std::vector<const TrackedObject*> leaked;
    std::vector<uint64_t> stack(owner.children.begin(), owner.children.end());
    while (!stack.empty()) {
        uint64_t id = stack.back();
        stack.pop_back();
        auto cit = _byId.find(id);
        if (cit == _byId.end()) continue;
        const TrackedObject& o = cit->second;
        for (uint64_t c : o.children) stack.push_back(c);
        if (o.type == HT_VkQueue || o.type == HT_VkPhysicalDevice || o.type == HT_VkDescriptorSet || o.type == HT_VkCommandBuffer) continue;
        if (o.type == HT_VkImage && o.cmd == VkCmdId::GetSwapchainImagesKHR) continue;
        leaked.push_back(&o);
    }
    if (leaked.empty()) {
        Log("no leaked objects under %s %llu", kHandleTypeNames[owner.type], (unsigned long long)owner.id);
        return;
    }
    std::sort(leaked.begin(), leaked.end(), [](auto* a, auto* b) { return a->id < b->id; });
    std::map<std::string, uint32_t> byType;
    for (auto* o : leaked) byType[kHandleTypeNames[o->type]]++;
    Log("%zu objects still alive under %s %llu at destruction", leaked.size(), kHandleTypeNames[owner.type], (unsigned long long)owner.id);
    if (!_live) return;

    constexpr size_t kMaxListed = 2000;
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("LeakReport");
    w.Key("owner"); w.Uint(owner.id);
    w.Key("ownerClass"); w.String(kHandleTypeNames[owner.type]);
    w.Key("count"); w.Uint(leaked.size());
    w.Key("byType"); w.BeginObject();
    for (auto& [name, n] : byType) { w.Key(name.c_str()); w.Uint(n); }
    w.EndObject();
    w.Key("objects"); w.BeginArray();
    size_t listed = 0;
    for (auto* o : leaked) {
        if (listed++ >= kMaxListed) break;
        w.BeginObject();
        w.Key("id"); w.Uint(o->id);
        w.Key("class"); w.String(kHandleTypeNames[o->type]);
        w.Key("name"); if (o->label.empty()) w.Null(); else w.String(o->label);
        w.Key("cmd"); w.String(kVkCommandNames[(int)o->cmd]);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void Tracker::SetLabel(HandleType type, uint64_t handle, const char* label) {
    std::unique_lock lock(_mutex);
    auto& m = _byHandle[type];
    auto it = m.find(handle);
    if (it == m.end()) return;
    TrackedObject& o = _byId[it->second];
    o.label = label ? label : "";
    if (_live) {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("ObjectSetLabel");
        w.Key("id"); w.Uint(o.id);
        w.Key("label"); w.String(o.label);
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
    }
}

void Tracker::Update(uint64_t id, const std::string& key, const std::string& messageJson) {
    std::unique_lock lock(_mutex);
    auto it = _byId.find(id);
    if (it == _byId.end()) return;
    bool replaced = false;
    for (auto& [k, v] : it->second.updates) {
        if (k == key) { v = messageJson; replaced = true; break; }
    }
    if (!replaced) it->second.updates.emplace_back(key, messageJson);
    if (_live) Transport::Get().SendJson(messageJson);
}

void Tracker::AddBlob(HandleType type, uint64_t handle, const std::string& name,
                      std::shared_ptr<std::vector<uint8_t>> blob) {
    std::unique_lock lock(_mutex);
    auto& m = _byHandle[type];
    auto it = m.find(handle);
    if (it == m.end()) return;
    TrackedObject& o = _byId[it->second];
    o.blobs.emplace_back(name, std::move(blob));
    if (_live) {
        // Tell the UI the object gained a payload (it may already have been sent).
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("ObjectBlobs");
        w.Key("id"); w.Uint(o.id);
        w.Key("blobs"); w.BeginArray();
        for (auto& [n, d] : o.blobs) {
            w.BeginObject();
            w.Key("name"); w.String(n);
            w.Key("size"); w.Uint(d ? d->size() : 0);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
    }
}

StackTrace Tracker::GetStack(uint64_t id) {
    std::shared_lock lock(_mutex);
    auto it = _byId.find(id);
    return it == _byId.end() ? StackTrace() : it->second.stack;
}

std::shared_ptr<std::vector<uint8_t>> Tracker::GetBlob(uint64_t id, uint32_t index) {
    std::shared_lock lock(_mutex);
    auto it = _byId.find(id);
    if (it == _byId.end() || index >= it->second.blobs.size()) return nullptr;
    return it->second.blobs[index].second;
}

std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> Tracker::GetBlobs(HandleType type, uint64_t handle) {
    std::shared_lock lock(_mutex);
    auto& m = _byHandle[type];
    auto it = m.find(handle);
    if (it == m.end()) return {};
    return _byId[it->second].blobs;
}

bool Tracker::Find(HandleType type, uint64_t handle, TrackedObject& out) {
    std::shared_lock lock(_mutex);
    auto& m = _byHandle[type];
    auto it = m.find(handle);
    if (it == m.end()) return false;
    out = _byId[it->second];
    return true;
}

bool Tracker::FindById(uint64_t id, TrackedObject& out) {
    std::shared_lock lock(_mutex);
    auto it = _byId.find(id);
    if (it == _byId.end()) return false;
    out = it->second;
    return true;
}

void Tracker::SendSnapshot() {
    std::unique_lock lock(_mutex);
    // Parents before children: ids are monotonic and a parent always exists before its children.
    std::vector<const TrackedObject*> objs;
    objs.reserve(_byId.size());
    for (auto& [id, o] : _byId) objs.push_back(&o);
    std::sort(objs.begin(), objs.end(), [](auto* a, auto* b) { return a->id < b->id; });
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("Snapshot");
    w.Key("count"); w.Uint(objs.size());
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    for (auto* o : objs) {
        Transport::Get().SendJson(AddObjectMessage(*o));
        for (auto& [k, v] : o->updates) Transport::Get().SendJson(v);
    }
    _live = true;
    Log("snapshot sent: %zu objects", objs.size());
}

void Tracker::OnDisconnect() {
    std::unique_lock lock(_mutex);
    _live = false;
}

} // namespace vkinsp
