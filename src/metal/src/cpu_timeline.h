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
// Timing capture.
//
// Where a frame capture keeps every timed call of one frame, a timing capture keeps each frame's
// *totals* over minutes: the wall time and how much of it went to each category. That is what says
// "the frame that hitched spent 38 of its 40 milliseconds waiting on a fence", and it is what the
// app's Timing view reads (a hitch, rather than a slow frame — docs/PROFILING.md).
//
// The same shape as src/d3d12/src/cpu_timeline.h's: a ring of frames, batched out on the frame
// report's interval, with the oldest dropped once it is full — a timing capture left running should
// not grow without bound, and what matters is the recent minutes.
//
// `sampleHz` is accepted and ignored: sampling every thread's call stack is Windows-only
// (src/vulkan/src/cpu_sampler.h), so on Metal a timing capture records where the frame's *calls*
// went and not what the threads were doing between them.

/** Starts recording per-frame timings, discarding anything a previous run held. */
void BeginTimingCapture(uint32_t sampleHz = 0);
/** Stops recording. What was taken stays until the next Begin. */
void EndTimingCapture();
/** Whether a timing capture is running. */
bool TimingCaptureRunning();

/**
 * Closes off the frame that just ended, with its wall time: called from the frame boundary
 * (frame_stats.mm). Quiet unless a timing capture is running.
 */
void NoteFrameTiming(uint32_t frame, double frameMs);

/** Sends the frames recorded since the last call, if any. Called with the frame report. */
void SendTimingFrames();

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
