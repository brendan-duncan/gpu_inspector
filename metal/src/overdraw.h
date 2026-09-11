// Overdraw, measured while capturing: each render pass is drawn a second time, right after the
// application ends its encoder and in the same command buffer, with every pipeline replaced by a
// copy whose fragment function writes 1.0 into an R16Float target blended additively. Each pixel
// ends up holding how many fragments landed on it. Twice per pass:
//   * with the pass's depth and stencil tests, against the depth and stencil the pass started
//     from (a copy taken before it began, when it loads them; its clear values otherwise): the
//     fragments that passed, in draw order;
//   * without depth and stencil: every fragment the draws rasterized.
//
// The Vulkan counterpart is vkinsp_replay --overdraw (docs/REPLAY.md), which has to rebuild the
// frame from a capture file first. Here nothing is rebuilt: the draws are issued again with the
// application's own buffers, textures and argument buffers, as it bound them, because the library
// records each render encoder call as a closure while the capture is recording (hooks_encoders.mm)
// and runs those closures against its own encoder.
//
// A pipeline state cannot be copied, only its descriptor, so every render pipeline's descriptor is
// kept from its creation until the pipeline is released (RememberRenderPipeline).
#pragma once

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

/** One of the two passes an overdraw measurement draws, as the recorded calls see it. */
class OverdrawReplay {
public:
    /** The pipeline bound from here on: its counting copy, or nothing when there is none. */
    virtual void BindPipeline(id<MTLRenderCommandEncoder> encoder, id state) = 0;
    /** Whether the depth-stencil state the application sets applies (the pass tests depth or stencil). */
    virtual bool TestsDepthStencil() const = 0;
    /** Counts a draw, and says whether to issue it: false when no counting pipeline is bound. */
    virtual bool Draw() = 0;
    /** A draw that cannot be measured at all (an indirect command buffer's). */
    virtual void Skip() = 0;

protected:
    ~OverdrawReplay() = default;
};

/** A render encoder call recorded while capturing, issued again against the measurement's encoder. */
using OverdrawOp = std::function<void(id<MTLRenderCommandEncoder> encoder, OverdrawReplay &replay)>;

/** Whether a capture that measures overdraw is recording. Checked before a hook builds its closure. */
bool OverdrawActive();

/**
 * Records a call made on a render encoder (or on an encoder a parallel render encoder handed out)
 * of the pass being measured. Dropped when the encoder's pass is not measured.
 */
void LogOverdrawOp(id encoder, OverdrawOp op);

struct OverdrawPass;

/**
 * Before the application's render encoder is created: whether this pass will be measured, and
 * the copies of its depth and stencil attachments as they are before it begins. Null when the
 * capture does not measure overdraw. `descriptor` is the pass descriptor the encoder is made from.
 */
std::shared_ptr<OverdrawPass> PrepareOverdrawPass(id commandBuffer, MTLRenderPassDescriptor *descriptor);

/** The application's encoder exists: the pass's calls are recorded against it from here on. */
void BeginOverdrawPass(id encoder, std::shared_ptr<OverdrawPass> pass, uint32_t passIndex);

/** A parallel render encoder handed out a sub-encoder: its calls are drawn in an encoder of their own, in creation order. */
void NoteOverdrawSubEncoder(id parent, id encoder);

/** After the application's endEncoding has been forwarded: draws the measurement into the command buffer. */
void EndOverdrawPass(id encoder);

/** A render pipeline was created: its descriptor is kept, since a counting copy is made from it. */
void RememberRenderPipeline(id state, id descriptor);

/** An object is being deallocated: drops the descriptor and counting copies of a pipeline. */
void ForgetRenderPipeline(id object);

/**
 * A capture starts recording: whether it measures overdraw, with nothing left from the last one.
 * Counts larger than `maxDataSize` bytes are reported without their pixels.
 */
void StartOverdrawCapture(bool enabled, uint64_t maxDataSize);

/**
 * The capture's command buffers have completed: the counts are read, and sent as CaptureOverdraw
 * plus a CaptureOverdrawData binary frame per measurement. Not under the capture's lock.
 */
void SendOverdraw();

}  // namespace mtlinsp
