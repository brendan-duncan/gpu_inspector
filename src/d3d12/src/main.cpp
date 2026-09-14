// Where the library gets into the process.
//
// dxinsp_launch.exe creates the target suspended, loads this DLL into it with a remote LoadLibraryW
// thread, runs DxinspInitialize in a second remote thread, and only then resumes the main thread.
// DllMain does nothing but opt out of thread notifications: hooking under the loader lock is what
// the second thread avoids.
#include "common.h"
#include "hooks.h"
#include "ui_messages.h"

#include <cstdio>

namespace {

bool g_initialized = false;

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI DxinspInitialize(LPVOID) {
    if (g_initialized) return 0;
    g_initialized = true;
    dxinsp::LogAlways("loaded into pid %lu", GetCurrentProcessId());
    // The listener comes up before the application has a device, so the UI can be waiting when the
    // process starts, or attach later and get a snapshot either way. It starts listening only once
    // a D3D12 device exists (ui_messages.cpp), so a Vulkan application launched the same way, with
    // this library along for the ride, leaves the port to the Vulkan layer.
    if (!dxinsp::InstallEntryPointHooks()) {
        dxinsp::LogAlways("the D3D12 entry points could not be hooked: no D3D12 capture in this process");
        return 1;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
