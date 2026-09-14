// dxinsp_launch: starts an application with the D3D12 capture library injected.
//
//   dxinsp_launch.exe --dll <path to dxinsp_capture.dll> [--cwd <dir>] -- <exe> [args...]
//
// The target is created suspended, the library is loaded into it with a remote LoadLibraryW
// thread, its DxinspInitialize export runs in a second remote thread (which installs the entry
// point hooks before the application's first instruction), and the main thread is resumed. The
// launcher inherits its standard handles and environment to the target, waits for it and exits
// with its exit code, so whoever started the launcher (the inspector, over pipes) sees the target
// as one process. A target the library cannot be injected into is still started, with the reason
// on stderr: a Vulkan application launched this way with the Vulkan layer in its environment must
// keep working.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

void Note(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fwprintf(stderr, L"dxinsp: ");
    vfwprintf(stderr, fmt, ap);
    fwprintf(stderr, L"\n");
    va_end(ap);
    fflush(stderr);
}

// One argument quoted the way the CRT's parser (and CommandLineToArgvW) reads it back.
std::wstring Quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += c;
        } else {
            out.append(backslashes, L'\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

bool IsX64Image(const std::wstring& exe, std::wstring& why) {
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { why = L"cannot open the executable"; return false; }
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    bool ok = ReadFile(f, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    if (ok) ok = SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
    if (ok) ok = ReadFile(f, &sig, sizeof(sig), &read, nullptr) && sig == IMAGE_NT_SIGNATURE;
    if (ok) ok = ReadFile(f, &fh, sizeof(fh), &read, nullptr) && read == sizeof(fh);
    CloseHandle(f);
    if (!ok) { why = L"not a Windows executable"; return false; }
    if (fh.Machine != IMAGE_FILE_MACHINE_AMD64) { why = L"not an x64 executable (only x64 targets are injected)"; return false; }
    return true;
}

HMODULE RemoteModule(DWORD pid, const std::wstring& dllPath) {
    std::wstring name = dllPath.substr(dllPath.find_last_of(L"\\/") + 1);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    HMODULE found = nullptr;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name.c_str()) == 0) { found = me.hModule; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

bool RunRemote(HANDLE process, LPTHREAD_START_ROUTINE entry, LPVOID arg, DWORD* exitCode, std::wstring& why) {
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, entry, arg, 0, nullptr);
    if (!thread) { why = L"CreateRemoteThread failed (" + std::to_wstring(GetLastError()) + L")"; return false; }
    DWORD wait = WaitForSingleObject(thread, 30000);
    if (wait != WAIT_OBJECT_0) { CloseHandle(thread); why = L"the remote thread did not finish"; return false; }
    if (exitCode) GetExitCodeThread(thread, exitCode);
    CloseHandle(thread);
    return true;
}

bool Inject(HANDLE process, DWORD pid, const std::wstring& dllPath, std::wstring& why) {
    // Our own copy of the library gives the offset of the export; the target's base comes from
    // its module list (a remote thread's exit code holds only 32 bits of the HMODULE).
    HMODULE local = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) { why = L"cannot load " + dllPath + L" (" + std::to_wstring(GetLastError()) + L")"; return false; }
    FARPROC init = GetProcAddress(local, "DxinspInitialize");
    if (!init) { FreeLibrary(local); why = L"the library has no DxinspInitialize export"; return false; }
    uintptr_t offset = (uintptr_t)init - (uintptr_t)local;
    FreeLibrary(local);

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) { why = L"VirtualAllocEx failed"; return false; }
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, nullptr)) { why = L"WriteProcessMemory failed"; return false; }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel, "LoadLibraryW");
    DWORD code = 0;
    if (!RunRemote(process, loadLibrary, remotePath, &code, why)) return false;
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    HMODULE remote = RemoteModule(pid, dllPath);
    if (!remote) { why = L"LoadLibraryW in the target failed (is the library's directory readable, and are its dependencies present?)"; return false; }
    if (!RunRemote(process, (LPTHREAD_START_ROUTINE)((uintptr_t)remote + offset), nullptr, &code, why)) return false;
    if (code != 0) { why = L"DxinspInitialize returned " + std::to_wstring(code); return false; }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring dll;
    std::wstring cwd;
    int i = 1;
    for (; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
        else if (a == L"--cwd" && i + 1 < argc) cwd = argv[++i];
        else if (a == L"--") { ++i; break; }
        else break;
    }
    if (i >= argc) {
        fwprintf(stderr, L"usage: dxinsp_launch.exe --dll <dxinsp_capture.dll> [--cwd <dir>] -- <exe> [args...]\n");
        return 2;
    }
    std::wstring exe = argv[i];
    std::wstring commandLine = Quote(exe);
    for (int k = i + 1; k < argc; ++k) {
        commandLine += L' ';
        commandLine += Quote(argv[k]);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(0);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        Note(L"cannot start %s (error %lu)", exe.c_str(), GetLastError());
        return 1;
    }

    std::wstring why;
    if (dll.empty()) {
        Note(L"no --dll given: the target runs without the D3D12 capture library");
    } else if (!IsX64Image(exe, why)) {
        Note(L"%s: %s; the target runs without the D3D12 capture library", exe.c_str(), why.c_str());
    } else if (!Inject(pi.hProcess, pi.dwProcessId, dll, why)) {
        Note(L"injection failed: %s; the target runs without the D3D12 capture library", why.c_str());
    } else {
        Note(L"injected %s into pid %lu", dll.c_str(), pi.dwProcessId);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
