// Frame timing, reported to the UI ten times a second: the counterpart of the Vulkan layer's
// FrameStats message (layer/src/layer.cpp, EndFrame), which feeds the session bar's frame-time
// meter and Frame Stats' Frame Bound card. Same fields, same cadence, so the UI needs nothing.
//
// The frame counter here is the one a queued capture names ("capture at frame N"): it advances
// at every frame boundary the capture logic decides (capture.mm), which is a present on either
// of Metal's two paths.
#pragma once

#include <cstdint>

#import <objc/objc.h>

namespace mtlinsp {

/** A frame ended. Returns its number: the first frame to end is 1. */
uint64_t OnFrameEnded();

/** The number of frames that have ended so far. */
uint64_t FrameNumber();

/** CPU time spent inside a commit, from the hook, for the report's submit time. */
void AddSubmitTime(uint64_t nanoseconds);

/**
 * From the layer hook: whether the layer presents in step with the display. A layer with
 * display sync off is Vulkan's immediate mode, and no refresh rate is reported for it.
 */
void NoteDisplaySync(bool enabled);

/**
 * The device whose `currentAllocatedSize` the report carries: the first one tracked. A process
 * with several devices reports the first; a Mac has one GPU that draws.
 */
void NoteDevice(id device);

}  // namespace mtlinsp
