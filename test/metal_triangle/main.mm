// A small Metal application, the counterpart of test/triangle: something to point the Metal
// capture library at without needing a Unity build. It exercises the parts of the API the
// library has to intercept — a device, a command queue, buffers in shared and private storage, a
// library compiled at run time, two render pipelines, a sampler, a compute pass, a multisampled
// render pass through a parallel encoder resolving into a texture, a render pass to the drawable
// that samples it with inline constants bound, and a present.
//
//   mtlinsp_triangle              a window, until it is closed
//   mtlinsp_triangle --frames N   render N frames and exit (no interaction needed)
//   mtlinsp_triangle --present-direct
//                                 present through [drawable present] from a scheduled handler,
//                                 the way Unity's macOS player does, instead of through
//                                 [MTLCommandBuffer presentDrawable:]
//
// Built unsigned by CMake, so DYLD_INSERT_LIBRARIES reaches it. See metal/README.md.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdlib>
#include <cstring>

namespace {

// Position (x, y) and colour (r, g, b) per vertex, in the order the vertex descriptor expects.
const float kVertices[] = {
     0.0f,  0.6f,   1.0f, 0.2f, 0.2f,
    -0.6f, -0.4f,   0.2f, 1.0f, 0.2f,
     0.6f, -0.4f,   0.2f, 0.2f, 1.0f,
};
const uint16_t kIndices[] = { 0, 1, 2 };

// Both stages in one library, plus a compute kernel, so the run keeps a compute pass in it, and
// the pass that copies the resolved triangle to the drawable: a full-screen triangle from the
// vertex id, sampling the resolve with a tint bound as inline bytes.
NSString *const kShaderSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn  { float2 position [[attribute(0)]]; float3 colour [[attribute(1)]]; };
struct VertexOut { float4 position [[position]];     float3 colour; };
struct Uniforms  { float angle; float scale; };
struct BlitOut   { float4 position [[position]];     float2 uv; };

vertex BlitOut blit_vertex(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    BlitOut out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

fragment float4 blit_fragment(BlitOut in [[stage_in]],
                              texture2d<float> source [[texture(0)]],
                              sampler smp [[sampler(0)]],
                              constant float4 &tint [[buffer(0)]]) {
    return source.sample(smp, in.uv) * tint;
}

vertex VertexOut vertex_main(VertexIn in [[stage_in]],
                             constant Uniforms &u [[buffer(1)]],
                             uint instance [[instance_id]]) {
    float c = cos(u.angle + float(instance) * 0.7);
    float s = sin(u.angle + float(instance) * 0.7);
    float2 p = float2(in.position.x * c - in.position.y * s,
                      in.position.x * s + in.position.y * c) * u.scale;
    VertexOut out;
    out.position = float4(p, 0.0, 1.0);
    out.colour = in.colour;
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]]) {
    return float4(in.colour, 1.0);
}

kernel void wave_main(device float *values [[buffer(0)]],
                      constant Uniforms &u [[buffer(1)]],
                      uint i [[thread_position_in_grid]]) {
    values[i] = sin(u.angle + float(i) * 0.05);
}
)MSL";

struct Uniforms {
    float angle;
    float scale;
};

constexpr NSUInteger kWaveCount = 256;

}  // namespace

// ------------------------------------------------------------------------------------------

@interface Renderer : NSObject
- (instancetype)initWithLayer:(CAMetalLayer *)layer;
- (void)renderFrame;
@property(nonatomic, readonly) NSUInteger frameCount;
/** --present-direct: present through the drawable, the way Unity's macOS player does. */
@property(nonatomic) BOOL presentDirect;
@end

@implementation Renderer {
    CAMetalLayer *_layer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLRenderPipelineState> _blit;
    id<MTLComputePipelineState> _wave;
    id<MTLSamplerState> _sampler;
    // The triangle is drawn into a 4x multisampled target resolved into _resolved, which the
    // drawable pass then samples. The private-storage vertices are what an engine binds.
    id<MTLTexture> _msaaTarget;
    id<MTLTexture> _resolved;
    id<MTLBuffer> _vertices;
    id<MTLBuffer> _verticesPrivate;
    id<MTLBuffer> _indices;
    id<MTLBuffer> _uniforms;
    id<MTLBuffer> _waveOut;
    // Resources allocated while running rather than at start-up, so that a capture library has
    // something to stream to a UI that is already connected — the snapshot path and the live path
    // are different code and only one of them is exercised by start-up allocations.
    NSMutableArray *_later;
    NSUInteger _frameCount;
}

- (instancetype)initWithLayer:(CAMetalLayer *)layer {
    if (!(self = [super init])) return nil;
    _layer = layer;
    _device = layer.device;
    _queue = [_device newCommandQueue];
    _queue.label = @"triangle queue";

    NSError *error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:kShaderSource options:nil error:&error];
    if (!library) {
        NSLog(@"shader compilation failed: %@", error);
        exit(1);
    }
    library.label = @"triangle shaders";

    MTLVertexDescriptor *vertexDescriptor = [[MTLVertexDescriptor alloc] init];
    vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
    vertexDescriptor.attributes[0].offset = 0;
    vertexDescriptor.attributes[0].bufferIndex = 0;
    vertexDescriptor.attributes[1].format = MTLVertexFormatFloat3;
    vertexDescriptor.attributes[1].offset = sizeof(float) * 2;
    vertexDescriptor.attributes[1].bufferIndex = 0;
    vertexDescriptor.layouts[0].stride = sizeof(float) * 5;

    MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.label = @"triangle pipeline";
    pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    pipelineDescriptor.vertexDescriptor = vertexDescriptor;
    pipelineDescriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
    pipelineDescriptor.rasterSampleCount = 4;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    if (!_pipeline) {
        NSLog(@"pipeline creation failed: %@", error);
        exit(1);
    }

    // The drawable pass, made through the form an engine uses: options and reflection.
    MTLRenderPipelineDescriptor *blitDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    blitDescriptor.label = @"blit pipeline";
    blitDescriptor.vertexFunction = [library newFunctionWithName:@"blit_vertex"];
    blitDescriptor.fragmentFunction = [library newFunctionWithName:@"blit_fragment"];
    blitDescriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
    MTLAutoreleasedRenderPipelineReflection reflection = nil;
    _blit = [_device newRenderPipelineStateWithDescriptor:blitDescriptor
                                                  options:MTLPipelineOptionNone
                                               reflection:&reflection
                                                    error:&error];
    if (!_blit) {
        NSLog(@"blit pipeline creation failed: %@", error);
        exit(1);
    }

    MTLSamplerDescriptor *samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
    samplerDescriptor.label = @"linear clamp";
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _sampler = [_device newSamplerStateWithDescriptor:samplerDescriptor];

    const CGSize size = layer.drawableSize;
    MTLTextureDescriptor *msaa = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:layer.pixelFormat
                                                                                    width:(NSUInteger)size.width
                                                                                   height:(NSUInteger)size.height
                                                                                mipmapped:NO];
    msaa.textureType = MTLTextureType2DMultisample;
    msaa.sampleCount = 4;
    msaa.usage = MTLTextureUsageRenderTarget;
    msaa.storageMode = MTLStorageModePrivate;
    _msaaTarget = [_device newTextureWithDescriptor:msaa];
    _msaaTarget.label = @"triangle msaa";
    MTLTextureDescriptor *resolved = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:layer.pixelFormat
                                                                                        width:(NSUInteger)size.width
                                                                                       height:(NSUInteger)size.height
                                                                                    mipmapped:NO];
    resolved.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    resolved.storageMode = MTLStorageModePrivate;
    _resolved = [_device newTextureWithDescriptor:resolved];
    _resolved.label = @"triangle resolved";

    _wave = [_device newComputePipelineStateWithFunction:[library newFunctionWithName:@"wave_main"]
                                                   error:&error];
    if (!_wave) {
        NSLog(@"compute pipeline creation failed: %@", error);
        exit(1);
    }

    _vertices = [_device newBufferWithBytes:kVertices length:sizeof(kVertices)
                                    options:MTLResourceStorageModeShared];
    _vertices.label = @"vertices";
    // Private storage, filled by a blit the way an engine uploads its meshes: the vertices the
    // draw actually uses, so that a capture has to read a buffer with no CPU side.
    _verticesPrivate = [_device newBufferWithLength:sizeof(kVertices)
                                            options:MTLResourceStorageModePrivate];
    _verticesPrivate.label = @"vertices (private)";
    {
        id<MTLCommandBuffer> upload = [_queue commandBuffer];
        upload.label = @"upload";
        id<MTLBlitCommandEncoder> blit = [upload blitCommandEncoder];
        blit.label = @"vertex upload";
        [blit copyFromBuffer:_vertices sourceOffset:0 toBuffer:_verticesPrivate destinationOffset:0
                        size:sizeof(kVertices)];
        [blit endEncoding];
        [upload commit];
        [upload waitUntilCompleted];
    }
    _indices = [_device newBufferWithBytes:kIndices length:sizeof(kIndices)
                                   options:MTLResourceStorageModeShared];
    _indices.label = @"indices";
    _uniforms = [_device newBufferWithLength:sizeof(Uniforms) options:MTLResourceStorageModeShared];
    _uniforms.label = @"uniforms";
    _waveOut = [_device newBufferWithLength:kWaveCount * sizeof(float)
                                    options:MTLResourceStorageModeShared];
    _waveOut.label = @"wave output";
    _later = [NSMutableArray array];
    return self;
}

- (void)renderFrame {
    id<CAMetalDrawable> drawable = [_layer nextDrawable];
    if (!drawable) return;

    Uniforms uniforms = { .angle = (float)_frameCount * 0.02f, .scale = 0.8f };
    memcpy(_uniforms.contents, &uniforms, sizeof(uniforms));

    id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
    commandBuffer.label = @"frame";

    // Compute pass first: Metal has a real compute encoder, so this is a pass of its own rather
    // than a run of dispatches the way it has to be inferred in Vulkan.
    id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
    compute.label = @"wave";
    [compute setComputePipelineState:_wave];
    [compute setBuffer:_waveOut offset:0 atIndex:0];
    [compute setBuffer:_uniforms offset:0 atIndex:1];
    [compute dispatchThreads:MTLSizeMake(kWaveCount, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [compute endEncoding];

    // The triangle, into the multisampled target, resolved: a capture reads the resolve, since a
    // multisample texture cannot be copied to a buffer. Through a parallel encoder, whose
    // sub-encoder does the drawing, the way a multithreaded engine records a pass.
    MTLRenderPassDescriptor *trianglePass = [MTLRenderPassDescriptor renderPassDescriptor];
    trianglePass.colorAttachments[0].texture = _msaaTarget;
    trianglePass.colorAttachments[0].resolveTexture = _resolved;
    trianglePass.colorAttachments[0].loadAction = MTLLoadActionClear;
    trianglePass.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.09, 0.11, 1.0);
    trianglePass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;

    id<MTLParallelRenderCommandEncoder> parallel =
        [commandBuffer parallelRenderCommandEncoderWithDescriptor:trianglePass];
    parallel.label = @"triangle (parallel)";
    id<MTLRenderCommandEncoder> encoder = [parallel renderCommandEncoder];
    encoder.label = @"triangle";
    [encoder pushDebugGroup:@"triangles"];
    [encoder setRenderPipelineState:_pipeline];
    [encoder setVertexBuffer:_verticesPrivate offset:0 atIndex:0];
    [encoder setVertexBuffer:_uniforms offset:0 atIndex:1];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:sizeof(kIndices) / sizeof(kIndices[0])
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:_indices
                 indexBufferOffset:0
                     instanceCount:3];
    [encoder popDebugGroup];
    [encoder endEncoding];
    [parallel endEncoding];

    // The resolve onto the drawable, tinted through inline constants. Store, so a capture that
    // reads the attachment back sees the result. A pass that did not store would be the Metal
    // counterpart of Vulkan's storeOp DONT_CARE problem.
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> blit = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    blit.label = @"blit";
    [blit setRenderPipelineState:_blit];
    [blit setFragmentTexture:_resolved atIndex:0];
    [blit setFragmentSamplerState:_sampler atIndex:0];
    const float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    [blit setFragmentBytes:tint length:sizeof(tint) atIndex:0];
    [blit drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [blit endEncoding];

    if (self.presentDirect) {
        // What Unity's macOS player does: present the drawable itself from a scheduled handler
        // rather than through [MTLCommandBuffer presentDrawable:]. The frame boundary then
        // arrives on Metal's callback thread, after the command buffer is already committed.
        [commandBuffer addScheduledHandler:^(id<MTLCommandBuffer> _) { [drawable present]; }];
    } else {
        [commandBuffer presentDrawable:drawable];
    }
    [commandBuffer commit];
    _frameCount++;

    // One more resource every second, held so it stays alive.
    if (_frameCount % 60 == 0) {
        id<MTLBuffer> buffer = [_device newBufferWithLength:4096
                                                    options:MTLResourceStorageModeShared];
        buffer.label = [NSString stringWithFormat:@"streamed %lu", (unsigned long)_frameCount / 60];
        [_later addObject:buffer];
    }
}

@end

// ------------------------------------------------------------------------------------------

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) NSUInteger frameLimit;  // 0: run until the window is closed
@property(nonatomic) BOOL presentDirect;
@end

@implementation AppDelegate {
    NSWindow *_window;
    Renderer *_renderer;
    NSTimer *_timer;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        NSLog(@"no Metal device");
        exit(1);
    }
    NSLog(@"device: %@", device.name);

    const NSRect frame = NSMakeRect(0, 0, 640, 480);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Metal Triangle";

    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;  // a capture reads the drawable back
    layer.drawableSize = CGSizeMake(frame.size.width * 2, frame.size.height * 2);

    NSView *view = [[NSView alloc] initWithFrame:frame];
    view.wantsLayer = YES;
    view.layer = layer;
    _window.contentView = view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    _renderer = [[Renderer alloc] initWithLayer:layer];
    _renderer.presentDirect = self.presentDirect;
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                             repeats:YES
                                               block:^(NSTimer *t) {
        [self->_renderer renderFrame];
        if (self.frameLimit > 0 && self->_renderer.frameCount >= self.frameLimit) {
            NSLog(@"rendered %lu frames", (unsigned long)self->_renderer.frameCount);
            [t invalidate];
            [NSApp terminate:nil];
        }
    }];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    return YES;
}

@end

int main(int argc, const char *argv[]) {
    NSUInteger frameLimit = 0;
    BOOL presentDirect = NO;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frameLimit = (NSUInteger)atoi(argv[++i]);
        else if (strcmp(argv[i], "--present-direct") == 0) presentDirect = YES;
    }
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        delegate.frameLimit = frameLimit;
        delegate.presentDirect = presentDirect;
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
