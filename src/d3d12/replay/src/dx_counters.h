// NVIDIA's Nsight Perf SDK behind a small interface, so the replayer sees none of its headers.
// dx_nvperf.cpp implements it when dxinsp_replay is built with the SDK (DXINSP_NVPERF); otherwise
// every call says the SDK is not built in.
//
// This is the Direct3D 12 counterpart of src/replay/src/hw_counters.h, and the shape is deliberately
// the same: a session on one command queue, metrics configured by name, the frame replayed once per
// collection pass, and a range pushed around each render pass. What differs is only which family of
// the SDK's entry points is called (NVPW_D3D12_* rather than NVPW_VK_*), since the SDK's own
// utility layer ships a Vulkan range profiler but not a D3D12 one.
//
// The SDK's host library (nvperf_grfx_host) is not shipped with GPU Inspector: it is loaded at run
// time from beside dxinsp_replay, from the directory DXINSP_NVPERF_DIR names, or from an Nsight
// Graphics, Systems or Compute install on this machine, which all carry it.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace dxreplay {

/** One metric the GPU can report, as the UI lists it (the Vulkan replay's HwCounterInfo). */
struct DxCounterInfo {
    std::string name;
    std::string description;
    /** The hardware unit it belongs to ("sm", "dram"), for grouping. */
    std::string category;
    std::string unit;
};

namespace nvperf {

/** Finds and initialises the SDK's library; false, with why, when it is not built in or not found. */
bool Load(std::string& note);

/** Where Load found the library, for the notes. */
std::string LibraryPath();

/**
 * Puts the Direct3D 12 driver into profiling mode. It has to happen *before the device is created*,
 * the way the Vulkan path appends the SDK's instance and device extensions before creating those:
 * a device made before this has no profiling support, and a pass on its queue never completes.
 */
bool LoadDriver(std::string& note);

/**
 * Whether this process may read the GPU's performance counters. On Windows NVIDIA restricts them to
 * administrators unless the machine says otherwise (NVIDIA Control Panel, Developer > Manage GPU
 * Performance Counters, which sets RmProfilingAdminOnly to 0). Vulkan reports the refusal as
 * ERR_NVGPUCTRPERM when the session begins; Direct3D 12 accepts the session and then never
 * completes the first profiled submission, so it is worth asking before replaying a frame 30 times.
 */
bool ProfilingPermitted(std::string& note);

/**
 * Whether this process may read the GPU's performance counters. On Windows NVIDIA restricts them to
 * administrators unless the machine says otherwise (NVIDIA Control Panel, Developer > Manage GPU
 * Performance Counters, which sets RmProfilingAdminOnly to 0). Vulkan reports the refusal as
 * ERR_NVGPUCTRPERM when the session begins; Direct3D 12 accepts the session and then never
 * completes the first profiled submission, so it is worth asking before replaying a frame 30 times.
 */
bool ProfilingPermitted(std::string& note);

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
    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& note);
    /**
     * Begins the profiling session, which needs GPU performance-counter access enabled;
     * `maxRanges` bounds how many ranges a pass may push. Call after Init. False with why in `note`.
     */
    bool Begin(uint32_t maxRanges, std::string& note);
    /** The chip the counters are for ("AD103"); empty before Init succeeded. */
    const std::string& Chip() const;
    /** Every metric the chip has, with one submetric each in the spelling Configure accepts. */
    void ListMetrics(std::vector<DxCounterInfo>& out);
    /**
     * Configures the metrics to collect. Names the evaluator does not know, or that cannot be
     * scheduled, are left out with a note; `chosen` lists the rest, in `names` order.
     */
    bool Configure(const std::vector<std::string>& names, uint16_t nestingLevels, std::vector<DxCounterInfo>& chosen,
                   std::vector<std::string>& notes);
    /** Collection passes the configuration needs at one nesting level; each replay of the frame is one. */
    size_t Passes() const;
    bool BeginPass();
    bool EndPass();
    void PushRange(ID3D12GraphicsCommandList* list, const char* name);
    void PopRange(ID3D12GraphicsCommandList* list);
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
} // namespace dxreplay
