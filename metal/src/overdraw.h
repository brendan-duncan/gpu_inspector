// Measurements taken while capturing, by drawing a render pass again right after the application
// ends its encoder, in the same command buffer, with the application's own buffers, textures and
// argument buffers as it bound them. The library records each render encoder call as a closure
// while such a capture records (hooks_encoders.mm) and runs those closures against its own
// encoders. Two measurements use them:
//
// Overdraw (overdraw.mm): every pipeline is replaced by a copy whose fragment function writes 1.0
// into an R16Float target blended additively, so each pixel ends up holding how many fragments
// landed on it. Twice per pass:
//   * with the pass's depth and stencil tests, against the depth and stencil the pass started
//     from (a copy taken before it began, when it loads them; its clear values otherwise): the
//     fragments that passed, in draw order;
//   * without depth and stencil: every fragment the draws rasterized.
//
// Pixel history (pixel_history.mm): one pixel of one texture, followed through every pass that
// renders to it, one draw at a time; see the file.
//
// The Vulkan counterparts are vkinsp_replay --overdraw and --pixel (docs/REPLAY.md), which have to
// rebuild the frame from a capture file first. A pipeline state or depth-stencil state cannot be
// copied, only its descriptor, so every render pipeline's and depth-stencil state's descriptor is
// kept from its creation until the object is released (RememberRenderPipeline).
#pragma once

#include "capture.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#import <Metal/Metal.h>

namespace mtlinsp {

/**
 * An Objective-C object held strongly by a copyable C++ value. The library is built without ARC,
 * so a closure capturing an `id` would not keep the object alive; one capturing a Strong does.
 */
class Strong {
public:
    Strong() = default;
    explicit Strong(id object) : object_([object retain]) {}
    Strong(const Strong &other) : object_([other.object_ retain]) {}
    Strong(Strong &&other) noexcept : object_(other.object_) { other.object_ = nil; }
    Strong &operator=(Strong other) noexcept {
        std::swap(object_, other.object_);
        return *this;
    }
    ~Strong() { [object_ release]; }
    id get() const { return object_; }

private:
    id object_ = nil;
};

/** An array of objects (setVertexBuffers:, useResources:), held strongly. */
class StrongList {
public:
    StrongList(const id *objects, NSUInteger count) {
        for (NSUInteger i = 0; objects != nullptr && i < count; i++) objects_.push_back([objects[i] retain]);
        if (objects == nullptr) objects_.assign(count, nil);
    }
    StrongList(const StrongList &other) : objects_(other.objects_) {
        for (id o : objects_) [o retain];
    }
    StrongList &operator=(const StrongList &) = delete;
    ~StrongList() {
        for (id o : objects_) [o release];
    }
    /** The objects as the pointer the array form of a call takes. */
    const id *data() const { return objects_.data(); }

private:
    std::vector<id> objects_;
};

/**
 * A measurement's encoder, as the recorded calls see it. The calls a measurement changes go
 * through here; the rest are issued as the application made them.
 */
class OverdrawReplay {
public:
    /** The application bound a pipeline. */
    virtual void BindPipeline(id<MTLRenderCommandEncoder> encoder, id state) = 0;
    virtual void SetDepthStencilState(id<MTLRenderCommandEncoder> encoder, id state) = 0;
    virtual void SetCullMode(id<MTLRenderCommandEncoder> encoder, NSUInteger mode) {
        [encoder setCullMode:(MTLCullMode)mode];
    }
    virtual void SetScissorRects(id<MTLRenderCommandEncoder> encoder, const MTLScissorRect *rects, NSUInteger count) {
        if (count == 1) [encoder setScissorRect:rects[0]];
        else [encoder setScissorRects:rects count:count];
    }
    /** A draw, which the measurement issues through `draw` as many times as it needs, or not at all. */
    virtual void IssueDraw(id<MTLRenderCommandEncoder> encoder, const std::function<void(id<MTLRenderCommandEncoder>)> &draw) = 0;
    /** A draw that cannot be measured at all (an indirect command buffer's). */
    virtual void Skip() = 0;

protected:
    ~OverdrawReplay() = default;
};

/** A render encoder call recorded while capturing, issued again against a measurement's encoder. */
using OverdrawOp = std::function<void(id<MTLRenderCommandEncoder> encoder, OverdrawReplay &replay)>;

/**
 * What a recorded call sets, so a measurement that needs the encoder's state at one draw can issue
 * only the calls still in effect there rather than every call before it (pixel_history.mm):
 *   Draw        a draw (or indirect command buffer execution): it sets no state;
 *   Replace     sets the state `name` names outright, undoing any earlier call with that name;
 *   Slot        binds `name` slot `location` (a buffer, bytes, texture or sampler), undoing earlier
 *               binds and offsets of that slot;
 *   SlotOffset  changes the offset of `name` slot `location`, undoing earlier offsets of it;
 *   Range       binds `name` slots location..location+length-1, undoing earlier binds of each and
 *               an earlier bind of exactly the same range.
 */
enum class OpPolicy : uint8_t { Draw, Replace, Slot, SlotOffset, Range };

struct OpKey {
    OpPolicy policy = OpPolicy::Replace;
    std::string name;
    uint32_t location = 0;
    uint32_t length = 1;

    static OpKey DrawCall() { return {OpPolicy::Draw, {}, 0, 0}; }
    static OpKey Replace(std::string name) { return {OpPolicy::Replace, std::move(name), 0, 1}; }
    static OpKey Slot(std::string family, NSUInteger index) { return {OpPolicy::Slot, std::move(family), (uint32_t)index, 1}; }
    static OpKey Offset(std::string family, NSUInteger index) { return {OpPolicy::SlotOffset, std::move(family), (uint32_t)index, 1}; }
    static OpKey Range(std::string family, NSRange range) {
        return {OpPolicy::Range, std::move(family), (uint32_t)range.location, (uint32_t)range.length};
    }
};

/** Whether a capture that records passes for a measurement is recording. Checked before a hook builds its closure. */
bool OverdrawActive();

/**
 * Records a call made on a render encoder (or on an encoder a parallel render encoder handed out)
 * of a pass being measured, with the command index the capture recorded it as. Dropped when the
 * encoder's pass is not measured.
 */
void LogOverdrawOp(id encoder, OpKey key, OverdrawOp op);

struct OverdrawPass;

/**
 * Before the application's render encoder is created: whether this pass will be measured, with
 * what each measurement needs copied before the pass can change it. Null when no measurement of
 * the capture concerns the pass. `descriptor` is the pass descriptor the encoder is made from.
 */
std::shared_ptr<OverdrawPass> PrepareOverdrawPass(id commandBuffer, MTLRenderPassDescriptor *descriptor);

/** The application's encoder exists: the pass's calls are recorded against it from here on. */
void BeginOverdrawPass(id encoder, std::shared_ptr<OverdrawPass> pass, uint32_t passIndex);

/** The command the capture recorded the pass's beginning as. */
void NotePassBeginCommand(id encoder, uint32_t command);

/** A parallel render encoder handed out a sub-encoder: its calls are drawn in an encoder of their own, in creation order. */
void NoteOverdrawSubEncoder(id parent, id encoder);

/** After the application's endEncoding has been forwarded: draws the measurements into the command buffer. */
void EndOverdrawPass(id encoder);

/** A render pipeline was created: its descriptor is kept, since the measurements' copies are made from it. */
void RememberRenderPipeline(id state, id descriptor);

/** A depth-stencil state was created: its descriptor is kept, for the pixel history's test copies. */
void RememberDepthStencilState(id state, MTLDepthStencilDescriptor *descriptor);

/** An object is being deallocated: drops what was kept of a pipeline or depth-stencil state, and its copies. */
void ForgetRenderPipeline(id object);

/**
 * A capture starts recording: whether it measures overdraw, whether it records passes at all (for
 * overdraw or a pixel history), with nothing left from the last one. Counts larger than
 * `maxDataSize` bytes are reported without their pixels.
 */
void StartOverdrawCapture(bool overdraw, bool recordPasses, uint64_t maxDataSize);

/**
 * The capture's command buffers have completed: the counts are read, and sent as CaptureOverdraw
 * plus a CaptureOverdrawData binary frame per measurement. Not under the capture's lock.
 */
void SendOverdraw();

/** A capture starts recording: the pixel it follows, if any (pixel_history.mm). */
void StartPixelHistoryCapture(const PixelHistoryRequest &request);

/** The capture's command buffers have completed: the pixel's history is sent as CapturePixelHistory. */
void SendPixelHistory();

}  // namespace mtlinsp
