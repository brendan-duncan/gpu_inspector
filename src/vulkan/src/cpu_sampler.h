// What the CPU was doing in a frame: a sampling profiler inside the capture library, for timing
// captures (cpu_timeline.h). Shared by the Vulkan layer and the D3D12 library; there is no graphics
// API in it.
//
// A timing capture finds the hitch and says which of the calls it times the frame spent its time
// in. For most hitches that is none of them: "no timed call accounts for it: the application's own
// work between them", which is true and is where the answer stops. The tools that go on from
// there (PIX's timing captures, Nsight Systems) do it with a kernel trace — context switches and
// a sampled profile from ETW — which takes an elevated session and a second process to run it.
// This gets the same two facts from inside the process, with no privilege at all:
//
//   * which function each thread was in, by sampling: a few hundred times a second every thread
//     of the process is stopped for the few microseconds it takes to copy its registers and the
//     top of its stack, and the copy is walked afterwards into a call stack;
//   * whether it was running or waiting there, by asking the scheduler how many cycles the thread
//     has used since the sample before. A thread that used none was blocked, and its stack says on
//     what: a file read, a lock, a fence, the presenter.
//
// The second is also what keeps the first cheap enough to leave on. A thread that has used no
// cycles since it was last sampled is still in the wait it was found in, so its stack is already
// known and it is not stopped again: of the dozens of threads a game has, the ones stopped at any
// tick are the one or two that are running. Stopping all of them every tick is measurable — a
// driver's worker threads woken two hundred times a second to be suspended missed vertical blanks
// in testing — and a profiler that causes hitches is no use for finding them.
//
// Each sample is filed under the frame it fell in, so a hitch frame is a handful of stacks per
// thread, and the one the render thread spent 38 of its 40 milliseconds under is the answer.
//
// What it cannot see is what is outside the process: which other process took the core, what the
// GPU's hardware queue was doing. That is what a kernel trace is still for.
//
// The one rule in here is about the moment a thread is suspended. It may hold any lock in the
// process — the heap's, the loader's — so between SuspendThread and ResumeThread this code touches
// nothing that could want one: no allocation, no logging, no unwinding (RtlLookupFunctionEntry
// takes the loader's function table lock). It copies memory, and everything else happens after the
// thread is running again. Breaking that rule does not crash: it deadlocks the application, some
// of the time, which is worse.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32) && defined(_M_X64) && defined(_MSC_VER)
#define GPUINSP_CPU_SAMPLER 1
#include <windows.h>
#include <tlhelp32.h>
#else
#define GPUINSP_CPU_SAMPLER 0
#endif

namespace gpuinsp {

class CpuSampler {
public:
    /** What was recorded since the last Take. */
    struct Batch {
        struct Thread { uint32_t id = 0; std::string name; };
        struct Stack { uint32_t id = 0; std::vector<uint64_t> addresses; };
        /** `count` samples of thread `thread` (an index into `threads`) under stack `stack` in frame `frame`. */
        struct Sample { uint32_t frame = 0; uint32_t thread = 0; uint32_t stack = 0; bool running = false; uint32_t count = 0; };
        double periodMs = 0;
        /** Every thread seen so far, so an index stays good from one batch to the next. */
        std::vector<Thread> threads;
        /** Only the stacks that are new since the last batch. */
        std::vector<Stack> stacks;
        std::vector<Sample> samples;
        /** Samples lost to the stack table being full. */
        uint64_t dropped = 0;
    };

    static CpuSampler& Get() {
        static CpuSampler instance;
        return instance;
    }

    /** Whether this build can sample at all (Windows x64). */
    static bool Available() { return GPUINSP_CPU_SAMPLER != 0; }

    /** The calling thread is the library's own and is left out: its stack is never the application's answer. */
    void ExcludeCurrentThread() {
#if GPUINSP_CPU_SAMPLER
        std::lock_guard<std::mutex> lock(_mutex);
        _excluded.insert(GetCurrentThreadId());
#endif
    }

    /** Starts sampling at `hz` (clamped to 10..2000), discarding what an earlier run recorded. False where it cannot. */
    bool Start(uint32_t hz) {
#if GPUINSP_CPU_SAMPLER
        Stop();
        std::lock_guard<std::mutex> lock(_mutex);
        _periodMs = 1000.0 / (double)(hz < 10 ? 10 : hz > 2000 ? 2000 : hz);
        _threads.clear();
        _threadIndex.clear();
        _stackIds.clear();
        _stacks.clear();
        _stacksSent = 0;
        _counts.clear();
        _dropped = 0;
        _running.store(true, std::memory_order_release);
        _worker = std::thread([this] { Run(); });
        return true;
#else
        (void)hz;
        return false;
#endif
    }

    void Stop() {
#if GPUINSP_CPU_SAMPLER
        if (!_running.exchange(false, std::memory_order_acq_rel)) return;
        if (_worker.joinable()) _worker.join();
#endif
    }

    bool Running() const { return _running.load(std::memory_order_relaxed); }

    /** The frame that is starting: samples from here on are its. */
    void NoteFrame(uint32_t frame) { _frame.store(frame, std::memory_order_relaxed); }

    /** What was recorded since the last call; false when that is nothing. */
    bool Take(Batch& out) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_counts.empty() && _stacksSent >= _stacks.size()) return false;
        out.periodMs = _periodMs;
        out.threads.clear();
        for (const ThreadInfo& t : _threads) out.threads.push_back({t.id, t.name});
        out.stacks.clear();
        for (size_t i = _stacksSent; i < _stacks.size(); ++i) out.stacks.push_back({(uint32_t)i + 1, _stacks[i]});
        _stacksSent = _stacks.size();
        out.samples.clear();
        for (const auto& [key, count] : _counts) {
            Batch::Sample s;
            s.frame = std::get<0>(key);
            s.thread = std::get<1>(key);
            s.stack = std::get<2>(key);
            s.running = std::get<3>(key);
            s.count = count;
            out.samples.push_back(s);
        }
        _counts.clear();
        out.dropped = _dropped;
        return true;
    }

private:
    CpuSampler() = default;
    ~CpuSampler() { Stop(); }

    struct ThreadInfo {
        uint32_t id = 0;
        std::string name;
#if GPUINSP_CPU_SAMPLER
        HANDLE handle = nullptr;
        /** The thread's TEB, whose stack bounds are read while it is suspended (a fiber switch moves them). */
        const NT_TIB* tib = nullptr;
        uint64_t cycles = 0;
        /** The stack it was last found under, which still holds while it uses no cycles. */
        uint32_t lastStack = 0;
#endif
        bool gone = false;
    };

    std::mutex _mutex;
    std::atomic<bool> _running{false};
    std::atomic<uint32_t> _frame{0};
    std::thread _worker;
    double _periodMs = 4;
    std::unordered_set<uint32_t> _excluded;
    std::vector<ThreadInfo> _threads;
    std::unordered_map<uint32_t, uint32_t> _threadIndex;   // OS id -> index into _threads
    std::unordered_map<uint64_t, std::vector<uint32_t>> _stackIds;   // hash -> ids (1-based) with that hash
    std::vector<std::vector<uint64_t>> _stacks;
    size_t _stacksSent = 0;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, bool>, uint32_t> _counts;
    uint64_t _dropped = 0;

    /** Distinct stacks kept; past this the samples are counted as dropped rather than grown without bound. */
    static constexpr size_t kMaxStacks = 1u << 17;
    static constexpr size_t kMaxThreads = 64;
    static constexpr size_t kMaxFrames = 48;
    /** Cycles between two samples that mean the thread ran: well above what being sampled costs it, well below a period's worth. */
    static constexpr uint64_t kRunningCycles = 150000;
    /** How much of a stack is copied, from its top: deeper frames than this holds are left out. */
    static constexpr size_t kMaxCopy = 48 * 1024;

#if GPUINSP_CPU_SAMPLER
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

    /** ThreadBasicInformation: the TEB is the second pointer of it. */
    struct ThreadBasicInformation {
        LONG exitStatus;
        PVOID tebBaseAddress;
        PVOID clientId[2];
        ULONG_PTR affinityMask;
        LONG priority;
        LONG basePriority;
    };

    /**
     * The process's threads, opened for sampling: new ones added, exited ones marked. The snapshot
     * walks every thread of the system and takes milliseconds, so it is taken without the lock:
     * the application's own thread takes that lock when it collects a batch (Take), from inside
     * its frame, and a frame that waits milliseconds for a profiler is a hitch the profiler made.
     */
    void RefreshThreads(uint32_t self) {
        static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        static const auto describe = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription"));
        if (!query) return;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        const DWORD pid = GetCurrentProcessId();
        std::unordered_set<uint32_t> alive;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
            if (entry.th32OwnerProcessID == pid) alive.insert(entry.th32ThreadID);
        CloseHandle(snapshot);

        // Known threads are only ever changed by this thread, so they can be read without the lock.
        std::vector<ThreadInfo> found;
        for (const uint32_t id : alive) {
            bool skip = id == self || _threadIndex.count(id) != 0 || _threads.size() + found.size() >= kMaxThreads;
            if (!skip) {
                std::lock_guard<std::mutex> lock(_mutex);
                skip = _excluded.count(id) != 0;
            }
            if (skip) continue;
            const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
            if (!handle) continue;
            ThreadBasicInformation basic{};
            if (query(handle, 0, &basic, sizeof(basic), nullptr) < 0 || !basic.tebBaseAddress) {
                CloseHandle(handle);
                continue;
            }
            ThreadInfo t;
            t.id = id;
            t.handle = handle;
            t.tib = static_cast<const NT_TIB*>(basic.tebBaseAddress);
            QueryThreadCycleTime(handle, &t.cycles);
            PWSTR wide = nullptr;
            if (describe && SUCCEEDED(describe(handle, &wide)) && wide) {
                const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    t.name.resize((size_t)n - 1);
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, t.name.data(), n, nullptr, nullptr);
                }
                LocalFree(wide);
            }
            found.push_back(std::move(t));
        }
        std::lock_guard<std::mutex> lock(_mutex);
        for (ThreadInfo& t : found) {
            _threadIndex[t.id] = (uint32_t)_threads.size();
            _threads.push_back(std::move(t));
        }
        for (ThreadInfo& t : _threads) {
            if (t.gone || alive.count(t.id)) continue;
            t.gone = true;
            CloseHandle(t.handle);
            t.handle = nullptr;
        }
    }

    /**
     * Walks a copy of a stack. `context` is the thread's, `copy` holds the `copied` bytes that were
     * at `original`; addresses into that range, in the stack pointer and in the registers the
     * unwind restores, are moved into the copy as they appear. Plain data only, so that the walk
     * can sit in a __try: a frame that runs past what was copied reads past the buffer.
     */
    static size_t Unwind(CONTEXT context, const uint8_t* copy, size_t copied, uint64_t original, uint64_t* out, size_t capacity) {
        size_t count = 0;
        const uint64_t low = reinterpret_cast<uint64_t>(copy), high = low + copied;
        const int64_t delta = (int64_t)low - (int64_t)original;
        auto move = [&](DWORD64& r) { if (r >= original && r < original + copied) r = (DWORD64)((int64_t)r + delta); };
        auto moveAll = [&](CONTEXT& c) {
            move(c.Rsp); move(c.Rbp); move(c.Rbx); move(c.Rsi); move(c.Rdi);
            move(c.R12); move(c.R13); move(c.R14); move(c.R15);
        };
        __try {
            if (capacity) out[count++] = context.Rip;
            moveAll(context);
            while (count < capacity) {
                if (context.Rsp < low || context.Rsp + sizeof(uint64_t) > high) break;
                DWORD64 imageBase = 0;
                const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!function) {
                    // A leaf: its return address is at the top of the stack.
                    std::memcpy(&context.Rip, reinterpret_cast<const void*>(context.Rsp), sizeof(uint64_t));
                    context.Rsp += sizeof(uint64_t);
                } else {
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
                    moveAll(context);
                }
                if (!context.Rip) break;
                out[count++] = context.Rip;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // What was walked before the fault stands.
        }
        return count;
    }

    void Run() {
        const uint32_t self = GetCurrentThreadId();
        // A finer timer for the life of the capture: the default 15.6 ms tick would make 250 Hz 64.
        const HMODULE winmm = LoadLibraryW(L"winmm.dll");
        using PeriodFn = UINT(WINAPI*)(UINT);
        const auto beginPeriod = winmm ? reinterpret_cast<PeriodFn>(GetProcAddress(winmm, "timeBeginPeriod")) : nullptr;
        const auto endPeriod = winmm ? reinterpret_cast<PeriodFn>(GetProcAddress(winmm, "timeEndPeriod")) : nullptr;
        if (beginPeriod) beginPeriod(1);

        std::vector<uint8_t> copy(kMaxCopy + 4096, 0);   // and a page of zeros after it for a walk that overruns a little
        uint64_t frames[kMaxFrames];
        LARGE_INTEGER frequency, next;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&next);
        uint32_t tick = 0;
        std::vector<ThreadInfo*> targets;
        targets.reserve(kMaxThreads);

        while (_running.load(std::memory_order_relaxed)) {
            const double periodMs = _periodMs;   // set before this thread started
            // Twice a second: a snapshot walks every thread of the system.
            if (tick++ % (uint32_t)(500.0 / periodMs + 1) == 0) RefreshThreads(self);
            // Only this thread adds to the list or marks a thread gone, so it reads it as it is.
            targets.clear();
            for (ThreadInfo& t : _threads) if (!t.gone) targets.push_back(&t);
            const uint32_t frame = _frame.load(std::memory_order_relaxed);
            for (ThreadInfo* t : targets) {
                // Whether it ran since the sample before: a thread that used no cycles was waiting.
                // None to speak of, that is: being stopped for the last sample woke it for the few
                // thousand cycles a suspend costs, which a thread that really ran spends in microseconds.
                ULONG64 cycles = 0;
                const bool ran = QueryThreadCycleTime(t->handle, &cycles) && cycles - t->cycles > kRunningCycles;
                t->cycles = cycles;
                // Still where it was: the wait it was last found in, which needs no stopping it to know.
                if (!ran && t->lastStack) {
                    Count(frame, *t, t->lastStack, false);
                    continue;
                }

                CONTEXT context;
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                size_t copied = 0;
                uint64_t original = 0;
                bool ok = false;
                if (SuspendThread(t->handle) != (DWORD)-1) {
                    // ---- The thread is stopped and may hold any lock: memory is copied, and nothing else.
                    if (GetThreadContext(t->handle, &context)) {
                        const uint64_t base = reinterpret_cast<uint64_t>(t->tib->StackBase);
                        const uint64_t limit = reinterpret_cast<uint64_t>(t->tib->StackLimit);
                        original = context.Rsp;
                        if (original >= limit && original < base) {
                            copied = (size_t)(base - original < kMaxCopy ? base - original : kMaxCopy);
                            std::memcpy(copy.data(), reinterpret_cast<const void*>(original), copied);
                            ok = true;
                        }
                    }
                    ResumeThread(t->handle);
                    // ---- Running again.
                }
                if (!ok) continue;
                const size_t depth = Unwind(context, copy.data(), copied, original, frames, kMaxFrames);
                if (!depth) continue;
                t->lastStack = Record(frame, *t, frames, depth, ran);
                // Being stopped cost it cycles of its own, which are not work it did.
                QueryThreadCycleTime(t->handle, &cycles);
                t->cycles = cycles;
            }
            next.QuadPart += (LONGLONG)((double)frequency.QuadPart * periodMs / 1000.0);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (next.QuadPart <= now.QuadPart) {
                next = now;   // fell behind (many threads, or a stall): no catching up in a burst
            } else {
                const DWORD ms = (DWORD)((next.QuadPart - now.QuadPart) * 1000 / frequency.QuadPart);
                Sleep(ms ? ms : 1);
            }
        }
        if (endPeriod) endPeriod(1);
        std::lock_guard<std::mutex> lock(_mutex);
        for (ThreadInfo& t : _threads) {
            if (t.handle) CloseHandle(t.handle);
            t.handle = nullptr;
        }
    }

    void Count(uint32_t frame, const ThreadInfo& thread, uint32_t stack, bool running) {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _threadIndex.find(thread.id);
        if (it != _threadIndex.end()) ++_counts[{frame, it->second, stack, running}];
    }

    /** Files a sample under its stack, interning the stack; the stack's id, or 0 when the table is full. */
    uint32_t Record(uint32_t frame, const ThreadInfo& thread, const uint64_t* frames, size_t depth, bool running) {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < depth; ++i) hash = (hash ^ frames[i]) * 1099511628211ull;
        std::lock_guard<std::mutex> lock(_mutex);
        uint32_t id = 0;
        std::vector<uint32_t>& candidates = _stackIds[hash];
        for (uint32_t candidate : candidates) {
            const std::vector<uint64_t>& s = _stacks[candidate - 1];
            if (s.size() == depth && std::memcmp(s.data(), frames, depth * sizeof(uint64_t)) == 0) {
                id = candidate;
                break;
            }
        }
        if (!id) {
            if (_stacks.size() >= kMaxStacks) {
                ++_dropped;
                return 0;
            }
            _stacks.emplace_back(frames, frames + depth);
            id = (uint32_t)_stacks.size();
            candidates.push_back(id);
        }
        const auto it = _threadIndex.find(thread.id);
        if (it != _threadIndex.end()) ++_counts[{frame, it->second, id, running}];
        return id;
    }
#endif
};

}  // namespace gpuinsp
