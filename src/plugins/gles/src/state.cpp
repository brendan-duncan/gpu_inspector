#include "state.h"

#include "server.h"

#include <gpu_inspector/sdk/config.h>
#include <gpu_inspector/sdk/transport.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

namespace glesinsp {

using gpuinsp::sdk::Config;
using gpuinsp::sdk::Server;

// ------------------------------------------------------------------------------------------------
// Logging

bool LogEnabled() {
    static int enabled = -1;
    if (enabled < 0) enabled = Config::Get().Flag("GLESINSP_LOG") ? 1 : 0;
    return enabled == 1;
}

static void Write(const char* fmt, va_list args) {
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "glesinsp", "%s", buf);
#else
    fprintf(stderr, "glesinsp: %s\n", buf);
    fflush(stderr);
#endif
#if defined(_WIN32)
    OutputDebugStringA("glesinsp: ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#endif
    static FILE* file = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::string path = Config::Get().Value("GLESINSP_LOG_FILE");
        if (!path.empty()) file = fopen(path.c_str(), "a");
    }
    if (file) {
        fprintf(file, "glesinsp: %s\n", buf);
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

// ------------------------------------------------------------------------------------------------
// JSON helpers

std::string JsonString(const std::string& s) {
    JsonWriter w;
    w.String(s);
    return w.str();
}

std::string JsonEnum(GLenum v) {
    if (const char* name = EnumName(v)) return JsonString(name);
    return std::to_string(v);
}

std::string JsonEnum(GLenum v, const char* (*nameOf)(GLenum)) {
    if (const char* name = nameOf(v)) return JsonString(name);
    return std::to_string(v);
}

std::string JsonRef(uint64_t id, const char* className) {
    JsonWriter w;
    w.Ref(id, className);
    return w.str();
}

const char* TypeName(ObjType t) {
    switch (t) {
        case ObjType::Buffer: return "GLBuffer";
        case ObjType::Texture: return "GLTexture";
        case ObjType::Renderbuffer: return "GLRenderbuffer";
        case ObjType::Program: return "GLProgram";
        case ObjType::Shader: return "GLShader";
        case ObjType::Sampler: return "GLSampler";
        case ObjType::Sync: return "GLSync";
        case ObjType::VertexArray: return "GLVertexArray";
        case ObjType::Framebuffer: return "GLFramebuffer";
        case ObjType::Query: return "GLQuery";
        case ObjType::TransformFeedback: return "GLTransformFeedback";
        case ObjType::ProgramPipeline: return "GLProgramPipeline";
        default: return "GLObject";
    }
}

// ------------------------------------------------------------------------------------------------
// State

LibraryState& State() {
    static LibraryState* state = new LibraryState();
    return *state;
}

static thread_local Context* t_current = nullptr;

void SetCurrent(Context* c) {
    t_current = c;
}

Drawable CurrentDrawable(Context* c) {
    if (!c) return {};
#if defined(_WIN32)
    if (c->wgl) return WglDrawable(c);
#elif defined(__linux__) && !defined(__ANDROID__)
    if (c->glx) return GlxDrawable(c);
#endif
    return EglDrawable(c);
}

void* LookupProc(Context* c, const char* name) {
    if (!c) return nullptr;
#if defined(_WIN32)
    if (c->wgl) return WglLookupProc(name);
#elif defined(__linux__) && !defined(__ANDROID__)
    if (c->glx) return GlxLookupProc(name);
#endif
    return EglLookupProc(name);
}

uint64_t SurfaceId(void* surface) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.surfaces.find(surface);
    return it == s.surfaces.end() ? 0 : it->second;
}

Context* CurrentKnown() {
    return t_current;
}

Context* ContextOf(EGLContext handle) {
    if (!handle) return nullptr;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.contexts.find(handle);
    return it == s.contexts.end() ? nullptr : it->second.get();
}

Context* Current() {
    if (t_current) return t_current;
    // A context made before the hooks went in, or through a path they do not see: taken on here,
    // with a share group of its own.
    if (!g_egl.eglGetCurrentContext) return nullptr;
    EGLContext handle = g_egl.eglGetCurrentContext();
    if (!handle) return nullptr;
    // A desktop OpenGL context of EGL's (eglBindAPI(EGL_OPENGL_API), as on Linux) is not taken on:
    // asked once per context, and remembered per thread so its calls stay cheap.
    static thread_local EGLContext t_foreign = nullptr;
    if (handle == t_foreign) return nullptr;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.contexts.find(handle);
    if (it == s.contexts.end()) {
        EGLint type = 0;
        EGLDisplay display = g_egl.eglGetCurrentDisplay ? g_egl.eglGetCurrentDisplay() : nullptr;
        if (g_egl.eglQueryContext && g_egl.eglQueryContext(display, handle, EGL_CONTEXT_CLIENT_TYPE, &type) && type != EGL_OPENGL_ES_API) {
            t_foreign = handle;
            return nullptr;
        }
        auto c = std::make_unique<Context>();
        c->handle = handle;
        c->display = g_egl.eglGetCurrentDisplay ? g_egl.eglGetCurrentDisplay() : nullptr;
        c->share = std::make_shared<ShareGroup>();
        EGLint version = 0;
        if (g_egl.eglQueryContext && g_egl.eglQueryContext(c->display, handle, EGL_CONTEXT_CLIENT_VERSION, &version)) c->major = version;
        Object& o = NewObject("GLContext", ObjType::Count, 0, "eglMakeCurrent");
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)handle);
        o.handle = buf;
        o.args.push_back({"clientVersion", std::to_string(c->major)});
        c->id = o.id;
        Announce(o);
        it = s.contexts.emplace(handle, std::move(c)).first;
        LogAlways("context 0x%llx was current before the library saw it made; taking it on", (unsigned long long)(uintptr_t)handle);
        StartServer();
    }
    SetCurrent(it->second.get());
    return t_current;
}

Object& NewObject(const char* type, ObjType kind, uint64_t parent, const char* cmd) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    const uint64_t id = s.nextId++;
    Object& o = s.objects[id];
    o.id = id;
    o.kind = kind;
    o.type = type;
    o.parent = parent;
    o.cmd = cmd ? cmd : "";
    return o;
}

Object* FindObject(uint64_t id) {
    if (!id) return nullptr;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.objects.find(id);
    return it == s.objects.end() ? nullptr : &it->second;
}

static std::unordered_map<GLuint, uint64_t>* NamesOf(Context* c, ObjType kind) {
    if (!c) return nullptr;
    return IsShared(kind) ? &c->share->names[(int)kind] : &c->names[(int)kind];
}

Object* ObjectOf(Context* c, ObjType kind, GLuint name) {
    if (!name) return nullptr;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto* names = NamesOf(c, kind);
    if (!names) return nullptr;
    auto it = names->find(name);
    return it == names->end() ? nullptr : FindObject(it->second);
}

uint64_t RegisterName(Context* c, ObjType kind, GLuint name, const char* cmd) {
    if (!name || !c) return 0;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto* names = NamesOf(c, kind);
    auto it = names->find(name);
    if (it != names->end()) return it->second;   // a name made twice is the same object
    Object& o = NewObject(TypeName(kind), kind, c->id, cmd);
    o.name = name;
    o.handle = std::to_string(name);
    (*names)[name] = o.id;
    Announce(o);
    return o.id;
}

uint64_t EnsureName(Context* c, ObjType kind, GLuint name, const char* cmd) {
    if (!name || !c) return 0;
    if (Object* o = ObjectOf(c, kind, name)) return o->id;
    return RegisterName(c, kind, name, cmd);
}

void Forget(uint64_t id) {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    if (!s.objects.erase(id)) return;
    if (Server::Get().Connected()) Server::Get().SendJson("{\"action\":\"DeleteObjects\",\"ids\":[" + std::to_string(id) + "]}");
}

void ForgetName(Context* c, ObjType kind, GLuint name) {
    if (!name || !c) return;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto* names = NamesOf(c, kind);
    auto it = names->find(name);
    if (it == names->end()) return;
    const uint64_t id = it->second;
    names->erase(it);
    Forget(id);
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
    w.Key("handle"); w.String(o.handle);
    w.Key("label");
    if (o.label.empty()) w.Null(); else w.String(o.label);
    w.Key("args");
    w.BeginObject();
    for (const auto& [key, json] : o.args) {
        w.Key(key);
        w.Raw(json);
    }
    w.EndObject();
    w.EndObject();
    return w.str();
}

void Announce(const Object& o) {
    if (Server::Get().Connected()) Server::Get().SendJson(AddObjectJson(o));
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

void SendSnapshot() {
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    std::vector<const Object*> all;
    all.reserve(s.objects.size());
    for (const auto& [id, o] : s.objects) all.push_back(&o);
    // Parents before their children: ids grow with creation, and a parent is always made first.
    std::sort(all.begin(), all.end(), [](const Object* a, const Object* b) { return a->id < b->id; });
    Server::Get().SendJson("{\"action\":\"Snapshot\",\"count\":" + std::to_string(all.size()) + "}");
    for (const Object* o : all) Server::Get().SendJson(AddObjectJson(*o));
}

// ------------------------------------------------------------------------------------------------
// What the generated hooks use (runtime.h)

uint64_t RefOf(ObjType type, GLuint name) {
    if (!name) return 0;
    Object* o = ObjectOf(Current(), type, name);
    return o ? o->id : 0;
}

uint64_t SyncRef(GLsync sync) {
    if (!sync) return 0;
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.syncs.find(sync);
    return it == s.syncs.end() ? 0 : it->second;
}

void WriteRefs(JsonWriter& w, ObjType type, const char* className, const GLuint* names, size_t n) {
    if (!names) { w.Null(); return; }
    if (n > 256) { w.ArraySummary(n); return; }
    w.BeginArray();
    for (size_t i = 0; i < n; ++i) w.Ref(RefOf(type, names[i]), className);
    w.EndArray();
}

void WriteEnums(JsonWriter& w, const char* (*nameOf)(GLenum), const GLenum* values, size_t n) {
    if (!values) { w.Null(); return; }
    if (n > 256) { w.ArraySummary(n); return; }
    w.BeginArray();
    for (size_t i = 0; i < n; ++i) w.Enum(nameOf(values[i]), (int64_t)values[i]);
    w.EndArray();
}

void WriteString(JsonWriter& w, const GLchar* s, GLsizei length) {
    if (!s) { w.Null(); return; }
    if (length < 0) w.String(s);
    else w.String(std::string_view(s, (size_t)length));
}

std::string JoinStrings(GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    std::string out;
    if (!strings) return out;
    for (GLsizei i = 0; i < count; ++i) {
        if (!strings[i]) continue;
        if (lengths && lengths[i] >= 0) out.append(strings[i], (size_t)lengths[i]);
        else out.append(strings[i]);
    }
    return out;
}

}  // namespace glesinsp
