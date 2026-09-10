// Frame capture: the command stream of one or more frames, streamed to the UI, with the render
// targets, the bound buffers and the GPU time of every pass.
//
// The model is the Vulkan layer's (docs/ARCHITECTURE.md, "Frame capture"): the UI asks for a
// capture, the library arms at the next frame boundary, records every command of the frames that
// follow as they are encoded, and sends them once the GPU has finished the last of them. Nothing
// is replayed.
//
// Metal makes two parts of this easier than Vulkan. The frame boundary is a present rather than
// something to infer, and a compute pass is a real encoder rather than a run of dispatches that
// has to be bracketed by a heuristic. What Metal does not have is a command buffer that can be
// recorded once and submitted repeatedly, so there is no counterpart of the layer's "record
// always".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#import <Metal/Metal.h>
#import <objc/runtime.h>

namespace mtlinsp {

/** The UI's `Capture` message, with the Vulkan layer's defaults for what it leaves out. */
struct CaptureOptions {
    uint32_t frameCount = 1;
    /** Frame number to start at (frame_stats.h's counter); UINT64_MAX for the next boundary. */
    uint64_t atFrame = UINT64_MAX;
    uint64_t maxBufferSize = 64 * 1024;       // per captured buffer range (longer ranges are truncated)
    uint64_t maxBufferTotal = 512ull << 20;   // stop capturing buffers past this many bytes per capture
    uint64_t maxTextureSize = 256ull << 20;   // skip render targets larger than this
    bool captureTextures = true;
    bool captureBuffers = true;
    bool profilePasses = true;
    /** The call stack of every recorded command, symbolized by the UI on demand. */
    bool stacktraces = false;
};

/** Arms a capture, from the UI's `Capture` message. */
void RequestCapture(const CaptureOptions &options);

/**
 * Whether commands should be recorded right now. Checked on every intercepted encoder call.
 * False while the library issues Metal calls of its own (see Internal in swizzle.h).
 */
bool Recording();

// --------------------------------------------------------------------------------------------
// What the recorded commands are attributed to.
//
// Every recorded command names its command buffer, the way a Vulkan capture does: that is what
// the UI groups the tree by, numbers passes within, and matches render targets and timings to. A
// command buffer lives for a frame, so it is announced as an object only while a capture is
// recording and dropped, through the tracker's dealloc hook, when the application releases it.
// Encoders are named too, with an id from the same sequence, but never announced.

/** The tracked id of a command buffer, tracking it first if this is the first sighting. */
uint64_t CommandBufferId(id commandBuffer);

/**
 * Remembers which command buffer an encoder belongs to. Called for every encoder, recording or
 * not, because the answer is also what the frame-boundary logic and the read-back need. An
 * encoder a parallel render encoder handed out names it as `parent`: it shares that pass, and
 * its own end is not the pass's.
 */
void RegisterEncoder(id encoder, id commandBuffer, const char *type, id parent);
/** The encoder is closed: nothing more will be recorded against it. */
void ForgetEncoder(id encoder);
/** The command buffer a registered encoder belongs to, or nil. */
id EncoderCommandBuffer(id encoder, bool *secondary);
/** Whether an object is an encoder or a command buffer: something whose label is a command. */
bool IsCommandStreamObject(id object);

// --------------------------------------------------------------------------------------------
// Recording

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
 * Queues a bound buffer range to be read back with the capture, and returns the id the command
 * should carry in `bufferData` so the UI can find the contents (0 when it cannot be read).
 *
 * A buffer in shared storage is mapped into the process the whole time, so this is a memcpy at
 * bind time. A managed buffer's CPU copy may be stale if the GPU wrote it, so it is synchronized
 * at the end of the pass and read after the frame completes; a private buffer has no CPU copy at
 * all and is blitted into staging at the end of the pass, the way the Vulkan layer does for every
 * buffer. `encoder` is the encoder the bind was made on, whose end is where those blits go.
 *
 * The same range bound twice in a capture — a uniform block bound at every draw — is read once.
 */
uint64_t QueueBufferCapture(id encoder, id buffer, uint64_t offset, uint64_t size);

/** Queues inline bytes (`setVertexBytes:` and friends) as a CaptureBuffers entry with no buffer. */
uint64_t QueueBytesCapture(const void *bytes, uint64_t size);

// --------------------------------------------------------------------------------------------
// Passes

enum class PassKind { Render, Compute, Blit, Other };

/**
 * Where a pass's GPU timestamps go, decided before the encoder exists because a render or
 * compute pass takes them through its descriptor.
 */
struct PassTimingSlot {
    id sampleBuffer = nil;          // id<MTLCounterSampleBuffer>, or nil for no timing
    uint32_t startIndex = 0;
    uint32_t endIndex = 0;
    /** A render pass sampled at all four stage boundaries: the vertex end and fragment start. */
    uint32_t vertexEndIndex = UINT32_MAX;
    uint32_t fragmentStartIndex = UINT32_MAX;
    /** The statistic counter set (invocations), sampled beside the timestamps; nil without. */
    id statisticBuffer = nil;
    uint32_t statisticStart = 0;
    uint32_t statisticEnd = 0;
    /** The stage-utilization counter set (cycles per stage); nil without. */
    id utilizationBuffer = nil;
    uint32_t utilizationStart = 0;
    uint32_t utilizationEnd = 0;
    /** Sampled by the encoder at its beginning and end rather than by the pass descriptor. */
    bool onEncoder = false;
};

/**
 * Reserves timestamp samples for a pass about to begin, and fills the descriptor's sample buffer
 * attachment when the device samples at stage boundaries. A descriptor of nil reserves for the
 * encoder-boundary path. Returns a slot with no buffer when timing is off or exhausted.
 */
PassTimingSlot ReserveRenderPassTiming(id commandBuffer, MTLRenderPassDescriptor *descriptor);
PassTimingSlot ReserveComputePassTiming(id commandBuffer, MTLComputePassDescriptor *descriptor);
PassTimingSlot ReserveBlitPassTiming(id commandBuffer, MTLBlitPassDescriptor *descriptor);

/**
 * Registers a pass beginning, and returns the pass index the UI will give it: its ordinal among
 * the passes of its command buffer, render, compute and blit alike, because the UI's PASS_BEGIN
 * set holds all three and it numbers them in one sequence per command buffer.
 */
uint32_t BeginPass(id encoder, id commandBuffer, PassKind kind, const PassTimingSlot &timing);

/**
 * Notes an attachment of the render pass just begun, for read-back at endEncoding. Multisample
 * attachments are read through their resolve texture; what cannot be read is reported with a
 * reason rather than dropped.
 */
void AddPassAttachment(id encoder, MTLRenderPassAttachmentDescriptor *attachment,
                       uint32_t index, bool depth);

/** Just before the application's endEncoding is forwarded: the end-of-pass timestamp. */
void BeforeEndEncoding(id encoder);
/**
 * Just after it: the read-back blits. A command buffer allows one encoder at a time, so the
 * blit encoder can only be created once the application's is really closed.
 */
void AfterEndEncoding(id encoder);

// --------------------------------------------------------------------------------------------
// Frame boundaries
//
// There are two ways an application presents, and an engine may use either.
//
// `[MTLCommandBuffer presentDrawable:]` is the documented convenience, and its boundary is the
// `commit` that follows it, not the call itself: Metal presents by asking a command buffer to,
// partway through encoding it, with the commit after.
//
// `[MTLDrawable present]` is the other, and Unity's macOS player uses it, from a scheduled
// handler of the command buffer rather than through it. That arrives on Metal's callback thread
// after the commit, so taking it as the boundary starts and ends a capture partway into the next
// frame's encoding. Once an application has been seen to present that way, the boundary is
// taken earlier and on the encoding thread instead: at the commit of the command buffer that
// rendered into the drawable's texture, which is the one whose handler will present it. The
// present itself then only confirms a frame already counted.

/** From `nextDrawable`: the texture a frame will render into, and which drawable owns it. */
void OnDrawableAcquired(id drawable, id texture);

/** From the render-encoder hook, recording or not: the command buffer draws into this texture. */
void OnRenderTarget(id commandBuffer, id texture);

/** Notes that this command buffer will present, so its commit is the end of a frame. */
void OnPresentDrawable(id commandBuffer, id drawable);

/**
 * A frame boundary from the drawable's own `present`. Returns false when the frame was already
 * counted — by the convenience method calling through, or at the commit that rendered into it —
 * so the caller leaves it out of the log as well as out of the frame count.
 */
bool OnDrawablePresent(id drawable);

/**
 * Installs the logger for a frame that ends at a commit rather than at a present call.
 *
 * The two present hooks report their own boundary. Once an application presents its drawables
 * itself, though, the frame ends at the commit that rendered into one, and nothing there said so:
 * the log fell silent after the first frame while the counting stayed correct. A log that goes
 * quiet is how the Unity present path stayed undiagnosed, so this closes the same gap.
 */
void SetCommitBoundaryLogger(void (*logger)(id commandBuffer));

/**
 * The frame boundary: closes a recording frame, arms a pending capture, sends a finished one.
 * Called from `commit`, before it is forwarded. Records the commit itself, so that the present
 * marker of a frame ending here comes before it.
 */
void OnCommit(id commandBuffer);

}  // namespace mtlinsp
