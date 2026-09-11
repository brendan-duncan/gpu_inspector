// Pixel history, measured while capturing: one pixel of one texture followed through every render
// pass of the capture that renders to it, the way vkinsp_replay --pixel follows one through a
// replayed Vulkan frame (replay/src/history.cpp). The UI names the pixel from an earlier capture,
// and the next frame is the one followed.
//
// For each pass that renders to the texture:
//   * Before the pass begins, every attachment is copied at the pixel into a texture of the
//     library's own (the "shadows"), when the attachment loads.
//   * After the application ends the encoder, in the same command buffer, the shadows are cleared
//     or loaded the way the pass's attachments were, and the pixel is read: the pass's start.
//   * Then, for every draw of the pass, an encoder of the library's own over the shadows issues the
//     calls still in effect at the draw (OpKey in overdraw.h: the last pipeline, the last bind of
//     each slot, ...), a one-pixel scissor, and the draw six times under visibility results in
//     counting mode, with copies of its state that add one step each: its primitives with no culling
//     and no tests (covered), with its cull mode (facing), with its fragment function (shaded, so a
//     discard shows), with its depth test, its stencil test, and both. Nothing is written by those.
//     Then the draw itself, with the application's pipeline and depth-stencil state, and the pixel is
//     read again.
// Cull mode and the depth-stencil state are encoder state in Metal, so only the two pipeline copies
// (a fragment function that writes nothing, and the application's with colour writes off) are made.
//
// Not followed: multisampled and layered passes, and indirect command buffers' draws (their
// commands carry their own pipelines).
#include "overdraw.h"

#include "capture.h"
#include "formats.h"
#include "json_writer.h"
#include "pass_record.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <list>
#include <mutex>
#include <string>
#include <vector>

namespace mtlinsp {

/** A pixel followed through one pass: where, and the copies of its attachments it is drawn into. */
struct HistoryPass {
    /** The texture followed: the request's, or the drawable the frame rendered into instead. */
    uint64_t texture = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    /** The colour attachment the pixel is read from. */
    int target = -1;
    /** Why the pass is not followed. */
    std::string note;
    struct Attachment {
        id<MTLTexture> shadow = nil;   // retained
        MTLPixelFormat format = MTLPixelFormatInvalid;
        MTLLoadAction loadAction = MTLLoadActionDontCare;
        /** The shadow holds the attachment's pixel from before the pass (it loads). */
        bool loads = false;
        MTLClearColor clearColor = MTLClearColorMake(0, 0, 0, 0);
        double clearDepth = 1.0;
        uint32_t clearStencil = 0;
    };
    Attachment colors[8];
    Attachment depth;
    Attachment stencil;
    bool combined = false;

    ~HistoryPass() {
        for (Attachment &a : colors) [a.shadow release];
        [depth.shadow release];
        [stencil.shadow release];
    }
};

namespace {

constexpr int kVariants = 6;
/** Draws followed per pass: two encoders each, in the application's command buffer. */
constexpr uint32_t kMaxDraws = 1024;

struct PendingEvent {
    std::string kind;
    std::string method;
    std::string detail;
    uint32_t command = 0;
    uint64_t commandBuffer = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    uint64_t pipeline = 0;
    bool scissored = false;
    uint32_t testsMeasured = 0;
    int64_t query = -1;       // the draw's visibility result buffer, in PendingHistory::visibility
    int64_t slot = -1;        // where the pixel after the event is in the staging buffer
};

/** One followed pass drawn into a command buffer, waiting for it to complete. */
struct PendingHistory {
    uint64_t texture = 0;
    std::vector<PendingEvent> events;
    std::vector<std::string> notes;
    std::string pixelFormat;
    std::string depthFormat;
    uint32_t colorBytes = 0;
    uint32_t depthBytes = 0;
    id<MTLBuffer> staging = nil;      // retained
    /**
     * One visibility result buffer per followed draw, retained: an encoder resets the visibility
     * results when it is created, and one buffer shared by every draw's encoder could lose the
     * counts of the draws before.
     */
    std::vector<id<MTLBuffer>> visibility;
    std::string device;
    /** The shadows and pipeline copies the encoders use, kept until the command buffer completes. */
    std::vector<Strong> keep;
};

std::mutex g_mutex;
PixelHistoryRequest g_request;
/** Whether the request's texture is (or was) a drawable, so that whichever drawable the frame renders into is followed. */
bool g_resolved = false;
bool g_anyDrawable = false;
std::vector<PendingHistory> g_pending;

/**
 * The calls in effect at a point of an encoder: each call recorded, less those a later call undid
 * (OpKey). Issued at a draw, they leave an encoder of the library's own in the state the
 * application's was in there, without every call before it.
 */
class StateSnapshot {
public:
    void Apply(const LoggedOp &op) {
        const OpKey &k = op.key;
        auto drop = [&](auto undone) {
            for (auto it = ops_.begin(); it != ops_.end();) {
                if (undone((*it)->key)) it = ops_.erase(it);
                else ++it;
            }
        };
        switch (k.policy) {
            case OpPolicy::Draw:
                return;
            case OpPolicy::Replace:
                drop([&](const OpKey &o) { return o.policy == OpPolicy::Replace && o.name == k.name; });
                break;
            case OpPolicy::Slot:
                drop([&](const OpKey &o) {
                    return (o.policy == OpPolicy::Slot || o.policy == OpPolicy::SlotOffset) && o.name == k.name && o.location == k.location;
                });
                break;
            case OpPolicy::SlotOffset:
                drop([&](const OpKey &o) { return o.policy == OpPolicy::SlotOffset && o.name == k.name && o.location == k.location; });
                break;
            case OpPolicy::Range:
                drop([&](const OpKey &o) {
                    if (o.name != k.name) return false;
                    if (o.policy == OpPolicy::Range) return o.location == k.location && o.length == k.length;
                    return (o.policy == OpPolicy::Slot || o.policy == OpPolicy::SlotOffset) && o.location >= k.location
                        && o.location < k.location + k.length;
                });
                break;
        }
        ops_.push_back(&op);
    }
    const std::list<const LoggedOp *> &ops() const { return ops_; }

private:
    std::list<const LoggedOp *> ops_;
};

/** The pixel history's encoder, as the recorded calls see it: the state they set is noted, and a draw is kept rather than issued. */
class HistoryReplay final : public OverdrawReplay {
public:
    id pipeline = nil;
    id depthStencil = nil;
    NSUInteger cullMode = MTLCullModeNone;
    bool hasScissor = false;
    MTLScissorRect scissor{};
    std::function<void(id<MTLRenderCommandEncoder>)> draw;
    bool skipped = false;

    void BindPipeline(id<MTLRenderCommandEncoder> encoder, id state) override {
        pipeline = state;
        if (state != nil) [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)state];
    }
    void SetDepthStencilState(id<MTLRenderCommandEncoder> encoder, id state) override {
        depthStencil = state;
        [encoder setDepthStencilState:(id<MTLDepthStencilState>)state];
    }
    void SetCullMode(id<MTLRenderCommandEncoder> encoder, NSUInteger mode) override {
        cullMode = mode;
        [encoder setCullMode:(MTLCullMode)mode];
    }
    /** Noted only: the history's scissor is the pixel. */
    void SetScissorRects(id<MTLRenderCommandEncoder>, const MTLScissorRect *rects, NSUInteger count) override {
        hasScissor = count > 0 && rects != nullptr;
        if (hasScissor) scissor = rects[0];
    }
    void IssueDraw(id<MTLRenderCommandEncoder>, const std::function<void(id<MTLRenderCommandEncoder>)> &d) override { draw = d; }
    void Skip() override { skipped = true; }
};

std::string Hex(const uint8_t *bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; i++) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

}  // namespace

// --------------------------------------------------------------------------------------------

void StartPixelHistoryCapture(const PixelHistoryRequest &request) {
    std::vector<PendingHistory> pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pending.swap(g_pending);
        g_request = request;
        // Resolved at the first pass: whether the texture is a drawable asks the capture, whose
        // lock the caller holds.
        g_resolved = false;
        g_anyDrawable = false;
    }
    for (PendingHistory &h : pending) {
        [h.staging release];
        for (id<MTLBuffer> b : h.visibility) [b release];
    }
    if (request.enabled) {
        Log("pixel history: following pixel (%u, %u) of texture %llu, level %u, slice %u", request.x, request.y,
            (unsigned long long)request.texture, request.level, request.slice);
    }
}

int MatchPixelHistoryAttachment(MTLRenderPassDescriptor *descriptor) {
    PixelHistoryRequest request;
    bool resolved = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
        resolved = g_resolved;
    }
    if (!request.enabled || descriptor == nil) return -1;
    if (!resolved) {
        // A texture the frame no longer has, or a drawable: the frame renders into a drawable of its
        // own, which is the one to follow.
        id texture = request.texture != 0 ? LiveObject(request.texture) : nil;
        const bool anyDrawable = texture == nil || IsDrawableTexture(texture);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_anyDrawable = anyDrawable;
        g_resolved = true;
    }
    bool anyDrawable = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        anyDrawable = g_anyDrawable;
    }
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = descriptor.colorAttachments[i];
        id<MTLTexture> t = a.texture;
        if (t == nil) continue;
        const bool same = request.texture != 0 && IdOf(t) == request.texture;
        if (!same && !(anyDrawable && IsDrawableTexture(t))) continue;
        const NSUInteger slice = t.textureType == MTLTextureType3D ? a.depthPlane : a.slice;
        if (a.level != request.level || slice != request.slice) continue;
        return (int)i;
    }
    return -1;
}

void PreparePixelHistory(OverdrawPass &pass, id commandBuffer, MTLRenderPassDescriptor *descriptor, int attachment) {
    PixelHistoryRequest request;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
    }
    auto h = std::make_shared<HistoryPass>();
    pass.history = h;
    h->target = attachment;
    h->x = request.x;
    h->y = request.y;
    MTLRenderPassColorAttachmentDescriptor *target = descriptor.colorAttachments[attachment];
    h->texture = IdOf(target.texture);
    h->width = (uint32_t)std::max<NSUInteger>(1, target.texture.width >> target.level);
    h->height = (uint32_t)std::max<NSUInteger>(1, target.texture.height >> target.level);
    if (h->x >= h->width || h->y >= h->height) {
        h->note = "the pixel is outside the pass's render target";
        return;
    }
    if (pass.multisampled) {
        h->note = "a multisampled pass is not followed yet";
        return;
    }
    if (pass.layered) {
        h->note = "a layered pass is not followed yet";
        return;
    }

    Internal internal;
    id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)commandBuffer;
    id<MTLDevice> device = cb.device;
    id<MTLBlitCommandEncoder> blit = nil;
    const MTLOrigin pixel = MTLOriginMake(h->x, h->y, 0);
    // A copy of an attachment the size of the pass, holding its pixel from before the pass when it loads.
    auto shadow = [&](MTLRenderPassAttachmentDescriptor *a, HistoryPass::Attachment &out) -> bool {
        const uint32_t width = (uint32_t)std::max<NSUInteger>(1, a.texture.width >> a.level);
        const uint32_t height = (uint32_t)std::max<NSUInteger>(1, a.texture.height >> a.level);
        if (width != h->width || height != h->height) {
            h->note = "the pass's attachments differ in size";
            return false;
        }
        out.format = a.texture.pixelFormat;
        out.loadAction = a.loadAction;
        out.shadow = NewRenderTexture(device, out.format, width, height);
        if (out.shadow == nil) {
            h->note = "no memory for copies of the pass's attachments";
            return false;
        }
        out.loads = a.loadAction == MTLLoadActionLoad && a.texture.storageMode != MTLStorageModeMemoryless;
        if (out.loads) {
            if (blit == nil) {
                blit = [cb blitCommandEncoder];
                blit.label = @"gpu-inspector pixel history start";
            }
            [blit copyFromTexture:a.texture
                      sourceSlice:a.slice
                      sourceLevel:a.level
                     sourceOrigin:MTLOriginMake(h->x, h->y, a.texture.textureType == MTLTextureType3D ? a.depthPlane : 0)
                       sourceSize:MTLSizeMake(1, 1, 1)
                        toTexture:out.shadow
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:pixel];
        }
        return true;
    };
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = descriptor.colorAttachments[i];
        if (a.texture == nil) continue;
        h->colors[i].clearColor = a.clearColor;
        if (!shadow(a, h->colors[i])) break;
    }
    if (h->note.empty() && descriptor.depthAttachment.texture != nil) {
        h->depth.clearDepth = descriptor.depthAttachment.clearDepth;
        shadow(descriptor.depthAttachment, h->depth);
    }
    if (h->note.empty() && descriptor.stencilAttachment.texture != nil) {
        MTLRenderPassStencilAttachmentDescriptor *s = descriptor.stencilAttachment;
        if (s.texture == descriptor.depthAttachment.texture && h->depth.shadow != nil) {
            // One texture for both: the depth's copy holds the stencil too.
            h->combined = true;
            h->stencil.shadow = [h->depth.shadow retain];
            h->stencil.format = s.texture.pixelFormat;
            h->stencil.loadAction = s.loadAction;
            h->stencil.loads = h->depth.loads && s.loadAction == MTLLoadActionLoad;
            h->stencil.clearStencil = s.clearStencil;
        } else {
            h->stencil.clearStencil = s.clearStencil;
            shadow(s, h->stencil);
        }
    }
    [blit endEncoding];
}

void FollowPixel(OverdrawPass &pass) {
    HistoryPass &h = *pass.history;
    const std::string where = "command buffer " + std::to_string(pass.commandBufferId) + ", pass " + std::to_string(pass.passIndex);
    PendingHistory out;
    out.texture = h.texture;
    if (!h.note.empty()) {
        out.notes.push_back(where + ": " + h.note);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back(std::move(out));
        return;
    }
  @autoreleasepool {
    Internal internal;
    id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)pass.commandBuffer;
    id<MTLDevice> device = commandBuffer.device;
    out.device = device.name.UTF8String ?: "";
    for (HistoryPass::Attachment &a : h.colors) {
        if (a.shadow != nil) out.keep.emplace_back(a.shadow);
    }
    if (h.depth.shadow != nil) out.keep.emplace_back(h.depth.shadow);
    if (h.stencil.shadow != nil) out.keep.emplace_back(h.stencil.shadow);

    // What the pixel reads as: the target's format, and the depth aspect of the depth attachment.
    const HistoryPass::Attachment &target = h.colors[h.target];
    const PixelFormatInfo colorInfo = PixelFormatDetails(target.format);
    if (colorInfo.name != nullptr && colorInfo.name[0] != '\0' && colorInfo.blockWidth == 1 && colorInfo.blockHeight == 1) {
        out.pixelFormat = colorInfo.name;
        out.colorBytes = colorInfo.blockBytes;
    } else {
        out.notes.push_back(where + std::string(": the render target's format ") + PixelFormatEnumName(target.format) + " cannot be read back");
    }
    MTLBlitOption depthOption = MTLBlitOptionNone;
    if (h.depth.shadow != nil) {
        const PixelFormatInfo depthInfo = DepthReadbackDetails(h.depth.format, &depthOption);
        if (depthInfo.name != nullptr && depthInfo.name[0] != '\0') {
            out.depthFormat = depthInfo.name;
            out.depthBytes = depthInfo.blockBytes;
        }
    }

    uint32_t draws = 0;
    for (const OverdrawPass::Segment &segment : pass.segments) {
        for (const LoggedOp &op : segment.ops) draws += op.key.policy == OpPolicy::Draw ? 1 : 0;
    }
    if (draws > kMaxDraws) {
        out.notes.push_back(where + ": only the first " + std::to_string(kMaxDraws) + " of its " + std::to_string(draws) + " draws are followed");
        draws = kMaxDraws;
    }
    const uint32_t slotBytes = out.colorBytes + out.depthBytes;
    out.staging = [device newBufferWithLength:std::max<NSUInteger>(1, (NSUInteger)(draws + 1) * slotBytes) options:MTLResourceStorageModeShared];
    if (out.staging == nil) {
        out.notes.push_back(where + ": no staging memory for the pixel's values");
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back(std::move(out));
        return;
    }

    auto descriptorFor = [&](bool start, id<MTLBuffer> visibility) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        for (NSUInteger i = 0; i < 8; i++) {
            const HistoryPass::Attachment &a = h.colors[i];
            if (a.shadow == nil) continue;
            rp.colorAttachments[i].texture = a.shadow;
            rp.colorAttachments[i].loadAction = !start || a.loads ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = a.loadAction == MTLLoadActionClear ? a.clearColor : MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        }
        if (h.depth.shadow != nil) {
            rp.depthAttachment.texture = h.depth.shadow;
            rp.depthAttachment.loadAction = !start || h.depth.loads ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.depthAttachment.clearDepth = h.depth.clearDepth;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
        }
        if (h.stencil.shadow != nil) {
            rp.stencilAttachment.texture = h.stencil.shadow;
            rp.stencilAttachment.loadAction = !start || h.stencil.loads ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.stencilAttachment.clearStencil = h.stencil.clearStencil;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (visibility != nil) rp.visibilityResultBuffer = visibility;
        return rp;
    };
    // The pixel, and the depth under it, into the staging buffer after an encoder has ended.
    auto readback = [&](int64_t slot) {
        if (slotBytes == 0) return;
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        blit.label = @"gpu-inspector pixel history readback";
        const NSUInteger offset = (NSUInteger)slot * slotBytes;
        if (out.colorBytes) {
            [blit copyFromTexture:target.shadow sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(h.x, h.y, 0)
                       sourceSize:MTLSizeMake(1, 1, 1) toBuffer:out.staging destinationOffset:offset
           destinationBytesPerRow:out.colorBytes destinationBytesPerImage:out.colorBytes];
        }
        if (out.depthBytes) {
            [blit copyFromTexture:h.depth.shadow sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(h.x, h.y, 0)
                       sourceSize:MTLSizeMake(1, 1, 1) toBuffer:out.staging destinationOffset:offset + out.colorBytes
           destinationBytesPerRow:out.depthBytes destinationBytesPerImage:out.depthBytes options:depthOption];
        }
        [blit endEncoding];
    };

    // The pass's start: the attachments loaded or cleared the way the pass's were.
    id<MTLRenderCommandEncoder> start = [commandBuffer renderCommandEncoderWithDescriptor:descriptorFor(true, nil)];
    start.label = @"gpu-inspector pixel history start";
    [start endEncoding];
    readback(0);
    PendingEvent load;
    load.kind = "load";
    load.command = pass.beginCommand;
    load.method = RecordedCommandMethod(pass.beginCommand);
    load.detail = LoadActionEnumName(target.loadAction);
    load.commandBuffer = pass.commandBufferId;
    load.frame = pass.frame;
    load.passIndex = pass.passIndex;
    load.slot = 0;
    out.events.push_back(load);

    id<MTLFunction> cover = LibraryFunction(device, "gpu_inspector_pixel_cover",
                                            @"#include <metal_stdlib>\nfragment void gpu_inspector_pixel_cover() {}\n");
    id none = DepthStencilCopy(device, nil, DepthStencilVariant::None);
    const MTLScissorRect pixel = {h.x, h.y, 1, 1};
    std::vector<const void *> kept;
    auto keep = [&](id object) {
        if (object == nil || std::find(kept.begin(), kept.end(), (__bridge const void *)object) != kept.end()) return;
        kept.push_back((__bridge const void *)object);
        out.keep.emplace_back(object);
    };
    bool noteIndirect = false;
    std::string copyError;
    uint32_t drawIndex = 0;
    for (const OverdrawPass::Segment &segment : pass.segments) {
        StateSnapshot snapshot;   // each encoder starts from Metal's default state
        for (const LoggedOp &op : segment.ops) {
            if (op.key.policy != OpPolicy::Draw) {
                snapshot.Apply(op);
                continue;
            }
            if (drawIndex >= draws) break;
            PendingEvent e;
            e.kind = "draw";
            e.command = op.command;
            e.method = RecordedCommandMethod(op.command);
            e.commandBuffer = pass.commandBufferId;
            e.frame = pass.frame;
            e.passIndex = pass.passIndex;

            id<MTLBuffer> visibility = [device newBufferWithLength:kVariants * 8 options:MTLResourceStorageModeShared];
            if (visibility == nil) {
                out.notes.push_back(where + ": no memory for the draws' sample counts");
                break;
            }
            const int64_t query = (int64_t)out.visibility.size();
            out.visibility.push_back(visibility);
            id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:descriptorFor(false, visibility)];
            if (encoder == nil) {
                out.notes.push_back(where + ": could not open an encoder to follow the pixel");
                break;
            }
            encoder.label = @"gpu-inspector pixel history";
            HistoryReplay replay;
            for (const LoggedOp *state : snapshot.ops()) state->op(encoder, replay);
            [encoder setScissorRect:pixel];
            op.op(encoder, replay);   // keeps the draw
            e.pipeline = replay.pipeline != nil ? IdOf(replay.pipeline) : 0;
            const bool inside = !replay.hasScissor
                || (h.x >= replay.scissor.x && h.x < replay.scissor.x + replay.scissor.width
                    && h.y >= replay.scissor.y && h.y < replay.scissor.y + replay.scissor.height);
            if (replay.skipped) {
                noteIndirect = true;
            } else if (!inside) {
                e.scissored = true;
            } else if (replay.draw && replay.pipeline != nil) {
                e.query = query;
                const DerivedPipeline coverCopy = PipelineCopy(device, replay.pipeline, PipelineVariant::HistoryCover, cover);
                const DerivedPipeline noWrite = PipelineCopy(device, replay.pipeline, PipelineVariant::HistoryNoWrite, nil);
                keep(coverCopy.pipeline);
                keep(noWrite.pipeline);
                if (copyError.empty() && !coverCopy.error.empty()) copyError = coverCopy.error;
                if (copyError.empty() && !noWrite.error.empty()) copyError = noWrite.error;
                auto query = [&](int variant, id pipeline, NSUInteger cull, id depthStencil) {
                    if (pipeline == nil || depthStencil == nil) return;
                    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline];
                    [encoder setCullMode:(MTLCullMode)cull];
                    [encoder setDepthStencilState:(id<MTLDepthStencilState>)depthStencil];
                    [encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)variant * 8];
                    replay.draw(encoder);
                    [encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
                    e.testsMeasured |= 1u << variant;
                };
                if (!coverCopy.rasterless && cover != nil && none != nil) {
                    query(0, coverCopy.pipeline, MTLCullModeNone, none);
                    query(1, coverCopy.pipeline, replay.cullMode, none);
                    query(2, noWrite.pipeline, replay.cullMode, none);
                    const id depthOnly = DepthStencilCopy(device, replay.depthStencil, DepthStencilVariant::DepthOnly);
                    const id stencilOnly = DepthStencilCopy(device, replay.depthStencil, DepthStencilVariant::StencilOnly);
                    const id both = DepthStencilCopy(device, replay.depthStencil, DepthStencilVariant::Both);
                    query(3, noWrite.pipeline, replay.cullMode, depthOnly);
                    query(4, noWrite.pipeline, replay.cullMode, stencilOnly);
                    query(5, noWrite.pipeline, replay.cullMode, both);
                }
                // The draw itself, with the application's state.
                [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)replay.pipeline];
                [encoder setCullMode:(MTLCullMode)replay.cullMode];
                [encoder setDepthStencilState:(id<MTLDepthStencilState>)(replay.depthStencil != nil ? replay.depthStencil : none)];
                replay.draw(encoder);
            }
            [encoder endEncoding];
            readback(drawIndex + 1);
            e.slot = drawIndex + 1;
            out.events.push_back(std::move(e));
            drawIndex++;
        }
    }
    if (noteIndirect) out.notes.push_back(where + ": indirect command buffers' draws are not followed; the values after them may be missing their writes");
    if (!copyError.empty()) out.notes.push_back(where + ": some draws were not measured: " + copyError);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back(std::move(out));
  }
}

void SendPixelHistory() {
    PixelHistoryRequest request;
    std::vector<PendingHistory> pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
        pending.swap(g_pending);
        g_request = PixelHistoryRequest();
    }
    if (!request.enabled) return;

    std::string device;
    std::string pixelFormat;
    std::string depthFormat;
    uint64_t texture = 0;
    for (const PendingHistory &h : pending) {
        if (texture == 0) texture = h.texture;
        if (device.empty()) device = h.device;
        if (pixelFormat.empty()) pixelFormat = h.pixelFormat;
        if (depthFormat.empty()) depthFormat = h.depthFormat;
    }
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CapturePixelHistory");
    w.Key("history"); w.BeginObject();
    w.Key("format"); w.String("gpu-inspector-pixel-history");
    w.Key("version"); w.Uint(1);
    w.Key("device"); w.String(device);
    w.Key("image"); w.Uint(texture != 0 ? texture : request.texture);
    w.Key("requestedImage"); w.Uint(request.texture);
    w.Key("x"); w.Uint(request.x);
    w.Key("y"); w.Uint(request.y);
    w.Key("mip"); w.Uint(request.level);
    w.Key("layer"); w.Uint(request.slice);
    w.Key("pixelFormat"); w.String(pixelFormat);
    w.Key("depthFormat"); w.String(depthFormat);
    w.Key("events"); w.BeginArray();
    size_t events = 0;
    for (const PendingHistory &h : pending) {
        const uint8_t *staging = h.staging != nil ? static_cast<const uint8_t *>(h.staging.contents) : nullptr;
        const uint32_t slotBytes = h.colorBytes + h.depthBytes;
        for (const PendingEvent &e : h.events) {
            w.BeginObject();
            w.Key("kind"); w.String(e.kind);
            w.Key("command"); w.Uint(e.command);
            w.Key("method"); w.String(e.method);
            w.Key("detail"); w.String(e.detail);
            w.Key("commandBuffer"); w.Uint(e.commandBuffer);
            w.Key("frame"); w.Uint(e.frame);
            w.Key("passIndex"); w.Uint(e.passIndex);
            w.Key("pipeline"); w.Uint(e.pipeline);
            w.Key("scissored"); w.Boolean(e.scissored);
            w.Key("testsMeasured"); w.Uint(e.testsMeasured);
            static const char *const kCounts[kVariants] = {"covered", "facing", "shaded", "depthPassed", "stencilPassed", "passed"};
            const uint64_t *counts = e.query >= 0 && (size_t)e.query < h.visibility.size()
                ? static_cast<const uint64_t *>(h.visibility[(size_t)e.query].contents) : nullptr;
            for (int v = 0; v < kVariants; v++) {
                const bool measured = counts != nullptr && (e.testsMeasured & (1u << v)) != 0;
                w.Key(kCounts[v]); w.Uint(measured ? counts[v] : 0);
            }
            const bool read = staging != nullptr && e.slot >= 0;
            w.Key("value"); w.String(read && h.colorBytes ? Hex(staging + e.slot * slotBytes, h.colorBytes) : "");
            w.Key("depth"); w.String(read && h.depthBytes ? Hex(staging + e.slot * slotBytes + h.colorBytes, h.depthBytes) : "");
            w.EndObject();
            events++;
        }
    }
    w.EndArray();
    w.Key("notes"); w.BeginArray();
    if (pending.empty()) {
        w.String("No render pass of the capture rendered to the texture at that level and slice.");
    }
    for (const PendingHistory &h : pending) {
        for (const std::string &n : h.notes) w.String(n);
    }
    w.EndArray();
    w.Key("problems"); w.BeginArray(); w.EndArray();
    w.EndObject();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    for (PendingHistory &h : pending) {
        [h.staging release];
        for (id<MTLBuffer> b : h.visibility) [b release];
    }
    Log("pixel history: %zu event(s) over %zu pass(es) sent", events, pending.size());
}

}  // namespace mtlinsp
