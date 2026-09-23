// Draw-call overlays measured while capturing: where one draw of a render pass landed, as the
// render target tab draws it over the target — RenderDoc's highlight drawcall, depth test,
// stencil test, backface and wireframe overlays, the same five `vkinsp_replay --overlay` measures
// by replaying a Vulkan capture (src/replay/src/overlay.cpp) and the D3D12 library measures inside
// the application (src/d3d12/src/draw_overlay.cpp).
//
// The third consumer of pass_record.h's machinery, after overdraw.mm and pixel_history.mm: nothing
// is rebuilt or replayed here. The pass is issued again into the application's own command buffer
// right after it ends, out of the calls the capture kept, with copies of the application's own
// pipelines. Five runs, each into an R8Unorm target of the pass's size, each read back and folded
// into one byte of OVERLAY_* bits per pixel (renderer/draw_overlay.ts):
//
//   Cover      the draw alone, its own culling, no depth or stencil test -> OVERLAY_COVERED
//   Passed     the pass's earlier draws first, writing no color but still moving depth and
//              stencil, from the copies taken before the pass began; then the draw with its own
//              tests -> OVERLAY_PASSED
//   Wireframe  the draw alone, filled as lines -> OVERLAY_WIREFRAME
//   Stencil    the earlier draws as above, then the draw with its stencil test only
//              -> OVERLAY_STENCIL_PASSED
//   BackFace   the draw alone with nothing culled; a pixel it covers that Cover does not is one
//              the draw's culling removed -> OVERLAY_BACK_FACING
//
// Where D3D12 needs a pipeline variant per run, Metal needs one for all five: the fill mode and the
// cull mode are encoder state (`setTriangleFillMode:`, `setCullMode:`), and the tests are a
// DepthStencilVariant. So every run binds PipelineVariant::OverlayMask and differs only in what it
// sets on the encoder — which is also why the runs cannot simply be one pass with five targets.
//
// A draw is named by its pass and its ordinal within that pass rather than by the command index the
// UI clicked, because the measurement happens while the *next* frame records and that frame's
// commands are numbered again from the start. The reply carries the command index the draw took in
// the new capture, which is the one the overlay is shown on.
//
// What it cannot see is a fragment the draw's own shader discards: the mask shader does not
// discard, so alpha-tested geometry covers its whole quad. The same is true of the other two
// backends' overlays.
#include "pass_record.h"

#include "capture.h"
#include "json_writer.h"
#include "swizzle.h"
#include "transport.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace mtlinsp
{
namespace
{

/** The mask bits, as renderer/draw_overlay.ts reads them. */
constexpr uint8_t kCovered = 1;
constexpr uint8_t kPassed = 2;
constexpr uint8_t kWireframe = 4;
constexpr uint8_t kStencilPassed = 8;
constexpr uint8_t kBackFacing = 16;

/** The runs, in the order they are drawn and read back. */
enum class Run : int
{
    Cover = 0,
    Passed = 1,
    Wireframe = 2,
    Stencil = 3,
    BackFace = 4,
    Count = 5
};
constexpr int kRuns = (int)Run::Count;

/** Writes 1 wherever a fragment lands. The counterpart of overdraw's counting function. */
NSString* const kMaskSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
fragment float gpu_inspector_overlay_mask() { return 1.0; }
)MSL";

/** One overlay waiting for its command buffers to complete. */
struct PendingOverlay
{
    uint32_t frame = 0;
    uint64_t commandBufferId = 0;
    uint32_t passIndex = 0;
    uint32_t drawIndex = 0;
    /** The command the capture recorded the draw as, which is what the UI shows the overlay on. */
    uint32_t command = 0;
    std::string method;
    uint32_t width = 0;
    uint32_t height = 0;
    bool measured = false;
    bool depthTested = false;
    bool stencilTested = false;
    bool backFaceTested = false;
    bool wireframe = false;
    std::string note;
    /** One staging buffer per run, in Run order; nil for a run that was not drawn. */
    id staging[kRuns] = {};
    /**
     * Everything the runs need alive until the command buffer completes.
     *
     * Strong, not raw `id`: this file is built without ARC, so a vector of `id` would hold
     * pointers to objects nothing owns — and the targets are created with a +1 that is dropped
     * straight after, so they would be freed while the GPU was still drawing into them.
     */
    std::vector<Strong> keep;
};

std::mutex g_mutex;
DrawOverlayRequest g_request;
std::vector<PendingOverlay> g_pending;

/**
 * One run's encoder, as the recorded calls see it.
 *
 * Every draw before the requested one is issued with the application's own fragment function and no
 * color writes, so it still moves depth and stencil — which is what makes the Passed and Stencil
 * runs mean anything. The requested draw is issued with the mask function. Draws after it are not
 * issued at all.
 */
class OverlayRun final : public OverdrawReplay
{
public:
    OverlayRun(id<MTLDevice> device, id<MTLFunction> mask, Run run, uint32_t drawIndex,
        MTLPixelFormat depthFormat, MTLPixelFormat stencilFormat, PendingOverlay& out)
        : device_(device), mask_(mask), run_(run), wanted_(drawIndex), depthFormat_(depthFormat), stencilFormat_(stencilFormat), out_(out) {}

    /** Whether the requested draw was reached at all: a pass with fewer draws than asked for. */
    bool found() const { return found_; }

    void BeginEncoder()
    {
        appPipeline_ = nil;
        appDepthStencil_ = nil;
        bound_ = false;
        rasterless_ = false;
    }

    void BindPipeline(id<MTLRenderCommandEncoder> encoder, id state) override
    {
        appPipeline_ = state;
        bound_ = false;
        rasterless_ = false;
        if (state == nil)
            return;
        // Both copies are needed: which one is bound depends on whether this draw is the one being
        // drawn, and that is not known until the draw arrives.
        const DerivedPipeline mask = PipelineCopy(device_, state, PipelineVariant::OverlayMask, mask_,
            depthFormat_, stencilFormat_);
        if (mask.rasterless)
        {
            rasterless_ = true;
            return;
        }
        if (mask.pipeline == nil)
        {
            if (out_.note.empty() && !mask.error.empty())
                out_.note = "the draw was not drawn: " + mask.error;
            return;
        }
        Keep(mask.pipeline);
        bound_ = true;
    }

    void SetDepthStencilState(id<MTLRenderCommandEncoder> encoder, id state) override
    {
        appDepthStencil_ = state;
        // Set as the application had it, so the earlier draws move depth and stencil the way they
        // did. The requested draw's own state is chosen at the draw (see IssueDraw).
        [encoder setDepthStencilState:(id<MTLDepthStencilState>)state];
    }

    void SetCullMode(id<MTLRenderCommandEncoder> encoder, NSUInteger mode) override
    {
        appCull_ = mode;
        [encoder setCullMode:(MTLCullMode)mode];
    }

    void IssueDraw(id<MTLRenderCommandEncoder> encoder,
        const std::function<void(id<MTLRenderCommandEncoder>)>& draw) override
    {
        const uint32_t index = next_++;
        if (index > wanted_)
            return;                 // after the one asked for: nothing to draw
        if (rasterless_)
            return;                     // no fragments either way
        if (index < wanted_)
        {
            // An earlier draw: the application's own shader, writing no color, so depth and
            // stencil move as they did. Not counted as the overlay's.
            if (!NeedsEarlierDraws())
                return;
            const DerivedPipeline quiet = PipelineCopy(device_, appPipeline_, PipelineVariant::OverlayQuiet, nil,
                depthFormat_, stencilFormat_);
            if (quiet.pipeline == nil)
                return;
            Keep(quiet.pipeline);
            [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)quiet.pipeline];
            [encoder setDepthStencilState:(id<MTLDepthStencilState>)appDepthStencil_];
            [encoder setCullMode:(MTLCullMode)appCull_];
            [encoder setTriangleFillMode:MTLTriangleFillModeFill];
            draw(encoder);
            return;
        }

        // The draw itself.
        found_ = true;
        if (!bound_)
            return;
        const DerivedPipeline mask = PipelineCopy(device_, appPipeline_, PipelineVariant::OverlayMask, mask_,
            depthFormat_, stencilFormat_);
        if (mask.pipeline == nil)
            return;
        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)mask.pipeline];
        [encoder setTriangleFillMode:run_ == Run::Wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        // Nothing culled for the back-face run: a pixel it covers that the Cover run does not is a
        // pixel the draw's own culling removed.
        [encoder setCullMode:run_ == Run::BackFace ? MTLCullModeNone : (MTLCullMode)appCull_];
        [encoder setDepthStencilState:(id<MTLDepthStencilState>)TestState()];
        draw(encoder);
        drawn_ = true;
    }

    void Skip() override
    {
        // An indirect command buffer's draws cannot be counted, and there is no way to tell how
        // many of them there were: the ordinals after it would be wrong, so the run stops meaning
        // anything and says so.
        if (next_ <= wanted_ && out_.note.empty())
        {
            out_.note = "the pass executes an indirect command buffer, whose draws cannot be told apart";
        }
        next_++;
    }

    bool drawn() const { return drawn_; }

private:
    /** Only the runs that test anything need the pass's earlier draws to have moved depth and stencil. */
    bool NeedsEarlierDraws() const { return run_ == Run::Passed || run_ == Run::Stencil; }

    /** The depth-stencil state the requested draw is issued with, per run. */
    id TestState() const
    {
        switch (run_)
        {
            case Run::Passed: return DepthStencilCopy(device_, appDepthStencil_, DepthStencilVariant::Both);
            case Run::Stencil: return DepthStencilCopy(device_, appDepthStencil_, DepthStencilVariant::StencilOnly);
            default: return DepthStencilCopy(device_, nil, DepthStencilVariant::None);
        }
    }

    void Keep(id object)
    {
        if (kept_.insert((__bridge const void*)object).second)
            out_.keep.emplace_back(object);
    }

    id<MTLDevice> device_;
    id<MTLFunction> mask_;
    Run run_;
    uint32_t wanted_;
    MTLPixelFormat depthFormat_;
    MTLPixelFormat stencilFormat_;
    PendingOverlay& out_;
    uint32_t next_ = 0;
    id appPipeline_ = nil;
    id appDepthStencil_ = nil;
    NSUInteger appCull_ = MTLCullModeNone;
    bool bound_ = false;
    bool rasterless_ = false;
    bool found_ = false;
    bool drawn_ = false;
    std::unordered_set<const void*> kept_;
};

/** Reads a run's target into a staging buffer at the end of the command buffer. */
id ReadBack(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> target, uint32_t width, uint32_t height)
{
    id<MTLBuffer> staging = [commandBuffer.device newBufferWithLength:(NSUInteger)width * height
                                                              options:MTLResourceStorageModeShared];
    if (staging == nil)
        return nil;
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    if (blit == nil)
    {
        [staging release];
        return nil;
    }
    blit.label = @"gpu-inspector overlay readback";
    [blit copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:staging
               destinationOffset:0
          destinationBytesPerRow:width
        destinationBytesPerImage:(NSUInteger)width * height];
    [blit endEncoding];
    return staging;
}

}  // namespace

bool DrawOverlayWanted()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_request.enabled;
}

bool MatchDrawOverlayPass(uint32_t passIndex)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_request.enabled && g_request.passIndex == passIndex;
}

void StartDrawOverlayCapture(const DrawOverlayRequest& request)
{
    std::vector<PendingOverlay> stale;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_request = request;
        stale.swap(g_pending);
    }
    for (PendingOverlay& p : stale)
    {
        for (id s : p.staging)
            [s release];
    }
    if (request.enabled)
    {
        Log("draw overlay: draw %u of pass %u", request.drawIndex, request.passIndex);
    }
}

void MeasureDrawOverlay(OverdrawPass& pass)
{
    @autoreleasepool
    {
        Internal internal;
        id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)pass.commandBuffer;
        id<MTLDevice> device = commandBuffer.device;

        PendingOverlay out;
        out.frame = pass.frame;
        out.commandBufferId = pass.commandBufferId;
        out.passIndex = pass.passIndex;
        out.width = pass.width;
        out.height = pass.height;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            out.drawIndex = g_request.drawIndex;
        }

    // Which command the capture recorded the requested draw as: the ordinal counted over the
    // pass's recorded draws, which is what the UI shows the overlay on. Found before anything is
    // drawn, so a run that fails still names the draw it was about.
        {
            uint32_t ordinal = 0;
            bool done = false;
            for (const OverdrawPass::Segment& segment : pass.segments)
            {
                for (const LoggedOp& logged : segment.ops)
                {
                    if (logged.key.policy != OpPolicy::Draw)
                        continue;
                    if (ordinal++ != out.drawIndex)
                        continue;
                    out.command = logged.command;
                    out.method = RecordedCommandMethod(logged.command);
                    done = true;
                    break;
                }
                if (done)
                    break;
            }
        }

        if (!pass.note.empty())
        {
            out.note = pass.note;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.push_back(std::move(out));
            return;
        }
        id<MTLFunction> mask = LibraryFunction(device, "gpu_inspector_overlay_mask", kMaskSource);
        if (mask == nil)
        {
            out.note = "the overlay's fragment function did not compile";
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.push_back(std::move(out));
            return;
        }

    // The overlay's own pipelines rasterize at 1x into a 1x mask whatever the application's pass
    // does (PipelineCopy sets rasterSampleCount to 1), which is what the mask means: a pixel is
    // covered or it is not, and resolving coverage would only blur that. So a multisampled pass can
    // still be drawn -- what it cannot do is *test*, because the runs that test start from a copy
    // of the depth the pass began with, and a multisampled depth texture is not copied
    // (overdraw.mm, PrepareOverdrawPass). Which leaves highlight, wireframe and backface working on
    // the multisampled passes most real frames are made of, rather than none of the five.
        const bool hasDepthStencil = !pass.multisampled && (pass.depthFormat != MTLPixelFormatInvalid || pass.stencilFormat != MTLPixelFormatInvalid);
        out.measured = true;
        out.wireframe = true;
        out.backFaceTested = true;
        if (pass.multisampled)
        {
            out.note = "a multisampled pass: its depth and stencil are not copied, so nothing was tested";
        }
        else if (!hasDepthStencil)
        {
            out.note = "the pass has no depth or stencil attachment, so nothing was tested";
        }

        bool found = false;
        for (int run = 0; run < kRuns; run++)
        {
            const Run kind = (Run)run;
            const bool tests = kind == Run::Passed || kind == Run::Stencil;
            if (tests && !hasDepthStencil)
                continue;
        // A pass with no stencil attachment has no stencil test to replay: an absent test passes
        // every fragment, which would read as "the stencil rejected nothing" rather than as "there
        // was no stencil to reject anything".
            if (kind == Run::Stencil && pass.stencilFormat == MTLPixelFormatInvalid)
                continue;

            id<MTLTexture> target = NewRenderTexture(device, MTLPixelFormatR8Unorm, pass.width, pass.height);
            if (target == nil)
            {
                out.note = "no memory for the overlay target";
                out.measured = false;
                break;
            }
            out.keep.emplace_back(target);
            [target release];

        // The depth and stencil the pass started from, for the runs that test: the copies taken
        // before it began, or fresh textures cleared the way it cleared them.
            id<MTLTexture> depth = nil;
            id<MTLTexture> stencil = nil;
            if (tests)
            {
                if (pass.depthFormat != MTLPixelFormatInvalid)
                {
                    depth = pass.depthStart != nil ? pass.depthStart
                                                   : NewRenderTexture(device, pass.depthFormat, pass.width, pass.height);
                    if (depth != nil)
                        out.keep.emplace_back(depth);
                    if (depth != pass.depthStart)
                        [depth release];
                }
                if (pass.combined)
                {
                    stencil = depth;
                }
                else if (pass.stencilFormat != MTLPixelFormatInvalid)
                {
                    stencil = pass.stencilStart != nil ? pass.stencilStart
                                                       : NewRenderTexture(device, pass.stencilFormat, pass.width, pass.height);
                    if (stencil != nil)
                        out.keep.emplace_back(stencil);
                    if (stencil != pass.stencilStart)
                        [stencil release];
                }
            }

            OverlayRun replay(device, mask, kind, out.drawIndex,
                depth != nil ? pass.depthFormat : MTLPixelFormatInvalid,
                stencil != nil ? pass.stencilFormat : MTLPixelFormatInvalid, out);
            bool first = true;
            bool ok = true;
            for (OverdrawPass::Segment& segment : pass.segments)
            {
                if (segment.ops.empty())
                    continue;
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = target;
                rp.colorAttachments[0].loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                if (depth != nil)
                {
                    rp.depthAttachment.texture = depth;
                    rp.depthAttachment.loadAction = !first || pass.depthLoads ? MTLLoadActionLoad : MTLLoadActionClear;
                    rp.depthAttachment.clearDepth = pass.clearDepth;
                    rp.depthAttachment.storeAction = MTLStoreActionStore;
                }
                if (stencil != nil)
                {
                    rp.stencilAttachment.texture = stencil;
                    rp.stencilAttachment.loadAction = !first || pass.stencilLoads ? MTLLoadActionLoad : MTLLoadActionClear;
                    rp.stencilAttachment.clearStencil = pass.clearStencil;
                    rp.stencilAttachment.storeAction = MTLStoreActionStore;
                }
                id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:rp];
                if (encoder == nil)
                {
                    out.note = "could not open an encoder for the overlay";
                    ok = false;
                    break;
                }
                encoder.label = @"gpu-inspector overlay";
                replay.BeginEncoder();
                for (LoggedOp& logged : segment.ops)
                    logged.op(encoder, replay);
                [encoder endEncoding];
                first = false;
            }
            if (!ok)
            {
                out.measured = false;
                break;
            }
            found = found || replay.found();
            if (kind == Run::Passed)
                out.depthTested = replay.drawn();
            if (kind == Run::Stencil)
                out.stencilTested = replay.drawn();
            out.staging[run] = ReadBack(commandBuffer, target, pass.width, pass.height);
        }

        if (out.measured && !found)
        {
            out.measured = false;
            out.note = "the pass has no draw at that ordinal";
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back(std::move(out));
    }
}

void SendDrawOverlay()
{
    std::vector<PendingOverlay> pending;
    bool wanted = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        wanted = g_request.enabled;
        g_request = DrawOverlayRequest();
        pending.swap(g_pending);
    }
    if (!wanted)
        return;

    for (PendingOverlay& p : pending)
    {
        std::vector<uint8_t> merged;
        uint64_t covered = 0, passed = 0, wire = 0, stencilPassed = 0, backFacing = 0, fragments = 0;
        if (p.measured)
        {
            const size_t texels = (size_t)p.width * p.height;
            merged.assign(texels, 0);
            auto bytesOf = [&](Run run) -> const uint8_t* {
                id s = p.staging[(int)run];
                return s != nil ? (const uint8_t*)((id<MTLBuffer>)s).contents : nullptr;
            };
            const uint8_t* cover = bytesOf(Run::Cover);
            const uint8_t* test = p.depthTested ? bytesOf(Run::Passed) : nullptr;
            const uint8_t* lines = bytesOf(Run::Wireframe);
            const uint8_t* stencilRun = p.stencilTested ? bytesOf(Run::Stencil) : nullptr;
            const uint8_t* back = bytesOf(Run::BackFace);
            for (size_t i = 0; i < texels; i++)
            {
                uint8_t bits = 0;
                const bool isCovered = cover != nullptr && cover[i] != 0;
                if (isCovered)
                    bits |= kCovered;
                // A pass with nothing to test against rejects nothing, so every covered pixel
                // passed: the same reading src/replay/src/overlay.cpp:168 and D3D12's
                // draw_overlay.cpp:496 take, so the three backends' overlays say the same thing
                // about a pass with no depth attachment rather than three different things.
                if (test != nullptr ? test[i] != 0 : isCovered)
                    bits |= kPassed;
                if (lines != nullptr && lines[i] != 0)
                    bits |= kWireframe;
                if (stencilRun != nullptr && stencilRun[i] != 0)
                    bits |= kStencilPassed;
                // Only where the culling actually removed what would have been drawn: every pixel
                // of a closed mesh has a back face behind it.
                if (back != nullptr && back[i] != 0 && !isCovered)
                    bits |= kBackFacing;
                merged[i] = bits;
                if (bits & kCovered)
                    covered++;
                if (bits & kPassed)
                    passed++;
                if (bits & kWireframe)
                    wire++;
                if (bits & kStencilPassed)
                    stencilPassed++;
                if (bits & kBackFacing)
                    backFacing++;
            }
            // The mask says which pixels, not how many fragments: an unblended target cannot count
            // overdraw. Reported as the pixels covered rather than left at zero, which would read
            // as a draw that rasterized nothing.
            fragments = covered;
        }

        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("action");
        w.String("CaptureDrawOverlay");
        w.Key("command");
        w.Uint(p.command);
        w.Key("method");
        w.String(p.method);
        w.Key("frame");
        w.Uint(p.frame);
        w.Key("commandBuffer");
        w.Uint(p.commandBufferId);
        w.Key("passIndex");
        w.Uint(p.passIndex);
        w.Key("drawIndex");
        w.Uint(p.drawIndex);
        w.Key("measured");
        w.Boolean(p.measured);
        w.Key("width");
        w.Uint(p.width);
        w.Key("height");
        w.Uint(p.height);
        w.Key("fragments");
        w.Uint(fragments);
        w.Key("pixelsCovered");
        w.Uint(covered);
        w.Key("pixelsPassed");
        w.Uint(passed);
        w.Key("pixelsRejected");
        w.Uint(p.depthTested && covered > passed ? covered - passed : 0);
        w.Key("pixelsStencilRejected");
        w.Uint(p.stencilTested && covered > stencilPassed ? covered - stencilPassed : 0);
        w.Key("pixelsBackFacing");
        w.Uint(backFacing);
        w.Key("depthTested");
        w.Boolean(p.depthTested);
        w.Key("wireframe");
        w.Boolean(p.wireframe && wire != 0);
        w.Key("stencilTested");
        w.Boolean(p.stencilTested);
        w.Key("backFaceTested");
        w.Boolean(p.backFaceTested);
        w.Key("size");
        w.Uint(merged.size());
        if (!p.note.empty())
        {
            w.Key("note");
            w.String(p.note);
        }
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));

        if (!merged.empty())
        {
            vkinsp::JsonWriter h;
            h.BeginObject();
            h.Key("action");
            h.String("CaptureDrawOverlayData");
            h.Key("command");
            h.Uint(p.command);
            h.Key("size");
            h.Uint(merged.size());
            h.EndObject();
            Transport::Get().SendBinary(std::move(h.str()), merged.data(), merged.size());
        }
        for (id s : p.staging)
            [s release];
        Log("draw overlay: draw %u of pass %u, %llu pixels covered, %llu passed", p.drawIndex, p.passIndex,
            (unsigned long long)covered, (unsigned long long)passed);
    }
}

}  // namespace mtlinsp
