#include "present_timing.h"

#include "layer.h"
#include "refresh_rate.h"

#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace vkinsp {

namespace {

// How many presents may have results waiting: a present's result completes a frame or two after
// it, and the results are drained before every present, so a handful is plenty. A queue that fills
// anyway (an application that stops presenting this swapchain for a while) refuses the layer's
// request, not the application's present: see AfterPresent.
constexpr uint32_t kQueueSize = 16;
// Clocks drift: the domain-to-host relation is sampled again this often (about once a second).
constexpr uint64_t kCalibrateEvery = 120;
// A latency outside this is a result matched to the wrong call, or a clock that jumped.
constexpr double kMaxLatencyMs = 1000.0;

bool ChainHas(const void* pNext, VkStructureType type) {
    for (auto* p = (const VkBaseInStructure*)pNext; p; p = p->pNext)
        if (p->sType == type) return true;
    return false;
}

#if defined(_WIN32)
double QpcNsPerTick() {
    static double ns = [] {
        LARGE_INTEGER f;
        return QueryPerformanceFrequency(&f) && f.QuadPart > 0 ? 1e9 / (double)f.QuadPart : 0.0;
    }();
    return ns;
}
constexpr VkTimeDomainKHR kHostDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
constexpr VkTimeDomainKHR kHostDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif

/** The host clock the calibration is against, in nanoseconds: QPC on Windows, CLOCK_MONOTONIC elsewhere. */
double HostNowNs() {
#if defined(_WIN32)
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * QpcNsPerTick();
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
#endif
}

/** A raw host-domain stamp (from the calibration) in nanoseconds. */
double HostStampNs(uint64_t raw) {
#if defined(_WIN32)
    return (double)raw * QpcNsPerTick();
#else
    return (double)raw;
#endif
}

}  // namespace

PresentTiming& PresentTiming::Get() {
    static PresentTiming instance;
    return instance;
}

double PresentTiming::DomainToNs(const State& s, uint64_t raw) const {
    return (double)raw * s.nsPerTick;
}

void PresentTiming::OnCreateSwapchain(DeviceData* dev, VkSwapchainKHR swapchain, VkSurfaceKHR surface) {
    if (!dev || !dev->presentTiming || !dev->dispatch.SetSwapchainPresentTimingQueueSizeEXT ||
        !dev->dispatch.GetSwapchainTimeDomainPropertiesEXT || !dev->dispatch.GetPastPresentationTimingEXT) return;
    // The stage to ask for: when the first pixel left for the display, or the nearest the surface
    // reports. The end of the queue operations says nothing about the display, so it is no use here.
    const VkPresentStageFlagsEXT offered = SurfacePresentStages(dev, surface);
    VkPresentStageFlagsEXT stage = 0;
    for (VkPresentStageFlagsEXT want : { (VkPresentStageFlagsEXT)VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
                                         (VkPresentStageFlagsEXT)VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT,
                                         (VkPresentStageFlagsEXT)VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT }) {
        if (offered & want) { stage = want; break; }
    }
    if (!stage) {
        Log("present timing: the surface reports no display stage (0x%x), so dropped frames stay estimated", (unsigned)offered);
        return;
    }
    // A time domain the results come in. The latency wants one the host can read or calibrate
    // against, so those come first; the swapchain-local domain, which is neither, still serves
    // the dropped-frame count, since its results are only compared with each other.
    VkSwapchainTimeDomainPropertiesEXT domains{VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT};
    uint64_t counter = 0;
    if (dev->dispatch.GetSwapchainTimeDomainPropertiesEXT(dev->device, swapchain, &domains, &counter) < 0 || !domains.timeDomainCount) {
        Log("present timing: no time domains for the swapchain, so dropped frames stay estimated");
        return;
    }
    std::vector<VkTimeDomainKHR> kinds(domains.timeDomainCount);
    std::vector<uint64_t> ids(domains.timeDomainCount);
    domains.pTimeDomains = kinds.data();
    domains.pTimeDomainIds = ids.data();
    if (dev->dispatch.GetSwapchainTimeDomainPropertiesEXT(dev->device, swapchain, &domains, &counter) < 0) return;
    int best = -1, bestRank = 99;
    for (uint32_t i = 0; i < domains.timeDomainCount; ++i) {
        int rank;
        switch (kinds[i]) {
            case kHostDomain: rank = 0; break;
#if !defined(_WIN32)
            case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR: rank = 1; break;
#endif
            case VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT: rank = 2; break;
            case VK_TIME_DOMAIN_DEVICE_KHR: rank = 3; break;
            case VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT: rank = 4; break;
            default: continue;
        }
        if (rank < bestRank) { bestRank = rank; best = (int)i; }
    }
    if (best < 0) {
        Log("present timing: no time domain with a known unit, so dropped frames stay estimated");
        return;
    }
    const VkResult sized = dev->dispatch.SetSwapchainPresentTimingQueueSizeEXT(dev->device, swapchain, kQueueSize);
    if (sized != VK_SUCCESS) {
        Log("present timing: vkSetSwapchainPresentTimingQueueSizeEXT gave %d, so dropped frames stay estimated", (int)sized);
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    State& s = _swapchains[swapchain];
    s = State{};
    s.dev = dev;
    s.enabled = true;
    s.queueSize = kQueueSize;
    s.timeDomainId = ids[best];
    s.timeDomain = kinds[best];
    s.stage = stage;
    switch (s.timeDomain) {
        case VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR:
#if defined(_WIN32)
            s.nsPerTick = QpcNsPerTick();
            s.hostReadable = s.nsPerTick > 0;
#endif
            break;
        case VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR:
            s.hostReadable = true;
            break;
        case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR:
            // Readable, but not the clock HostNowNs reads: calibrated like a device domain.
            s.calibratable = dev->calibratedTimestamps;
            break;
        case VK_TIME_DOMAIN_DEVICE_KHR:
            s.nsPerTick = dev->properties.limits.timestampPeriod;
            s.calibratable = dev->calibratedTimestamps && s.nsPerTick > 0;
            break;
        case VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT:
            s.calibratable = dev->calibratedTimestamps;
            break;
        default:
            break;
    }
    QueryRefresh(s, swapchain);
    if (s.calibratable) Calibrate(s, swapchain);
    Log("present timing: measuring dropped frames%s on swapchain %p (stage 0x%x of 0x%x, time domain %d%s)",
        s.hostReadable || s.calibrated ? " and present latency" : "", (void*)swapchain, (unsigned)stage, (unsigned)offered,
        (int)s.timeDomain, s.hostReadable ? ", host-readable" : s.calibrated ? ", calibrated" : s.calibratable ? ", calibration failed" : "");
}

void PresentTiming::OnDestroySwapchain(VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(_mutex);
    _swapchains.erase(swapchain);
}

void PresentTiming::QueryRefresh(State& s, VkSwapchainKHR swapchain) {
    if (!s.dev->dispatch.GetSwapchainTimingPropertiesEXT) return;
    VkSwapchainTimingPropertiesEXT props{VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
    uint64_t counter = 0;
    if (s.dev->dispatch.GetSwapchainTimingPropertiesEXT(s.dev->device, swapchain, &props, &counter) >= 0 && props.refreshDuration > 0) {
        s.refreshNs = props.refreshDuration;
        s.timingCounter = counter;
    }
}

/**
 * Samples the stage's domain and the host clock at one instant (VK_KHR_calibrated_timestamps),
 * which is what relates a result's time to the call time. The present-stage-local domain is named
 * through VkSwapchainCalibratedTimestampInfoEXT; the device domain and the raw monotonic clock are
 * ordinary calibrateable domains.
 */
void PresentTiming::Calibrate(State& s, VkSwapchainKHR swapchain) {
    DeviceData* dev = s.dev;
    auto get = dev->dispatch.GetCalibratedTimestampsKHR ? dev->dispatch.GetCalibratedTimestampsKHR
                                                        : dev->dispatch.GetCalibratedTimestampsEXT;
    s.presentsSinceCalibration = 0;
    if (!get) { s.calibratable = false; return; }
    VkSwapchainCalibratedTimestampInfoEXT local{VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT};
    local.swapchain = swapchain;
    local.presentStage = s.stage;
    local.timeDomainId = s.timeDomainId;
    VkCalibratedTimestampInfoKHR infos[2]{};
    infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[0].timeDomain = s.timeDomain;
    if (s.timeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) infos[0].pNext = &local;
    infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[1].timeDomain = kHostDomain;
    uint64_t stamps[2] = {0, 0};
    uint64_t deviation = 0;
    const VkResult r = get(dev->device, 2, infos, stamps, &deviation);
    if (r != VK_SUCCESS || stamps[0] == 0) {
        if (!s.calibrated) {
            Log("present timing: calibrating time domain %d against the host gave %d, so present latency is not measured", (int)s.timeDomain, (int)r);
            s.calibratable = false;
        }
        return;
    }
    s.domainMinusHostNs = DomainToNs(s, stamps[0]) - HostStampNs(stamps[1]);
    s.calibrated = true;
}

void PresentTiming::Drain(State& s, VkSwapchainKHR swapchain) {
    DeviceData* dev = s.dev;
    VkPastPresentationTimingInfoEXT info{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
    info.swapchain = swapchain;
    VkPastPresentationTimingPropertiesEXT props{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
    if (dev->dispatch.GetPastPresentationTimingEXT(dev->device, &info, &props) < 0 || !props.presentationTimingCount) return;
    const uint32_t count = props.presentationTimingCount;
    s.results.assign(count, VkPastPresentationTimingEXT{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT});
    s.stages.assign(count, VkPresentStageTimeEXT{});
    for (uint32_t i = 0; i < count; ++i) {
        s.results[i].presentStageCount = 1;
        s.results[i].pPresentStages = &s.stages[i];
    }
    props.presentationTimingCount = count;
    props.pPresentationTimings = s.results.data();
    if (dev->dispatch.GetPastPresentationTimingEXT(dev->device, &info, &props) < 0) return;
    // The refresh period can change under the swapchain (a mode switch); the counter says when.
    if (props.timingPropertiesCounter != s.timingCounter) QueryRefresh(s, swapchain);
    for (uint32_t i = 0; i < props.presentationTimingCount; ++i) {
        const VkPastPresentationTimingEXT& r = s.results[i];
        // A result still waiting for its stage stays in the queue and comes back later, complete.
        if (!r.reportComplete) continue;
        if (s.outstanding) s.outstanding--;
        s.queueFull = false;
        // The call this result answers: the oldest tagged present still unanswered.
        double callNs = 0;
        if (!s.callHostNs.empty()) { callNs = s.callHostNs.front(); s.callHostNs.pop_front(); }
        if (r.presentStageCount < 1 || s.stages[i].stage != s.stage || s.stages[i].time == 0) continue;
        const double shownNs = DomainToNs(s, s.stages[i].time);
        if (s.lastShownNs > 0 && s.refreshNs > 0 && shownNs > (double)s.lastShownNs) {
            // Two frames shown n refreshes apart: the display repeated the earlier one n-1 times.
            const long refreshes = std::lround((shownNs - (double)s.lastShownNs) / (double)s.refreshNs);
            if (refreshes > 1) {
                dev->droppedMeasuredSince += (uint32_t)(refreshes - 1);
                dev->droppedMeasuredTotal += (uint64_t)(refreshes - 1);
            }
        }
        s.lastShownNs = (uint64_t)shownNs;
        dev->droppedMeasured = true;
        // The latency: from the call to the stage, both on the host clock.
        if (callNs > 0 && (s.hostReadable || s.calibrated)) {
            const double shownHostNs = s.hostReadable ? shownNs : shownNs - s.domainMinusHostNs;
            const double latencyMs = (shownHostNs - callNs) / 1e6;
            if (latencyMs > 0 && latencyMs < kMaxLatencyMs) dev->presentLatencySamplesMs.push_back(latencyMs);
        }
    }
}

const VkPresentInfoKHR* PresentTiming::BeforePresent(DeviceData* dev, const VkPresentInfoKHR* info, PresentStorage& storage) {
    if (!dev || !dev->presentTiming || !info || !info->swapchainCount || !info->pSwapchains) return info;
    // An application that times its presents itself, or numbers them, keeps its chain: a second
    // request would take a slot of a queue it sized for its own, and its results would gain ours.
    if (ChainHas(info->pNext, VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) ||
        ChainHas(info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) ||
        ChainHas(info->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR)) return info;
    std::lock_guard<std::mutex> lock(_mutex);
    bool any = false;
    storage.infos.assign(info->swapchainCount, VkPresentTimingInfoEXT{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT});
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        auto it = _swapchains.find(info->pSwapchains[i]);
        if (it == _swapchains.end() || !it->second.enabled) continue;
        State& s = it->second;
        Drain(s, info->pSwapchains[i]);
        if (s.calibratable && ++s.presentsSinceCalibration >= kCalibrateEvery) Calibrate(s, info->pSwapchains[i]);
        // A slot has to be free for the request, or the present itself would be refused.
        if (s.queueFull || s.outstanding >= s.queueSize) continue;
        VkPresentTimingInfoEXT& t = storage.infos[i];
        t.timeDomainId = s.timeDomainId;
        t.presentStageQueries = s.stage;
        s.outstanding++;
        // The call time, taken now: the present goes down the chain the moment this returns.
        s.callHostNs.push_back(HostNowNs());
        any = true;
    }
    if (!any) return info;
    storage.info = *info;
    storage.timings.pNext = info->pNext;
    storage.timings.swapchainCount = info->swapchainCount;
    storage.timings.pTimingInfos = storage.infos.data();
    storage.info.pNext = &storage.timings;
    return &storage.info;
}

void PresentTiming::AfterPresent(const VkPresentInfoKHR* tagged, VkResult res) {
    if (res != VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT || !tagged) return;
    // The request was not taken (and neither was the present, which the caller retries untagged):
    // the outstanding count was wrong about the queue, so wait for a drain to say it has room.
    std::lock_guard<std::mutex> lock(_mutex);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("present timing: the swapchain's results queue is full; the present was retried without a timing request");
    }
    for (uint32_t i = 0; i < tagged->swapchainCount; ++i) {
        auto it = _swapchains.find(tagged->pSwapchains[i]);
        if (it == _swapchains.end()) continue;
        if (it->second.outstanding) it->second.outstanding--;
        if (!it->second.callHostNs.empty()) it->second.callHostNs.pop_back();
        it->second.queueFull = true;
    }
}

bool PresentTiming::Measured(DeviceData* dev, uint32_t& sinceReport, uint64_t& total, double& latencyMs) {
    if (!dev || !dev->droppedMeasured) return false;
    sinceReport = dev->droppedMeasuredSince;
    total = dev->droppedMeasuredTotal;
    dev->droppedMeasuredSince = 0;
    // The median rather than the mean: a present that found the queue empty shows almost at
    // once, and one that queued behind others waits a refresh or two; the middle one is the
    // typical frame, where an average of the two is a frame that never happened.
    std::vector<double>& samples = dev->presentLatencySamplesMs;
    latencyMs = 0;
    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        latencyMs = samples[samples.size() / 2];
        samples.clear();
    }
    return true;
}

}  // namespace vkinsp
