// The intercepted Metal methods.
//
// Every hook has the same shape: guard against re-entry, record the call if this is the
// application's own rather than one of Metal's wrappers calling the next (see Reentry in
// swizzle.h), forward to the implementation that was replaced, and hook the class of any Metal
// object that came back so the next level down is covered too. Nothing is wrapped and nothing is
// withheld from the application: the forward happens on every path.
//
// Compiled without ARC on purpose. These forward to the original implementation through a raw
// IMP, and the `new*` methods return an object the caller owns (+1); handing that straight back
// is only obviously correct when the compiler is not also inserting retains and releases.
#include "hooks.h"

#include "capture.h"
#include "formats.h"
#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <cstring>
#include <string>

namespace mtlinsp {
namespace {

std::atomic<uint64_t> g_frame{0};
std::atomic<uint32_t> g_drawsThisFrame{0};
std::atomic<uint32_t> g_dispatchesThisFrame{0};
std::atomic<uint32_t> g_encodersThisFrame{0};

const char *LabelOf(id object) {
    if (![object respondsToSelector:@selector(label)]) return "";
    NSString *label = [object performSelector:@selector(label)];
    return label == nil ? "" : label.UTF8String;
}

/** A tracked object as the UI's `{"__id", "__class"}` reference, or null. */
void WriteRef(vkinsp::JsonWriter &w, id object, const char *type) {
    const uint64_t id = IdOf(object);
    if (id == 0) {
        w.Null();
        return;
    }
    w.BeginObject();
    w.Key("__id"); w.Uint(id);
    w.Key("__class"); w.String(type);
    w.EndObject();
}

void WriteSize(vkinsp::JsonWriter &w, MTLSize size) {
    w.BeginObject();
    w.Key("width"); w.Uint(size.width);
    w.Key("height"); w.Uint(size.height);
    w.Key("depth"); w.Uint(size.depth);
    w.EndObject();
}

// The "args" of an AddObject: the descriptor the object was created from, in the same shape the
// Vulkan layer serializes a VkCreateInfo into. Written by hand, one per creating call — there is
// no vk.xml for Metal to generate them from, which is the main cost of this backend.

std::string BufferArgs(NSUInteger length, MTLResourceOptions options) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("length"); w.Uint(length);
    w.Key("options"); w.Uint((uint64_t)options);
    w.EndObject();
    return std::move(w.str());
}

/** An enum as its Metal name, falling back to the raw value for one the tables do not know. */
void WriteEnum(vkinsp::JsonWriter &w, const char *name, uint64_t value) {
    if (name != nullptr && name[0] != '\0') w.String(name);
    else w.Uint(value);
}

std::string TextureArgs(MTLTextureDescriptor *d) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("textureType"); WriteEnum(w, TextureTypeEnumName(d.textureType), (uint64_t)d.textureType);
    w.Key("pixelFormat"); WriteEnum(w, PixelFormatEnumName(d.pixelFormat), (uint64_t)d.pixelFormat);
    w.Key("width"); w.Uint(d.width);
    w.Key("height"); w.Uint(d.height);
    w.Key("depth"); w.Uint(d.depth);
    w.Key("mipmapLevelCount"); w.Uint(d.mipmapLevelCount);
    w.Key("sampleCount"); w.Uint(d.sampleCount);
    w.Key("arrayLength"); w.Uint(d.arrayLength);
    w.Key("usage"); w.Uint((uint64_t)d.usage);
    w.Key("storageMode"); WriteEnum(w, StorageModeEnumName(d.storageMode), (uint64_t)d.storageMode);
    w.EndObject();
    return std::move(w.str());
}

std::string RenderPipelineArgs(MTLRenderPipelineDescriptor *d) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("label");
    if (d.label == nil) w.Null(); else w.String(d.label.UTF8String);
    w.Key("vertexFunction");
    if (d.vertexFunction == nil) w.Null(); else w.String(d.vertexFunction.name.UTF8String);
    w.Key("fragmentFunction");
    if (d.fragmentFunction == nil) w.Null(); else w.String(d.fragmentFunction.name.UTF8String);
    w.Key("rasterSampleCount"); w.Uint(d.rasterSampleCount);
    w.Key("colorAttachments"); w.BeginArray();
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(i);
        w.Key("pixelFormat"); WriteEnum(w, PixelFormatEnumName(a.pixelFormat), (uint64_t)a.pixelFormat);
        w.Key("blendingEnabled"); w.Boolean(a.blendingEnabled);
        w.EndObject();
    }
    w.EndArray();
    w.Key("depthAttachmentPixelFormat");
    WriteEnum(w, PixelFormatEnumName(d.depthAttachmentPixelFormat),
              (uint64_t)d.depthAttachmentPixelFormat);
    w.EndObject();
    return std::move(w.str());
}

/**
 * A library's descriptor: how it was made and what is in it.
 *
 * `functionNames` is the useful part and is always available, whether the library was compiled
 * from source here or loaded precompiled — it is the only way to see what a shipped metallib
 * contains without a disassembler.
 */
std::string LibraryArgs(id<MTLLibrary> library, const char *origin, uint64_t sourceLength) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("origin"); w.String(origin);
    if (sourceLength != 0) { w.Key("sourceLength"); w.Uint(sourceLength); }
    w.Key("functionNames"); w.BeginArray();
    if (library != nil) {
        for (NSString *name in library.functionNames) w.String(name.UTF8String);
    }
    w.EndArray();
    w.EndObject();
    return std::move(w.str());
}

std::string FunctionArgs(id<MTLFunction> function) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("function");
    if (function == nil) w.Null(); else w.String(function.name.UTF8String);
    w.EndObject();
    return std::move(w.str());
}

void Replaced_setLabel(id self, SEL _cmd, NSString *label) {
    Reentry reentry;
    // Forward first: TrackLabel reads the label back off the object, so it has to be set.
    ((void (*)(id, SEL, NSString *))Original(self, _cmd))(self, _cmd, label);
    if (reentry.outermost()) TrackLabel(self);
}

/** Registers an object and hooks `setLabel:` on its class, so later labelling is streamed. */
uint64_t Track(id object, const char *type, const char *cmd, id parent, const std::string &args) {
    const uint64_t id = TrackObject(object, type, cmd, parent, args);
    Class cls = object_getClass(object);
    if (cls != nil && [object respondsToSelector:@selector(setLabel:)]) {
        Hook(cls, @selector(setLabel:), (IMP)Replaced_setLabel);
    }
    return id;
}

// --------------------------------------------------------------------------------------------
// CAMetalLayer
//
// A drawable's texture is the one a frame actually renders to, and it arrives from
// `CAMetalLayer.nextDrawable` rather than from any of the device's `new*` calls, so nothing else
// here would ever see it. Without it a render pass's colour attachment resolves to null.
// CAMetalLayer is a public QuartzCore class, so unlike everything else in this file it can be
// hooked by name, at load, before the application has a layer.

id Replaced_nextDrawable(id self, SEL _cmd) {
    Reentry reentry;
    // A framebufferOnly layer's drawable texture cannot be a blit source, and the flag has to be
    // off before the drawable is made. This is the Metal counterpart of the layer adding
    // TRANSFER_SRC usage to every image it sees: a small cost paid always, so a capture can be
    // taken at any moment without the application having been told to expect one.
    CAMetalLayer *metalLayer = (CAMetalLayer *)self;
    if (metalLayer.framebufferOnly) metalLayer.framebufferOnly = NO;
    id drawable = ((id (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    if (reentry.outermost() && drawable != nil) {
        HookDrawableClass(drawable);
        CAMetalLayer *layer = (CAMetalLayer *)self;
        id<CAMetalDrawable> metalDrawable = (id<CAMetalDrawable>)drawable;
        id<MTLTexture> texture = metalDrawable.texture;
        if (texture != nil && IdOf(texture) == 0) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("pixelFormat");
            WriteEnum(w, PixelFormatEnumName(texture.pixelFormat), (uint64_t)texture.pixelFormat);
            w.Key("textureType");
            WriteEnum(w, TextureTypeEnumName(texture.textureType), (uint64_t)texture.textureType);
            w.Key("mipmapLevelCount"); w.Uint(texture.mipmapLevelCount);
            w.Key("arrayLength"); w.Uint(texture.arrayLength);
            w.Key("width"); w.Uint(texture.width);
            w.Key("height"); w.Uint(texture.height);
            w.Key("usage"); w.Uint((uint64_t)texture.usage);
            w.Key("framebufferOnly"); w.Boolean(layer.framebufferOnly);
            w.EndObject();
            // A layer cycles a small pool of drawables, so this registers each of them once.
            Track(texture, "MTLTexture", "CAMetalLayer nextDrawable", layer.device, w.str());
            Log("nextDrawable -> texture %s %lux%lu", ClassName(texture),
                (unsigned long)texture.width, (unsigned long)texture.height);
        }
    }
    return drawable;
}

// The drawable's own present, which is a frame boundary in its own right: an engine may call it
// directly instead of going through the command buffer (Unity's macOS player does).
/** Shared by the drawable-present hooks: the log line the command-buffer path also writes. */
void LogDrawableFrame(id drawable, const char *how) {
    Log("--- frame %llu: %u encoders, %u draws, %u dispatches (%s presents itself, %s) ---",
        (unsigned long long)g_frame++, g_encodersThisFrame.exchange(0),
        g_drawsThisFrame.exchange(0), g_dispatchesThisFrame.exchange(0), ClassName(drawable), how);
}

void Replaced_drawablePresent(id self, SEL _cmd) {
    Reentry reentry;
    ((void (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    // Logged only when it is a boundary: the convenience method calls through to here, and its
    // own hook has already reported the frame.
    if (reentry.outermost() && OnDrawablePresent(self)) LogDrawableFrame(self, "present");
}

void Replaced_drawablePresentAtTime(id self, SEL _cmd, CFTimeInterval time) {
    Reentry reentry;
    ((void (*)(id, SEL, CFTimeInterval))Original(self, _cmd))(self, _cmd, time);
    // Logged only when it is a boundary: the convenience method calls through to here, and its
    // own hook has already reported the frame.
    if (reentry.outermost() && OnDrawablePresent(self)) LogDrawableFrame(self, "presentAtTime:");
}

void Replaced_drawablePresentAfterMinimumDuration(id self, SEL _cmd, CFTimeInterval duration) {
    Reentry reentry;
    ((void (*)(id, SEL, CFTimeInterval))Original(self, _cmd))(self, _cmd, duration);
    // Logged only when it is a boundary: the convenience method calls through to here, and its
    // own hook has already reported the frame.
    if (reentry.outermost() && OnDrawablePresent(self)) LogDrawableFrame(self, "presentAfterMinimumDuration:");
}

// --------------------------------------------------------------------------------------------
// MTLDevice

id Replaced_newCommandQueue(id self, SEL _cmd) {
    Reentry reentry;
    id queue = ((id (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    if (reentry.outermost()) {
        Log("device.newCommandQueue -> %s", ClassName(queue));
        Track(queue, "MTLCommandQueue", "newCommandQueue", self, {});
    }
    HookCommandQueueClass(queue);
    return queue;
}

id Replaced_newCommandQueueWithMaxCommandBufferCount(id self, SEL _cmd, NSUInteger count) {
    Reentry reentry;
    id queue = ((id (*)(id, SEL, NSUInteger))Original(self, _cmd))(self, _cmd, count);
    if (reentry.outermost()) {
        Log("device.newCommandQueueWithMaxCommandBufferCount:%lu -> %s", (unsigned long)count,
            ClassName(queue));
        Track(queue, "MTLCommandQueue", "newCommandQueueWithMaxCommandBufferCount:", self,
                    {});
    }
    HookCommandQueueClass(queue);
    return queue;
}

id Replaced_newBufferWithLength(id self, SEL _cmd, NSUInteger length, MTLResourceOptions options) {
    Reentry reentry;
    id buffer = ((id (*)(id, SEL, NSUInteger, MTLResourceOptions))Original(self, _cmd))(
        self, _cmd, length, options);
    if (reentry.outermost()) {
        Log("device.newBufferWithLength:%lu options:0x%lx -> %s", (unsigned long)length,
            (unsigned long)options, ClassName(buffer));
        Track(buffer, "MTLBuffer", "newBufferWithLength:options:", self,
                    BufferArgs(length, options));
    }
    return buffer;
}

id Replaced_newBufferWithBytes(id self, SEL _cmd, const void *bytes, NSUInteger length,
                               MTLResourceOptions options) {
    Reentry reentry;
    id buffer = ((id (*)(id, SEL, const void *, NSUInteger, MTLResourceOptions))Original(self, _cmd))(
        self, _cmd, bytes, length, options);
    if (reentry.outermost()) {
        Log("device.newBufferWithBytes:length:%lu options:0x%lx -> %s", (unsigned long)length,
            (unsigned long)options, ClassName(buffer));
        Track(buffer, "MTLBuffer", "newBufferWithBytes:length:options:", self,
                    BufferArgs(length, options));
    }
    return buffer;
}

id Replaced_newTextureWithDescriptor(id self, SEL _cmd, MTLTextureDescriptor *descriptor) {
    Reentry reentry;
    id texture = ((id (*)(id, SEL, MTLTextureDescriptor *))Original(self, _cmd))(self, _cmd,
                                                                                descriptor);
    if (reentry.outermost()) {
        Log("device.newTextureWithDescriptor: %lux%lu fmt=%lu usage=0x%lx -> %s",
            (unsigned long)descriptor.width, (unsigned long)descriptor.height,
            (unsigned long)descriptor.pixelFormat, (unsigned long)descriptor.usage,
            ClassName(texture));
        Track(texture, "MTLTexture", "newTextureWithDescriptor:", self,
                    TextureArgs(descriptor));
    }
    return texture;
}

id Replaced_newRenderPipelineState(id self, SEL _cmd, MTLRenderPipelineDescriptor *descriptor,
                                   NSError **error) {
    Reentry reentry;
    id state = ((id (*)(id, SEL, MTLRenderPipelineDescriptor *, NSError **))Original(self, _cmd))(
        self, _cmd, descriptor, error);
    if (reentry.outermost()) {
        Log("device.newRenderPipelineStateWithDescriptor: label=\"%s\" -> %s",
            descriptor.label == nil ? "" : descriptor.label.UTF8String, ClassName(state));
        Track(state, "MTLRenderPipelineState",
                    "newRenderPipelineStateWithDescriptor:error:", self,
                    RenderPipelineArgs(descriptor));
    }
    return state;
}

id Replaced_newLibraryWithData(id self, SEL _cmd, dispatch_data_t data, NSError **error) {
    Reentry reentry;
    id library = ((id (*)(id, SEL, dispatch_data_t, NSError **))Original(self, _cmd))(self, _cmd,
                                                                                      data, error);
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
    return library;
}

id Replaced_newLibraryWithURL(id self, SEL _cmd, NSURL *url, NSError **error) {
    Reentry reentry;
    id library = ((id (*)(id, SEL, NSURL *, NSError **))Original(self, _cmd))(self, _cmd, url,
                                                                              error);
    if (reentry.outermost()) {
        Log("device.newLibraryWithURL: %s -> %s", url.path.UTF8String, ClassName(library));
        Track(library, "MTLLibrary", "newLibraryWithURL:error:", self,
              LibraryArgs((id<MTLLibrary>)library, url.path.UTF8String, 0));
    }
    return library;
}

id Replaced_newDefaultLibrary(id self, SEL _cmd) {
    Reentry reentry;
    id library = ((id (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    if (reentry.outermost()) {
        Log("device.newDefaultLibrary -> %s", ClassName(library));
        Track(library, "MTLLibrary", "newDefaultLibrary", self,
              LibraryArgs((id<MTLLibrary>)library, "default.metallib", 0));
    }
    return library;
}

id Replaced_newComputePipelineStateWithFunction(id self, SEL _cmd, id<MTLFunction> function,
                                                NSError **error) {
    Reentry reentry;
    id state = ((id (*)(id, SEL, id, NSError **))Original(self, _cmd))(self, _cmd, function, error);
    if (reentry.outermost()) {
        Log("device.newComputePipelineStateWithFunction: %s -> %s",
            function == nil ? "" : function.name.UTF8String, ClassName(state));
        Track(state, "MTLComputePipelineState",
                    "newComputePipelineStateWithFunction:error:", self, FunctionArgs(function));
    }
    return state;
}

id Replaced_newLibraryWithSource(id self, SEL _cmd, NSString *source, MTLCompileOptions *options,
                                 NSError **error) {
    Reentry reentry;
    id library = ((id (*)(id, SEL, NSString *, MTLCompileOptions *, NSError **))Original(self, _cmd))(
        self, _cmd, source, options, error);
    if (reentry.outermost()) {
        Log("device.newLibraryWithSource: %lu chars -> %s", (unsigned long)source.length,
            ClassName(library));
        Track(library, "MTLLibrary", "newLibraryWithSource:options:error:", self,
              LibraryArgs((id<MTLLibrary>)library, "source", source.length));
        // The Metal Shading Language the application compiled, verbatim.
        const char *utf8 = source.UTF8String;
        if (utf8 != nullptr) AddBlob(library, "Metal Shading Language", utf8, strlen(utf8));
    }
    return library;
}

// --------------------------------------------------------------------------------------------
// MTLCommandQueue

id Replaced_commandBuffer(id self, SEL _cmd) {
    Reentry reentry;
    id commandBuffer = ((id (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    if (reentry.outermost()) Log("queue.commandBuffer -> %s", ClassName(commandBuffer));
    HookCommandBufferClass(commandBuffer);
    return commandBuffer;
}

id Replaced_commandBufferWithDescriptor(id self, SEL _cmd, id descriptor) {
    Reentry reentry;
    id commandBuffer = ((id (*)(id, SEL, id))Original(self, _cmd))(self, _cmd, descriptor);
    if (reentry.outermost()) Log("queue.commandBufferWithDescriptor: -> %s", ClassName(commandBuffer));
    HookCommandBufferClass(commandBuffer);
    return commandBuffer;
}

id Replaced_computeCommandEncoderWithDispatchType(id self, SEL _cmd, NSUInteger dispatchType) {
    Reentry reentry;
    id encoder = ((id (*)(id, SEL, NSUInteger))Original(self, _cmd))(self, _cmd, dispatchType);
    if (reentry.outermost()) {
        Log("commandBuffer.computeCommandEncoderWithDispatchType:%lu -> %s",
            (unsigned long)dispatchType, ClassName(encoder));
        g_encodersThisFrame++;
        BeginPass(encoder, self);
        if (Recording()) RecordCommand("computeCommandEncoderWithDispatchType:", encoder, {});
    }
    HookComputeEncoderClass(encoder);
    return encoder;
}

// --------------------------------------------------------------------------------------------
// MTLCommandBuffer

id Replaced_renderCommandEncoder(id self, SEL _cmd, MTLRenderPassDescriptor *descriptor) {
    Reentry reentry;
    // An attachment with storeAction DontCare has undefined contents after the pass, so during a
    // capture the store is forced on — the counterpart of the layer's store-everything render
    // pass, and far less work because Metal's store action is a mutable field. The application's
    // own descriptor is left alone: it owns and reuses that object.
    MTLRenderPassDescriptor *pass = descriptor;
    if (reentry.outermost() && Recording() && descriptor != nil) {
        pass = [descriptor copy];
        for (NSUInteger i = 0; i < 8; i++) {
            MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[i];
            if (a.texture != nil && a.storeAction == MTLStoreActionDontCare) {
                a.storeAction = MTLStoreActionStore;
            }
        }
    }
    id encoder = ((id (*)(id, SEL, MTLRenderPassDescriptor *))Original(self, _cmd))(self, _cmd,
                                                                                    pass);
    if (reentry.outermost()) {
        BeginPass(encoder, self);
        if (Recording()) {
            for (NSUInteger i = 0; i < 8; i++) {
                MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[i];
                if (a.texture != nil) AddPassAttachment(encoder, a.texture, (uint32_t)i);
            }
        }
        MTLRenderPassColorAttachmentDescriptor *color = descriptor.colorAttachments[0];
        Log("commandBuffer.renderCommandEncoderWithDescriptor: load=%lu store=%lu texture=%s -> %s",
            (unsigned long)color.loadAction, (unsigned long)color.storeAction,
            ClassName(color.texture), ClassName(encoder));
        g_encodersThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("colorAttachments"); w.BeginArray();
            for (NSUInteger i = 0; i < 8; i++) {
                MTLRenderPassColorAttachmentDescriptor *a = descriptor.colorAttachments[i];
                if (a.texture == nil) continue;
                w.BeginObject();
                w.Key("index"); w.Uint(i);
                w.Key("texture"); WriteRef(w, a.texture, "MTLTexture");
                w.Key("loadAction"); w.Uint((uint64_t)a.loadAction);
                w.Key("storeAction"); w.Uint((uint64_t)a.storeAction);
                w.EndObject();
            }
            w.EndArray();
            w.Key("depthAttachment"); WriteRef(w, descriptor.depthAttachment.texture, "MTLTexture");
            w.EndObject();
            RecordCommand("renderCommandEncoderWithDescriptor:", encoder, w.str());
        }
    }
    HookRenderEncoderClass(encoder);
    return encoder;
}

id Replaced_computeCommandEncoder(id self, SEL _cmd) {
    Reentry reentry;
    id encoder = ((id (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    if (reentry.outermost()) {
        Log("commandBuffer.computeCommandEncoder -> %s", ClassName(encoder));
        g_encodersThisFrame++;
        BeginPass(encoder, self);
        if (Recording()) RecordCommand("computeCommandEncoder", encoder, {});
    }
    HookComputeEncoderClass(encoder);
    return encoder;
}

id Replaced_blitCommandEncoder(id self, SEL _cmd) {
    Reentry reentry;
    id encoder = ((id (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    if (reentry.outermost()) Log("commandBuffer.blitCommandEncoder -> %s", ClassName(encoder));
    HookBlitEncoderClass(encoder);
    return encoder;
}

void Replaced_presentDrawable(id self, SEL _cmd, id drawable) {
    Reentry reentry;
    if (reentry.outermost()) {
        // The frame boundary, the counterpart of vkQueuePresentKHR in the Vulkan layer.
        Log("--- frame %llu: %u encoders, %u draws, %u dispatches (%s presents %s) ---",
            (unsigned long long)g_frame++, g_encodersThisFrame.exchange(0),
            g_drawsThisFrame.exchange(0), g_dispatchesThisFrame.exchange(0), ClassName(self),
            ClassName(drawable));
        if (Recording()) RecordCommand("presentDrawable:", self, {});
        // Not the frame boundary itself: the commit that follows is. See OnCommit.
        OnPresentDrawable(self, drawable);
    }
    ((void (*)(id, SEL, id))Original(self, _cmd))(self, _cmd, drawable);
}

void Replaced_commit(id self, SEL _cmd) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("commandBuffer.commit label=\"%s\"", LabelOf(self));
        if (Recording()) RecordCommand("commit", self, {});
        // Drives the capture state machine: arms, counts a frame, or finishes and sends.
        OnCommit(self);
    }
    ((void (*)(id, SEL))Original(self, _cmd))(self, _cmd);
}

// --------------------------------------------------------------------------------------------
// MTLRenderCommandEncoder

void Replaced_setRenderPipelineState(id self, SEL _cmd, id state) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.setRenderPipelineState: %s", ClassName(state));
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("pipeline"); WriteRef(w, state, "MTLRenderPipelineState");
            w.EndObject();
            RecordCommand("setRenderPipelineState:", self, w.str());
        }
    }
    ((void (*)(id, SEL, id))Original(self, _cmd))(self, _cmd, state);
}

void Replaced_setVertexBuffer(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger index) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.setVertexBuffer:\"%s\" offset:%lu atIndex:%lu", LabelOf(buffer),
            (unsigned long)offset, (unsigned long)index);
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("buffer"); WriteRef(w, buffer, "MTLBuffer");
            w.Key("offset"); w.Uint(offset);
            w.Key("index"); w.Uint(index);
            w.EndObject();
            // From the offset to the end of the buffer: Metal does not say how much a draw will
            // read, and the stride is in the pipeline's vertex descriptor, not the binding.
            const uint64_t data = QueueBufferCapture(buffer, offset, 0);
            RecordCommandWithBuffers("setVertexBuffer:offset:atIndex:", self, w.str(), {data});
        }
    }
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))Original(self, _cmd))(self, _cmd, buffer,
                                                                          offset, index);
}

void Replaced_drawPrimitives(id self, SEL _cmd, NSUInteger type, NSUInteger start,
                             NSUInteger count, NSUInteger instances) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.drawPrimitives: type=%lu start=%lu count=%lu instances=%lu",
            (unsigned long)type, (unsigned long)start, (unsigned long)count,
            (unsigned long)instances);
        g_drawsThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("primitiveType"); w.Uint(type);
            w.Key("vertexStart"); w.Uint(start);
            w.Key("vertexCount"); w.Uint(count);
            w.Key("instanceCount"); w.Uint(instances);
            w.EndObject();
            RecordCommand("drawPrimitives:vertexStart:vertexCount:instanceCount:", self, w.str());
        }
    }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger))Original(self, _cmd))(
        self, _cmd, type, start, count, instances);
}

void Replaced_drawIndexedPrimitives(id self, SEL _cmd, NSUInteger type, NSUInteger indexCount,
                                    NSUInteger indexType, id indexBuffer, NSUInteger offset,
                                    NSUInteger instances) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.drawIndexedPrimitives: type=%lu indexCount=%lu indexBuffer=\"%s\" "
            "instances=%lu",
            (unsigned long)type, (unsigned long)indexCount, LabelOf(indexBuffer),
            (unsigned long)instances);
        g_drawsThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("primitiveType"); w.Uint(type);
            w.Key("indexCount"); w.Uint(indexCount);
            w.Key("indexType"); w.Uint(indexType);
            w.Key("indexBuffer"); WriteRef(w, indexBuffer, "MTLBuffer");
            w.Key("indexBufferOffset"); w.Uint(offset);
            w.Key("instanceCount"); w.Uint(instances);
            w.EndObject();
            // MTLIndexType: 0 = UInt16, 1 = UInt32. The draw says exactly how many it reads.
            const uint64_t indexBytes = indexCount * (indexType == 0 ? 2 : 4);
            const uint64_t data = QueueBufferCapture(indexBuffer, offset, indexBytes);
            RecordCommandWithBuffers("drawIndexedPrimitives:indexCount:indexType:indexBuffer:"
                                     "indexBufferOffset:instanceCount:", self, w.str(), {data});
        }
    }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger))Original(
        self, _cmd))(self, _cmd, type, indexCount, indexType, indexBuffer, offset, instances);
}

void Replaced_endEncoding(id self, SEL _cmd) {
    Reentry reentry;
    const bool outermost = reentry.outermost();
    if (outermost) {
        Log("  encoder.endEncoding (%s \"%s\")", ClassName(self), LabelOf(self));
        if (Recording()) RecordCommand("endEncoding", self, {});
    }
    ((void (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    // Only after the forward: a command buffer allows one encoder at a time, and until the
    // application's is really closed the read-back's blit encoder cannot be created.
    if (outermost) EndRenderPass(self);
}

// Present has three spellings, and an engine that paces its frames uses one of the timed ones.
// Missing the one an application uses means never seeing a frame boundary at all, so a capture
// never arms and nothing is recorded — which looks like "capture is broken" rather than "one
// selector is unhooked".

void Replaced_presentDrawableAtTime(id self, SEL _cmd, id drawable, CFTimeInterval time) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("--- frame %llu: %u encoders, %u draws, %u dispatches (%s presents %s at %.3f) ---",
            (unsigned long long)g_frame++, g_encodersThisFrame.exchange(0),
            g_drawsThisFrame.exchange(0), g_dispatchesThisFrame.exchange(0), ClassName(self),
            ClassName(drawable), time);
        if (Recording()) RecordCommand("presentDrawable:atTime:", self, {});
        OnPresentDrawable(self, drawable);
    }
    ((void (*)(id, SEL, id, CFTimeInterval))Original(self, _cmd))(self, _cmd, drawable, time);
}

void Replaced_presentDrawableAfterMinimumDuration(id self, SEL _cmd, id drawable,
                                                  CFTimeInterval duration) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("--- frame %llu: %u encoders, %u draws, %u dispatches (%s presents %s after %.3f) ---",
            (unsigned long long)g_frame++, g_encodersThisFrame.exchange(0),
            g_drawsThisFrame.exchange(0), g_dispatchesThisFrame.exchange(0), ClassName(self),
            ClassName(drawable), duration);
        if (Recording()) RecordCommand("presentDrawable:afterMinimumDuration:", self, {});
        OnPresentDrawable(self, drawable);
    }
    ((void (*)(id, SEL, id, CFTimeInterval))Original(self, _cmd))(self, _cmd, drawable, duration);
}

// The draw calls, in the spellings an engine actually emits. Metal has one selector per overload
// rather than optional arguments, so each has to be hooked separately; a real renderer uses the
// base-vertex/base-instance forms that a hand-written sample never does.

void Replaced_drawPrimitivesSimple(id self, SEL _cmd, NSUInteger type, NSUInteger start,
                                   NSUInteger count) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.drawPrimitives: type=%lu start=%lu count=%lu", (unsigned long)type,
            (unsigned long)start, (unsigned long)count);
        g_drawsThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("primitiveType"); w.Uint(type);
            w.Key("vertexStart"); w.Uint(start);
            w.Key("vertexCount"); w.Uint(count);
            w.Key("instanceCount"); w.Uint(1);
            w.EndObject();
            RecordCommand("drawPrimitives:vertexStart:vertexCount:", self, w.str());
        }
    }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))Original(self, _cmd))(self, _cmd, type,
                                                                                  start, count);
}

void Replaced_drawPrimitivesBaseInstance(id self, SEL _cmd, NSUInteger type, NSUInteger start,
                                         NSUInteger count, NSUInteger instances,
                                         NSUInteger baseInstance) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.drawPrimitives: type=%lu count=%lu instances=%lu baseInstance=%lu",
            (unsigned long)type, (unsigned long)count, (unsigned long)instances,
            (unsigned long)baseInstance);
        g_drawsThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("primitiveType"); w.Uint(type);
            w.Key("vertexStart"); w.Uint(start);
            w.Key("vertexCount"); w.Uint(count);
            w.Key("instanceCount"); w.Uint(instances);
            w.Key("baseInstance"); w.Uint(baseInstance);
            w.EndObject();
            RecordCommand("drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
                          self, w.str());
        }
    }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger, NSUInteger))Original(
        self, _cmd))(self, _cmd, type, start, count, instances, baseInstance);
}

void Replaced_drawIndexedSimple(id self, SEL _cmd, NSUInteger type, NSUInteger indexCount,
                                NSUInteger indexType, id indexBuffer, NSUInteger offset) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.drawIndexedPrimitives: type=%lu indexCount=%lu indexBuffer=\"%s\"",
            (unsigned long)type, (unsigned long)indexCount, LabelOf(indexBuffer));
        g_drawsThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("primitiveType"); w.Uint(type);
            w.Key("indexCount"); w.Uint(indexCount);
            w.Key("indexType"); w.Uint(indexType);
            w.Key("indexBuffer"); WriteRef(w, indexBuffer, "MTLBuffer");
            w.Key("indexBufferOffset"); w.Uint(offset);
            w.Key("instanceCount"); w.Uint(1);
            w.EndObject();
            const uint64_t data = QueueBufferCapture(indexBuffer, offset,
                                                     indexCount * (indexType == 0 ? 2 : 4));
            RecordCommandWithBuffers("drawIndexedPrimitives:indexCount:indexType:indexBuffer:"
                                     "indexBufferOffset:", self, w.str(), {data});
        }
    }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger))Original(self, _cmd))(
        self, _cmd, type, indexCount, indexType, indexBuffer, offset);
}

void Replaced_drawIndexedBaseVertex(id self, SEL _cmd, NSUInteger type, NSUInteger indexCount,
                                    NSUInteger indexType, id indexBuffer, NSUInteger offset,
                                    NSUInteger instances, NSInteger baseVertex,
                                    NSUInteger baseInstance) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.drawIndexedPrimitives: type=%lu indexCount=%lu indexBuffer=\"%s\" "
            "instances=%lu baseVertex=%ld", (unsigned long)type, (unsigned long)indexCount,
            LabelOf(indexBuffer), (unsigned long)instances, (long)baseVertex);
        g_drawsThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("primitiveType"); w.Uint(type);
            w.Key("indexCount"); w.Uint(indexCount);
            w.Key("indexType"); w.Uint(indexType);
            w.Key("indexBuffer"); WriteRef(w, indexBuffer, "MTLBuffer");
            w.Key("indexBufferOffset"); w.Uint(offset);
            w.Key("instanceCount"); w.Uint(instances);
            w.Key("baseVertex"); w.Int(baseVertex);
            w.Key("baseInstance"); w.Uint(baseInstance);
            w.EndObject();
            const uint64_t data = QueueBufferCapture(indexBuffer, offset,
                                                     indexCount * (indexType == 0 ? 2 : 4));
            RecordCommandWithBuffers("drawIndexedPrimitives:indexCount:indexType:indexBuffer:"
                                     "indexBufferOffset:instanceCount:baseVertex:baseInstance:",
                                     self, w.str(), {data});
        }
    }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger, NSInteger,
               NSUInteger))Original(self, _cmd))(self, _cmd, type, indexCount, indexType,
                                                 indexBuffer, offset, instances, baseVertex,
                                                 baseInstance);
}

// Bound textures, which is how a real renderer gets its images to a shader.

void Replaced_setFragmentTexture(id self, SEL _cmd, id texture, NSUInteger index) {
    Reentry reentry;
    if (reentry.outermost() && Recording()) {
        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("texture"); WriteRef(w, texture, "MTLTexture");
        w.Key("index"); w.Uint(index);
        w.EndObject();
        RecordCommand("setFragmentTexture:atIndex:", self, w.str());
    }
    ((void (*)(id, SEL, id, NSUInteger))Original(self, _cmd))(self, _cmd, texture, index);
}

void Replaced_setFragmentBuffer(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger index) {
    Reentry reentry;
    if (reentry.outermost() && Recording()) {
        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("buffer"); WriteRef(w, buffer, "MTLBuffer");
        w.Key("offset"); w.Uint(offset);
        w.Key("index"); w.Uint(index);
        w.EndObject();
        const uint64_t data = QueueBufferCapture(buffer, offset, 0);
        RecordCommandWithBuffers("setFragmentBuffer:offset:atIndex:", self, w.str(), {data});
    }
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))Original(self, _cmd))(self, _cmd, buffer,
                                                                          offset, index);
}

// --------------------------------------------------------------------------------------------
// MTLComputeCommandEncoder

void Replaced_setComputePipelineState(id self, SEL _cmd, id state) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.setComputePipelineState: %s", ClassName(state));
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("pipeline"); WriteRef(w, state, "MTLComputePipelineState");
            w.EndObject();
            RecordCommand("setComputePipelineState:", self, w.str());
        }
    }
    ((void (*)(id, SEL, id))Original(self, _cmd))(self, _cmd, state);
}

void Replaced_dispatchThreads(id self, SEL _cmd, MTLSize threads, MTLSize perGroup) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.dispatchThreads: %lux%lux%lu group %lux%lux%lu",
            (unsigned long)threads.width, (unsigned long)threads.height,
            (unsigned long)threads.depth, (unsigned long)perGroup.width,
            (unsigned long)perGroup.height, (unsigned long)perGroup.depth);
        g_dispatchesThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("threadsPerGrid"); WriteSize(w, threads);
            w.Key("threadsPerThreadgroup"); WriteSize(w, perGroup);
            w.EndObject();
            RecordCommand("dispatchThreads:threadsPerThreadgroup:", self, w.str());
        }
    }
    ((void (*)(id, SEL, MTLSize, MTLSize))Original(self, _cmd))(self, _cmd, threads, perGroup);
}

void Replaced_dispatchThreadgroups(id self, SEL _cmd, MTLSize groups, MTLSize perGroup) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.dispatchThreadgroups: %lux%lux%lu group %lux%lux%lu",
            (unsigned long)groups.width, (unsigned long)groups.height, (unsigned long)groups.depth,
            (unsigned long)perGroup.width, (unsigned long)perGroup.height,
            (unsigned long)perGroup.depth);
        g_dispatchesThisFrame++;
        if (Recording()) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("threadgroupsPerGrid"); WriteSize(w, groups);
            w.Key("threadsPerThreadgroup"); WriteSize(w, perGroup);
            w.EndObject();
            RecordCommand("dispatchThreadgroups:threadsPerThreadgroup:", self, w.str());
        }
    }
    ((void (*)(id, SEL, MTLSize, MTLSize))Original(self, _cmd))(self, _cmd, groups, perGroup);
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

void TrackDeviceObject(id device) {
    if (device == nil) return;
    id<MTLDevice> metalDevice = (id<MTLDevice>)device;
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("name"); w.String(metalDevice.name.UTF8String);
    w.Key("registryID"); w.Uint(metalDevice.registryID);
    w.Key("hasUnifiedMemory"); w.Boolean(metalDevice.hasUnifiedMemory);
    w.Key("recommendedMaxWorkingSetSize"); w.Uint(metalDevice.recommendedMaxWorkingSetSize);
    w.Key("maxBufferLength"); w.Uint(metalDevice.maxBufferLength);
    w.EndObject();
    Track(device, "MTLDevice", "MTLCreateSystemDefaultDevice", nil, w.str());
    HookDeviceClass(device);
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
    Hook(cls, @selector(newCommandQueue), (IMP)Replaced_newCommandQueue);
    Hook(cls, @selector(newCommandQueueWithMaxCommandBufferCount:),
         (IMP)Replaced_newCommandQueueWithMaxCommandBufferCount);
    Hook(cls, @selector(newBufferWithLength:options:), (IMP)Replaced_newBufferWithLength);
    Hook(cls, @selector(newBufferWithBytes:length:options:), (IMP)Replaced_newBufferWithBytes);
    Hook(cls, @selector(newTextureWithDescriptor:), (IMP)Replaced_newTextureWithDescriptor);
    Hook(cls, @selector(newRenderPipelineStateWithDescriptor:error:),
         (IMP)Replaced_newRenderPipelineState);
    Hook(cls, @selector(newComputePipelineStateWithFunction:error:),
         (IMP)Replaced_newComputePipelineStateWithFunction);
    Hook(cls, @selector(newLibraryWithSource:options:error:), (IMP)Replaced_newLibraryWithSource);
    Hook(cls, @selector(newLibraryWithData:error:), (IMP)Replaced_newLibraryWithData);
    Hook(cls, @selector(newLibraryWithURL:error:), (IMP)Replaced_newLibraryWithURL);
    Hook(cls, @selector(newDefaultLibrary), (IMP)Replaced_newDefaultLibrary);
}

void HookCommandQueueClass(id queue) {
    Class cls = object_getClass(queue);
    if (!FirstSighting(cls)) return;
    Log("hooking command queue class %s", class_getName(cls));
    Hook(cls, @selector(commandBuffer), (IMP)Replaced_commandBuffer);
    // Engines commonly take the unretained form for its lower overhead; without this nothing
    // below the queue is ever hooked.
    Hook(cls, @selector(commandBufferWithUnretainedReferences), (IMP)Replaced_commandBuffer);
    Hook(cls, @selector(commandBufferWithDescriptor:),
         (IMP)Replaced_commandBufferWithDescriptor);
}

void HookCommandBufferClass(id commandBuffer) {
    Class cls = object_getClass(commandBuffer);
    if (!FirstSighting(cls)) return;
    Log("hooking command buffer class %s", class_getName(cls));
    Hook(cls, @selector(renderCommandEncoderWithDescriptor:), (IMP)Replaced_renderCommandEncoder);
    Hook(cls, @selector(computeCommandEncoder), (IMP)Replaced_computeCommandEncoder);
    Hook(cls, @selector(computeCommandEncoderWithDispatchType:),
         (IMP)Replaced_computeCommandEncoderWithDispatchType);
    Hook(cls, @selector(blitCommandEncoder), (IMP)Replaced_blitCommandEncoder);
    Hook(cls, @selector(presentDrawable:), (IMP)Replaced_presentDrawable);
    Hook(cls, @selector(presentDrawable:atTime:), (IMP)Replaced_presentDrawableAtTime);
    Hook(cls, @selector(presentDrawable:afterMinimumDuration:),
         (IMP)Replaced_presentDrawableAfterMinimumDuration);
    Hook(cls, @selector(commit), (IMP)Replaced_commit);
}

void HookRenderEncoderClass(id encoder) {
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking render encoder class %s", class_getName(cls));
    Hook(cls, @selector(setRenderPipelineState:), (IMP)Replaced_setRenderPipelineState);
    Hook(cls, @selector(setVertexBuffer:offset:atIndex:), (IMP)Replaced_setVertexBuffer);
    Hook(cls, @selector(setFragmentTexture:atIndex:), (IMP)Replaced_setFragmentTexture);
    Hook(cls, @selector(setFragmentBuffer:offset:atIndex:), (IMP)Replaced_setFragmentBuffer);
    Hook(cls, @selector(drawPrimitives:vertexStart:vertexCount:), (IMP)Replaced_drawPrimitivesSimple);
    Hook(cls, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:),
         (IMP)Replaced_drawPrimitivesBaseInstance);
    Hook(cls, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:),
         (IMP)Replaced_drawIndexedSimple);
    Hook(cls,
         @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:
                                        instanceCount:baseVertex:baseInstance:),
         (IMP)Replaced_drawIndexedBaseVertex);
    Hook(cls, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:),
         (IMP)Replaced_drawPrimitives);
    Hook(cls,
         @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:
                                        instanceCount:),
         (IMP)Replaced_drawIndexedPrimitives);
    Hook(cls, @selector(endEncoding), (IMP)Replaced_endEncoding);
}

void HookComputeEncoderClass(id encoder) {
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking compute encoder class %s", class_getName(cls));
    Hook(cls, @selector(setComputePipelineState:), (IMP)Replaced_setComputePipelineState);
    Hook(cls, @selector(dispatchThreads:threadsPerThreadgroup:), (IMP)Replaced_dispatchThreads);
    Hook(cls, @selector(dispatchThreadgroups:threadsPerThreadgroup:),
         (IMP)Replaced_dispatchThreadgroups);
    Hook(cls, @selector(endEncoding), (IMP)Replaced_endEncoding);
}

void HookBlitEncoderClass(id encoder) {
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking blit encoder class %s", class_getName(cls));
    Hook(cls, @selector(endEncoding), (IMP)Replaced_endEncoding);
}

}  // namespace mtlinsp
