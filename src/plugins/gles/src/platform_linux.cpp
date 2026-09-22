// Linux: how the library gets in and finds OpenGL ES.
//
// The inspector preloads the library into an application it launches (LD_PRELOAD, plugin.json), with
// its settings in the environment. OpenGL ES reaches a Linux application from libEGL.so.1 and
// libGLESv2.so.2 (libglvnd's, or Mesa's), or from an OpenGL ES profile context of GLX's (hooks_glx.cpp).
// Three ways in, and each is covered:
//   - An application linked against those libraries calls whichever loaded library exports a name
//     first, and a preloaded one comes first: every hook is exported under the entry point's own name
//     (gen/gles_hooks.gen.cpp, hooks_egl.cpp, hooks_glx.cpp).
//   - One that loads them itself (SDL, GLFW, most engines) calls dlopen("libEGL.so.1") and dlsym()s
//     from the handle, which a preloaded export does not reach: dlopen is hooked, and for the system's
//     libEGL or libGLESv2 it returns this library's own handle. A dlsym() from it finds the hooks here,
//     and every other name in this library's dependencies, which are those same libraries.
//   - Entry points fetched by name at run time come from eglGetProcAddress or glXGetProcAddress, which
//     are hooked and hand out the hooks.
// The real entry points the hooks call are the next definitions after this library's (RTLD_NEXT).
//
// An EGL or GLES library an application ships in its own directory (Chromium's and Electron's ANGLE)
// is a different library from the system's, so its handle is passed through, and it is not captured.
#include "egl_api.h"
#include "state.h"

#include <gpu_inspector/sdk/config.h>

#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace glesinsp {

/** The definition of `name` that this library's stands in front of: the real entry point. */
void* RealSymbol(const char* name) {
    return dlsym(RTLD_NEXT, name);
}

/**
 * The entry points the libraries do not export (libGLESv2 exports the core ones), for the library's
 * own calls: from eglGetProcAddress, once the process makes an OpenGL ES context.
 */
void ResolveEglProcs() {
    static bool resolved = false;
    if (resolved || !g_egl.eglGetProcAddress) return;
    resolved = true;
    void** slots = reinterpret_cast<void**>(&g_gl);
    for (size_t i = 0; i < kCommandCount; ++i) {
        if (!slots[i]) slots[i] = (void*)g_egl.eglGetProcAddress(kCommandNames[i]);
    }
}

namespace {

using PFN_dlopen = void* (*)(const char*, int);

PFN_dlopen RealDlopen() {
    static PFN_dlopen real = (PFN_dlopen)dlsym(RTLD_NEXT, "dlopen");
    return real;
}

const char* BaseName(const char* path) {
    const char* slash = path ? strrchr(path, '/') : nullptr;
    return slash ? slash + 1 : path ? path : "";
}

bool StartsWith(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/** libEGL.so, libEGL.so.1, libGLESv2.so.2...: the libraries whose handle this library stands in for. */
bool IsGlesLibrary(const char* file) {
    const char* base = BaseName(file);
    return StartsWith(base, "libEGL.so") || StartsWith(base, "libGLESv2.so");
}

/** This library's own handle, one more reference to it. */
void* SelfHandle() {
    Dl_info info{};
    if (!dladdr((void*)&SelfHandle, &info) || !info.dli_fname) return nullptr;
    return RealDlopen()(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
}

/**
 * dlopen(file) as the caller would have had it. The dynamic loader searches a bare name along the
 * calling object's own path (its RUNPATH, $ORIGIN), and a call forwarded from here would search this
 * library's instead: so a bare name not loaded yet is looked for along the caller's path first.
 */
void* OpenAsCaller(const char* file, int flags, void* caller) {
    if (!file || strchr(file, '/')) return RealDlopen()(file, flags);
    if (void* loaded = RealDlopen()(file, flags | RTLD_NOLOAD)) {
        dlclose(loaded);
        return RealDlopen()(file, flags);
    }
    Dl_info info{};
    struct link_map* map = nullptr;
    if (caller && dladdr1(caller, &info, (void**)&map, RTLD_DL_LINKMAP) && map) {
        Dl_serinfo size{};
        if (dlinfo(map, RTLD_DI_SERINFOSIZE, &size) == 0 && size.dls_size >= sizeof(Dl_serinfo)) {
            std::vector<char> buffer(size.dls_size);
            Dl_serinfo* paths = reinterpret_cast<Dl_serinfo*>(buffer.data());
            paths->dls_size = size.dls_size;
            paths->dls_cnt = size.dls_cnt;
            if (dlinfo(map, RTLD_DI_SERINFOSIZE, paths) == 0 && dlinfo(map, RTLD_DI_SERINFO, paths) == 0) {
                for (unsigned i = 0; i < paths->dls_cnt; ++i) {
                    const std::string candidate = std::string(paths->dls_serpath[i].dls_name) + "/" + file;
                    if (access(candidate.c_str(), F_OK) == 0) return RealDlopen()(candidate.c_str(), flags);
                }
            }
        }
    }
    return RealDlopen()(file, flags);
}

/** Every real entry point the hooks call and the library's own calls use, from the libraries after this one. */
void ResolveReal() {
    size_t found = 0;
    for (size_t i = 0; i < kHookCount; ++i) {
        if (!*kHooks[i].real && (*kHooks[i].real = RealSymbol(kHooks[i].name))) ++found;
    }
    void** slots = reinterpret_cast<void**>(&g_gl);
    for (size_t i = 0; i < kCommandCount; ++i) {
        if (!slots[i]) slots[i] = RealSymbol(kCommandNames[i]);
    }
    for (size_t i = 0; i < kEglHookCount; ++i) {
        if (!*kEglHooks[i].real) *kEglHooks[i].real = RealSymbol(kEglHooks[i].name);
    }
    for (size_t i = 0; i < kEglImportCount; ++i) {
        if (!*kEglImports[i].slot) *kEglImports[i].slot = RealSymbol(kEglImports[i].name);
    }
    Log("%zu of %zu GL entry points found in the system's libraries", found, kHookCount);
}

__attribute__((constructor)) void Initialize() {
    Log("loaded into pid %d", (int)getpid());
    ResolveReal();
}

}  // namespace
}  // namespace glesinsp

using namespace glesinsp;

/**
 * dlopen: for the system's libEGL or libGLESv2, this library's handle in its place (see above).
 * The libraries' own calls (libglvnd loading its vendor libraries) and this library's pass through.
 */
extern "C" __attribute__((visibility("default"))) void* dlopen(const char* file, int flags) {
    void* caller = __builtin_return_address(0);
    void* handle = OpenAsCaller(file, flags, caller);
    if (!handle || !file || !IsGlesLibrary(file)) return handle;
    Dl_info from{};
    if (dladdr(caller, &from) && from.dli_fname) {
        const char* base = BaseName(from.dli_fname);
        if (StartsWith(base, "libEGL") || StartsWith(base, "libGL") || strstr(base, "glesinsp")) return handle;
    }
    // Only the libraries this one depends on: another one of the same name is the application's own.
    void* system = RealDlopen()(StartsWith(BaseName(file), "libEGL") ? "libEGL.so.1" : "libGLESv2.so.2", RTLD_NOW | RTLD_NOLOAD);
    const bool same = system == handle;
    if (system) dlclose(system);
    if (!same) {
        static bool said = false;
        if (!said) {
            said = true;
            LogAlways("%s is not the system's library (an application's own, such as ANGLE): its OpenGL ES is not captured", file);
        }
        return handle;
    }
    void* self = SelfHandle();
    if (!self) return handle;
    dlclose(handle);
    return self;
}
