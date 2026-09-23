// Pixel history, measured while capturing: one pixel of one texture followed through every render
// pass of the capture that renders to it, the way vkinsp_replay --pixel follows one through a
// replayed Vulkan frame (src/replay/src/history.cpp). The UI names the pixel from an earlier capture,
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
// (a fragment function that writes nothing, and the application's with color writes off) are made.
//
// The shadows take the pass's shape: its sample count, since the draws re-issued into them are the
// application's and a pipeline's sample count has to match its attachment, and its
// renderTargetArrayLength, since a layered pass's draws pick a layer with
// `render_target_array_index` and one without those layers would have nowhere to put them. Nothing
// can be blitted out of a multisampled texture, so the two attachments the pixel is read from
// resolve into single-sample copies first.
//
// An indirect command buffer's commands carry their own pipelines, which the library never saw
// created and so cannot copy: those are executed one at a time under a visibility result, which
// says what each wrote at the pixel but not where its fragments stopped.
//
// Writes to the texture from outside any render pass are events of their own, with no fragments to
// account for and the value after the write as the whole answer: a pass's multisample resolve
// (FollowPixelResolve), a blit command (NotePixelHistoryBlit, per command, read back on the
// application's own encoder), and a compute encoder that had the texture bound
// (EndPixelHistoryEncoder, per encoder — a compute encoder cannot be interrupted to read a
// texture, so its dispatches share one event).
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
#include <atomic>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtlinsp
{

/** A pixel followed through one pass: where, and the copies of its attachments it is drawn into. */
struct HistoryPass
{
    /** The texture followed: the request's, or the drawable the frame rendered into instead. */
    uint64_t texture = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    /**
     * The pass's renderTargetArrayLength, 1 when it is not layered: how many layers the shadows
     * have, so a draw picking one with `render_target_array_index` lands where it did.
     */
    uint32_t layers = 1;
    /** Which of those layers the pixel is in: the request's slice, less the attachment's base. */
    uint32_t layer = 0;
    /** The color attachment the pixel is read from. */
    int target = -1;
    /** Why the pass is not followed. */
    std::string note;
    struct Attachment
    {
        id<MTLTexture> shadow = nil;   // retained
        /**
         * Single-sample copy of `shadow`, for a multisampled attachment the pixel is read from:
         * nothing can be blitted out of a multisampled texture, so the samples are resolved into
         * this first. Only the attachments read back have one. Retained.
         */
        id<MTLTexture> resolve = nil;
        MTLPixelFormat format = MTLPixelFormatInvalid;
        MTLLoadAction loadAction = MTLLoadActionDontCare;
        /** 1 unless the pass is multisampled, when the shadow has to match its sample count. */
        uint32_t sampleCount = 1;
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

    ~HistoryPass()
    {
        for (Attachment& a : colors)
        {
            [a.shadow release];
            [a.resolve release];
        }
        [depth.shadow release];
        [depth.resolve release];
        [stencil.shadow release];
        [stencil.resolve release];
    }
};

namespace
{

constexpr int kVariants = 6;
/** The last variant: the draw with every test, which is what it actually wrote at the pixel. */
constexpr int kPassedVariant = 5;
/** Draws followed per pass: two encoders each, in the application's command buffer. */
constexpr uint32_t kMaxDraws = 1024;

struct PendingEvent
{
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
struct PendingHistory
{
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
/** g_request.enabled, without the lock: the hooks of every bind and blit read it (PixelHistoryActive). */
std::atomic<bool> g_active{false};
/** Whether the request's texture is (or was) a drawable, so that whichever drawable the frame renders into is followed. */
bool g_resolved = false;
bool g_anyDrawable = false;
std::vector<PendingHistory> g_pending;
/**
 * Compute encoders the followed texture is bound to, and whether one of them has dispatched.
 * Keyed by the encoder; an entry appears the first time the texture is bound to it and is taken
 * out when the encoder closes.
 */
struct ComputeBinding
{
    /** The slots the followed texture is bound to: empty once every one of them is rebound. */
    std::vector<NSUInteger> slots;
    /**
     * The texture itself, retained: what the pixel is read out of when the encoder closes. Kept
     * rather than looked up from the request, which for a drawable names whichever one the earlier
     * capture saw, not the one this frame bound.
     */
    Strong texture;
    /** The last dispatch made while it was bound, and how many there were. */
    uint32_t command = 0;
    uint32_t dispatches = 0;
};
std::unordered_map<const void*, ComputeBinding> g_computeBindings;

/**
 * The calls in effect at a point of an encoder: each call recorded, less those a later call undid
 * (OpKey). Issued at a draw, they leave an encoder of the library's own in the state the
 * application's was in there, without every call before it.
 */
class StateSnapshot
{
public:
    void Apply(const LoggedOp& op)
    {
        const OpKey& k = op.key;
        auto drop = [&](auto undone) {
            for (auto it = ops_.begin(); it != ops_.end();)
            {
                if (undone((*it)->key))
                    it = ops_.erase(it);
                else
                    ++it;
            }
        };
        switch (k.policy)
        {
            case OpPolicy::Draw:
                return;
            case OpPolicy::Replace:
                drop([&](const OpKey& o) { return o.policy == OpPolicy::Replace && o.name == k.name; });
                break;
            case OpPolicy::Slot:
                drop([&](const OpKey& o) {
                    return (o.policy == OpPolicy::Slot || o.policy == OpPolicy::SlotOffset) && o.name == k.name && o.location == k.location;
                });
                break;
            case OpPolicy::SlotOffset:
                drop([&](const OpKey& o) { return o.policy == OpPolicy::SlotOffset && o.name == k.name && o.location == k.location; });
                break;
            case OpPolicy::Range:
                drop([&](const OpKey& o) {
                    if (o.name != k.name)
                        return false;
                    if (o.policy == OpPolicy::Range)
                        return o.location == k.location && o.length == k.length;
                    return (o.policy == OpPolicy::Slot || o.policy == OpPolicy::SlotOffset) && o.location >= k.location && o.location < k.location + k.length;
                });
                break;
        }
        ops_.push_back(&op);
    }
    const std::list<const LoggedOp*>& ops() const { return ops_; }

private:
    std::list<const LoggedOp*> ops_;
};

/** The pixel history's encoder, as the recorded calls see it: the state they set is noted, and a draw is kept rather than issued. */
class HistoryReplay final : public OverdrawReplay
{
public:
    id pipeline = nil;
    id depthStencil = nil;
    NSUInteger cullMode = MTLCullModeNone;
    bool hasScissor = false;
    MTLScissorRect scissor{};
    std::function<void(id<MTLRenderCommandEncoder>)> draw;
    bool skipped = false;
    /** An indirect command buffer's commands, executed one at a time rather than as one draw. */
    id indirect = nil;
    NSRange indirectRange = {0, 0};

    void BindPipeline(id<MTLRenderCommandEncoder> encoder, id state) override
    {
        pipeline = state;
        if (state != nil)
            [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)state];
    }
    void SetDepthStencilState(id<MTLRenderCommandEncoder> encoder, id state) override
    {
        depthStencil = state;
        [encoder setDepthStencilState:(id<MTLDepthStencilState>)state];
    }
    void SetCullMode(id<MTLRenderCommandEncoder> encoder, NSUInteger mode) override
    {
        cullMode = mode;
        [encoder setCullMode:(MTLCullMode)mode];
    }
    /** Noted only: the history's scissor is the pixel. */
    void SetScissorRects(id<MTLRenderCommandEncoder>, const MTLScissorRect* rects, NSUInteger count) override
    {
        hasScissor = count > 0 && rects != nullptr;
        if (hasScissor)
            scissor = rects[0];
    }
    void IssueDraw(id<MTLRenderCommandEncoder>, const std::function<void(id<MTLRenderCommandEncoder>)>& d) override { draw = d; }
    /**
     * Kept, not issued: the history runs each command of the range on its own, under the pixel's
     * scissor and a visibility result. The commands carry their own pipelines, so the copies the
     * other counts need (a fragment function that writes nothing, one with color writes off)
     * cannot be made — only the last count, of what the command actually wrote, is measurable.
     */
    void ExecuteIndirect(id<MTLRenderCommandEncoder>, id icb, NSRange range) override
    {
        if (icb == nil || range.length == 0)
        {
            skipped = true;
            return;
        }
        indirect = icb;
        indirectRange = range;
    }
    void Skip() override { skipped = true; }
};

std::string Hex(const uint8_t* bytes, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; i++)
    {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

}  // namespace

// --------------------------------------------------------------------------------------------

bool PixelHistoryActive()
{
    return g_active.load(std::memory_order_relaxed);
}

void StartPixelHistoryCapture(const PixelHistoryRequest& request)
{
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
    g_active.store(request.enabled, std::memory_order_relaxed);
    for (PendingHistory& h : pending)
    {
        [h.staging release];
        for (id<MTLBuffer> b : h.visibility)
            [b release];
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_computeBindings.clear();
    }
    if (request.enabled)
    {
        Log("pixel history: following pixel (%u, %u) of texture %llu, level %u, slice %u", request.x, request.y,
            (unsigned long long)request.texture, request.level, request.slice);
    }
}

// --------------------------------------------------------------------------------------------
// Writes from outside a render pass: blit commands, compute encoders, and a pass's multisample
// resolve. None of them has fragments to account for, so the event is the value after the write —
// which is the whole answer the history can give for one (renderer/pixel_history.ts's "copy",
// "blit", "resolve" and "compute" kinds, rendered under "Outside a render pass").

namespace
{

/** No pass: the index the renderer reads as "Outside a render pass" (capture_panel.ts). */
constexpr uint32_t kNoPass = 0xffffffffu;

/** Whether a texture is the one the capture follows, at the level and slice it follows. */
bool FollowedSubresource(id texture, NSUInteger slice, NSUInteger sliceCount, NSUInteger level, NSUInteger levelCount)
{
    PixelHistoryRequest request;
    bool anyDrawable = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
        anyDrawable = g_anyDrawable;
    }
    if (!request.enabled || texture == nil)
        return false;
    const bool same = request.texture != 0 && IdOf(texture) == request.texture;
    if (!same && !(anyDrawable && IsDrawableTexture(texture)))
        return false;
    return request.slice >= slice && request.slice < slice + sliceCount && request.level >= level
        && request.level < level + levelCount;
}

/**
 * The pixel copied out of a texture into a staging buffer of its own, on an encoder the caller
 * owns, and pushed as a one-event history. `command` is the command the capture recorded the
 * write as.
 */
void ReadPixelAfterWrite(id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture, uint64_t commandBufferId,
    uint32_t command, const char* kind, const char* detail)
{
    PixelHistoryRequest request;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
    }
    const PixelFormatInfo info = PixelFormatDetails(texture.pixelFormat);
    const std::string where = "command buffer " + std::to_string(commandBufferId);
    PendingHistory out;
    out.texture = IdOf(texture);
    out.device = texture.device.name.UTF8String ?: "";
    if (info.name == nullptr || info.name[0] == '\0' || info.blockWidth != 1 || info.blockHeight != 1
        || texture.sampleCount > 1)
    {
        // A compressed or multisampled texture cannot be the source of a one-texel copy. The event
        // is still worth reporting: what wrote the pixel is the question, and the value is extra.
        out.notes.push_back(where + std::string(": the value after the ") + kind + " could not be read back out of "
            + PixelFormatEnumName(texture.pixelFormat));
    }
    else
    {
        out.pixelFormat = info.name;
        out.colorBytes = info.blockBytes;
        out.staging = [texture.device newBufferWithLength:info.blockBytes options:MTLResourceStorageModeShared];
        if (out.staging == nil)
            out.colorBytes = 0;
    }
    PendingEvent e;
    e.kind = kind;
    e.detail = detail;
    e.command = command;
    e.method = RecordedCommandMethod(command);
    e.commandBuffer = commandBufferId;
    e.frame = CaptureFrameIndex();
    e.passIndex = kNoPass;
    if (out.colorBytes != 0)
    {
        [blit copyFromTexture:texture
                  sourceSlice:request.slice
                  sourceLevel:request.level
                 sourceOrigin:MTLOriginMake(request.x, request.y, 0)
                   sourceSize:MTLSizeMake(1, 1, 1)
                     toBuffer:out.staging
            destinationOffset:0
       destinationBytesPerRow:out.colorBytes
     destinationBytesPerImage:out.colorBytes];
        e.slot = 0;
    }
    out.events.push_back(std::move(e));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back(std::move(out));
}

}  // namespace

int MatchPixelHistoryResolve(MTLRenderPassDescriptor* descriptor)
{
    if (!PixelHistoryActive() || descriptor == nil)
        return -1;
    for (NSUInteger i = 0; i < 8; i++)
    {
        MTLRenderPassColorAttachmentDescriptor* a = descriptor.colorAttachments[i];
        id<MTLTexture> t = a.resolveTexture;
        if (t == nil)
            continue;
        const NSUInteger slice = t.textureType == MTLTextureType3D ? a.resolveDepthPlane : a.resolveSlice;
        if (!FollowedSubresource(t, slice, 1, a.resolveLevel, 1))
            continue;
        return (int)i;
    }
    return -1;
}

void PreparePixelHistoryResolve(OverdrawPass& pass, MTLRenderPassDescriptor* descriptor, int attachment)
{
    pass.historyResolve = [descriptor.colorAttachments[attachment].resolveTexture retain];
}

void FollowPixelResolve(OverdrawPass& pass)
{
    if (pass.historyResolve == nil || pass.commandBuffer == nil)
        return;
    @autoreleasepool
    {
        Internal internal;
        id<MTLBlitCommandEncoder> blit = [(id<MTLCommandBuffer>)pass.commandBuffer blitCommandEncoder];
        if (blit == nil)
            return;
        blit.label = @"gpu-inspector pixel history resolve read";
        ReadPixelAfterWrite(blit, pass.historyResolve, pass.commandBufferId, pass.beginCommand, "resolve",
            "resolved from the pass's multisampled attachment");
        [blit endEncoding];
    }
    Log("pixel history: pass %u resolved into the texture", pass.passIndex);
}

void NotePixelHistoryBlit(id encoder, const BlitWrite& write)
{
    if (!PixelHistoryActive() || encoder == nil || write.texture == nil)
        return;
    id<MTLTexture> texture = (id<MTLTexture>)write.texture;
    if (!FollowedSubresource(texture, write.slice, write.sliceCount, write.level, write.levelCount))
        return;
    PixelHistoryRequest request;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
    }
    if (!write.wholeLevel)
    {
        // The pixel is in the level's coordinates; so is the destination origin.
        const NSUInteger x = request.x, y = request.y;
        if (x < write.origin.x || x >= write.origin.x + write.size.width || y < write.origin.y
            || y >= write.origin.y + write.size.height)
        {
            return;
        }
    }
    id commandBuffer = EncoderCommandBuffer(encoder, nullptr);
    if (commandBuffer == nil)
        return;
    Internal internal;
    // On the application's own encoder, behind the write it just made: a blit encoder cannot be
    // interrupted by one of the library's own, and a copy out of a texture is all this needs.
    ReadPixelAfterWrite((id<MTLBlitCommandEncoder>)encoder, texture, CommandBufferId(commandBuffer),
        LastRecordedCommand(), write.kind, write.detail);
    Log("pixel history: %s wrote the pixel (%s)", write.kind, write.detail);
}

void NotePixelHistoryComputeTexture(id encoder, id texture, NSUInteger index)
{
    if (!PixelHistoryActive() || encoder == nil)
        return;
    const bool followed = FollowedSubresource(texture, 0, texture != nil ? [(id<MTLTexture>)texture arrayLength] : 1,
        0, texture != nil ? [(id<MTLTexture>)texture mipmapLevelCount] : 1);
    const void* key = (__bridge const void*)encoder;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (followed)
    {
        ComputeBinding& binding = g_computeBindings[key];
        binding.texture = Strong(texture);
        if (std::find(binding.slots.begin(), binding.slots.end(), index) == binding.slots.end())
            binding.slots.push_back(index);
        return;
    }
    auto it = g_computeBindings.find(key);
    if (it == g_computeBindings.end())
        return;
    std::vector<NSUInteger>& slots = it->second.slots;
    slots.erase(std::remove(slots.begin(), slots.end(), index), slots.end());
}

void NotePixelHistoryDispatch(id encoder)
{
    if (!PixelHistoryActive() || encoder == nil)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_computeBindings.find((__bridge const void*)encoder);
    if (it == g_computeBindings.end() || it->second.slots.empty())
        return;
    it->second.command = LastRecordedCommand();
    it->second.dispatches++;
}

void EndPixelHistoryEncoder(id encoder)
{
    if (!PixelHistoryActive() || encoder == nil)
        return;
    ComputeBinding binding;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_computeBindings.find((__bridge const void*)encoder);
        if (it == g_computeBindings.end())
            return;
        binding = it->second;
        g_computeBindings.erase(it);
    }
    if (binding.dispatches == 0)
        return;
    id commandBuffer = EncoderCommandBuffer(encoder, nullptr);
    id texture = binding.texture.get();
    if (commandBuffer == nil || texture == nil)
        return;
    @autoreleasepool
    {
        Internal internal;
        id<MTLBlitCommandEncoder> blit = [(id<MTLCommandBuffer>)commandBuffer blitCommandEncoder];
        if (blit == nil)
            return;
        blit.label = @"gpu-inspector pixel history compute read";
        const std::string detail = binding.dispatches == 1
            ? "dispatched with the texture bound to be written"
            : std::to_string(binding.dispatches)
                + " dispatches of the encoder had the texture bound to be written; the value is the pixel after all of them";
        ReadPixelAfterWrite(blit, (id<MTLTexture>)texture, CommandBufferId(commandBuffer), binding.command,
            "compute", detail.c_str());
        [blit endEncoding];
    }
    Log("pixel history: a compute encoder's %u dispatch(es) had the texture bound", binding.dispatches);
}

int MatchPixelHistoryAttachment(MTLRenderPassDescriptor* descriptor)
{
    PixelHistoryRequest request;
    bool resolved = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
        resolved = g_resolved;
    }
    if (!request.enabled || descriptor == nil)
        return -1;
    if (!resolved)
    {
        // A texture the frame no longer has, or a drawable: the frame renders into a drawable of its
        // own, which is the one to follow.
        id texture = request.texture != 0 ? LiveObject(request.texture) : nil;
        const bool anyDrawable = texture == nil || IsDrawableTexture(texture);
        Log("pixel history: texture %llu is %s, so %s", (unsigned long long)request.texture,
            texture == nil ? "gone from the frame" : (anyDrawable ? "a drawable" : "still live"),
            anyDrawable ? "whichever drawable the frame renders into is followed" : "only that texture is followed");
        std::lock_guard<std::mutex> lock(g_mutex);
        g_anyDrawable = anyDrawable;
        g_resolved = true;
    }
    bool anyDrawable = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        anyDrawable = g_anyDrawable;
    }
    for (NSUInteger i = 0; i < 8; i++)
    {
        MTLRenderPassColorAttachmentDescriptor* a = descriptor.colorAttachments[i];
        id<MTLTexture> t = a.texture;
        if (t == nil)
            continue;
        const bool same = request.texture != 0 && IdOf(t) == request.texture;
        if (!same && !(anyDrawable && IsDrawableTexture(t)))
        {
            Log("pixel history: attachment %lu is texture %llu%s, not the one followed", (unsigned long)i,
                (unsigned long long)IdOf(t), IsDrawableTexture(t) ? " (a drawable)" : "");
            continue;
        }
        const NSUInteger slice = t.textureType == MTLTextureType3D ? a.depthPlane : a.slice;
        // A layered pass renders to `renderTargetArrayLength` slices from that one, which draws
        // pick between with `render_target_array_index`: the pixel is in the pass if it is in any
        // of them.
        const NSUInteger layers = std::max<NSUInteger>(1, descriptor.renderTargetArrayLength);
        if (a.level != request.level || request.slice < slice || request.slice >= slice + layers)
        {
            Log("pixel history: attachment %lu is the texture followed at level %lu slice %lu (%lu layer(s)), not level %u slice %u",
                (unsigned long)i, (unsigned long)a.level, (unsigned long)slice, (unsigned long)layers, request.level,
                request.slice);
            continue;
        }
        return (int)i;
    }
    return -1;
}

void PreparePixelHistory(OverdrawPass& pass, id commandBuffer, MTLRenderPassDescriptor* descriptor, int attachment)
{
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
    MTLRenderPassColorAttachmentDescriptor* target = descriptor.colorAttachments[attachment];
    h->texture = IdOf(target.texture);
    h->width = (uint32_t)std::max<NSUInteger>(1, target.texture.width >> target.level);
    h->height = (uint32_t)std::max<NSUInteger>(1, target.texture.height >> target.level);
    if (h->x >= h->width || h->y >= h->height)
    {
        h->note = "the pixel is outside the pass's render target";
        return;
    }
    // A layered pass: the shadows are arrays of the same length, so a draw that picks a layer
    // with `render_target_array_index` lands in the same one, and the pixel is read out of the
    // layer the request named.
    h->layers = (uint32_t)std::max<NSUInteger>(1, descriptor.renderTargetArrayLength);
    const NSUInteger baseSlice = target.texture.textureType == MTLTextureType3D ? target.depthPlane : target.slice;
    h->layer = request.slice >= baseSlice ? (uint32_t)(request.slice - baseSlice) : 0;

    Internal internal;
    id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)commandBuffer;
    id<MTLDevice> device = cb.device;
    id<MTLBlitCommandEncoder> blit = nil;
    const MTLOrigin pixel = MTLOriginMake(h->x, h->y, 0);
    // A copy of an attachment the size of the pass, holding its pixel from before the pass when it loads.
    auto shadow = [&](MTLRenderPassAttachmentDescriptor* a, HistoryPass::Attachment& out) -> bool {
        const uint32_t width = (uint32_t)std::max<NSUInteger>(1, a.texture.width >> a.level);
        const uint32_t height = (uint32_t)std::max<NSUInteger>(1, a.texture.height >> a.level);
        if (width != h->width || height != h->height)
        {
            h->note = "the pass's attachments differ in size";
            return false;
        }
        out.format = a.texture.pixelFormat;
        out.loadAction = a.loadAction;
        out.sampleCount = (uint32_t)std::max<NSUInteger>(1, a.texture.sampleCount);
        out.shadow = NewRenderTexture(device, out.format, width, height, out.sampleCount, h->layers);
        if (out.shadow == nil)
        {
            h->note = "no memory for copies of the pass's attachments";
            return false;
        }
        // A multisampled shadow cannot be blitted out of, so the attachments whose pixel is read
        // back get a single-sample texture to resolve into (FollowPixel's readback).
        out.loads = a.loadAction == MTLLoadActionLoad && a.texture.storageMode != MTLStorageModeMemoryless;
        if (out.loads)
        {
            if (blit == nil)
            {
                blit = [cb blitCommandEncoder];
                blit.label = @"gpu-inspector pixel history start";
            }
            // Only the layer the pixel is in: the others are never read out of the shadow.
            const NSUInteger base = a.texture.textureType == MTLTextureType3D ? a.depthPlane : a.slice;
            const bool is3D = a.texture.textureType == MTLTextureType3D;
            [blit copyFromTexture:a.texture
                      sourceSlice:is3D ? 0 : base + h->layer
                      sourceLevel:a.level
                     sourceOrigin:MTLOriginMake(h->x, h->y, is3D ? base + h->layer : 0)
                       sourceSize:MTLSizeMake(1, 1, 1)
                        toTexture:out.shadow
                 destinationSlice:h->layer
                 destinationLevel:0
                destinationOrigin:pixel];
        }
        return true;
    };
    for (NSUInteger i = 0; i < 8; i++)
    {
        MTLRenderPassColorAttachmentDescriptor* a = descriptor.colorAttachments[i];
        if (a.texture == nil)
            continue;
        h->colors[i].clearColor = a.clearColor;
        if (!shadow(a, h->colors[i]))
            break;
    }
    if (h->note.empty() && descriptor.depthAttachment.texture != nil)
    {
        h->depth.clearDepth = descriptor.depthAttachment.clearDepth;
        shadow(descriptor.depthAttachment, h->depth);
    }
    if (h->note.empty() && descriptor.stencilAttachment.texture != nil)
    {
        MTLRenderPassStencilAttachmentDescriptor* s = descriptor.stencilAttachment;
        if (s.texture == descriptor.depthAttachment.texture && h->depth.shadow != nil)
        {
            // One texture for both: the depth's copy holds the stencil too.
            h->combined = true;
            h->stencil.shadow = [h->depth.shadow retain];
            h->stencil.format = s.texture.pixelFormat;
            h->stencil.loadAction = s.loadAction;
            h->stencil.loads = h->depth.loads && s.loadAction == MTLLoadActionLoad;
            h->stencil.clearStencil = s.clearStencil;
        }
        else
        {
            h->stencil.clearStencil = s.clearStencil;
            shadow(s, h->stencil);
        }
    }
    // The two attachments the pixel is read from need somewhere to resolve into when the pass is
    // multisampled. The others do not: nothing is ever copied out of them.
    auto resolveTarget = [&](HistoryPass::Attachment& out) {
        if (out.shadow == nil || out.sampleCount <= 1 || !h->note.empty())
            return;
        out.resolve = NewRenderTexture(device, out.format, h->width, h->height, 1, h->layers);
        if (out.resolve == nil)
            h->note = "no memory for the resolve of the pass's multisampled attachments";
    };
    resolveTarget(h->colors[h->target]);
    resolveTarget(h->depth);
    [blit endEncoding];
}

void FollowPixel(OverdrawPass& pass)
{
    HistoryPass& h = *pass.history;
    const std::string where = "command buffer " + std::to_string(pass.commandBufferId) + ", pass " + std::to_string(pass.passIndex);
    PendingHistory out;
    out.texture = h.texture;
    if (!h.note.empty())
    {
        Log("pixel history: %s declined: %s", where.c_str(), h.note.c_str());
        out.notes.push_back(where + ": " + h.note);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back(std::move(out));
        return;
    }
    @autoreleasepool
    {
        Internal internal;
        id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)pass.commandBuffer;
        id<MTLDevice> device = commandBuffer.device;
        out.device = device.name.UTF8String ?: "";
        for (HistoryPass::Attachment& a : h.colors)
        {
            if (a.shadow != nil)
                out.keep.emplace_back(a.shadow);
        }
        if (h.depth.shadow != nil)
            out.keep.emplace_back(h.depth.shadow);
        if (h.stencil.shadow != nil)
            out.keep.emplace_back(h.stencil.shadow);

    // What the pixel reads as: the target's format, and the depth aspect of the depth attachment.
        const HistoryPass::Attachment& target = h.colors[h.target];
        const PixelFormatInfo colorInfo = PixelFormatDetails(target.format);
        if (colorInfo.name != nullptr && colorInfo.name[0] != '\0' && colorInfo.blockWidth == 1 && colorInfo.blockHeight == 1)
        {
            out.pixelFormat = colorInfo.name;
            out.colorBytes = colorInfo.blockBytes;
        }
        else
        {
            out.notes.push_back(where + std::string(": the render target's format ") + PixelFormatEnumName(target.format) + " cannot be read back");
        }
        MTLBlitOption depthOption = MTLBlitOptionNone;
        if (h.depth.shadow != nil)
        {
            const PixelFormatInfo depthInfo = DepthReadbackDetails(h.depth.format, &depthOption);
            if (depthInfo.name != nullptr && depthInfo.name[0] != '\0')
            {
                out.depthFormat = depthInfo.name;
                out.depthBytes = depthInfo.blockBytes;
            }
        }

        uint32_t draws = 0;
        for (const OverdrawPass::Segment& segment : pass.segments)
        {
            for (const LoggedOp& op : segment.ops)
                draws += op.key.policy == OpPolicy::Draw ? 1 : 0;
        }
        if (draws > kMaxDraws)
        {
            out.notes.push_back(where + ": only the first " + std::to_string(kMaxDraws) + " of its " + std::to_string(draws) + " draws are followed");
            draws = kMaxDraws;
        }
        const uint32_t slotBytes = out.colorBytes + out.depthBytes;
        // One slot per event. Sized for the cap rather than for the draws just counted: an
        // indirect command buffer's execution is one op here and one event per *command* there,
        // and how many commands that is only comes out when the op runs.
        out.staging = [device newBufferWithLength:std::max<NSUInteger>(1, (NSUInteger)(kMaxDraws + 1) * slotBytes) options:MTLResourceStorageModeShared];
        if (out.staging == nil)
        {
            out.notes.push_back(where + ": no staging memory for the pixel's values");
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.push_back(std::move(out));
            return;
        }

        auto descriptorFor = [&](bool start, id<MTLBuffer> visibility) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            for (NSUInteger i = 0; i < 8; i++)
            {
                const HistoryPass::Attachment& a = h.colors[i];
                if (a.shadow == nil)
                    continue;
                rp.colorAttachments[i].texture = a.shadow;
                rp.colorAttachments[i].loadAction = !start || a.loads ? MTLLoadActionLoad : MTLLoadActionClear;
                rp.colorAttachments[i].clearColor = a.loadAction == MTLLoadActionClear ? a.clearColor : MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
            if (h.depth.shadow != nil)
            {
                rp.depthAttachment.texture = h.depth.shadow;
                rp.depthAttachment.loadAction = !start || h.depth.loads ? MTLLoadActionLoad : MTLLoadActionClear;
                rp.depthAttachment.clearDepth = h.depth.clearDepth;
                rp.depthAttachment.storeAction = MTLStoreActionStore;
            }
            if (h.stencil.shadow != nil)
            {
                rp.stencilAttachment.texture = h.stencil.shadow;
                rp.stencilAttachment.loadAction = !start || h.stencil.loads ? MTLLoadActionLoad : MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = h.stencil.clearStencil;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            if (visibility != nil)
                rp.visibilityResultBuffer = visibility;
            // A layered pass: the same number of layers, so a draw's render_target_array_index
            // means the same thing here as it did in the application's pass.
            if (h.layers > 1)
                rp.renderTargetArrayLength = h.layers;
            return rp;
        };
    // Nothing can be copied out of a multisampled texture, so a multisampled shadow is resolved
    // into the single-sample copy beside it first: a render pass that draws nothing, keeping the
    // samples (the next event draws into them again) and resolving them as it stores. The color
    // is resolved the way the hardware would have; the depth is sample 0, since averaging depths
    // would invent a value no fragment wrote.
        auto resolveShadows = [&]() {
            const bool colorMs = target.resolve != nil;
            const bool depthMs = h.depth.resolve != nil && out.depthBytes;
            if (!colorMs && !depthMs)
                return;
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            if (h.layers > 1)
                rp.renderTargetArrayLength = h.layers;
            if (colorMs)
            {
                rp.colorAttachments[0].texture = target.shadow;
                rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                rp.colorAttachments[0].resolveTexture = target.resolve;
                rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            }
            if (depthMs)
            {
                rp.depthAttachment.texture = h.depth.shadow;
                rp.depthAttachment.loadAction = MTLLoadActionLoad;
                rp.depthAttachment.resolveTexture = h.depth.resolve;
                rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
                rp.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            }
            id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:rp];
            encoder.label = @"gpu-inspector pixel history resolve";
            [encoder endEncoding];
        };
    // The pixel, and the depth under it, into the staging buffer after an encoder has ended.
        auto readback = [&](int64_t slot) {
            if (slotBytes == 0)
                return;
            resolveShadows();
            id<MTLTexture> colorFrom = target.resolve != nil ? target.resolve : target.shadow;
            id<MTLTexture> depthFrom = h.depth.resolve != nil ? h.depth.resolve : h.depth.shadow;
            id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
            blit.label = @"gpu-inspector pixel history readback";
            const NSUInteger offset = (NSUInteger)slot * slotBytes;
            if (out.colorBytes)
            {
                [blit copyFromTexture:colorFrom
                                 sourceSlice:h.layer
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(h.x, h.y, 0)
                                  sourceSize:MTLSizeMake(1, 1, 1)
                                    toBuffer:out.staging
                           destinationOffset:offset
                      destinationBytesPerRow:out.colorBytes
                    destinationBytesPerImage:out.colorBytes];
            }
            if (out.depthBytes)
            {
                [blit copyFromTexture:depthFrom
                                 sourceSlice:h.layer
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(h.x, h.y, 0)
                                  sourceSize:MTLSizeMake(1, 1, 1)
                                    toBuffer:out.staging
                           destinationOffset:offset + out.colorBytes
                      destinationBytesPerRow:out.depthBytes
                    destinationBytesPerImage:out.depthBytes
                                     options:depthOption];
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
        std::vector<const void*> kept;
        auto keep = [&](id object) {
            if (object == nil || std::find(kept.begin(), kept.end(), (__bridge const void*)object) != kept.end())
                return;
            kept.push_back((__bridge const void*)object);
            out.keep.emplace_back(object);
        };
        bool noteIndirect = false;
        std::string copyError;
        uint32_t drawIndex = 0;
        for (const OverdrawPass::Segment& segment : pass.segments)
        {
            StateSnapshot snapshot;   // each encoder starts from Metal's default state
            for (const LoggedOp& op : segment.ops)
            {
                if (op.key.policy != OpPolicy::Draw)
                {
                    snapshot.Apply(op);
                    continue;
                }
                // Against the cap, not against `draws`: an indirect command buffer's execution is
                // one op there and one event per command here.
                if (drawIndex >= kMaxDraws)
                    break;
                PendingEvent e;
                e.kind = "draw";
                e.command = op.command;
                e.method = RecordedCommandMethod(op.command);
                e.commandBuffer = pass.commandBufferId;
                e.frame = pass.frame;
                e.passIndex = pass.passIndex;

                id<MTLBuffer> visibility = [device newBufferWithLength:kVariants * 8 options:MTLResourceStorageModeShared];
                if (visibility == nil)
                {
                    out.notes.push_back(where + ": no memory for the draws' sample counts");
                    break;
                }
                const int64_t query = (int64_t)out.visibility.size();
                out.visibility.push_back(visibility);
                id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:descriptorFor(false, visibility)];
                if (encoder == nil)
                {
                    out.notes.push_back(where + ": could not open an encoder to follow the pixel");
                    break;
                }
                encoder.label = @"gpu-inspector pixel history";
                HistoryReplay replay;
                for (const LoggedOp* state : snapshot.ops())
                    state->op(encoder, replay);
                [encoder setScissorRect:pixel];
                op.op(encoder, replay);   // keeps the draw
                e.pipeline = replay.pipeline != nil ? IdOf(replay.pipeline) : 0;
                const bool inside = !replay.hasScissor || (h.x >= replay.scissor.x && h.x < replay.scissor.x + replay.scissor.width && h.y >= replay.scissor.y && h.y < replay.scissor.y + replay.scissor.height);
                if (replay.skipped)
                {
                    noteIndirect = true;
                }
                else if (replay.indirect != nil && inside)
                {
                    // An indirect command buffer's commands, one at a time. Each is a draw with a
                    // pipeline of its own, which the library never saw created and so cannot copy:
                    // only the last count — what the command actually wrote at the pixel — can be
                    // measured, and it comes from running the command under a visibility result.
                    // This encoder issued nothing; each command gets one of its own.
                    [encoder endEncoding];
                    const NSRange range = replay.indirectRange;
                    for (NSUInteger i = 0; i < range.length && drawIndex < kMaxDraws; i++)
                    {
                        id<MTLBuffer> counts = [device newBufferWithLength:kVariants * 8 options:MTLResourceStorageModeShared];
                        if (counts == nil)
                        {
                            out.notes.push_back(where + ": no memory for the indirect commands' sample counts");
                            break;
                        }
                        const int64_t indirectQuery = (int64_t)out.visibility.size();
                        out.visibility.push_back(counts);
                        id<MTLRenderCommandEncoder> one = [commandBuffer renderCommandEncoderWithDescriptor:descriptorFor(false, counts)];
                        if (one == nil)
                        {
                            out.notes.push_back(where + ": could not open an encoder to follow the pixel");
                            break;
                        }
                        one.label = @"gpu-inspector pixel history (indirect)";
                        HistoryReplay again;
                        for (const LoggedOp* state : snapshot.ops())
                            state->op(one, again);
                        [one setScissorRect:pixel];
                        [one setVisibilityResultMode:MTLVisibilityResultModeCounting offset:kPassedVariant * 8];
                        [one executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)replay.indirect
                                           withRange:NSMakeRange(range.location + i, 1)];
                        [one setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
                        [one endEncoding];
                        readback(drawIndex + 1);
                        PendingEvent c = e;
                        c.detail = "command " + std::to_string((unsigned long long)(range.location + i))
                            + " of the indirect command buffer";
                        c.query = indirectQuery;
                        c.testsMeasured = 1u << kPassedVariant;
                        c.slot = drawIndex + 1;
                        out.events.push_back(std::move(c));
                        drawIndex++;
                    }
                    if (drawIndex >= kMaxDraws && range.length > 0)
                        out.notes.push_back(where + ": the indirect command buffer's later commands are not followed");
                    continue;
                }
                else if (!inside)
                {
                    e.scissored = true;
                }
                else if (replay.draw && replay.pipeline != nil)
                {
                    e.query = query;
                    const DerivedPipeline coverCopy = PipelineCopy(device, replay.pipeline, PipelineVariant::HistoryCover, cover);
                    const DerivedPipeline noWrite = PipelineCopy(device, replay.pipeline, PipelineVariant::HistoryNoWrite, nil);
                    keep(coverCopy.pipeline);
                    keep(noWrite.pipeline);
                    if (copyError.empty() && !coverCopy.error.empty())
                        copyError = coverCopy.error;
                    if (copyError.empty() && !noWrite.error.empty())
                        copyError = noWrite.error;
                    auto query = [&](int variant, id pipeline, NSUInteger cull, id depthStencil) {
                        if (pipeline == nil || depthStencil == nil)
                            return;
                        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline];
                        [encoder setCullMode:(MTLCullMode)cull];
                        [encoder setDepthStencilState:(id<MTLDepthStencilState>)depthStencil];
                        [encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)variant * 8];
                        replay.draw(encoder);
                        [encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
                        e.testsMeasured |= 1u << variant;
                    };
                    if (!coverCopy.rasterless && cover != nil && none != nil)
                    {
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
        if (noteIndirect)
            out.notes.push_back(where + ": an indirect command buffer executed over a range the GPU chooses is not followed; the values after it may be missing its writes");
        if (!copyError.empty())
            out.notes.push_back(where + ": some draws were not measured: " + copyError);
        size_t ops = 0;
        for (const OverdrawPass::Segment& segment : pass.segments)
            ops += segment.ops.size();
        Log("pixel history: %s followed at (%u, %u): %zu encoder(s), %zu recorded call(s), %u draw(s), %zu event(s)",
            where.c_str(), h.x, h.y, pass.segments.size(), ops, draws, out.events.size());
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back(std::move(out));
    }
}

void SendPixelHistory()
{
    PixelHistoryRequest request;
    std::vector<PendingHistory> pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request = g_request;
        pending.swap(g_pending);
        g_request = PixelHistoryRequest();
        g_computeBindings.clear();
    }
    g_active.store(false, std::memory_order_relaxed);
    if (!request.enabled)
        return;

    std::string device;
    std::string pixelFormat;
    std::string depthFormat;
    uint64_t texture = 0;
    for (const PendingHistory& h : pending)
    {
        if (texture == 0)
            texture = h.texture;
        if (device.empty())
            device = h.device;
        if (pixelFormat.empty())
            pixelFormat = h.pixelFormat;
        if (depthFormat.empty())
            depthFormat = h.depthFormat;
    }
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("CapturePixelHistory");
    w.Key("history");
    w.BeginObject();
    w.Key("format");
    w.String("gpu-inspector-pixel-history");
    w.Key("version");
    w.Uint(1);
    w.Key("device");
    w.String(device);
    w.Key("image");
    w.Uint(texture != 0 ? texture : request.texture);
    w.Key("requestedImage");
    w.Uint(request.texture);
    w.Key("x");
    w.Uint(request.x);
    w.Key("y");
    w.Uint(request.y);
    w.Key("mip");
    w.Uint(request.level);
    w.Key("layer");
    w.Uint(request.slice);
    w.Key("pixelFormat");
    w.String(pixelFormat);
    w.Key("depthFormat");
    w.String(depthFormat);
    w.Key("events");
    w.BeginArray();
    size_t events = 0;
    size_t drawEvents = 0;
    for (const PendingHistory& h : pending)
    {
        const uint8_t* staging = h.staging != nil ? static_cast<const uint8_t*>(h.staging.contents) : nullptr;
        const uint32_t slotBytes = h.colorBytes + h.depthBytes;
        for (const PendingEvent& e : h.events)
        {
            w.BeginObject();
            w.Key("kind");
            w.String(e.kind);
            w.Key("command");
            w.Uint(e.command);
            w.Key("method");
            w.String(e.method);
            w.Key("detail");
            w.String(e.detail);
            w.Key("commandBuffer");
            w.Uint(e.commandBuffer);
            w.Key("frame");
            w.Uint(e.frame);
            w.Key("passIndex");
            w.Uint(e.passIndex);
            w.Key("pipeline");
            w.Uint(e.pipeline);
            w.Key("scissored");
            w.Boolean(e.scissored);
            w.Key("testsMeasured");
            w.Uint(e.testsMeasured);
            static const char* const kCounts[kVariants] = {"covered", "facing", "shaded", "depthPassed", "stencilPassed", "passed"};
            const uint64_t* counts = e.query >= 0 && (size_t)e.query < h.visibility.size()
                ? static_cast<const uint64_t*>(h.visibility[(size_t)e.query].contents)
                : nullptr;
            for (int v = 0; v < kVariants; v++)
            {
                const bool measured = counts != nullptr && (e.testsMeasured & (1u << v)) != 0;
                w.Key(kCounts[v]);
                w.Uint(measured ? counts[v] : 0);
            }
            const bool read = staging != nullptr && e.slot >= 0;
            w.Key("value");
            w.String(read && h.colorBytes ? Hex(staging + e.slot * slotBytes, h.colorBytes) : "");
            w.Key("depth");
            w.String(read && h.depthBytes ? Hex(staging + e.slot * slotBytes + h.colorBytes, h.depthBytes) : "");
            w.EndObject();
            events++;
            drawEvents += e.kind == "draw" ? 1 : 0;
        }
    }
    w.EndArray();
    w.Key("notes");
    w.BeginArray();
    if (pending.empty())
    {
        w.String("No render pass of the capture rendered to the texture at that level and slice.");
    }
    else if (drawEvents == 0)
    {
        // Every pass that renders to the texture only clears or loads it. Worth saying outright:
        // without it the tab is a list of pass starts and nothing explains the absence of draws,
        // which is what a Unity frame's clear-only passes looked like (TODO.md).
        w.String("No pass that renders to the texture makes a draw, so only their starts are reported.");
    }
    for (const PendingHistory& h : pending)
    {
        for (const std::string& n : h.notes)
            w.String(n);
    }
    w.EndArray();
    w.Key("problems");
    w.BeginArray();
    w.EndArray();
    w.EndObject();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    for (PendingHistory& h : pending)
    {
        [h.staging release];
        for (id<MTLBuffer> b : h.visibility)
            [b release];
    }
    Log("pixel history: %zu event(s) over %zu pass(es) sent", events, pending.size());
}

}  // namespace mtlinsp
