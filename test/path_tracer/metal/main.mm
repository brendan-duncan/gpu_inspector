// A Metal path tracer: the final scene of "Ray Tracing in One Weekend"
// (https://raytracing.github.io/books/RayTracingInOneWeekend.html) on Metal ray tracing,
// progressively accumulated one frame at a time. The counterpart of test/path_tracer/vulkan and
// test/path_tracer/d3d12, and something to debug ray tracing with:
//
//   - three primitive acceleration structures of bounding boxes (one per material; their scratch
//     at three offsets into one buffer) under an instance acceleration structure of three
//     instances, built by acceleration structure encoders;
//   - a compute kernel that traverses the scene with an intersector and shades each hit itself,
//     since Metal has no hit shaders: the material comes from a table indexed by the instance;
//   - a bounding box intersection function for the spheres, linked into the compute pipeline and
//     reached through an intersection function table with buffers of its own;
//   - a read-write texture that every frame reads and writes (the running mean), so a frame
//     depends on the frames before it; the output is blitted to the drawable.
//
//   mtlinsp_path_tracer [--frames N] [--width W] [--height H] [--spp N] [--depth N]
//                       [--no-accumulate] [--rebuild]
//
//   --frames N        render N frames and exit
//   --width, --height the window's size in points (960x540)
//   --spp N           samples per pixel per frame (1)
//   --depth N         bounces per path (50, the book's)
//   --no-accumulate   every frame stands alone: noisy, but independent of the frames before it
//   --rebuild         rebuild every acceleration structure in every frame, so a captured frame
//                     holds the builds as well as the trace
//
// Needs a GPU with ray tracing (Apple silicon, or a recent AMD GPU). Built unsigned by CMake, so
// DYLD_INSERT_LIBRARIES reaches it. See src/metal/README.md.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../scene.h"

namespace {

// The layouts match test/path_tracer/scene.h: packed_float3 is the 12-byte float3 the other
// APIs have, where MSL's float3 is 16 bytes.
NSString *const kShaderSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;
using namespace raytracing;

struct Sphere {
    packed_float3 center;
    float radius;
    packed_float3 albedo;
    float param;   // metal: fuzz; dielectric: index of refraction
};

struct Camera {
    packed_float3 center;
    float defocusAngle;
    packed_float3 pixel00;
    float pad0;
    packed_float3 pixelDeltaU;
    float pad1;
    packed_float3 pixelDeltaV;
    float pad2;
    packed_float3 defocusDiskU;
    float pad3;
    packed_float3 defocusDiskV;
    float pad4;
};

struct FrameParams {
    uint frame;
    uint samplesPerFrame;
    uint maxDepth;
    uint accumulate;
};

// One per instance, in instance order: where its spheres start and what they are made of.
struct InstanceInfo {
    uint firstSphere;
    uint material;
};

constant uint kLambertian = 0;
constant uint kMetal = 1;   // anything else is dielectric

// PCG (Jarzynski and Olano, "Hash Functions for GPU Rendering").
uint Pcg(thread uint &state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// [0, 1)
float RandomFloat(thread uint &state) {
    return float(Pcg(state) >> 8u) / 16777216.0;
}

float3 RandomUnitVector(thread uint &state) {
    float z = RandomFloat(state) * 2.0 - 1.0;
    float a = RandomFloat(state) * 6.28318530718;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(a), r * sin(a), z);
}

float2 RandomInUnitDisk(thread uint &state) {
    float r = sqrt(RandomFloat(state));
    float a = RandomFloat(state) * 6.28318530718;
    return float2(r * cos(a), r * sin(a));
}

bool NearZero(float3 v) {
    return all(abs(v) < 1e-8);
}

float Reflectance(float cosine, float refractionIndex) {
    float r0 = (1.0 - refractionIndex) / (1.0 + refractionIndex);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
}

// ---------------------------------------------------------------------------- intersection

struct BoundingBoxIntersection {
    bool accept [[accept_intersection]];
    float distance [[distance]];
};

// sphere::hit, for every material's bounding boxes. Accepts the nearer root when it lies in the
// ray's interval and the farther one otherwise (a ray leaving a glass sphere). The normal is
// worked out again by the kernel from the distance.
[[intersection(bounding_box, instancing)]]
BoundingBoxIntersection sphereIntersection(float3 origin [[origin]],
                                           float3 direction [[direction]],
                                           float minDistance [[min_distance]],
                                           float maxDistance [[max_distance]],
                                           uint primitiveIndex [[primitive_id]],
                                           uint instanceIndex [[instance_id]],
                                           const device Sphere *spheres [[buffer(0)]],
                                           const device InstanceInfo *instances [[buffer(1)]]) {
    Sphere s = spheres[instances[instanceIndex].firstSphere + primitiveIndex];
    float3 center = s.center;

    BoundingBoxIntersection result;
    result.accept = false;
    result.distance = 0.0;

    float3 oc = center - origin;
    float a = dot(direction, direction);
    float h = dot(direction, oc);
    float c = dot(oc, oc) - s.radius * s.radius;
    float discriminant = h * h - a * c;
    if (discriminant < 0.0) return result;
    float sqrtd = sqrt(discriminant);

    float root = (h - sqrtd) / a;
    if (root < minDistance || root > maxDistance) {
        root = (h + sqrtd) / a;
        if (root < minDistance || root > maxDistance) return result;
    }
    result.accept = true;
    result.distance = root;
    return result;
}

// ---------------------------------------------------------------------------- the kernel

// camera::get_ray: a ray from the defocus disk through a random point in pixel (i, j).
ray CameraRay(uint2 pixel, constant Camera &camera, thread uint &rng) {
    float2 offset = float2(RandomFloat(rng), RandomFloat(rng)) - 0.5;
    float3 pixelSample = float3(camera.pixel00) + (float(pixel.x) + offset.x) * float3(camera.pixelDeltaU) +
                         (float(pixel.y) + offset.y) * float3(camera.pixelDeltaV);
    float3 origin = camera.center;
    if (camera.defocusAngle > 0.0) {
        float2 p = RandomInUnitDisk(rng);
        origin += p.x * float3(camera.defocusDiskU) + p.y * float3(camera.defocusDiskV);
    }
    ray r;
    r.origin = origin;
    r.direction = pixelSample - origin;
    r.min_distance = 0.001;   // the book's cure for shadow acne
    r.max_distance = 1.0e30;
    return r;
}

// camera::ray_color, with the recursion unrolled into a loop and each material's scatter inline.
float3 RayColor(ray r,
                instance_acceleration_structure scene,
                intersection_function_table<instancing> functions,
                const device Sphere *spheres,
                const device InstanceInfo *instances,
                uint maxDepth,
                thread uint &rng) {
    intersector<instancing> isect;
    isect.assume_geometry_type(geometry_type::bounding_box);
    isect.force_opacity(forced_opacity::opaque);
    isect.accept_any_intersection(false);

    float3 throughput = 1.0;
    for (uint depth = 0; depth < maxDepth; ++depth) {
        intersector<instancing>::result_type hit = isect.intersect(r, scene, 0xFF, functions);
        if (hit.type == intersection_type::none) {
            // ray_color's background: white blended to sky blue by the ray's height.
            float3 unitDirection = normalize(r.direction);
            float a = 0.5 * (unitDirection.y + 1.0);
            return throughput * ((1.0 - a) * float3(1.0) + a * float3(0.5, 0.7, 1.0));
        }

        InstanceInfo info = instances[hit.instance_id];
        Sphere s = spheres[info.firstSphere + hit.primitive_id];
        float3 p = r.origin + hit.distance * r.direction;
        float3 outwardNormal = (p - float3(s.center)) / s.radius;
        bool frontFace = dot(r.direction, outwardNormal) < 0.0;
        float3 normal = frontFace ? outwardNormal : -outwardNormal;

        float3 scattered;
        if (info.material == kLambertian) {
            scattered = normal + RandomUnitVector(rng);
            if (NearZero(scattered)) scattered = normal;
            throughput *= float3(s.albedo);
        } else if (info.material == kMetal) {
            scattered = normalize(reflect(r.direction, normal)) + s.param * RandomUnitVector(rng);
            if (dot(scattered, normal) <= 0.0) return 0.0;   // absorbed
            throughput *= float3(s.albedo);
        } else {   // dielectric
            float ri = frontFace ? 1.0 / s.param : s.param;
            float3 unitDirection = normalize(r.direction);
            float cosTheta = min(dot(-unitDirection, normal), 1.0);
            float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
            bool cannotRefract = ri * sinTheta > 1.0;
            if (cannotRefract || Reflectance(cosTheta, ri) > RandomFloat(rng))
                scattered = reflect(unitDirection, normal);
            else
                scattered = refract(unitDirection, normal, ri);
        }
        r.origin = p;
        r.direction = scattered;
    }
    return 0.0;
}

kernel void pathTrace(uint2 pixel [[thread_position_in_grid]],
                      instance_acceleration_structure scene [[buffer(0)]],
                      intersection_function_table<instancing> functions [[buffer(1)]],
                      const device Sphere *spheres [[buffer(2)]],
                      const device InstanceInfo *instances [[buffer(3)]],
                      constant Camera &camera [[buffer(4)]],
                      constant FrameParams &params [[buffer(5)]],
                      texture2d<float, access::read_write> accumulation [[texture(0)]],
                      texture2d<float, access::write> output [[texture(1)]]) {
    if (pixel.x >= output.get_width() || pixel.y >= output.get_height()) return;
    uint rng = (pixel.x * 1973u + pixel.y * 9277u + params.frame * 26699u) | 1u;

    float3 color = 0.0;
    for (uint s = 0; s < params.samplesPerFrame; ++s) {
        ray r = CameraRay(pixel, camera, rng);
        color += RayColor(r, scene, functions, spheres, instances, params.maxDepth, rng);
    }
    color /= float(params.samplesPerFrame);

    // A running mean over every frame since the last reset: frame n has weight 1 / (n + 1).
    if (params.accumulate != 0 && params.frame > 0) {
        float3 previous = accumulation.read(pixel).rgb;
        color = mix(previous, color, 1.0 / float(params.frame + 1));
    }
    accumulation.write(float4(color, 1.0), pixel);

    // write_color: gamma 2.
    output.write(float4(sqrt(saturate(color)), 1.0), pixel);
}
)MSL";

struct InstanceInfo {
    uint32_t firstSphere;
    uint32_t material;
};

struct Options {
    NSUInteger frameLimit = 0;   // 0: run until the window is closed
    CGFloat width = 960, height = 540;
    uint32_t samplesPerFrame = 1;
    uint32_t maxDepth = 50;
    bool accumulate = true;
    bool rebuild = false;
};

constexpr uint32_t M = rtiow::kMaterialCount;

[[noreturn]] void Fail(NSString *what, NSError *error) {
    NSLog(@"%@: %@", what, error);
    exit(1);
}

}  // namespace

// ------------------------------------------------------------------------------------------

@interface Renderer : NSObject
- (instancetype)initWithLayer:(CAMetalLayer *)layer options:(const Options &)options;
- (void)renderFrame;
@property(nonatomic, readonly) NSUInteger frameCount;
@end

@implementation Renderer {
    Options _options;
    CAMetalLayer *_layer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLComputePipelineState> _pipeline;
    id<MTLIntersectionFunctionTable> _functions;

    rtiow::Scene _scene;
    id<MTLBuffer> _spheres;
    id<MTLBuffer> _instanceInfo;
    id<MTLBuffer> _aabbs[M];
    MTLPrimitiveAccelerationStructureDescriptor *_blasDescriptors[M];
    id<MTLAccelerationStructure> _blas[M];
    NSUInteger _blasScratchOffset[M];
    id<MTLBuffer> _instances;
    MTLInstanceAccelerationStructureDescriptor *_tlasDescriptor;
    id<MTLAccelerationStructure> _tlas;
    id<MTLBuffer> _scratch;

    id<MTLTexture> _accumulation;
    id<MTLTexture> _output;
    rtiow::Camera _camera;
    NSUInteger _frameCount;
    uint32_t _accumulated;   // frames in the running mean
}

- (instancetype)initWithLayer:(CAMetalLayer *)layer options:(const Options &)options {
    if (!(self = [super init])) return nil;
    _options = options;
    _layer = layer;
    _device = layer.device;
    _queue = [_device newCommandQueue];
    _queue.label = @"path tracer queue";

    [self createPipeline];
    [self createScene];
    [self createTextures];
    return self;
}

- (void)createPipeline {
    NSError *error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:kShaderSource options:nil error:&error];
    if (!library) Fail(@"shader compilation failed", error);
    library.label = @"path tracer shaders";

    // The intersection function is linked into the kernel's pipeline, which then hands out the
    // handle the function table holds.
    id<MTLFunction> intersection = [library newFunctionWithName:@"sphereIntersection"];
    MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
    linked.functions = @[ intersection ];
    MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
    descriptor.label = @"path tracer pipeline";
    descriptor.computeFunction = [library newFunctionWithName:@"pathTrace"];
    descriptor.linkedFunctions = linked;
    _pipeline = [_device newComputePipelineStateWithDescriptor:descriptor
                                                       options:MTLPipelineOptionNone
                                                    reflection:nil
                                                         error:&error];
    if (!_pipeline) Fail(@"pipeline creation failed", error);

    MTLIntersectionFunctionTableDescriptor *tableDescriptor = [[MTLIntersectionFunctionTableDescriptor alloc] init];
    tableDescriptor.functionCount = 1;
    _functions = [_pipeline newIntersectionFunctionTableWithDescriptor:tableDescriptor];
    _functions.label = @"sphere intersection table";
    [_functions setFunction:[_pipeline functionHandleWithFunction:intersection] atIndex:0];
}

- (void)createScene {
    _scene = rtiow::MakeScene();
    NSLog(@"scene: %zu spheres (%u Lambertian, %u metal, %u dielectric)", _scene.spheres.size(),
          _scene.count[rtiow::kLambertian], _scene.count[rtiow::kMetal], _scene.count[rtiow::kDielectric]);

    _spheres = [_device newBufferWithBytes:_scene.spheres.data()
                                    length:_scene.spheres.size() * sizeof(rtiow::Sphere)
                                   options:MTLResourceStorageModeShared];
    _spheres.label = @"spheres";
    InstanceInfo info[M];
    for (uint32_t m = 0; m < M; ++m) info[m] = {_scene.first[m], m};
    _instanceInfo = [_device newBufferWithBytes:info length:sizeof(info) options:MTLResourceStorageModeShared];
    _instanceInfo.label = @"instance materials";
    // The intersection function reads both through the table's own bindings.
    [_functions setBuffer:_spheres offset:0 atIndex:0];
    [_functions setBuffer:_instanceInfo offset:0 atIndex:1];

    // One primitive structure per material. They are built in one encoder, so each has its own
    // stretch of the scratch buffer.
    static_assert(sizeof(rtiow::Aabb) == sizeof(MTLAxisAlignedBoundingBox));
    NSUInteger blasScratch = 0;
    NSMutableArray<id<MTLAccelerationStructure>> *instanced = [NSMutableArray array];
    for (uint32_t m = 0; m < M; ++m) {
        std::vector<rtiow::Aabb> boxes = rtiow::Bounds(_scene, (rtiow::Material)m);
        NSString *material = [NSString stringWithUTF8String:rtiow::MaterialName(m)];
        _aabbs[m] = [_device newBufferWithBytes:boxes.data()
                                         length:boxes.size() * sizeof(rtiow::Aabb)
                                        options:MTLResourceStorageModeShared];
        _aabbs[m].label = [material stringByAppendingString:@" AABBs"];

        MTLAccelerationStructureBoundingBoxGeometryDescriptor *geometry =
            [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
        geometry.boundingBoxBuffer = _aabbs[m];
        geometry.boundingBoxBufferOffset = 0;
        geometry.boundingBoxStride = sizeof(MTLAxisAlignedBoundingBox);
        geometry.boundingBoxCount = _scene.count[m];
        geometry.intersectionFunctionTableOffset = 0;
        geometry.opaque = YES;
        _blasDescriptors[m] = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        _blasDescriptors[m].geometryDescriptors = @[ geometry ];

        MTLAccelerationStructureSizes sizes = [_device accelerationStructureSizesWithDescriptor:_blasDescriptors[m]];
        _blas[m] = [_device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        _blas[m].label = [material stringByAppendingString:@" BLAS"];
        [instanced addObject:_blas[m]];
        _blasScratchOffset[m] = blasScratch;
        blasScratch += sizes.buildScratchBufferSize;
    }

    // One instance per material, in material order: the identity transform, the material's
    // structure, and the one intersection function.
    _instances = [_device newBufferWithLength:M * sizeof(MTLAccelerationStructureInstanceDescriptor)
                                      options:MTLResourceStorageModeShared];
    _instances.label = @"scene instances";
    auto *records = (MTLAccelerationStructureInstanceDescriptor *)_instances.contents;
    for (uint32_t m = 0; m < M; ++m) {
        MTLAccelerationStructureInstanceDescriptor &r = records[m];
        memset(&r, 0, sizeof(r));
        for (int i = 0; i < 3; ++i) r.transformationMatrix.columns[i].elements[i] = 1.0f;
        r.options = MTLAccelerationStructureInstanceOptionOpaque;
        r.mask = 0xFF;
        r.intersectionFunctionTableOffset = 0;
        r.accelerationStructureIndex = m;
    }
    _tlasDescriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
    _tlasDescriptor.instancedAccelerationStructures = instanced;
    _tlasDescriptor.instanceCount = M;
    _tlasDescriptor.instanceDescriptorBuffer = _instances;
    _tlasDescriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeDefault;
    MTLAccelerationStructureSizes sizes = [_device accelerationStructureSizesWithDescriptor:_tlasDescriptor];
    _tlas = [_device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
    _tlas.label = @"scene TLAS";

    // The top level is built in an encoder after the bottom levels' and starts at the beginning
    // of the scratch buffer again.
    _scratch = [_device newBufferWithLength:std::max(blasScratch, (NSUInteger)sizes.buildScratchBufferSize)
                                    options:MTLResourceStorageModePrivate];
    _scratch.label = @"build scratch";

    if (!_options.rebuild) {
        id<MTLCommandBuffer> commands = [_queue commandBuffer];
        commands.label = @"build";
        [self encodeBuilds:commands];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.error) Fail(@"building the acceleration structures failed", commands.error);
    }
}

// The three bottom levels in one encoder, then the top level over them in a second: the encoders
// run in order, since the top level reads what the first one writes.
- (void)encodeBuilds:(id<MTLCommandBuffer>)commands {
    id<MTLAccelerationStructureCommandEncoder> bottom = [commands accelerationStructureCommandEncoder];
    bottom.label = @"build bottom levels";
    for (uint32_t m = 0; m < M; ++m) {
        [bottom buildAccelerationStructure:_blas[m]
                                descriptor:_blasDescriptors[m]
                             scratchBuffer:_scratch
                       scratchBufferOffset:_blasScratchOffset[m]];
    }
    [bottom endEncoding];

    id<MTLAccelerationStructureCommandEncoder> top = [commands accelerationStructureCommandEncoder];
    top.label = @"build top level";
    [top buildAccelerationStructure:_tlas descriptor:_tlasDescriptor scratchBuffer:_scratch scratchBufferOffset:0];
    [top endEncoding];
}

// The running mean and the output at the drawable's size.
- (void)createTextures {
    const CGSize size = _layer.drawableSize;
    const NSUInteger width = (NSUInteger)size.width, height = (NSUInteger)size.height;

    MTLTextureDescriptor *accumulation = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                            width:width
                                                                                           height:height
                                                                                        mipmapped:NO];
    accumulation.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    accumulation.storageMode = MTLStorageModePrivate;
    _accumulation = [_device newTextureWithDescriptor:accumulation];
    _accumulation.label = @"path tracer accumulation";

    // The drawable's format, so the blit is a plain copy.
    MTLTextureDescriptor *output = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:_layer.pixelFormat
                                                                                      width:width
                                                                                     height:height
                                                                                  mipmapped:NO];
    output.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    output.storageMode = MTLStorageModePrivate;
    _output = [_device newTextureWithDescriptor:output];
    _output.label = @"path tracer output";

    _camera = rtiow::MakeCamera((uint32_t)width, (uint32_t)height);
    _accumulated = 0;
}

- (void)renderFrame {
    id<CAMetalDrawable> drawable = [_layer nextDrawable];
    if (!drawable) return;

    id<MTLCommandBuffer> commands = [_queue commandBuffer];
    commands.label = @"frame";
    if (_options.rebuild) [self encodeBuilds:commands];

    rtiow::FrameParams params{_options.accumulate ? _accumulated : (uint32_t)_frameCount, _options.samplesPerFrame,
                              _options.maxDepth, _options.accumulate ? 1u : 0u};
    id<MTLComputeCommandEncoder> trace = [commands computeCommandEncoder];
    trace.label = @"path trace";
    [trace setComputePipelineState:_pipeline];
    [trace setAccelerationStructure:_tlas atBufferIndex:0];
    [trace setIntersectionFunctionTable:_functions atBufferIndex:1];
    [trace setBuffer:_spheres offset:0 atIndex:2];
    [trace setBuffer:_instanceInfo offset:0 atIndex:3];
    [trace setBytes:&_camera length:sizeof(_camera) atIndex:4];
    [trace setBytes:&params length:sizeof(params) atIndex:5];
    [trace setTexture:_accumulation atIndex:0];
    [trace setTexture:_output atIndex:1];
    // What the kernel reaches only through the top level: the encoder does not see it otherwise.
    for (uint32_t m = 0; m < M; ++m) [trace useResource:_blas[m] usage:MTLResourceUsageRead];
    const NSUInteger w = _pipeline.threadExecutionWidth;
    const NSUInteger h = std::max<NSUInteger>(1, _pipeline.maxTotalThreadsPerThreadgroup / w);
    [trace dispatchThreads:MTLSizeMake(_output.width, _output.height, 1) threadsPerThreadgroup:MTLSizeMake(w, h, 1)];
    [trace endEncoding];

    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    blit.label = @"present";
    [blit copyFromTexture:_output toTexture:drawable.texture];
    [blit endEncoding];

    [commands presentDrawable:drawable];
    [commands commit];
    _frameCount++;
    _accumulated++;
    if (_frameCount % 100 == 0)
        NSLog(@"frame %lu: %u samples per pixel", (unsigned long)_frameCount,
              (_options.accumulate ? _accumulated : 1) * _options.samplesPerFrame);
}

@end

// ------------------------------------------------------------------------------------------

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic) Options options;
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
    if (!device.supportsRaytracing) {
        NSLog(@"%@ has no ray tracing", device.name);
        exit(1);
    }
    NSLog(@"device: %@", device.name);

    const Options options = self.options;
    const NSRect frame = NSMakeRect(0, 0, options.width, options.height);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"GPU Inspector test: Metal path tracer";

    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;   // UNORM: the kernel applies the book's gamma itself
    layer.framebufferOnly = NO;                     // the blit writes to it, and a capture reads it back
    const CGFloat scale = _window.backingScaleFactor;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(frame.size.width * scale, frame.size.height * scale);

    NSView *view = [[NSView alloc] initWithFrame:frame];
    view.wantsLayer = YES;
    view.layer = layer;
    _window.contentView = view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    _renderer = [[Renderer alloc] initWithLayer:layer options:options];
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                             repeats:YES
                                               block:^(NSTimer *t) {
        [self->_renderer renderFrame];
        if (options.frameLimit > 0 && self->_renderer.frameCount >= options.frameLimit) {
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
    Options options;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) options.frameLimit = (NSUInteger)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) options.width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) options.height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spp") && i + 1 < argc) options.samplesPerFrame = (uint32_t)std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) options.maxDepth = (uint32_t)std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-accumulate")) options.accumulate = false;
        else if (!strcmp(argv[i], "--rebuild")) options.rebuild = true;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 1;
        }
    }
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        delegate.options = options;
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
