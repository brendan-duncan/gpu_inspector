// How the library gets in: the inspector's launcher (dxinsp_launch.exe, which injects the D3D12
// library the same way) loads this library into the target before its first instruction and calls
// GpuInspectorInitialize, with the settings as an environment block when it did not start the
// process itself (plugin.json names the library; docs/PLUGINS.md).
//
// Direct3D 11 has two exported entry points every device passes through, D3D11CreateDevice and
// D3D11CreateDeviceAndSwapChain, and COM objects whose methods are reached through a vtable. So
// d3d11.dll is loaded here (a system library the process would load anyway if it uses the API)
// and its two exports patched in place (MinHook); everything below is a vtable patch on the
// objects the devices hand out (hooks.h). A process that never creates a device costs nothing
// more than the two patched functions.
#include "common.h"
#include "hooks.h"

#include <gpu_inspector/sdk/config.h>

namespace {
bool g_initialized = false;
}

using namespace d3d11insp;

extern "C" __declspec(dllexport) DWORD WINAPI GpuInspectorInitialize(LPVOID settings) {
    if (g_initialized) return 0;
    g_initialized = true;
    const std::string applied = gpuinsp::sdk::Config::Get().ApplySettingsBlock((const wchar_t*)settings);
    LogAlways("loaded into pid %lu", GetCurrentProcessId());
    if (!applied.empty()) LogAlways("settings from the launcher: %s", applied.c_str());
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) {
        LogAlways("d3d11.dll could not be loaded (%lu): no Direct3D 11 capture in this process", GetLastError());
        return 1;
    }
    if (!InstallEntryPointHooks(d3d11)) return 1;
    LogAlways("hooked D3D11CreateDevice and D3D11CreateDeviceAndSwapChain");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
