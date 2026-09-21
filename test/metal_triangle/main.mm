// A small Metal application, the counterpart of test/triangle: something to point the Metal
// capture library at without needing a Unity build. It exercises the parts of the API the
// library has to intercept — a device, a command queue, buffers in shared and private storage, a
// heap with resources suballocated from it, a library compiled at run time, two render pipelines,
// a sampler, a compute pass, a multisampled render pass through a parallel encoder resolving into
// a texture, a render pass to the drawable that samples it with inline constants bound, and a
// present.
//
//   mtlinsp_triangle              a window, until it is closed
//   mtlinsp_triangle --frames N   render N frames and exit (no interaction needed)
//   mtlinsp_triangle --present-direct
//                                 present through [drawable present] from a scheduled handler,
//                                 the way Unity's macOS player does, instead of through
//                                 [MTLCommandBuffer presentDrawable:]
//   mtlinsp_triangle --compile-hitch
//                                 compile a library and a pipeline inside every frame, so the CPU
//                                 timeline has a stall in it to attribute
//   mtlinsp_triangle --occluded   draw the triangles twice, the second set behind the first, in a
//                                 pass with a depth attachment: every fragment of the second draw
//                                 is rejected, which is what an overdraw measurement counting with
//                                 and without the depth test has to tell apart
//   mtlinsp_triangle --stencil    give the triangle pass a combined depth/stencil attachment and a
//                                 stencil state that always passes and writes 1, so a capture reads
//                                 a stencil target back beside the depth one. Two aspects of one
//                                 texture: a blit can fetch only one at a time, so the library
//                                 copies it twice (src/metal/src/capture.h, PassAspect)
//   mtlinsp_triangle --ray-tracing
//                                 build a triangle bottom level and an instance top level over two
//                                 copies of it, and trace the scene in a compute pass. The
//                                 counterpart of test/triangle --ray-tracing, and the one sample
//                                 with *triangle* geometry in an acceleration structure —
//                                 test/path_tracer/metal is bounding boxes only. Both are rebuilt
//                                 every frame, so a captured frame holds the builds
//   mtlinsp_triangle --static-blas
//                                 with --ray-tracing: build the bottom level once at start-up and
//                                 only rebuild the top level, which is what an engine does. A
//                                 captured frame then holds no build of the bottom level, and what
//                                 is in it can only come from the read-back the capture library
//                                 takes when the capture begins (src/metal/src/raytracing.h,
//                                 ReadBackEarlierStructures)
//
// Built unsigned by CMake, so DYLD_INSERT_LIBRARIES reaches it. See src/metal/README.md.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdlib>
#include <cstring>

namespace {

// Position (x, y) and color (r, g, b) per vertex, in the order the vertex descriptor expects.
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

struct VertexIn  { float2 position [[attribute(0)]]; float3 color [[attribute(1)]]; };
struct VertexOut { float4 position [[position]];     float3 color; };
// `depth` is 0 except under --occluded, which draws the triangle twice at two depths.
struct Uniforms  { float angle; float scale; float depth; };
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
    out.position = float4(p, u.depth, 1.0);
    out.color = in.color;
    return out;
}

// Function constants, the way an engine ships one library of variants: the fragment is
// specialized at newFunctionWithName:constantValues: rather than compiled twice.
constant int kTintMode [[function_constant(0)]];
constant float kTintAmount [[function_constant(1)]];

fragment float4 fragment_main(VertexOut in [[stage_in]]) {
    float3 color = in.color;
    if (is_function_constant_defined(kTintMode) && kTintMode == 1) {
        color = mix(color, float3(1.0, 1.0, 1.0), kTintAmount);
    }
    return float4(color, 1.0);
}

kernel void wave_main(device float *values [[buffer(0)]],
                      constant Uniforms &u [[buffer(1)]],
                      uint i [[thread_position_in_grid]]) {
    values[i] = sin(u.angle + float(i) * 0.05);
}
)MSL";

// --compile-hitch compiles this, with the frame number substituted in, once per frame. The source
// has to differ every time: Metal keeps a compiler cache, and recompiling identical source would
// be answered from it in microseconds, which is the opposite of the stall being staged.
NSString *const kHitchSourceFormat = @R"MSL(
#include <metal_stdlib>
using namespace metal;

kernel void hitch_main(device float *values [[buffer(0)]],
                       uint i [[thread_position_in_grid]]) {
    values[i] = %f + sin(float(i));
}
)MSL";

// --ray-tracing. A library of its own rather than another entry point in kShaderSource: including
// <metal_raytracing> in the main library would make the whole of it fail to compile on a device
// without ray tracing, and would put types the MSL interpreter does not know yet in front of every
// shader-debugger test (src/app/src/renderer/msl/).
//
// The scene is the triangle twice, placed by the two instance transforms, traced from a grid of
// parallel rays down -Z. Each thread writes what it hit: the instance's user index, the triangle's
// barycentrics, and the distance — enough that the output says whether the transforms were read the
// way Metal stores them, since an untransposed transform puts the triangles somewhere else.
NSString *const kRayTracingSource = @R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace metal::raytracing;

struct TraceUniforms { float angle; float scale; float depth; };

kernel void trace_main(texture2d<float, access::write> out [[texture(0)]],
                       instance_acceleration_structure scene [[buffer(0)]],
                       constant TraceUniforms &u [[buffer(1)]],
                       uint2 gid [[thread_position_in_grid]]) {
    const float2 size = float2(out.get_width(), out.get_height());
    if (gid.x >= uint(size.x) || gid.y >= uint(size.y)) return;
    // A ray per pixel, straight down -Z through the plane the triangles lie in.
    const float2 uv = (float2(gid) + 0.5) / size;
    ray r;
    r.origin = float3((uv.x * 2.0 - 1.0) * 2.0, (1.0 - uv.y * 2.0) * 2.0, 2.0);
    r.direction = float3(0.0, 0.0, -1.0);
    r.min_distance = 0.0;
    r.max_distance = 10.0;

    intersector<instancing, triangle_data> isect;
    isect.assume_geometry_type(geometry_type::triangle);
    intersector<instancing, triangle_data>::result_type hit = isect.intersect(r, scene, 0xFF);
    if (hit.type == intersection_type::none) {
        out.write(float4(0.0, 0.0, 0.0, 1.0), gid);
        return;
    }
    const float2 bary = hit.triangle_barycentric_coord;
    out.write(float4(float(hit.instance_id) + 1.0, bary.x, bary.y, hit.distance), gid);
}
)MSL";

struct Uniforms {
    float angle;
    float scale;
    float depth;
};

constexpr NSUInteger kWaveCount = 256;

// --ray-tracing: the triangle as an acceleration structure needs float3 positions of its own — the
// draw's vertices are position (float2) and colour (float3) interleaved at a stride of 20, which is
// not a layout a build can read as a position.
const float kRayVertices[] = {
     0.0f,  0.6f, 0.0f,
    -0.6f, -0.4f, 0.0f,
     0.6f, -0.4f, 0.0f,
};
const uint16_t kRayIndices[] = { 0, 1, 2 };

/** How many instances the top level places the triangle at, and where. */
constexpr NSUInteger kInstanceCount = 2;

/** The traced image, small on purpose: it is read back with every capture. */
constexpr NSUInteger kTraceSize = 64;

// The heap is deliberately far larger than the two resources taken out of it, so that the
// breakdown's "mostly empty" call-out (renderer/metal/metal_memory.ts, HEAP_OCCUPANCY_LOW) has
// something to report as well as the "In heaps" row.
constexpr NSUInteger kHeapSize = 4 * 1024 * 1024;

}  // namespace

// ------------------------------------------------------------------------------------------

@interface Renderer : NSObject
/** `occluded` is an initializer argument rather than a property: it decides the pipeline's depth
 *  attachment format, which is fixed when the pipeline is built. */
- (instancetype)initWithLayer:(CAMetalLayer *)layer occluded:(BOOL)occluded stencil:(BOOL)stencil
                  rayTracing:(BOOL)rayTracing staticBlas:(BOOL)staticBlas;
- (void)renderFrame;
@property(nonatomic, readonly) NSUInteger frameCount;
/** --occluded: the triangle drawn twice, the second behind the first, with a depth test. */
@property(nonatomic, readonly) BOOL occluded;
/** --stencil: a combined depth/stencil attachment, so both aspects are read back. */
@property(nonatomic, readonly) BOOL stencil;
/** --present-direct: present through the drawable, the way Unity's macOS player does. */
@property(nonatomic) BOOL presentDirect;
/** --compile-hitch: build a library and a pipeline inside every frame. */
@property(nonatomic) BOOL compileHitch;
/** --ray-tracing: build a triangle scene and trace it in a compute pass. */
@property(nonatomic, readonly) BOOL rayTracing;
/** --static-blas: build the bottom level once at start-up rather than every frame. */
@property(nonatomic, readonly) BOOL staticBlas;
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
    // A heap and what was suballocated from it: memory the process holds through one reservation
    // rather than a resource at a time, which the memory breakdown counts differently (their bytes
    // are the heap's, not their own).
    id<MTLHeap> _heap;
    id<MTLBuffer> _heapBuffer;
    id<MTLTexture> _heapTexture;
    // --occluded: a depth attachment on the triangle pass and a state that tests and writes it,
    // so the second draw is behind the first and the depth test has something to reject.
    id<MTLTexture> _depthTarget;
    id<MTLDepthStencilState> _depthState;
    // Resources allocated while running rather than at start-up, so that a capture library has
    // something to stream to a UI that is already connected — the snapshot path and the live path
    // are different code and only one of them is exercised by start-up allocations.
    NSMutableArray *_later;
    NSUInteger _frameCount;
    // --ray-tracing: a triangle bottom level, a top level placing it twice, and the kernel that
    // traces them. The scratch is one buffer both builds take their stretch of, the way an engine
    // does rather than allocating per build.
    id<MTLComputePipelineState> _trace;
    id<MTLBuffer> _rayVertices;
    id<MTLBuffer> _rayIndices;
    MTLPrimitiveAccelerationStructureDescriptor *_blasDescriptor;
    id<MTLAccelerationStructure> _blas;
    MTLInstanceAccelerationStructureDescriptor *_tlasDescriptor;
    id<MTLAccelerationStructure> _tlas;
    id<MTLBuffer> _instances;
    id<MTLBuffer> _scratch;
    NSUInteger _tlasScratchOffset;
    id<MTLTexture> _traceTarget;
}

- (instancetype)initWithLayer:(CAMetalLayer *)layer occluded:(BOOL)occluded stencil:(BOOL)stencil
                  rayTracing:(BOOL)rayTracing staticBlas:(BOOL)staticBlas {
    if (!(self = [super init])) return nil;
    _occluded = occluded;
    _stencil = stencil;
    _rayTracing = rayTracing;
    _staticBlas = staticBlas;
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
    // Specialized: kTintMode selects the branch, kTintAmount is what it mixes by. Nothing reads
    // these back out of Metal, so the capture library watches the setters (src/metal/src/function_constants.h).
    MTLFunctionConstantValues *constants = [[MTLFunctionConstantValues alloc] init];
    const int tintMode = 1;
    const float tintAmount = 0.25f;
    [constants setConstantValue:&tintMode type:MTLDataTypeInt atIndex:0];
    [constants setConstantValue:&tintAmount type:MTLDataTypeFloat atIndex:1];
    NSError *functionError = nil;
    pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"
                                                       constantValues:constants
                                                                error:&functionError];
    if (pipelineDescriptor.fragmentFunction == nil) {
        NSLog(@"specializing fragment_main failed: %@", functionError);
        exit(1);
    }
    pipelineDescriptor.vertexDescriptor = vertexDescriptor;
    pipelineDescriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
    // --stencil shares --occluded's single-sampled depth path: a multisampled depth attachment is
    // deliberately not copied for the overdraw measurement, and the stencil read-back wants a
    // target it can actually blit from.
    const BOOL depthPass = occluded || stencil;
    pipelineDescriptor.rasterSampleCount = depthPass ? 1 : 4;
    if (depthPass) {
        pipelineDescriptor.depthAttachmentPixelFormat =
            stencil ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float;
        if (stencil) pipelineDescriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    }
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

    // A heap, the way an engine reserves once and suballocates: the resources made from it never
    // pass through the device, so they reach the library through the heap's own hooks
    // (src/metal/src/hooks_device.mm, H_newBufferWithLength and friends) and each one moves the
    // heap's reported usage.
    MTLHeapDescriptor *heapDescriptor = [[MTLHeapDescriptor alloc] init];
    heapDescriptor.size = kHeapSize;
    heapDescriptor.storageMode = MTLStorageModePrivate;
    _heap = [_device newHeapWithDescriptor:heapDescriptor];
    if (_heap) {
        _heap.label = @"scratch heap";
        _heapBuffer = [_heap newBufferWithLength:64 * 1024 options:MTLResourceStorageModePrivate];
        _heapBuffer.label = @"heap scratch";
        MTLTextureDescriptor *heapTexture =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:128
                                                              height:128
                                                           mipmapped:NO];
        heapTexture.usage = MTLTextureUsageShaderRead;
        heapTexture.storageMode = MTLStorageModePrivate;
        _heapTexture = [_heap newTextureWithDescriptor:heapTexture];
        _heapTexture.label = @"heap lightmap";
    }

    if (self.occluded || self.stencil) {
        // A depth attachment for the triangle pass and a state that tests and writes it: without
        // the write the second draw could not be rejected by the first. Under --stencil the format
        // is combined, so the one texture carries both aspects.
        const MTLPixelFormat depthFormat =
            self.stencil ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatDepth32Float;
        MTLTextureDescriptor *depth =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depthFormat
                                                               width:(NSUInteger)size.width
                                                              height:(NSUInteger)size.height
                                                           mipmapped:NO];
        depth.usage = MTLTextureUsageRenderTarget;
        depth.storageMode = MTLStorageModePrivate;
        _depthTarget = [_device newTextureWithDescriptor:depth];
        _depthTarget.label = self.stencil ? @"triangle depth+stencil" : @"triangle depth";
        MTLDepthStencilDescriptor *depthState = [[MTLDepthStencilDescriptor alloc] init];
        depthState.label = self.stencil ? @"less, writing, stencil 1" : @"less, writing";
        depthState.depthCompareFunction = MTLCompareFunctionLess;
        depthState.depthWriteEnabled = YES;
        if (self.stencil) {
            // Always passes and replaces with the reference (set to 1 on the encoder), so the
            // stencil read-back holds 1 wherever the triangles landed and 0 everywhere else —
            // which is what makes a wrong aspect or a missing store visible rather than plausible.
            MTLStencilDescriptor *always = [[MTLStencilDescriptor alloc] init];
            always.stencilCompareFunction = MTLCompareFunctionAlways;
            always.depthStencilPassOperation = MTLStencilOperationReplace;
            always.writeMask = 0xFF;
            always.readMask = 0xFF;
            depthState.frontFaceStencil = always;
            depthState.backFaceStencil = always;
        }
        _depthState = [_device newDepthStencilStateWithDescriptor:depthState];
    }

    [self setUpRayTracing];

    _later = [NSMutableArray array];
    return self;
}

/**
 * --ray-tracing: the scene, and the kernel that traces it.
 *
 * The bottom level is one triangle; the top level places it twice, translated apart and the second
 * one rotated, so the two are distinguishable in the traced image and the instance transforms are
 * not the identity — an identity transform is its own transpose and would say nothing about whether
 * MTLPackedFloat4x3 was read the right way round.
 */
- (void)setUpRayTracing {
    if (!self.rayTracing) return;
    if (!_device.supportsRaytracing) {
        NSLog(@"--ray-tracing: this device has no ray tracing");
        exit(1);
    }

    NSError *error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:kRayTracingSource options:nil error:&error];
    if (!library) {
        NSLog(@"ray tracing shader compilation failed: %@", error);
        exit(1);
    }
    library.label = @"ray tracing shaders";
    _trace = [_device newComputePipelineStateWithFunction:[library newFunctionWithName:@"trace_main"]
                                                    error:&error];
    if (!_trace) {
        NSLog(@"trace pipeline creation failed: %@", error);
        exit(1);
    }

    // Private storage for the geometry, which is what an engine uses and the case a capture has to
    // blit to read: a shared buffer is only a memcpy away (src/metal/src/capture.h).
    _rayVertices = [_device newBufferWithLength:sizeof(kRayVertices) options:MTLResourceStorageModePrivate];
    _rayVertices.label = @"ray tracing vertices";
    _rayIndices = [_device newBufferWithLength:sizeof(kRayIndices) options:MTLResourceStorageModePrivate];
    _rayIndices.label = @"ray tracing indices";
    {
        id<MTLBuffer> stagingVertices = [_device newBufferWithBytes:kRayVertices length:sizeof(kRayVertices)
                                                           options:MTLResourceStorageModeShared];
        id<MTLBuffer> stagingIndices = [_device newBufferWithBytes:kRayIndices length:sizeof(kRayIndices)
                                                          options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> upload = [_queue commandBuffer];
        upload.label = @"ray tracing upload";
        id<MTLBlitCommandEncoder> blit = [upload blitCommandEncoder];
        [blit copyFromBuffer:stagingVertices sourceOffset:0 toBuffer:_rayVertices destinationOffset:0
                        size:sizeof(kRayVertices)];
        [blit copyFromBuffer:stagingIndices sourceOffset:0 toBuffer:_rayIndices destinationOffset:0
                        size:sizeof(kRayIndices)];
        [blit endEncoding];
        [upload commit];
        [upload waitUntilCompleted];
    }

    MTLAccelerationStructureTriangleGeometryDescriptor *geometry =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    if (@available(macOS 12.0, *)) geometry.label = @"triangle";
    geometry.vertexBuffer = _rayVertices;
    geometry.vertexBufferOffset = 0;
    geometry.vertexStride = sizeof(float) * 3;
    geometry.indexBuffer = _rayIndices;
    geometry.indexBufferOffset = 0;
    geometry.indexType = MTLIndexTypeUInt16;
    geometry.triangleCount = 1;
    geometry.opaque = YES;
    _blasDescriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    _blasDescriptor.geometryDescriptors = @[ geometry ];
    // Refit, because --static-blas builds it once and an engine that builds once asks for this.
    _blasDescriptor.usage = MTLAccelerationStructureUsageRefit;

    MTLAccelerationStructureSizes blasSizes = [_device accelerationStructureSizesWithDescriptor:_blasDescriptor];
    _blas = [_device newAccelerationStructureWithSize:blasSizes.accelerationStructureSize];
    _blas.label = @"triangle BLAS";

    // Two instances: one moved left, one moved right and turned a quarter turn about Z. The
    // transform is an MTLPackedFloat4x3, four columns of three, so columns[3] is the translation.
    _instances = [_device newBufferWithLength:kInstanceCount * sizeof(MTLAccelerationStructureInstanceDescriptor)
                                     options:MTLResourceStorageModeShared];
    _instances.label = @"scene instances";
    auto *records = (MTLAccelerationStructureInstanceDescriptor *)_instances.contents;
    for (NSUInteger n = 0; n < kInstanceCount; ++n) {
        MTLAccelerationStructureInstanceDescriptor &r = records[n];
        memset(&r, 0, sizeof(r));
        if (n == 0) {
            r.transformationMatrix.columns[0] = MTLPackedFloat3(1.0f, 0.0f, 0.0f);
            r.transformationMatrix.columns[1] = MTLPackedFloat3(0.0f, 1.0f, 0.0f);
        } else {
            // A quarter turn about Z: x' = -y, y' = x. Column-major, so this is the transpose of
            // how it reads on paper — which is the whole point of having it here.
            r.transformationMatrix.columns[0] = MTLPackedFloat3(0.0f, 1.0f, 0.0f);
            r.transformationMatrix.columns[1] = MTLPackedFloat3(-1.0f, 0.0f, 0.0f);
        }
        r.transformationMatrix.columns[2] = MTLPackedFloat3(0.0f, 0.0f, 1.0f);
        r.transformationMatrix.columns[3] = MTLPackedFloat3(n == 0 ? -0.8f : 0.8f, 0.0f, 0.0f);
        r.options = MTLAccelerationStructureInstanceOptionOpaque;
        r.mask = n == 0 ? 0xFF : 0x0F;
        r.intersectionFunctionTableOffset = 0;
        r.accelerationStructureIndex = 0;   // both instances are the one bottom level
    }
    _tlasDescriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
    _tlasDescriptor.instancedAccelerationStructures = @[ _blas ];
    _tlasDescriptor.instanceCount = kInstanceCount;
    _tlasDescriptor.instanceDescriptorBuffer = _instances;
    // instanceDescriptorType is left at its default, which *is*
    // MTLAccelerationStructureInstanceDescriptorTypeDefault — setting it would only add a macOS 12
    // availability guard for no change.

    MTLAccelerationStructureSizes tlasSizes = [_device accelerationStructureSizesWithDescriptor:_tlasDescriptor];
    _tlas = [_device newAccelerationStructureWithSize:tlasSizes.accelerationStructureSize];
    _tlas.label = @"scene TLAS";

    // One scratch buffer, a stretch per build, so the two builds in one encoder do not write into
    // each other.
    _tlasScratchOffset = (blasSizes.buildScratchBufferSize + 255) & ~(NSUInteger)255;
    _scratch = [_device newBufferWithLength:_tlasScratchOffset + tlasSizes.buildScratchBufferSize
                                    options:MTLResourceStorageModePrivate];
    _scratch.label = @"build scratch";

    MTLTextureDescriptor *traced = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                     width:kTraceSize
                                                                                    height:kTraceSize
                                                                                 mipmapped:NO];
    traced.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    traced.storageMode = MTLStorageModePrivate;
    _traceTarget = [_device newTextureWithDescriptor:traced];
    _traceTarget.label = @"traced";

    // --static-blas: built once, here, so a frame captured later holds no build of it and what is
    // in it can only come from the capture library's read-back at the start of the capture.
    if (self.staticBlas) {
        id<MTLCommandBuffer> commands = [_queue commandBuffer];
        commands.label = @"build bottom level";
        id<MTLAccelerationStructureCommandEncoder> encoder = [commands accelerationStructureCommandEncoder];
        encoder.label = @"build bottom level";
        [encoder buildAccelerationStructure:_blas descriptor:_blasDescriptor
                             scratchBuffer:_scratch scratchBufferOffset:0];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.error) {
            NSLog(@"building the bottom level failed: %@", commands.error);
            exit(1);
        }
    }
}

/**
 * --ray-tracing: the frame's builds and the trace that reads them.
 *
 * The top level is rebuilt every frame — what an engine does, since its instances move — and the
 * bottom level with it unless --static-blas. Both in one encoder, so the pass holds two builds and
 * the top level reads what the same encoder wrote just before it.
 */
- (void)encodeRayTracing:(id<MTLCommandBuffer>)commandBuffer {
    if (!self.rayTracing) return;
    id<MTLAccelerationStructureCommandEncoder> builds = [commandBuffer accelerationStructureCommandEncoder];
    builds.label = @"build scene";
    if (!self.staticBlas) {
        [builds buildAccelerationStructure:_blas descriptor:_blasDescriptor
                             scratchBuffer:_scratch scratchBufferOffset:0];
    }
    [builds buildAccelerationStructure:_tlas descriptor:_tlasDescriptor
                         scratchBuffer:_scratch scratchBufferOffset:_tlasScratchOffset];
    [builds endEncoding];

    Uniforms uniforms = { .angle = (float)_frameCount * 0.02f, .scale = 0.8f, .depth = 0.0f };
    id<MTLComputeCommandEncoder> trace = [commandBuffer computeCommandEncoder];
    trace.label = @"trace scene";
    [trace setComputePipelineState:_trace];
    [trace setTexture:_traceTarget atIndex:0];
    [trace setAccelerationStructure:_tlas atBufferIndex:0];
    [trace setBytes:&uniforms length:sizeof(uniforms) atIndex:1];
    // The bottom levels a top level references have to be named as used: the traversal reads them,
    // and nothing else in the encoder mentions them.
    [trace useResource:_blas usage:MTLResourceUsageRead];
    [trace dispatchThreads:MTLSizeMake(kTraceSize, kTraceSize, 1)
     threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [trace endEncoding];
}

/**
 * --compile-hitch: builds a pipeline in the middle of the frame, the way an engine does when it
 * meets a material it has not compiled yet. The point is the stall, so the library is compiled
 * from source that has never been seen before and both it and the pipeline are thrown away again:
 * the timeline should show the frame stopping for it (**Where the CPU went**, "Creating
 * pipelines"), and nothing the inspector itself compiles should join it there.
 */
- (void)runCompileHitch {
    if (!self.compileHitch) return;
    NSString *source = [NSString stringWithFormat:kHitchSourceFormat, (double)_frameCount];
    NSError *error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        NSLog(@"hitch compilation failed: %@", error);
        return;
    }
    library.label = @"compile hitch";
    id<MTLComputePipelineState> pipeline =
        [_device newComputePipelineStateWithFunction:[library newFunctionWithName:@"hitch_main"]
                                               error:&error];
    if (!pipeline) NSLog(@"hitch pipeline creation failed: %@", error);
}

- (void)renderFrame {
    [self runCompileHitch];
    id<CAMetalDrawable> drawable = [_layer nextDrawable];
    if (!drawable) return;

    Uniforms uniforms = { .angle = (float)_frameCount * 0.02f, .scale = 0.8f,
                          .depth = self.occluded ? 0.4f : 0.0f };
    memcpy(_uniforms.contents, &uniforms, sizeof(uniforms));

    id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
    commandBuffer.label = @"frame";

    // --ray-tracing: the builds and the trace, before the rest of the frame.
    [self encodeRayTracing:commandBuffer];

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
    trianglePass.colorAttachments[0].loadAction = MTLLoadActionClear;
    trianglePass.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.09, 0.11, 1.0);
    const BOOL depthPass = self.occluded || self.stencil;
    if (depthPass) {
        // Single-sampled under --occluded: a multisampled pass's depth is deliberately not copied
        // for the overdraw measurement (src/metal/src/overdraw.mm), which is the one thing this
        // mode exists to exercise, so it renders straight into the resolve target instead.
        trianglePass.colorAttachments[0].texture = _resolved;
        trianglePass.colorAttachments[0].storeAction = MTLStoreActionStore;
    } else {
        trianglePass.colorAttachments[0].texture = _msaaTarget;
        trianglePass.colorAttachments[0].resolveTexture = _resolved;
        trianglePass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    }
    if (depthPass) {
        trianglePass.depthAttachment.texture = _depthTarget;
        trianglePass.depthAttachment.loadAction = MTLLoadActionClear;
        trianglePass.depthAttachment.clearDepth = 1.0;
        trianglePass.depthAttachment.storeAction = MTLStoreActionDontCare;
    }
    if (self.stencil) {
        // The same texture as the depth attachment, which is how a combined format is bound, and
        // DontCare on purpose: an application that only tests stencil sets exactly this, and the
        // capture library has to force the store for its read-back (ForceStore in
        // src/metal/src/hooks_command_buffer.mm). Leaving it Store here would hide that.
        trianglePass.stencilAttachment.texture = _depthTarget;
        trianglePass.stencilAttachment.loadAction = MTLLoadActionClear;
        trianglePass.stencilAttachment.clearStencil = 0;
        trianglePass.stencilAttachment.storeAction = MTLStoreActionDontCare;
    }

    id<MTLParallelRenderCommandEncoder> parallel =
        [commandBuffer parallelRenderCommandEncoderWithDescriptor:trianglePass];
    parallel.label = @"triangle (parallel)";
    id<MTLRenderCommandEncoder> encoder = [parallel renderCommandEncoder];
    encoder.label = @"triangle";
    [encoder pushDebugGroup:@"triangles"];
    [encoder setRenderPipelineState:_pipeline];
    if (depthPass) [encoder setDepthStencilState:_depthState];
    if (self.stencil) [encoder setStencilReferenceValue:1];
    [encoder setVertexBuffer:_verticesPrivate offset:0 atIndex:0];
    [encoder setVertexBuffer:_uniforms offset:0 atIndex:1];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:sizeof(kIndices) / sizeof(kIndices[0])
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:_indices
                 indexBufferOffset:0
                     instanceCount:3];
    if (self.occluded) {
        // The same triangles again, behind the ones just drawn: every fragment is rasterized and
        // every one of them fails the depth test, so a measurement that counts fragments with the
        // pass's depth test must come out half of the one that counts without it.
        Uniforms behind = uniforms;
        behind.depth = 0.8f;
        [encoder setVertexBytes:&behind length:sizeof(behind) atIndex:1];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:sizeof(kIndices) / sizeof(kIndices[0])
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:_indices
                     indexBufferOffset:0
                         instanceCount:3];
    }
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
@property(nonatomic) BOOL compileHitch;
@property(nonatomic) BOOL occluded;
@property(nonatomic) BOOL rayTracing;
@property(nonatomic) BOOL staticBlas;
@property(nonatomic) BOOL stencilPass;
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

    _renderer = [[Renderer alloc] initWithLayer:layer occluded:self.occluded stencil:self.stencilPass
                                     rayTracing:self.rayTracing staticBlas:self.staticBlas];
    _renderer.presentDirect = self.presentDirect;
    _renderer.compileHitch = self.compileHitch;
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
    BOOL compileHitch = NO;
    BOOL occluded = NO;
    BOOL rayTracing = NO;
    BOOL staticBlas = NO;
    BOOL stencil = NO;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frameLimit = (NSUInteger)atoi(argv[++i]);
        else if (strcmp(argv[i], "--present-direct") == 0) presentDirect = YES;
        else if (strcmp(argv[i], "--compile-hitch") == 0) compileHitch = YES;
        else if (strcmp(argv[i], "--occluded") == 0) occluded = YES;
        else if (strcmp(argv[i], "--stencil") == 0) stencil = YES;
        else if (strcmp(argv[i], "--ray-tracing") == 0) rayTracing = YES;
        else if (strcmp(argv[i], "--static-blas") == 0) staticBlas = YES;
    }
    // --static-blas only means anything with a scene to build.
    if (staticBlas) rayTracing = YES;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        delegate.frameLimit = frameLimit;
        delegate.presentDirect = presentDirect;
        delegate.compileHitch = compileHitch;
        delegate.occluded = occluded;
        delegate.stencilPass = stencil;
        delegate.rayTracing = rayTracing;
        delegate.staticBlas = staticBlas;
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
