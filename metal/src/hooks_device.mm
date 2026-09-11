// Hooks on the device and the objects it creates: CAMetalLayer, drawables, MTLDevice, MTLHeap,
// MTLLibrary, MTLTexture and MTLBuffer. See hooks_common.h for the shape every hook has.
#include "hooks.h"
#include "hooks_common.h"
#include "frame_stats.h"
#include "overdraw.h"

#import <QuartzCore/CAMetalLayer.h>

#include <cstring>

namespace mtlinsp {

uint64_t Track(id object, const char *type, const char *cmd, id parent, const std::string &args) {
    if (object == nil) return 0;
    const uint64_t id = TrackObject(object, type, cmd, parent, args);
    if (id == 0) return 0;
    Class cls = object_getClass(object);
    if (cls != nil && [object respondsToSelector:@selector(setLabel:)]) {
        Hook(cls, @selector(setLabel:), (IMP)Replaced_setLabel);
    }
    return id;
}

void Replaced_setLabel(id self, SEL _cmd, NSString *label) {
    Reentry reentry(self, _cmd);
    // Forward first: TrackLabel reads the label back off the object, so it has to be set.
    ORIG(void (*)(id, SEL, NSString *))(self, _cmd, label);
    if (!reentry.outermost()) return;
    TrackLabel(self);
    // An encoder's or command buffer's label is part of the command stream — it is how the UI
    // names the pass — where a resource's is a property of the object.
    if (Recording() && IsCommandStreamObject(self)) {
        RecordCommand("setLabel:", self, Args().s("label", label).str());
    }
}

namespace {

// --------------------------------------------------------------------------------------------
// CAMetalLayer
//
// A drawable's texture is the one a frame actually renders to, and it arrives from
// `CAMetalLayer.nextDrawable` rather than from any of the device's `new*` calls, so nothing else
// here would ever see it. Without it a render pass's colour attachment resolves to null.
// CAMetalLayer is a public QuartzCore class, so unlike everything else in this file it can be
// hooked by name, at load, before the application has a layer.

id Replaced_nextDrawable(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    CAMetalLayer *layer = (CAMetalLayer *)self;
    const BOOL wasFramebufferOnly = layer.framebufferOnly;
    if (reentry.outermost()) {
        // Every device that draws to a window passes through here, whichever entry point handed
        // it out — a layer's preferredDevice and MetalKit's default are not interposed. This is
        // where RenderDoc registers the layer's device too.
        TrackDeviceObject(layer.device, "CAMetalLayer.device");
        // A framebufferOnly layer's drawable texture cannot be a blit source, and the flag has
        // to be off before the drawable is made. This is the Metal counterpart of the layer
        // adding TRANSFER_SRC usage to every image it sees: a small cost paid always, so a
        // capture can be taken at any moment without the application having been told to
        // expect one.
        if (wasFramebufferOnly) layer.framebufferOnly = NO;
    }
    id drawable = ORIG(id (*)(id, SEL))(self, _cmd);
    if (reentry.outermost() && drawable != nil) {
        HookDrawableClass(drawable);
        // Whether presents run in step with the display decides whether a refresh rate is
        // reported at all: with sync off the frame interval says nothing about the display.
        NoteDisplaySync(layer.displaySyncEnabled);
        id<CAMetalDrawable> metalDrawable = (id<CAMetalDrawable>)drawable;
        id<MTLTexture> texture = metalDrawable.texture;
        if (texture != nil) {
            if (IdOf(texture) == 0) {
                // The application's value, not the one it was just replaced with.
                Args a;
                a.e("pixelFormat", PixelFormatEnumName(texture.pixelFormat), (uint64_t)texture.pixelFormat)
                 .e("textureType", TextureTypeEnumName(texture.textureType), (uint64_t)texture.textureType)
                 .u("width", texture.width).u("height", texture.height)
                 .u("mipmapLevelCount", texture.mipmapLevelCount).u("arrayLength", texture.arrayLength)
                 .u("usage", (uint64_t)texture.usage)
                 .b("framebufferOnly", wasFramebufferOnly)
                 .b("drawable", true);
                WriteGpuIds(a, texture);
                WriteMemoryInfo(a, texture);
                // A layer cycles a small pool of drawables, so this registers each of them once.
                Track(texture, "MTLTexture", "CAMetalLayer nextDrawable", layer.device, a.str());
                Log("nextDrawable -> texture %s %lux%lu", ClassName(texture),
                    (unsigned long)texture.width, (unsigned long)texture.height);
            }
            HookTextureClass(texture);
            OnDrawableAcquired(drawable, texture);
        }
    }
    return drawable;
}

// The drawable's own present, which is a frame boundary in its own right: an engine may call it
// directly instead of going through the command buffer (Unity's macOS player does).
void LogDrawableFrame(id drawable, const char *how) {
    Log("--- frame %llu: %u encoders, %u draws, %u dispatches (%s presents itself, %s) ---",
        (unsigned long long)g_frame++, g_encodersThisFrame.exchange(0),
        g_drawsThisFrame.exchange(0), g_dispatchesThisFrame.exchange(0), ClassName(drawable), how);
}

/** Installed on the capture side, so a frame that ends at a commit still reports itself. */
void LogCommitFrame(id commandBuffer) {
    Log("--- frame %llu: %u encoders, %u draws, %u dispatches (%s commits, drawable presented "
        "later) ---", (unsigned long long)g_frame++, g_encodersThisFrame.exchange(0),
        g_drawsThisFrame.exchange(0), g_dispatchesThisFrame.exchange(0), ClassName(commandBuffer));
}

void Replaced_drawablePresent(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    ORIG(void (*)(id, SEL))(self, _cmd);
    if (reentry.outermost() && OnDrawablePresent(self)) LogDrawableFrame(self, "present");
}

void Replaced_drawablePresentAtTime(id self, SEL _cmd, CFTimeInterval time) {
    Reentry reentry(self, _cmd);
    ORIG(void (*)(id, SEL, CFTimeInterval))(self, _cmd, time);
    if (reentry.outermost() && OnDrawablePresent(self)) LogDrawableFrame(self, "presentAtTime:");
}

void Replaced_drawablePresentAfterMinimumDuration(id self, SEL _cmd, CFTimeInterval duration) {
    Reentry reentry(self, _cmd);
    ORIG(void (*)(id, SEL, CFTimeInterval))(self, _cmd, duration);
    if (reentry.outermost() && OnDrawablePresent(self)) {
        LogDrawableFrame(self, "presentAfterMinimumDuration:");
    }
}

// --------------------------------------------------------------------------------------------
// MTLDevice: queues, resources, state objects

id D_newCommandQueue(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id queue = ORIG(id (*)(id, SEL))(self, _cmd);
    if (reentry.outermost()) {
        Log("device.newCommandQueue -> %s", ClassName(queue));
        Track(queue, "MTLCommandQueue", "newCommandQueue", self, {});
    }
    HookCommandQueueClass(queue);
    return queue;
}

id D_newCommandQueueWithMaxCommandBufferCount(id self, SEL _cmd, NSUInteger count) {
    Reentry reentry(self, _cmd);
    id queue = ORIG(id (*)(id, SEL, NSUInteger))(self, _cmd, count);
    if (reentry.outermost()) {
        Log("device.newCommandQueueWithMaxCommandBufferCount:%lu -> %s", (unsigned long)count,
            ClassName(queue));
        Track(queue, "MTLCommandQueue", "newCommandQueueWithMaxCommandBufferCount:", self,
              Args().u("maxCommandBufferCount", count).str());
    }
    HookCommandQueueClass(queue);
    return queue;
}

id D_newBufferWithLength(id self, SEL _cmd, NSUInteger length, MTLResourceOptions options) {
    Reentry reentry(self, _cmd);
    id buffer = ORIG(id (*)(id, SEL, NSUInteger, MTLResourceOptions))(self, _cmd, length, options);
    if (reentry.outermost()) {
        Track(buffer, "MTLBuffer", "newBufferWithLength:options:", self, BufferArgs(buffer, length, options));
    }
    HookBufferClass(buffer);
    return buffer;
}

id D_newBufferWithBytes(id self, SEL _cmd, const void *bytes, NSUInteger length,
                        MTLResourceOptions options) {
    Reentry reentry(self, _cmd);
    id buffer = ORIG(id (*)(id, SEL, const void *, NSUInteger, MTLResourceOptions))(
        self, _cmd, bytes, length, options);
    if (reentry.outermost()) {
        Track(buffer, "MTLBuffer", "newBufferWithBytes:length:options:", self,
              BufferArgs(buffer, length, options));
    }
    HookBufferClass(buffer);
    return buffer;
}

id D_newBufferWithBytesNoCopy(id self, SEL _cmd, void *pointer, NSUInteger length,
                              MTLResourceOptions options, id deallocator) {
    Reentry reentry(self, _cmd);
    id buffer = ORIG(id (*)(id, SEL, void *, NSUInteger, MTLResourceOptions, id))(
        self, _cmd, pointer, length, options, deallocator);
    if (reentry.outermost()) {
        Track(buffer, "MTLBuffer", "newBufferWithBytesNoCopy:length:options:deallocator:", self,
              BufferArgs(buffer, length, options));
    }
    HookBufferClass(buffer);
    return buffer;
}

id D_newTextureWithDescriptor(id self, SEL _cmd, MTLTextureDescriptor *descriptor) {
    Reentry reentry(self, _cmd);
    id texture = ORIG(id (*)(id, SEL, MTLTextureDescriptor *))(self, _cmd, descriptor);
    if (reentry.outermost()) {
        Log("device.newTextureWithDescriptor: %lux%lu fmt=%lu -> %s",
            (unsigned long)descriptor.width, (unsigned long)descriptor.height,
            (unsigned long)descriptor.pixelFormat, ClassName(texture));
        Track(texture, "MTLTexture", "newTextureWithDescriptor:", self,
              TextureDescriptorArgs(descriptor, texture));
    }
    HookTextureClass(texture);
    return texture;
}

id D_newTextureWithDescriptorIOSurface(id self, SEL _cmd, MTLTextureDescriptor *descriptor,
                                       IOSurfaceRef surface, NSUInteger plane) {
    Reentry reentry(self, _cmd);
    id texture = ORIG(id (*)(id, SEL, MTLTextureDescriptor *, IOSurfaceRef, NSUInteger))(
        self, _cmd, descriptor, surface, plane);
    if (reentry.outermost()) {
        Track(texture, "MTLTexture", "newTextureWithDescriptor:iosurface:plane:", self,
              TextureDescriptorArgs(descriptor, texture));
    }
    HookTextureClass(texture);
    return texture;
}

id D_newSharedTextureWithDescriptor(id self, SEL _cmd, MTLTextureDescriptor *descriptor) {
    Reentry reentry(self, _cmd);
    id texture = ORIG(id (*)(id, SEL, MTLTextureDescriptor *))(self, _cmd, descriptor);
    if (reentry.outermost()) {
        Track(texture, "MTLTexture", "newSharedTextureWithDescriptor:", self,
              TextureDescriptorArgs(descriptor, texture));
    }
    HookTextureClass(texture);
    return texture;
}

id D_newSharedTextureWithHandle(id self, SEL _cmd, id handle) {
    Reentry reentry(self, _cmd);
    id texture = ORIG(id (*)(id, SEL, id))(self, _cmd, handle);
    if (reentry.outermost() && texture != nil) {
        Track(texture, "MTLTexture", "newSharedTextureWithHandle:", self,
              TextureObjectArgs((id<MTLTexture>)texture));
    }
    HookTextureClass(texture);
    return texture;
}

id D_newSamplerStateWithDescriptor(id self, SEL _cmd, MTLSamplerDescriptor *descriptor) {
    Reentry reentry(self, _cmd);
    id sampler = ORIG(id (*)(id, SEL, MTLSamplerDescriptor *))(self, _cmd, descriptor);
    if (reentry.outermost()) {
        Track(sampler, "MTLSamplerState", "newSamplerStateWithDescriptor:", self,
              SamplerArgs(descriptor, sampler));
    }
    return sampler;
}

id D_newDepthStencilStateWithDescriptor(id self, SEL _cmd, MTLDepthStencilDescriptor *descriptor) {
    Reentry reentry(self, _cmd);
    id state = ORIG(id (*)(id, SEL, MTLDepthStencilDescriptor *))(self, _cmd, descriptor);
    if (reentry.outermost()) {
        Track(state, "MTLDepthStencilState", "newDepthStencilStateWithDescriptor:", self,
              DepthStencilArgs(descriptor));
        RememberDepthStencilState(state, descriptor);
    }
    return state;
}

id D_newHeapWithDescriptor(id self, SEL _cmd, MTLHeapDescriptor *descriptor) {
    Reentry reentry(self, _cmd);
    id heap = ORIG(id (*)(id, SEL, MTLHeapDescriptor *))(self, _cmd, descriptor);
    if (reentry.outermost()) {
        Log("device.newHeapWithDescriptor: %lu bytes -> %s", (unsigned long)descriptor.size,
            ClassName(heap));
        Track(heap, "MTLHeap", "newHeapWithDescriptor:", self, HeapArgs(descriptor, heap));
    }
    HookHeapClass(heap);
    return heap;
}

id D_newFence(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id fence = ORIG(id (*)(id, SEL))(self, _cmd);
    if (reentry.outermost()) Track(fence, "MTLFence", "newFence", self, {});
    return fence;
}

id D_newEvent(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id event = ORIG(id (*)(id, SEL))(self, _cmd);
    if (reentry.outermost()) Track(event, "MTLEvent", "newEvent", self, {});
    return event;
}

id D_newSharedEvent(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id event = ORIG(id (*)(id, SEL))(self, _cmd);
    if (reentry.outermost()) Track(event, "MTLSharedEvent", "newSharedEvent", self, {});
    return event;
}

id D_newSharedEventWithHandle(id self, SEL _cmd, id handle) {
    Reentry reentry(self, _cmd);
    id event = ORIG(id (*)(id, SEL, id))(self, _cmd, handle);
    if (reentry.outermost()) Track(event, "MTLSharedEvent", "newSharedEventWithHandle:", self, {});
    return event;
}

id D_newArgumentEncoderWithArguments(id self, SEL _cmd, NSArray *arguments) {
    Reentry reentry(self, _cmd);
    id encoder = ORIG(id (*)(id, SEL, NSArray *))(self, _cmd, arguments);
    if (reentry.outermost() && encoder != nil) {
        id<MTLArgumentEncoder> e = (id<MTLArgumentEncoder>)encoder;
        Args a;
        a.u("argumentCount", arguments.count).u("encodedLength", e.encodedLength)
         .u("alignment", e.alignment);
        Track(encoder, "MTLArgumentEncoder", "newArgumentEncoderWithArguments:", self, a.str());
    }
    return encoder;
}

id D_newIndirectCommandBuffer(id self, SEL _cmd, MTLIndirectCommandBufferDescriptor *descriptor,
                              NSUInteger maxCount, MTLResourceOptions options) {
    Reentry reentry(self, _cmd);
    id icb = ORIG(id (*)(id, SEL, MTLIndirectCommandBufferDescriptor *, NSUInteger, MTLResourceOptions))(
        self, _cmd, descriptor, maxCount, options);
    if (reentry.outermost()) {
        Args a;
        a.u("commandTypes", (uint64_t)descriptor.commandTypes)
         .b("inheritPipelineState", descriptor.inheritPipelineState)
         .b("inheritBuffers", descriptor.inheritBuffers)
         .u("maxVertexBufferBindCount", descriptor.maxVertexBufferBindCount)
         .u("maxFragmentBufferBindCount", descriptor.maxFragmentBufferBindCount)
         .u("maxCommandCount", maxCount).u("options", (uint64_t)options);
        Track(icb, "MTLIndirectCommandBuffer",
              "newIndirectCommandBufferWithDescriptor:maxCommandCount:options:", self, a.str());
    }
    return icb;
}

// --------------------------------------------------------------------------------------------
// MTLDevice: libraries

id D_newLibraryWithData(id self, SEL _cmd, dispatch_data_t data, NSError **error) {
    Reentry reentry(self, _cmd);
    id library = ORIG(id (*)(id, SEL, dispatch_data_t, NSError **))(self, _cmd, data, error);
    if (reentry.outermost()) {
        Log("device.newLibraryWithData: -> %s", ClassName(library));
        Track(library, "MTLLibrary", "newLibraryWithData:error:", self,
              LibraryArgs((id<MTLLibrary>)library, "metallib", 0));
        // The AIR bitcode itself. Nothing here disassembles it — that needs Apple's tooling —
        // but the bytes are what a report or a bug attachment needs, and functionNames above
        // already says what is inside.
        if (library != nil && data != nil) {
            const void *bytes = nullptr;
            size_t size = 0;
            dispatch_data_t contiguous = dispatch_data_create_map(data, &bytes, &size);
            if (bytes != nullptr && size != 0) AddBlob(library, "metallib", bytes, size);
            if (contiguous != nil) dispatch_release(contiguous);
        }
    }
    HookLibraryClass(library);
    return library;
}

id D_newLibraryWithURL(id self, SEL _cmd, NSURL *url, NSError **error) {
    Reentry reentry(self, _cmd);
    id library = ORIG(id (*)(id, SEL, NSURL *, NSError **))(self, _cmd, url, error);
    if (reentry.outermost()) {
        Log("device.newLibraryWithURL: %s -> %s", url.path.UTF8String, ClassName(library));
        Track(library, "MTLLibrary", "newLibraryWithURL:error:", self,
              LibraryArgs((id<MTLLibrary>)library, url.path.UTF8String, 0));
    }
    HookLibraryClass(library);
    return library;
}

id D_newLibraryWithFile(id self, SEL _cmd, NSString *path, NSError **error) {
    Reentry reentry(self, _cmd);
    id library = ORIG(id (*)(id, SEL, NSString *, NSError **))(self, _cmd, path, error);
    if (reentry.outermost()) {
        Track(library, "MTLLibrary", "newLibraryWithFile:error:", self,
              LibraryArgs((id<MTLLibrary>)library, path == nil ? "" : path.UTF8String, 0));
    }
    HookLibraryClass(library);
    return library;
}

id D_newDefaultLibrary(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id library = ORIG(id (*)(id, SEL))(self, _cmd);
    if (reentry.outermost()) {
        Log("device.newDefaultLibrary -> %s", ClassName(library));
        Track(library, "MTLLibrary", "newDefaultLibrary", self,
              LibraryArgs((id<MTLLibrary>)library, "default.metallib", 0));
    }
    HookLibraryClass(library);
    return library;
}

id D_newDefaultLibraryWithBundle(id self, SEL _cmd, NSBundle *bundle, NSError **error) {
    Reentry reentry(self, _cmd);
    id library = ORIG(id (*)(id, SEL, NSBundle *, NSError **))(self, _cmd, bundle, error);
    if (reentry.outermost()) {
        Track(library, "MTLLibrary", "newDefaultLibraryWithBundle:error:", self,
              LibraryArgs((id<MTLLibrary>)library,
                          bundle.bundlePath == nil ? "bundle" : bundle.bundlePath.UTF8String, 0));
    }
    HookLibraryClass(library);
    return library;
}

id D_newLibraryWithStitchedDescriptor(id self, SEL _cmd, id descriptor, NSError **error) {
    Reentry reentry(self, _cmd);
    id library = ORIG(id (*)(id, SEL, id, NSError **))(self, _cmd, descriptor, error);
    if (reentry.outermost()) {
        Track(library, "MTLLibrary", "newLibraryWithStitchedDescriptor:error:", self,
              LibraryArgs((id<MTLLibrary>)library, "stitched", 0));
    }
    HookLibraryClass(library);
    return library;
}

void TrackSourceLibrary(id device, id library, NSString *source, const char *cmd) {
    Track(library, "MTLLibrary", cmd, device,
          LibraryArgs((id<MTLLibrary>)library, "source", source.length));
    // The Metal Shading Language the application compiled, verbatim.
    const char *utf8 = source.UTF8String;
    if (utf8 != nullptr) AddBlob(library, "Metal Shading Language", utf8, strlen(utf8));
    HookLibraryClass(library);
}

id D_newLibraryWithSource(id self, SEL _cmd, NSString *source, MTLCompileOptions *options,
                          NSError **error) {
    Reentry reentry(self, _cmd);
    id library = ORIG(id (*)(id, SEL, NSString *, MTLCompileOptions *, NSError **))(
        self, _cmd, source, options, error);
    if (reentry.outermost()) {
        Log("device.newLibraryWithSource: %lu chars -> %s", (unsigned long)source.length,
            ClassName(library));
        TrackSourceLibrary(self, library, source, "newLibraryWithSource:options:error:");
    } else {
        HookLibraryClass(library);
    }
    return library;
}

void D_newLibraryWithSourceAsync(id self, SEL _cmd, NSString *source, MTLCompileOptions *options,
                                 MTLNewLibraryCompletionHandler handler) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost() && handler != nil) {
        // The application's handler, wrapped: the library exists only once it runs. Captured
        // block references are copied along with this block when Metal copies it.
        NSString *keptSource = [source copy];
        MTLNewLibraryCompletionHandler wrapped = ^(id<MTLLibrary> library, NSError *error) {
            TrackSourceLibrary(self, library, keptSource,
                               "newLibraryWithSource:options:completionHandler:");
            [keptSource release];
            handler(library, error);
        };
        ORIG(void (*)(id, SEL, NSString *, MTLCompileOptions *, MTLNewLibraryCompletionHandler))(
            self, _cmd, source, options, wrapped);
        return;
    }
    ORIG(void (*)(id, SEL, NSString *, MTLCompileOptions *, MTLNewLibraryCompletionHandler))(
        self, _cmd, source, options, handler);
}

// --------------------------------------------------------------------------------------------
// MTLDevice: pipelines
//
// Engines rarely use the one-argument forms a sample does: Unity asks for reflection with the
// pipeline, and builds many asynchronously. Every spelling has to land in the tracker or
// setRenderPipelineState: resolves to null in the capture.
//
// Every spelling also asks Metal for reflection (reflection.h), whether the application did or
// not: the forms without an options argument are redirected to the form with one, through the
// hook for that selector, which is nested and so only forwards; the forms with one get the
// reflection options added and, when the application passed no reflection out-parameter, one of
// the library's own. The application sees exactly what it asked for.

id D_newRenderPipelineState(id self, SEL _cmd, MTLRenderPipelineDescriptor *descriptor,
                            NSError **error) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost()) {
        return ORIG(id (*)(id, SEL, MTLRenderPipelineDescriptor *, NSError **))(
            self, _cmd, descriptor, error);
    }
    MTLRenderPipelineReflection *reflection = nil;
    id state = [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:descriptor
                                                                 options:kReflectionOptions
                                                              reflection:&reflection
                                                                   error:error];
    Log("device.newRenderPipelineStateWithDescriptor: label=\"%s\" -> %s",
        descriptor.label == nil ? "" : descriptor.label.UTF8String, ClassName(state));
    Track(state, "MTLRenderPipelineState", "newRenderPipelineStateWithDescriptor:error:", self,
          RenderPipelineArgs(descriptor, reflection));
    RememberRenderPipeline(state, descriptor);
    return state;
}

id D_newRenderPipelineStateReflection(id self, SEL _cmd, MTLRenderPipelineDescriptor *descriptor,
                                      MTLPipelineOption options,
                                      MTLRenderPipelineReflection **reflection, NSError **error) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost()) {
        return ORIG(id (*)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption,
                           MTLRenderPipelineReflection **, NSError **))(
            self, _cmd, descriptor, options, reflection, error);
    }
    MTLRenderPipelineReflection *local = nil;
    MTLRenderPipelineReflection **out = reflection != nullptr ? reflection : &local;
    id state = ORIG(id (*)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption,
                           MTLRenderPipelineReflection **, NSError **))(
        self, _cmd, descriptor, options | kReflectionOptions, out, error);
    Track(state, "MTLRenderPipelineState",
          "newRenderPipelineStateWithDescriptor:options:reflection:error:", self,
          RenderPipelineArgs(descriptor, *out));
    RememberRenderPipeline(state, descriptor);
    return state;
}

void D_newRenderPipelineStateAsync(id self, SEL _cmd, MTLRenderPipelineDescriptor *descriptor,
                                   MTLNewRenderPipelineStateCompletionHandler handler) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost() || handler == nil) {
        ORIG(void (*)(id, SEL, MTLRenderPipelineDescriptor *, MTLNewRenderPipelineStateCompletionHandler))(
            self, _cmd, descriptor, handler);
        return;
    }
    // The application's handler, wrapped: the pipeline exists only once it runs, and the
    // descriptor is copied because the application may change its own the moment this returns.
    MTLRenderPipelineDescriptor *kept = [descriptor copy];
    [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:descriptor
                                                      options:kReflectionOptions
                                            completionHandler:^(id<MTLRenderPipelineState> state,
                                                                MTLRenderPipelineReflection *reflection,
                                                                NSError *error) {
        Track(state, "MTLRenderPipelineState",
              "newRenderPipelineStateWithDescriptor:completionHandler:", self,
              RenderPipelineArgs(kept, reflection));
        RememberRenderPipeline(state, kept);
        [kept release];
        handler(state, error);
    }];
}

void D_newRenderPipelineStateOptionsAsync(id self, SEL _cmd, MTLRenderPipelineDescriptor *descriptor,
                                          MTLPipelineOption options,
                                          MTLNewRenderPipelineStateWithReflectionCompletionHandler handler) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost() || handler == nil) {
        ORIG(void (*)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption,
                      MTLNewRenderPipelineStateWithReflectionCompletionHandler))(
            self, _cmd, descriptor, options, handler);
        return;
    }
    MTLRenderPipelineDescriptor *kept = [descriptor copy];
    MTLNewRenderPipelineStateWithReflectionCompletionHandler wrapped =
        ^(id<MTLRenderPipelineState> state, MTLRenderPipelineReflection *reflection, NSError *error) {
            Track(state, "MTLRenderPipelineState",
                  "newRenderPipelineStateWithDescriptor:options:completionHandler:", self,
                  RenderPipelineArgs(kept, reflection));
            RememberRenderPipeline(state, kept);
            [kept release];
            handler(state, reflection, error);
        };
    ORIG(void (*)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption,
                  MTLNewRenderPipelineStateWithReflectionCompletionHandler))(
        self, _cmd, descriptor, options | kReflectionOptions, wrapped);
}

id D_newTileRenderPipelineState(id self, SEL _cmd, MTLTileRenderPipelineDescriptor *descriptor,
                                MTLPipelineOption options, MTLRenderPipelineReflection **reflection,
                                NSError **error) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost()) {
        return ORIG(id (*)(id, SEL, MTLTileRenderPipelineDescriptor *, MTLPipelineOption,
                           MTLRenderPipelineReflection **, NSError **))(
            self, _cmd, descriptor, options, reflection, error);
    }
    MTLRenderPipelineReflection *local = nil;
    MTLRenderPipelineReflection **out = reflection != nullptr ? reflection : &local;
    id state = ORIG(id (*)(id, SEL, MTLTileRenderPipelineDescriptor *, MTLPipelineOption,
                           MTLRenderPipelineReflection **, NSError **))(
        self, _cmd, descriptor, options | kReflectionOptions, out, error);
    Track(state, "MTLRenderPipelineState",
          "newRenderPipelineStateWithTileDescriptor:options:reflection:error:", self,
          TileRenderPipelineArgs(descriptor, *out));
    return state;
}

void D_newTileRenderPipelineStateAsync(id self, SEL _cmd, MTLTileRenderPipelineDescriptor *descriptor,
                                       MTLPipelineOption options,
                                       MTLNewRenderPipelineStateWithReflectionCompletionHandler handler) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost() || handler == nil) {
        ORIG(void (*)(id, SEL, MTLTileRenderPipelineDescriptor *, MTLPipelineOption,
                      MTLNewRenderPipelineStateWithReflectionCompletionHandler))(
            self, _cmd, descriptor, options, handler);
        return;
    }
    MTLTileRenderPipelineDescriptor *kept = [descriptor copy];
    MTLNewRenderPipelineStateWithReflectionCompletionHandler wrapped =
        ^(id<MTLRenderPipelineState> state, MTLRenderPipelineReflection *reflection, NSError *error) {
            Track(state, "MTLRenderPipelineState",
                  "newRenderPipelineStateWithTileDescriptor:options:completionHandler:", self,
                  TileRenderPipelineArgs(kept, reflection));
            [kept release];
            handler(state, reflection, error);
        };
    ORIG(void (*)(id, SEL, MTLTileRenderPipelineDescriptor *, MTLPipelineOption,
                  MTLNewRenderPipelineStateWithReflectionCompletionHandler))(
        self, _cmd, descriptor, options | kReflectionOptions, wrapped);
}

id D_newMeshRenderPipelineState(id self, SEL _cmd, id descriptor, MTLPipelineOption options,
                                MTLRenderPipelineReflection **reflection, NSError **error) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost()) {
        return ORIG(id (*)(id, SEL, id, MTLPipelineOption, MTLRenderPipelineReflection **, NSError **))(
            self, _cmd, descriptor, options, reflection, error);
    }
    MTLRenderPipelineReflection *local = nil;
    MTLRenderPipelineReflection **out = reflection != nullptr ? reflection : &local;
    id state = ORIG(id (*)(id, SEL, id, MTLPipelineOption, MTLRenderPipelineReflection **, NSError **))(
        self, _cmd, descriptor, options | kReflectionOptions, out, error);
    Track(state, "MTLRenderPipelineState",
          "newRenderPipelineStateWithMeshDescriptor:options:reflection:error:", self,
          MeshRenderPipelineArgs(descriptor, *out));
    RememberRenderPipeline(state, descriptor);
    return state;
}

void D_newMeshRenderPipelineStateAsync(id self, SEL _cmd, id descriptor, MTLPipelineOption options,
                                       MTLNewRenderPipelineStateWithReflectionCompletionHandler handler) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost() || handler == nil) {
        ORIG(void (*)(id, SEL, id, MTLPipelineOption, MTLNewRenderPipelineStateWithReflectionCompletionHandler))(
            self, _cmd, descriptor, options, handler);
        return;
    }
    id kept = [descriptor copy];
    MTLNewRenderPipelineStateWithReflectionCompletionHandler wrapped =
        ^(id<MTLRenderPipelineState> state, MTLRenderPipelineReflection *reflection, NSError *error) {
            Track(state, "MTLRenderPipelineState",
                  "newRenderPipelineStateWithMeshDescriptor:options:completionHandler:", self,
                  MeshRenderPipelineArgs(kept, reflection));
            RememberRenderPipeline(state, kept);
            [kept release];
            handler(state, reflection, error);
        };
    ORIG(void (*)(id, SEL, id, MTLPipelineOption, MTLNewRenderPipelineStateWithReflectionCompletionHandler))(
        self, _cmd, descriptor, options | kReflectionOptions, wrapped);
}

id D_newComputePipelineStateWithFunction(id self, SEL _cmd, id<MTLFunction> function, NSError **error) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost()) {
        return ORIG(id (*)(id, SEL, id, NSError **))(self, _cmd, function, error);
    }
    MTLComputePipelineReflection *reflection = nil;
    id state = [(id<MTLDevice>)self newComputePipelineStateWithFunction:function
                                                                options:kReflectionOptions
                                                             reflection:&reflection
                                                                  error:error];
    Log("device.newComputePipelineStateWithFunction: %s -> %s",
        function == nil ? "" : function.name.UTF8String, ClassName(state));
    Track(state, "MTLComputePipelineState", "newComputePipelineStateWithFunction:error:", self,
          ComputePipelineFunctionArgs(function, (id<MTLComputePipelineState>)state, reflection));
    return state;
}

id D_newComputePipelineStateWithFunctionReflection(id self, SEL _cmd, id<MTLFunction> function,
                                                   MTLPipelineOption options,
                                                   MTLComputePipelineReflection **reflection,
                                                   NSError **error) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost()) {
        return ORIG(id (*)(id, SEL, id, MTLPipelineOption, MTLComputePipelineReflection **, NSError **))(
            self, _cmd, function, options, reflection, error);
    }
    MTLComputePipelineReflection *local = nil;
    MTLComputePipelineReflection **out = reflection != nullptr ? reflection : &local;
    id state = ORIG(id (*)(id, SEL, id, MTLPipelineOption, MTLComputePipelineReflection **, NSError **))(
        self, _cmd, function, options | kReflectionOptions, out, error);
    Track(state, "MTLComputePipelineState",
          "newComputePipelineStateWithFunction:options:reflection:error:", self,
          ComputePipelineFunctionArgs(function, (id<MTLComputePipelineState>)state, *out));
    return state;
}

void D_newComputePipelineStateWithFunctionAsync(id self, SEL _cmd, id<MTLFunction> function,
                                                MTLNewComputePipelineStateCompletionHandler handler) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost() || handler == nil) {
        ORIG(void (*)(id, SEL, id, MTLNewComputePipelineStateCompletionHandler))(
            self, _cmd, function, handler);
        return;
    }
    [(id<MTLDevice>)self newComputePipelineStateWithFunction:function
                                                     options:kReflectionOptions
                                           completionHandler:^(id<MTLComputePipelineState> state,
                                                               MTLComputePipelineReflection *reflection,
                                                               NSError *error) {
        Track(state, "MTLComputePipelineState",
              "newComputePipelineStateWithFunction:completionHandler:", self,
              ComputePipelineFunctionArgs(function, state, reflection));
        handler(state, error);
    }];
}

void D_newComputePipelineStateWithFunctionOptionsAsync(
    id self, SEL _cmd, id<MTLFunction> function, MTLPipelineOption options,
    MTLNewComputePipelineStateWithReflectionCompletionHandler handler) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost() || handler == nil) {
        ORIG(void (*)(id, SEL, id, MTLPipelineOption, MTLNewComputePipelineStateWithReflectionCompletionHandler))(
            self, _cmd, function, options, handler);
        return;
    }
    MTLNewComputePipelineStateWithReflectionCompletionHandler wrapped =
        ^(id<MTLComputePipelineState> state, MTLComputePipelineReflection *reflection, NSError *error) {
            Track(state, "MTLComputePipelineState",
                  "newComputePipelineStateWithFunction:options:completionHandler:", self,
                  ComputePipelineFunctionArgs(function, state, reflection));
            handler(state, reflection, error);
        };
    ORIG(void (*)(id, SEL, id, MTLPipelineOption, MTLNewComputePipelineStateWithReflectionCompletionHandler))(
        self, _cmd, function, options | kReflectionOptions, wrapped);
}

id D_newComputePipelineStateWithDescriptor(id self, SEL _cmd, MTLComputePipelineDescriptor *descriptor,
                                           MTLPipelineOption options,
                                           MTLComputePipelineReflection **reflection, NSError **error) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost()) {
        return ORIG(id (*)(id, SEL, MTLComputePipelineDescriptor *, MTLPipelineOption,
                           MTLComputePipelineReflection **, NSError **))(
            self, _cmd, descriptor, options, reflection, error);
    }
    MTLComputePipelineReflection *local = nil;
    MTLComputePipelineReflection **out = reflection != nullptr ? reflection : &local;
    id state = ORIG(id (*)(id, SEL, MTLComputePipelineDescriptor *, MTLPipelineOption,
                           MTLComputePipelineReflection **, NSError **))(
        self, _cmd, descriptor, options | kReflectionOptions, out, error);
    Track(state, "MTLComputePipelineState",
          "newComputePipelineStateWithDescriptor:options:reflection:error:", self,
          ComputePipelineDescriptorArgs(descriptor, (id<MTLComputePipelineState>)state, *out));
    return state;
}

void D_newComputePipelineStateWithDescriptorAsync(
    id self, SEL _cmd, MTLComputePipelineDescriptor *descriptor, MTLPipelineOption options,
    MTLNewComputePipelineStateWithReflectionCompletionHandler handler) {
    Reentry reentry(self, _cmd);
    if (!reentry.outermost() || handler == nil) {
        ORIG(void (*)(id, SEL, MTLComputePipelineDescriptor *, MTLPipelineOption,
                      MTLNewComputePipelineStateWithReflectionCompletionHandler))(
            self, _cmd, descriptor, options, handler);
        return;
    }
    MTLComputePipelineDescriptor *kept = [descriptor copy];
    MTLNewComputePipelineStateWithReflectionCompletionHandler wrapped =
        ^(id<MTLComputePipelineState> state, MTLComputePipelineReflection *reflection, NSError *error) {
            Track(state, "MTLComputePipelineState",
                  "newComputePipelineStateWithDescriptor:options:completionHandler:", self,
                  ComputePipelineDescriptorArgs(kept, state, reflection));
            [kept release];
            handler(state, reflection, error);
        };
    ORIG(void (*)(id, SEL, MTLComputePipelineDescriptor *, MTLPipelineOption,
                  MTLNewComputePipelineStateWithReflectionCompletionHandler))(
        self, _cmd, descriptor, options | kReflectionOptions, wrapped);
}

// --------------------------------------------------------------------------------------------
// MTLHeap: resources sub-allocated from a heap never pass through the device. Each one moves
// the heap's usage, which goes out as an update so the Inspect view of the heap stays current.

id H_newBufferWithLength(id self, SEL _cmd, NSUInteger length, MTLResourceOptions options) {
    Reentry reentry(self, _cmd);
    id buffer = ORIG(id (*)(id, SEL, NSUInteger, MTLResourceOptions))(self, _cmd, length, options);
    if (reentry.outermost()) {
        Track(buffer, "MTLBuffer", "heap newBufferWithLength:options:", self,
              BufferArgs(buffer, length, options));
        UpdateObject(self, "usage", HeapUsageArgs(self));
    }
    HookBufferClass(buffer);
    return buffer;
}

id H_newBufferWithLengthOffset(id self, SEL _cmd, NSUInteger length, MTLResourceOptions options,
                               NSUInteger offset) {
    Reentry reentry(self, _cmd);
    id buffer = ORIG(id (*)(id, SEL, NSUInteger, MTLResourceOptions, NSUInteger))(
        self, _cmd, length, options, offset);
    if (reentry.outermost()) {
        // BufferArgs carries the heap and the offset (WriteMemoryInfo) for a placement heap too.
        Track(buffer, "MTLBuffer", "heap newBufferWithLength:options:offset:", self,
              BufferArgs(buffer, length, options));
        UpdateObject(self, "usage", HeapUsageArgs(self));
    }
    HookBufferClass(buffer);
    return buffer;
}

id H_newTextureWithDescriptor(id self, SEL _cmd, MTLTextureDescriptor *descriptor) {
    Reentry reentry(self, _cmd);
    id texture = ORIG(id (*)(id, SEL, MTLTextureDescriptor *))(self, _cmd, descriptor);
    if (reentry.outermost()) {
        Track(texture, "MTLTexture", "heap newTextureWithDescriptor:", self,
              TextureDescriptorArgs(descriptor, texture));
        UpdateObject(self, "usage", HeapUsageArgs(self));
    }
    HookTextureClass(texture);
    return texture;
}

id H_newTextureWithDescriptorOffset(id self, SEL _cmd, MTLTextureDescriptor *descriptor,
                                    NSUInteger offset) {
    Reentry reentry(self, _cmd);
    id texture = ORIG(id (*)(id, SEL, MTLTextureDescriptor *, NSUInteger))(
        self, _cmd, descriptor, offset);
    if (reentry.outermost()) {
        Track(texture, "MTLTexture", "heap newTextureWithDescriptor:offset:", self,
              TextureDescriptorArgs(descriptor, texture));
        UpdateObject(self, "usage", HeapUsageArgs(self));
    }
    HookTextureClass(texture);
    return texture;
}

// --------------------------------------------------------------------------------------------
// MTLResource and MTLHeap: memory state that changes after creation. A volatile or empty
// resource still counts in the device's allocated size until Metal reclaims it, so the state is
// shown on the object rather than subtracted from the meter.

MTLPurgeableState R_setPurgeableState(id self, SEL _cmd, MTLPurgeableState state) {
    Reentry reentry(self, _cmd);
    const MTLPurgeableState previous =
        ORIG(MTLPurgeableState (*)(id, SEL, MTLPurgeableState))(self, _cmd, state);
    // KeepCurrent is the query form: nothing changed.
    if (reentry.outermost() && state != MTLPurgeableStateKeepCurrent) {
        UpdateObject(self, "purgeable",
                     Args().e("purgeableState", PurgeableStateEnumName(state), (uint64_t)state).str());
    }
    return previous;
}

void R_makeAliasable(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    ORIG(void (*)(id, SEL))(self, _cmd);
    if (reentry.outermost()) UpdateObject(self, "aliasable", Args().b("aliasable", true).str());
}

// --------------------------------------------------------------------------------------------
// MTLLibrary: functions, so a pipeline's function resolves to the library it came from.

id L_newFunctionWithName(id self, SEL _cmd, NSString *name) {
    Reentry reentry(self, _cmd);
    id function = ORIG(id (*)(id, SEL, NSString *))(self, _cmd, name);
    if (reentry.outermost()) {
        Track(function, "MTLFunction", "newFunctionWithName:", self,
              FunctionArgs((id<MTLFunction>)function));
    }
    return function;
}

id L_newFunctionWithNameConstants(id self, SEL _cmd, NSString *name,
                                  MTLFunctionConstantValues *values, NSError **error) {
    Reentry reentry(self, _cmd);
    id function = ORIG(id (*)(id, SEL, NSString *, MTLFunctionConstantValues *, NSError **))(
        self, _cmd, name, values, error);
    if (reentry.outermost()) {
        Track(function, "MTLFunction", "newFunctionWithName:constantValues:error:", self,
              FunctionArgs((id<MTLFunction>)function));
    }
    return function;
}

void L_newFunctionWithNameConstantsAsync(id self, SEL _cmd, NSString *name,
                                         MTLFunctionConstantValues *values,
                                         void (^handler)(id<MTLFunction>, NSError *)) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost() && handler != nil) {
        void (^wrapped)(id<MTLFunction>, NSError *) = ^(id<MTLFunction> function, NSError *error) {
            Track(function, "MTLFunction", "newFunctionWithName:constantValues:completionHandler:",
                  self, FunctionArgs(function));
            handler(function, error);
        };
        ORIG(void (*)(id, SEL, NSString *, MTLFunctionConstantValues *, id))(
            self, _cmd, name, values, wrapped);
        return;
    }
    ORIG(void (*)(id, SEL, NSString *, MTLFunctionConstantValues *, id))(
        self, _cmd, name, values, handler);
}

id L_newFunctionWithDescriptor(id self, SEL _cmd, id descriptor, NSError **error) {
    Reentry reentry(self, _cmd);
    id function = ORIG(id (*)(id, SEL, id, NSError **))(self, _cmd, descriptor, error);
    if (reentry.outermost()) {
        Track(function, "MTLFunction", "newFunctionWithDescriptor:error:", self,
              FunctionArgs((id<MTLFunction>)function));
    }
    return function;
}

// --------------------------------------------------------------------------------------------
// MTLTexture: views, which are what a render pass attachment often is.

id T_newTextureViewWithPixelFormat(id self, SEL _cmd, MTLPixelFormat format) {
    Reentry reentry(self, _cmd);
    id view = ORIG(id (*)(id, SEL, MTLPixelFormat))(self, _cmd, format);
    if (reentry.outermost() && view != nil) {
        id<MTLTexture> v = (id<MTLTexture>)view;
        Track(view, "MTLTexture", "newTextureViewWithPixelFormat:", self,
              TextureViewArgs(v, format, v.textureType, NSMakeRange(0, v.mipmapLevelCount),
                              NSMakeRange(0, v.arrayLength)));
    }
    HookTextureClass(view);
    return view;
}

id T_newTextureViewFull(id self, SEL _cmd, MTLPixelFormat format, MTLTextureType type,
                        NSRange levels, NSRange slices) {
    Reentry reentry(self, _cmd);
    id view = ORIG(id (*)(id, SEL, MTLPixelFormat, MTLTextureType, NSRange, NSRange))(
        self, _cmd, format, type, levels, slices);
    if (reentry.outermost() && view != nil) {
        Track(view, "MTLTexture", "newTextureViewWithPixelFormat:textureType:levels:slices:", self,
              TextureViewArgs((id<MTLTexture>)view, format, type, levels, slices));
    }
    HookTextureClass(view);
    return view;
}

id T_newTextureViewSwizzle(id self, SEL _cmd, MTLPixelFormat format, MTLTextureType type,
                           NSRange levels, NSRange slices, MTLTextureSwizzleChannels swizzle) {
    Reentry reentry(self, _cmd);
    id view = ORIG(id (*)(id, SEL, MTLPixelFormat, MTLTextureType, NSRange, NSRange,
                          MTLTextureSwizzleChannels))(self, _cmd, format, type, levels, slices, swizzle);
    if (reentry.outermost() && view != nil) {
        Track(view, "MTLTexture",
              "newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:", self,
              TextureViewArgs((id<MTLTexture>)view, format, type, levels, slices));
    }
    HookTextureClass(view);
    return view;
}

// --------------------------------------------------------------------------------------------
// MTLBuffer

id B_newTextureWithDescriptor(id self, SEL _cmd, MTLTextureDescriptor *descriptor, NSUInteger offset,
                              NSUInteger bytesPerRow) {
    Reentry reentry(self, _cmd);
    id texture = ORIG(id (*)(id, SEL, MTLTextureDescriptor *, NSUInteger, NSUInteger))(
        self, _cmd, descriptor, offset, bytesPerRow);
    if (reentry.outermost()) {
        Track(texture, "MTLTexture", "buffer newTextureWithDescriptor:offset:bytesPerRow:", self,
              TextureDescriptorArgs(descriptor, texture));
    }
    HookTextureClass(texture);
    return texture;
}

}  // namespace

// --------------------------------------------------------------------------------------------
// Installation. Each is called with every object of its kind, not only new ones, so each stops at
// the first sighting of a class; Hook() is idempotent besides.

void HookDrawableSource(void) {
    Class cls = objc_getClass("CAMetalLayer");
    if (cls == nil || !FirstSighting(cls)) return;
    Log("hooking CAMetalLayer");
    Hook(cls, @selector(nextDrawable), (IMP)Replaced_nextDrawable);
}

void TrackDeviceObject(id device, const char *origin) {
    if (device == nil) return;
    if (IdOf(device) == 0) {
        Log("%s -> %s (%s)", origin, ClassName(device), ((id<MTLDevice>)device).name.UTF8String);
        Track(device, "MTLDevice", origin, nil, DeviceArgs((id<MTLDevice>)device, origin));
        NoteDevice(device);
    }
    HookDeviceClass(device);
}

void InstallFrameLogging(void) {
    SetCommitBoundaryLogger(LogCommitFrame);
}

void HookDrawableClass(id drawable) {
    Class cls = object_getClass(drawable);
    if (!FirstSighting(cls)) return;
    Log("hooking drawable class %s", class_getName(cls));
    Hook(cls, @selector(present), (IMP)Replaced_drawablePresent);
    Hook(cls, @selector(presentAtTime:), (IMP)Replaced_drawablePresentAtTime);
    Hook(cls, @selector(presentAfterMinimumDuration:),
         (IMP)Replaced_drawablePresentAfterMinimumDuration);
}

void HookDeviceClass(id device) {
    Class cls = object_getClass(device);
    if (!FirstSighting(cls)) return;
    Log("hooking device class %s", class_getName(cls));
    Hook(cls, @selector(newCommandQueue), (IMP)D_newCommandQueue);
    Hook(cls, @selector(newCommandQueueWithMaxCommandBufferCount:),
         (IMP)D_newCommandQueueWithMaxCommandBufferCount);
    Hook(cls, @selector(newBufferWithLength:options:), (IMP)D_newBufferWithLength);
    Hook(cls, @selector(newBufferWithBytes:length:options:), (IMP)D_newBufferWithBytes);
    Hook(cls, @selector(newBufferWithBytesNoCopy:length:options:deallocator:),
         (IMP)D_newBufferWithBytesNoCopy);
    Hook(cls, @selector(newTextureWithDescriptor:), (IMP)D_newTextureWithDescriptor);
    Hook(cls, @selector(newTextureWithDescriptor:iosurface:plane:),
         (IMP)D_newTextureWithDescriptorIOSurface);
    Hook(cls, @selector(newSharedTextureWithDescriptor:), (IMP)D_newSharedTextureWithDescriptor);
    Hook(cls, @selector(newSharedTextureWithHandle:), (IMP)D_newSharedTextureWithHandle);
    Hook(cls, @selector(newSamplerStateWithDescriptor:), (IMP)D_newSamplerStateWithDescriptor);
    Hook(cls, @selector(newDepthStencilStateWithDescriptor:),
         (IMP)D_newDepthStencilStateWithDescriptor);
    Hook(cls, @selector(newHeapWithDescriptor:), (IMP)D_newHeapWithDescriptor);
    Hook(cls, @selector(newFence), (IMP)D_newFence);
    Hook(cls, @selector(newEvent), (IMP)D_newEvent);
    Hook(cls, @selector(newSharedEvent), (IMP)D_newSharedEvent);
    Hook(cls, @selector(newSharedEventWithHandle:), (IMP)D_newSharedEventWithHandle);
    Hook(cls, @selector(newArgumentEncoderWithArguments:), (IMP)D_newArgumentEncoderWithArguments);
    Hook(cls, @selector(newIndirectCommandBufferWithDescriptor:maxCommandCount:options:),
         (IMP)D_newIndirectCommandBuffer);

    Hook(cls, @selector(newDefaultLibrary), (IMP)D_newDefaultLibrary);
    Hook(cls, @selector(newDefaultLibraryWithBundle:error:), (IMP)D_newDefaultLibraryWithBundle);
    Hook(cls, @selector(newLibraryWithFile:error:), (IMP)D_newLibraryWithFile);
    Hook(cls, @selector(newLibraryWithURL:error:), (IMP)D_newLibraryWithURL);
    Hook(cls, @selector(newLibraryWithData:error:), (IMP)D_newLibraryWithData);
    Hook(cls, @selector(newLibraryWithSource:options:error:), (IMP)D_newLibraryWithSource);
    Hook(cls, @selector(newLibraryWithSource:options:completionHandler:),
         (IMP)D_newLibraryWithSourceAsync);
    Hook(cls, sel_registerName("newLibraryWithStitchedDescriptor:error:"),
         (IMP)D_newLibraryWithStitchedDescriptor);

    Hook(cls, @selector(newRenderPipelineStateWithDescriptor:error:), (IMP)D_newRenderPipelineState);
    Hook(cls, @selector(newRenderPipelineStateWithDescriptor:options:reflection:error:),
         (IMP)D_newRenderPipelineStateReflection);
    Hook(cls, @selector(newRenderPipelineStateWithDescriptor:completionHandler:),
         (IMP)D_newRenderPipelineStateAsync);
    Hook(cls, @selector(newRenderPipelineStateWithDescriptor:options:completionHandler:),
         (IMP)D_newRenderPipelineStateOptionsAsync);
    Hook(cls, @selector(newRenderPipelineStateWithTileDescriptor:options:reflection:error:),
         (IMP)D_newTileRenderPipelineState);
    Hook(cls, @selector(newRenderPipelineStateWithTileDescriptor:options:completionHandler:),
         (IMP)D_newTileRenderPipelineStateAsync);
    Hook(cls, sel_registerName("newRenderPipelineStateWithMeshDescriptor:options:reflection:error:"),
         (IMP)D_newMeshRenderPipelineState);
    Hook(cls, sel_registerName("newRenderPipelineStateWithMeshDescriptor:options:completionHandler:"),
         (IMP)D_newMeshRenderPipelineStateAsync);
    Hook(cls, @selector(newComputePipelineStateWithFunction:error:),
         (IMP)D_newComputePipelineStateWithFunction);
    Hook(cls, @selector(newComputePipelineStateWithFunction:options:reflection:error:),
         (IMP)D_newComputePipelineStateWithFunctionReflection);
    Hook(cls, @selector(newComputePipelineStateWithFunction:completionHandler:),
         (IMP)D_newComputePipelineStateWithFunctionAsync);
    Hook(cls, @selector(newComputePipelineStateWithFunction:options:completionHandler:),
         (IMP)D_newComputePipelineStateWithFunctionOptionsAsync);
    Hook(cls, @selector(newComputePipelineStateWithDescriptor:options:reflection:error:),
         (IMP)D_newComputePipelineStateWithDescriptor);
    Hook(cls, @selector(newComputePipelineStateWithDescriptor:options:completionHandler:),
         (IMP)D_newComputePipelineStateWithDescriptorAsync);
}

void HookHeapClass(id heap) {
    if (heap == nil) return;
    Class cls = object_getClass(heap);
    if (!FirstSighting(cls)) return;
    Log("hooking heap class %s", class_getName(cls));
    Hook(cls, @selector(newBufferWithLength:options:), (IMP)H_newBufferWithLength);
    Hook(cls, @selector(newBufferWithLength:options:offset:), (IMP)H_newBufferWithLengthOffset);
    Hook(cls, @selector(newTextureWithDescriptor:), (IMP)H_newTextureWithDescriptor);
    Hook(cls, @selector(newTextureWithDescriptor:offset:), (IMP)H_newTextureWithDescriptorOffset);
    Hook(cls, @selector(setPurgeableState:), (IMP)R_setPurgeableState);
}

void HookLibraryClass(id library) {
    if (library == nil) return;
    Class cls = object_getClass(library);
    if (!FirstSighting(cls)) return;
    Log("hooking library class %s", class_getName(cls));
    Hook(cls, @selector(newFunctionWithName:), (IMP)L_newFunctionWithName);
    Hook(cls, @selector(newFunctionWithName:constantValues:error:), (IMP)L_newFunctionWithNameConstants);
    Hook(cls, @selector(newFunctionWithName:constantValues:completionHandler:),
         (IMP)L_newFunctionWithNameConstantsAsync);
    Hook(cls, sel_registerName("newFunctionWithDescriptor:error:"), (IMP)L_newFunctionWithDescriptor);
}

void HookTextureClass(id texture) {
    if (texture == nil) return;
    Class cls = object_getClass(texture);
    if (!FirstSighting(cls)) return;
    Log("hooking texture class %s", class_getName(cls));
    Hook(cls, @selector(newTextureViewWithPixelFormat:), (IMP)T_newTextureViewWithPixelFormat);
    Hook(cls, @selector(newTextureViewWithPixelFormat:textureType:levels:slices:),
         (IMP)T_newTextureViewFull);
    Hook(cls, @selector(newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:),
         (IMP)T_newTextureViewSwizzle);
    Hook(cls, @selector(setPurgeableState:), (IMP)R_setPurgeableState);
    Hook(cls, @selector(makeAliasable), (IMP)R_makeAliasable);
}

void HookBufferClass(id buffer) {
    if (buffer == nil) return;
    Class cls = object_getClass(buffer);
    if (!FirstSighting(cls)) return;
    Log("hooking buffer class %s", class_getName(cls));
    Hook(cls, @selector(newTextureWithDescriptor:offset:bytesPerRow:), (IMP)B_newTextureWithDescriptor);
    Hook(cls, @selector(setPurgeableState:), (IMP)R_setPurgeableState);
    Hook(cls, @selector(makeAliasable), (IMP)R_makeAliasable);
}

}  // namespace mtlinsp
