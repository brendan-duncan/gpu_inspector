/*
 * GPU Inspector: asking for a capture from inside the application.
 *
 * One header, no library to link. The capture library is already in the process when the
 * application was started from GPU Inspector (the Vulkan layer, or the injected Direct3D 12
 * library), and these functions find it there. When it is not — the application was started some
 * other way, or this is a build nobody is inspecting — they return 0 and do nothing, so the calls
 * can stay in the code.
 *
 *     #include "gpu_inspector.h"
 *
 *     if (the_frame_i_care_about) gpu_inspector_capture(1);
 *
 * A capture is of whole frames, and starts at the next frame boundary the capture library sees, a
 * frame or two after the call: the request goes to the inspector, which takes the capture with the
 * options its capture bar has set, exactly as if Capture had been pressed at that moment. It opens
 * in a tab of its own.
 *
 * C and C++. MIT, like the rest of GPU Inspector.
 */
#ifndef GPU_INSPECTOR_H
#define GPU_INSPECTOR_H

#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*gpu_inspector_pfn_capture)(uint32_t);
typedef int (*gpu_inspector_pfn_connected)(void);

/* An entry point of whichever capture library is in this process, or null. */
static inline void* gpu_inspector_symbol(const char* name) {
#if defined(_WIN32)
    static const char* const modules[] = { "dxinsp_capture.dll", "VkLayer_inspector_capture.dll" };
    for (int i = 0; i < 2; ++i) {
        HMODULE module = GetModuleHandleA(modules[i]);
        if (!module) continue;
        /* Both can be loaded at once (GPU Inspector starts a Windows application with both, since
         * it cannot know the API in advance); the one that is not in use answers "not connected". */
        gpu_inspector_pfn_connected connected = (gpu_inspector_pfn_connected)(void*)GetProcAddress(module, "GpuInspectorConnected");
        if (!connected || !connected()) continue;
        return (void*)GetProcAddress(module, name);
    }
    return 0;
#else
    /* RTLD_NOLOAD: only if the loader already brought the layer in. */
    void* module = dlopen("libVkLayer_inspector_capture.so", RTLD_NOW | RTLD_NOLOAD);
    return module ? dlsym(module, name) : 0;
#endif
}

/* 1 when a capture library is in the process and an inspector is connected to it. */
static inline int gpu_inspector_connected(void) {
    gpu_inspector_pfn_connected fn = (gpu_inspector_pfn_connected)gpu_inspector_symbol("GpuInspectorConnected");
    return fn ? fn() : 0;
}

/*
 * Asks the inspector to capture `frame_count` frames (0 means 1), starting at the next frame
 * boundary it can. Returns 1 when the request was sent, 0 when there is nobody to send it to.
 * Safe from any thread. A request made while a capture is already being taken is ignored.
 */
static inline int gpu_inspector_capture(uint32_t frame_count) {
    gpu_inspector_pfn_capture fn = (gpu_inspector_pfn_capture)gpu_inspector_symbol("GpuInspectorCapture");
    return fn ? fn(frame_count) : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* GPU_INSPECTOR_H */
