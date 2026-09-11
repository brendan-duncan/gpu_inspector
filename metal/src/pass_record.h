// What overdraw.mm and pixel_history.mm share: a render pass whose encoder calls were recorded while
// capturing, and the copies of the application's pipelines and depth-stencil states both
// measurements draw with. Internal to the library; the hooks use overdraw.h.
#pragma once

#include "overdraw.h"

#include <memory>
#include <string>
#include <vector>

#import <Metal/Metal.h>

namespace mtlinsp {

/** A pixel followed through a pass (pixel_history.mm). */
struct HistoryPass;

/** A recorded call: what it does, what state it sets, and the command the capture recorded it as. */
struct LoggedOp {
    OverdrawOp op;
    OpKey key;
    uint32_t command = 0;
};

/** A render pass being measured: where it is, what it starts from, and the calls it made. */
struct OverdrawPass {
    id commandBuffer = nil;   // retained
    uint64_t commandBufferId = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    /** The command the capture recorded the pass's beginning as. */
    uint32_t beginCommand = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    /** Whether the capture measures the pass's overdraw. */
    bool measureOverdraw = false;
    /** Why the pass's overdraw is not measured at all. */
    std::string note;
    bool multisampled = false;
    bool layered = false;
    MTLPixelFormat depthFormat = MTLPixelFormatInvalid;
    MTLPixelFormat stencilFormat = MTLPixelFormatInvalid;
    /** One texture holds both depth and stencil. */
    bool combined = false;
    bool depthLoads = false;
    bool stencilLoads = false;
    /** Overdraw: copies of the depth and stencil textures taken before the pass began, when it loads them. Retained. */
    id<MTLTexture> depthStart = nil;
    id<MTLTexture> stencilStart = nil;
    double clearDepth = 1.0;
    uint32_t clearStencil = 0;
    /** The pixel followed through the pass, when the capture follows one of its render target's. */
    std::shared_ptr<HistoryPass> history;
    /**
     * The recorded calls, per encoder: one for a render encoder, one per sub-encoder, in creation
     * order, for a parallel render encoder. Each is drawn in an encoder of its own, since each
     * started from Metal's default state.
     */
    struct Segment {
        const void *encoder = nullptr;
        std::vector<LoggedOp> ops;
    };
    std::vector<Segment> segments;

    ~OverdrawPass() {
        [commandBuffer release];
        [depthStart release];
        [stencilStart release];
    }
};

/** A private render target texture of one level; +1, the caller releases it. */
id<MTLTexture> NewRenderTexture(id<MTLDevice> device, MTLPixelFormat format, uint32_t width, uint32_t height);

/** A function of Metal Shading Language compiled once per device; nil when it does not compile. Not retained for the caller. */
id<MTLFunction> LibraryFunction(id<MTLDevice> device, const char *name, NSString *source);

enum class PipelineVariant : int {
    /** Overdraw: the counting fragment function into one R16Float target blended ONE + ONE, one sample. */
    OverdrawCount = 0,
    /** Pixel history: `fragment` (writes nothing) in place of the fragment function, no colour writes, no alpha to coverage. */
    HistoryCover = 1,
    /** Pixel history: the application's fragment function, no colour writes. */
    HistoryNoWrite = 2,
};

struct DerivedPipeline {
    id pipeline = nil;        // not retained for the caller: kept by the cache until the pipeline is released
    bool rasterless = false;  // rasterization is off: its draws have no fragments
    std::string error;
};

/**
 * A copy of a recorded pipeline for a measurement, cached per pipeline, variant and (for the
 * overdraw count, which changes the attachment formats) the depth and stencil formats.
 */
DerivedPipeline PipelineCopy(id<MTLDevice> device, id state, PipelineVariant variant, id<MTLFunction> fragment,
                             MTLPixelFormat depthFormat = MTLPixelFormatInvalid, MTLPixelFormat stencilFormat = MTLPixelFormatInvalid);

enum class DepthStencilVariant : int {
    /** Tests nothing and writes nothing: Metal's default state. */
    None = 0,
    /** The state's depth test, writing nothing, without the stencil test. */
    DepthOnly = 1,
    /** The state's stencil test, keeping the stencil, without the depth test. */
    StencilOnly = 2,
    /** Both tests, writing nothing. */
    Both = 3,
};

/**
 * A copy of a recorded depth-stencil state that writes nothing; for a nil state (Metal's default)
 * the default. Nil when the state's descriptor was not recorded. Not retained for the caller.
 */
id DepthStencilCopy(id<MTLDevice> device, id state, DepthStencilVariant variant);

/** The colour attachment of a pass that renders to the pixel the capture follows, or -1. */
int MatchPixelHistoryAttachment(MTLRenderPassDescriptor *descriptor);

/** Before the pass begins: copies of its attachments at the pixel, for the history to start from. */
void PreparePixelHistory(OverdrawPass &pass, id commandBuffer, MTLRenderPassDescriptor *descriptor, int attachment);

/** After the application's endEncoding: the pass drawn again one draw at a time, measured at the pixel. */
void FollowPixel(OverdrawPass &pass);

}  // namespace mtlinsp
