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
#include "transport.h"

namespace vkinsp {

const char* const kCpuCategoryNames[(size_t)CpuCategory::Count] = {
    "submit", "present", "waitFences", "acquire", "waitIdle",
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
}

uint64_t CpuEventBegin() {
    if (!g_recording.load(std::memory_order_relaxed)) return 0;
    return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
}

void CpuEventEnd(DeviceData* dev, uint64_t started, CpuCategory category) {
    if (!started || !g_recording.load(std::memory_order_relaxed)) return;
    const uint64_t now = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    const uint64_t originNs = (uint64_t)g_origin.time_since_epoch().count();
    if (started < originNs) return;   // began before the capture did
    std::lock_guard lock(g_mutex);
    if (!g_running) return;
    if (g_events.size() >= kMaxEvents) {
        ++g_dropped;
        return;
    }
    CpuEvent e;
    e.startNs = started - originNs;
    e.durationNs = (uint32_t)std::min<uint64_t>(now > started ? now - started : 0, UINT32_MAX);
    e.thread = ThreadIndex();
    e.category = (uint16_t)category;
    e.frame = dev ? (uint32_t)dev->frameIndex : 0;
    g_events.push_back(e);
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

} // namespace vkinsp
