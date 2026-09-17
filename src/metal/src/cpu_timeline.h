// The CPU side of a frame, on the same clock as the GPU side.
//
// The Metal counterpart of src/vulkan/src/cpu_timeline.h and src/d3d12/src/cpu_timeline.h, sending
// the same CaptureCpuTimeline message, because the app's "Where the CPU went" and Timeline cards do
// not care which API the frame came from. What differs is which calls a frame's CPU actually sits
// in, and Metal's answer is the shortest of the three:
//
//   - Submit is `commit`, as `vkQueueSubmit` is on Vulkan.
//   - Waiting for the GPU is `waitUntilCompleted` / `waitUntilScheduled` on a command buffer.
//   - Waiting for the display is `CAMetalLayer.nextDrawable`, which blocks once every drawable is
//     in flight. This is Metal's `vkAcquireNextImageKHR`, and it is where a display-paced frame
//     spends its time.
//
// There is deliberately no present category. Metal's `presentDrawable:` only schedules the present
// and returns at once, so timing it would record a call that never waits for anything. The display
// pacing is in nextDrawable instead, which the app already reads as waiting on the display, so the
// verdict comes out right without a present span.
//
// Relating the two clocks needs no extension: `[MTLDevice sampleTimestamps:gpuTimestamp:]` samples
// both at one instant, which is what capture.mm already uses to convert GPU ticks to nanoseconds.
#pragma once

#include <cstdint>

#import <objc/objc.h>

namespace mtlinsp {

/** What a CPU event was, in the words the timeline shows. The names match the other two backends'. */
enum class CpuCategory : uint16_t {
    Submit = 0,     // commit: handing work to the GPU
    WaitFences,     // waitUntilCompleted / waitUntilScheduled: blocked until the GPU caught up
    Acquire,        // nextDrawable: blocked until the presenter freed a drawable
    // Pipeline state and library creation, the synchronous forms only: those block the thread
    // that asked while the driver compiles, and a pipeline built inside a frame stops it. The
    // completion-handler forms are deliberately not timed — they return at once and compile
    // elsewhere, which is the pattern this is meant to point an application towards.
    PipelineCreate,
    Count,
};

/**
 * The start of a timed call, or 0 when nothing is being captured. Cheap enough to sit in every
 * commit and drawable: one relaxed atomic read, and while capturing one clock read.
 */
uint64_t CpuEventBegin();

/** The end of one: records it when a capture is running and `started` is not 0. */
void CpuEventEnd(uint64_t started, CpuCategory category);

/** Clears the recorded events and takes the capture's time origin; called when a capture starts. */
void BeginCpuTimeline();

/**
 * Relates the GPU clock to the host's, so pass timestamps can be placed on the CPU axis. Sampled
 * when the capture ends, while the device is still alive. False when the device is too old to
 * sample both (macOS 10.15), which the capture records rather than guessing an alignment.
 *
 * `nsPerTick` is the ratio capture.mm measured over the capture: on Apple Silicon it is one, and on
 * a discrete GPU the timestamp counter runs at its own rate.
 */
bool SampleCalibration(id device, double nsPerTick);

/** Writes the capture's CpuTimeline section; nothing when no events were recorded. */
void SendCpuTimeline();

// ---------------------------------------------------------------------------------------------
// Memory over time.
//
// Metal has no heap table to enumerate the way Vulkan does, and no separate residency figure: the
// device's own `currentAllocatedSize` is both what this process has allocated and what is resident,
// and `recommendedMaxWorkingSetSize` is what it should stay under. So the sample carries one heap,
// with the device's two numbers, which is enough for the series to have a shape — and the shape is
// what an instant cannot show (renderer/memory_timeline.ts).

/** One sample of the memory series. Called with the frame report; quiet without a device. */
void SendMemorySample(id device);

}  // namespace mtlinsp
