// GLX: OpenGL ES from the desktop driver on Linux, the counterpart of WGL (hooks_wgl.cpp). An X11
// application can ask the GPU's own driver for an OpenGL ES context
// (GLX_EXT_create_context_es2_profile) through glXCreateContextAttribsARB, which it gets from
// glXGetProcAddress; its GL entry points it gets the same way, or from libGL's exports.
//
// The library is preloaded (platform_linux.cpp), so the GLX functions below are exported under their
// own names and an application linked against libGL (or libGLX) calls them; glXGetProcAddress hands
// out the hooks for the rest. A context made without the ES profile is desktop OpenGL, which this
// library does not capture: it is never taken on, and its calls pass straight through the hooks.
// Frames end at glXSwapBuffers.
#include "capture.h"
#include "server.h"
#include "state.h"

#include <cstring>
#include <dlfcn.h>

// X11 and GLX types, written out like the EGL ones (egl_api.h) so the library builds with no X or GL
// headers around: a Display is opaque, a drawable an XID.
struct _XDisplay;
typedef struct _XDisplay Display;
typedef struct __GLXcontextRec* GLXContext;
typedef struct __GLXFBConfigRec* GLXFBConfig;
typedef unsigned long GLXDrawable;
typedef int Bool;
typedef void (*GLXFuncPtr)(void);

namespace glesinsp
{

void* RealSymbol(const char* name);   // platform_linux.cpp

namespace
{

constexpr int kContextMajorVersion = 0x2091;       // GLX_CONTEXT_MAJOR_VERSION_ARB
constexpr int kContextMinorVersion = 0x2092;       // GLX_CONTEXT_MINOR_VERSION_ARB
constexpr int kContextProfileMask = 0x9126;        // GLX_CONTEXT_PROFILE_MASK_ARB
constexpr int kContextEs2ProfileBit = 0x0004;      // GLX_CONTEXT_ES2_PROFILE_BIT_EXT
constexpr int kWidth = 0x801D;                     // GLX_WIDTH
constexpr int kHeight = 0x801E;                    // GLX_HEIGHT

using PFN_glXGetProcAddress = GLXFuncPtr (*)(const unsigned char*);
using PFN_glXCreateContextAttribsARB = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
using PFN_glXMakeCurrent = Bool (*)(Display*, GLXDrawable, GLXContext);
using PFN_glXMakeContextCurrent = Bool (*)(Display*, GLXDrawable, GLXDrawable, GLXContext);
using PFN_glXDestroyContext = void (*)(Display*, GLXContext);
using PFN_glXSwapBuffers = void (*)(Display*, GLXDrawable);
using PFN_glXQueryDrawable = void (*)(Display*, GLXDrawable, int, unsigned int*);
using PFN_glXGetCurrentDrawable = GLXDrawable (*)(void);
using PFN_glXGetCurrentDisplay = Display* (*)(void);

struct GlxDispatch
{
    PFN_glXGetProcAddress glXGetProcAddress;
    PFN_glXGetProcAddress glXGetProcAddressARB;
    PFN_glXCreateContextAttribsARB glXCreateContextAttribsARB;
    PFN_glXMakeCurrent glXMakeCurrent;
    PFN_glXMakeContextCurrent glXMakeContextCurrent;
    PFN_glXDestroyContext glXDestroyContext;
    PFN_glXSwapBuffers glXSwapBuffers;
    PFN_glXQueryDrawable glXQueryDrawable;
    PFN_glXGetCurrentDrawable glXGetCurrentDrawable;
    PFN_glXGetCurrentDisplay glXGetCurrentDisplay;
};
GlxDispatch g_glx{};

/** A real GLX entry point: the one a hook stands in for, found the first time it is needed. */
template <class T>
T Real(T& slot, const char* name)
{
    if (!slot)
        slot = (T)RealSymbol(name);
    return slot;
}

GLXFuncPtr RealGetProcAddress(const char* name)
{
    PFN_glXGetProcAddress get = Real(g_glx.glXGetProcAddressARB, "glXGetProcAddressARB");
    if (!get)
        get = Real(g_glx.glXGetProcAddress, "glXGetProcAddress");
    return get ? get((const unsigned char*)name) : nullptr;
}

int Attrib(const int* list, int key, int def)
{
    if (!list)
        return def;
    for (const int* p = list; *p; p += 2)
    {
        if (p[0] == key)
            return p[1];
    }
    return def;
}

void QuerySize(Display* display, GLXDrawable drawable, int& w, int& h)
{
    unsigned int width = 0, height = 0;
    if (Real(g_glx.glXQueryDrawable, "glXQueryDrawable") && display && drawable)
    {
        g_glx.glXQueryDrawable(display, drawable, kWidth, &width);
        g_glx.glXQueryDrawable(display, drawable, kHeight, &height);
    }
    w = (int)width;
    h = (int)height;
}

/** A surface object for the drawable the context draws into. */
void EnsureSurface(Display* display, GLXDrawable drawable)
{
    if (!drawable || SurfaceId((void*)drawable))
        return;
    int w = 0, h = 0;
    QuerySize(display, drawable, w, h);
    Object& o = NewObject("GLSurface", ObjType::Count, 0, "glXMakeCurrent");
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)drawable);
    o.handle = buf;
    o.args.push_back({"kind", JsonString("window")});
    o.args.push_back({"width", JsonInt(w)});
    o.args.push_back({"height", JsonInt(h)});
    {
        std::lock_guard lock(State().mutex);
        State().surfaces[(void*)drawable] = o.id;
    }
    Announce(o);
}

/** Every GL entry point the application has not asked for yet, for the library's own calls. */
void ResolveProcs()
{
    static bool resolved = false;
    if (resolved)
        return;
    resolved = true;
    void** slots = reinterpret_cast<void**>(&g_gl);
    for (size_t i = 0; i < kCommandCount; ++i)
    {
        if (!slots[i])
            slots[i] = (void*)RealGetProcAddress(kCommandNames[i]);
    }
}

Bool MadeCurrent(Display* display, GLXDrawable drawable, GLXContext context)
{
    Context* c = context ? ContextOf((EGLContext)context) : nullptr;
    // Current() would look for an EGL context; a GLX one this library did not see made is desktop OpenGL.
    SetCurrent(c);
    if (c)
    {
        c->drawSurface = (EGLSurface)drawable;
        c->display = (EGLDisplay)display;
        ResolveProcs();
        EnsureSurface(display, drawable);
    }
    return 1;
}

}  // namespace

void* GlxLookupProc(const char* name)
{
    return (void*)RealGetProcAddress(name);
}

Drawable GlxDrawable(Context* c)
{
    Drawable d;
    GLXDrawable drawable = Real(g_glx.glXGetCurrentDrawable, "glXGetCurrentDrawable") ? g_glx.glXGetCurrentDrawable() : 0;
    if (!drawable)
        drawable = (GLXDrawable)c->drawSurface;
    Display* display = Real(g_glx.glXGetCurrentDisplay, "glXGetCurrentDisplay") ? g_glx.glXGetCurrentDisplay() : nullptr;
    if (!display)
        display = (Display*)c->display;
    QuerySize(display, drawable, d.width, d.height);
    d.id = SurfaceId((void*)drawable);
    return d;
}

}  // namespace glesinsp

using namespace glesinsp;

#define GLESINSP_EXPORT extern "C" __attribute__((visibility("default")))

GLESINSP_EXPORT GLXContext glXCreateContextAttribsARB(Display* display, GLXFBConfig config, GLXContext share, Bool direct, const int* attribs)
{
    if (!g_glx.glXCreateContextAttribsARB)
        g_glx.glXCreateContextAttribsARB = (PFN_glXCreateContextAttribsARB)RealGetProcAddress("glXCreateContextAttribsARB");
    if (!g_glx.glXCreateContextAttribsARB)
        return nullptr;
    GLXContext result = g_glx.glXCreateContextAttribsARB(display, config, share, direct, attribs);
    if (!result || !(Attrib(attribs, kContextProfileMask, 0) & kContextEs2ProfileBit))
        return result;
    // An OpenGL ES context: from here the process is one this library captures.
    StartServer();
    auto c = std::make_unique<Context>();
    c->handle = (EGLContext)result;
    c->display = (EGLDisplay)display;
    c->glx = true;
    c->major = Attrib(attribs, kContextMajorVersion, 2);
    c->minor = Attrib(attribs, kContextMinorVersion, 0);
    Context* shared = ContextOf((EGLContext)share);
    c->share = shared ? shared->share : std::make_shared<ShareGroup>();
    Object& o = NewObject("GLContext", ObjType::Count, 0, "glXCreateContextAttribsARB");
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)result);
    o.handle = buf;
    o.args.push_back({"clientVersion", JsonInt(c->major)});
    if (c->minor)
        o.args.push_back({"minorVersion", JsonInt(c->minor)});
    o.args.push_back({"shareContext", JsonRef(shared ? shared->id : 0, "GLContext")});
    o.args.push_back({"windowSystem", JsonString("GLX")});
    c->id = o.id;
    {
        std::lock_guard lock(State().mutex);
        State().contexts[(EGLContext)result] = std::move(c);
    }
    Announce(o);
    LogAlways("context %s: OpenGL ES %d.%d from the desktop driver (GLX)", o.handle.c_str(), Attrib(attribs, kContextMajorVersion, 2), Attrib(attribs, kContextMinorVersion, 0));
    return result;
}

GLESINSP_EXPORT Bool glXMakeCurrent(Display* display, GLXDrawable drawable, GLXContext context)
{
    if (!Real(g_glx.glXMakeCurrent, "glXMakeCurrent"))
        return 0;
    const Bool result = g_glx.glXMakeCurrent(display, drawable, context);
    return result ? MadeCurrent(display, drawable, context) : result;
}

GLESINSP_EXPORT Bool glXMakeContextCurrent(Display* display, GLXDrawable draw, GLXDrawable read, GLXContext context)
{
    if (!Real(g_glx.glXMakeContextCurrent, "glXMakeContextCurrent"))
        return 0;
    const Bool result = g_glx.glXMakeContextCurrent(display, draw, read, context);
    return result ? MadeCurrent(display, draw, context) : result;
}

GLESINSP_EXPORT void glXDestroyContext(Display* display, GLXContext context)
{
    if (!Real(g_glx.glXDestroyContext, "glXDestroyContext"))
        return;
    g_glx.glXDestroyContext(display, context);
    uint64_t id = 0;
    {
        std::lock_guard lock(State().mutex);
        auto it = State().contexts.find((EGLContext)context);
        if (it != State().contexts.end())
        {
            id = it->second->id;
            if (CurrentKnown() == it->second.get())
                SetCurrent(nullptr);
            State().contexts.erase(it);
        }
    }
    if (id)
        Forget(id);
}

GLESINSP_EXPORT void glXSwapBuffers(Display* display, GLXDrawable drawable)
{
    if (!Real(g_glx.glXSwapBuffers, "glXSwapBuffers"))
        return;
    Context* c = CurrentKnown();
    if (c && !c->glx)
        c = nullptr;
    if (c)
        BeforeSwap(c, nullptr, (EGLSurface)drawable, "glXSwapBuffers");
    g_glx.glXSwapBuffers(display, drawable);
    if (c)
        AfterSwap(c);
}

namespace
{

/** What glXGetProcAddress hands out: our hook for a GLX or GL entry point we hook, else the driver's. */
GLXFuncPtr GetProcAddress(const unsigned char* name, PFN_glXGetProcAddress real)
{
    GLXFuncPtr p = real ? real(name) : nullptr;
    if (!p || !name)
        return p;
    const char* n = (const char*)name;
    if (strcmp(n, "glXCreateContextAttribsARB") == 0)
    {
        if (!g_glx.glXCreateContextAttribsARB)
            g_glx.glXCreateContextAttribsARB = (PFN_glXCreateContextAttribsARB)p;
        return (GLXFuncPtr)&glXCreateContextAttribsARB;
    }
    if (strcmp(n, "glXMakeCurrent") == 0)
        return (GLXFuncPtr)&glXMakeCurrent;
    if (strcmp(n, "glXMakeContextCurrent") == 0)
        return (GLXFuncPtr)&glXMakeContextCurrent;
    if (strcmp(n, "glXDestroyContext") == 0)
        return (GLXFuncPtr)&glXDestroyContext;
    if (strcmp(n, "glXSwapBuffers") == 0)
        return (GLXFuncPtr)&glXSwapBuffers;
    // A GL entry point: our hook stands in for the driver's, which is what the hook calls.
    for (size_t i = 0; i < kHookCount; ++i)
    {
        if (strcmp(kHooks[i].name, n) != 0)
            continue;
        if (!*kHooks[i].real)
            *kHooks[i].real = (void*)p;
        return (GLXFuncPtr)kHooks[i].hook;
    }
    return p;
}

}  // namespace

GLESINSP_EXPORT GLXFuncPtr glXGetProcAddressARB(const unsigned char* name)
{
    return GetProcAddress(name, Real(g_glx.glXGetProcAddressARB, "glXGetProcAddressARB"));
}

GLESINSP_EXPORT GLXFuncPtr glXGetProcAddress(const unsigned char* name)
{
    return GetProcAddress(name, Real(g_glx.glXGetProcAddress, "glXGetProcAddress"));
}
