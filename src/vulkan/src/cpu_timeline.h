// The CPU side of a frame, on the same clock as the GPU side.
//
// GPU Bottlenecks answers "which pass is slow". The question before it — is the GPU the problem at
// all — is answered today by comparing three aggregates: the frame interval, the CPU time inside
// vkQueueSubmit, and the passes' GPU time. That tells you the ratio but not the shape: a frame that
// spends four milliseconds blocked in vkWaitForFences looks identical to one that spends four
// milliseconds submitting, and they need opposite fixes.
//
// So during a capture the layer times the calls where a frame actually spends its CPU — submitting,
// presenting, waiting on fences, acquiring swapchain images — and records each one with the thread
// that made it. Nsight Systems shows the same thing across every thread; this is the part of it that
// a frame capture can carry.
//
// Putting the two on one axis needs the GPU's clock related to the host's, which is what
// VK_EXT_calibrated_timestamps is for: it samples both at one instant, so a pass's GPU timestamp can
// be placed against the host time of the submit that launched it. Without the extension the CPU
// events still stand on their own and the GPU passes keep their own origin, which the capture says.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vkinsp {

struct DeviceData;
struct InstanceData;

/** What a CPU event was, in the words the timeline shows. */
enum class CpuCategory : uint16_t {
    Submit = 0,     // vkQueueSubmit and friends: handing work to the GPU
    Present,        // vkQueuePresentKHR
    WaitFences,     // vkWaitForFences: blocked until the GPU caught up
    Acquire,        // vkAcquireNextImageKHR: blocked until the presenter freed an image
    WaitIdle,       // vkQueueWaitIdle / vkDeviceWaitIdle
    // Pipeline and shader creation: the driver compiling, on the thread that asked. Timed because
    // a pipeline built inside a frame stops it, which is what a hitch on first sight of a material
    // usually is — and unlike the categories above, the fix is to do it earlier rather than less.
    PipelineCreate,
    Count,
};

/** Their names, indexed by CpuCategory. */
extern const char* const kCpuCategoryNames[(size_t)CpuCategory::Count];

/** What a device asked for at creation, decided before vkCreateDevice. */
struct CpuTimelineSetup {
    std::vector<const char*> extensionNames;
    /** The layer added VK_EXT_memory_budget, so the driver's residency view is available. */
    bool memoryBudget = false;
    /** The layer added a calibrated-timestamps extension, so GPU and CPU times can share an axis. */
    bool calibrated = false;
    bool added = false;
};

/**
 * Adds a calibrated-timestamps extension to a device being created when the physical device offers
 * one. Cheap and passive — it adds no work, only the ability to relate the two clocks — so unlike
 * the breadcrumbs and the compiler statistics this is not behind an option.
 */
void PlanCpuTimeline(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, CpuTimelineSetup& setup);

/** Notes what the device ended up with; call after vkCreateDevice. */
void InitCpuTimeline(DeviceData* dev, const CpuTimelineSetup& setup);

/**
 * The start of a timed call, or 0 when nothing is being captured. Cheap enough to sit in every
 * submit and present: one atomic read and, while capturing, one clock read.
 */
uint64_t CpuEventBegin();

/** The end of one: records it when a capture is running and `started` is not 0. */
void CpuEventEnd(DeviceData* dev, uint64_t started, CpuCategory category);

/** Clears the recorded events and takes the capture's time origin; called when a capture starts. */
void BeginCpuTimeline();

// ---------------------------------------------------------------------------------------------
// Timing captures.
//
// A frame report averages over its interval (about 100 ms, so five or six frames at 60 Hz), and a
// hitch is one frame: averaged together with five good ones it disappears. A capture keeps every
// call of a few frames, which is the opposite problem — all the detail, none of the duration.
//
// A timing capture is the shape in between: for every frame, its wall time and how long the CPU
// spent in each category during it. That is 32 bytes a frame, so minutes of it fit in memory, and
// it is enough to find a hitch and say what the CPU was doing in it.
//
// It is a mode rather than something always on. Recording needs a clock read in every timed call,
// and the layer's whole cost when idle today is one relaxed atomic read; making every run pay for
// a feature few runs use would be the wrong trade.

/** One frame: how long it took, and where its CPU time went. */
struct FrameTiming {
    uint32_t frame = 0;
    float durationMs = 0;
    float categoryMs[(size_t)CpuCategory::Count] = {};
};

/**
 * Starts recording per-frame timings, discarding anything held from a previous one. `sampleHz`
 * above zero also samples every thread's call stack that often (cpu_sampler.h), which is what says
 * what the CPU was doing in a frame none of the timed calls account for. Windows only; elsewhere
 * the timings are recorded without it.
 */
void BeginTimingCapture(uint32_t sampleHz = 0);
/** Stops recording. The records already taken stay until the next Begin. */
void EndTimingCapture();
/** Whether a timing capture is running, for the frame report to say so. */
bool TimingCaptureRunning();

/**
 * Closes off the frame that just ended: called from the frame boundary with its wall time. Quiet
 * unless a timing capture is running.
 */
void NoteFrameTiming(uint32_t frame, double frameMs);

/** Writes the records taken since the last call as a TimingFrames message; quiet when there are none. */
void SendTimingFrames();

/** Writes the call stacks sampled since the last call as a TimingSamples message; quiet when there are none. */
void SendTimingSamples();

/**
 * Relates the GPU clock to the host's, so pass timestamps can be placed on the CPU axis. Sampled
 * when the capture ends, while the device is still alive. Returns false when the device has no
 * calibrated-timestamps extension, which the capture records rather than guessing an alignment.
 */
bool SampleCalibration(DeviceData* dev);

/** Writes the capture's CpuTimeline section; nothing when no events were recorded. */
void SendCpuTimeline();

// ---------------------------------------------------------------------------------------------
// Memory residency.
//
// What the application allocated is in the object graph already: every VkDeviceMemory records its
// size and memory type. What it cannot know is how much of each heap is actually resident and how
// much the driver will let this process have — that counts every process on the GPU, and only the
// driver can say. VK_EXT_memory_budget is how it is asked.

/** Adds VK_EXT_memory_budget to a device being created when the physical device offers it. */
void PlanMemoryBudget(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, CpuTimelineSetup& setup);

/**
 * Sends the driver's per-heap budget and usage as an update on the physical device, for the memory
 * view. Cheap enough for the frame report's interval; quiet when the device has no budget extension.
 */
void SendMemoryBudget(DeviceData* dev);

// ---------------------------------------------------------------------------------------------
// Memory over time.
//
// The update above is an instant: it says what is held now and overwrites what it said before. What
// that cannot show is the shape — memory climbing frame after frame is a leak, memory sawtoothing
// is a pool being refilled, and memory flat is neither, and all three look identical at any one
// moment. So the layer keeps a running total per heap and reports it with each frame report, giving
// a series the UI can plot against the frame.
//
// The totals are kept as the application allocates rather than counted on demand: a renderer with
// tens of thousands of allocations would otherwise be walked ten times a second.

/** An allocation, added to its heap's running total. */
void NoteAllocation(DeviceData* dev, VkDeviceMemory memory, const VkMemoryAllocateInfo* info);

/** A free, subtracted from it. Quiet for a handle that was never noted. */
void NoteFree(DeviceData* dev, VkDeviceMemory memory);

/**
 * Sends one sample of the memory series: what this application holds per heap, and what the driver
 * says is resident and allowed where it reports that. Called with the frame report.
 */
void SendMemorySample(DeviceData* dev);

// ---------------------------------------------------------------------------------------------
// Memory captures.
//
// The series above says which way memory is going; it cannot say *what* is going. A total that
// climbs a megabyte a second is one allocation a frame that is never freed, or a thousand that
// mostly are, and the fix for each is nowhere near the other. A memory capture is the record that
// tells them apart: every allocation and every free while it runs, with the frame it happened in
// and the object it was, so what is still held at the end can be named, and what was made and
// thrown away within a frame or two can be counted.
//
// A mode rather than always on, like a timing capture: idle, it costs the allocation paths one
// relaxed atomic read. The events ride on the frame report's interval as a MemoryEvents message.

/** Starts recording allocations and frees, discarding what a previous capture held. */
void BeginMemoryCapture();
/** Stops recording; what was recorded and not yet sent goes with the next report. */
void EndMemoryCapture();

/** Writes the events recorded since the last call as a MemoryEvents message; quiet when there are none. */
void SendMemoryEvents(DeviceData* dev);

} // namespace vkinsp
