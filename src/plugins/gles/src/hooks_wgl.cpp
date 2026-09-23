// WGL: OpenGL ES from the desktop driver. A Windows application can ask the GPU's own OpenGL driver
// for an OpenGL ES context (WGL_EXT_create_context_es2_profile), which is what Unity's -force-gles32
// and -force-gles31 do: no EGL and no ANGLE, only opengl32.dll and the driver behind it.
//
// Such an application makes a context with wglCreateContextAttribsARB, which it can only get from
// wglGetProcAddress, and gets every GL entry point beyond OpenGL 1.1 the same way; the 1.1 ones
// (glDrawElements, glClear, glBindTexture...) are opengl32.dll's own exports. So opengl32's wgl
// exports are hooked as it loads, wglGetProcAddress hands out our hooks, and opengl32's GL exports
// are hooked once an OpenGL ES context exists. A context made without the ES profile is desktop
// OpenGL, which this library does not capture: it is never taken on, and its calls pass straight
// through the hooks. Frames end at SwapBuffers (gdi32) or wglSwapBuffers.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "capture.h"
#include "platform_win32.h"
#include "server.h"
#include "state.h"

#include <cstring>

namespace glesinsp
{
namespace
{

constexpr int kContextMajorVersion = 0x2091;       // WGL_CONTEXT_MAJOR_VERSION_ARB
constexpr int kContextMinorVersion = 0x2092;       // WGL_CONTEXT_MINOR_VERSION_ARB
constexpr int kContextProfileMask = 0x9126;        // WGL_CONTEXT_PROFILE_MASK_ARB
constexpr int kContextEs2ProfileBit = 0x0004;      // WGL_CONTEXT_ES2_PROFILE_BIT_EXT

using PFN_wglGetProcAddress = PROC(WINAPI*)(LPCSTR);
using PFN_wglCreateContextAttribsARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PFN_wglMakeCurrent = BOOL(WINAPI*)(HDC, HGLRC);
using PFN_wglDeleteContext = BOOL(WINAPI*)(HGLRC);
using PFN_wglShareLists = BOOL(WINAPI*)(HGLRC, HGLRC);
using PFN_wglSwapBuffers = BOOL(WINAPI*)(HDC);
using PFN_wglSwapLayerBuffers = BOOL(WINAPI*)(HDC, UINT);
using PFN_SwapBuffers = BOOL(WINAPI*)(HDC);
using PFN_wglGetCurrentDC = HDC(WINAPI*)();

struct WglDispatch
{
    PFN_wglGetProcAddress wglGetProcAddress;
    PFN_wglCreateContextAttribsARB wglCreateContextAttribsARB;
    PFN_wglMakeCurrent wglMakeCurrent;
    PFN_wglDeleteContext wglDeleteContext;
    PFN_wglShareLists wglShareLists;
    PFN_wglSwapBuffers wglSwapBuffers;
    PFN_wglSwapLayerBuffers wglSwapLayerBuffers;
    PFN_SwapBuffers SwapBuffers;
    PFN_wglGetCurrentDC wglGetCurrentDC;
};
WglDispatch g_wgl{};

HMODULE g_opengl32 = nullptr;
bool g_glExportsHooked = false;
bool g_procsResolved = false;
/** A swap in progress on this thread: gdi32's SwapBuffers may reach wglSwapBuffers, which must not count again. */
thread_local int t_swapDepth = 0;

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

/**
 * Every GL entry point the application has not asked for yet, while an OpenGL ES context is current
 * (wglGetProcAddress answers only then): the read-backs call ones the application may never use.
 */
void ResolveProcs()
{
    if (g_procsResolved || !g_wgl.wglGetProcAddress)
        return;
    g_procsResolved = true;
    void** slots = reinterpret_cast<void**>(&g_gl);
    for (size_t i = 0; i < kCommandCount; ++i)
    {
        if (slots[i])
            continue;
        PROC p = g_wgl.wglGetProcAddress(kCommandNames[i]);
        // Some drivers answer 1, 2, 3 or -1 for a name they do not have.
        if ((uintptr_t)p > 3 && (intptr_t)p != -1)
            slots[i] = (void*)p;
    }
}

/** A surface object for the device context the context draws into (its window's). */
void EnsureSurface(HDC dc)
{
    if (!dc || SurfaceId(dc))
        return;
    HWND window = WindowFromDC(dc);
    RECT r{};
    if (window)
        GetClientRect(window, &r);
    Object& o = NewObject("GLSurface", ObjType::Count, 0, "wglMakeCurrent");
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)dc);
    o.handle = buf;
    o.args.push_back({"kind", JsonString("window")});
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)window);
    o.args.push_back({"window", JsonString(buf)});
    o.args.push_back({"width", JsonInt(r.right - r.left)});
    o.args.push_back({"height", JsonInt(r.bottom - r.top)});
    {
        std::lock_guard lock(State().mutex);
        State().surfaces[dc] = o.id;
    }
    Announce(o);
}

HGLRC WINAPI Hook_wglCreateContextAttribsARB(HDC dc, HGLRC share, const int* attribs)
{
    HGLRC result = g_wgl.wglCreateContextAttribsARB(dc, share, attribs);
    if (!result || !(Attrib(attribs, kContextProfileMask, 0) & kContextEs2ProfileBit))
        return result;
    // An OpenGL ES context: from here the process is one this library captures.
    if (!g_glExportsHooked && g_opengl32)
    {
        g_glExportsHooked = true;
        HookGlModule(g_opengl32);
    }
    StartServer();
    auto c = std::make_unique<Context>();
    c->handle = result;
    c->wgl = true;
    c->major = Attrib(attribs, kContextMajorVersion, 2);
    c->minor = Attrib(attribs, kContextMinorVersion, 0);
    Context* shared = ContextOf(share);
    c->share = shared ? shared->share : std::make_shared<ShareGroup>();
    Object& o = NewObject("GLContext", ObjType::Count, 0, "wglCreateContextAttribsARB");
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)result);
    o.handle = buf;
    o.args.push_back({"clientVersion", JsonInt(c->major)});
    if (c->minor)
        o.args.push_back({"minorVersion", JsonInt(c->minor)});
    o.args.push_back({"shareContext", JsonRef(shared ? shared->id : 0, "GLContext")});
    o.args.push_back({"windowSystem", JsonString("WGL")});
    c->id = o.id;
    {
        std::lock_guard lock(State().mutex);
        State().contexts[result] = std::move(c);
    }
    Announce(o);
    LogAlways("context %s: OpenGL ES %d.%d from the desktop driver (WGL)", o.handle.c_str(), Attrib(attribs, kContextMajorVersion, 2), Attrib(attribs, kContextMinorVersion, 0));
    return result;
}

BOOL WINAPI Hook_wglMakeCurrent(HDC dc, HGLRC rc)
{
    const BOOL result = g_wgl.wglMakeCurrent(dc, rc);
    if (!result)
        return result;
    Context* c = rc ? ContextOf(rc) : nullptr;
    // Current() would take an unknown context on; a WGL one this library did not see made is desktop OpenGL.
    SetCurrent(c);
    if (c)
    {
        c->drawSurface = dc;
        ResolveProcs();
        EnsureSurface(dc);
    }
    return result;
}

BOOL WINAPI Hook_wglDeleteContext(HGLRC rc)
{
    const BOOL result = g_wgl.wglDeleteContext(rc);
    if (!result)
        return result;
    uint64_t id = 0;
    {
        std::lock_guard lock(State().mutex);
        auto it = State().contexts.find(rc);
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
    return result;
}

BOOL WINAPI Hook_wglShareLists(HGLRC a, HGLRC b)
{
    const BOOL result = g_wgl.wglShareLists(a, b);
    Context* from = ContextOf(a);
    Context* to = ContextOf(b);
    if (result && from && to)
        to->share = from->share;
    return result;
}

/** A frame's end: the current OpenGL ES context's pass ends and the capture moves on a frame. */
BOOL Swap(HDC dc, const char* method, BOOL (*call)(HDC, void*), void* arg)
{
    Context* c = t_swapDepth ? nullptr : CurrentKnown();
    if (c && !c->wgl)
        c = nullptr;
    ++t_swapDepth;
    if (c)
        BeforeSwap(c, nullptr, dc, method);
    const BOOL result = call(dc, arg);
    if (c)
        AfterSwap(c);
    --t_swapDepth;
    return result;
}

BOOL WINAPI Hook_SwapBuffers(HDC dc)
{
    return Swap(dc, "SwapBuffers", [](HDC d, void*) { return g_wgl.SwapBuffers(d); }, nullptr);
}

BOOL WINAPI Hook_wglSwapBuffers(HDC dc)
{
    return Swap(dc, "wglSwapBuffers", [](HDC d, void*) { return g_wgl.wglSwapBuffers(d); }, nullptr);
}

BOOL WINAPI Hook_wglSwapLayerBuffers(HDC dc, UINT planes)
{
    return Swap(dc, "wglSwapLayerBuffers", [](HDC d, void* p) { return g_wgl.wglSwapLayerBuffers(d, (UINT)(uintptr_t)p); }, (void*)(uintptr_t)planes);
}

PROC WINAPI Hook_wglGetProcAddress(LPCSTR name)
{
    PROC real = g_wgl.wglGetProcAddress(name);
    if (!name || (uintptr_t)real <= 3 || (intptr_t)real == -1)
        return real;
    if (strcmp(name, "wglCreateContextAttribsARB") == 0)
    {
        if (!g_wgl.wglCreateContextAttribsARB)
            g_wgl.wglCreateContextAttribsARB = (PFN_wglCreateContextAttribsARB)real;
        return (PROC)&Hook_wglCreateContextAttribsARB;
    }
    // A GL entry point: our hook stands in for the driver's, which is what the hook calls.
    for (size_t i = 0; i < kHookCount; ++i)
    {
        if (strcmp(kHooks[i].name, name) != 0)
            continue;
        if (!*kHooks[i].real)
            *kHooks[i].real = (void*)real;
        return (PROC)kHooks[i].hook;
    }
    return real;
}

}  // namespace

void HookWgl(HMODULE opengl32)
{
    g_opengl32 = opengl32;
    g_wgl.wglGetCurrentDC = (PFN_wglGetCurrentDC)GetProcAddress(opengl32, "wglGetCurrentDC");
    size_t hooked = 0;
    hooked += HookExport(opengl32, "wglGetProcAddress", (void*)&Hook_wglGetProcAddress, (void**)&g_wgl.wglGetProcAddress);
    hooked += HookExport(opengl32, "wglMakeCurrent", (void*)&Hook_wglMakeCurrent, (void**)&g_wgl.wglMakeCurrent);
    hooked += HookExport(opengl32, "wglDeleteContext", (void*)&Hook_wglDeleteContext, (void**)&g_wgl.wglDeleteContext);
    hooked += HookExport(opengl32, "wglShareLists", (void*)&Hook_wglShareLists, (void**)&g_wgl.wglShareLists);
    hooked += HookExport(opengl32, "wglSwapBuffers", (void*)&Hook_wglSwapBuffers, (void**)&g_wgl.wglSwapBuffers);
    hooked += HookExport(opengl32, "wglSwapLayerBuffers", (void*)&Hook_wglSwapLayerBuffers, (void**)&g_wgl.wglSwapLayerBuffers);
    EnableHooks();
    LogAlways("hooked %zu WGL entry points", hooked);
}

void HookGdiSwap(HMODULE gdi32)
{
    if (HookExport(gdi32, "SwapBuffers", (void*)&Hook_SwapBuffers, (void**)&g_wgl.SwapBuffers))
        EnableHooks();
}

void* WglLookupProc(const char* name)
{
    PROC p = g_wgl.wglGetProcAddress ? g_wgl.wglGetProcAddress(name) : nullptr;
    return (uintptr_t)p > 3 && (intptr_t)p != -1 ? (void*)p : nullptr;
}

Drawable WglDrawable(Context* c)
{
    Drawable d;
    HDC dc = g_wgl.wglGetCurrentDC ? g_wgl.wglGetCurrentDC() : (HDC)c->drawSurface;
    if (!dc)
        dc = (HDC)c->drawSurface;
    HWND window = dc ? WindowFromDC(dc) : nullptr;
    RECT r{};
    if (window)
        GetClientRect(window, &r);
    d.id = SurfaceId(dc);
    d.width = r.right - r.left;
    d.height = r.bottom - r.top;
    return d;
}

}  // namespace glesinsp
