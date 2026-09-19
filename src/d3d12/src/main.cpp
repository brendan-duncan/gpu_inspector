// Where the library gets into the process.
//
// dxinsp_launch.exe creates the target suspended, loads this DLL into it with a remote LoadLibraryW
// thread, runs DxinspInitialize in a second remote thread, and only then resumes the main thread.
// Watching for an application the launcher did not start (dxinsp_launch.exe --watch) is the same,
// except that the process is already running and its environment is the user's rather than the
// inspector's: the launcher then passes DxinspInitialize an environment block with the library's
// settings, which are applied here before anything reads one.
// DllMain does nothing but opt out of thread notifications: hooking under the loader lock is what
// the second thread avoids.
#include "common.h"
#include "hooks.h"
#include "ui_messages.h"

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

bool g_initialized = false;

/**
 * The launcher's settings: NAME=VALUE entries, each terminated by a null, the block terminated by
 * another, exactly as a Win32 environment block is written. Every DXINSP_* setting then reads as if
 * the application had been started with it (common.h, SetConfigValue). They are kept in the library
 * rather than written into the process environment: a watched process is caught a millisecond or
 * two after its first instruction, and its loader may still be building that environment.
 *
 * Nothing is logged here: the log reads its own settings on its first line, so the whole block has
 * to be in place before that (the names applied are returned for the caller to log).
 */
std::string ApplySettings(const wchar_t* block) {
    std::string applied;
    if (!block) return applied;
    // A malformed block would run off the end of the allocation: 64 entries and 32 KB are far more
    // than the handful of variables the library reads.
    constexpr size_t kMaxEntries = 64;
    constexpr size_t kMaxChars = 16 * 1024;
    size_t chars = 0;
    for (size_t n = 0; n < kMaxEntries && *block; ++n) {
        size_t length = wcsnlen(block, kMaxChars - chars);
        if (length == 0 || length >= kMaxChars - chars) break;
        const wchar_t* equals = wcschr(block, L'=');
        if (equals && equals != block) {
            dxinsp::SetConfigValue(dxinsp::Narrow(block, equals - block).c_str(), dxinsp::Narrow(equals + 1).c_str());
            if (!applied.empty()) applied += " ";
            applied += dxinsp::Narrow(block, length);
        }
        block += length + 1;
        chars += length + 1;
    }
    return applied;
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI DxinspInitialize(LPVOID settings) {
    if (g_initialized) return 0;
    g_initialized = true;
    // Before the first log line: the log's own variables are read once, on first use.
    const std::string applied = ApplySettings((const wchar_t*)settings);
    // The command line as well as the pid: one target can be a tree of processes (a browser's
    // renderers and its GPU process), and the log is otherwise a column of numbers.
    dxinsp::LogAlways("loaded into pid %lu: %s", GetCurrentProcessId(), dxinsp::Narrow(GetCommandLineW()).c_str());
    if (!applied.empty()) dxinsp::LogAlways("settings from the launcher: %s", applied.c_str());
    // The listener comes up before the application has a device, so the UI can be waiting when the
    // process starts, or attach later and get a snapshot either way. It starts listening only once
    // a D3D12 device exists (ui_messages.cpp), so a Vulkan application launched the same way, with
    // this library along for the ride, leaves the port to the Vulkan layer. That is also what tells
    // a waiting inspector whether the injection was in time: a process whose device was already
    // made never opens the port, since the hooks are on the calls that make one.
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
