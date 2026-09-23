#include "hud.h"

#include "frame_pause.h"
#include "frame_stats.h"
#include "swizzle.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdlib>
#include <cstring>

namespace mtlinsp
{
namespace
{

// Compiled at run time by newLibraryWithSource:, which is the one thing Metal makes easier than
// the other two backends: no offline compiler, and so no generated header to keep in step.
//
// One instance per rectangle, four vertices as a triangle strip, nothing sampled. Rectangles are
// in pixels from the top-left, so the y of the clip-space position is negated -- Metal's clip
// space has y up, as D3D's does and Vulkan's does not.
const char* kHudSource = R"(
#include <metal_stdlib>
using namespace metal;

struct HudRect {
    float4 rect;    // x, y, w, h in pixels
    float4 color;   // straight (non-premultiplied) RGBA
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut hud_vertex(uint vertexId [[vertex_id]],
                            uint instanceId [[instance_id]],
                            constant HudRect *rects [[buffer(0)]],
                            constant float2 &invTargetSize [[buffer(1)]]) {
    float2 corner = float2(vertexId & 1, (vertexId >> 1) & 1);
    float2 px = rects[instanceId].rect.xy + corner * rects[instanceId].rect.zw;
    float2 ndc = px * invTargetSize * 2.0 - 1.0;
    VertexOut out;
    out.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
    out.color = rects[instanceId].color;
    return out;
}

fragment float4 hud_fragment(VertexOut in [[stage_in]]) {
    return in.color;
}
)";

constexpr size_t kRingSize = 3;

}  // namespace

Hud& Hud::Get()
{
    static Hud* instance = [] {
        Hud* hud = new Hud();
        const char* value = getenv("MTLINSP_HUD");
        if (value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0)
            hud->SetEnabled(true);
        return hud;
    }();
    return *instance;
}

void Hud::SetEnabled(bool on)
{
    const bool was = _enabled.exchange(on, std::memory_order_relaxed);
    if (was != on)
        Log("in-app HUD %s", on ? "on" : "off");
}

void Hud::NoteUnsupportedPresentPath()
{
    if (_warnedUnsupportedPath.exchange(true, std::memory_order_relaxed))
        return;
    Log("HUD: this application presents its drawables itself, after the command buffer that drew "
        "them has completed, so there is nothing left to draw the HUD into; live pause still works");
}

// -----------------------------------------------------------------------------------------------
// Setup

Hud::DeviceResources* Hud::Resources(id device)
{
    const void* key = (__bridge const void*)device;
    auto it = _devices.find(key);
    if (it != _devices.end())
        return it->second.failed ? nullptr : &it->second;

    DeviceResources r;
    auto fail = [&](const char* why) -> DeviceResources* {
        Log("HUD: %s; the HUD will not be drawn on this device", why);
        r.failed = true;
        _devices[key] = r;
        return nullptr;
    };

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kHudSource];
    r.library = [[(id<MTLDevice>)device newLibraryWithSource:source options:nil error:&error] retain];
    if (r.library == nil)
    {
        if (error != nil && error.localizedDescription != nil)
            Log("HUD: the shaders did not compile: %s", error.localizedDescription.UTF8String);
        return fail("the shaders did not compile");
    }
    r.vertexFunction = [[(id<MTLLibrary>)r.library newFunctionWithName:@"hud_vertex"] retain];
    r.fragmentFunction = [[(id<MTLLibrary>)r.library newFunctionWithName:@"hud_fragment"] retain];
    if (r.vertexFunction == nil || r.fragmentFunction == nil)
        return fail("a shader function is missing");

    r.frames.resize(kRingSize);
    Log("HUD: ready");
    _devices[key] = r;
    return &_devices[key];
}

id Hud::PipelineFor(id device, DeviceResources& r, uint64_t pixelFormat)
{
    auto it = r.pipelines.find(pixelFormat);
    if (it != r.pipelines.end())
        return it->second;

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = (id<MTLFunction>)r.vertexFunction;
    descriptor.fragmentFunction = (id<MTLFunction>)r.fragmentFunction;
    MTLRenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[0];
    attachment.pixelFormat = (MTLPixelFormat)pixelFormat;
    attachment.blendingEnabled = YES;
    attachment.rgbBlendOperation = MTLBlendOperationAdd;
    attachment.alphaBlendOperation = MTLBlendOperationAdd;
    attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
    attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    NSError* error = nil;
    id pipeline = [[(id<MTLDevice>)device newRenderPipelineStateWithDescriptor:descriptor error:&error] retain];
    [descriptor release];
    if (pipeline == nil)
    {
        Log("HUD: the pipeline did not build for pixel format %llu%s%s", (unsigned long long)pixelFormat,
            error != nil && error.localizedDescription != nil ? ": " : "",
            error != nil && error.localizedDescription != nil ? error.localizedDescription.UTF8String : "");
        return nil;
    }
    r.pipelines[pixelFormat] = pipeline;
    return pipeline;
}

id Hud::BufferFor(id device, DeviceResources& r, uint32_t rects)
{
    Frame& f = r.frames[r.next];
    if (f.buffer != nil && f.capacity >= rects)
        return f.buffer;
    uint32_t capacity = 256;
    while (capacity < rects)
        capacity *= 2;
    id buffer = [[(id<MTLDevice>)device newBufferWithLength:capacity * sizeof(gpuhud::Rect)
                                                    options:MTLResourceStorageModeShared] retain];
    if (buffer == nil)
        return nil;
    [f.buffer release];
    f.buffer = buffer;
    f.capacity = capacity;
    return buffer;
}

// -----------------------------------------------------------------------------------------------
// Timing (the same window as the Vulkan layer's; see src/vulkan/src/hud.cpp)

void Hud::UpdateTiming(DeviceResources& r)
{
    const auto now = std::chrono::steady_clock::now();
    const auto previous = r.lastDraw;
    const uint64_t generation = gpuinsp::FramePause::Get().Generation();
    const bool acrossPause = generation != r.pauseGeneration;
    r.lastDraw = now;
    r.pauseGeneration = generation;
    if (previous.time_since_epoch().count() == 0)
        return;
    if (acrossPause)
        return;   // the interval is the length of a pause, not of a frame
    const double ms = std::chrono::duration<double, std::milli>(now - previous).count();
    if (ms <= 0 || ms > 10000)
        return;

    if (r.windowFrames == 0)
    {
        r.minMs = ms;
        r.maxMs = ms;
    }
    else
    {
        if (ms < r.minMs)
            r.minMs = ms;
        if (ms > r.maxMs)
            r.maxMs = ms;
    }
    r.windowMs += ms;
    r.windowFrames++;
    if (r.smoothedMs == 0)
    {
        r.smoothedMs = ms;
        r.shownMinMs = ms;
        r.shownMaxMs = ms;
    }
    if (r.windowMs >= 500.0)
    {
        r.smoothedMs = r.windowMs / r.windowFrames;
        r.shownMinMs = r.minMs;
        r.shownMaxMs = r.maxMs;
        r.windowMs = 0;
        r.windowFrames = 0;
    }
}

// -----------------------------------------------------------------------------------------------
// Drawing

void Hud::DrawInto(id commandBuffer, id drawable)
{
    if (!Enabled() || commandBuffer == nil || drawable == nil)
        return;

    // Everything below is the library's own Metal, not the application's: without this the shader
    // library, the pipeline, the buffer and above all the render encoder would be announced as the
    // application's objects, and the HUD's pass would be recorded into captures as one of its
    // passes. Covers the helpers too, which are only ever called from here.
    Internal internal;

    // A CAMetalDrawable has a texture; another kind of drawable may not, and there would be
    // nothing to draw into. Sent through the protocol rather than to a bare `id` so the compiler
    // has one declaration of `texture` to pick.
    if (![drawable respondsToSelector:@selector(texture)])
        return;
    id<MTLTexture> texture = [(id<CAMetalDrawable>)drawable texture];
    if (texture == nil)
        return;
    id<MTLDevice> device = [(id<MTLCommandBuffer>)commandBuffer device];
    if (device == nil)
        return;

    std::lock_guard<std::mutex> lock(_mutex);
    DeviceResources* res = Resources((id)device);
    if (!res)
        return;
    DeviceResources& r = *res;
    UpdateTiming(r);
    if (r.smoothedMs <= 0)
        return;

    const uint32_t width = (uint32_t)texture.width;
    const uint32_t height = (uint32_t)texture.height;
    if (!width || !height)
        return;

    gpuhud::HudState state;
    state.frameMs = r.smoothedMs;
    state.minMs = r.shownMinMs;
    state.maxMs = r.shownMaxMs;
    state.frame = FrameNumber() + 1;   // this frame has not ended yet
    state.paused = gpuinsp::FramePause::Get().Paused();
    state.backend = "METAL";
    std::vector<gpuhud::Rect> rects;
    gpuhud::BuildHud(rects, state, width, height, gpuhud::HudScale(width));
    if (rects.empty())
        return;

    id pipeline = PipelineFor((id)device, r, (uint64_t)texture.pixelFormat);
    if (pipeline == nil)
        return;
    id buffer = BufferFor((id)device, r, (uint32_t)rects.size());
    if (buffer == nil)
        return;
    memcpy([(id<MTLBuffer>)buffer contents], rects.data(), rects.size() * sizeof(gpuhud::Rect));
    r.next = (r.next + 1) % r.frames.size();

    // Loads what the application drew and stores it back: the pass adds to the frame rather than
    // replacing it.
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> encoder =
        [(id<MTLCommandBuffer>)commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil)
        return;
    [encoder setLabel:@"GPU Inspector HUD"];
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline];
    [encoder setVertexBuffer:(id<MTLBuffer>)buffer offset:0 atIndex:0];
    const float invTargetSize[2] = {1.0f / (float)width, 1.0f / (float)height};
    [encoder setVertexBytes:invTargetSize length:sizeof(invTargetSize) atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                vertexStart:0
                vertexCount:4
              instanceCount:rects.size()];
    [encoder endEncoding];
}

}  // namespace mtlinsp
