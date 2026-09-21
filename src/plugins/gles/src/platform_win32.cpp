// Windows: how the library gets in and finds OpenGL ES.
//
// The inspector's launcher (dxinsp_launch.exe, which injects the D3D12 library the same way) loads
// this library into the target before its first instruction and calls GpuInspectorInitialize, with
// the settings as an environment block when it did not start the process itself (plugin.json names
// the library; docs/PLUGINS.md).
//
// OpenGL ES reaches a Windows application two ways:
//   - from the desktop driver, through WGL: an OpenGL ES profile context of opengl32.dll's
//     (WGL_EXT_create_context_es2_profile), which is what Unity's -force-gles32 makes (hooks_wgl.cpp);
//   - from ANGLE, a library the application ships: libGLESv2.dll and libEGL.dll, beside an Electron or
//     a Chromium application, or a port from mobile (hooks_egl.cpp).
// None of these is usually loaded yet when the library goes in, so it asks the loader to say when any
// library loads (LdrRegisterDllNotification) and hooks each the moment it does: every exported entry
// point the library records is patched in place (MinHook), so a call through the import table,
// through GetProcAddress or through a pointer fetched long ago all arrive at the hook. Entry points a
// library does not export are caught where they are handed out: eglGetProcAddress, wglGetProcAddress.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "platform_win32.h"

#include "egl_api.h"
#include "state.h"

#include <gpu_inspector/sdk/config.h>

#include <MinHook.h>

#include <cwchar>
#include <mutex>
#include <string>

namespace glesinsp {
namespace {

struct UnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct DllNotificationData {
    ULONG Flags;
    const UnicodeString* FullDllName;
    const UnicodeString* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};

constexpr ULONG kDllLoaded = 1;

using PFN_DllNotification = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID context);
using PFN_LdrRegisterDllNotification = LONG(NTAPI*)(ULONG flags, PFN_DllNotification callback, PVOID context, PVOID* cookie);

std::recursive_mutex g_hookMutex;
bool g_glHooked = false;
bool g_eglHooked = false;
bool g_wglHooked = false;
bool g_gdiHooked = false;
bool g_initialized = false;

bool NameIs(const UnicodeString* s, const wchar_t* want) {
    if (!s || !s->Buffer) return false;
    const size_t n = s->Length / sizeof(wchar_t);
    return n == wcslen(want) && _wcsnicmp(s->Buffer, want, n) == 0;
}

}  // namespace

bool HookExport(HMODULE module, const char* name, void* hook, void** real) {
    void* target = (void*)GetProcAddress(module, name);
    if (!target) return false;
    if (*real) return true;   // already hooked, through another library that exports the same function
    const MH_STATUS s = MH_CreateHook(target, hook, real);
    if (s != MH_OK) {
        LogAlways("%s could not be hooked (MinHook %d)", name, (int)s);
        *real = target;   // the application's calls go straight through; ours still work
        return false;
    }
    return true;
}

void EnableHooks() {
    MH_EnableHook(MH_ALL_HOOKS);
}

void HookGlModule(HMODULE module) {
    std::lock_guard lock(g_hookMutex);
    size_t hooked = 0;
    for (size_t i = 0; i < kHookCount; ++i) {
        if (HookExport(module, kHooks[i].name, kHooks[i].hook, kHooks[i].real)) ++hooked;
    }
    // The ones not hooked (glGet* and the other queries) are called directly.
    void** slots = reinterpret_cast<void**>(&g_gl);
    for (size_t i = 0; i < kCommandCount; ++i) {
        if (!slots[i]) slots[i] = (void*)GetProcAddress(module, kCommandNames[i]);
    }
    MH_EnableHook(MH_ALL_HOOKS);
    LogAlways("hooked %zu GL entry points", hooked);
}

namespace {

void HookEgl(HMODULE module) {
    size_t hooked = 0;
    for (size_t i = 0; i < kEglHookCount; ++i) {
        if (HookExport(module, kEglHooks[i].name, kEglHooks[i].hook, kEglHooks[i].real)) ++hooked;
    }
    for (size_t i = 0; i < kEglImportCount; ++i) {
        if (!*kEglImports[i].slot) *kEglImports[i].slot = (void*)GetProcAddress(module, kEglImports[i].name);
    }
    MH_EnableHook(MH_ALL_HOOKS);
    LogAlways("hooked %zu EGL entry points", hooked);
}

/**
 * A library just loaded, or found loaded. opengl32.dll's GL exports are left alone here: the process
 * may never make an OpenGL ES context, and they are hooked when it does (hooks_wgl.cpp), so a desktop
 * OpenGL application, or ANGLE's own libGLESv2, keeps them.
 */
void Consider(HMODULE module, const UnicodeString* name, const wchar_t* fallback) {
    std::lock_guard lock(g_hookMutex);
    auto is = [&](const wchar_t* want) { return name ? NameIs(name, want) : _wcsicmp(fallback, want) == 0; };
    if (is(L"libGLESv2.dll") && !g_glHooked) {
        g_glHooked = true;
        HookGlModule(module);
    } else if (is(L"libEGL.dll") && !g_eglHooked) {
        g_eglHooked = true;
        HookEgl(module);
    } else if (is(L"opengl32.dll") && !g_wglHooked) {
        g_wglHooked = true;
        HookWgl(module);
    } else if (is(L"gdi32.dll") && !g_gdiHooked) {
        g_gdiHooked = true;
        HookGdiSwap(module);
    }
}

VOID CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, PVOID) {
    if (reason != kDllLoaded || !data) return;
    Consider((HMODULE)data->DllBase, data->BaseDllName, nullptr);
}

}  // namespace
}  // namespace glesinsp

using namespace glesinsp;

extern "C" __declspec(dllexport) DWORD WINAPI GpuInspectorInitialize(LPVOID settings) {
    if (g_initialized) return 0;
    g_initialized = true;
    const std::string applied = gpuinsp::sdk::Config::Get().ApplySettingsBlock((const wchar_t*)settings);
    LogAlways("loaded into pid %lu", GetCurrentProcessId());
    if (!applied.empty()) LogAlways("settings from the launcher: %s", applied.c_str());
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LogAlways("MinHook could not start (%d): no OpenGL ES capture in this process", (int)status);
        return 1;
    }
    // Libraries loaded later are hooked as they load; ones already there, now.
    auto reg = (PFN_LdrRegisterDllNotification)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie = nullptr;
    if (!reg || reg(0, &OnDllNotification, nullptr, &cookie) != 0) LogAlways("LdrRegisterDllNotification is not available: only libraries already loaded are hooked");
    for (const wchar_t* name : {L"libGLESv2.dll", L"libEGL.dll", L"opengl32.dll", L"gdi32.dll"}) {
        if (HMODULE m = GetModuleHandleW(name)) Consider(m, nullptr, name);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
