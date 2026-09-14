// dxinsp_launch: gets the D3D12 capture library into an application, either by starting the
// application itself or by watching for one to start.
//
//   dxinsp_launch.exe --dll <path to dxinsp_capture.dll> [--cwd <dir>] -- <exe> [args...]
//   dxinsp_launch.exe --watch <image name or full path> --dll <path to dxinsp_capture.dll>
//                     [--env NAME=VALUE]... [--timeout <seconds>] [--poll <ms>] [--once]
//
// **Launch.** The target is created suspended, the library is loaded into it with a remote
// LoadLibraryW thread, its DxinspInitialize export runs in a second remote thread (which installs
// the entry point hooks before the application's first instruction), and the main thread is
// resumed. The launcher inherits its standard handles and environment to the target, waits for it
// and exits with its exit code, so whoever started the launcher (the inspector, over pipes) sees
// the target as one process. A target the library cannot be injected into is still started, with
// the reason on stderr: a Vulkan application launched this way with the Vulkan layer in its
// environment must keep working.
//
// **Watch.** The application is started by something else (an editor, a launcher, Steam), which is
// the D3D12 counterpart of the Vulkan implicit layer: there is no loader to insert us, so instead
// the process list is polled every couple of milliseconds for a matching image name and the library
// goes into the first process that appears, injected as above except that the process is running:
// it is held still meanwhile (Freezer), or the application would reach D3D12CreateDevice first.
// That races the application's start, so it only works when the watch is running before the
// application is launched; a process that already has an ID3D12Device cannot be helped, since the
// hooks are on the entry points that make one. The age of the process when the library went in is
// reported for that reason.
//
// The watched process was not started by us, so its environment is the user's rather than the
// inspector's: --env passes the library's settings (DXINSP_PORT and the rest) to DxinspInitialize,
// which applies them before it reads any of them.
//
// Exit codes (watch mode): 0 injected (and, with --once, the watched application exited with 0),
// 3 the timeout passed with nothing injected, 2 a usage error, 1 nothing to inject with. With
// --once the launcher waits for the application it injected into and exits with its exit code,
// as the launch path does.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <set>
#include <string>
#include <vector>

namespace {

/** Watch mode's exit codes; a launch exits with the target's own. */
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitTimedOut = 3;

/** How long a matching process may have been running before its device is probably already made. */
constexpr unsigned long long kLateMs = 1000;

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

/**
 * A module of another process by name, null when it has none. The snapshot is retried: taking one
 * of a process whose module list is being written fails with ERROR_BAD_LENGTH, which is exactly
 * what a process that has just started (or has just been given a library) is doing — and a watched
 * process is caught milliseconds after its first instruction.
 */
HMODULE RemoteModuleByName(DWORD pid, const std::wstring& name, int tries) {
    for (int attempt = 0; attempt < tries; ++attempt) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            HMODULE found = nullptr;
            if (Module32FirstW(snap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, name.c_str()) == 0) { found = me.hModule; break; }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
            if (found) return found;
        }
        if (attempt + 1 < tries) Sleep(5);
    }
    return nullptr;
}

HMODULE RemoteModule(DWORD pid, const std::wstring& dllPath) {
    return RemoteModuleByName(pid, dllPath.substr(dllPath.find_last_of(L"\\/") + 1), 40);
}

/**
 * Holds a watched application still while the library goes into it. A launch injects into a process
 * created suspended, which cannot race us; a watched process is already running, and
 * dxinsp_triangle has its device some fifteen milliseconds after its first instruction, long before
 * a remote LoadLibraryW of a two megabyte library returns. So the process is stopped the moment it
 * is seen and let go once the hooks are in. What decides the race is not how long the injection
 * takes — the application is frozen throughout — but how much running time the application gets
 * anyway, which is the milliseconds before it was stopped plus the bursts below.
 *
 * Stopping is NtSuspendProcess: one call, every thread, nothing to enumerate first (a thread
 * snapshot is system wide and costs milliseconds the application would spend running). Once the
 * thread doing the injection exists it cannot stay suspended with the rest, so the hold is handed
 * over to per-thread suspensions of every thread but that one, and the process-wide suspension is
 * released.
 *
 * In bursts, because a process this young is mostly loading libraries: the remote LoadLibraryW then
 * waits for a loader lock the frozen application holds and nothing would ever release. Whenever the
 * remote thread has made no progress for kPulseWaitMs the application is let go for a burst and
 * stopped again. Bursts start at kFirstBurstUs and double, so the application is given the least
 * running time that gets the lock released rather than a fixed slice of it.
 */
constexpr DWORD kPulseWaitMs = 5;
constexpr unsigned kFirstBurstUs = 200;
constexpr unsigned kMaxBurstUs = 4000;
constexpr DWORD kRemoteThreadTimeoutMs = 30000;

using NtProcessFn = LONG(NTAPI*)(HANDLE);

NtProcessFn NtProcess(const char* name) {
    static HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll ? (NtProcessFn)GetProcAddress(ntdll, name) : nullptr;
}

/** Busy-waits, since Sleep cannot do fractions of a millisecond and a burst is measured in microseconds. */
void SpinUs(unsigned us) {
    LARGE_INTEGER frequency{}, start{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const long long ticks = (long long)((double)frequency.QuadPart * us / 1e6);
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < ticks);
}

class Freezer {
public:
    Freezer(HANDLE process, DWORD pid) : _process(process), _pid(pid) {}
    ~Freezer() { Release(); }
    Freezer(const Freezer&) = delete;
    Freezer& operator=(const Freezer&) = delete;

    /** Stops the whole process at once. False when it could not be (then the injection races it). */
    bool Stop() {
        NtProcessFn suspend = NtProcess("NtSuspendProcess");
        _stopped = suspend && suspend(_process) >= 0;
        return _stopped;
    }

    /**
     * Hands the hold over to per-thread suspensions of every thread but `exceptTid`, the one
     * running the injection, and lets the process-wide suspension go. Called with the process
     * already stopped, so enumerating its threads costs the application nothing.
     */
    void HoldAllBut(DWORD exceptTid) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != _pid || te.th32ThreadID == exceptTid) continue;
                    if (_ids.count(te.th32ThreadID)) continue;
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (!thread) continue;
                    if (SuspendThread(thread) != (DWORD)-1) {
                        _ids.insert(te.th32ThreadID);
                        _threads.push_back(thread);
                    } else {
                        CloseHandle(thread);
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        // The process-wide suspension would hold the injecting thread too, so it goes now that the
        // application's own threads are held one by one.
        if (_stopped) {
            if (NtProcessFn resume = NtProcess("NtResumeProcess")) resume(_process);
            _stopped = false;
        }
    }

    /** Lets the application run for `us` microseconds and holds it again (the loader lock). */
    void Burst(unsigned us) {
        ++_bursts;
        for (HANDLE thread : _threads) ResumeThread(thread);
        SpinUs(us);
        for (HANDLE thread : _threads) SuspendThread(thread);
    }

    /** Lets the application go, however it is being held. */
    void Release() {
        for (HANDLE thread : _threads) {
            ResumeThread(thread);
            CloseHandle(thread);
        }
        _threads.clear();
        _ids.clear();
        if (_stopped) {
            if (NtProcessFn resume = NtProcess("NtResumeProcess")) resume(_process);
            _stopped = false;
        }
    }

    bool holding() const { return _stopped || !_threads.empty(); }
    /** How many bursts the application needed to release the loader lock. */
    int bursts() const { return _bursts; }

private:
    HANDLE _process;
    DWORD _pid;
    bool _stopped = false;
    std::vector<HANDLE> _threads;
    std::set<DWORD> _ids;
    int _bursts = 0;
};

/**
 * Runs `entry` in the target and waits for it. With a `freezer`, the application is held around the
 * remote thread and let go in bursts while it blocks (above).
 */
bool RunRemote(HANDLE process, LPTHREAD_START_ROUTINE entry, LPVOID arg, DWORD* exitCode, std::wstring& why, Freezer* freezer) {
    DWORD tid = 0;
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, entry, arg, 0, &tid);
    if (!thread) { why = L"CreateRemoteThread failed (" + std::to_wstring(GetLastError()) + L")"; return false; }
    // The thread just made is one of the target's own: the hold has to leave it out.
    if (freezer) freezer->HoldAllBut(tid);
    const ULONGLONG deadline = GetTickCount64() + kRemoteThreadTimeoutMs;
    unsigned burstUs = kFirstBurstUs;
    for (;;) {
        DWORD wait = WaitForSingleObject(thread, freezer ? kPulseWaitMs : kRemoteThreadTimeoutMs);
        if (wait == WAIT_OBJECT_0) break;
        if (!freezer || GetTickCount64() >= deadline) {
            CloseHandle(thread);
            why = L"the remote thread did not finish";
            return false;
        }
        freezer->Burst(burstUs);
        burstUs = burstUs < kMaxBurstUs ? burstUs * 2 : kMaxBurstUs;
    }
    if (exitCode) GetExitCodeThread(thread, exitCode);
    CloseHandle(thread);
    return true;
}

/** Writes `bytes` of `data` into the target; null (with `why` set) when it could not be written. */
LPVOID WriteRemote(HANDLE process, const void* data, size_t bytes, std::wstring& why) {
    LPVOID remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { why = L"VirtualAllocEx failed (" + std::to_wstring(GetLastError()) + L")"; return nullptr; }
    if (!WriteProcessMemory(process, remote, data, bytes, nullptr)) {
        why = L"WriteProcessMemory failed (" + std::to_wstring(GetLastError()) + L")";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return nullptr;
    }
    return remote;
}

/**
 * The library's settings for a process we did not start, as the environment block DxinspInitialize
 * takes: NAME=VALUE entries, each terminated, the whole terminated again. Empty when there are
 * none, and then nothing is passed and the target's own environment is what the library reads.
 */
std::vector<wchar_t> SettingsBlock(const std::vector<std::wstring>& entries) {
    std::vector<wchar_t> block;
    if (entries.empty()) return block;
    for (const std::wstring& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(0);
    }
    block.push_back(0);
    return block;
}

/**
 * The offset of DxinspInitialize inside the library, from our own copy of it; the target's base
 * comes from its module list (a remote thread's exit code holds only 32 bits of the HMODULE).
 * SIZE_MAX with `why` set when the library cannot be read. Read once, since a watch injects into
 * one process after another and loading a copy of the library takes milliseconds the race for a
 * device does not have.
 */
uintptr_t InitOffset(const std::wstring& dllPath, std::wstring& why) {
    HMODULE local = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) { why = L"cannot load " + dllPath + L" (" + std::to_wstring(GetLastError()) + L")"; return SIZE_MAX; }
    FARPROC init = GetProcAddress(local, "DxinspInitialize");
    if (!init) { FreeLibrary(local); why = L"the library has no DxinspInitialize export"; return SIZE_MAX; }
    uintptr_t offset = (uintptr_t)init - (uintptr_t)local;
    FreeLibrary(local);
    return offset;
}

/**
 * Loads the library into the process and runs its initializer there. `settings` is the environment
 * block above, given to DxinspInitialize; empty for a launch, whose target inherited our
 * environment already.
 */
bool Inject(HANDLE process, DWORD pid, const std::wstring& dllPath, uintptr_t offset,
            const std::vector<wchar_t>& settings, Freezer* freezer, std::wstring& why) {
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = WriteRemote(process, dllPath.c_str(), bytes, why);
    if (!remotePath) return false;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel, "LoadLibraryW");
    DWORD code = 0;
    if (!RunRemote(process, loadLibrary, remotePath, &code, why, freezer)) return false;
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    HMODULE remote = RemoteModule(pid, dllPath);
    if (!remote) { why = L"LoadLibraryW in the target failed (is the library's directory readable, and are its dependencies present?)"; return false; }
    LPVOID remoteSettings = nullptr;
    if (!settings.empty()) {
        remoteSettings = WriteRemote(process, settings.data(), settings.size() * sizeof(wchar_t), why);
        if (!remoteSettings) return false;
    }
    bool ok = RunRemote(process, (LPTHREAD_START_ROUTINE)((uintptr_t)remote + offset), remoteSettings, &code, why, freezer);
    // The initializer copies what it needs out of the block before it returns.
    if (remoteSettings) VirtualFreeEx(process, remoteSettings, 0, MEM_RELEASE);
    if (!ok) return false;
    if (code != 0) { why = L"DxinspInitialize returned " + std::to_wstring(code); return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Watch mode

struct WatchOptions {
    /** What the user named: an image name ("TestVulkan.exe") or a full path to one. */
    std::wstring wanted;
    /** The image name alone, which is what the process list holds. */
    std::wstring image;
    /** `wanted` is a path: the process's own image path has to match it too. */
    bool fullPath = false;
    std::wstring dll;
    std::vector<std::wstring> env;
    int timeoutSeconds = 0;
    int pollMs = 2;
    bool once = false;
};

/** How long the process had been running when we caught it, in milliseconds; 0 when unknown. */
unsigned long long ProcessAgeMs(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = created.dwLowDateTime;
    a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return 0;
    return (b.QuadPart - a.QuadPart) / 10000;
}

/** A path spelled the way the system spells one, so a --watch given with forward slashes matches. */
std::wstring Normalized(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

/** The process's executable path, empty when it cannot be read. */
std::wstring ImagePath(HANDLE process) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    if (!QueryFullProcessImageNameW(process, 0, buf, &n)) return std::wstring();
    return std::wstring(buf, n);
}

/**
 * Injects into one matching process, holding it still meanwhile. `keep` is left holding the process
 * when the injection worked and the caller wants to wait for it (--once); otherwise the handle is
 * closed here.
 */
bool InjectRunning(const WatchOptions& o, uintptr_t offset, DWORD pid, const std::wstring& image,
                   const std::vector<wchar_t>& settings, HANDLE* keep) {
    const DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                         PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_SUSPEND_RESUME | SYNCHRONIZE;
    HANDLE process = OpenProcess(rights, FALSE, pid);
    if (!process) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) {
            Note(L"%s (pid %lu): access denied; the inspector has to run elevated to inject into a process "
                 L"running elevated or as another user", image.c_str(), pid);
        } else {
            Note(L"%s (pid %lu): cannot open the process (error %lu)", image.c_str(), pid, e);
        }
        return false;
    }
    // Stopped first and asked questions after: every millisecond between the process appearing and
    // being stopped is a millisecond it spends on its way to D3D12CreateDevice.
    Freezer freezer(process, pid);
    const bool held = freezer.Stop();
    const unsigned long long age = ProcessAgeMs(process);
    // A 32-bit process on x64 Windows runs under WOW64; only x64 targets are injected, as in a
    // launch, and for the same reason.
    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64) && wow64) {
        Note(L"%s (pid %lu): not an x64 process (only x64 targets are injected); it runs without the D3D12 capture library", image.c_str(), pid);
        freezer.Release();
        CloseHandle(process);
        return false;
    }
    const ULONGLONG begun = GetTickCount64();
    std::wstring why;
    const bool ok = Inject(process, pid, o.dll, offset, settings, &freezer, why);
    const int bursts = freezer.bursts();
    freezer.Release();
    if (!ok) {
        Note(L"%s (pid %lu): injection failed: %s", image.c_str(), pid, why.c_str());
        CloseHandle(process);
        return false;
    }
    Note(L"injected %s into pid %lu (%s), %llu ms after it started, in %llu ms (%s)", o.dll.c_str(), pid, image.c_str(), age,
         GetTickCount64() - begun,
         !held ? L"the application could not be held meanwhile, so this raced it"
               : (L"held meanwhile, let go " + std::to_wstring(bursts) + L" times for its own libraries").c_str());
    if (age > kLateMs) {
        // The hooks are on the entry points that make a device, so a device made before we got in
        // is invisible: say so rather than let the session wait for a connection that never comes.
        Note(L"warning: %s had already been running for %llu ms when the library went in; if it had "
             L"created its D3D12 device by then the hooks came too late and nothing will be captured. "
             L"Start the application after the watch begins.", image.c_str(), age);
    }
    if (keep) *keep = process; else CloseHandle(process);
    return true;
}

/**
 * A millisecond timer for the duration of a watch. Without it Sleep(5) sleeps for a scheduler tick
 * (about 16 ms), which is most of the time a fast application takes to reach its device.
 */
class FineTimer {
public:
    FineTimer() : _ok(timeBeginPeriod(1) == TIMERR_NOERROR) {}
    ~FineTimer() { if (_ok) timeEndPeriod(1); }
    FineTimer(const FineTimer&) = delete;
    FineTimer& operator=(const FineTimer&) = delete;

private:
    bool _ok;
};

/** Polls the process list and injects into each new match; see the exit codes at the top. */
int Watch(const WatchOptions& o) {
    FineTimer timer;
    if (GetFileAttributesW(o.dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Note(L"the capture library is not at %s", o.dll.c_str());
        return kExitFailed;
    }
    std::wstring why;
    const uintptr_t offset = InitOffset(o.dll, why);
    if (offset == SIZE_MAX) {
        Note(L"%s", why.c_str());
        return kExitFailed;
    }
    const std::vector<wchar_t> settings = SettingsBlock(o.env);
    const DWORD self = GetCurrentProcessId();
    // Processes already tried, so each is reported once however long the watch runs. A pid is
    // reused only after the process is gone, by which time a new match deserves a new attempt.
    std::vector<DWORD> tried;
    HANDLE injected = nullptr;
    DWORD injectedPid = 0;
    Note(L"watching for %s every %d ms%s; start the application now", o.wanted.c_str(), o.pollMs,
         o.timeoutSeconds > 0 ? (L", for " + std::to_wstring(o.timeoutSeconds) + L" seconds").c_str() : L"");
    const ULONGLONG started = GetTickCount64();
    // Every pid looked at, matching or not: a pid names one process for its lifetime, so a process
    // is examined (and reported) once however long the watch runs.
    std::set<DWORD> known;
    std::vector<DWORD> pids(2048);
    for (;;) {
        DWORD bytes = 0;
        if (EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &bytes)) {
            const size_t count = bytes / sizeof(DWORD);
            for (size_t i = 0; i < count; ++i) {
                const DWORD pid = pids[i];
                if (pid == self || pid == 0) continue;
                if (!known.insert(pid).second) continue;
                // Only a process never seen before is asked for its name, which is what keeps a
                // poll to a fraction of a millisecond (a toolhelp process snapshot costs several).
                HANDLE query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!query) continue;   // a system process, or one of another user: not ours to inject into
                const std::wstring path = ImagePath(query);
                CloseHandle(query);
                if (path.empty()) continue;
                const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
                if (_wcsicmp(name.c_str(), o.image.c_str()) != 0) continue;
                if (o.fullPath && _wcsicmp(Normalized(path).c_str(), o.wanted.c_str()) != 0) continue;
                if (InjectRunning(o, offset, pid, name, settings, o.once ? &injected : nullptr)) {
                    injectedPid = pid;
                    if (o.once) break;
                }
            }
        }
        if (injectedPid && o.once) break;
        if (o.timeoutSeconds > 0 && (GetTickCount64() - started) >= (ULONGLONG)o.timeoutSeconds * 1000) {
            if (injectedPid) {
                Note(L"the %d second watch for %s is over; it injected into pid %lu", o.timeoutSeconds, o.wanted.c_str(), injectedPid);
                return 0;
            }
            Note(L"no %s started within %d seconds; nothing was injected", o.wanted.c_str(), o.timeoutSeconds);
            return kExitTimedOut;
        }
        Sleep((DWORD)o.pollMs);
    }
    // --once: the watch is over, and from here the launcher stands in for the application the way
    // it does for one it started, so the session sees it exit when the application does.
    DWORD code = 0;
    if (injected) {
        WaitForSingleObject(injected, INFINITE);
        GetExitCodeProcess(injected, &code);
        CloseHandle(injected);
        Note(L"pid %lu exited with code %lu", injectedPid, code);
    }
    return (int)code;
}

int Usage() {
    fwprintf(stderr, L"usage: dxinsp_launch.exe --dll <dxinsp_capture.dll> [--cwd <dir>] -- <exe> [args...]\n");
    fwprintf(stderr, L"       dxinsp_launch.exe --watch <image name or full path> --dll <dxinsp_capture.dll>\n");
    fwprintf(stderr, L"                         [--env NAME=VALUE]... [--timeout <seconds>] [--poll <ms>] [--once]\n");
    return kExitUsage;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring dll;
    std::wstring cwd;
    WatchOptions watch;
    bool watching = false;
    int i = 1;
    for (; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
        else if (a == L"--cwd" && i + 1 < argc) cwd = argv[++i];
        else if (a == L"--watch" && i + 1 < argc) { watching = true; watch.wanted = argv[++i]; }
        else if (a == L"--env" && i + 1 < argc) watch.env.push_back(argv[++i]);
        else if (a == L"--timeout" && i + 1 < argc) watch.timeoutSeconds = _wtoi(argv[++i]);
        else if (a == L"--poll" && i + 1 < argc) watch.pollMs = _wtoi(argv[++i]);
        else if (a == L"--once") watch.once = true;
        else if (a == L"--") { ++i; break; }
        // An option of ours with its value missing, or one we do not know: saying so beats
        // starting a program named "--watch".
        else if (a.rfind(L"--", 0) == 0) { Note(L"unknown or incomplete option %s", a.c_str()); return Usage(); }
        else break;
    }

    if (watching) {
        if (i < argc) {
            Note(L"--watch takes no command line: the application is started by something else");
            return Usage();
        }
        if (watch.wanted.empty()) return Usage();
        if (dll.empty()) {
            Note(L"--watch needs --dll: there is nothing to inject");
            return kExitFailed;
        }
        watch.dll = dll;
        size_t slash = watch.wanted.find_last_of(L"\\/");
        watch.fullPath = slash != std::wstring::npos;
        watch.image = watch.fullPath ? watch.wanted.substr(slash + 1) : watch.wanted;
        // A path the user typed may hold forward slashes; the one a process reports never does.
        if (watch.fullPath) watch.wanted = Normalized(watch.wanted);
        if (watch.image.empty()) return Usage();
        if (watch.pollMs < 1) watch.pollMs = 1;
        if (watch.pollMs > 1000) watch.pollMs = 1000;
        return Watch(watch);
    }

    if (i >= argc) return Usage();
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
    uintptr_t offset = dll.empty() ? SIZE_MAX : InitOffset(dll, why);
    if (dll.empty()) {
        Note(L"no --dll given: the target runs without the D3D12 capture library");
    } else if (offset == SIZE_MAX) {
        Note(L"%s; the target runs without the D3D12 capture library", why.c_str());
    } else if (!IsX64Image(exe, why)) {
        Note(L"%s: %s; the target runs without the D3D12 capture library", exe.c_str(), why.c_str());
    } else if (!Inject(pi.hProcess, pi.dwProcessId, dll, offset, std::vector<wchar_t>(), nullptr, why)) {
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
