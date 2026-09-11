#include "overdraw.h"

#include "capture.h"
#include "json_writer.h"
#include "swizzle.h"
#include "transport.h"

#import <Metal/Metal.h>
#import <objc/message.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace mtlinsp {

/** A render pass being measured: where it is, what it starts from, and the calls it made. */
struct OverdrawPass {
    id commandBuffer = nil;   // retained
    uint64_t commandBufferId = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    /** Why the pass is not measured at all. */
    std::string note;
    bool multisampled = false;
    bool layered = false;
    MTLPixelFormat depthFormat = MTLPixelFormatInvalid;
    MTLPixelFormat stencilFormat = MTLPixelFormatInvalid;
    /** One texture holds both depth and stencil. */
    bool combined = false;
    bool depthLoads = false;
    bool stencilLoads = false;
    /** Copies of the depth and stencil textures taken before the pass began, when it loads them. Retained. */
    id<MTLTexture> depthStart = nil;
    id<MTLTexture> stencilStart = nil;
    double clearDepth = 1.0;
    uint32_t clearStencil = 0;
    /**
     * The recorded calls, per encoder: one for a render encoder, one per sub-encoder, in creation
     * order, for a parallel render encoder. Each is drawn in an encoder of its own, since each
     * started from Metal's default state.
     */
    struct Segment {
        const void *encoder = nullptr;
        std::vector<OverdrawOp> ops;
    };
    std::vector<Segment> segments;

    ~OverdrawPass() {
        [commandBuffer release];
        [depthStart release];
        [stencilStart release];
    }
};

namespace {

/** One measurement drawn into a command buffer, waiting for it to complete. */
struct PendingMeasurement {
    uint32_t frame = 0;
    uint64_t commandBufferId = 0;
    uint32_t passIndex = 0;
    bool depthTested = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t draws = 0;
    uint32_t skipped = 0;
    std::string note;
    /** The measurement was drawn (its counts are meaningful, if only zeros). */
    bool measured = false;
    /** Nothing was drawn: every count is zero, and there is no staging to read. */
    bool empty = true;
    id<MTLBuffer> staging = nil;   // retained: the count target's half floats, row by row
    /**
     * What the measurement's encoders use and the library made: the count target, the depth
     * copies, the counting pipelines. Kept until the command buffer has completed, because one
     * made with unretained references does not keep them alive itself.
     */
    std::vector<Strong> keep;
};

std::mutex g_mutex;
std::atomic<bool> g_active{false};
uint64_t g_maxDataSize = 256ull << 20;
std::unordered_map<const void *, std::shared_ptr<OverdrawPass>> g_passes;   // pass encoder -> pass
std::unordered_map<const void *, const void *> g_subEncoders;               // sub-encoder -> parallel encoder
std::vector<PendingMeasurement> g_pending;

// Pipelines. Their own lock: a pipeline's dealloc can happen inside any other call.
struct CountingPipeline {
    id pipeline = nil;        // retained
    bool rasterless = false;  // rasterization is off: its draws have no fragments to count
    std::string error;
};
std::mutex g_pipelineMutex;
std::unordered_map<const void *, id> g_descriptors;   // pipeline state -> descriptor copy (retained)
std::map<std::tuple<const void *, MTLPixelFormat, MTLPixelFormat>, CountingPipeline> g_counting;
std::unordered_map<const void *, id> g_countFunctions;   // device -> counting fragment function (retained)

constexpr uint32_t kHistogramBuckets = 8;

float HalfToFloat(uint16_t h) {
    const int sign = (h >> 15) ? -1 : 1;
    const int exponent = (h >> 10) & 0x1F;
    const int mantissa = h & 0x3FF;
    if (exponent == 0) return sign * std::ldexp((float)mantissa, -24);
    if (exponent == 31) return mantissa ? NAN : sign * INFINITY;
    return sign * std::ldexp((float)(mantissa + 1024), exponent - 25);
}

id<MTLTexture> NewTexture(id<MTLDevice> device, MTLPixelFormat format, uint32_t width, uint32_t height) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    return [device newTextureWithDescriptor:d];
}

/** The fragment function that counts: 1.0 into the first colour target. Compiled once per device. */
id<MTLFunction> CountFunction(id<MTLDevice> device) {
    {
        std::lock_guard<std::mutex> lock(g_pipelineMutex);
        auto it = g_countFunctions.find((__bridge const void *)device);
        if (it != g_countFunctions.end()) return it->second;
    }
    static NSString *const kSource = @"#include <metal_stdlib>\n"
                                      "fragment float gpu_inspector_overdraw_count() { return 1.0; }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:kSource options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"gpu_inspector_overdraw_count"];
    [library release];
    if (function == nil) {
        Log("overdraw: the counting fragment function did not compile: %s",
            error.localizedDescription.UTF8String ?: "no error given");
    }
    std::lock_guard<std::mutex> lock(g_pipelineMutex);
    auto it = g_countFunctions.find((__bridge const void *)device);
    if (it != g_countFunctions.end()) {
        [function release];
        return it->second;
    }
    g_countFunctions[(__bridge const void *)device] = function;
    return function;
}

/** Sets a property both descriptor classes (render and mesh) may have, when this one has it. */
void Send(id object, const char *selector, NSUInteger value) {
    SEL sel = sel_registerName(selector);
    if ([object respondsToSelector:sel]) ((void (*)(id, SEL, NSUInteger))objc_msgSend)(object, sel, value);
}

void SendBool(id object, const char *selector, BOOL value) {
    SEL sel = sel_registerName(selector);
    if ([object respondsToSelector:sel]) ((void (*)(id, SEL, BOOL))objc_msgSend)(object, sel, value);
}

/**
 * The counting copy of a pipeline for a pass with these depth and stencil formats: the
 * application's descriptor with the fragment function replaced by the counting one, a single
 * R16Float colour target blended ONE + ONE, one sample, and no alpha to coverage.
 */
CountingPipeline CountingCopy(id<MTLDevice> device, id state, id<MTLFunction> count, MTLPixelFormat depthFormat,
                              MTLPixelFormat stencilFormat) {
    const auto key = std::make_tuple((__bridge const void *)state, depthFormat, stencilFormat);
    id descriptor = nil;
    {
        std::lock_guard<std::mutex> lock(g_pipelineMutex);
        auto cached = g_counting.find(key);
        if (cached != g_counting.end()) return cached->second;
        auto it = g_descriptors.find((__bridge const void *)state);
        if (it != g_descriptors.end()) descriptor = [it->second copy];
    }
    CountingPipeline result;
    if (descriptor == nil) {
        result.error = "a pipeline created before the capture library was loaded, or through a form it does not hook";
    } else {
        SEL rasterization = sel_registerName("isRasterizationEnabled");
        if ([descriptor respondsToSelector:rasterization]
            && !((BOOL (*)(id, SEL))objc_msgSend)(descriptor, rasterization)) {
            result.rasterless = true;
        } else {
            ((void (*)(id, SEL, id))objc_msgSend)(descriptor, sel_registerName("setFragmentFunction:"), count);
            MTLRenderPipelineColorAttachmentDescriptorArray *colors =
                ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("colorAttachments"));
            for (NSUInteger i = 0; i < 8; i++) {
                MTLRenderPipelineColorAttachmentDescriptor *c = colors[i];
                if (i == 0) {
                    c.pixelFormat = MTLPixelFormatR16Float;
                    c.writeMask = MTLColorWriteMaskRed;
                    c.blendingEnabled = YES;
                    c.rgbBlendOperation = MTLBlendOperationAdd;
                    c.alphaBlendOperation = MTLBlendOperationAdd;
                    c.sourceRGBBlendFactor = MTLBlendFactorOne;
                    c.destinationRGBBlendFactor = MTLBlendFactorOne;
                    c.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    c.destinationAlphaBlendFactor = MTLBlendFactorOne;
                } else {
                    c.pixelFormat = MTLPixelFormatInvalid;
                    c.blendingEnabled = NO;
                }
            }
            Send(descriptor, "setDepthAttachmentPixelFormat:", depthFormat);
            Send(descriptor, "setStencilAttachmentPixelFormat:", stencilFormat);
            // rasterSampleCount from macOS 13; sampleCount, its deprecated spelling, before that.
            Send(descriptor, "setSampleCount:", 1);
            Send(descriptor, "setRasterSampleCount:", 1);
            SendBool(descriptor, "setAlphaToCoverageEnabled:", NO);
            SendBool(descriptor, "setAlphaToOneEnabled:", NO);
            // An archive holds the application's pipelines, not this one: a lookup would only miss.
            SEL archives = sel_registerName("setBinaryArchives:");
            if ([descriptor respondsToSelector:archives]) ((void (*)(id, SEL, id))objc_msgSend)(descriptor, archives, nil);

            NSError *error = nil;
            if ([descriptor isKindOfClass:[MTLRenderPipelineDescriptor class]]) {
                result.pipeline = [device newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                                                         error:&error];
            } else {
                SEL mesh = sel_registerName("newRenderPipelineStateWithMeshDescriptor:options:reflection:error:");
                if ([device respondsToSelector:mesh]) {
                    result.pipeline = ((id (*)(id, SEL, id, NSUInteger, id *, NSError **))objc_msgSend)(
                        device, mesh, descriptor, 0, nullptr, &error);
                }
            }
            if (result.pipeline == nil) {
                result.error = error != nil && error.localizedDescription != nil
                    ? std::string("its counting copy did not build: ") + error.localizedDescription.UTF8String
                    : "its counting copy did not build";
                Log("overdraw: %s", result.error.c_str());
            }
        }
        [descriptor release];
    }
    std::lock_guard<std::mutex> lock(g_pipelineMutex);
    auto cached = g_counting.find(key);
    if (cached != g_counting.end()) {
        [result.pipeline release];
        return cached->second;
    }
    g_counting[key] = result;
    return result;
}

/** One of a pass's two measurements, as the recorded calls see it. */
class Measurement final : public OverdrawReplay {
public:
    Measurement(id<MTLDevice> device, id<MTLFunction> count, MTLPixelFormat depthFormat, MTLPixelFormat stencilFormat,
                PendingMeasurement &out)
        : device_(device), count_(count), depthFormat_(depthFormat), stencilFormat_(stencilFormat), out_(out) {}

    void BeginEncoder() {
        bound_ = false;
        rasterless_ = false;
    }

    void BindPipeline(id<MTLRenderCommandEncoder> encoder, id state) override {
        bound_ = false;
        rasterless_ = false;
        if (state == nil) return;
        const CountingPipeline copy = CountingCopy(device_, state, count_, depthFormat_, stencilFormat_);
        if (copy.rasterless) {
            rasterless_ = true;
            return;
        }
        if (copy.pipeline == nil) {
            if (out_.note.empty() && !copy.error.empty()) out_.note = "draws not counted: " + copy.error;
            return;
        }
        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)copy.pipeline];
        if (kept_.insert((__bridge const void *)copy.pipeline).second) out_.keep.emplace_back(copy.pipeline);
        bound_ = true;
    }

    bool TestsDepthStencil() const override {
        return depthFormat_ != MTLPixelFormatInvalid || stencilFormat_ != MTLPixelFormatInvalid;
    }

    bool Draw() override {
        if (bound_) {
            out_.draws++;
            return true;
        }
        if (!rasterless_) out_.skipped++;
        return false;
    }

    void Skip() override { out_.skipped++; }

private:
    id<MTLDevice> device_;
    id<MTLFunction> count_;
    MTLPixelFormat depthFormat_;
    MTLPixelFormat stencilFormat_;
    PendingMeasurement &out_;
    bool bound_ = false;
    bool rasterless_ = false;
    std::unordered_set<const void *> kept_;
};

/** Draws a pass's two measurements into its command buffer. On the application's encoding thread, inside its endEncoding. */
void MeasurePass(OverdrawPass &pass) {
  @autoreleasepool {
    Internal internal;
    id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)pass.commandBuffer;
    id<MTLDevice> device = commandBuffer.device;
    std::vector<PendingMeasurement> results;
    for (int mode = 0; mode < 2; mode++) {
        PendingMeasurement m;
        m.frame = pass.frame;
        m.commandBufferId = pass.commandBufferId;
        m.passIndex = pass.passIndex;
        m.depthTested = mode == 0;
        m.width = pass.width;
        m.height = pass.height;
        if (!pass.note.empty()) {
            m.note = pass.note;
            results.push_back(std::move(m));
            continue;
        }
        const bool hasDepthStencil = pass.depthFormat != MTLPixelFormatInvalid || pass.stencilFormat != MTLPixelFormatInvalid;
        bool tests = m.depthTested && hasDepthStencil;
        if (m.depthTested && !hasDepthStencil) m.note = "the pass has no depth or stencil attachment";
        if (tests && pass.multisampled) {
            tests = false;
            m.note = "multisampled depth and stencil are not copied: counted without the tests";
        }
        if (pass.layered) m.note += std::string(m.note.empty() ? "" : "; ") + "a layered pass: every layer's fragments land in one count";
        id<MTLFunction> count = CountFunction(device);
        if (count == nil) {
            m.note = "the counting fragment function did not compile";
            results.push_back(std::move(m));
            continue;
        }

        id<MTLTexture> target = NewTexture(device, MTLPixelFormatR16Float, pass.width, pass.height);
        if (target == nil) {
            m.note = "no memory for the count target";
            results.push_back(std::move(m));
            continue;
        }
        m.keep.emplace_back(target);
        [target release];
        id<MTLTexture> depth = nil;
        id<MTLTexture> stencil = nil;
        if (tests) {
            // The copies taken before the pass began, or textures to clear the way the pass did.
            if (pass.depthFormat != MTLPixelFormatInvalid) {
                depth = pass.depthStart != nil ? pass.depthStart : NewTexture(device, pass.depthFormat, pass.width, pass.height);
                if (depth != nil) m.keep.emplace_back(depth);
                if (depth != pass.depthStart) [depth release];
            }
            if (pass.combined) {
                stencil = depth;
            } else if (pass.stencilFormat != MTLPixelFormatInvalid) {
                stencil = pass.stencilStart != nil ? pass.stencilStart : NewTexture(device, pass.stencilFormat, pass.width, pass.height);
                if (stencil != nil) m.keep.emplace_back(stencil);
                if (stencil != pass.stencilStart) [stencil release];
            }
        }

        Measurement measurement(device, count, depth != nil ? pass.depthFormat : MTLPixelFormatInvalid,
                                stencil != nil ? pass.stencilFormat : MTLPixelFormatInvalid, m);
        bool first = true;
        m.measured = true;
        for (OverdrawPass::Segment &segment : pass.segments) {
            if (segment.ops.empty()) continue;
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            if (depth != nil) {
                rp.depthAttachment.texture = depth;
                rp.depthAttachment.loadAction = !first || pass.depthLoads ? MTLLoadActionLoad : MTLLoadActionClear;
                rp.depthAttachment.clearDepth = pass.clearDepth;
                rp.depthAttachment.storeAction = MTLStoreActionStore;
            }
            if (stencil != nil) {
                rp.stencilAttachment.texture = stencil;
                rp.stencilAttachment.loadAction = !first || pass.stencilLoads ? MTLLoadActionLoad : MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = pass.clearStencil;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:rp];
            if (encoder == nil) {
                m.note = "could not open an encoder for the measurement";
                m.measured = false;
                break;
            }
            encoder.label = m.depthTested ? @"gpu-inspector overdraw (depth tested)" : @"gpu-inspector overdraw";
            measurement.BeginEncoder();
            for (OverdrawOp &op : segment.ops) op(encoder, measurement);
            [encoder endEncoding];
            first = false;
        }
        if (m.measured && !first) {
            const NSUInteger bytes = (NSUInteger)pass.width * pass.height * 2;
            id<MTLBuffer> staging = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = staging != nil ? [commandBuffer blitCommandEncoder] : nil;
            if (blit == nil) {
                [staging release];
                m.note = "no staging memory for the counts";
                m.measured = false;
            } else {
                blit.label = @"gpu-inspector overdraw readback";
                [blit copyFromTexture:target
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(pass.width, pass.height, 1)
                             toBuffer:staging
                    destinationOffset:0
               destinationBytesPerRow:(NSUInteger)pass.width * 2
             destinationBytesPerImage:bytes];
                [blit endEncoding];
                m.staging = staging;
                m.empty = false;
            }
        }
        results.push_back(std::move(m));
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (PendingMeasurement &m : results) g_pending.push_back(std::move(m));
  }
}

}  // namespace

// --------------------------------------------------------------------------------------------

bool OverdrawActive() {
    return g_active.load(std::memory_order_relaxed);
}

void LogOverdrawOp(id encoder, OverdrawOp op) {
    if (!OverdrawActive() || encoder == nil) return;
    const void *key = (__bridge const void *)encoder;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto sub = g_subEncoders.find(key);
    auto it = g_passes.find(sub != g_subEncoders.end() ? sub->second : key);
    if (it == g_passes.end()) return;
    std::vector<OverdrawPass::Segment> &segments = it->second->segments;
    for (auto s = segments.rbegin(); s != segments.rend(); ++s) {
        if (s->encoder == key) {
            s->ops.push_back(std::move(op));
            return;
        }
    }
    segments.push_back({key, {}});
    segments.back().ops.push_back(std::move(op));
}

std::shared_ptr<OverdrawPass> PrepareOverdrawPass(id commandBuffer, MTLRenderPassDescriptor *descriptor) {
    if (!OverdrawActive() || commandBuffer == nil || descriptor == nil) return nullptr;
    auto pass = std::make_shared<OverdrawPass>();
    pass->commandBuffer = [commandBuffer retain];
    pass->commandBufferId = CommandBufferId(commandBuffer);
    pass->frame = CaptureFrameIndex();

    // The count target takes the size of the first attachment, at the level the pass renders to.
    id<MTLTexture> sized = nil;
    NSUInteger level = 0;
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = descriptor.colorAttachments[i];
        if (c.texture == nil) continue;
        if (sized == nil) {
            sized = c.texture;
            level = c.level;
        }
        if (c.texture.sampleCount > 1) pass->multisampled = true;
    }
    MTLRenderPassDepthAttachmentDescriptor *depth = descriptor.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *stencil = descriptor.stencilAttachment;
    if (sized == nil && depth.texture != nil) {
        sized = depth.texture;
        level = depth.level;
    }
    if (sized == nil && stencil.texture != nil) {
        sized = stencil.texture;
        level = stencil.level;
    }
    if (sized == nil) {
        pass->note = "the pass has no attachments";
        return pass;
    }
    pass->width = (uint32_t)std::max<NSUInteger>(1, sized.width >> level);
    pass->height = (uint32_t)std::max<NSUInteger>(1, sized.height >> level);
    pass->layered = descriptor.renderTargetArrayLength > 1;
    auto memoryless = [](id<MTLTexture> t) { return t.storageMode == MTLStorageModeMemoryless; };
    if (depth.texture != nil) {
        pass->depthFormat = depth.texture.pixelFormat;
        pass->clearDepth = depth.clearDepth;
        pass->depthLoads = depth.loadAction == MTLLoadActionLoad && !memoryless(depth.texture);
        if (depth.texture.sampleCount > 1) pass->multisampled = true;
    }
    if (stencil.texture != nil) {
        pass->stencilFormat = stencil.texture.pixelFormat;
        pass->clearStencil = stencil.clearStencil;
        pass->stencilLoads = stencil.loadAction == MTLLoadActionLoad && !memoryless(stencil.texture);
        if (stencil.texture.sampleCount > 1) pass->multisampled = true;
    }
    pass->combined = depth.texture != nil && depth.texture == stencil.texture;
    if (pass->multisampled) return pass;

    // What the pass loads, copied before it can change it. The command buffer is free: the
    // application is asking for its next encoder.
    const bool copyDepth = depth.texture != nil && (pass->depthLoads || (pass->combined && pass->stencilLoads));
    const bool copyStencil = stencil.texture != nil && !pass->combined && pass->stencilLoads;
    if (!copyDepth && !copyStencil) return pass;
    Internal internal;
    id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)commandBuffer;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (blit == nil) return pass;
    blit.label = @"gpu-inspector overdraw depth";
    auto copy = [&](MTLRenderPassAttachmentDescriptor *a) -> id<MTLTexture> {
        const uint32_t width = (uint32_t)std::max<NSUInteger>(1, a.texture.width >> a.level);
        const uint32_t height = (uint32_t)std::max<NSUInteger>(1, a.texture.height >> a.level);
        if (width != pass->width || height != pass->height) return nil;
        id<MTLTexture> start = NewTexture(cb.device, a.texture.pixelFormat, width, height);
        if (start == nil) return nil;
        [blit copyFromTexture:a.texture
                  sourceSlice:a.slice
                  sourceLevel:a.level
                 sourceOrigin:MTLOriginMake(0, 0, a.texture.textureType == MTLTextureType3D ? a.depthPlane : 0)
                   sourceSize:MTLSizeMake(width, height, 1)
                    toTexture:start
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        return start;
    };
    if (copyDepth) pass->depthStart = copy(depth);
    if (copyStencil) pass->stencilStart = copy(stencil);
    [blit endEncoding];
    // A copy that could not be made starts from the clear value instead.
    if (pass->depthStart == nil) pass->depthLoads = false;
    if (pass->stencilStart == nil && !pass->combined) pass->stencilLoads = false;
    if (pass->combined && pass->depthStart == nil) pass->stencilLoads = false;
    return pass;
}

void BeginOverdrawPass(id encoder, std::shared_ptr<OverdrawPass> pass, uint32_t passIndex) {
    if (!pass || encoder == nil) return;
    pass->passIndex = passIndex;
    pass->segments.push_back({(__bridge const void *)encoder, {}});
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!OverdrawActive()) return;
    g_passes[(__bridge const void *)encoder] = std::move(pass);
}

void NoteOverdrawSubEncoder(id parent, id encoder) {
    if (!OverdrawActive() || parent == nil || encoder == nil) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_passes.find((__bridge const void *)parent);
    if (it == g_passes.end()) return;
    g_subEncoders[(__bridge const void *)encoder] = (__bridge const void *)parent;
    it->second->segments.push_back({(__bridge const void *)encoder, {}});
}

void EndOverdrawPass(id encoder) {
    if (!OverdrawActive()) return;
    std::shared_ptr<OverdrawPass> pass;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_passes.find((__bridge const void *)encoder);
        if (it == g_passes.end()) return;
        pass = std::move(it->second);
        g_passes.erase(it);
        for (auto s = g_subEncoders.begin(); s != g_subEncoders.end();) {
            if (s->second == (__bridge const void *)encoder) s = g_subEncoders.erase(s);
            else ++s;
        }
    }
    MeasurePass(*pass);
    // The recorded calls let go of what they held here, outside the lock.
}

void RememberRenderPipeline(id state, id descriptor) {
    if (state == nil || descriptor == nil) return;
    id kept = [descriptor copy];
    std::lock_guard<std::mutex> lock(g_pipelineMutex);
    id &slot = g_descriptors[(__bridge const void *)state];
    [slot release];
    slot = kept;
}

void ForgetRenderPipeline(id object) {
    std::vector<id> released;
    {
        std::lock_guard<std::mutex> lock(g_pipelineMutex);
        if (g_descriptors.empty()) return;
        const void *key = (__bridge const void *)object;
        auto it = g_descriptors.find(key);
        if (it == g_descriptors.end()) return;
        released.push_back(it->second);
        g_descriptors.erase(it);
        for (auto c = g_counting.begin(); c != g_counting.end();) {
            if (std::get<0>(c->first) == key) {
                if (c->second.pipeline != nil) released.push_back(c->second.pipeline);
                c = g_counting.erase(c);
            } else {
                ++c;
            }
        }
    }
    for (id o : released) [o release];
}

void StartOverdrawCapture(bool enabled, uint64_t maxDataSize) {
    std::vector<PendingMeasurement> pending;
    std::unordered_map<const void *, std::shared_ptr<OverdrawPass>> passes;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pending.swap(g_pending);
        passes.swap(g_passes);
        g_subEncoders.clear();
        g_maxDataSize = maxDataSize;
        g_active = enabled;
    }
    for (PendingMeasurement &m : pending) [m.staging release];
    if (enabled) Log("overdraw: measuring every render pass of the capture");
}

void SendOverdraw() {
    std::vector<PendingMeasurement> pending;
    std::unordered_map<const void *, std::shared_ptr<OverdrawPass>> unfinished;
    uint64_t maxDataSize = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const bool wasActive = g_active.exchange(false);
        pending.swap(g_pending);
        unfinished.swap(g_passes);
        g_subEncoders.clear();
        maxDataSize = g_maxDataSize;
        if (!wasActive && pending.empty()) return;
    }

    // The counts, and what they add up to.
    struct Result {
        uint64_t fragments = 0;
        uint64_t covered = 0;
        uint32_t maxCount = 0;
        uint64_t histogram[kHistogramBuckets] = {};
        std::vector<uint8_t> counts;   // u16 per pixel, little endian
    };
    std::vector<Result> results(pending.size());
    for (size_t i = 0; i < pending.size(); i++) {
        PendingMeasurement &m = pending[i];
        Result &r = results[i];
        const size_t pixels = (size_t)m.width * m.height;
        if (!m.measured) continue;                 // no counts: the note says why
        if (m.empty) {
            r.counts.assign(pixels * 2, 0);        // a pass with no calls to draw
            continue;
        }
        const uint8_t *bytes = m.staging != nil ? static_cast<const uint8_t *>(m.staging.contents) : nullptr;
        if (bytes == nullptr) {
            m.note = "the count target could not be read";
            m.measured = false;
            continue;
        }
        r.counts.resize(pixels * 2);
        for (size_t p = 0; p < pixels; p++) {
            const float value = HalfToFloat((uint16_t)(bytes[p * 2] | (bytes[p * 2 + 1] << 8)));
            const uint32_t n = std::isfinite(value) && value > 0 ? (uint32_t)std::min(65535L, std::lround(value)) : 0;
            r.counts[p * 2] = (uint8_t)(n & 0xFF);
            r.counts[p * 2 + 1] = (uint8_t)(n >> 8);
            if (n == 0) continue;
            r.fragments += n;
            r.covered++;
            r.maxCount = std::max(r.maxCount, n);
            const int bucket = n <= 4 ? (int)n - 1 : n <= 8 ? 4 : n <= 16 ? 5 : n <= 32 ? 6 : 7;
            r.histogram[bucket]++;
        }
    }
    for (size_t i = 0; i < pending.size(); i++) {
        if (results[i].counts.size() > maxDataSize) {
            results[i].counts.clear();
            pending[i].note += std::string(pending[i].note.empty() ? "" : "; ") + "per-pixel counts not sent: larger than the capture's texture size limit";
        }
    }

    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureOverdraw");
    w.Key("count"); w.Uint(pending.size());
    w.Key("passes"); w.BeginArray();
    for (size_t i = 0; i < pending.size(); i++) {
        const PendingMeasurement &m = pending[i];
        const Result &r = results[i];
        w.BeginObject();
        w.Key("frame"); w.Uint(m.frame);
        w.Key("commandBuffer"); w.Uint(m.commandBufferId);
        w.Key("passIndex"); w.Uint(m.passIndex);
        w.Key("depthTested"); w.Boolean(m.depthTested);
        w.Key("measured"); w.Boolean(m.measured);
        w.Key("width"); w.Uint(m.width);
        w.Key("height"); w.Uint(m.height);
        w.Key("fragments"); w.Uint(r.fragments);
        w.Key("coveredPixels"); w.Uint(r.covered);
        w.Key("maxCount"); w.Uint(r.maxCount);
        w.Key("draws"); w.Uint(m.draws);
        w.Key("skippedDraws"); w.Uint(m.skipped);
        w.Key("histogram"); w.BeginArray();
        for (uint64_t h : r.histogram) w.Uint(h);
        w.EndArray();
        w.Key("size"); w.Uint(r.counts.size());
        if (!m.note.empty()) { w.Key("note"); w.String(m.note); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));

    for (size_t i = 0; i < pending.size(); i++) {
        PendingMeasurement &m = pending[i];
        if (!results[i].counts.empty()) {
            vkinsp::JsonWriter h;
            h.BeginObject();
            h.Key("action"); h.String("CaptureOverdrawData");
            h.Key("frame"); h.Uint(m.frame);
            h.Key("commandBuffer"); h.Uint(m.commandBufferId);
            h.Key("passIndex"); h.Uint(m.passIndex);
            h.Key("depthTested"); h.Boolean(m.depthTested);
            h.Key("size"); h.Uint(results[i].counts.size());
            h.EndObject();
            Transport::Get().SendBinary(std::move(h.str()), std::move(results[i].counts));
        }
        [m.staging release];
        m.staging = nil;
    }
    Log("overdraw: %zu measurement(s) sent%s", pending.size(),
        unfinished.empty() ? "" : " (passes still open at the end of the capture were not measured)");
}

}  // namespace mtlinsp
