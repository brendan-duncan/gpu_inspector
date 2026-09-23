// Dropped frames and present latency measured rather than estimated, through VK_EXT_present_timing.
//
// The frame report's dropped-frame count in layer.cpp is worked out from the frame interval against
// the refresh period, which is the best a layer can do without the display's word. With
// VK_EXT_present_timing the display gives it: every present the layer tags asks the presentation
// engine to record when the image's first pixel went out (or the nearest stage the surface can
// report), and two consecutive presents shown more than one refresh apart mean the display showed
// the earlier one again in between — a dropped frame, in exactly the sense D3D12's
// DXGI_FRAME_STATISTICS counts them (src/d3d12/src/device_info.cpp).
//
// The same stage time, against the moment the application called vkQueuePresentKHR, is the
// present latency: how long a frame the application handed over took to reach the display, which
// is what a player feels as lag and what tells two queued frames from one. The two instants are on
// different clocks — the stage time is in a time domain of the swapchain's, the call time on the
// host's — so they are related through VK_KHR_calibrated_timestamps, which the layer enables with
// the extension: a swapchain's present-stage-local domain can be calibrated against the host clock
// (VkSwapchainCalibratedTimestampInfoEXT), and a host-readable domain needs no calibration at all.
// A swapchain that offers only a domain that can be neither read nor calibrated still counts its
// dropped frames and reports no latency.
//
// The layer already enables the extension and its features at device creation for the refresh
// period (refresh_rate.h) and creates swapchains with the present-timing flag. This adds the rest:
// the swapchain's results queue, a time domain, a VkPresentTimingsInfoEXT chained into every
// present that the application did not chain its own into, and the drain of results before each
// present. An application that uses present timing itself (a VkPresentTimingsInfoEXT or a present
// id in its own chain) is left alone, so its results queue stays its own.
//
// Everything here runs on the presenting thread, inside the layer's vkQueuePresentKHR.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vkinsp {

struct DeviceData;

class PresentTiming {
public:
    static PresentTiming& Get();

    // After a swapchain was created (with the present-timing flag, when the surface supports it):
    // sizes its results queue and picks the time domain and present stage to ask for.
    void OnCreateSwapchain(DeviceData* dev, VkSwapchainKHR swapchain, VkSurfaceKHR surface);
    void OnDestroySwapchain(VkSwapchainKHR swapchain);

    // The arrays a tagged present points at; lives on the caller's stack for the call.
    struct PresentStorage {
        VkPresentInfoKHR info{};
        VkPresentTimingsInfoEXT timings{VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT};
        std::vector<VkPresentTimingInfoEXT> infos;
    };
    /**
     * Before a present: drains the results of the earlier ones (counting the dropped frames and
     * latencies they show) and returns the present info to use — a copy with a timing request
     * chained, or the application's own when there is nothing to ask for.
     */
    const VkPresentInfoKHR* BeforePresent(DeviceData* dev, const VkPresentInfoKHR* info, PresentStorage& storage);
    /**
     * After a present made with a tagged info. VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT means the
     * present was refused for the layer's own request, which the caller retries untagged; this
     * stops asking until the queue has drained.
     */
    void AfterPresent(const VkPresentInfoKHR* tagged, VkResult res);

    /**
     * The measured figures for the frame report: the frames dropped since the last call and since
     * the connection, and the median present latency of the frames shown since the last call (0
     * when none was measured). False when nothing has been measured on this device yet, in which
     * case the report falls back to its estimate.
     */
    bool Measured(DeviceData* dev, uint32_t& sinceReport, uint64_t& total, double& latencyMs);

private:
    struct State {
        DeviceData* dev = nullptr;
        bool enabled = false;
        uint32_t queueSize = 0;
        uint32_t outstanding = 0;
        bool queueFull = false;
        uint64_t timeDomainId = 0;
        VkTimeDomainKHR timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
        double nsPerTick = 1.0;
        VkPresentStageFlagsEXT stage = 0;
        uint64_t refreshNs = 0;
        uint64_t timingCounter = 0;
        uint64_t lastShownNs = 0;
        // Latency: how the stage's domain relates to the host clock. A host-readable domain reads
        // directly (`hostReadable`); the present-stage-local and device domains are calibrated
        // (`calibrated`, `domainMinusHostNs`) every so often, since clocks drift; a domain that is
        // neither gives dropped frames only.
        bool hostReadable = false;
        bool calibratable = false;
        bool calibrated = false;
        double domainMinusHostNs = 0;
        uint64_t presentsSinceCalibration = 0;
        // The host time of each tagged present's call, oldest first: results come back in order.
        std::deque<double> callHostNs;
        // Result storage, reused between drains.
        std::vector<VkPastPresentationTimingEXT> results;
        std::vector<VkPresentStageTimeEXT> stages;
    };
    void Drain(State& s, VkSwapchainKHR swapchain);
    void QueryRefresh(State& s, VkSwapchainKHR swapchain);
    void Calibrate(State& s, VkSwapchainKHR swapchain);
    double DomainToNs(const State& s, uint64_t raw) const;

    std::mutex _mutex;
    std::unordered_map<VkSwapchainKHR, State> _swapchains;
};

}  // namespace vkinsp
