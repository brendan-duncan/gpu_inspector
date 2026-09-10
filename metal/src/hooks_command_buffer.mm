// Hooks on command queues, command buffers and the parallel render encoder: where passes begin,
// where frames end. See hooks_common.h for the shape every hook has.
#include "hooks.h"
#include "hooks_common.h"

namespace mtlinsp {
namespace {

// --------------------------------------------------------------------------------------------
// MTLCommandQueue

id Q_commandBuffer(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id commandBuffer = ORIG(id (*)(id, SEL))(self, _cmd);
    if (reentry.outermost()) Log("queue.%s -> %s", sel_getName(_cmd), ClassName(commandBuffer));
    HookCommandBufferClass(commandBuffer);
    return commandBuffer;
}

id Q_commandBufferWithDescriptor(id self, SEL _cmd, id descriptor) {
    Reentry reentry(self, _cmd);
    id commandBuffer = ORIG(id (*)(id, SEL, id))(self, _cmd, descriptor);
    if (reentry.outermost()) Log("queue.commandBufferWithDescriptor: -> %s", ClassName(commandBuffer));
    HookCommandBufferClass(commandBuffer);
    return commandBuffer;
}

// --------------------------------------------------------------------------------------------
// MTLCommandBuffer: encoders

/**
 * An attachment with storeAction DontCare has undefined contents after the pass, so during a
 * capture the store is forced on — the counterpart of the layer's store-everything render pass,
 * and far less work because Metal's store action is a mutable field. Only where a store is
 * possible: a memoryless attachment cannot be stored, a multisample one is read through its
 * resolve, and an Unknown action is decided later by the encoder.
 */
void ForceStore(MTLRenderPassAttachmentDescriptor *a) {
    if (a == nil || a.texture == nil || a.storeAction != MTLStoreActionDontCare) return;
    if (a.texture.storageMode == MTLStorageModeMemoryless || a.texture.sampleCount > 1) return;
    a.storeAction = MTLStoreActionStore;
}

id CreateRenderEncoder(id self, SEL _cmd, MTLRenderPassDescriptor *descriptor, Reentry &reentry,
                       const char *method, bool parallel) {
    const bool outermost = reentry.outermost();
    const bool rec = outermost && Recording();
    // The application's own descriptor is left alone: it owns and reuses that object. The copy
    // is what carries the forced stores and the timestamp attachment.
    MTLRenderPassDescriptor *pass = descriptor;
    PassTimingSlot timing;
    if (rec && descriptor != nil) {
        pass = [descriptor copy];
        for (NSUInteger i = 0; i < 8; i++) ForceStore(pass.colorAttachments[i]);
        ForceStore(pass.depthAttachment);
        timing = ReserveRenderPassTiming(self, pass);
    }
    id encoder = ORIG(id (*)(id, SEL, MTLRenderPassDescriptor *))(self, _cmd, pass);
    if (outermost) {
        g_encodersThisFrame++;
        RegisterEncoder(encoder, self,
                        parallel ? "MTLParallelRenderCommandEncoder" : "MTLRenderCommandEncoder",
                        nil);
        // Recording or not: which command buffer draws into the drawable decides where a frame
        // ends for an application that presents the drawable itself.
        for (NSUInteger i = 0; i < 8 && descriptor != nil; i++) {
            id texture = descriptor.colorAttachments[i].texture;
            if (texture != nil) OnRenderTarget(self, texture);
        }
        BeginPass(encoder, self, PassKind::Render, timing);
        if (rec) {
            for (NSUInteger i = 0; i < 8; i++) {
                if (pass.colorAttachments[i].texture != nil) {
                    AddPassAttachment(encoder, pass.colorAttachments[i], (uint32_t)i, false);
                }
            }
            if (pass.depthAttachment.texture != nil) {
                AddPassAttachment(encoder, pass.depthAttachment, 0, true);
            }
            RecordCommand(method, encoder, RenderPassArgs(descriptor));
        }
        if (LogEnabled() && descriptor != nil) {
            MTLRenderPassColorAttachmentDescriptor *color = descriptor.colorAttachments[0];
            Log("commandBuffer.%s load=%lu store=%lu texture=%s -> %s", method,
                (unsigned long)color.loadAction, (unsigned long)color.storeAction,
                ClassName(color.texture), ClassName(encoder));
        }
    }
    if (pass != descriptor) [pass release];
    if (parallel) HookParallelEncoderClass(encoder);
    else HookRenderEncoderClass(encoder);
    return encoder;
}

id CB_renderCommandEncoder(id self, SEL _cmd, MTLRenderPassDescriptor *descriptor) {
    Reentry reentry(self, _cmd);
    return CreateRenderEncoder(self, _cmd, descriptor, reentry, "renderCommandEncoderWithDescriptor:",
                               false);
}

id CB_parallelRenderCommandEncoder(id self, SEL _cmd, MTLRenderPassDescriptor *descriptor) {
    Reentry reentry(self, _cmd);
    return CreateRenderEncoder(self, _cmd, descriptor, reentry,
                               "parallelRenderCommandEncoderWithDescriptor:", true);
}

enum class ComputeForm { Plain, DispatchType, Descriptor };

/**
 * The three ways to open a compute encoder, with one twist while recording: a compute pass takes
 * its timestamp samples through a pass descriptor, which the plain forms do not have. So while
 * recording, on a device that samples at stage boundaries, the plain forms are opened through
 * computeCommandEncoderWithDescriptor: with a descriptor saying the same thing plus the samples.
 * The command is recorded under the selector the application called.
 */
id CreateComputeEncoder(id self, SEL _cmd, Reentry &reentry, ComputeForm form,
                        MTLDispatchType dispatchType, id descriptor) {
    const bool outermost = reentry.outermost();
    const bool rec = outermost && Recording();
    PassTimingSlot timing;
    id encoder = nil;
    id ours = nil;
    if (rec) {
        if (@available(macOS 11.0, *)) {
            MTLComputePassDescriptor *pass = nil;
            if (form == ComputeForm::Descriptor) {
                pass = [(MTLComputePassDescriptor *)descriptor copy];
            } else {
                pass = [[MTLComputePassDescriptor alloc] init];
                pass.dispatchType = form == ComputeForm::DispatchType ? dispatchType : MTLDispatchTypeSerial;
            }
            ours = pass;
            timing = ReserveComputePassTiming(self, pass);
            if (form == ComputeForm::Descriptor) {
                encoder = ORIG(id (*)(id, SEL, MTLComputePassDescriptor *))(self, _cmd, pass);
            } else if (timing.sampleBuffer != nil && !timing.onEncoder) {
                // Through the hook for that selector, which is nested and so only forwards.
                encoder = [(id<MTLCommandBuffer>)self computeCommandEncoderWithDescriptor:pass];
            }
        }
    }
    if (encoder == nil) {
        switch (form) {
            case ComputeForm::Plain:
                encoder = ORIG(id (*)(id, SEL))(self, _cmd);
                break;
            case ComputeForm::DispatchType:
                encoder = ORIG(id (*)(id, SEL, MTLDispatchType))(self, _cmd, dispatchType);
                break;
            case ComputeForm::Descriptor:
                encoder = ORIG(id (*)(id, SEL, id))(self, _cmd, descriptor);
                break;
        }
    }
    [ours release];
    if (outermost) {
        g_encodersThisFrame++;
        RegisterEncoder(encoder, self, "MTLComputeCommandEncoder", nil);
        BeginPass(encoder, self, PassKind::Compute, timing);
        if (rec) {
            Args a;
            if (form != ComputeForm::Plain) a.u("dispatchType", (uint64_t)dispatchType);
            RecordCommand(sel_getName(_cmd), encoder, a.str());
        }
        Log("commandBuffer.%s -> %s", sel_getName(_cmd), ClassName(encoder));
    }
    HookComputeEncoderClass(encoder);
    return encoder;
}

id CB_computeCommandEncoder(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    return CreateComputeEncoder(self, _cmd, reentry, ComputeForm::Plain, MTLDispatchTypeSerial, nil);
}

id CB_computeCommandEncoderWithDispatchType(id self, SEL _cmd, MTLDispatchType dispatchType) {
    Reentry reentry(self, _cmd);
    return CreateComputeEncoder(self, _cmd, reentry, ComputeForm::DispatchType, dispatchType, nil);
}

id CB_computeCommandEncoderWithDescriptor(id self, SEL _cmd, id descriptor) {
    Reentry reentry(self, _cmd);
    MTLDispatchType type = MTLDispatchTypeSerial;
    if (@available(macOS 11.0, *)) {
        if (descriptor != nil) type = ((MTLComputePassDescriptor *)descriptor).dispatchType;
    }
    return CreateComputeEncoder(self, _cmd, reentry, ComputeForm::Descriptor, type, descriptor);
}

id CreateBlitEncoder(id self, SEL _cmd, Reentry &reentry, bool withDescriptor, id descriptor) {
    const bool outermost = reentry.outermost();
    const bool rec = outermost && Recording();
    PassTimingSlot timing;
    id encoder = nil;
    id ours = nil;
    if (rec) {
        if (@available(macOS 11.0, *)) {
            MTLBlitPassDescriptor *pass = withDescriptor ? [(MTLBlitPassDescriptor *)descriptor copy]
                                                         : [[MTLBlitPassDescriptor alloc] init];
            ours = pass;
            timing = ReserveBlitPassTiming(self, pass);
            if (withDescriptor) {
                encoder = ORIG(id (*)(id, SEL, MTLBlitPassDescriptor *))(self, _cmd, pass);
            } else if (timing.sampleBuffer != nil && !timing.onEncoder) {
                encoder = [(id<MTLCommandBuffer>)self blitCommandEncoderWithDescriptor:pass];
            }
        }
    }
    if (encoder == nil) {
        encoder = withDescriptor ? ORIG(id (*)(id, SEL, id))(self, _cmd, descriptor)
                                 : ORIG(id (*)(id, SEL))(self, _cmd);
    }
    [ours release];
    if (outermost) {
        g_encodersThisFrame++;
        RegisterEncoder(encoder, self, "MTLBlitCommandEncoder", nil);
        BeginPass(encoder, self, PassKind::Blit, timing);
        if (rec) RecordCommand(sel_getName(_cmd), encoder, {});
        Log("commandBuffer.%s -> %s", sel_getName(_cmd), ClassName(encoder));
    }
    HookBlitEncoderClass(encoder);
    return encoder;
}

id CB_blitCommandEncoder(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    return CreateBlitEncoder(self, _cmd, reentry, false, nil);
}

id CB_blitCommandEncoderWithDescriptor(id self, SEL _cmd, id descriptor) {
    Reentry reentry(self, _cmd);
    return CreateBlitEncoder(self, _cmd, reentry, true, descriptor);
}

/** Resource state and acceleration structure encoders: a pass, without recorded commands yet. */
id CreateOtherEncoder(id self, SEL _cmd, Reentry &reentry, id encoder, const char *type) {
    if (reentry.outermost()) {
        g_encodersThisFrame++;
        RegisterEncoder(encoder, self, type, nil);
        BeginPass(encoder, self, PassKind::Other, PassTimingSlot());
        if (Recording()) RecordCommand(sel_getName(_cmd), encoder, {});
    }
    HookOtherEncoderClass(encoder);
    return encoder;
}

id CB_resourceStateCommandEncoder(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id encoder = ORIG(id (*)(id, SEL))(self, _cmd);
    return CreateOtherEncoder(self, _cmd, reentry, encoder, "MTLResourceStateCommandEncoder");
}

id CB_resourceStateCommandEncoderWithDescriptor(id self, SEL _cmd, id descriptor) {
    Reentry reentry(self, _cmd);
    id encoder = ORIG(id (*)(id, SEL, id))(self, _cmd, descriptor);
    return CreateOtherEncoder(self, _cmd, reentry, encoder, "MTLResourceStateCommandEncoder");
}

id CB_accelerationStructureCommandEncoder(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id encoder = ORIG(id (*)(id, SEL))(self, _cmd);
    return CreateOtherEncoder(self, _cmd, reentry, encoder, "MTLAccelerationStructureCommandEncoder");
}

id CB_accelerationStructureCommandEncoderWithDescriptor(id self, SEL _cmd, id descriptor) {
    Reentry reentry(self, _cmd);
    id encoder = ORIG(id (*)(id, SEL, id))(self, _cmd, descriptor);
    return CreateOtherEncoder(self, _cmd, reentry, encoder, "MTLAccelerationStructureCommandEncoder");
}

// --------------------------------------------------------------------------------------------
// MTLCommandBuffer: presenting, committing, and the rest

void LogFrame(id self, id drawable, const char *how) {
    Log("--- frame %llu: %u encoders, %u draws, %u dispatches (%s presents %s, %s) ---",
        (unsigned long long)g_frame++, g_encodersThisFrame.exchange(0),
        g_drawsThisFrame.exchange(0), g_dispatchesThisFrame.exchange(0), ClassName(self),
        ClassName(drawable), how);
}

/** The presented texture, so the UI's present row resolves to the image the frame ended on. */
std::string PresentArgs(id drawable, double timeValue, const char *timeKey) {
    Args a;
    id texture = [drawable respondsToSelector:@selector(texture)]
        ? [drawable performSelector:@selector(texture)] : nil;
    a.ref("texture", texture, "MTLTexture");
    if ([drawable respondsToSelector:@selector(drawableID)]) {
        a.u("drawableID", (uint64_t)[(id<MTLDrawable>)drawable drawableID]);
    }
    if (timeKey != nullptr) a.d(timeKey, timeValue);
    return a.str();
}

void CB_presentDrawable(id self, SEL _cmd, id drawable) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        LogFrame(self, drawable, "presentDrawable:");
        if (Recording()) RecordCommand("presentDrawable:", self, PresentArgs(drawable, 0, nullptr));
        // Not the frame boundary itself: the commit that follows is. See OnCommit.
        OnPresentDrawable(self, drawable);
    }
    ORIG(void (*)(id, SEL, id))(self, _cmd, drawable);
}

void CB_presentDrawableAtTime(id self, SEL _cmd, id drawable, CFTimeInterval time) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        LogFrame(self, drawable, "presentDrawable:atTime:");
        if (Recording()) {
            RecordCommand("presentDrawable:atTime:", self, PresentArgs(drawable, time, "presentationTime"));
        }
        OnPresentDrawable(self, drawable);
    }
    ORIG(void (*)(id, SEL, id, CFTimeInterval))(self, _cmd, drawable, time);
}

void CB_presentDrawableAfterMinimumDuration(id self, SEL _cmd, id drawable, CFTimeInterval duration) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        LogFrame(self, drawable, "presentDrawable:afterMinimumDuration:");
        if (Recording()) {
            RecordCommand("presentDrawable:afterMinimumDuration:", self,
                          PresentArgs(drawable, duration, "duration"));
        }
        OnPresentDrawable(self, drawable);
    }
    ORIG(void (*)(id, SEL, id, CFTimeInterval))(self, _cmd, drawable, duration);
}

void CB_commit(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        Log("commandBuffer.commit label=\"%s\"", LabelOf(self));
        // Records the commit, drives the capture state machine: arms, counts a frame, or
        // finishes and sends.
        OnCommit(self);
    }
    ORIG(void (*)(id, SEL))(self, _cmd);
}

void CB_enqueue(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("enqueue", self, {});
    ORIG(void (*)(id, SEL))(self, _cmd);
}

void CB_waitUntilScheduled(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("waitUntilScheduled", self, {});
    ORIG(void (*)(id, SEL))(self, _cmd);
}

void CB_waitUntilCompleted(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("waitUntilCompleted", self, {});
    ORIG(void (*)(id, SEL))(self, _cmd);
}

void CB_encodeWaitForEvent(id self, SEL _cmd, id event, uint64_t value) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("encodeWaitForEvent:value:", self,
                      Args().ref("event", event, "MTLEvent").u("value", value).str());
    }
    ORIG(void (*)(id, SEL, id, uint64_t))(self, _cmd, event, value);
}

void CB_encodeSignalEvent(id self, SEL _cmd, id event, uint64_t value) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("encodeSignalEvent:value:", self,
                      Args().ref("event", event, "MTLEvent").u("value", value).str());
    }
    ORIG(void (*)(id, SEL, id, uint64_t))(self, _cmd, event, value);
}

void CB_pushDebugGroup(id self, SEL _cmd, NSString *name) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("pushDebugGroup:", self, Args().s("label", name).str());
    ORIG(void (*)(id, SEL, NSString *))(self, _cmd, name);
}

void CB_popDebugGroup(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("popDebugGroup", self, {});
    ORIG(void (*)(id, SEL))(self, _cmd);
}

// --------------------------------------------------------------------------------------------
// MTLParallelRenderCommandEncoder
//
// The pass is the parallel encoder's; the sub-encoders it hands out share the pass, so their
// creation and end are recorded as ordinary commands rather than as passes, and the read-back
// happens when the parallel encoder itself ends.

id P_renderCommandEncoder(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    id encoder = ORIG(id (*)(id, SEL))(self, _cmd);
    if (reentry.outermost()) {
        bool secondary = false;
        id commandBuffer = EncoderCommandBuffer(self, &secondary);
        RegisterEncoder(encoder, commandBuffer, "MTLRenderCommandEncoder", self);
        if (Recording()) RecordCommand("renderCommandEncoder", encoder, {});
    }
    HookRenderEncoderClass(encoder);
    return encoder;
}

void P_setColorStoreAction(id self, SEL _cmd, MTLStoreAction action, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setColorStoreAction:atIndex:", self,
                      Args().e("storeAction", StoreActionEnumName(action), (uint64_t)action)
                            .u("index", index).str());
    }
    ORIG(void (*)(id, SEL, MTLStoreAction, NSUInteger))(self, _cmd, action, index);
}

void P_setDepthStoreAction(id self, SEL _cmd, MTLStoreAction action) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setDepthStoreAction:", self,
                      Args().e("storeAction", StoreActionEnumName(action), (uint64_t)action).str());
    }
    ORIG(void (*)(id, SEL, MTLStoreAction))(self, _cmd, action);
}

void P_setStencilStoreAction(id self, SEL _cmd, MTLStoreAction action) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setStencilStoreAction:", self,
                      Args().e("storeAction", StoreActionEnumName(action), (uint64_t)action).str());
    }
    ORIG(void (*)(id, SEL, MTLStoreAction))(self, _cmd, action);
}

}  // namespace

// --------------------------------------------------------------------------------------------

void HookCommandQueueClass(id queue) {
    if (queue == nil) return;
    Class cls = object_getClass(queue);
    if (!FirstSighting(cls)) return;
    Log("hooking command queue class %s", class_getName(cls));
    Hook(cls, @selector(commandBuffer), (IMP)Q_commandBuffer);
    // Engines commonly take the unretained form for its lower overhead; without this nothing
    // below the queue is ever hooked.
    Hook(cls, @selector(commandBufferWithUnretainedReferences), (IMP)Q_commandBuffer);
    Hook(cls, @selector(commandBufferWithDescriptor:), (IMP)Q_commandBufferWithDescriptor);
}

void HookCommandBufferClass(id commandBuffer) {
    if (commandBuffer == nil) return;
    Class cls = object_getClass(commandBuffer);
    if (!FirstSighting(cls)) return;
    Log("hooking command buffer class %s", class_getName(cls));
    Hook(cls, @selector(renderCommandEncoderWithDescriptor:), (IMP)CB_renderCommandEncoder);
    Hook(cls, @selector(parallelRenderCommandEncoderWithDescriptor:),
         (IMP)CB_parallelRenderCommandEncoder);
    Hook(cls, @selector(computeCommandEncoder), (IMP)CB_computeCommandEncoder);
    Hook(cls, @selector(computeCommandEncoderWithDispatchType:),
         (IMP)CB_computeCommandEncoderWithDispatchType);
    Hook(cls, @selector(computeCommandEncoderWithDescriptor:),
         (IMP)CB_computeCommandEncoderWithDescriptor);
    Hook(cls, @selector(blitCommandEncoder), (IMP)CB_blitCommandEncoder);
    Hook(cls, @selector(blitCommandEncoderWithDescriptor:), (IMP)CB_blitCommandEncoderWithDescriptor);
    Hook(cls, @selector(resourceStateCommandEncoder), (IMP)CB_resourceStateCommandEncoder);
    Hook(cls, @selector(resourceStateCommandEncoderWithDescriptor:),
         (IMP)CB_resourceStateCommandEncoderWithDescriptor);
    Hook(cls, @selector(accelerationStructureCommandEncoder),
         (IMP)CB_accelerationStructureCommandEncoder);
    Hook(cls, sel_registerName("accelerationStructureCommandEncoderWithDescriptor:"),
         (IMP)CB_accelerationStructureCommandEncoderWithDescriptor);
    Hook(cls, @selector(presentDrawable:), (IMP)CB_presentDrawable);
    Hook(cls, @selector(presentDrawable:atTime:), (IMP)CB_presentDrawableAtTime);
    Hook(cls, @selector(presentDrawable:afterMinimumDuration:),
         (IMP)CB_presentDrawableAfterMinimumDuration);
    Hook(cls, @selector(commit), (IMP)CB_commit);
    Hook(cls, @selector(enqueue), (IMP)CB_enqueue);
    Hook(cls, @selector(waitUntilScheduled), (IMP)CB_waitUntilScheduled);
    Hook(cls, @selector(waitUntilCompleted), (IMP)CB_waitUntilCompleted);
    Hook(cls, @selector(encodeWaitForEvent:value:), (IMP)CB_encodeWaitForEvent);
    Hook(cls, @selector(encodeSignalEvent:value:), (IMP)CB_encodeSignalEvent);
    Hook(cls, @selector(pushDebugGroup:), (IMP)CB_pushDebugGroup);
    Hook(cls, @selector(popDebugGroup), (IMP)CB_popDebugGroup);
    Hook(cls, @selector(setLabel:), (IMP)Replaced_setLabel);
}

void HookParallelEncoderClass(id encoder) {
    if (encoder == nil) return;
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking parallel render encoder class %s", class_getName(cls));
    Hook(cls, @selector(renderCommandEncoder), (IMP)P_renderCommandEncoder);
    Hook(cls, @selector(setColorStoreAction:atIndex:), (IMP)P_setColorStoreAction);
    Hook(cls, @selector(setDepthStoreAction:), (IMP)P_setDepthStoreAction);
    Hook(cls, @selector(setStencilStoreAction:), (IMP)P_setStencilStoreAction);
    HookCommonEncoderMethods(cls);
}

}  // namespace mtlinsp
