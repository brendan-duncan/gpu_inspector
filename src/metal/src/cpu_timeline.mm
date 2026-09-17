#include "cpu_timeline.h"

#include "frame_stats.h"
#include "json_writer.h"
#include "transport.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include <pthread.h>

namespace mtlinsp {
namespace {

using Clock = std::chrono::steady_clock;

const char *const kCpuCategoryNames[(size_t)CpuCategory::Count] = {
    "submit", "waitFences", "acquire", "pipeline",
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
Clock::time_point g_origin{};
bool g_running = false;
/** Set while a capture wants events, read without the lock on the hot path. */
std::atomic<bool> g_recording{false};
size_t g_dropped = 0;

std::vector<uint64_t> g_threadIds;

/** The GPU-to-host relation sampled at the end of the capture (see SampleCalibration). */
bool g_calibrated = false;
uint64_t g_deviceTicks = 0;
double g_hostMs = 0;           // host time of that instant, relative to the origin
double g_timestampPeriod = 0;  // nanoseconds per GPU tick

/** The calling thread's index in the report's list; assigned on first sight. Under g_mutex. */
uint32_t ThreadIndex() {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    for (size_t i = 0; i < g_threadIds.size(); ++i) {
        if (g_threadIds[i] == id) return (uint32_t)i;
    }
    g_threadIds.push_back(id);
    return (uint32_t)g_threadIds.size() - 1;
}

}  // namespace

uint64_t CpuEventBegin() {
    if (!g_recording.load(std::memory_order_relaxed)) return 0;
    return (uint64_t)Clock::now().time_since_epoch().count();
}

void CpuEventEnd(uint64_t started, CpuCategory category) {
    if (!started || !g_recording.load(std::memory_order_relaxed)) return;
    const uint64_t now = (uint64_t)Clock::now().time_since_epoch().count();
    // Read before this module's lock: FrameNumber takes frame_stats' own, and taking the two in a
    // fixed order is what keeps them from ever being taken in the opposite one.
    const uint32_t frame = (uint32_t)FrameNumber();
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
    e.frame = frame;
    g_events.push_back(e);
}

void BeginCpuTimeline() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_events.clear();
    g_threadIds.clear();
    g_dropped = 0;
    g_calibrated = false;
    g_origin = Clock::now();
    g_running = true;
    g_recording.store(true, std::memory_order_relaxed);
}

bool SampleCalibration(id device, double nsPerTick) {
    if (device == nil || nsPerTick <= 0) return false;
    if (@available(macOS 10.15, iOS 14.0, *)) {
        MTLTimestamp cpuTicks = 0, gpuTicks = 0;
        [(id<MTLDevice>)device sampleTimestamps:&cpuTicks gpuTimestamp:&gpuTicks];
        if (gpuTicks == 0) return false;
        // The host half of Metal's pair is in its own domain; what the timeline needs is where that
        // instant sits on the same steady_clock the events use, so the clock is read beside it
        // rather than converted. The two reads are microseconds apart, which is the same bracketing
        // the Vulkan and D3D12 backends do for their host domains.
        const Clock::time_point host = Clock::now();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_deviceTicks = gpuTicks;
        g_hostMs = std::chrono::duration<double, std::milli>(host - g_origin).count();
        g_timestampPeriod = nsPerTick;
        g_calibrated = true;
        return true;
    }
    return false;
}

void SendCpuTimeline() {
    std::vector<CpuEvent> events;
    std::vector<uint64_t> threads;
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
    std::sort(events.begin(), events.end(),
              [](const CpuEvent &a, const CpuEvent &b) { return a.startNs < b.startNs; });

    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureCpuTimeline");
    w.Key("threads"); w.BeginArray();
    for (uint64_t id : threads) w.Uint(id);
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
    for (const CpuEvent &e : events) {
        w.BeginObject();
        w.Key("thread"); w.Uint(e.thread);
        w.Key("category");
        w.String(kCpuCategoryNames[e.category < (uint16_t)CpuCategory::Count ? e.category : 0]);
        w.Key("frame"); w.Uint(e.frame);
        w.Key("startMs"); w.Double((double)e.startNs / 1e6);
        w.Key("durationMs"); w.Double((double)e.durationNs / 1e6);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void SendMemorySample(id device) {
    if (device == nil) return;
    const uint64_t allocated = ((id<MTLDevice>)device).currentAllocatedSize;
    const uint64_t budget = ((id<MTLDevice>)device).recommendedMaxWorkingSetSize;

    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("MemorySample");
    w.Key("frame"); w.Uint(FrameNumber());
    w.Key("heaps"); w.BeginArray();
    w.BeginObject();
    // Metal reports what the process holds directly, so there is nothing to total up from the
    // object graph and no separate residency figure to compare it against (cpu_timeline.h).
    w.Key("allocated"); w.Uint(allocated);
    if (budget) {
        w.Key("usage"); w.Uint(allocated);
        w.Key("budget"); w.Uint(budget);
    }
    w.EndObject();
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

}  // namespace mtlinsp
