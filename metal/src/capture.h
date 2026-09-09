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
#include <vector>

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
 * Queues a bound buffer range to be read back with the capture, and returns the id the command
 * should carry in `bufferData` so the UI can find the contents (0 when it cannot be read).
 *
 * Cheaper than the Vulkan layer's equivalent, which records a GPU copy into staging: a Metal
 * buffer in a shared or managed storage mode is mapped into the process the whole time, so this
 * is a memcpy. A private-storage buffer has no such pointer and is reported as an error rather
 * than blitted, for now.
 */
uint64_t QueueBufferCapture(id buffer, uint64_t offset, uint64_t size);

/**
 * Records one command of the frame being captured.
 *
 * `method` is the selector, `object` the encoder or command buffer it was called on, and
 * `argsJson` its arguments already serialized, or empty.
 */
void RecordCommand(const char *method, id object, const std::string &argsJson);

/** As RecordCommand, with the CaptureBuffers ids of the ranges the command bound. */
void RecordCommandWithBuffers(const char *method, id object, const std::string &argsJson,
                              std::vector<uint64_t> bufferData);

/**
 * Suppresses recording while the library issues Metal calls of its own.
 *
 * Read-back needs a blit encoder, and creating one goes through the same hooks the application
 * does — without this the capture would contain the commands the capture made.
 */
class Internal {
public:
    Internal();
    ~Internal();
};

/**
 * Registers a render pass beginning, and returns the pass index the UI will give it.
 *
 * Every encoder counts, render and compute alike, because the UI's PASS_BEGIN set holds both and
 * it numbers them in one sequence per command buffer.
 */
uint32_t BeginPass(id encoder, id commandBuffer);

/** Notes an attachment of the pass just begun, for read-back at endEncoding. */
void AddPassAttachment(id encoder, id texture, uint32_t attachment);

/** Reads the pass's colour attachments back, at endEncoding, into staging buffers. */
void EndRenderPass(id encoder);

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
