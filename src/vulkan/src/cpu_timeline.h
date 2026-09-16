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

} // namespace vkinsp
