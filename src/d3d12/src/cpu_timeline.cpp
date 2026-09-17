#include "cpu_timeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "capture.h"
#include "device_info.h"
#include "tracker.h"
#include "hook.h"
#include "json.h"
#include "transport.h"

namespace dxinsp {

namespace {

const char* const kCpuCategoryNames[(size_t)CpuCategory::Count] = {
    "submit", "present", "waitFences", "acquire", "pipeline",
};

/** One timed call. Kept small: a busy frame records a few hundred of these. */
struct CpuEvent {
    uint64_t startNs = 0;      // steady_clock nanoseconds since the capture's origin
    uint32_t durationNs = 0;   // a single call over four seconds is not worth recording exactly
    uint32_t thread = 0;       // index into the timeline's thread list, not an OS id
    uint16_t category = 0;
    uint32_t frame = 0;
};

/** How many events one capture keeps. A frame's worth is tens; this covers a long multi-frame capture. */
constexpr size_t kMaxEvents = 1u << 16;

std::mutex g_mutex;
std::vector<CpuEvent> g_events;
std::chrono::steady_clock::time_point g_origin{};
bool g_running = false;
/** Set while a capture wants events, read without the lock on the hot path. */
std::atomic<bool> g_recording{false};
size_t g_dropped = 0;

std::vector<uint32_t> g_threadIds;

/** The GPU-to-host relation sampled at the end of the capture (see SampleCalibration). */
bool g_calibrated = false;
uint64_t g_deviceTicks = 0;
double g_hostMs = 0;          // host time of that instant, relative to the origin
double g_timestampPeriod = 0; // nanoseconds per GPU tick

/** The index of the calling thread in the report's list; assigned on first sight. Under g_mutex. */
uint32_t ThreadIndex() {
    const uint32_t id = GetCurrentThreadId();
    for (size_t i = 0; i < g_threadIds.size(); ++i) {
        if (g_threadIds[i] == id) return (uint32_t)i;
    }
    g_threadIds.push_back(id);
    return (uint32_t)g_threadIds.size() - 1;
}

// ---------------------------------------------------------------------------------------------
// The handles worth timing a wait on.
//
// A fixed, lock-free table rather than a set behind a mutex. These are read from inside
// WaitForSingleObject, which the runtime, the driver and the application's own thread pools all
// call; taking a lock there could deadlock against whatever else holds one while waiting, and no
// diagnostic is worth that. An application has a handful of fence events — one or two per frame in
// flight — so a small table with a linear scan is both enough and faster than hashing.

constexpr size_t kMaxWaitHandles = 64;

struct WaitHandleTable {
    std::atomic<void*> handles[kMaxWaitHandles]{};

    void Add(HANDLE h) {
        if (!h) return;
        for (size_t i = 0; i < kMaxWaitHandles; ++i) {
            void* expected = handles[i].load(std::memory_order_relaxed);
            if (expected == h) return;   // already known
            if (expected) continue;
            if (handles[i].compare_exchange_strong(expected, h, std::memory_order_relaxed)) return;
            if (handles[i].load(std::memory_order_relaxed) == h) return;   // lost the race to the same handle
        }
        // Full: the application keeps more of these than expected. The waits on the ones already
        // known are still timed, which is the common case.
    }

    bool Has(HANDLE h) const {
        if (!h) return false;
        for (size_t i = 0; i < kMaxWaitHandles; ++i) {
            void* v = handles[i].load(std::memory_order_relaxed);
            if (!v) return false;   // entries are only ever appended, so the first empty slot ends it
            if (v == h) return true;
        }
        return false;
    }
};

WaitHandleTable g_fenceEvents;
WaitHandleTable g_frameLatencyEvents;

/** Which category a wait on this handle belongs to, or Count when it is not one of ours. */
CpuCategory WaitCategory(HANDLE h) {
    if (g_fenceEvents.Has(h)) return CpuCategory::WaitFences;
    if (g_frameLatencyEvents.Has(h)) return CpuCategory::Acquire;
    return CpuCategory::Count;
}

using PFN_WaitForSingleObject = DWORD(WINAPI*)(HANDLE, DWORD);
using PFN_WaitForSingleObjectEx = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
using PFN_WaitForMultipleObjectsEx = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);

PFN_WaitForSingleObject g_WaitForSingleObject = nullptr;
PFN_WaitForSingleObjectEx g_WaitForSingleObjectEx = nullptr;
PFN_WaitForMultipleObjectsEx g_WaitForMultipleObjectsEx = nullptr;

/**
 * Whether this thread is already inside a timed wait. Both wait functions are hooked because which
 * one an application reaches is a Windows implementation detail — WaitForSingleObject forwards to
 * the Ex form on some builds and goes straight to the kernel on others — and hooking only one is
 * how a wait goes unseen. When it does forward, this keeps the inner call from being recorded a
 * second time inside the outer one.
 */
thread_local int t_inWait = 0;

struct WaitScope {
    bool timing;
    explicit WaitScope(bool wanted) : timing(wanted && t_inWait == 0) { if (timing) ++t_inWait; }
    ~WaitScope() { if (timing) --t_inWait; }
};

DWORD WINAPI Hook_WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    // The fast path for every wait in the process that is not ours: one relaxed load.
    if (!g_recording.load(std::memory_order_relaxed)) return g_WaitForSingleObject(handle, milliseconds);
    const CpuCategory category = WaitCategory(handle);
    WaitScope scope(category != CpuCategory::Count);
    if (!scope.timing) return g_WaitForSingleObject(handle, milliseconds);
    const uint64_t started = CpuEventBegin();
    const DWORD result = g_WaitForSingleObject(handle, milliseconds);
    CpuEventEnd(nullptr, started, category);
    return result;
}

DWORD WINAPI Hook_WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable) {
    if (!g_recording.load(std::memory_order_relaxed)) return g_WaitForSingleObjectEx(handle, milliseconds, alertable);
    const CpuCategory category = WaitCategory(handle);
    WaitScope scope(category != CpuCategory::Count);
    if (!scope.timing) return g_WaitForSingleObjectEx(handle, milliseconds, alertable);
    const uint64_t started = CpuEventBegin();
    const DWORD result = g_WaitForSingleObjectEx(handle, milliseconds, alertable);
    CpuEventEnd(nullptr, started, category);
    return result;
}

DWORD WINAPI Hook_WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD milliseconds, BOOL alertable) {
    if (!g_recording.load(std::memory_order_relaxed) || !handles) {
        return g_WaitForMultipleObjectsEx(count, handles, waitAll, milliseconds, alertable);
    }
    // A wait on several handles is ours if any of them is: an application waiting on its fence and
    // something else at once is still waiting for the GPU for as long as the call takes.
    CpuCategory category = CpuCategory::Count;
    for (DWORD i = 0; i < count && category == CpuCategory::Count; ++i) category = WaitCategory(handles[i]);
    WaitScope scope(category != CpuCategory::Count);
    if (!scope.timing) return g_WaitForMultipleObjectsEx(count, handles, waitAll, milliseconds, alertable);
    const uint64_t started = CpuEventBegin();
    const DWORD result = g_WaitForMultipleObjectsEx(count, handles, waitAll, milliseconds, alertable);
    CpuEventEnd(nullptr, started, category);
    return result;
}

}  // namespace

void NoteFenceEvent(HANDLE event) { g_fenceEvents.Add(event); }
void NoteFrameLatencyEvent(HANDLE event) { g_frameLatencyEvents.Add(event); }

void InstallWaitHooks() {
    // Both forms, since which one reaches the kernel is a Windows implementation detail (see
    // WaitScope). KernelBase is where they really live; kernel32 only forwards, and GetProcAddress
    // resolves the forwarder either way.
    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    if (!kernel) kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    auto plain = (void*)GetProcAddress(kernel, "WaitForSingleObject");
    auto single = (void*)GetProcAddress(kernel, "WaitForSingleObjectEx");
    auto multiple = (void*)GetProcAddress(kernel, "WaitForMultipleObjectsEx");
    if (plain) HookFunction(plain, (void*)&Hook_WaitForSingleObject, (void**)&g_WaitForSingleObject, "WaitForSingleObject");
    if (single) HookFunction(single, (void*)&Hook_WaitForSingleObjectEx, (void**)&g_WaitForSingleObjectEx, "WaitForSingleObjectEx");
    if (multiple) HookFunction(multiple, (void*)&Hook_WaitForMultipleObjectsEx, (void**)&g_WaitForMultipleObjectsEx, "WaitForMultipleObjectsEx");
    Log("wait hooks: WaitForSingleObject%s, WaitForSingleObjectEx%s, WaitForMultipleObjectsEx%s",
        plain ? "" : " (absent)", single ? "" : " (absent)", multiple ? "" : " (absent)");
}

uint64_t CpuEventBegin() {
    if (!g_recording.load(std::memory_order_relaxed)) return 0;
    return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
}

void CpuEventEnd(ID3D12Device* /*device*/, uint64_t started, CpuCategory category) {
    if (!started || !g_recording.load(std::memory_order_relaxed)) return;
    const uint64_t now = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) return;
    const uint64_t originNs = (uint64_t)g_origin.time_since_epoch().count();
    if (started < originNs) return;   // began before the capture did
    if (g_events.size() >= kMaxEvents) {
        ++g_dropped;
        return;
    }
    CpuEvent e;
    e.startNs = started - originNs;
    e.durationNs = (uint32_t)std::min<uint64_t>(now > started ? now - started : 0, UINT32_MAX);
    e.thread = ThreadIndex();
    e.category = (uint16_t)category;
    e.frame = (uint32_t)CaptureManager::Get().FrameCounter();
    g_events.push_back(e);
}

void BeginCpuTimeline() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_events.clear();
    g_threadIds.clear();
    g_dropped = 0;
    g_calibrated = false;
    g_origin = std::chrono::steady_clock::now();
    g_running = true;
    g_recording.store(true, std::memory_order_relaxed);
}

bool SampleCalibration(ID3D12CommandQueue* queue) {
    if (!queue) return false;
    ScopedInternal internal;
    UINT64 frequency = 0;
    if (FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency) return false;
    UINT64 gpuTicks = 0, cpuTicks = 0;
    if (FAILED(queue->GetClockCalibration(&gpuTicks, &cpuTicks))) return false;

    // The CPU half comes back in QueryPerformanceCounter units; the events are on steady_clock.
    // Reading both around the call brackets the instant closely enough — the two reads are
    // microseconds apart — which is the same approach the Vulkan layer takes for its host domain.
    const auto host = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_deviceTicks = gpuTicks;
    g_hostMs = std::chrono::duration<double, std::milli>(host - g_origin).count();
    g_timestampPeriod = 1e9 / (double)frequency;   // nanoseconds per GPU tick
    g_calibrated = true;
    return true;
}

void SendCpuTimeline() {
    std::vector<CpuEvent> events;
    std::vector<uint32_t> threads;
    size_t dropped = 0;
    bool calibrated = false;
    uint64_t deviceTicks = 0;
    double hostMs = 0, period = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_recording.store(false, std::memory_order_relaxed);
        g_running = false;
        events.swap(g_events);
        threads = g_threadIds;
        dropped = g_dropped;
        calibrated = g_calibrated;
        deviceTicks = g_deviceTicks;
        hostMs = g_hostMs;
        period = g_timestampPeriod;
    }
    if (events.empty()) return;
    // In time order, so a reader can draw them without sorting.
    std::sort(events.begin(), events.end(), [](const CpuEvent& a, const CpuEvent& b) { return a.startNs < b.startNs; });

    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureCpuTimeline");
    w.Key("threads"); w.BeginArray();
    for (uint32_t id : threads) w.Uint(id);
    w.EndArray();
    if (dropped) { w.Key("dropped"); w.Uint(dropped); }
    // How to place a GPU timestamp on this axis: hostMs + (ticks - deviceTicks) * period / 1e6.
    if (calibrated) {
        w.Key("calibration"); w.BeginObject();
        w.Key("deviceTicks"); w.Uint(deviceTicks);
        w.Key("hostMs"); w.Double(hostMs);
        w.Key("timestampPeriod"); w.Double(period);
        w.EndObject();
    }
    w.Key("events"); w.BeginArray();
    for (const CpuEvent& e : events) {
        w.BeginObject();
        w.Key("thread"); w.Uint(e.thread);
        w.Key("category"); w.String(kCpuCategoryNames[e.category < (uint16_t)CpuCategory::Count ? e.category : 0]);
        w.Key("frame"); w.Uint(e.frame);
        w.Key("startMs"); w.Double((double)e.startNs / 1e6);
        w.Key("durationMs"); w.Double((double)e.durationNs / 1e6);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}


// ---------------------------------------------------------------------------------------------
// Memory residency (QueryVideoMemoryInfo)

namespace {

/** The two segments an adapter reports, in the order the memory view lists them. */
struct Segment {
    const char* name;
    DXGI_MEMORY_SEGMENT_GROUP group;
    bool deviceLocal;
};

constexpr Segment kSegments[2] = {
    {"Device local", DXGI_MEMORY_SEGMENT_GROUP_LOCAL, true},
    {"System (shared)", DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, false},
};

/** The heap types an application allocates from, and which segment each draws on. */
struct HeapTypeInfo {
    const char* name;
    D3D12_HEAP_TYPE type;
    uint32_t segment;   // index into kSegments
};

// UPLOAD and READBACK are system memory the GPU reads over the bus; DEFAULT is the GPU's own. On a
// UMA adapter there is one pool behind both, which the features update already reports (ARCHITECTURE1).
constexpr HeapTypeInfo kHeapTypes[3] = {
    {"DEFAULT", D3D12_HEAP_TYPE_DEFAULT, 0},
    {"UPLOAD", D3D12_HEAP_TYPE_UPLOAD, 1},
    {"READBACK", D3D12_HEAP_TYPE_READBACK, 1},
};

/** The adapter's id in the object graph, or 0 when it is not tracked. */
uint64_t AdapterId(IDXGIAdapter* adapter) {
    return adapter ? Tracker::Get().IdOf(adapter) : 0;
}

ComPtr<IDXGIAdapter3> AdapterWithBudget(ID3D12Device* device, uint64_t* idOut) {
    IDXGIAdapter* adapter = AdapterOf(device);
    if (idOut) *idOut = AdapterId(adapter);
    ComPtr<IDXGIAdapter3> adapter3;
    if (adapter) adapter->QueryInterface(IID_PPV_ARGS(adapter3.put()));
    return adapter3;
}

}  // namespace

void SendMemoryProperties(ID3D12Device* device, IDXGIAdapter* adapter) {
    if (!device || !adapter) return;
    const uint64_t id = AdapterId(adapter);
    if (!id) return;
    ScopedInternal internal;

    // The segment sizes come from the adapter description: what the GPU has of its own, and what it
    // may reach in system memory.
    uint64_t sizes[2] = {0, 0};
    ComPtr<IDXGIAdapter1> adapter1;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(adapter1.put()))) && SUCCEEDED(adapter1->GetDesc1(&desc))) {
        sizes[0] = desc.DedicatedVideoMemory;
        sizes[1] = desc.SharedSystemMemory;
    }

    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("memoryProperties"); w.BeginObject();
    w.Key("memoryHeapCount"); w.Uint(2);
    w.Key("memoryTypeCount"); w.Uint((uint64_t)std::size(kHeapTypes));
    w.Key("memoryHeaps"); w.BeginArray();
    for (const Segment& s : kSegments) {
        w.BeginObject();
        w.Key("size"); w.Uint(sizes[s.deviceLocal ? 0 : 1]);
        w.Key("name"); w.String(s.name);
        w.Key("flags"); w.String(s.deviceLocal ? "DEVICE_LOCAL" : "0");
        w.EndObject();
    }
    w.EndArray();
    w.Key("memoryTypes"); w.BeginArray();
    for (const HeapTypeInfo& t : kHeapTypes) {
        w.BeginObject();
        w.Key("heapIndex"); w.Uint(t.segment);
        w.Key("propertyFlags"); w.String(t.name);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndObject();
    Tracker::Get().UpdateById(id, "memoryProperties", w.str());
}

void SendMemoryBudget(ID3D12Device* device) {
    if (!device) return;
    uint64_t id = 0;
    ScopedInternal internal;
    ComPtr<IDXGIAdapter3> adapter = AdapterWithBudget(device, &id);
    if (!adapter || !id) return;

    uint64_t budget[2] = {0, 0}, usage[2] = {0, 0};
    bool any = false;
    for (size_t i = 0; i < std::size(kSegments); ++i) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter->QueryVideoMemoryInfo(0, kSegments[i].group, &info))) continue;
        budget[i] = info.Budget;
        usage[i] = info.CurrentUsage;
        any = true;
    }
    if (!any) return;

    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("memoryBudget"); w.BeginObject();
    // What the driver will let this process have, and what is resident from every process.
    w.Key("heapBudget"); w.BeginArray();
    for (uint64_t v : budget) w.Uint(v);
    w.EndArray();
    w.Key("heapUsage"); w.BeginArray();
    for (uint64_t v : usage) w.Uint(v);
    w.EndArray();
    w.EndObject();
    w.EndObject();
    Tracker::Get().UpdateById(id, "memoryBudget", w.str());
}

/** A committed resource's implicit heap in the running total; declared here, defined below. */
void NoteHeldAllocation(void* object, uint64_t sizeBytes, D3D12_HEAP_TYPE heapType);

void NoteCommittedAllocation(ID3D12Device* device, ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc,
                             D3D12_HEAP_TYPE heapType) {
    if (!device || !resource) return;
    const uint64_t id = Tracker::Get().IdOf(resource);
    if (!id) return;
    uint64_t bytes = 0;
    {
        ScopedInternal internal;
        // A reserved (tiled) resource has no heap and holds nothing until tiles are mapped, so
        // its full size is not memory the application has taken. The runtime marks it by refusing
        // to answer for heap properties, which is a surer test than the creation call's arguments.
        D3D12_HEAP_PROPERTIES props{};
        D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
        if (FAILED(resource->GetHeapProperties(&props, &flags))) return;
        // What the runtime actually reserves, which for a texture is more than width x height x
        // format: alignment, mip padding and the swizzle the driver wants are all in here.
        const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &desc);
        if (info.SizeInBytes == UINT64_MAX) return;   // the runtime rejected the description
        bytes = info.SizeInBytes;
    }
    uint32_t type = 0;
    for (uint32_t i = 0; i < (uint32_t)std::size(kHeapTypes); ++i)
        if (kHeapTypes[i].type == heapType) type = i;

    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("allocation"); w.BeginObject();
    w.Key("sizeBytes"); w.Uint(bytes);
    w.Key("heapTypeIndex"); w.Uint(type);
    w.EndObject();
    w.EndObject();
    Tracker::Get().UpdateById(id, "allocation", w.str());
    // The same bytes in the running total behind the memory series (NoteHeapAllocation).
    NoteHeldAllocation(resource, bytes, heapType);
}


// ---------------------------------------------------------------------------------------------
// Memory over time

namespace {

std::mutex g_memoryMutex;
std::unordered_map<void*, std::pair<uint64_t, uint32_t>> g_held;   // object -> (bytes, segment)
uint64_t g_segmentBytes[2] = {0, 0};
uint32_t g_segmentCount[2] = {0, 0};

/** Which segment a heap type draws on: DEFAULT is the GPU's own, UPLOAD and READBACK system memory. */
uint32_t SegmentOf(D3D12_HEAP_TYPE type) {
    return type == D3D12_HEAP_TYPE_DEFAULT ? 0u : 1u;
}

void AddHeld(void* object, uint64_t bytes, uint32_t segment) {
    if (!object || segment >= 2) return;
    std::lock_guard<std::mutex> lock(g_memoryMutex);
    auto it = g_held.find(object);
    // A pointer the allocator has handed out again without the release being seen: the old entry
    // would otherwise be counted twice.
    if (it != g_held.end()) {
        g_segmentBytes[it->second.second] -= (std::min)(g_segmentBytes[it->second.second], it->second.first);
        if (g_segmentCount[it->second.second]) --g_segmentCount[it->second.second];
    }
    g_held[object] = {bytes, segment};
    g_segmentBytes[segment] += bytes;
    ++g_segmentCount[segment];
}

}  // namespace

void NoteHeapAllocation(ID3D12Heap* heap, uint64_t sizeBytes, D3D12_HEAP_TYPE heapType) {
    AddHeld(heap, sizeBytes, SegmentOf(heapType));
}

void NoteHeldAllocation(void* object, uint64_t sizeBytes, D3D12_HEAP_TYPE heapType) {
    AddHeld(object, sizeBytes, SegmentOf(heapType));
}

void NoteMemoryReleased(void* object) {
    if (!object) return;
    std::lock_guard<std::mutex> lock(g_memoryMutex);
    auto it = g_held.find(object);
    if (it == g_held.end()) return;
    const uint32_t segment = it->second.second;
    if (segment < 2) {
        g_segmentBytes[segment] -= (std::min)(g_segmentBytes[segment], it->second.first);
        if (g_segmentCount[segment]) --g_segmentCount[segment];
    }
    g_held.erase(it);
}

void SendMemorySample(ID3D12Device* device) {
    if (!device) return;
    uint64_t budget[2] = {0, 0}, usage[2] = {0, 0};
    bool hasBudget = false;
    {
        ScopedInternal internal;
        uint64_t id = 0;
        ComPtr<IDXGIAdapter3> adapter = AdapterWithBudget(device, &id);
        if (adapter) {
            for (size_t i = 0; i < std::size(kSegments); ++i) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (FAILED(adapter->QueryVideoMemoryInfo(0, kSegments[i].group, &info))) continue;
                budget[i] = info.Budget;
                usage[i] = info.CurrentUsage;
                hasBudget = true;
            }
        }
    }

    uint64_t bytes[2];
    uint32_t counts[2];
    {
        std::lock_guard<std::mutex> lock(g_memoryMutex);
        bytes[0] = g_segmentBytes[0];
        bytes[1] = g_segmentBytes[1];
        counts[0] = g_segmentCount[0];
        counts[1] = g_segmentCount[1];
    }

    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("MemorySample");
    w.Key("frame"); w.Uint(CaptureManager::Get().FrameCounter());
    w.Key("heaps"); w.BeginArray();
    for (size_t i = 0; i < 2; ++i) {
        w.BeginObject();
        w.Key("allocated"); w.Uint(bytes[i]);
        w.Key("allocations"); w.Uint(counts[i]);
        if (hasBudget) {
            w.Key("usage"); w.Uint(usage[i]);
            w.Key("budget"); w.Uint(budget[i]);
        }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

}  // namespace dxinsp
