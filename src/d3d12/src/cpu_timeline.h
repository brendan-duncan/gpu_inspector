// The CPU side of a frame, on the same clock as the GPU side.
//
// The D3D12 counterpart of src/vulkan/src/cpu_timeline.h, sending the same CaptureCpuTimeline
// message, because the app's Timeline card and "Where the CPU went" do not care which API the
// frame came from. What differs is where the time is found.
//
// Two of the four categories are D3D12 calls and are timed in their hooks: ExecuteCommandLists is
// the submit, Present is the present. The other two are not D3D12 calls at all — a D3D12 fence is
// waited on with WaitForSingleObject on a Win32 event, and a swapchain's frame-latency object the
// same way — so they cannot be found by hooking D3D12. They are found by hooking the wait itself
// and recognising the handle: `ID3D12Fence::SetEventOnCompletion` says which event belongs to a
// fence, `IDXGISwapChain2::GetFrameLatencyWaitableObject` which belongs to the presenter, and a
// wait on anything else is the application's own and is left alone.
//
// That matters more here than it looks. "The CPU is waiting for the GPU" is the first verdict
// docs/PROFILING.md asks for, and without the wait there is no way to tell a frame blocked on the
// GPU from one that simply had little to do: both spend almost no time inside D3D12.
//
// Relating the two clocks is easier than on Vulkan, where it needs an extension:
// `ID3D12CommandQueue::GetClockCalibration` is core and samples both at one instant.
#pragma once

#include "common.h"

#include <cstdint>

namespace dxinsp {

/** What a CPU event was, in the words the timeline shows. The names match the Vulkan layer's. */
enum class CpuCategory : uint16_t {
    Submit = 0,     // ExecuteCommandLists: handing work to the GPU
    Present,        // IDXGISwapChain::Present
    WaitFences,     // waiting on a fence's event: blocked until the GPU caught up
    Acquire,        // waiting on a swapchain's frame-latency object: blocked until the presenter freed one
    Count,
};

/**
 * The start of a timed call, or 0 when nothing is being captured. Cheap enough to sit in every
 * submit and present: one relaxed atomic read, and while capturing one clock read.
 */
uint64_t CpuEventBegin();

/** The end of one: records it when a capture is running and `started` is not 0. */
void CpuEventEnd(ID3D12Device* device, uint64_t started, CpuCategory category);

/** Clears the recorded events and takes the capture's time origin; called when a capture starts. */
void BeginCpuTimeline();

/**
 * Relates the GPU clock to the host's through the queue's clock calibration, so pass timestamps can
 * be placed on the CPU axis. Sampled when the capture ends, while the queue is still alive. False
 * when the queue cannot calibrate, which the capture records rather than guessing an alignment.
 */
bool SampleCalibration(ID3D12CommandQueue* queue);

/** Writes the capture's CpuTimeline section; nothing when no events were recorded. */
void SendCpuTimeline();

// ---------------------------------------------------------------------------------------------
// The waits, which happen in Win32 rather than in D3D12 (see the note above).

/**
 * Installs the hooks on the Win32 wait functions. Called once with the entry-point hooks. They
 * forward untouched unless a capture is running *and* the handle is one of the two kinds below, so
 * an application's own waits cost one relaxed atomic read.
 */
void InstallWaitHooks();

/** A handle `ID3D12Fence::SetEventOnCompletion` was given: a wait on it is waiting for the GPU. */
void NoteFenceEvent(HANDLE event);

/** A swapchain's frame-latency waitable object: a wait on it is waiting for the presenter. */
void NoteFrameLatencyEvent(HANDLE event);

// ---------------------------------------------------------------------------------------------
// Memory residency, the counterpart of the Vulkan layer's VK_EXT_memory_budget reporting (which
// also lives beside its CPU timeline).
//
// What the application allocated is in the object graph: an ID3D12Heap records its size and heap
// type, and a committed resource records the size the runtime gave its implicit heap. What the
// graph cannot know is how much of the GPU's memory is actually resident and how much this process
// is allowed — that counts every process, and only the driver can say. QueryVideoMemoryInfo is how
// it is asked.
//
// D3D12 has no memory-type table to enumerate the way Vulkan does. What it has is two memory
// segments — local (the GPU's own) and non-local (system memory it reaches over the bus) — and four
// heap types that draw from them, which is the shape reported here.

/**
 * The adapter's memory segments and the heap types that draw from them, as an update on the
 * adapter. Sent once, when the device is created.
 */
void SendMemoryProperties(ID3D12Device* device, IDXGIAdapter* adapter);

/**
 * The driver's budget and residency per segment, as an update on the adapter. Cheap enough for the
 * frame report's interval; quiet when the adapter is too old to answer (IDXGIAdapter3, Windows 10).
 */
void SendMemoryBudget(ID3D12Device* device);

/**
 * The size the runtime gave a committed resource's implicit heap, recorded on the resource so the
 * memory view can total it. Placed resources are not counted here: they live inside an ID3D12Heap
 * whose own size is already counted, and counting both would double every placed byte.
 */
void NoteCommittedAllocation(ID3D12Device* device, ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc,
                             D3D12_HEAP_TYPE heapType);

}  // namespace dxinsp
