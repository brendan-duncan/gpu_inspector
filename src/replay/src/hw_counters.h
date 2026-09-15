// NVIDIA's Nsight Perf SDK behind a small interface, so hw_counters.cpp sees none of its headers.
// nvperf.cpp implements it when the replay is built with the SDK (VKINSP_NVPERF); otherwise every
// call says the SDK is not built in.
//
// The SDK's host library (nvperf_grfx_host) is not shipped with GPU Inspector: it is loaded at run
// time from beside vkinsp_replay, from the directory VKINSP_NVPERF_DIR names, or from an Nsight
// Graphics, Systems or Compute install on this machine, which all carry it.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vkreplay {

struct HwCounterInfo;

namespace nvperf {

/** Finds and initialises the SDK's library; false, with why, when it is not built in or not found. */
bool Load(std::string& note);

/** Where Load found the library, for the notes. */
std::string LibraryPath();

/** The instance extensions the SDK's profiler needs, appended (empty when the SDK is not loaded). */
void InstanceExtensions(uint32_t apiVersion, std::vector<const char*>& out);

/** The device extensions it needs on an NVIDIA device, appended. */
void DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr gipa, std::vector<const char*>& out);

/** One range-profiling session on a queue: the counters are collected over as many passes as they need. */
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /**
     * Loads the driver, checks the GPU and creates the metrics evaluator for its chip. Enough to
     * list metrics; collecting them also needs Begin. False with why in `note`.
     */
    bool Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t queueFamily,
              PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, std::string& note);
    /**
     * Begins the profiling session, which needs GPU performance-counter access enabled;
     * `maxRanges` bounds how many ranges a pass may push. Call after Init. False with why in `note`.
     */
    bool Begin(uint32_t maxRanges, std::string& note);
    /** The chip the counters are for ("AD103"); empty before Begin. */
    const std::string& Chip() const;
    /** Every metric the chip has, with one submetric each in the spelling Configure accepts. */
    void ListMetrics(std::vector<HwCounterInfo>& out);
    /**
     * Configures the metrics to collect. Names the evaluator does not know, or that cannot be
     * scheduled, are left out with a note; `chosen` lists the rest, in `names` order.
     */
    bool Configure(const std::vector<std::string>& names, std::vector<HwCounterInfo>& chosen, std::vector<std::string>& notes);
    /** Collection passes the configuration needs at one nesting level; each replay of the frame is one. */
    size_t Passes() const;
    bool BeginPass();
    bool EndPass();
    void PushRange(VkCommandBuffer cb, const char* name);
    void PopRange(VkCommandBuffer cb);
    /** After EndPass and the queue idle: decodes the pass; `done` once every pass has been collected. */
    bool Decode(bool& done, std::string& error);
    /** After `done`: every range's leaf name with the value of each chosen metric (NaN where it failed). */
    bool Results(std::vector<std::pair<std::string, std::vector<double>>>& out, std::string& error);
    void End();

private:
    struct Impl;
    Impl* _impl = nullptr;
};

} // namespace nvperf
} // namespace vkreplay
