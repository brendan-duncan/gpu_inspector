// The EGL entry points the library hooks: contexts and surfaces made and destroyed, which context is
// current where, eglSwapBuffers (the frame boundary), and eglGetProcAddress, which hands out our hook
// for a GL entry point in place of the driver's so a pointer fetched that way is seen too.
#include "capture.h"
#include "server.h"
#include "state.h"

#include <cstring>
#include <string>
#include <unordered_map>

namespace glesinsp
{

#if defined(__linux__) && !defined(__ANDROID__)
void ResolveEglProcs();   // platform_linux.cpp
#endif

EglDispatch g_egl{};

namespace
{

std::string HandleText(const void* p)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)p);
    return buf;
}

/** The value of `key` in an EGL_NONE-terminated attribute list, or `def`. */
template <class T>
T Attrib(const T* list, T key, T def)
{
    if (!list)
        return def;
    for (const T* p = list; *p != (T)EGL_NONE; p += 2)
    {
        if (p[0] == key)
            return p[1];
    }
    return def;
}

void NewSurface(EGLDisplay display, EGLSurface surface, const char* cmd, const char* kind, void* window)
{
    if (!surface)
        return;
    Object& o = NewObject("GLSurface", ObjType::Count, 0, cmd);
    o.handle = HandleText(surface);
    o.args.push_back({"kind", JsonString(kind)});
    if (window)
        o.args.push_back({"window", JsonString(HandleText(window))});
    EGLint w = 0, h = 0;
    if (g_egl.eglQuerySurface)
    {
        g_egl.eglQuerySurface(display, surface, EGL_WIDTH, &w);
        g_egl.eglQuerySurface(display, surface, EGL_HEIGHT, &h);
        o.args.push_back({"width", JsonInt(w)});
        o.args.push_back({"height", JsonInt(h)});
    }
    {
        std::lock_guard lock(State().mutex);
        State().surfaces[surface] = o.id;
    }
    Announce(o);
}

EGLContext EGLAPIENTRY Hook_eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share, const EGLint* attribs)
{
    EGLContext result = g_egl.eglCreateContext(display, config, share, attribs);
    if (!result)
        return result;
    // A desktop OpenGL context (eglBindAPI(EGL_OPENGL_API), which Linux applications use) is not
    // this library's to capture: it is left alone, and so are its calls.
    if (g_egl.eglQueryAPI && g_egl.eglQueryAPI() != EGL_OPENGL_ES_API)
        return result;
    // The inspector's connection comes up with the first context: a process that never makes one
    // (a Vulkan or D3D application the library went into beside the others) leaves the port alone.
    StartServer();
#if defined(__linux__) && !defined(__ANDROID__)
    ResolveEglProcs();
#endif
    auto c = std::make_unique<Context>();
    c->handle = result;
    c->display = display;
    // EGL_CONTEXT_CLIENT_VERSION defaults to 1, which no ES 2+ application leaves it at.
    c->major = Attrib<EGLint>(attribs, EGL_CONTEXT_CLIENT_VERSION, 1);
    c->minor = Attrib<EGLint>(attribs, EGL_CONTEXT_MINOR_VERSION, 0);
    Context* shared = ContextOf(share);
    c->share = shared ? shared->share : std::make_shared<ShareGroup>();
    Object& o = NewObject("GLContext", ObjType::Count, 0, "eglCreateContext");
    o.handle = HandleText(result);
    o.args.push_back({"clientVersion", JsonInt(c->major)});
    if (c->minor)
        o.args.push_back({"minorVersion", JsonInt(c->minor)});
    o.args.push_back({"shareContext", JsonRef(shared ? shared->id : 0, "GLContext")});
    o.args.push_back({"config", JsonString(HandleText(config))});
    c->id = o.id;
    {
        std::lock_guard lock(State().mutex);
        State().contexts[result] = std::move(c);
    }
    Announce(o);
    Log("context %s: OpenGL ES %d.%d", o.handle.c_str(), Attrib<EGLint>(attribs, EGL_CONTEXT_CLIENT_VERSION, 1), Attrib<EGLint>(attribs, EGL_CONTEXT_MINOR_VERSION, 0));
    return result;
}

EGLBoolean EGLAPIENTRY Hook_eglDestroyContext(EGLDisplay display, EGLContext context)
{
    const EGLBoolean result = g_egl.eglDestroyContext(display, context);
    if (!result)
        return result;
    uint64_t id = 0;
    {
        std::lock_guard lock(State().mutex);
        auto it = State().contexts.find(context);
        if (it != State().contexts.end())
        {
            id = it->second->id;
            // Still current somewhere, it lives on until released; the library forgets it now.
            if (CurrentKnown() == it->second.get())
                SetCurrent(nullptr);
            State().contexts.erase(it);
        }
    }
    if (id)
        Forget(id);
    return result;
}

EGLBoolean EGLAPIENTRY Hook_eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context)
{
    const EGLBoolean result = g_egl.eglMakeCurrent(display, draw, read, context);
    if (!result)
        return result;
    if (!context)
    {
        SetCurrent(nullptr);
        return result;
    }
    Context* c = ContextOf(context);
    if (!c)
    {
        SetCurrent(nullptr);
        c = Current();   // one made before the library was in: taken on here
    }
    else
    {
        SetCurrent(c);
    }
    if (c)
        c->drawSurface = draw;
    return result;
}

EGLBoolean Swap(EGLDisplay display, EGLSurface surface, const char* method, const EGLint* rects, EGLint n,
    PFN_eglSwapBuffersWithDamageKHR withDamage)
{
    Context* c = Current();
    BeforeSwap(c, display, surface, method);
    const EGLBoolean result = withDamage ? withDamage(display, surface, rects, n) : g_egl.eglSwapBuffers(display, surface);
    AfterSwap(c);
    return result;
}

EGLBoolean EGLAPIENTRY Hook_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    return Swap(display, surface, "eglSwapBuffers", nullptr, 0, nullptr);
}

EGLBoolean EGLAPIENTRY Hook_eglSwapBuffersWithDamageKHR(EGLDisplay display, EGLSurface surface, const EGLint* rects, EGLint n)
{
    return Swap(display, surface, "eglSwapBuffersWithDamageKHR", rects, n, g_egl.eglSwapBuffersWithDamageKHR);
}

EGLBoolean EGLAPIENTRY Hook_eglSwapBuffersWithDamageEXT(EGLDisplay display, EGLSurface surface, const EGLint* rects, EGLint n)
{
    return Swap(display, surface, "eglSwapBuffersWithDamageEXT", rects, n, g_egl.eglSwapBuffersWithDamageEXT);
}

EGLSurface EGLAPIENTRY Hook_eglCreateWindowSurface(EGLDisplay display, EGLConfig config, EGLNativeWindowType window, const EGLint* attribs)
{
    EGLSurface s = g_egl.eglCreateWindowSurface(display, config, window, attribs);
    NewSurface(display, s, "eglCreateWindowSurface", "window", window);
    return s;
}

EGLSurface EGLAPIENTRY Hook_eglCreatePlatformWindowSurface(EGLDisplay display, EGLConfig config, void* window, const EGLAttrib* attribs)
{
    EGLSurface s = g_egl.eglCreatePlatformWindowSurface(display, config, window, attribs);
    NewSurface(display, s, "eglCreatePlatformWindowSurface", "window", window);
    return s;
}

EGLSurface EGLAPIENTRY Hook_eglCreatePlatformWindowSurfaceEXT(EGLDisplay display, EGLConfig config, void* window, const EGLint* attribs)
{
    EGLSurface s = g_egl.eglCreatePlatformWindowSurfaceEXT(display, config, window, attribs);
    NewSurface(display, s, "eglCreatePlatformWindowSurfaceEXT", "window", window);
    return s;
}

EGLSurface EGLAPIENTRY Hook_eglCreatePbufferSurface(EGLDisplay display, EGLConfig config, const EGLint* attribs)
{
    EGLSurface s = g_egl.eglCreatePbufferSurface(display, config, attribs);
    NewSurface(display, s, "eglCreatePbufferSurface", "pbuffer", nullptr);
    return s;
}

EGLBoolean EGLAPIENTRY Hook_eglDestroySurface(EGLDisplay display, EGLSurface surface)
{
    const EGLBoolean result = g_egl.eglDestroySurface(display, surface);
    if (!result)
        return result;
    uint64_t id = 0;
    {
        std::lock_guard lock(State().mutex);
        auto it = State().surfaces.find(surface);
        if (it != State().surfaces.end())
        {
            id = it->second;
            State().surfaces.erase(it);
        }
    }
    if (id)
        Forget(id);
    return result;
}

EGLFuncPtr EGLAPIENTRY Hook_eglGetProcAddress(const char* name)
{
    EGLFuncPtr real = g_egl.eglGetProcAddress(name);
    if (!real || !name)
        return real;
    // Our hook stands in for the driver's entry point; the driver's is what the hook calls.
    for (size_t i = 0; i < kHookCount; ++i)
    {
        if (strcmp(kHooks[i].name, name) != 0)
            continue;
        if (!*kHooks[i].real)
            *kHooks[i].real = (void*)real;
        return (EGLFuncPtr)kHooks[i].hook;
    }
    for (size_t i = 0; i < kEglHookCount; ++i)
    {
        if (strcmp(kEglHooks[i].name, name) != 0)
            continue;
        if (!*kEglHooks[i].real)
            *kEglHooks[i].real = (void*)real;
        return (EGLFuncPtr)kEglHooks[i].hook;
    }
    return real;
}

}  // namespace

const EglHookEntry kEglHooks[] = {
    {"eglGetProcAddress", (void*)&Hook_eglGetProcAddress, (void**)&g_egl.eglGetProcAddress},
    {"eglCreateContext", (void*)&Hook_eglCreateContext, (void**)&g_egl.eglCreateContext},
    {"eglDestroyContext", (void*)&Hook_eglDestroyContext, (void**)&g_egl.eglDestroyContext},
    {"eglMakeCurrent", (void*)&Hook_eglMakeCurrent, (void**)&g_egl.eglMakeCurrent},
    {"eglSwapBuffers", (void*)&Hook_eglSwapBuffers, (void**)&g_egl.eglSwapBuffers},
    {"eglSwapBuffersWithDamageKHR", (void*)&Hook_eglSwapBuffersWithDamageKHR, (void**)&g_egl.eglSwapBuffersWithDamageKHR},
    {"eglSwapBuffersWithDamageEXT", (void*)&Hook_eglSwapBuffersWithDamageEXT, (void**)&g_egl.eglSwapBuffersWithDamageEXT},
    {"eglCreateWindowSurface", (void*)&Hook_eglCreateWindowSurface, (void**)&g_egl.eglCreateWindowSurface},
    {"eglCreatePlatformWindowSurface", (void*)&Hook_eglCreatePlatformWindowSurface, (void**)&g_egl.eglCreatePlatformWindowSurface},
    {"eglCreatePlatformWindowSurfaceEXT", (void*)&Hook_eglCreatePlatformWindowSurfaceEXT, (void**)&g_egl.eglCreatePlatformWindowSurfaceEXT},
    {"eglCreatePbufferSurface", (void*)&Hook_eglCreatePbufferSurface, (void**)&g_egl.eglCreatePbufferSurface},
    {"eglDestroySurface", (void*)&Hook_eglDestroySurface, (void**)&g_egl.eglDestroySurface},
};
const size_t kEglHookCount = sizeof(kEglHooks) / sizeof(kEglHooks[0]);

const EglImport kEglImports[] = {
    {"eglQuerySurface", (void**)&g_egl.eglQuerySurface},
    {"eglQueryContext", (void**)&g_egl.eglQueryContext},
    {"eglGetConfigAttrib", (void**)&g_egl.eglGetConfigAttrib},
    {"eglGetCurrentContext", (void**)&g_egl.eglGetCurrentContext},
    {"eglGetCurrentSurface", (void**)&g_egl.eglGetCurrentSurface},
    {"eglGetCurrentDisplay", (void**)&g_egl.eglGetCurrentDisplay},
    {"eglQueryAPI", (void**)&g_egl.eglQueryAPI},
};
const size_t kEglImportCount = sizeof(kEglImports) / sizeof(kEglImports[0]);

void* EglLookupProc(const char* name)
{
    return g_egl.eglGetProcAddress ? (void*)g_egl.eglGetProcAddress(name) : nullptr;
}

Drawable EglDrawable(Context* c)
{
    Drawable d;
    if (!g_egl.eglQuerySurface || !g_egl.eglGetCurrentSurface)
        return d;
    EGLDisplay display = g_egl.eglGetCurrentDisplay ? g_egl.eglGetCurrentDisplay() : c->display;
    EGLSurface surface = g_egl.eglGetCurrentSurface(EGL_DRAW);
    EGLint w = 0, h = 0, colorspace = 0;
    g_egl.eglQuerySurface(display, surface, EGL_WIDTH, &w);
    g_egl.eglQuerySurface(display, surface, EGL_HEIGHT, &h);
    g_egl.eglQuerySurface(display, surface, EGL_GL_COLORSPACE, &colorspace);
    d.id = SurfaceId(surface);
    d.width = w;
    d.height = h;
    d.srgb = colorspace == EGL_GL_COLORSPACE_SRGB;
    return d;
}

#if defined(__linux__) && !defined(__ANDROID__)
}  // namespace glesinsp

// Linux: the hooks exported under EGL's own names, for an application linked against libEGL (the
// library is preloaded), or one that gets them from the handle dlopen("libEGL.so.1") returned, which
// is this library's (platform_linux.cpp).
#define GLESINSP_EXPORT extern "C" __attribute__((visibility("default")))
GLESINSP_EXPORT EGLFuncPtr eglGetProcAddress(const char* name) { return glesinsp::Hook_eglGetProcAddress(name); }
GLESINSP_EXPORT EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint* a) { return glesinsp::Hook_eglCreateContext(d, c, s, a); }
GLESINSP_EXPORT EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c) { return glesinsp::Hook_eglDestroyContext(d, c); }
GLESINSP_EXPORT EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface r, EGLContext c) { return glesinsp::Hook_eglMakeCurrent(d, dr, r, c); }
GLESINSP_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) { return glesinsp::Hook_eglSwapBuffers(d, s); }
GLESINSP_EXPORT EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay d, EGLSurface s, const EGLint* r, EGLint n) { return glesinsp::Hook_eglSwapBuffersWithDamageKHR(d, s, r, n); }
GLESINSP_EXPORT EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay d, EGLSurface s, const EGLint* r, EGLint n) { return glesinsp::Hook_eglSwapBuffersWithDamageEXT(d, s, r, n); }
GLESINSP_EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint* a) { return glesinsp::Hook_eglCreateWindowSurface(d, c, w, a); }
GLESINSP_EXPORT EGLSurface eglCreatePlatformWindowSurface(EGLDisplay d, EGLConfig c, void* w, const EGLAttrib* a) { return glesinsp::Hook_eglCreatePlatformWindowSurface(d, c, w, a); }
GLESINSP_EXPORT EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay d, EGLConfig c, void* w, const EGLint* a) { return glesinsp::Hook_eglCreatePlatformWindowSurfaceEXT(d, c, w, a); }
GLESINSP_EXPORT EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint* a) { return glesinsp::Hook_eglCreatePbufferSurface(d, c, a); }
GLESINSP_EXPORT EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) { return glesinsp::Hook_eglDestroySurface(d, s); }

namespace glesinsp
{
#endif

void* HookFor(const char* name)
{
    static const std::unordered_map<std::string, void*> hooks = [] {
        std::unordered_map<std::string, void*> m;
        for (size_t i = 0; i < kHookCount; ++i)
            m[kHooks[i].name] = kHooks[i].hook;
        for (size_t i = 0; i < kEglHookCount; ++i)
            m[kEglHooks[i].name] = kEglHooks[i].hook;
        return m;
    }();
    auto it = hooks.find(name ? name : "");
    return it == hooks.end() ? nullptr : it->second;
}

}  // namespace glesinsp
