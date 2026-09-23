/*
 * GPU Inspector: asking for a capture from inside the application.
 *
 * One header, no library to link. The capture library is already in the process when the
 * application was started from GPU Inspector (the Vulkan layer, the Metal library, the injected
 * Direct3D 12 library, or a plugin's: Direct3D 11, OpenGL ES), and these functions find it there.
 * When it is not — the application was started some other way, or this is a build nobody is
 * inspecting — they return 0 and do nothing, so the calls can stay in the code.
 *
 *     #include "gpu_inspector.h"
 *
 *     if (the_frame_i_care_about) gpu_inspector_capture(1);
 *     if (!test_passed) gpu_inspector_capture_named(1, "shadow test failed");
 *
 * A capture is of whole frames, and starts at the next frame boundary the capture library sees, a
 * frame or two after the call: the request goes to the inspector, which takes the capture with the
 * options its capture bar has set, exactly as if Capture had been pressed at that moment. It opens
 * in a tab of its own, named after the label when one was given, so a capture taken from an
 * assertion or a failed test says what it is.
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
#include <string.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef int (*gpu_inspector_pfn_capture)(uint32_t);
    typedef int (*gpu_inspector_pfn_capture_named)(uint32_t, const char*);
    typedef int (*gpu_inspector_pfn_connected)(void);

/*
 * An entry point of whichever capture library is in this process, or null. Several can be loaded
 * at once (GPU Inspector starts a Windows application with all of them, since it cannot know the
 * API in advance); the ones not in use answer "not connected" and are passed over.
 */
    static inline void* gpu_inspector_symbol(const char* name)
    {
#if defined(_WIN32)
        static const char* const modules[] = {
            "dxinsp_capture.dll", "VkLayer_inspector_capture.dll", "d3d11insp_capture.dll", "glesinsp_capture.dll"};
        int i;
        for (i = 0; i < 4; ++i)
        {
            HMODULE module = GetModuleHandleA(modules[i]);
            gpu_inspector_pfn_connected connected;
            if (!module)
                continue;
            connected = (gpu_inspector_pfn_connected)(void*)GetProcAddress(module, "GpuInspectorConnected");
            if (!connected || !connected())
                continue;
            return (void*)GetProcAddress(module, name);
        }
        return 0;
#elif defined(__APPLE__)
    /* dlopen matches a loaded image by its path, and the library was inserted by one the
     * application does not know; the loaded images are walked for its leaf name instead. */
    static const char* const leaf = "libmtlinsp_capture.dylib";
    uint32_t count = _dyld_image_count(), i;
    for (i = 0; i < count; ++i)
    {
        const char* path = _dyld_get_image_name(i);
        const char* slash;
        void* module;
        void* symbol;
        gpu_inspector_pfn_connected connected;
        if (!path)
            continue;
        slash = strrchr(path, '/');
        if (strcmp(slash ? slash + 1 : path, leaf) != 0)
            continue;
        /* RTLD_NOLOAD: a handle to the image already in, never a second copy. */
        module = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        if (!module)
            continue;
        connected = (gpu_inspector_pfn_connected)dlsym(module, "GpuInspectorConnected");
        symbol = connected && connected() ? dlsym(module, name) : 0;
        dlclose(module);
        if (symbol)
            return symbol;
    }
    return 0;
#else
    /* Linux and Android: the Vulkan layer, or the OpenGL ES library (preloaded, or Android's
     * OpenGL ES layer). RTLD_NOLOAD: only if the loader already brought it in. */
    static const char* const modules[] = {"libVkLayer_inspector_capture.so", "libglesinsp_capture.so"};
    int i;
    for (i = 0; i < 2; ++i)
    {
        void* module = dlopen(modules[i], RTLD_NOW | RTLD_NOLOAD);
        void* symbol;
        gpu_inspector_pfn_connected connected;
        if (!module)
            continue;
        connected = (gpu_inspector_pfn_connected)dlsym(module, "GpuInspectorConnected");
        symbol = connected && connected() ? dlsym(module, name) : 0;
        dlclose(module);
        if (symbol)
            return symbol;
    }
    return 0;
#endif
    }

/* 1 when a capture library is in the process and an inspector is connected to it. */
    static inline int gpu_inspector_connected(void)
    {
        gpu_inspector_pfn_connected fn = (gpu_inspector_pfn_connected)gpu_inspector_symbol("GpuInspectorConnected");
        return fn ? fn() : 0;
    }

/*
 * Asks the inspector to capture `frame_count` frames (0 means 1), starting at the next frame
 * boundary it can, in a tab named after `label` (null or empty: the frame number alone). The label
 * is what a capture from an assertion or a test says it is; it goes into the tab's name and the
 * saved file's. Returns 1 when the request was sent, 0 when there is nobody to send it to.
 * Safe from any thread. A request made while a capture is already being taken is ignored.
 */
    static inline int gpu_inspector_capture_named(uint32_t frame_count, const char* label)
    {
        gpu_inspector_pfn_capture_named named = (gpu_inspector_pfn_capture_named)gpu_inspector_symbol("GpuInspectorCaptureNamed");
        gpu_inspector_pfn_capture fn;
        if (named)
            return named(frame_count, label);
    /* A capture library from before labels: the capture without one. */
        fn = (gpu_inspector_pfn_capture)gpu_inspector_symbol("GpuInspectorCapture");
        return fn ? fn(frame_count) : 0;
    }

/* The same capture, with no label: the tab is named by its frame number. */
    static inline int gpu_inspector_capture(uint32_t frame_count)
    {
        return gpu_inspector_capture_named(frame_count, 0);
    }

#ifdef __cplusplus
}
#endif

#endif /* GPU_INSPECTOR_H */
