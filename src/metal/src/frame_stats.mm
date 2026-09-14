#include "frame_stats.h"

#include "json_writer.h"
#include "swizzle.h"
#include "transport.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace mtlinsp {
namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
uint64_t g_frame = 0;
Clock::time_point g_lastFrame;
Clock::time_point g_lastReport;
double g_accumMs = 0, g_minMs = 0, g_maxMs = 0;
uint32_t g_count = 0;
uint64_t g_submitNanos = 0;
bool g_displaySync = true;
double g_displayRefreshMs = 0;   // from the display, 0 when it does not say
double g_refreshMs = 0;          // what is reported: the display's, or the estimate
const char *g_refreshSource = "";
double g_loggedRefreshMs = -1;
// The last ~240 intervals, for the estimate.
constexpr size_t kWindow = 240;
std::vector<double> g_intervals;
size_t g_intervalNext = 0;

/**
 * The refresh period the intervals fit best, from a list of common rates: the Vulkan layer's
 * EstimateRefreshMs, verbatim. Frames that took several refreshes count as multiples; a
 * present that consumed no refresh (under a millisecond) is left out; the median error decides.
 */
double EstimateRefreshMs(const std::vector<double> &intervals) {
    static const double kRates[] = {24, 30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 240, 360};
    const size_t n = intervals.size();
    double sum = 0;
    for (double ms : intervals) sum += ms;
    constexpr size_t kQueued = 8;
    std::vector<double> errors;
    errors.reserve(n);
    double best = 0, bestError = 0;
    for (double hz : kRates) {
        const double period = 1000.0 / hz;
        if (n <= kQueued || sum < period * (double)(n - kQueued) * 0.97) continue;
        errors.clear();
        for (double ms : intervals) {
            if (ms < 1.0) continue;
            const double k = std::max(1.0, std::round(ms / period));
            errors.push_back(std::fabs(ms - k * period));
        }
        if (errors.size() < 8) continue;
        std::nth_element(errors.begin(), errors.begin() + errors.size() / 2, errors.end());
        const double error = errors[errors.size() / 2];
        if (error > std::max(period * 0.08, 0.25)) continue;
        if (best == 0 || error + 0.1 < bestError) { best = period; bestError = error; }
    }
    return best;
}

/**
 * The main display's refresh period. CoreGraphics reports 0 for a built-in panel on Apple
 * Silicon, in which case AppKit's NSScreen knows; that is asked through the runtime so that the
 * library does not link AppKit into an application that may not have it.
 */
double QueryDisplayRefreshMs() {
    double hz = 0;
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID());
    if (mode != nullptr) {
        hz = CGDisplayModeGetRefreshRate(mode);
        CGDisplayModeRelease(mode);
    }
    if (hz <= 0) {
        Class screenClass = objc_getClass("NSScreen");
        if (screenClass != nil) {
            id screen = ((id (*)(Class, SEL))objc_msgSend)(screenClass, sel_registerName("mainScreen"));
            const SEL maximum = sel_registerName("maximumFramesPerSecond");
            if (screen != nil && [screen respondsToSelector:maximum]) {
                hz = (double)((NSInteger (*)(id, SEL))objc_msgSend)(screen, maximum);
            }
        }
    }
    return hz > 0 ? 1000.0 / hz : 0;
}

}  // namespace

uint64_t FrameNumber() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_frame;
}

void AddSubmitTime(uint64_t nanoseconds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_submitNanos += nanoseconds;
}

void NoteDisplaySync(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_displaySync = enabled;
}

// Retained: a device outlives everything, but the report reads it on every frame boundary.
id g_device = nil;

void NoteDevice(id device) {
    if (device == nil) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_device == nil) g_device = [device retain];
}

uint64_t OnFrameEnded() {
    std::string message;
    uint64_t frame = 0;
    // The device's memory figures, read outside the lock: two property reads, no hook of ours
    // in their path, but Metal is not called with a lock of this library's held.
    id<MTLDevice> device = nil;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        device = (id<MTLDevice>)g_device;
    }
    const uint64_t allocatedBytes = device != nil ? device.currentAllocatedSize : 0;
    const uint64_t workingSetBytes = device != nil ? device.recommendedMaxWorkingSetSize : 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        frame = ++g_frame;
        const Clock::time_point now = Clock::now();
        // The display can change (a window moved to another monitor, a mode switch): asked
        // again every ~2 s.
        if (g_frame % 120 == 1) g_displayRefreshMs = QueryDisplayRefreshMs();
        if (g_lastFrame.time_since_epoch().count() != 0) {
            const double ms = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
            if (g_count == 0) {
                g_minMs = g_maxMs = ms;
            } else {
                g_minMs = std::min(g_minMs, ms);
                g_maxMs = std::max(g_maxMs, ms);
            }
            g_accumMs += ms;
            g_count++;
            if (g_intervals.size() < kWindow) g_intervals.push_back(ms);
            else g_intervals[g_intervalNext] = ms;
            g_intervalNext = (g_intervalNext + 1) % kWindow;

            // The display's own period when it says; the estimate from the intervals otherwise;
            // nothing for a layer presenting out of step with the display.
            if (g_displaySync && g_displayRefreshMs > 0) {
                g_refreshMs = g_displayRefreshMs;
                g_refreshSource = "monitor";
            } else if (g_displaySync && g_intervals.size() >= 32) {
                g_refreshMs = EstimateRefreshMs(g_intervals);
                g_refreshSource = "estimate";
            } else if (!g_displaySync) {
                g_refreshMs = 0;
                g_refreshSource = "";
            }
            if (std::fabs(g_refreshMs - g_loggedRefreshMs) > std::fabs(g_loggedRefreshMs) * 0.005) {
                g_loggedRefreshMs = g_refreshMs;
                if (g_refreshMs > 0) {
                    Log("refresh rate: %.4g Hz (%.3f ms, %s)", 1000.0 / g_refreshMs, g_refreshMs, g_refreshSource);
                } else {
                    Log("refresh rate: unknown");
                }
            }

            const double sinceReport = std::chrono::duration<double, std::milli>(now - g_lastReport).count();
            if (sinceReport >= 100.0 && Transport::Get().Connected()) {
                vkinsp::JsonWriter w;
                w.BeginObject();
                w.Key("action"); w.String("FrameStats");
                w.Key("frame"); w.Uint(g_frame);
                w.Key("frameTimeMs"); w.Double(g_accumMs / g_count);
                w.Key("minMs"); w.Double(g_minMs);
                w.Key("maxMs"); w.Double(g_maxMs);
                w.Key("frames"); w.Uint(g_count);
                w.Key("submitMs"); w.Double((double)g_submitNanos / 1e6 / g_count);
                w.Key("refreshMs"); w.Double(g_refreshMs);
                w.Key("refreshSource"); w.String(g_refreshMs > 0 ? g_refreshSource : "");
                w.Key("displayRefreshMs"); w.Double(g_displayRefreshMs);
                w.Key("frameBoundary"); w.String("present");
                // What Metal has set aside for this process, and the size it recommends staying
                // under: the memory meter's total.
                w.Key("allocatedBytes"); w.Uint(allocatedBytes);
                w.Key("workingSetBytes"); w.Uint(workingSetBytes);
                w.EndObject();
                message = std::move(w.str());
                g_lastReport = now;
                g_accumMs = 0;
                g_count = 0;
                g_submitNanos = 0;
            }
        } else {
            g_lastReport = now;
        }
        g_lastFrame = now;
    }
    if (!message.empty()) Transport::Get().SendJson(std::move(message));
    return frame;
}

}  // namespace mtlinsp
