// Handing a frame to Xcode: a .gputrace document written by Metal's own capture manager, on
// request from the UI, for the next frame.
//
// Xcode's shader debugger and per-line shader profiler cannot be reproduced outside Apple's
// tooling; what can be done is to give them the frame the inspector is looking at.
// MTLCaptureManager writes a GPU trace document from inside the process, provided the process
// was started with METAL_CAPTURE_ENABLED=1 (the launch path sets it). The capture starts at the
// next frame boundary and stops at the one after, so the document holds exactly one frame.
#pragma once

#include <string>

#import <objc/runtime.h>

namespace mtlinsp {

/**
 * Asks for the next frame as a .gputrace at `path`, or at a default beside the Desktop when
 * empty. Answered with a `GpuTrace` message once written, or once it could not be.
 */
void RequestGpuTrace(const std::string &path);

/** A frame boundary, with the device the frame ran on: starts or stops the trace. */
void GpuTraceAtFrameBoundary(id device);

}  // namespace mtlinsp
