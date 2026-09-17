#include "cpu_timeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "capture.h"
#include "json_writer.h"
#include "layer.h"
#include "resources.h"
#include "tracker.h"
#include "transport.h"

namespace vkinsp {

const char* const kCpuCategoryNames[(size_t)CpuCategory::Count] = {
    "submit", "present", "waitFences", "acquire", "waitIdle", "pipeline",
};

namespace {

/** One timed call. Kept small: a busy frame records a few hundred of these. */
struct CpuEvent {
    uint64_t startNs = 0;      // steady_clock nanoseconds since the capture's origin
    uint32_t durationNs = 0;   // a single call over four seconds is not a thing worth recording exactly
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

// Timing captures (cpu_timeline.h). `g_timing` is read on the hot path beside g_recording; the
// clock is read when either wants it.
std::atomic<bool> g_timing{false};
/** The frame being accumulated: category totals in milliseconds, rolled up at the frame boundary. */
double g_frameCategoryMs[(size_t)CpuCategory::Count] = {};
std::vector<FrameTiming> g_frames;
/** How many of `g_frames` have been sent, so each report carries only what is new. */
size_t g_framesSent = 0;
/** About twenty minutes at 60 Hz, after which the oldest are dropped. */
constexpr size_t kMaxFrames = 72000;

/** Stable small indices for the threads that made calls, with their OS ids for the report. */
std::unordered_map<std::thread::id, uint32_t> g_threadIndex;
std::vector<uint64_t> g_threadIds;

/** The GPU-to-host relation sampled at the end of the capture (see SampleCalibration). */
bool g_calibrated = false;
uint64_t g_deviceTicks = 0;
double g_hostMs = 0;          // host time of that instant, relative to the origin
double g_timestampPeriod = 0; // nanoseconds per GPU tick

uint32_t ThreadIndex() {
    const std::thread::id id = std::this_thread::get_id();
    auto it = g_threadIndex.find(id);
    if (it != g_threadIndex.end()) return it->second;
    const uint32_t index = (uint32_t)g_threadIds.size();
    g_threadIndex.emplace(id, index);
    uint64_t osId = 0;
    static_assert(sizeof(std::thread::id) <= sizeof(uint64_t), "thread id does not fit");
    std::memcpy(&osId, &id, sizeof(id));
    g_threadIds.push_back(osId);
    return index;
}

} // namespace

void PlanCpuTimeline(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, CpuTimelineSetup& setup) {
    if (!inst || !inst->dispatch.EnumerateDeviceExtensionProperties) return;
    uint32_t count = 0;
    inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    if (count) inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data());
    auto offers = [&](const char* name) {
        return std::any_of(available.begin(), available.end(),
                           [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    // Either spelling will do; the entry points are the same shape.
    const char* name = offers(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                     : offers(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ? VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                     : nullptr;
    if (!name) return;
    setup.extensionNames.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
    const bool already = std::any_of(setup.extensionNames.begin(), setup.extensionNames.end(),
                                     [&](const char* e) { return std::strcmp(e, name) == 0; });
    if (!already) setup.extensionNames.push_back(name);
    info.ppEnabledExtensionNames = setup.extensionNames.data();
    info.enabledExtensionCount = (uint32_t)setup.extensionNames.size();
    setup.calibrated = true;
    setup.added = !already;
}

void InitCpuTimeline(DeviceData* dev, const CpuTimelineSetup& setup) {
    dev->calibratedTimestamps = setup.calibrated;
    dev->memoryBudget = setup.memoryBudget;
}

uint64_t CpuEventBegin() {
    // Either a capture (which keeps every call) or a timing capture (which keeps per-frame totals)
    // needs the clock; neither means this costs one relaxed read and nothing else.
    if (!g_recording.load(std::memory_order_relaxed) && !g_timing.load(std::memory_order_relaxed)) return 0;
    return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
}

void CpuEventEnd(DeviceData* dev, uint64_t started, CpuCategory category) {
    if (!started) return;
    const bool recording = g_recording.load(std::memory_order_relaxed);
    const bool timing = g_timing.load(std::memory_order_relaxed);
    if (!recording && !timing) return;
    const uint64_t now = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    const uint64_t durationNs = now > started ? now - started : 0;
    std::lock_guard lock(g_mutex);
    // The frame's running total, which is all a timing capture keeps of an individual call.
    if (timing && (size_t)category < (size_t)CpuCategory::Count) {
        g_frameCategoryMs[(size_t)category] += (double)durationNs / 1e6;
    }
    if (!recording || !g_running) return;
    const uint64_t originNs = (uint64_t)g_origin.time_since_epoch().count();
    if (started < originNs) return;   // began before the capture did
    if (g_events.size() >= kMaxEvents) {
        ++g_dropped;
        return;
    }
    CpuEvent e;
    e.startNs = started - originNs;
    e.durationNs = (uint32_t)std::min<uint64_t>(durationNs, UINT32_MAX);
    e.thread = ThreadIndex();
    e.category = (uint16_t)category;
    e.frame = dev ? (uint32_t)dev->frameIndex : 0;
    g_events.push_back(e);
}

void BeginTimingCapture() {
    std::lock_guard lock(g_mutex);
    g_frames.clear();
    g_framesSent = 0;
    for (double& v : g_frameCategoryMs) v = 0;
    g_timing.store(true, std::memory_order_relaxed);
    Log("timing capture: started");
}

void EndTimingCapture() {
    std::lock_guard lock(g_mutex);
    g_timing.store(false, std::memory_order_relaxed);
    Log("timing capture: stopped after %zu frames", g_frames.size());
}

bool TimingCaptureRunning() {
    return g_timing.load(std::memory_order_relaxed);
}

void NoteFrameTiming(uint32_t frame, double frameMs) {
    if (!g_timing.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mutex);
    if (!g_timing.load(std::memory_order_relaxed)) return;
    FrameTiming t;
    t.frame = frame;
    t.durationMs = (float)frameMs;
    for (size_t i = 0; i < (size_t)CpuCategory::Count; ++i) {
        t.categoryMs[i] = (float)g_frameCategoryMs[i];
        g_frameCategoryMs[i] = 0;
    }
    // The oldest go when the ring is full: a timing capture left running should not grow without
    // bound, and what matters is the recent minutes.
    if (g_frames.size() >= kMaxFrames) {
        g_frames.erase(g_frames.begin(), g_frames.begin() + (g_frames.size() - kMaxFrames + 1));
        if (g_framesSent > g_frames.size()) g_framesSent = 0;
    }
    g_frames.push_back(t);
}

void SendTimingFrames() {
    std::vector<FrameTiming> batch;
    {
        std::lock_guard lock(g_mutex);
        if (g_framesSent >= g_frames.size()) return;
        batch.assign(g_frames.begin() + (ptrdiff_t)g_framesSent, g_frames.end());
        g_framesSent = g_frames.size();
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("TimingFrames");
    w.Key("categories"); w.BeginArray();
    for (size_t i = 0; i < (size_t)CpuCategory::Count; ++i) w.String(kCpuCategoryNames[i]);
    w.EndArray();
    w.Key("frames"); w.BeginArray();
    for (const FrameTiming& t : batch) {
        w.BeginObject();
        w.Key("frame"); w.Uint(t.frame);
        w.Key("durationMs"); w.Double(t.durationMs);
        w.Key("categoryMs"); w.BeginArray();
        for (size_t i = 0; i < (size_t)CpuCategory::Count; ++i) w.Double(t.categoryMs[i]);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void BeginCpuTimeline() {
    std::lock_guard lock(g_mutex);
    g_events.clear();
    g_threadIndex.clear();
    g_threadIds.clear();
    g_dropped = 0;
    g_calibrated = false;
    g_origin = std::chrono::steady_clock::now();
    g_running = true;
    g_recording.store(true, std::memory_order_relaxed);
}

bool SampleCalibration(DeviceData* dev) {
    if (!dev || !dev->calibratedTimestamps) return false;
    auto get = dev->dispatch.GetCalibratedTimestampsKHR ? dev->dispatch.GetCalibratedTimestampsKHR
                                                        : dev->dispatch.GetCalibratedTimestampsEXT;
    if (!get) return false;
#if defined(_WIN32)
    const VkTimeDomainKHR hostDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
    const VkTimeDomainKHR hostDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif
    VkCalibratedTimestampInfoKHR infos[2]{};
    infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[1].timeDomain = hostDomain;
    uint64_t stamps[2] = {0, 0};
    uint64_t deviation = 0;
    if (get(dev->device, 2, infos, stamps, &deviation) != VK_SUCCESS) return false;

    // The host stamp is in the host domain's own units; what the timeline needs is where that
    // instant sits on the same steady_clock the events use. Reading the clock either side of the
    // call brackets it closely enough: the two reads are microseconds apart.
    const auto host = std::chrono::steady_clock::now();
    std::lock_guard lock(g_mutex);
    g_deviceTicks = stamps[0];
    g_hostMs = std::chrono::duration<double, std::milli>(host - g_origin).count();
    g_timestampPeriod = dev->properties.limits.timestampPeriod;
    g_calibrated = g_timestampPeriod > 0;
    return g_calibrated;
}

void SendCpuTimeline() {
    std::vector<CpuEvent> events;
    std::vector<uint64_t> threads;
    size_t dropped = 0;
    bool calibrated = false;
    uint64_t deviceTicks = 0;
    double hostMs = 0, period = 0;
    {
        std::lock_guard lock(g_mutex);
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
    for (uint64_t id : threads) w.Uint(id);
    w.EndArray();
    if (dropped) w.Key("dropped"), w.Uint(dropped);
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
// Memory residency (VK_EXT_memory_budget)

void PlanMemoryBudget(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, CpuTimelineSetup& setup) {
    if (!inst || !inst->dispatch.EnumerateDeviceExtensionProperties) return;
    uint32_t count = 0;
    inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    if (count) inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data());
    const bool has = std::any_of(available.begin(), available.end(), [](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
    });
    if (!has) return;
    // PlanCpuTimeline may already have taken a copy of the list; extend whichever is current.
    if (setup.extensionNames.empty()) {
        setup.extensionNames.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
    }
    const bool already = std::any_of(setup.extensionNames.begin(), setup.extensionNames.end(),
                                     [](const char* e) { return std::strcmp(e, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0; });
    if (!already) setup.extensionNames.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    info.ppEnabledExtensionNames = setup.extensionNames.data();
    info.enabledExtensionCount = (uint32_t)setup.extensionNames.size();
    setup.memoryBudget = true;
    setup.added = setup.added || !already;
}

void SendMemoryBudget(DeviceData* dev) {
    if (!dev || !dev->memoryBudget || !dev->instance) return;
    auto get = dev->instance->dispatch.GetPhysicalDeviceMemoryProperties2
                 ? dev->instance->dispatch.GetPhysicalDeviceMemoryProperties2
                 : dev->instance->dispatch.GetPhysicalDeviceMemoryProperties2KHR;
    if (!get) return;
    const uint64_t id = Tracker::Get().Resolve(HT_VkPhysicalDevice, (uint64_t)(uintptr_t)dev->physicalDevice);
    if (!id) return;

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    props.pNext = &budget;
    get(dev->physicalDevice, &props);
    const uint32_t heaps = props.memoryProperties.memoryHeapCount;

    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("memoryBudget"); w.BeginObject();
    // What the driver will let this process have, and what is resident from every process.
    w.Key("heapBudget"); w.BeginArray();
    for (uint32_t i = 0; i < heaps; ++i) w.Uint(budget.heapBudget[i]);
    w.EndArray();
    w.Key("heapUsage"); w.BeginArray();
    for (uint32_t i = 0; i < heaps; ++i) w.Uint(budget.heapUsage[i]);
    w.EndArray();
    w.EndObject();
    w.EndObject();
    Tracker::Get().Update(id, "memoryBudget", w.str());
}


// ---------------------------------------------------------------------------------------------
// Memory over time

namespace {

struct AllocationRecord {
    uint64_t size = 0;
    uint32_t heap = 0;
};

std::mutex g_memoryMutex;
std::unordered_map<uint64_t, AllocationRecord> g_allocations;   // VkDeviceMemory handle -> what it took
uint64_t g_heapBytes[VK_MAX_MEMORY_HEAPS] = {};
uint32_t g_heapCount[VK_MAX_MEMORY_HEAPS] = {};

}  // namespace

void NoteAllocation(DeviceData* dev, VkDeviceMemory memory, const VkMemoryAllocateInfo* info) {
    if (!dev || !memory || !info) return;
    const uint32_t type = info->memoryTypeIndex;
    if (type >= dev->memoryProperties.memoryTypeCount) return;
    // Kept apart from the per-heap totals below: reading a descriptor buffer means finding the
    // allocation a buffer sits in and whether the application has it mapped (descriptor_buffer.h).
    ResourceRegistry::Get().NoteMemory(
        memory, info->allocationSize,
        (dev->memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);
    const uint32_t heap = dev->memoryProperties.memoryTypes[type].heapIndex;
    if (heap >= VK_MAX_MEMORY_HEAPS) return;
    std::lock_guard lock(g_memoryMutex);
    AllocationRecord& r = g_allocations[(uint64_t)(uintptr_t)memory];
    // A handle the driver has handed out again after a free it did not report: the old record would
    // otherwise be counted twice.
    if (r.size) {
        g_heapBytes[r.heap] -= std::min(g_heapBytes[r.heap], r.size);
        if (g_heapCount[r.heap]) --g_heapCount[r.heap];
    }
    r.size = info->allocationSize;
    r.heap = heap;
    g_heapBytes[heap] += r.size;
    ++g_heapCount[heap];
}

void NoteFree(DeviceData* dev, VkDeviceMemory memory) {
    (void)dev;
    if (!memory) return;
    std::lock_guard lock(g_memoryMutex);
    auto it = g_allocations.find((uint64_t)(uintptr_t)memory);
    if (it == g_allocations.end()) return;
    const AllocationRecord& r = it->second;
    if (r.heap < VK_MAX_MEMORY_HEAPS) {
        g_heapBytes[r.heap] -= std::min(g_heapBytes[r.heap], r.size);
        if (g_heapCount[r.heap]) --g_heapCount[r.heap];
    }
    g_allocations.erase(it);
}

void SendMemorySample(DeviceData* dev) {
    if (!dev || !dev->instance) return;
    const uint32_t heaps = dev->memoryProperties.memoryHeapCount;
    if (!heaps) return;

    // The driver's own view, where the device reports one. Absent is not zero, so the two are kept
    // apart: a sample with no budget still carries what the application holds.
    bool hasBudget = false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (dev->memoryBudget) {
        auto get = dev->instance->dispatch.GetPhysicalDeviceMemoryProperties2
                     ? dev->instance->dispatch.GetPhysicalDeviceMemoryProperties2
                     : dev->instance->dispatch.GetPhysicalDeviceMemoryProperties2KHR;
        if (get) {
            VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
            props.pNext = &budget;
            get(dev->physicalDevice, &props);
            hasBudget = true;
        }
    }

    uint64_t bytes[VK_MAX_MEMORY_HEAPS];
    uint32_t counts[VK_MAX_MEMORY_HEAPS];
    {
        std::lock_guard lock(g_memoryMutex);
        std::copy(std::begin(g_heapBytes), std::end(g_heapBytes), std::begin(bytes));
        std::copy(std::begin(g_heapCount), std::end(g_heapCount), std::begin(counts));
    }

    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("MemorySample");
    w.Key("frame"); w.Uint(dev->frameIndex);
    w.Key("heaps"); w.BeginArray();
    for (uint32_t i = 0; i < heaps && i < VK_MAX_MEMORY_HEAPS; ++i) {
        w.BeginObject();
        w.Key("allocated"); w.Uint(bytes[i]);
        w.Key("allocations"); w.Uint(counts[i]);
        if (hasBudget) {
            w.Key("usage"); w.Uint(budget.heapUsage[i]);
            w.Key("budget"); w.Uint(budget.heapBudget[i]);
        }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

} // namespace vkinsp
