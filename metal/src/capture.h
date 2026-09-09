// Frame capture: the command stream of one or more frames, streamed to the UI.
//
// The model is the Vulkan layer's (docs/ARCHITECTURE.md, "Frame capture"): the UI asks for a
// capture, the library arms at the next frame boundary, records every command of the frames that
// follow as they are encoded, and sends them when the last one presents. Nothing is replayed.
//
// Metal makes two parts of this easier than Vulkan. The frame boundary is `presentDrawable:`
// rather than something to infer, and a compute pass is a real encoder rather than a run of
// dispatches that has to be bracketed by a heuristic. What Metal does not have is a command
// buffer that can be recorded once and submitted repeatedly, so there is no counterpart of the
// layer's "record always".
//
// Resource read-back is not implemented yet: this records the commands, not the pixels.
#pragma once

#include <cstdint>
#include <string>

#import <objc/runtime.h>

namespace mtlinsp {

struct CaptureOptions {
    uint32_t frameCount = 1;
};

/** Arms a capture, from the UI's `Capture` message. */
void RequestCapture(const CaptureOptions &options);

/** Whether commands should be recorded right now. Checked on every intercepted encoder call. */
bool Recording();

/**
 * Records one command of the frame being captured.
 *
 * `method` is the selector, `object` the encoder or command buffer it was called on, and
 * `argsJson` its arguments already serialized, or empty.
 */
void RecordCommand(const char *method, id object, const std::string &argsJson);

/** Notes that this command buffer will present, so its commit is the end of a frame. */
void OnPresentDrawable(id commandBuffer);

/**
 * The frame boundary: closes a recording frame, arms a pending capture, sends a finished one.
 *
 * Called from `commit`, not from `presentDrawable:`. Metal presents by asking a command buffer to,
 * partway through encoding it, and the commit comes after — so treating `presentDrawable:` itself
 * as the boundary starts a capture in the middle of a command buffer and its first recorded
 * command is the previous frame's `commit`. Vulkan does not have this problem, because
 * vkQueuePresentKHR is a queue operation that follows the submission it presents.
 */
void OnCommit(id commandBuffer);

}  // namespace mtlinsp
