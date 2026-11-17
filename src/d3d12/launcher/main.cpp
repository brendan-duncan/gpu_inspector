// dxinsp_launch: gets the D3D12 capture library into an application, either by starting the
// application itself or by watching for one to start.
//
//   dxinsp_launch.exe --dll <path to dxinsp_capture.dll> [--cwd <dir>] [--follow <text>]...
//                     [--follow-children] -- <exe> [args...]
//   dxinsp_launch.exe --watch <image name or full path> --dll <path to dxinsp_capture.dll>
//                     [--env NAME=VALUE]... [--timeout <seconds>] [--poll <ms>] [--once]
//                     [--follow <text>]... [--follow-children]
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
// **Follow.** The application renders in a process it starts itself: Chrome's GPU process, where
// Dawn's WebGPU work lands, is a child of the browser the user launched. --follow <text> injects
// into every process the target spawns whose command line contains <text>, caught and frozen as it
// appears the way a watch catches one (so `--follow --type=gpu-process` leaves a browser's
// renderers and utility processes alone). It can be given more than once, and `--follow !<text>`
// excludes instead: a child whose command line holds an excluded text is left alone whatever else
// it matches. --follow-children takes every child instead of the ones a text names, which is what
// the inspector's "Capture child processes" asks for: it is for an application whose renderer is
// some process it starts that nobody can name in advance, such as a game behind its own launcher.
// Exclusions still apply, so --follow-children --follow !--type=renderer is "everything but those".
//
// Following is by descent: a child counts when one of its ancestors is the target. That is the
// shape of the problem for a launcher that starts the game itself.
//
// **Package.** An <exe> inside an installed package (MSIX: the Microsoft Store, the Xbox app's
// games under C:\XboxGames) cannot be started with CreateProcess -- it would run without its
// package identity -- and the process the app model starts for us is not our child. So the
// launcher registers itself as the package's debugger (IPackageDebugSettings, as Visual Studio and
// PIX do) and activates the application; the app model creates the process suspended and runs
// `dxinsp_launch --package-debugger ... -p <pid> -tid <tid>`, which injects as a launch does and
// resumes it (LaunchPackage, PackageDebugger). --no-package starts such an <exe> directly instead.
//
// This is how PIX and RenderDoc get into Chrome; Chromium's
// own --gpu-launcher hook, which would start the GPU process through this launcher, is not a
// working configuration on current Chrome -- the GPU process exits within a fraction of a second
// however it is wrapped, and the browser respawns it in a loop.
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
#include <appmodel.h>
#include <objbase.h>
#include <psapi.h>
#include <shobjidl.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

/** Watch mode's exit codes; a launch exits with the target's own. */
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitTimedOut = 3;

/** How long a matching process may have been running before its device is probably already made. */
constexpr unsigned long long kLateMs = 1000;

/**
 * Package debugger mode (PackageDebugger): the app model started us, so nobody reads our stderr, and
 * the notes go down this pipe to the launcher that asked for the package instead, which prints them.
 */
HANDLE g_notePipe = nullptr;
/** Notes come from the pipe threads of package mode as well as the main thread. */
std::mutex g_noteLock;

std::string Utf8(const std::wstring& s)
{
    if (s.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

/** One line on stderr as it stands, "dxinsp: " and all (a note forwarded from a package debugger). */
void NoteLine(const std::wstring& line)
{
    std::lock_guard<std::mutex> lock(g_noteLock);
    fwprintf(stderr, L"%s\n", line.c_str());
    fflush(stderr);
}

void Note(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_noteLock);
    if (g_notePipe)
    {
        const std::string line = "dxinsp: " + Utf8(buf) + "\n";
        DWORD written = 0;
        WriteFile(g_notePipe, line.data(), (DWORD)line.size(), &written, nullptr);
        return;
    }
    fwprintf(stderr, L"dxinsp: %s\n", buf);
    fflush(stderr);
}

// One argument quoted the way the CRT's parser (and CommandLineToArgvW) reads it back.
std::wstring Quote(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (c == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
            out += c;
        }
        else
        {
            out.append(backslashes, L'\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

bool IsX64Image(const std::wstring& exe, std::wstring& why)
{
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        why = L"cannot open the executable";
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    bool ok = ReadFile(f, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    if (ok)
        ok = SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
    if (ok)
        ok = ReadFile(f, &sig, sizeof(sig), &read, nullptr) && sig == IMAGE_NT_SIGNATURE;
    if (ok)
        ok = ReadFile(f, &fh, sizeof(fh), &read, nullptr) && read == sizeof(fh);
    CloseHandle(f);
    if (!ok)
    {
        why = L"not a Windows executable";
        return false;
    }
    if (fh.Machine != IMAGE_FILE_MACHINE_AMD64)
    {
        why = L"not an x64 executable (only x64 targets are injected)";
        return false;
    }
    return true;
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
 *
 * Up to kMaxBurstUs while the wait is short, and up to kLongBurstUs once it has gone on for
 * kLongWaitMs. A wait that long means the application is still in its own start-up, holding the
 * loader lock through its static libraries' initialization (Flight Simulator 2024 does so for half
 * a minute), and an application inside its loader has not reached its device yet. Short bursts
 * there only slowed it down: at 4 ms against the 5 ms hold it ran at under half speed, so a start-up
 * that takes ten seconds took thirty.
 */
constexpr DWORD kPulseWaitMs = 5;
constexpr unsigned kFirstBurstUs = 200;
constexpr unsigned kMaxBurstUs = 4000;
constexpr unsigned kLongBurstUs = 100000;
constexpr ULONGLONG kLongWaitMs = 2000;
constexpr DWORD kRemoteThreadTimeoutMs = 120000;

using NtProcessFn = LONG(NTAPI*)(HANDLE);

NtProcessFn NtProcess(const char* name)
{
    static HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll ? (NtProcessFn)GetProcAddress(ntdll, name) : nullptr;
}

/** Busy-waits, since Sleep cannot do fractions of a millisecond and a burst is measured in microseconds. */
void SpinUs(unsigned us)
{
    LARGE_INTEGER frequency{}, start{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const long long ticks = (long long)((double)frequency.QuadPart * us / 1e6);
    do
    {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < ticks);
}

class Freezer
{
public:
    Freezer(HANDLE process, DWORD pid) : _process(process), _pid(pid) {}
    ~Freezer() { Release(); }
    Freezer(const Freezer&) = delete;
    Freezer& operator=(const Freezer&) = delete;

    /** Stops the whole process at once. False when it could not be (then the injection races it). */
    bool Stop()
    {
        NtProcessFn suspend = NtProcess("NtSuspendProcess");
        _stopped = suspend && suspend(_process) >= 0;
        return _stopped;
    }

    /**
     * Hands the hold over to per-thread suspensions of every thread but `exceptTid`, the one
     * running the injection, and lets the process-wide suspension go. Called with the process
     * already stopped, so enumerating its threads costs the application nothing.
     */
    void HoldAllBut(DWORD exceptTid)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID != _pid || te.th32ThreadID == exceptTid)
                        continue;
                    if (_ids.count(te.th32ThreadID))
                        continue;
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (!thread)
                        continue;
                    if (SuspendThread(thread) != (DWORD)-1)
                    {
                        _ids.insert(te.th32ThreadID);
                        _threads.push_back(thread);
                    }
                    else
                    {
                        CloseHandle(thread);
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        // The process-wide suspension would hold the injecting thread too, so it goes now that the
        // application's own threads are held one by one.
        if (_stopped)
        {
            if (NtProcessFn resume = NtProcess("NtResumeProcess"))
                resume(_process);
            _stopped = false;
        }
    }

    /** Lets the application run for `us` microseconds and holds it again (the loader lock). */
    void Burst(unsigned us)
    {
        ++_bursts;
        for (HANDLE thread : _threads)
            ResumeThread(thread);
        SpinUs(us);
        for (HANDLE thread : _threads)
            SuspendThread(thread);
    }

    /** Lets the application go, however it is being held. */
    void Release()
    {
        for (HANDLE thread : _threads)
        {
            ResumeThread(thread);
            CloseHandle(thread);
        }
        _threads.clear();
        _ids.clear();
        if (_stopped)
        {
            if (NtProcessFn resume = NtProcess("NtResumeProcess"))
                resume(_process);
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
 *
 * `stillRunning` is set when the wait gave up on a thread that exists: it may yet run, so whatever
 * it uses in the target has to stay there. Freeing its code under it is what crashed Flight
 * Simulator 2024 (an execute fault at the start of the freed block, once the loader let it go).
 */
bool RunRemote(HANDLE process, LPTHREAD_START_ROUTINE entry, LPVOID arg, DWORD* exitCode, std::wstring& why,
    Freezer* freezer, bool* stillRunning)
{
    *stillRunning = false;
    DWORD tid = 0;
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, entry, arg, 0, &tid);
    if (!thread)
    {
        why = L"CreateRemoteThread failed (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    // The thread just made is one of the target's own: the hold has to leave it out.
    if (freezer)
        freezer->HoldAllBut(tid);
    const ULONGLONG begun = GetTickCount64();
    const ULONGLONG deadline = begun + kRemoteThreadTimeoutMs;
    unsigned burstUs = kFirstBurstUs;
    for (;;)
    {
        DWORD wait = WaitForSingleObject(thread, freezer ? kPulseWaitMs : kRemoteThreadTimeoutMs);
        if (wait == WAIT_OBJECT_0)
            break;
        if (!freezer || GetTickCount64() >= deadline)
        {
            CloseHandle(thread);
            *stillRunning = true;
            why = L"the remote thread did not finish within " + std::to_wstring(kRemoteThreadTimeoutMs / 1000) + L" seconds";
            return false;
        }
        freezer->Burst(burstUs);
        const unsigned cap = GetTickCount64() - begun >= kLongWaitMs ? kLongBurstUs : kMaxBurstUs;
        burstUs = burstUs < cap ? std::min(burstUs * 2, cap) : cap;
    }
    if (exitCode)
        GetExitCodeThread(thread, exitCode);
    CloseHandle(thread);
    return true;
}

/** Writes `bytes` of `data` into the target; null (with `why` set) when it could not be written. */
LPVOID WriteRemote(HANDLE process, const void* data, size_t bytes, std::wstring& why)
{
    LPVOID remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        why = L"VirtualAllocEx failed (" + std::to_wstring(GetLastError()) + L")";
        return nullptr;
    }
    if (!WriteProcessMemory(process, remote, data, bytes, nullptr))
    {
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
std::vector<wchar_t> SettingsBlock(const std::vector<std::wstring>& entries)
{
    std::vector<wchar_t> block;
    if (entries.empty())
        return block;
    for (const std::wstring& e : entries)
    {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(0);
    }
    block.push_back(0);
    return block;
}

/**
 * The offset of the library's initializer inside it, from our own copy of it; the stub below adds
 * it to whatever base the target's loader chose. SIZE_MAX with `why` set when the library cannot
 * be read. Read once, since a watch injects into one process after another and loading a copy of
 * the library takes milliseconds the race for a device does not have.
 *
 * The initializer is GpuInspectorInitialize, the export a plugin's capture library provides
 * (docs/PLUGINS.md), or DxinspInitialize, the D3D12 library's own name for the same thing.
 */
uintptr_t InitOffset(const std::wstring& dllPath, std::wstring& why)
{
    HMODULE local = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local)
    {
        why = L"cannot load " + dllPath + L" (" + std::to_wstring(GetLastError()) + L")";
        return SIZE_MAX;
    }
    FARPROC init = GetProcAddress(local, "GpuInspectorInitialize");
    if (!init)
        init = GetProcAddress(local, "DxinspInitialize");
    if (!init)
    {
        FreeLibrary(local);
        why = dllPath + L" has no GpuInspectorInitialize (or DxinspInitialize) export";
        return SIZE_MAX;
    }
    uintptr_t offset = (uintptr_t)init - (uintptr_t)local;
    FreeLibrary(local);
    return offset;
}

/**
 * A library to inject and its initializer's offset. The D3D12 library comes first; any others are
 * the capture libraries of plugins (docs/PLUGINS.md), which go in the same way, one after another,
 * while the process is still held.
 */
struct Library
{
    std::wstring path;
    uintptr_t offset = SIZE_MAX;
};

/** The libraries whose initializer could be found, each failure noted with what `what` calls the target. */
std::vector<Library> Resolve(const std::vector<std::wstring>& dlls)
{
    std::vector<Library> libs;
    for (const std::wstring& dll : dlls)
    {
        std::wstring why;
        const uintptr_t offset = InitOffset(dll, why);
        if (offset == SIZE_MAX)
            Note(L"%s; the target runs without it", why.c_str());
        else
            libs.push_back({dll, offset});
    }
    return libs;
}

void Emit(std::vector<uint8_t>& code, std::initializer_list<uint8_t> bytes)
{
    code.insert(code.end(), bytes);
}

void EmitU32(std::vector<uint8_t>& code, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        code.push_back((uint8_t)(v >> (8 * i)));
}

void EmitU64(std::vector<uint8_t>& code, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        code.push_back((uint8_t)(v >> (8 * i)));
}

/** What the stub writes back: the library's base in the target, and what its initializer returned. */
struct StubResult
{
    uint64_t module;
    uint32_t initResult;
};

/** initResult before the stub has run the initializer, which is not a value it can return. */
constexpr uint32_t kInitNotRun = 0xFFFFFFFFu;

/**
 * The code run in the target: load the library, and if that worked call its initializer at
 * `base + offset`, writing both answers into `result`.
 *
 * Doing it in one go is what makes this reliable. The two steps used to be two remote threads with
 * a look at the target's module list in between, to turn the base into the initializer's address --
 * a remote thread's exit code holds only the low half of the HMODULE, so the base had to come from
 * somewhere. But that list cannot be read while a process is held still in the middle of its own
 * start-up: Toolhelp and psapi both answer ERROR_PARTIAL_COPY, and an application caught two
 * milliseconds after it started with a hundred libraries left to load is in exactly that state
 * about half the time. Minecraft is; the test applications, with a handful of libraries, are not.
 * A stub returns the whole pointer through memory we allocated ourselves, which reads back
 * whatever the loader is doing.
 *
 * x64 only, which is all that is injected into (IsX64Image). At entry the stack is aligned as a
 * call leaves it, so the 0x38 taken here puts it back on a 16-byte boundary and leaves the 32
 * bytes of shadow space the calls need.
 */
std::vector<uint8_t> InjectStub(uint64_t pathAddr, uint64_t loadLibrary, uint64_t resultAddr,
    uint64_t settingsAddr, uint32_t initOffset)
{
    std::vector<uint8_t> c;
    Emit(c, {0x48, 0x83, 0xEC, 0x38});                    // sub  rsp, 38h
    Emit(c, {0x48, 0xB9});
    EmitU64(c, pathAddr);          // mov  rcx, <path>
    Emit(c, {0x48, 0xB8});
    EmitU64(c, loadLibrary);       // mov  rax, <LoadLibraryW>
    Emit(c, {0xFF, 0xD0});                                // call rax
    Emit(c, {0x48, 0xBA});
    EmitU64(c, resultAddr);        // mov  rdx, <result>
    Emit(c, {0x48, 0x89, 0x02});                          // mov  [rdx], rax        (the module)
    Emit(c, {0x48, 0x85, 0xC0});                          // test rax, rax
    Emit(c, {0x74, 0x1F});                                // je   done              (31 bytes on)
    Emit(c, {0x48, 0xB9});
    EmitU64(c, settingsAddr);      // mov  rcx, <settings>
    Emit(c, {0x48, 0x05});
    EmitU32(c, initOffset);        // add  rax, <offset>
    Emit(c, {0xFF, 0xD0});                                // call rax               (DxinspInitialize)
    Emit(c, {0x48, 0xBA});
    EmitU64(c, resultAddr);        // mov  rdx, <result>
    Emit(c, {0x89, 0x42, 0x08});                          // mov  [rdx+8], eax      (what it returned)
    Emit(c, {0x31, 0xC0});                                // done: xor eax, eax
    Emit(c, {0x48, 0x83, 0xC4, 0x38});                    // add  rsp, 38h
    Emit(c, {0xC3});                                      // ret
    return c;
}

/** Remote memory freed with the process handle it belongs to. */
struct RemoteBlock
{
    HANDLE process = nullptr;
    LPVOID address = nullptr;
    RemoteBlock() = default;
    RemoteBlock(HANDLE p, LPVOID a) : process(p), address(a) {}
    RemoteBlock(const RemoteBlock&) = delete;
    RemoteBlock& operator=(const RemoteBlock&) = delete;
    RemoteBlock(RemoteBlock&& other) noexcept : process(other.process), address(other.address)
    {
        other.address = nullptr;
    }
    RemoteBlock& operator=(RemoteBlock&& other) noexcept
    {
        if (this != &other)
        {
            if (address)
                VirtualFreeEx(process, address, 0, MEM_RELEASE);
            process = other.process;
            address = other.address;
            other.address = nullptr;
        }
        return *this;
    }
    ~RemoteBlock()
    {
        if (address)
            VirtualFreeEx(process, address, 0, MEM_RELEASE);
    }
    uint64_t addr() const { return (uint64_t)address; }
};

/**
 * Loads the library into the process and runs its initializer there. `settings` is the environment
 * block above, given to DxinspInitialize; empty for a launch, whose target inherited our
 * environment already.
 */
bool Inject(HANDLE process, DWORD pid, const std::wstring& dllPath, uintptr_t offset,
    const std::vector<wchar_t>& settings, Freezer* freezer, std::wstring& why)
{
    (void)pid;
    RemoteBlock path(process, WriteRemote(process, dllPath.c_str(), (dllPath.size() + 1) * sizeof(wchar_t), why));
    if (!path.address)
        return false;

    RemoteBlock remoteSettings;
    if (!settings.empty())
    {
        remoteSettings = RemoteBlock(process, WriteRemote(process, settings.data(), settings.size() * sizeof(wchar_t), why));
        if (!remoteSettings.address)
            return false;
    }

    // The stub writes into this, and the initializer's answer is told apart from a zero it could
    // return by the value put there first.
    const StubResult blank{0, kInitNotRun};
    RemoteBlock result(process, WriteRemote(process, &blank, sizeof(blank), why));
    if (!result.address)
        return false;

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = (uint64_t)GetProcAddress(kernel, "LoadLibraryW");
    if (!loadLibrary)
    {
        why = L"kernel32!LoadLibraryW could not be found";
        return false;
    }
    const std::vector<uint8_t> stub =
        InjectStub(path.addr(), loadLibrary, result.addr(), remoteSettings.addr(), (uint32_t)offset);

    RemoteBlock code(process, WriteRemote(process, stub.data(), stub.size(), why));
    if (!code.address)
        return false;
    DWORD previous = 0;
    if (!VirtualProtectEx(process, code.address, stub.size(), PAGE_EXECUTE_READ, &previous))
    {
        why = L"VirtualProtectEx failed (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    FlushInstructionCache(process, code.address, stub.size());

    DWORD ignored = 0;
    bool stillRunning = false;
    if (!RunRemote(process, (LPTHREAD_START_ROUTINE)code.address, nullptr, &ignored, why, freezer, &stillRunning))
    {
        if (stillRunning)
        {
            // The thread may run later, and reads all four blocks when it does: they stay, a few
            // hundred bytes left behind rather than an execute fault in the application.
            path.address = remoteSettings.address = result.address = code.address = nullptr;
            why += L"; it was left to finish on its own, so the library may still load, too late for a device made meanwhile";
        }
        return false;
    }

    StubResult got{};
    DWORD readError = 0;
    bool haveAnswer = false;
    for (int attempt = 0; attempt < 20 && !haveAnswer; ++attempt)
    {
        SIZE_T read = 0;
        if (ReadProcessMemory(process, result.address, &got, sizeof(got), &read) && read == sizeof(got))
        {
            haveAnswer = true;
            break;
        }
        readError = GetLastError();
        if (freezer)
            freezer->Burst(kFirstBurstUs);
        Sleep(5);
    }
    if (!haveAnswer)
    {
        DWORD exitCode = 0;
        const bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
        why = L"the stub's answer could not be read back (" + std::to_wstring(readError) + L"); the target is " + (alive ? L"still running" : L"gone (exit code " + std::to_wstring(exitCode) + L")");
        return false;
    }
    if (!got.module)
    {
        why = L"LoadLibraryW in the target returned null: it could not load " + dllPath + L" (is that path readable by the target, and are the library's dependencies present?)";
        return false;
    }
    if (got.initResult == kInitNotRun)
    {
        why = L"the initializer was not reached";
        return false;
    }
    if (got.initResult != 0)
    {
        why = L"the initializer returned " + std::to_wstring(got.initResult);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Watch mode

struct WatchOptions
{
    /** What the user named: an image name ("TestVulkan.exe") or a full path to one. */
    std::wstring wanted;
    /** The image name alone, which is what the process list holds. */
    std::wstring image;
    /** `wanted` is a path: the process's own image path has to match it too. */
    bool fullPath = false;
    std::vector<std::wstring> dlls;
    std::vector<std::wstring> env;
    /** --follow: the children of the watched process to inject into as well (Follower). */
    std::vector<std::wstring> follow;
    /** --follow-children: every child, rather than the ones --follow names. */
    bool followChildren = false;
    int timeoutSeconds = 0;
    int pollMs = 2;
    bool once = false;
};

/** How long the process had been running when we caught it, in milliseconds; 0 when unknown. */
unsigned long long ProcessAgeMs(HANDLE process)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = created.dwLowDateTime;
    a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart)
        return 0;
    return (b.QuadPart - a.QuadPart) / 10000;
}

/** A path spelled the way the system spells one, so a --watch given with forward slashes matches. */
std::wstring Normalized(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

/** The process's executable path, empty when it cannot be read. */
std::wstring ImagePath(HANDLE process)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    if (!QueryFullProcessImageNameW(process, 0, buf, &n))
        return std::wstring();
    return std::wstring(buf, n);
}

/**
 * Injects into one matching process, holding it still meanwhile. `keep` is left holding the process
 * when the injection worked and the caller wants to wait for it (--once); otherwise the handle is
 * closed here.
 */
bool InjectRunning(const std::vector<Library>& libs, DWORD pid, const std::wstring& image,
    const std::vector<wchar_t>& settings, HANDLE* keep)
{
    const DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_SUSPEND_RESUME | SYNCHRONIZE;
    HANDLE process = OpenProcess(rights, FALSE, pid);
    if (!process)
    {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED)
        {
            Note(
                L"%s (pid %lu): access denied; the inspector has to run elevated to inject into a process "
                L"running elevated or as another user",
                image.c_str(), pid);
        }
        else
        {
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
    if (IsWow64Process(process, &wow64) && wow64)
    {
        Note(L"%s (pid %lu): not an x64 process (only x64 targets are injected); it runs without the D3D12 capture library", image.c_str(), pid);
        freezer.Release();
        CloseHandle(process);
        return false;
    }
    const ULONGLONG begun = GetTickCount64();
    std::wstring injected;
    for (const Library& lib : libs)
    {
        std::wstring why;
        if (Inject(process, pid, lib.path, lib.offset, settings, &freezer, why))
        {
            injected += (injected.empty() ? L"" : L", ") + lib.path;
        }
        else
        {
            Note(L"%s (pid %lu): injecting %s failed: %s", image.c_str(), pid, lib.path.c_str(), why.c_str());
        }
    }
    const int bursts = freezer.bursts();
    freezer.Release();
    if (injected.empty())
    {
        CloseHandle(process);
        return false;
    }
    Note(L"injected %s into pid %lu (%s), %llu ms after it started, in %llu ms (%s)", injected.c_str(), pid, image.c_str(), age,
        GetTickCount64() - begun,
        !held ? L"the application could not be held meanwhile, so this raced it"
              : (L"held meanwhile, let go " + std::to_wstring(bursts) + L" times for its own libraries").c_str());
    if (age > kLateMs)
    {
        // The hooks are on the entry points that make a device, so a device made before we got in
        // is invisible: say so rather than let the session wait for a connection that never comes.
        Note(
            L"warning: %s had already been running for %llu ms when the library went in; if it had "
            L"created its D3D12 device by then the hooks came too late and nothing will be captured. "
            L"Start the application after the watch begins.",
            image.c_str(), age);
    }
    if (keep)
        *keep = process;
    else
        CloseHandle(process);
    return true;
}

/**
 * A millisecond timer for the duration of a watch. Without it Sleep(5) sleeps for a scheduler tick
 * (about 16 ms), which is most of the time a fast application takes to reach its device.
 */
class FineTimer
{
public:
    FineTimer() : _ok(timeBeginPeriod(1) == TIMERR_NOERROR) {}
    ~FineTimer()
    {
        if (_ok)
            timeEndPeriod(1);
    }
    FineTimer(const FineTimer&) = delete;
    FineTimer& operator=(const FineTimer&) = delete;

private:
    bool _ok;
};

// ---------------------------------------------------------------------------------------------
// Follow mode: the processes the target starts

/** How often the target's children are looked for, in milliseconds. */
constexpr DWORD kFollowPollMs = 5;

using PFN_NtQueryInformationProcess = LONG(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

/**
 * Another process's command line, read out of its PEB the way Task Manager reads one; empty when
 * it cannot be (a process of another user, or one exiting as we look). The image name would not
 * do for a browser, whose every process is chrome.exe and whose --type says which is which.
 */
std::wstring RemoteCommandLine(HANDLE process)
{
    static auto query = (PFN_NtQueryInformationProcess)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!query)
        return std::wstring();
    PROCESS_BASIC_INFORMATION basic{};
    ULONG written = 0;
    if (query(process, ProcessBasicInformation, &basic, sizeof(basic), &written) < 0 || !basic.PebBaseAddress)
        return std::wstring();
    PEB peb{};
    if (!ReadProcessMemory(process, basic.PebBaseAddress, &peb, sizeof(peb), nullptr) || !peb.ProcessParameters)
        return std::wstring();
    RTL_USER_PROCESS_PARAMETERS parameters{};
    if (!ReadProcessMemory(process, peb.ProcessParameters, &parameters, sizeof(parameters), nullptr))
        return std::wstring();
    const USHORT bytes = parameters.CommandLine.Length;
    if (bytes == 0 || !parameters.CommandLine.Buffer)
        return std::wstring();
    std::wstring line(bytes / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(process, parameters.CommandLine.Buffer, line.data(), bytes, nullptr))
        return std::wstring();
    return line;
}

/** Whether `line` holds `text` anywhere, without regard to case; an empty `text` matches anything. */
bool ContainsNoCase(const std::wstring& line, const std::wstring& text)
{
    if (text.empty())
        return true;
    if (text.size() > line.size())
        return false;
    auto lower = [](wchar_t c) { return (wchar_t)towlower(c); };
    auto at = std::search(line.begin(), line.end(), text.begin(), text.end(),
        [&](wchar_t a, wchar_t b) { return lower(a) == lower(b); });
    return at != line.end();
}

/**
 * Injects into the processes the target starts, for as long as the target runs. Each pass takes a
 * process snapshot (a few tenths of a millisecond), looks only at the processes that have appeared
 * since the last one, and follows each one's parents back to the target: a browser's GPU process is
 * its child, a grandchild would be a relaunched browser's. A match is injected into exactly as a
 * watched process is, frozen while the library goes in, which is what gets the hooks in before the
 * child's D3D12CreateDevice -- Chrome's GPU process makes its device a few hundred milliseconds
 * after it starts, so a poll every few milliseconds is in time with room to spare.
 */
class Follower
{
public:
    Follower(std::vector<Library> libs, std::vector<wchar_t> settings,
        std::vector<std::wstring> patterns, bool all, DWORD root)
        : _libs(std::move(libs)), _settings(std::move(settings)), _patterns(std::move(patterns)), _all(all), _root(root)
    {
        _known.insert(root);
        _tree.insert(root);
    }

    void Poll()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        std::map<DWORD, DWORD> parents;
        std::vector<std::pair<DWORD, std::wstring>> fresh;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        _live = 0;
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                parents[pe.th32ProcessID] = pe.th32ParentProcessID;
                if (_tree.count(pe.th32ProcessID))
                    ++_live;
                // A pid names one process for its lifetime, so each is considered once.
                if (!_known.count(pe.th32ProcessID))
                    fresh.emplace_back(pe.th32ProcessID, pe.szExeFile);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        for (const auto& [pid, image] : fresh)
        {
            _known.insert(pid);
            if (!InTree(pid, parents))
                continue;
            _tree.insert(pid);
            ++_live;
            Consider(pid, image);
        }
    }

    /** How many of the target's processes were running at the last poll. */
    size_t live() const { return _live; }

private:
    /**
     * Whether the process belongs to the target's tree: one of its ancestors is the target or a
     * process already in the tree. Membership is kept rather than walked back to the target every
     * time because the target itself may be gone — Firefox's first process re-launches the browser
     * and exits, so the browser's own children descend from a pid that no longer exists — and
     * because only a process that appeared while we were watching is ever considered, which is
     * what keeps a reused pid from being taken for the target's.
     */
    bool InTree(DWORD pid, const std::map<DWORD, DWORD>& parents) const
    {
        for (int generation = 0; generation < 8 && pid != 0; ++generation)
        {
            auto it = parents.find(pid);
            if (it == parents.end())
                return false;
            if (_tree.count(it->second))
                return true;
            pid = it->second;
        }
        return false;
    }

    /**
     * A child is followed when its command line holds one of the wanted texts and none of the
     * excluded ones (a pattern written "!text"). Chrome starts a second --type=gpu-process to
     * collect GPU information, which makes a device of its own and exits again; "!--use-gl=disabled"
     * leaves that one alone, so the port belongs to the process that renders whichever starts first.
     *
     * With --follow-children every child is wanted to begin with, and the patterns only take away.
     * Injecting into a child that never renders costs it the load of a library that then sits
     * still: the listener comes up on D3D12CreateDevice (hooks_device.cpp), so a child with no
     * device of its own takes no port and never shows up as something to attach to.
     *
     * The image name counts as part of the command line, since a process started with its
     * application name given apart need not repeat it there: a package launch follows the game
     * behind an entry like GameLaunchHelper.exe by its name (LaunchPackage).
     */
    void Consider(DWORD pid, const std::wstring& image)
    {
        HANDLE query = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!query)
            return;
        const std::wstring line = RemoteCommandLine(query) + L" " + image;
        CloseHandle(query);
        bool wanted = _all;
        for (const std::wstring& pattern : _patterns)
        {
            const bool excluding = !pattern.empty() && pattern[0] == L'!';
            const std::wstring text = excluding ? pattern.substr(1) : pattern;
            if (!ContainsNoCase(line, text))
                continue;
            if (excluding)
                return;
            wanted = true;
        }
        if (wanted)
            InjectRunning(_libs, pid, image, _settings, nullptr);
    }

    std::vector<Library> _libs;
    std::vector<wchar_t> _settings;
    std::vector<std::wstring> _patterns;
    /** --follow-children: every child of the target, rather than the ones the patterns name. */
    bool _all = false;
    DWORD _root;
    std::set<DWORD> _known;
    /** The target and every process descended from it that we have seen, and how many still run. */
    std::set<DWORD> _tree;
    size_t _live = 0;
};

/** How long the tree may be empty before a followed target counts as finished. */
constexpr DWORD kTreeGraceMs = 2000;

/**
 * Waits for the target and returns its exit code, injecting into the processes it starts meanwhile
 * when --follow asked for them. Without --follow (or without a library to inject) this is the plain
 * wait the launcher has always done.
 *
 * With --follow the wait is for the target's whole tree, not the process we started: Firefox's
 * first process launches the browser and exits within a second, and the browser's GPU process — the
 * one worth following — starts after that. So once the target is gone the poll goes on while any of
 * its processes are still running, and the launcher stands in for the tree the way it stands in for
 * a single target (the session's log, its status and its Stop then apply to the whole browser).
 */
DWORD WaitForTarget(HANDLE process, DWORD pid, const std::vector<Library>& libs,
    const std::vector<std::wstring>& patterns, bool followAll,
    const std::vector<wchar_t>& settings, DWORD pollMs)
{
    if ((patterns.empty() && !followAll) || libs.empty())
    {
        WaitForSingleObject(process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        return code;
    }
    FineTimer timer;   // or the poll sleeps a scheduler tick, and children start in less
    Follower follower(libs, settings, patterns, followAll, pid);
    while (WaitForSingleObject(process, pollMs) == WAIT_TIMEOUT)
        follower.Poll();
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    follower.Poll();
    if (follower.live() == 0)
        return code;
    Note(L"pid %lu exited with code %lu, leaving %zu process(es) of its own running: following those",
        pid, code, follower.live());
    ULONGLONG emptySince = 0;
    for (;;)
    {
        Sleep(pollMs);
        follower.Poll();
        if (follower.live() > 0)
        {
            emptySince = 0;
            continue;
        }
        // A tree can be briefly empty between one process exiting and the next appearing.
        if (emptySince == 0)
            emptySince = GetTickCount64();
        else if (GetTickCount64() - emptySince >= kTreeGraceMs)
            break;
    }
    return code;
}

/** Polls the process list and injects into each new match; see the exit codes at the top. */
int Watch(const WatchOptions& o)
{
    FineTimer timer;
    for (const std::wstring& dll : o.dlls)
    {
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
            Note(L"the capture library is not at %s", dll.c_str());
    }
    const std::vector<Library> libs = Resolve(o.dlls);
    if (libs.empty())
        return kExitFailed;
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
    for (;;)
    {
        DWORD bytes = 0;
        if (EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &bytes))
        {
            const size_t count = bytes / sizeof(DWORD);
            for (size_t i = 0; i < count; ++i)
            {
                const DWORD pid = pids[i];
                if (pid == self || pid == 0)
                    continue;
                if (!known.insert(pid).second)
                    continue;
                // Only a process never seen before is asked for its name, which is what keeps a
                // poll to a fraction of a millisecond (a toolhelp process snapshot costs several).
                HANDLE query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!query)
                    continue;   // a system process, or one of another user: not ours to inject into
                const std::wstring path = ImagePath(query);
                CloseHandle(query);
                if (path.empty())
                    continue;
                const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
                if (_wcsicmp(name.c_str(), o.image.c_str()) != 0)
                    continue;
                if (o.fullPath && _wcsicmp(Normalized(path).c_str(), o.wanted.c_str()) != 0)
                    continue;
                if (InjectRunning(libs, pid, name, settings, o.once ? &injected : nullptr))
                {
                    injectedPid = pid;
                    if (o.once)
                        break;
                }
            }
        }
        if (injectedPid && o.once)
            break;
        if (o.timeoutSeconds > 0 && (GetTickCount64() - started) >= (ULONGLONG)o.timeoutSeconds * 1000)
        {
            if (injectedPid)
            {
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
    if (injected)
    {
        code = WaitForTarget(injected, injectedPid, libs, o.follow, o.followChildren, settings, (DWORD)o.pollMs);
        CloseHandle(injected);
        Note(L"pid %lu exited with code %lu", injectedPid, code);
    }
    return (int)code;
}

// ---------------------------------------------------------------------------------------------
// Package mode: a packaged (MSIX, Microsoft Store, Xbox app) application

/** An installed package's application, as the app model knows it. */
struct PackageApp
{
    /** Microsoft.FlightSimulator_1.39.12.0_x64__8wekyb3d8bbwe */
    std::wstring fullName;
    /** The application user model ID ActivateApplication takes: <family name>!<application id>. */
    std::wstring aumid;
    /** The application's own executable, the manifest's Executable under the install directory. */
    std::wstring executable;
};

/** The file read as UTF-8 (a manifest), empty when it cannot be. */
std::wstring ReadTextFile(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return std::wstring();
    std::string bytes;
    char buf[65536];
    DWORD read = 0;
    while (ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0)
        bytes.append(buf, read);
    CloseHandle(f);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    return Wide(bytes);
}

/** The XML start tags named `name` (unprefixed), each from "<" to its ">". */
std::vector<std::wstring> XmlTags(const std::wstring& doc, const std::wstring& name)
{
    std::vector<std::wstring> tags;
    const std::wstring open = L"<" + name;
    for (size_t at = doc.find(open); at != std::wstring::npos; at = doc.find(open, at + 1))
    {
        const size_t after = at + open.size();
        if (after >= doc.size() || !(iswspace(doc[after]) || doc[after] == L'/' || doc[after] == L'>'))
            continue;   // <Applications>, say, when <Application was asked for
        const size_t end = doc.find(L'>', after);
        if (end == std::wstring::npos)
            break;
        tags.push_back(doc.substr(at, end - at + 1));
    }
    return tags;
}

/** An attribute of a start tag, its five predefined entities decoded; empty when it is absent. */
std::wstring XmlAttribute(const std::wstring& tag, const std::wstring& name)
{
    for (size_t at = tag.find(name); at != std::wstring::npos; at = tag.find(name, at + 1))
    {
        if (at == 0 || !iswspace(tag[at - 1]))
            continue;
        size_t p = at + name.size();
        while (p < tag.size() && iswspace(tag[p]))
            ++p;
        if (p >= tag.size() || tag[p] != L'=')
            continue;
        ++p;
        while (p < tag.size() && iswspace(tag[p]))
            ++p;
        if (p >= tag.size() || (tag[p] != L'"' && tag[p] != L'\''))
            continue;
        const wchar_t quote = tag[p++];
        const size_t end = tag.find(quote, p);
        if (end == std::wstring::npos)
            return std::wstring();
        std::wstring raw = tag.substr(p, end - p), out;
        static const std::pair<const wchar_t*, wchar_t> kEntities[] = {
            {L"&amp;", L'&'}, {L"&quot;", L'"'}, {L"&apos;", L'\''}, {L"&lt;", L'<'}, {L"&gt;", L'>'}};
        for (size_t i = 0; i < raw.size();)
        {
            bool decoded = false;
            if (raw[i] == L'&')
            {
                for (const auto& [text, c] : kEntities)
                {
                    if (raw.compare(i, wcslen(text), text) == 0)
                    {
                        out += c;
                        i += wcslen(text);
                        decoded = true;
                        break;
                    }
                }
            }
            if (!decoded)
                out += raw[i++];
        }
        return out;
    }
    return std::wstring();
}

/** The path with every junction and link resolved, so two spellings of one directory compare equal. */
std::wstring FinalPath(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Normalized(path);
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFinalPathNameByHandleW(h, buf, (DWORD)_countof(buf), FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= _countof(buf))
        return Normalized(path);
    return std::wstring(buf, n);
}

/**
 * The installed package `exe` belongs to. A packaged application cannot be started by
 * CreateProcess the way an ordinary one is -- it would run without its package identity, which
 * a Store game needs for its licence, its storage and its own launcher -- so a launch of one goes
 * through the app model instead (LaunchPackage).
 *
 * The package is found from the AppxManifest.xml in the executable's directory or one above it:
 * its Identity names the package family, which with the manifest's directory picks the installed
 * package, and the Application whose Executable is `exe` (or else the first) is what is activated.
 * Returns 1 with `app` filled in, 0 when `exe` is not in a package, and -1 with `why` set when there
 * is a manifest but no installed package to go with it (an unregistered build layout, say).
 */
int FindPackageApp(const std::wstring& exe, PackageApp& app, std::wstring& why)
{
    std::wstring dir = Normalized(exe);
    std::wstring manifest;
    for (;;)
    {
        const size_t slash = dir.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash < 3)
            return 0;
        dir.erase(slash);
        const std::wstring candidate = dir + L"\\AppxManifest.xml";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            manifest = candidate;
            break;
        }
    }
    const std::wstring doc = ReadTextFile(manifest);
    const std::vector<std::wstring> identity = XmlTags(doc, L"Identity");
    const std::vector<std::wstring> applications = XmlTags(doc, L"Application");
    if (identity.empty() || applications.empty())
    {
        why = manifest + L" names no package identity or no application";
        return -1;
    }
    std::wstring name = XmlAttribute(identity[0], L"Name");
    std::wstring publisher = XmlAttribute(identity[0], L"Publisher");
    PACKAGE_ID id{};
    id.name = name.data();
    id.publisher = publisher.data();
    wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    UINT32 familyLength = (UINT32)_countof(family);
    if (PackageFamilyNameFromId(&id, &familyLength, family) != ERROR_SUCCESS)
    {
        why = manifest + L": no package family name for " + name;
        return -1;
    }

    UINT32 count = 0, bufferLength = 0;
    LONG rc = GetPackagesByPackageFamily(family, &count, nullptr, &bufferLength, nullptr);
    if ((rc != ERROR_SUCCESS && rc != ERROR_INSUFFICIENT_BUFFER) || count == 0)
    {
        why = std::wstring(L"the package ") + family + L" is not installed for this user (" + manifest + L")";
        return -1;
    }
    std::vector<PWSTR> fullNames(count);
    std::vector<wchar_t> buffer(bufferLength);
    if (GetPackagesByPackageFamily(family, &count, fullNames.data(), &bufferLength, buffer.data()) != ERROR_SUCCESS || count == 0)
    {
        why = std::wstring(L"the installed packages of ") + family + L" cannot be listed";
        return -1;
    }
    // The family can have more than one package installed (another version, another
    // architecture): the one installed where the manifest is is the one meant. The Xbox app's
    // games are reached through a junction, so the directories are compared resolved.
    const std::wstring wantedDir = FinalPath(dir);
    std::wstring chosen;
    for (UINT32 i = 0; i < count && chosen.empty(); ++i)
    {
        UINT32 pathLength = 0;
        if (GetPackagePathByFullName(fullNames[i], &pathLength, nullptr) != ERROR_INSUFFICIENT_BUFFER)
            continue;
        std::wstring path(pathLength, L'\0');
        if (GetPackagePathByFullName(fullNames[i], &pathLength, path.data()) != ERROR_SUCCESS)
            continue;
        path.resize(wcslen(path.c_str()));
        if (_wcsicmp(FinalPath(path).c_str(), wantedDir.c_str()) == 0)
            chosen = fullNames[i];
    }
    if (chosen.empty())
    {
        chosen = fullNames[0];
        Note(L"none of the installed packages of %s is installed at %s; using %s", family, dir.c_str(), chosen.c_str());
    }

    const std::wstring wantedExe = Normalized(exe);
    const std::wstring* picked = &applications[0];
    for (const std::wstring& a : applications)
    {
        const std::wstring executable = Normalized(dir + L"\\" + XmlAttribute(a, L"Executable"));
        if (_wcsicmp(executable.c_str(), wantedExe.c_str()) == 0)
        {
            picked = &a;
            break;
        }
    }
    app.fullName = chosen;
    app.aumid = std::wstring(family) + L"!" + XmlAttribute(*picked, L"Id");
    app.executable = Normalized(dir + L"\\" + XmlAttribute(*picked, L"Executable"));
    return 1;
}

/** Turns the app model's debugger hook for the package on (with `debugger`'s command line) or off (null). */
HRESULT SetPackageDebugging(const std::wstring& fullName, const std::wstring* debugger)
{
    IPackageDebugSettings* settings = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_PackageDebugSettings, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&settings));
    if (FAILED(hr))
        return hr;
    hr = debugger ? settings->EnableDebugging(fullName.c_str(), debugger->c_str(), nullptr)
                  : settings->DisableDebugging(fullName.c_str());
    settings->Release();
    return hr;
}

/** A running process of the package, 0 when none is: it made its device long ago, so it cannot be captured. */
DWORD RunningPackageProcess(const std::wstring& fullName)
{
    std::vector<DWORD> pids(4096);
    DWORD bytes = 0;
    if (!EnumProcesses(pids.data(), (DWORD)(pids.size() * sizeof(DWORD)), &bytes))
        return 0;
    for (size_t i = 0; i < bytes / sizeof(DWORD); ++i)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!process)
            continue;
        wchar_t name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
        UINT32 length = (UINT32)_countof(name);
        const bool same = GetPackageFullName(process, &length, name) == ERROR_SUCCESS && _wcsicmp(name, fullName.c_str()) == 0;
        CloseHandle(process);
        if (same)
            return pids[i];
    }
    return 0;
}

/**
 * The launcher's library settings, for a process that does not inherit our environment: every
 * variable whose name holds "INSP_" (DXINSP_PORT, D3D11INSP_LOG, a plugin's), which is what the
 * inspector gives the launcher for the libraries to read (d3d12.ts, plugins.ts).
 */
std::vector<std::wstring> LibrarySettingsFromEnvironment()
{
    std::vector<std::wstring> entries;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block)
        return entries;
    for (const wchar_t* e = block; *e; e += wcslen(e) + 1)
    {
        const std::wstring entry = e;
        const size_t eq = entry.find(L'=', 1);
        if (eq == std::wstring::npos)
            continue;
        std::wstring name = entry.substr(0, eq);
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return (wchar_t)towupper(c); });
        if (name.find(L"INSP_") != std::wstring::npos)
            entries.push_back(entry);
    }
    FreeEnvironmentStringsW(block);
    return entries;
}

/** The package whose debugger hook is on, so a Ctrl+C or a closed console still turns it off. */
std::wstring g_debuggedPackage;

BOOL WINAPI DisableDebuggingOnExit(DWORD)
{
    if (!g_debuggedPackage.empty())
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetPackageDebugging(g_debuggedPackage, nullptr);
    }
    return FALSE;
}

/**
 * The launcher's end of the pipe its package debuggers talk down. Each debugger (PackageDebugger)
 * says which process it holds; that process goes into our job before the debugger lets it go, and
 * everything else it writes is a note to print. A debugger keeps its end open for as long as the
 * process (and, following, its tree) runs, which is how the launcher knows when the application
 * has finished.
 */
class PackagePipe
{
public:
    explicit PackagePipe(HANDLE job) : _job(job)
    {
        _name = L"\\\\.\\pipe\\dxinsp-package-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    }

    /** What each debugger is told before "go": its libraries, their settings, what to follow. */
    void SetConfig(std::string config) { _config = std::move(config); }

    const std::wstring& name() const { return _name; }

    /** Makes the first instance (so no debugger can come before the pipe exists) and serves from then on. */
    bool Start()
    {
        HANDLE first = Instance();
        if (first == INVALID_HANDLE_VALUE)
            return false;
        std::thread([this, first] { Serve(first); }).detach();
        return true;
    }

    /** Waits until a debugger has said which process it holds, or `ms` pass; whether one did. */
    bool WaitForAttach(DWORD ms)
    {
        std::unique_lock<std::mutex> lock(_lock);
        return _changed.wait_for(lock, std::chrono::milliseconds(ms), [this] { return _attached > 0; });
    }

    /** Waits until every debugger has hung up. */
    void WaitForDebuggers()
    {
        std::unique_lock<std::mutex> lock(_lock);
        _changed.wait(lock, [this] { return _open == 0; });
    }

private:
    HANDLE Instance()
    {
        return CreateNamedPipeW(_name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    }

    void Serve(HANDLE pipe)
    {
        for (;;)
        {
            const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            // The next instance before this one is served, so a second debugger never finds none.
            HANDLE next = Instance();
            if (connected)
            {
                {
                    std::lock_guard<std::mutex> lock(_lock);
                    ++_open;
                }
                std::thread([this, pipe] { Converse(pipe); }).detach();
            }
            else
            {
                CloseHandle(pipe);
            }
            if (next == INVALID_HANDLE_VALUE)
                return;
            pipe = next;
        }
    }

    void Converse(HANDLE pipe)
    {
        std::string pending;
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(pipe, buf, sizeof(buf), &read, nullptr) && read > 0)
        {
            pending.append(buf, read);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;)
            {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.rfind("attach ", 0) == 0)
                    Attach(pipe, (DWORD)strtoul(line.c_str() + 7, nullptr, 10));
                else if (!line.empty())
                    NoteLine(Wide(line));
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        std::lock_guard<std::mutex> lock(_lock);
        --_open;
        _changed.notify_all();
    }

    /**
     * Puts the process into the launcher's job, so that stopping the session (which ends the
     * launcher, the job's last handle) ends the application and everything it started, as it does
     * for an application the launcher started itself. The process is still suspended, so its
     * children are all born in the job.
     */
    void Attach(HANDLE pipe, DWORD pid)
    {
        HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
        if (!process || !AssignProcessToJobObject(_job, process))
            Note(L"pid %lu could not be put in the launcher's job (error %lu): stopping the session will not end it", pid, GetLastError());
        if (process)
            CloseHandle(process);
        const std::string reply = _config + "go\n";
        DWORD written = 0;
        WriteFile(pipe, reply.data(), (DWORD)reply.size(), &written, nullptr);
        std::lock_guard<std::mutex> lock(_lock);
        ++_attached;
        _changed.notify_all();
    }

    std::wstring _name;
    std::string _config;
    HANDLE _job;
    std::mutex _lock;
    std::condition_variable _changed;
    int _open = 0;
    int _attached = 0;
};

/** How long after activation the app model has to run our debugger before we say something is wrong. */
constexpr DWORD kDebuggerWaitMs = 30000;

/**
 * Starts a packaged application through the app model, the library going in before its first
 * instruction. This is how Visual Studio and PIX start one: IPackageDebugSettings::EnableDebugging
 * registers a debugger command line for the package, and the next activation creates the
 * application's process suspended and runs that command with "-p <pid> -tid <thread id>" appended.
 * Our debugger is this launcher again (PackageDebugger), which injects exactly as a launch does
 * into a process it created suspended, and resumes the thread. The hook is turned off again as soon
 * as the process is caught, so the package starts normally the next time.
 *
 * The application's process is not our child, so its output never reaches us and its environment
 * is not ours: the debugger hands the libraries their settings as a watch does, and its own notes
 * come back through a pipe (PackagePipe). A Store game's entry is often a launcher of its own
 * (the Xbox app's GameLaunchHelper.exe) that starts the game proper; when `exe` is not the
 * application's entry, every process the entry starts is followed.
 */
int LaunchPackage(const PackageApp& app, const std::wstring& exe, const std::wstring& arguments,
    const std::vector<std::wstring>& dlls, const std::vector<std::wstring>& followPatterns, bool followChildren, int pollMs)
{
    Note(L"%s belongs to the package %s: starting it through the app model as %s", exe.c_str(), app.fullName.c_str(), app.aumid.c_str());
    if (const DWORD running = RunningPackageProcess(app.fullName))
    {
        Note(L"the package is already running (pid %lu); close it first: an application that has made its device cannot be captured", running);
        return kExitFailed;
    }
    const bool viaEntry = _wcsicmp(Normalized(exe).c_str(), app.executable.c_str()) != 0;
    // Only the game: following every child of the entry would also take the updaters and consoles
    // a game keeps starting (Flight Simulator's nvngx_update.exe, a conhost.exe each), every one
    // held while a library goes in.
    std::vector<std::wstring> follow = followPatterns;
    if (viaEntry && !followChildren)
    {
        const std::wstring image = exe.substr(exe.find_last_of(L"\\/") + 1);
        Note(L"the package starts %s, not %s: the processes it starts named %s are captured as well", app.executable.c_str(), exe.c_str(), image.c_str());
        follow.push_back(image);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        Note(L"COM could not be initialized (0x%08lx)", (unsigned long)hr);
        return kExitFailed;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job)
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    PackagePipe pipe(job);

    bool debugging = false;
    if (dlls.empty())
    {
        Note(L"no --dll given: the application runs without the D3D12 capture library");
    }
    else if (!pipe.Start())
    {
        Note(L"the pipe for the package debugger could not be made (error %lu); the application runs without the capture libraries", GetLastError());
    }
    else
    {
        // The debugger, preferably the windowless build of this launcher, or the app model opens
        // a console window for it.
        wchar_t self[MAX_PATH * 2];
        const DWORD selfLength = GetModuleFileNameW(nullptr, self, (DWORD)_countof(self));
        std::wstring debugger(self, selfLength);
        const std::wstring windowless = debugger.substr(0, debugger.find_last_of(L'\\') + 1) + L"dxinsp_package_debugger.exe";
        if (GetFileAttributesW(windowless.c_str()) != INVALID_FILE_ATTRIBUTES)
            debugger = windowless;
        // The app model refuses a debugger command line over 260 characters, so everything else
        // goes down the pipe (PackagePipe::Attach).
        const std::wstring command = Quote(debugger) + L" --package-debugger " + Quote(pipe.name());
        std::string config;
        for (const std::wstring& dll : dlls)
            config += "dll " + Utf8(dll) + "\n";
        for (const std::wstring& entry : LibrarySettingsFromEnvironment())
            config += "env " + Utf8(entry) + "\n";
        for (const std::wstring& f : follow)
            config += "follow " + Utf8(f) + "\n";
        if (followChildren)
            config += "follow-children\n";
        config += "poll " + std::to_string(pollMs) + "\n";
        pipe.SetConfig(config);
        hr = SetPackageDebugging(app.fullName, &command);
        if (FAILED(hr))
        {
            Note(L"the package's debugger could not be set (0x%08lx); the application runs without the capture libraries", (unsigned long)hr);
        }
        else
        {
            debugging = true;
            g_debuggedPackage = app.fullName;
            SetConsoleCtrlHandler(DisableDebuggingOnExit, TRUE);
        }
    }

    IApplicationActivationManager* activation = nullptr;
    hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&activation));
    DWORD pid = 0;
    if (SUCCEEDED(hr))
    {
        CoAllowSetForegroundWindow(activation, nullptr);
        hr = activation->ActivateApplication(app.aumid.c_str(), arguments.empty() ? nullptr : arguments.c_str(), AO_NONE, &pid);
        activation->Release();
    }
    if (FAILED(hr))
    {
        Note(L"activating %s failed (0x%08lx)", app.aumid.c_str(), (unsigned long)hr);
        if (debugging)
            SetPackageDebugging(app.fullName, nullptr);
        CoUninitialize();
        return kExitFailed;
    }
    Note(L"activated %s: pid %lu", app.aumid.c_str(), pid);
    if (debugging)
    {
        if (!pipe.WaitForAttach(kDebuggerWaitMs))
        {
            Note(L"warning: the app model never ran the package debugger, so nothing was injected and nothing will be captured");
        }
        SetPackageDebugging(app.fullName, nullptr);
        g_debuggedPackage.clear();
    }

    DWORD code = 0;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process)
    {
        WaitForSingleObject(process, INFINITE);
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
    }
    // The debugger stays for as long as the tree it follows: the game behind a launcher entry
    // outlives the entry.
    if (debugging)
        pipe.WaitForDebuggers();
    Note(L"pid %lu exited with code %lu", pid, code);
    CoUninitialize();
    return (int)code;
}

/**
 * The package debugger: run by the app model with the application's process created suspended (see
 * LaunchPackage). Injects the libraries, lets the main thread go and stands in for the application
 * (following its children when asked) for the launcher at the other end of `pipeName`.
 *
 * When the pipe is not there the launcher that set the hook up died before it could take it off
 * again (killed, say), and this activation is the user starting the application normally: the hook
 * is taken off and the application let go untouched.
 */
int PackageDebugger(const std::wstring& pipeName, DWORD pid, DWORD tid)
{
    HANDLE thread = tid ? OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid) : nullptr;
    HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        HANDLE query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        wchar_t fullName[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
        UINT32 length = (UINT32)_countof(fullName);
        if (query && GetPackageFullName(query, &length, fullName) == ERROR_SUCCESS &&
            SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        {
            SetPackageDebugging(fullName, nullptr);
        }
        if (query)
            CloseHandle(query);
        if (thread)
            ResumeThread(thread);
        return 0;
    }
    g_notePipe = pipe;

    // The launcher answers once the process is in its job, with what the command line has no room
    // for (the app model takes 260 characters of it): one setting a line, then "go".
    std::vector<std::wstring> dlls, env, follow;
    bool followChildren = false;
    int pollMs = (int)kFollowPollMs;
    {
        const std::string hello = "attach " + std::to_string(pid) + "\n";
        DWORD io = 0;
        WriteFile(pipe, hello.data(), (DWORD)hello.size(), &io, nullptr);
        std::string pending;
        char buf[4096];
        bool done = false;
        while (!done && ReadFile(pipe, buf, sizeof(buf), &io, nullptr) && io > 0)
        {
            pending.append(buf, io);
            for (size_t nl; !done && (nl = pending.find('\n')) != std::string::npos;)
            {
                const std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                const size_t space = line.find(' ');
                const std::string key = line.substr(0, space);
                const std::wstring value = space == std::string::npos ? std::wstring() : Wide(line.substr(space + 1));
                if (key == "go")
                    done = true;
                else if (key == "dll")
                    dlls.push_back(value);
                else if (key == "env")
                    env.push_back(value);
                else if (key == "follow")
                    follow.push_back(value);
                else if (key == "follow-children")
                    followChildren = true;
                else if (key == "poll")
                    pollMs = _wtoi(value.c_str());
            }
        }
    }

    const DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_SUSPEND_RESUME | SYNCHRONIZE;
    HANDLE process = OpenProcess(rights, FALSE, pid);
    if (!process)
    {
        Note(L"pid %lu cannot be opened (error %lu); it runs without the capture libraries", pid, GetLastError());
        if (thread)
            ResumeThread(thread);
        CloseHandle(pipe);
        return kExitFailed;
    }
    const std::wstring path = ImagePath(process);
    const std::wstring image = path.substr(path.find_last_of(L'\\') + 1);
    std::vector<Library> libs = Resolve(dlls);
    const std::vector<wchar_t> settings = SettingsBlock(env);

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64) && wow64)
    {
        Note(L"%s (pid %lu): not an x64 process (only x64 targets are injected); it runs without the capture libraries", image.c_str(), pid);
        libs.clear();
    }
    // What would make the library's LoadLibraryW fail, said before it does.
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token))
    {
        DWORD container = 0, size = 0;
        if (GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &size) && container)
        {
            Note(L"%s runs in an app container: the capture libraries' directory has to be readable by it "
                 L"(icacls <dir> /grant *S-1-15-2-1:(OI)(CI)RX)", image.c_str());
        }
        CloseHandle(token);
    }
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
    if (GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &signature, sizeof(signature)) && signature.MicrosoftSignedOnly)
    {
        Note(L"%s only loads libraries Microsoft signed, so the capture libraries cannot go in", image.c_str());
    }

    // The app model created the process with this thread suspended. Suspended once more, so the
    // count says whether it really was; a process that is already running is injected the way a
    // watch injects one.
    const DWORD previous = thread ? SuspendThread(thread) : (DWORD)-1;
    if (previous == (DWORD)-1 || previous == 0)
    {
        if (thread && previous == 0)
            ResumeThread(thread);
        Note(L"%s (pid %lu) was not suspended when the debugger started; injecting into it running", image.c_str(), pid);
        if (!libs.empty())
            InjectRunning(libs, pid, image, settings, nullptr);
    }
    else
    {
        for (const Library& lib : libs)
        {
            std::wstring why;
            if (Inject(process, pid, lib.path, lib.offset, settings, nullptr, why))
                Note(L"injected %s into pid %lu (%s)", lib.path.c_str(), pid, image.c_str());
            else
                Note(L"injecting %s into pid %lu (%s) failed: %s; it runs without it", lib.path.c_str(), pid, image.c_str(), why.c_str());
        }
        if (followChildren)
            Note(L"following every child of pid %lu every %d ms", pid, pollMs);
        ResumeThread(thread);   // ours
        ResumeThread(thread);   // the app model's
    }
    if (thread)
        CloseHandle(thread);
    const DWORD code = WaitForTarget(process, pid, libs, follow, followChildren, settings, (DWORD)(pollMs > 0 ? pollMs : (int)kFollowPollMs));
    CloseHandle(process);
    g_notePipe = nullptr;
    CloseHandle(pipe);
    return (int)code;
}

int Usage()
{
    fwprintf(stderr, L"usage: dxinsp_launch.exe --dll <dxinsp_capture.dll> [--cwd <dir>] [--follow <text>]...\n");
    fwprintf(stderr, L"                         -- <exe> [args...]\n");
    fwprintf(stderr, L"       dxinsp_launch.exe --watch <image name or full path> --dll <dxinsp_capture.dll>\n");
    fwprintf(stderr, L"                         [--env NAME=VALUE]... [--timeout <seconds>] [--poll <ms>] [--once]\n");
    fwprintf(stderr, L"                         [--follow <text>]...\n");
    fwprintf(stderr, L"       --follow also injects into the processes the target starts whose command line holds\n");
    fwprintf(stderr, L"       <text>, such as --follow --type=gpu-process for a browser's GPU process;\n");
    fwprintf(stderr, L"       --follow !<text> leaves a child holding <text> alone instead.\n");
    fwprintf(stderr, L"       --follow-children takes every child, which --follow !<text> can then narrow.\n");
    fwprintf(stderr, L"       An <exe> inside an installed package (Microsoft Store, Xbox app) is started through the\n");
    fwprintf(stderr, L"       app model with the libraries injected by its debugger hook; --no-package starts it directly.\n");
    fwprintf(stderr, L"       --dll may be given more than once: each library is injected in turn (a plugin's\n");
    fwprintf(stderr, L"       capture library beside the D3D12 one), its GpuInspectorInitialize export run.\n");
    return kExitUsage;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> dlls;
    std::wstring cwd;
    WatchOptions watch;
    std::vector<std::wstring> follow;
    bool followChildren = false;
    bool watching = false;
    bool noPackage = false;
    // Package debugger mode (PackageDebugger): what the app model's debugger hook runs.
    std::wstring packagePipe;
    DWORD debugPid = 0, debugTid = 0;
    int followPollMs = (int)kFollowPollMs;
    int i = 1;
    for (; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--dll" && i + 1 < argc)
            dlls.push_back(argv[++i]);
        else if (a == L"--cwd" && i + 1 < argc)
            cwd = argv[++i];
        else if (a == L"--watch" && i + 1 < argc)
        {
            watching = true;
            watch.wanted = argv[++i];
        }
        else if (a == L"--env" && i + 1 < argc)
            watch.env.push_back(argv[++i]);
        else if (a == L"--follow" && i + 1 < argc)
            follow.push_back(argv[++i]);
        else if (a == L"--follow-children")
            followChildren = true;
        else if (a == L"--timeout" && i + 1 < argc)
            watch.timeoutSeconds = _wtoi(argv[++i]);
        else if (a == L"--poll" && i + 1 < argc)
        {
            watch.pollMs = _wtoi(argv[++i]);
            followPollMs = watch.pollMs;
        }
        else if (a == L"--no-package")
            noPackage = true;
        else if (a == L"--package-debugger" && i + 1 < argc)
            packagePipe = argv[++i];
        // What the app model appends to a package debugger's command line.
        else if (!packagePipe.empty() && a == L"-p" && i + 1 < argc)
            debugPid = (DWORD)wcstoul(argv[++i], nullptr, 10);
        else if (!packagePipe.empty() && a == L"-tid" && i + 1 < argc)
            debugTid = (DWORD)wcstoul(argv[++i], nullptr, 10);
        else if (a == L"--once")
            watch.once = true;
        else if (a == L"--")
        {
            ++i;
            break;
        }
        // An option of ours with its value missing, or one we do not know: saying so beats
        // starting a program named "--watch".
        else if (a.rfind(L"--", 0) == 0)
        {
            Note(L"unknown or incomplete option %s", a.c_str());
            return Usage();
        }
        else
            break;
    }

    if (watching)
    {
        if (i < argc)
        {
            Note(L"--watch takes no command line: the application is started by something else");
            return Usage();
        }
        if (watch.wanted.empty())
            return Usage();
        if (dlls.empty())
        {
            Note(L"--watch needs --dll: there is nothing to inject");
            return kExitFailed;
        }
        watch.dlls = dlls;
        watch.follow = follow;
        watch.followChildren = followChildren;
        size_t slash = watch.wanted.find_last_of(L"\\/");
        watch.fullPath = slash != std::wstring::npos;
        watch.image = watch.fullPath ? watch.wanted.substr(slash + 1) : watch.wanted;
        // A path the user typed may hold forward slashes; the one a process reports never does.
        if (watch.fullPath)
            watch.wanted = Normalized(watch.wanted);
        if (watch.image.empty())
            return Usage();
        if (watch.pollMs < 1)
            watch.pollMs = 1;
        if (watch.pollMs > 1000)
            watch.pollMs = 1000;
        return Watch(watch);
    }

    if (!packagePipe.empty())
    {
        if (!debugPid)
            return Usage();
        return PackageDebugger(packagePipe, debugPid, debugTid);
    }

    if (i >= argc)
        return Usage();
    std::wstring exe = argv[i];
    std::wstring commandLine = Quote(exe);
    std::wstring arguments;
    for (int k = i + 1; k < argc; ++k)
    {
        commandLine += L' ';
        commandLine += Quote(argv[k]);
        arguments += (arguments.empty() ? L"" : L" ") + Quote(argv[k]);
    }

    if (!noPackage)
    {
        PackageApp app;
        std::wstring why;
        const int packaged = FindPackageApp(exe, app, why);
        if (packaged > 0)
            return LaunchPackage(app, exe, arguments, dlls, follow, followChildren, followPollMs);
        if (packaged < 0)
            Note(L"%s; starting %s directly", why.c_str(), exe.c_str());
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
            cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
    {
        Note(L"cannot start %s (error %lu)", exe.c_str(), GetLastError());
        return 1;
    }

    std::wstring why;
    std::vector<Library> libs = Resolve(dlls);
    if (dlls.empty())
    {
        Note(L"no --dll given: the target runs without the D3D12 capture library");
    }
    else if (!libs.empty() && !IsX64Image(exe, why))
    {
        Note(L"%s: %s; the target runs without the capture libraries", exe.c_str(), why.c_str());
        libs.clear();
    }
    else
    {
        for (const Library& lib : libs)
        {
            if (!Inject(pi.hProcess, pi.dwProcessId, lib.path, lib.offset, std::vector<wchar_t>(), nullptr, why))
            {
                Note(L"injecting %s failed: %s; the target runs without it", lib.path.c_str(), why.c_str());
            }
            else
            {
                Note(L"injected %s into pid %lu", lib.path.c_str(), pi.dwProcessId);
            }
        }
    }
    if (followChildren)
    {
        Note(L"following every child of pid %lu every %d ms", pi.dwProcessId, followPollMs);
    }
    else if (!follow.empty())
    {
        Note(L"following the children of pid %lu every %d ms, injecting into those whose command line matches",
            pi.dwProcessId, followPollMs);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    // A launched target inherited our environment, so its children have the library's settings
    // already and nothing has to be handed to their initializer.
    const DWORD code = WaitForTarget(pi.hProcess, pi.dwProcessId, libs, follow, followChildren,
        std::vector<wchar_t>(), (DWORD)(followPollMs > 0 ? followPollMs : (int)kFollowPollMs));
    CloseHandle(pi.hProcess);
    return (int)code;
}
