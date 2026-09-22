#include "state.h"

#include <gpu_inspector/sdk/config.h>
#include <gpu_inspector/sdk/transport.h>

#include <algorithm>
#include <cstdarg>

namespace d3d11insp {

using gpuinsp::sdk::Config;
using gpuinsp::sdk::Server;

// ------------------------------------------------------------------------------------------------
// Logging

bool LogEnabled() {
    static int enabled = -1;
    if (enabled < 0) enabled = Config::Get().Flag("D3D11INSP_LOG") ? 1 : 0;
    return enabled == 1;
}

static void Write(const char* fmt, va_list args) {
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    fprintf(stderr, "d3d11insp: %s\n", buf);
    fflush(stderr);
    OutputDebugStringA("d3d11insp: ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    static FILE* file = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::string path = Config::Get().Value("D3D11INSP_LOG_FILE");
        if (!path.empty()) file = fopen(path.c_str(), "a");
    }
    if (file) {
        fprintf(file, "d3d11insp: %s\n", buf);
        fflush(file);
    }
}

void Log(const char* fmt, ...) {
    if (!LogEnabled()) return;
    va_list args;
    va_start(args, fmt);
    Write(fmt, args);
    va_end(args);
}

void LogAlways(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Write(fmt, args);
    va_end(args);
}

std::string Narrow(const wchar_t* s, size_t length) {
    if (!s || !length) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, (int)length, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(0, n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, (int)length, out.data(), n, nullptr, nullptr);
    return out;
}

std::string Narrow(const wchar_t* s) {
    return s ? Narrow(s, wcslen(s)) : std::string();
}

// ------------------------------------------------------------------------------------------------
// JSON helpers

std::string JsonString(const std::string& s) {
    JsonWriter w;
    w.String(s);
    return w.str();
}

std::string JsonRef(uint64_t id, const char* className) {
    JsonWriter w;
    w.Ref(id, className);
    return w.str();
}

std::string JsonRef(const void* ptr) {
    return JsonRef(IdOf(ptr), ClassOf(ptr));
}

const char* ClassOf(const void* ptr) {
    if (Object* o = Find(ptr)) return o->type.c_str();
    return "ID3D11DeviceChild";
}

const char* StageName(int stage) {
    switch (stage) {
        case VS: return "vertex";
        case HS: return "tess_control";
        case DS: return "tess_eval";
        case GS: return "geometry";
        case PS: return "fragment";
        case CS: return "compute";
        default: return "unknown";
    }
}

const char* StagePrefix(int stage) {
    switch (stage) {
        case VS: return "VS";
        case HS: return "HS";
        case DS: return "DS";
        case GS: return "GS";
        case PS: return "PS";
        case CS: return "CS";
        default: return "?";
    }
}

// ------------------------------------------------------------------------------------------------
// Objects

LibraryState& State() {
    static LibraryState* state = new LibraryState();
    return *state;
}

static std::string AddObjectJson(const Object& o) {
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("AddObject");
    w.Key("id"); w.Uint(o.id);
    w.Key("parent"); w.Uint(o.parent);
    w.Key("type"); w.String(o.type);
    w.Key("cmd"); w.String(o.cmd);
    w.Key("index"); w.Uint(0);
    w.Key("handle"); w.String(Hex((uint64_t)(uintptr_t)o.ptr));
    w.Key("label");
    if (o.label.empty()) w.Null(); else w.String(o.label);
    w.Key("args");
    w.BeginObject();
    for (const auto& [key, json] : o.args) {
        w.Key(key);
        w.Raw(json);
    }
    w.EndObject();
    if (!o.blobs.empty()) {
        w.Key("blobs");
        w.BeginArray();
        for (const auto& [name, data] : o.blobs) {
            w.BeginObject();
            w.Key("name"); w.String(name);
            w.Key("size"); w.Uint(data ? data->size() : 0);
            w.EndObject();
        }
        w.EndArray();
    }
    w.EndObject();
    return w.str();
}

Object& Track(IUnknown* ptr, ObjKind kind, const char* type, uint64_t parent, const char* cmd) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    if (Object* existing = Find(ptr)) return *existing;
    const uint64_t id = s.nextId++;
    auto o = std::make_unique<Object>();
    o->id = id;
    o->ptr = ptr;
    o->kind = kind;
    o->type = type;
    o->parent = parent;
    o->cmd = cmd ? cmd : "";
    Object& ref = *o;
    s.objects[id] = std::move(o);
    s.byPointer[ptr] = id;
    return ref;
}

Object* Find(const void* ptr) {
    if (!ptr) return nullptr;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.byPointer.find(ptr);
    if (it == s.byPointer.end()) return nullptr;
    auto oit = s.objects.find(it->second);
    return oit == s.objects.end() ? nullptr : oit->second.get();
}

Object* FindById(uint64_t id) {
    if (!id) return nullptr;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.objects.find(id);
    return it == s.objects.end() ? nullptr : it->second.get();
}

uint64_t IdOf(const void* ptr) {
    Object* o = Find(ptr);
    return o ? o->id : 0;
}

void Untrack(const void* ptr) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.byPointer.find(ptr);
    if (it == s.byPointer.end()) return;
    const uint64_t id = it->second;
    s.byPointer.erase(it);
    s.objects.erase(id);
    if (Server::Get().Connected()) Server::Get().SendJson("{\"action\":\"DeleteObjects\",\"ids\":[" + std::to_string(id) + "]}");
}

void Describe(Object& o, const std::string& key, const std::string& json) {
    {
        LibraryState& s = State();
        std::lock_guard lock(s.mutex);
        auto it = std::find_if(o.args.begin(), o.args.end(), [&](const auto& kv) { return kv.first == key; });
        if (it != o.args.end()) {
            if (it->second == json) return;
            it->second = json;
        } else {
            o.args.push_back({key, json});
        }
    }
    if (!Server::Get().Connected()) return;
    // The inspector keeps an update beside the arguments (its `updates`); the plugin's backend reads
    // both, the update winning. The snapshot sends the arguments with every update folded in.
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(o.id);
    w.Key(key); w.Raw(json);
    w.EndObject();
    Server::Get().SendJson(w.str());
}

void SetLabel(Object& o, const std::string& label) {
    o.label = label;
    if (!Server::Get().Connected()) return;
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ObjectSetLabel");
    w.Key("id"); w.Uint(o.id);
    w.Key("label"); w.String(label);
    w.EndObject();
    Server::Get().SendJson(w.str());
}

void AddBlob(Object& o, const std::string& name, std::shared_ptr<std::vector<uint8_t>> data) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    o.blobs.emplace_back(name, std::move(data));
}

std::shared_ptr<std::vector<uint8_t>> BlobOf(uint64_t id, size_t index) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    Object* o = FindById(id);
    if (!o || index >= o->blobs.size()) return nullptr;
    return o->blobs[index].second;
}

void Announce(const Object& o) {
    if (Server::Get().Connected()) Server::Get().SendJson(AddObjectJson(o));
}

Context& ContextOf(ID3D11DeviceContext* ctx) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.contexts.find(ctx);
    if (it != s.contexts.end()) return *it->second;
    auto c = std::make_unique<Context>();
    c->ptr = ctx;
    c->id = IdOf(ctx);
    {
        ScopedInternal internal;
        ctx->GetDevice(&c->device);
        if (c->device) c->device->Release();   // the device outlives its contexts; a weak pointer is enough
        c->deferred = ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
    }
    c->deviceId = IdOf(c->device);
    Context& ref = *c;
    s.contexts[ctx] = std::move(c);
    return ref;
}

Context* ContextOfProxy(const void* proxy) {
    if (!proxy) return nullptr;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    for (auto& [ptr, c] : s.contexts)
        if (c->proxy == proxy) return c.get();
    return nullptr;
}

Context* FindContext(const void* ctx) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.contexts.find(ctx);
    return it == s.contexts.end() ? nullptr : it->second.get();
}

void SendSnapshot() {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    std::vector<const Object*> all;
    all.reserve(s.objects.size());
    for (const auto& [id, o] : s.objects) all.push_back(o.get());
    // Parents before their children: ids grow with creation, and a parent is always made first.
    std::sort(all.begin(), all.end(), [](const Object* a, const Object* b) { return a->id < b->id; });
    Server::Get().SendJson("{\"action\":\"Snapshot\",\"count\":" + std::to_string(all.size()) + "}");
    for (const Object* o : all) Server::Get().SendJson(AddObjectJson(*o));
}

}  // namespace d3d11insp
