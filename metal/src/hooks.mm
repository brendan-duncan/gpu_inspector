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
#include "swizzle.h"

#import <Metal/Metal.h>

#include <atomic>

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

// --------------------------------------------------------------------------------------------
// MTLDevice

id Replaced_newCommandQueue(id self, SEL _cmd) {
    Reentry reentry;
    id queue = ((id (*)(id, SEL))Original(self, _cmd))(self, _cmd);
    if (reentry.outermost()) Log("device.newCommandQueue -> %s", ClassName(queue));
    HookCommandQueueClass(queue);
    return queue;
}

id Replaced_newCommandQueueWithMaxCommandBufferCount(id self, SEL _cmd, NSUInteger count) {
    Reentry reentry;
    id queue = ((id (*)(id, SEL, NSUInteger))Original(self, _cmd))(self, _cmd, count);
    if (reentry.outermost()) {
        Log("device.newCommandQueueWithMaxCommandBufferCount:%lu -> %s", (unsigned long)count,
            ClassName(queue));
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

// --------------------------------------------------------------------------------------------
// MTLCommandBuffer

id Replaced_renderCommandEncoder(id self, SEL _cmd, MTLRenderPassDescriptor *descriptor) {
    Reentry reentry;
    id encoder = ((id (*)(id, SEL, MTLRenderPassDescriptor *))Original(self, _cmd))(self, _cmd,
                                                                                    descriptor);
    if (reentry.outermost()) {
        MTLRenderPassColorAttachmentDescriptor *color = descriptor.colorAttachments[0];
        Log("commandBuffer.renderCommandEncoderWithDescriptor: load=%lu store=%lu texture=%s -> %s",
            (unsigned long)color.loadAction, (unsigned long)color.storeAction,
            ClassName(color.texture), ClassName(encoder));
        g_encodersThisFrame++;
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
    }
    ((void (*)(id, SEL, id))Original(self, _cmd))(self, _cmd, drawable);
}

void Replaced_commit(id self, SEL _cmd) {
    Reentry reentry;
    if (reentry.outermost()) Log("commandBuffer.commit label=\"%s\"", LabelOf(self));
    ((void (*)(id, SEL))Original(self, _cmd))(self, _cmd);
}

// --------------------------------------------------------------------------------------------
// MTLRenderCommandEncoder

void Replaced_setRenderPipelineState(id self, SEL _cmd, id state) {
    Reentry reentry;
    if (reentry.outermost()) Log("  encoder.setRenderPipelineState: %s", ClassName(state));
    ((void (*)(id, SEL, id))Original(self, _cmd))(self, _cmd, state);
}

void Replaced_setVertexBuffer(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger index) {
    Reentry reentry;
    if (reentry.outermost()) {
        Log("  encoder.setVertexBuffer:\"%s\" offset:%lu atIndex:%lu", LabelOf(buffer),
            (unsigned long)offset, (unsigned long)index);
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
    }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger))Original(
        self, _cmd))(self, _cmd, type, indexCount, indexType, indexBuffer, offset, instances);
}

void Replaced_endEncoding(id self, SEL _cmd) {
    Reentry reentry;
    if (reentry.outermost()) Log("  encoder.endEncoding (%s \"%s\")", ClassName(self), LabelOf(self));
    ((void (*)(id, SEL))Original(self, _cmd))(self, _cmd);
}

// --------------------------------------------------------------------------------------------
// MTLComputeCommandEncoder

void Replaced_setComputePipelineState(id self, SEL _cmd, id state) {
    Reentry reentry;
    if (reentry.outermost()) Log("  encoder.setComputePipelineState: %s", ClassName(state));
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
    }
    ((void (*)(id, SEL, MTLSize, MTLSize))Original(self, _cmd))(self, _cmd, groups, perGroup);
}

}  // namespace

// --------------------------------------------------------------------------------------------
// Installation. Each is called with every object of its kind, not only new ones, so each stops at
// the first sighting of a class; Hook() is idempotent besides.

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
    Hook(cls, @selector(newLibraryWithSource:options:error:), (IMP)Replaced_newLibraryWithSource);
}

void HookCommandQueueClass(id queue) {
    Class cls = object_getClass(queue);
    if (!FirstSighting(cls)) return;
    Log("hooking command queue class %s", class_getName(cls));
    Hook(cls, @selector(commandBuffer), (IMP)Replaced_commandBuffer);
}

void HookCommandBufferClass(id commandBuffer) {
    Class cls = object_getClass(commandBuffer);
    if (!FirstSighting(cls)) return;
    Log("hooking command buffer class %s", class_getName(cls));
    Hook(cls, @selector(renderCommandEncoderWithDescriptor:), (IMP)Replaced_renderCommandEncoder);
    Hook(cls, @selector(computeCommandEncoder), (IMP)Replaced_computeCommandEncoder);
    Hook(cls, @selector(blitCommandEncoder), (IMP)Replaced_blitCommandEncoder);
    Hook(cls, @selector(presentDrawable:), (IMP)Replaced_presentDrawable);
    Hook(cls, @selector(commit), (IMP)Replaced_commit);
}

void HookRenderEncoderClass(id encoder) {
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking render encoder class %s", class_getName(cls));
    Hook(cls, @selector(setRenderPipelineState:), (IMP)Replaced_setRenderPipelineState);
    Hook(cls, @selector(setVertexBuffer:offset:atIndex:), (IMP)Replaced_setVertexBuffer);
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
