// Android: the library is an OpenGL ES layer (Android 10 and later).
//
// Android's EGL loader loads the libraries named in the `gpu_debug_layers_gles` setting into a
// debuggable application it is told to (`gpu_debug_app`), from the application's data directory or
// from a layer package (`gpu_debug_layer_app`), and builds its dispatch table through them: for every
// EGL and GL entry point it knows, it asks each layer's AndroidGLESLayer_GetProcAddress for what to
// call, handing it the next one in the chain (the driver's, with one layer). The library answers with
// its hook for the entry points it records, and keeps `next` as the real one the hook calls; the rest
// it keeps too (the queries it makes itself) and hands back unchanged. So nothing is patched: every
// call the application makes through libEGL and libGLESv*, however it got the pointer, goes through
// the dispatch table and so through the hooks. eglGetProcAddress is hooked as on Windows, for the
// entry points the loader's table does not know.
//
// The settings come from `debug.glesinsp.*` system properties (gpuinsp::sdk::Config), the log goes to
// logcat (tag "glesinsp"), and the inspector connects through `adb forward` to the abstract socket
// @glesinsp:<port>:<package> the library listens on (gpuinsp::sdk::Server).
#include "egl_api.h"
#include "state.h"

#include <cstring>
#include <unistd.h>

typedef void* (*PFNEGLGETNEXTLAYERPROCADDRESSPROC)(void* layerId, const char* name);

namespace glesinsp {
namespace {

bool g_initialized = false;

/** `next` in the slot `real`, and the hook for it in its place; `next` alone when there is none (the driver lacks the entry point). */
void* Chain(void* hook, void** real, void* next) {
    if (!next) return nullptr;
    *real = next;
    return hook;
}

}  // namespace
}  // namespace glesinsp

using namespace glesinsp;

extern "C" __attribute__((visibility("default")))
void AndroidGLESLayer_Initialize(void* layerId, PFNEGLGETNEXTLAYERPROCADDRESSPROC getNext) {
    (void)layerId;
    (void)getNext;
    if (g_initialized) return;
    g_initialized = true;
    LogAlways("loaded into pid %d as an OpenGL ES layer", (int)getpid());
}

extern "C" __attribute__((visibility("default")))
void* AndroidGLESLayer_GetProcAddress(const char* name, EGLFuncPtr next) {
    if (!name) return (void*)next;
    for (size_t i = 0; i < kHookCount; ++i) {
        if (strcmp(kHooks[i].name, name) == 0) return Chain(kHooks[i].hook, kHooks[i].real, (void*)next);
    }
    for (size_t i = 0; i < kEglHookCount; ++i) {
        if (strcmp(kEglHooks[i].name, name) == 0) return Chain(kEglHooks[i].hook, kEglHooks[i].real, (void*)next);
    }
    // The ones not hooked (glGet* and the other queries) are called directly.
    for (size_t i = 0; i < kEglImportCount; ++i) {
        if (strcmp(kEglImports[i].name, name) == 0 && next) *kEglImports[i].slot = (void*)next;
    }
    void** slots = reinterpret_cast<void**>(&g_gl);
    for (size_t i = 0; i < kCommandCount; ++i) {
        if (strcmp(kCommandNames[i], name) == 0) {
            if (next && !slots[i]) slots[i] = (void*)next;
            break;
        }
    }
    return (void*)next;
}
