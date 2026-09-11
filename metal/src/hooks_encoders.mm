// Hooks on the encoders: MTLRenderCommandEncoder, MTLComputeCommandEncoder,
// MTLBlitCommandEncoder, and what every encoder shares. See hooks_common.h for the shape every
// hook has.
//
// Metal has one selector per overload rather than optional arguments, so each has to be hooked
// separately; a real renderer uses the base-vertex/base-instance forms that a hand-written sample
// never does. RenderDoc's metal_render_command_encoder_bridge.mm enumerates the protocol in full
// and is the checklist this was written against.
#include "hooks.h"
#include "hooks_common.h"
#include "overdraw.h"

#import <objc/message.h>

#include <vector>

namespace mtlinsp {
namespace {

// Overdraw (overdraw.h): while a capture that measures it is recording, every render encoder call
// that shapes what a pass rasterizes is also recorded as a closure, which the measurement issues
// again against its own encoder. Bindings of every stage are kept, since a counting pipeline keeps
// the application's vertex stage; store actions, visibility results, fences and barriers are not,
// since they have no bearing on the count.

/** Whether this hook invocation is the application's call and a capture that measures overdraw is recording. */
inline bool Measuring(const Reentry &reentry) { return reentry.outermost() && OverdrawActive(); }

std::vector<uint8_t> CopyBytes(const void *bytes, NSUInteger length) {
    const uint8_t *b = static_cast<const uint8_t *>(bytes);
    return b != nullptr ? std::vector<uint8_t>(b, b + length) : std::vector<uint8_t>(length, 0);
}

std::vector<NSUInteger> CopyOffsets(const NSUInteger *offsets, NSUInteger count) {
    return offsets != nullptr ? std::vector<NSUInteger>(offsets, offsets + count) : std::vector<NSUInteger>(count, 0);
}

/** Inline bytes bound as a constant block: recorded the way the UI reads push constants. */
std::string BytesArgs(const char *stage, const void *bytes, NSUInteger length, NSUInteger index) {
    Args a;
    a.c("stageFlags", stage).u("offset", 0).u("size", length).u("index", index)
     .bytes("pValues", bytes, length);
    return a.str();
}

/** The read-back ids for a range of buffers bound in one call, one per slot. */
std::vector<uint64_t> QueueBuffers(id encoder, const id *buffers, const NSUInteger *offsets,
                                   NSRange range) {
    std::vector<uint64_t> data;
    data.reserve(range.length);
    for (NSUInteger n = 0; n < range.length; n++) {
        data.push_back(QueueBufferCapture(encoder, buffers == nullptr ? nil : buffers[n],
                                          offsets == nullptr ? 0 : offsets[n], 0));
    }
    return data;
}

// --------------------------------------------------------------------------------------------
// Every encoder: ending, debug groups

void E_endEncoding(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    const bool outermost = reentry.outermost();
    bool secondary = false;
    if (outermost) {
        EncoderCommandBuffer(self, &secondary);
        Log("  encoder.endEncoding (%s \"%s\")", ClassName(self), LabelOf(self));
        // A parallel encoder's sub-encoder shares its parent's pass; its end is not the pass's.
        if (Recording() && !secondary) RecordCommand("endEncoding", self, {});
        BeforeEndEncoding(self);
    }
    ORIG(void (*)(id, SEL))(self, _cmd);
    // Only after the forward: a command buffer allows one encoder at a time, and until the
    // application's is really closed the read-back's blit encoder cannot be created.
    if (outermost) {
        AfterEndEncoding(self);
        // The pass drawn again to count its overdraw, after the read-back of what it drew.
        if (!secondary) EndOverdrawPass(self);
        ForgetEncoder(self);
    }
}

void E_pushDebugGroup(id self, SEL _cmd, NSString *name) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("pushDebugGroup:", self, Args().s("label", name).str());
    ORIG(void (*)(id, SEL, NSString *))(self, _cmd, name);
}

void E_popDebugGroup(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("popDebugGroup", self, {});
    ORIG(void (*)(id, SEL))(self, _cmd);
}

void E_insertDebugSignpost(id self, SEL _cmd, NSString *name) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("insertDebugSignpost:", self, Args().s("label", name).str());
    ORIG(void (*)(id, SEL, NSString *))(self, _cmd, name);
}

// --------------------------------------------------------------------------------------------
// MTLRenderCommandEncoder: pipeline and vertex stage

void R_setRenderPipelineState(id self, SEL _cmd, id state) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setRenderPipelineState:", self,
                      Args().ref("pipeline", state, "MTLRenderPipelineState").str());
    }
    if (Measuring(reentry)) {
        LogOverdrawOp(self, [s = Strong(state)](id<MTLRenderCommandEncoder> e, OverdrawReplay &r) {
            r.BindPipeline(e, s.get());
        });
    }
    ORIG(void (*)(id, SEL, id))(self, _cmd, state);
}

// The binding forms every stage shares, issued again by selector: the vertex, fragment, object
// and mesh stages take the same arguments under different names.

void LogBytes(id self, SEL sel, const void *bytes, NSUInteger length, NSUInteger index) {
    LogOverdrawOp(self, [sel, data = CopyBytes(bytes, length), index](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))objc_msgSend)(e, sel, data.data(), data.size(), index);
    });
}

void LogBuffer(id self, SEL sel, id buffer, NSUInteger offset, NSUInteger index) {
    LogOverdrawOp(self, [sel, b = Strong(buffer), offset, index](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, id, NSUInteger, NSUInteger))objc_msgSend)(e, sel, b.get(), offset, index);
    });
}

void LogIndexed(id self, SEL sel, NSUInteger first, NSUInteger second) {
    LogOverdrawOp(self, [sel, first, second](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(e, sel, first, second);
    });
}

void LogBuffers(id self, SEL sel, const id *buffers, const NSUInteger *offsets, NSRange range) {
    LogOverdrawOp(self, [sel, list = StrongList(buffers, range.length), offs = CopyOffsets(offsets, range.length),
                         range](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, const id *, const NSUInteger *, NSRange))objc_msgSend)(e, sel, list.data(), offs.data(), range);
    });
}

void LogObject(id self, SEL sel, id object, NSUInteger index) {
    LogOverdrawOp(self, [sel, o = Strong(object), index](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, id, NSUInteger))objc_msgSend)(e, sel, o.get(), index);
    });
}

void LogObjects(id self, SEL sel, const id *objects, NSRange range) {
    LogOverdrawOp(self, [sel, list = StrongList(objects, range.length), range](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, const id *, NSRange))objc_msgSend)(e, sel, list.data(), range);
    });
}

void LogSamplerLod(id self, SEL sel, id sampler, float lodMin, float lodMax, NSUInteger index) {
    LogOverdrawOp(self, [sel, s = Strong(sampler), lodMin, lodMax, index](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, id, float, float, NSUInteger))objc_msgSend)(e, sel, s.get(), lodMin, lodMax, index);
    });
}

void LogUint(id self, SEL sel, NSUInteger value) {
    LogOverdrawOp(self, [sel, value](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, NSUInteger))objc_msgSend)(e, sel, value);
    });
}

// Residency: useResource:, useHeap: and their array and stage forms.

void LogUse(id self, SEL sel, id object) {
    LogOverdrawOp(self, [sel, o = Strong(object)](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, id))objc_msgSend)(e, sel, o.get());
    });
}

void LogUse(id self, SEL sel, id object, NSUInteger a) {
    LogOverdrawOp(self, [sel, o = Strong(object), a](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, id, NSUInteger))objc_msgSend)(e, sel, o.get(), a);
    });
}

void LogUse(id self, SEL sel, id object, NSUInteger a, NSUInteger b) {
    LogOverdrawOp(self, [sel, o = Strong(object), a, b](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, id, NSUInteger, NSUInteger))objc_msgSend)(e, sel, o.get(), a, b);
    });
}

void LogUseList(id self, SEL sel, const id *objects, NSUInteger count) {
    LogOverdrawOp(self, [sel, list = StrongList(objects, count), count](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, const id *, NSUInteger))objc_msgSend)(e, sel, list.data(), count);
    });
}

void LogUseList(id self, SEL sel, const id *objects, NSUInteger count, NSUInteger a) {
    LogOverdrawOp(self, [sel, list = StrongList(objects, count), count, a](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, const id *, NSUInteger, NSUInteger))objc_msgSend)(e, sel, list.data(), count, a);
    });
}

void LogUseList(id self, SEL sel, const id *objects, NSUInteger count, NSUInteger a, NSUInteger b) {
    LogOverdrawOp(self, [sel, list = StrongList(objects, count), count, a, b](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
        ((void (*)(id, SEL, const id *, NSUInteger, NSUInteger, NSUInteger))objc_msgSend)(e, sel, list.data(), count, a, b);
    });
}

/** A draw: issued when a counting pipeline is bound. */
void LogDraw(id self, std::function<void(id<MTLRenderCommandEncoder>)> draw) {
    LogOverdrawOp(self, [draw = std::move(draw)](id<MTLRenderCommandEncoder> e, OverdrawReplay &r) {
        if (r.Draw()) draw(e);
    });
}

void R_setVertexBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setVertexBytes:length:atIndex:", self,
                                 BytesArgs("vertex", bytes, length, index),
                                 {QueueBytesCapture(bytes, length)});
    }
    if (Measuring(reentry)) LogBytes(self, _cmd, bytes, length, index);
    ORIG(void (*)(id, SEL, const void *, NSUInteger, NSUInteger))(self, _cmd, bytes, length, index);
}

void R_setVertexBuffer(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        // From the offset to the end of the buffer: Metal does not say how much a draw will
        // read, and the stride is in the pipeline's vertex descriptor, not the binding.
        RecordCommandWithBuffers("setVertexBuffer:offset:atIndex:", self,
                                 Args().ref("buffer", buffer, "MTLBuffer").u("offset", offset)
                                       .u("index", index).str(),
                                 {QueueBufferCapture(self, buffer, offset, 0)});
    }
    if (Measuring(reentry)) LogBuffer(self, _cmd, buffer, offset, index);
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, buffer, offset, index);
}

void R_setVertexBufferOffset(id self, SEL _cmd, NSUInteger offset, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setVertexBufferOffset:atIndex:", self,
                      Args().u("offset", offset).u("index", index).str());
    }
    if (Measuring(reentry)) LogIndexed(self, _cmd, offset, index);
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger))(self, _cmd, offset, index);
}

void R_setVertexBuffers(id self, SEL _cmd, const id *buffers, const NSUInteger *offsets, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setVertexBuffers:offsets:withRange:", self,
                                 Args().refs("buffers", buffers, range.length, "MTLBuffer")
                                       .uints("offsets", offsets, range.length).range("range", range).str(),
                                 QueueBuffers(self, buffers, offsets, range));
    }
    if (Measuring(reentry)) LogBuffers(self, _cmd, buffers, offsets, range);
    ORIG(void (*)(id, SEL, const id *, const NSUInteger *, NSRange))(self, _cmd, buffers, offsets, range);
}

void R_setVertexTexture(id self, SEL _cmd, id texture, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setVertexTexture:atIndex:", self,
                      Args().ref("texture", texture, "MTLTexture").u("index", index).str());
    }
    if (Measuring(reentry)) LogObject(self, _cmd, texture, index);
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, texture, index);
}

void R_setVertexTextures(id self, SEL _cmd, const id *textures, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setVertexTextures:withRange:", self,
                      Args().refs("textures", textures, range.length, "MTLTexture").range("range", range).str());
    }
    if (Measuring(reentry)) LogObjects(self, _cmd, textures, range);
    ORIG(void (*)(id, SEL, const id *, NSRange))(self, _cmd, textures, range);
}

void R_setVertexSamplerState(id self, SEL _cmd, id sampler, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setVertexSamplerState:atIndex:", self,
                      Args().ref("sampler", sampler, "MTLSamplerState").u("index", index).str());
    }
    if (Measuring(reentry)) LogObject(self, _cmd, sampler, index);
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, sampler, index);
}

void R_setVertexSamplerStateLod(id self, SEL _cmd, id sampler, float lodMin, float lodMax, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:", self,
                      Args().ref("sampler", sampler, "MTLSamplerState").d("lodMinClamp", lodMin)
                            .d("lodMaxClamp", lodMax).u("index", index).str());
    }
    if (Measuring(reentry)) LogSamplerLod(self, _cmd, sampler, lodMin, lodMax, index);
    ORIG(void (*)(id, SEL, id, float, float, NSUInteger))(self, _cmd, sampler, lodMin, lodMax, index);
}

void R_setVertexSamplerStates(id self, SEL _cmd, const id *samplers, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setVertexSamplerStates:withRange:", self,
                      Args().refs("samplers", samplers, range.length, "MTLSamplerState").range("range", range).str());
    }
    if (Measuring(reentry)) LogObjects(self, _cmd, samplers, range);
    ORIG(void (*)(id, SEL, const id *, NSRange))(self, _cmd, samplers, range);
}

// --------------------------------------------------------------------------------------------
// MTLRenderCommandEncoder: fixed function

void R_setViewport(id self, SEL _cmd, MTLViewport viewport) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("setViewport:", self, Args().viewport("viewport", viewport).str());
    if (Measuring(reentry)) {
        LogOverdrawOp(self, [viewport](id<MTLRenderCommandEncoder> e, OverdrawReplay &) { [e setViewport:viewport]; });
    }
    ORIG(void (*)(id, SEL, MTLViewport))(self, _cmd, viewport);
}

void R_setViewports(id self, SEL _cmd, const MTLViewport *viewports, NSUInteger count) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        Args a;
        a.u("count", count);
        a.writer().Key("viewports");
        a.writer().BeginArray();
        for (NSUInteger n = 0; viewports != nullptr && n < count; n++) {
            Args v;
            v.viewport("viewport", viewports[n]);
            a.writer().Raw(v.str());
        }
        a.writer().EndArray();
        RecordCommand("setViewports:count:", self, a.str());
    }
    if (Measuring(reentry) && viewports != nullptr) {
        LogOverdrawOp(self, [list = std::vector<MTLViewport>(viewports, viewports + count)](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
            [e setViewports:list.data() count:list.size()];
        });
    }
    ORIG(void (*)(id, SEL, const MTLViewport *, NSUInteger))(self, _cmd, viewports, count);
}

void R_setFrontFacingWinding(id self, SEL _cmd, NSUInteger winding) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setFrontFacingWinding:", self,
                      Args().c("winding", winding == MTLWindingClockwise ? "MTLWindingClockwise"
                                                                          : "MTLWindingCounterClockwise").str());
    }
    if (Measuring(reentry)) LogUint(self, _cmd, winding);
    ORIG(void (*)(id, SEL, NSUInteger))(self, _cmd, winding);
}

void R_setVertexAmplificationCount(id self, SEL _cmd, NSUInteger count, const void *mappings) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setVertexAmplificationCount:viewMappings:", self, Args().u("count", count).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger, const void *))(self, _cmd, count, mappings);
}

void R_setCullMode(id self, SEL _cmd, NSUInteger mode) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        const char *name = mode == MTLCullModeNone ? "MTLCullModeNone"
                         : mode == MTLCullModeFront ? "MTLCullModeFront"
                         : mode == MTLCullModeBack ? "MTLCullModeBack" : "";
        RecordCommand("setCullMode:", self, Args().e("cullMode", name, mode).str());
    }
    if (Measuring(reentry)) LogUint(self, _cmd, mode);
    ORIG(void (*)(id, SEL, NSUInteger))(self, _cmd, mode);
}

void R_setDepthClipMode(id self, SEL _cmd, NSUInteger mode) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("setDepthClipMode:", self, Args().u("depthClipMode", mode).str());
    if (Measuring(reentry)) LogUint(self, _cmd, mode);
    ORIG(void (*)(id, SEL, NSUInteger))(self, _cmd, mode);
}

void R_setDepthBias(id self, SEL _cmd, float bias, float slopeScale, float clamp) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setDepthBias:slopeScale:clamp:", self,
                      Args().d("depthBias", bias).d("slopeScale", slopeScale).d("clamp", clamp).str());
    }
    if (Measuring(reentry)) {
        LogOverdrawOp(self, [bias, slopeScale, clamp](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
            [e setDepthBias:bias slopeScale:slopeScale clamp:clamp];
        });
    }
    ORIG(void (*)(id, SEL, float, float, float))(self, _cmd, bias, slopeScale, clamp);
}

void R_setScissorRect(id self, SEL _cmd, MTLScissorRect rect) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("setScissorRect:", self, Args().scissor("rect", rect).str());
    if (Measuring(reentry)) {
        LogOverdrawOp(self, [rect](id<MTLRenderCommandEncoder> e, OverdrawReplay &) { [e setScissorRect:rect]; });
    }
    ORIG(void (*)(id, SEL, MTLScissorRect))(self, _cmd, rect);
}

void R_setScissorRects(id self, SEL _cmd, const MTLScissorRect *rects, NSUInteger count) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        Args a;
        a.u("count", count);
        a.writer().Key("rects");
        a.writer().BeginArray();
        for (NSUInteger n = 0; rects != nullptr && n < count; n++) {
            Args r;
            r.scissor("rect", rects[n]);
            a.writer().Raw(r.str());
        }
        a.writer().EndArray();
        RecordCommand("setScissorRects:count:", self, a.str());
    }
    if (Measuring(reentry) && rects != nullptr) {
        LogOverdrawOp(self, [list = std::vector<MTLScissorRect>(rects, rects + count)](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
            [e setScissorRects:list.data() count:list.size()];
        });
    }
    ORIG(void (*)(id, SEL, const MTLScissorRect *, NSUInteger))(self, _cmd, rects, count);
}

void R_setTriangleFillMode(id self, SEL _cmd, NSUInteger mode) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setTriangleFillMode:", self,
                      Args().c("fillMode", mode == MTLTriangleFillModeLines ? "MTLTriangleFillModeLines"
                                                                             : "MTLTriangleFillModeFill").str());
    }
    if (Measuring(reentry)) LogUint(self, _cmd, mode);
    ORIG(void (*)(id, SEL, NSUInteger))(self, _cmd, mode);
}

void R_setBlendColor(id self, SEL _cmd, float red, float green, float blue, float alpha) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setBlendColorRed:green:blue:alpha:", self,
                      Args().d("red", red).d("green", green).d("blue", blue).d("alpha", alpha).str());
    }
    ORIG(void (*)(id, SEL, float, float, float, float))(self, _cmd, red, green, blue, alpha);
}

void R_setDepthStencilState(id self, SEL _cmd, id state) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setDepthStencilState:", self,
                      Args().ref("depthStencilState", state, "MTLDepthStencilState").str());
    }
    if (Measuring(reentry)) {
        // Only where the measurement tests depth and stencil; the untested count keeps Metal's
        // default state, which tests nothing.
        LogOverdrawOp(self, [s = Strong(state)](id<MTLRenderCommandEncoder> e, OverdrawReplay &r) {
            if (r.TestsDepthStencil()) [e setDepthStencilState:(id<MTLDepthStencilState>)s.get()];
        });
    }
    ORIG(void (*)(id, SEL, id))(self, _cmd, state);
}

void R_setStencilReferenceValue(id self, SEL _cmd, uint32_t value) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("setStencilReferenceValue:", self, Args().u("referenceValue", value).str());
    if (Measuring(reentry)) {
        LogOverdrawOp(self, [value](id<MTLRenderCommandEncoder> e, OverdrawReplay &) { [e setStencilReferenceValue:value]; });
    }
    ORIG(void (*)(id, SEL, uint32_t))(self, _cmd, value);
}

void R_setStencilFrontBackReference(id self, SEL _cmd, uint32_t front, uint32_t back) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setStencilFrontReferenceValue:backReferenceValue:", self,
                      Args().u("frontReferenceValue", front).u("backReferenceValue", back).str());
    }
    if (Measuring(reentry)) {
        LogOverdrawOp(self, [front, back](id<MTLRenderCommandEncoder> e, OverdrawReplay &) {
            [e setStencilFrontReferenceValue:front backReferenceValue:back];
        });
    }
    ORIG(void (*)(id, SEL, uint32_t, uint32_t))(self, _cmd, front, back);
}

void R_setVisibilityResultMode(id self, SEL _cmd, NSUInteger mode, NSUInteger offset) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setVisibilityResultMode:offset:", self, Args().u("mode", mode).u("offset", offset).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger))(self, _cmd, mode, offset);
}

void R_setColorStoreAction(id self, SEL _cmd, MTLStoreAction action, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setColorStoreAction:atIndex:", self,
                      Args().e("storeAction", StoreActionEnumName(action), (uint64_t)action).u("index", index).str());
    }
    ORIG(void (*)(id, SEL, MTLStoreAction, NSUInteger))(self, _cmd, action, index);
}

void R_setDepthStoreAction(id self, SEL _cmd, MTLStoreAction action) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setDepthStoreAction:", self,
                      Args().e("storeAction", StoreActionEnumName(action), (uint64_t)action).str());
    }
    ORIG(void (*)(id, SEL, MTLStoreAction))(self, _cmd, action);
}

void R_setStencilStoreAction(id self, SEL _cmd, MTLStoreAction action) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setStencilStoreAction:", self,
                      Args().e("storeAction", StoreActionEnumName(action), (uint64_t)action).str());
    }
    ORIG(void (*)(id, SEL, MTLStoreAction))(self, _cmd, action);
}

void R_setColorStoreActionOptions(id self, SEL _cmd, NSUInteger options, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setColorStoreActionOptions:atIndex:", self,
                      Args().u("storeActionOptions", options).u("index", index).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger))(self, _cmd, options, index);
}

void R_setDepthStoreActionOptions(id self, SEL _cmd, NSUInteger options) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setDepthStoreActionOptions:", self, Args().u("storeActionOptions", options).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger))(self, _cmd, options);
}

void R_setStencilStoreActionOptions(id self, SEL _cmd, NSUInteger options) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setStencilStoreActionOptions:", self, Args().u("storeActionOptions", options).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger))(self, _cmd, options);
}

// --------------------------------------------------------------------------------------------
// MTLRenderCommandEncoder: fragment stage

void R_setFragmentBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setFragmentBytes:length:atIndex:", self,
                                 BytesArgs("fragment", bytes, length, index),
                                 {QueueBytesCapture(bytes, length)});
    }
    if (Measuring(reentry)) LogBytes(self, _cmd, bytes, length, index);
    ORIG(void (*)(id, SEL, const void *, NSUInteger, NSUInteger))(self, _cmd, bytes, length, index);
}

void R_setFragmentBuffer(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setFragmentBuffer:offset:atIndex:", self,
                                 Args().ref("buffer", buffer, "MTLBuffer").u("offset", offset)
                                       .u("index", index).str(),
                                 {QueueBufferCapture(self, buffer, offset, 0)});
    }
    if (Measuring(reentry)) LogBuffer(self, _cmd, buffer, offset, index);
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, buffer, offset, index);
}

void R_setFragmentBufferOffset(id self, SEL _cmd, NSUInteger offset, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setFragmentBufferOffset:atIndex:", self,
                      Args().u("offset", offset).u("index", index).str());
    }
    if (Measuring(reentry)) LogIndexed(self, _cmd, offset, index);
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger))(self, _cmd, offset, index);
}

void R_setFragmentBuffers(id self, SEL _cmd, const id *buffers, const NSUInteger *offsets, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setFragmentBuffers:offsets:withRange:", self,
                                 Args().refs("buffers", buffers, range.length, "MTLBuffer")
                                       .uints("offsets", offsets, range.length).range("range", range).str(),
                                 QueueBuffers(self, buffers, offsets, range));
    }
    if (Measuring(reentry)) LogBuffers(self, _cmd, buffers, offsets, range);
    ORIG(void (*)(id, SEL, const id *, const NSUInteger *, NSRange))(self, _cmd, buffers, offsets, range);
}

void R_setFragmentTexture(id self, SEL _cmd, id texture, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setFragmentTexture:atIndex:", self,
                      Args().ref("texture", texture, "MTLTexture").u("index", index).str());
    }
    if (Measuring(reentry)) LogObject(self, _cmd, texture, index);
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, texture, index);
}

void R_setFragmentTextures(id self, SEL _cmd, const id *textures, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setFragmentTextures:withRange:", self,
                      Args().refs("textures", textures, range.length, "MTLTexture").range("range", range).str());
    }
    if (Measuring(reentry)) LogObjects(self, _cmd, textures, range);
    ORIG(void (*)(id, SEL, const id *, NSRange))(self, _cmd, textures, range);
}

void R_setFragmentSamplerState(id self, SEL _cmd, id sampler, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setFragmentSamplerState:atIndex:", self,
                      Args().ref("sampler", sampler, "MTLSamplerState").u("index", index).str());
    }
    if (Measuring(reentry)) LogObject(self, _cmd, sampler, index);
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, sampler, index);
}

void R_setFragmentSamplerStateLod(id self, SEL _cmd, id sampler, float lodMin, float lodMax, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:", self,
                      Args().ref("sampler", sampler, "MTLSamplerState").d("lodMinClamp", lodMin)
                            .d("lodMaxClamp", lodMax).u("index", index).str());
    }
    if (Measuring(reentry)) LogSamplerLod(self, _cmd, sampler, lodMin, lodMax, index);
    ORIG(void (*)(id, SEL, id, float, float, NSUInteger))(self, _cmd, sampler, lodMin, lodMax, index);
}

void R_setFragmentSamplerStates(id self, SEL _cmd, const id *samplers, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setFragmentSamplerStates:withRange:", self,
                      Args().refs("samplers", samplers, range.length, "MTLSamplerState").range("range", range).str());
    }
    if (Measuring(reentry)) LogObjects(self, _cmd, samplers, range);
    ORIG(void (*)(id, SEL, const id *, NSRange))(self, _cmd, samplers, range);
}

// --------------------------------------------------------------------------------------------
// MTLRenderCommandEncoder: draws

std::string DrawArgs(NSUInteger type, NSUInteger start, NSUInteger count, NSUInteger instances,
                     NSUInteger baseInstance) {
    Args a;
    a.e("primitiveType", PrimitiveTypeEnumName((MTLPrimitiveType)type), type)
     .u("vertexStart", start).u("vertexCount", count).u("instanceCount", instances)
     .u("baseInstance", baseInstance);
    return a.str();
}

void R_drawPrimitives3(id self, SEL _cmd, NSUInteger type, NSUInteger start, NSUInteger count) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawPrimitives:vertexStart:vertexCount:", self, DrawArgs(type, start, count, 1, 0));
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [type, start, count](id<MTLRenderCommandEncoder> e) {
            [e drawPrimitives:(MTLPrimitiveType)type vertexStart:start vertexCount:count];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))(self, _cmd, type, start, count);
}

void R_drawPrimitives4(id self, SEL _cmd, NSUInteger type, NSUInteger start, NSUInteger count,
                       NSUInteger instances) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawPrimitives:vertexStart:vertexCount:instanceCount:", self,
                          DrawArgs(type, start, count, instances, 0));
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [type, start, count, instances](id<MTLRenderCommandEncoder> e) {
            [e drawPrimitives:(MTLPrimitiveType)type vertexStart:start vertexCount:count instanceCount:instances];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger))(
        self, _cmd, type, start, count, instances);
}

void R_drawPrimitives5(id self, SEL _cmd, NSUInteger type, NSUInteger start, NSUInteger count,
                       NSUInteger instances, NSUInteger baseInstance) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:", self,
                          DrawArgs(type, start, count, instances, baseInstance));
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [type, start, count, instances, baseInstance](id<MTLRenderCommandEncoder> e) {
            [e drawPrimitives:(MTLPrimitiveType)type vertexStart:start vertexCount:count instanceCount:instances
                 baseInstance:baseInstance];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger, NSUInteger))(
        self, _cmd, type, start, count, instances, baseInstance);
}

/** MTLIndexType: 0 = UInt16, 1 = UInt32. The draw says exactly how many it reads. */
uint64_t IndexBytes(NSUInteger indexCount, NSUInteger indexType) {
    return (uint64_t)indexCount * (indexType == MTLIndexTypeUInt16 ? 2 : 4);
}

std::string IndexedDrawArgs(NSUInteger type, NSUInteger indexCount, NSUInteger indexType,
                            id indexBuffer, NSUInteger offset, NSUInteger instances,
                            NSInteger baseVertex, NSUInteger baseInstance) {
    Args a;
    a.e("primitiveType", PrimitiveTypeEnumName((MTLPrimitiveType)type), type)
     .u("indexCount", indexCount).u("indexType", indexType)
     .ref("indexBuffer", indexBuffer, "MTLBuffer").u("indexBufferOffset", offset)
     .u("instanceCount", instances).i("baseVertex", baseVertex).u("baseInstance", baseInstance);
    return a.str();
}

void R_drawIndexed5(id self, SEL _cmd, NSUInteger type, NSUInteger indexCount, NSUInteger indexType,
                    id indexBuffer, NSUInteger offset) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommandWithBuffers(
                "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", self,
                IndexedDrawArgs(type, indexCount, indexType, indexBuffer, offset, 1, 0, 0),
                {QueueBufferCapture(self, indexBuffer, offset, IndexBytes(indexCount, indexType))});
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [type, indexCount, indexType, b = Strong(indexBuffer), offset](id<MTLRenderCommandEncoder> e) {
            [e drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:indexCount indexType:(MTLIndexType)indexType
                         indexBuffer:(id<MTLBuffer>)b.get() indexBufferOffset:offset];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger))(
        self, _cmd, type, indexCount, indexType, indexBuffer, offset);
}

void R_drawIndexed6(id self, SEL _cmd, NSUInteger type, NSUInteger indexCount, NSUInteger indexType,
                    id indexBuffer, NSUInteger offset, NSUInteger instances) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommandWithBuffers(
                "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
                self, IndexedDrawArgs(type, indexCount, indexType, indexBuffer, offset, instances, 0, 0),
                {QueueBufferCapture(self, indexBuffer, offset, IndexBytes(indexCount, indexType))});
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [type, indexCount, indexType, b = Strong(indexBuffer), offset, instances](id<MTLRenderCommandEncoder> e) {
            [e drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:indexCount indexType:(MTLIndexType)indexType
                         indexBuffer:(id<MTLBuffer>)b.get() indexBufferOffset:offset instanceCount:instances];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger))(
        self, _cmd, type, indexCount, indexType, indexBuffer, offset, instances);
}

void R_drawIndexed8(id self, SEL _cmd, NSUInteger type, NSUInteger indexCount, NSUInteger indexType,
                    id indexBuffer, NSUInteger offset, NSUInteger instances, NSInteger baseVertex,
                    NSUInteger baseInstance) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommandWithBuffers(
                "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"
                "instanceCount:baseVertex:baseInstance:",
                self, IndexedDrawArgs(type, indexCount, indexType, indexBuffer, offset, instances,
                                      baseVertex, baseInstance),
                {QueueBufferCapture(self, indexBuffer, offset, IndexBytes(indexCount, indexType))});
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [type, indexCount, indexType, b = Strong(indexBuffer), offset, instances, baseVertex,
                       baseInstance](id<MTLRenderCommandEncoder> e) {
            [e drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:indexCount indexType:(MTLIndexType)indexType
                         indexBuffer:(id<MTLBuffer>)b.get() indexBufferOffset:offset instanceCount:instances
                          baseVertex:baseVertex baseInstance:baseInstance];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger, NSInteger,
                  NSUInteger))(self, _cmd, type, indexCount, indexType, indexBuffer, offset,
                               instances, baseVertex, baseInstance);
}

void R_drawPrimitivesIndirect(id self, SEL _cmd, NSUInteger type, id indirect, NSUInteger offset) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            // MTLDrawPrimitivesIndirectArguments: four 32-bit words.
            RecordCommandWithBuffers("drawPrimitives:indirectBuffer:indirectBufferOffset:", self,
                                     Args().e("primitiveType", PrimitiveTypeEnumName((MTLPrimitiveType)type), type)
                                           .ref("indirectBuffer", indirect, "MTLBuffer")
                                           .u("indirectBufferOffset", offset).str(),
                                     {QueueBufferCapture(self, indirect, offset, 16)});
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [type, b = Strong(indirect), offset](id<MTLRenderCommandEncoder> e) {
            [e drawPrimitives:(MTLPrimitiveType)type indirectBuffer:(id<MTLBuffer>)b.get() indirectBufferOffset:offset];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, id, NSUInteger))(self, _cmd, type, indirect, offset);
}

void R_drawIndexedIndirect(id self, SEL _cmd, NSUInteger type, NSUInteger indexType, id indexBuffer,
                           NSUInteger indexOffset, id indirect, NSUInteger indirectOffset) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            // MTLDrawIndexedPrimitivesIndirectArguments: five 32-bit words. The index range is
            // in the GPU's hands, so the index buffer is read from its offset to its end.
            RecordCommandWithBuffers(
                "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
                self,
                Args().e("primitiveType", PrimitiveTypeEnumName((MTLPrimitiveType)type), type)
                      .u("indexType", indexType).ref("indexBuffer", indexBuffer, "MTLBuffer")
                      .u("indexBufferOffset", indexOffset).ref("indirectBuffer", indirect, "MTLBuffer")
                      .u("indirectBufferOffset", indirectOffset).str(),
                {QueueBufferCapture(self, indexBuffer, indexOffset, 0),
                 QueueBufferCapture(self, indirect, indirectOffset, 20)});
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [type, indexType, ib = Strong(indexBuffer), indexOffset, b = Strong(indirect),
                       indirectOffset](id<MTLRenderCommandEncoder> e) {
            [e drawIndexedPrimitives:(MTLPrimitiveType)type indexType:(MTLIndexType)indexType
                         indexBuffer:(id<MTLBuffer>)ib.get() indexBufferOffset:indexOffset
                      indirectBuffer:(id<MTLBuffer>)b.get() indirectBufferOffset:indirectOffset];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, id, NSUInteger, id, NSUInteger))(
        self, _cmd, type, indexType, indexBuffer, indexOffset, indirect, indirectOffset);
}

void R_drawPatches(id self, SEL _cmd, NSUInteger controlPoints, NSUInteger patchStart, NSUInteger patchCount,
                   id patchIndexBuffer, NSUInteger patchIndexOffset, NSUInteger instances,
                   NSUInteger baseInstance) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:"
                          "instanceCount:baseInstance:", self,
                          Args().u("numberOfPatchControlPoints", controlPoints).u("patchStart", patchStart)
                                .u("patchCount", patchCount).ref("patchIndexBuffer", patchIndexBuffer, "MTLBuffer")
                                .u("patchIndexBufferOffset", patchIndexOffset).u("instanceCount", instances)
                                .u("baseInstance", baseInstance).str());
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [controlPoints, patchStart, patchCount, pb = Strong(patchIndexBuffer), patchIndexOffset, instances,
                       baseInstance](id<MTLRenderCommandEncoder> e) {
            [e drawPatches:controlPoints patchStart:patchStart patchCount:patchCount
                patchIndexBuffer:(id<MTLBuffer>)pb.get() patchIndexBufferOffset:patchIndexOffset
                   instanceCount:instances baseInstance:baseInstance];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger, NSUInteger))(
        self, _cmd, controlPoints, patchStart, patchCount, patchIndexBuffer, patchIndexOffset,
        instances, baseInstance);
}

void R_drawPatchesIndirect(id self, SEL _cmd, NSUInteger controlPoints, id patchIndexBuffer,
                           NSUInteger patchIndexOffset, id indirect, NSUInteger indirectOffset) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
                          self,
                          Args().u("numberOfPatchControlPoints", controlPoints)
                                .ref("patchIndexBuffer", patchIndexBuffer, "MTLBuffer")
                                .u("patchIndexBufferOffset", patchIndexOffset)
                                .ref("indirectBuffer", indirect, "MTLBuffer")
                                .u("indirectBufferOffset", indirectOffset).str());
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [controlPoints, pb = Strong(patchIndexBuffer), patchIndexOffset, b = Strong(indirect),
                       indirectOffset](id<MTLRenderCommandEncoder> e) {
            [e drawPatches:controlPoints patchIndexBuffer:(id<MTLBuffer>)pb.get() patchIndexBufferOffset:patchIndexOffset
                indirectBuffer:(id<MTLBuffer>)b.get() indirectBufferOffset:indirectOffset];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, id, NSUInteger, id, NSUInteger))(
        self, _cmd, controlPoints, patchIndexBuffer, patchIndexOffset, indirect, indirectOffset);
}

void R_drawIndexedPatches(id self, SEL _cmd, NSUInteger controlPoints, NSUInteger patchStart,
                          NSUInteger patchCount, id patchIndexBuffer, NSUInteger patchIndexOffset,
                          id controlPointIndexBuffer, NSUInteger controlPointIndexOffset,
                          NSUInteger instances, NSUInteger baseInstance) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:"
                          "controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:baseInstance:",
                          self,
                          Args().u("numberOfPatchControlPoints", controlPoints).u("patchStart", patchStart)
                                .u("patchCount", patchCount).ref("patchIndexBuffer", patchIndexBuffer, "MTLBuffer")
                                .u("patchIndexBufferOffset", patchIndexOffset)
                                .ref("controlPointIndexBuffer", controlPointIndexBuffer, "MTLBuffer")
                                .u("controlPointIndexBufferOffset", controlPointIndexOffset)
                                .u("instanceCount", instances).u("baseInstance", baseInstance).str());
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [controlPoints, patchStart, patchCount, pb = Strong(patchIndexBuffer), patchIndexOffset,
                       cb = Strong(controlPointIndexBuffer), controlPointIndexOffset, instances,
                       baseInstance](id<MTLRenderCommandEncoder> e) {
            [e drawIndexedPatches:controlPoints patchStart:patchStart patchCount:patchCount
                 patchIndexBuffer:(id<MTLBuffer>)pb.get() patchIndexBufferOffset:patchIndexOffset
          controlPointIndexBuffer:(id<MTLBuffer>)cb.get() controlPointIndexBufferOffset:controlPointIndexOffset
                    instanceCount:instances baseInstance:baseInstance];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, id, NSUInteger, NSUInteger,
                  NSUInteger))(self, _cmd, controlPoints, patchStart, patchCount, patchIndexBuffer,
                               patchIndexOffset, controlPointIndexBuffer, controlPointIndexOffset,
                               instances, baseInstance);
}

void R_drawIndexedPatchesIndirect(id self, SEL _cmd, NSUInteger controlPoints, id patchIndexBuffer,
                                  NSUInteger patchIndexOffset, id controlPointIndexBuffer,
                                  NSUInteger controlPointIndexOffset, id indirect, NSUInteger indirectOffset) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:"
                          "controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:", self,
                          Args().u("numberOfPatchControlPoints", controlPoints)
                                .ref("patchIndexBuffer", patchIndexBuffer, "MTLBuffer")
                                .u("patchIndexBufferOffset", patchIndexOffset)
                                .ref("controlPointIndexBuffer", controlPointIndexBuffer, "MTLBuffer")
                                .u("controlPointIndexBufferOffset", controlPointIndexOffset)
                                .ref("indirectBuffer", indirect, "MTLBuffer")
                                .u("indirectBufferOffset", indirectOffset).str());
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [controlPoints, pb = Strong(patchIndexBuffer), patchIndexOffset, cb = Strong(controlPointIndexBuffer),
                       controlPointIndexOffset, b = Strong(indirect), indirectOffset](id<MTLRenderCommandEncoder> e) {
            [e drawIndexedPatches:controlPoints patchIndexBuffer:(id<MTLBuffer>)pb.get()
               patchIndexBufferOffset:patchIndexOffset controlPointIndexBuffer:(id<MTLBuffer>)cb.get()
        controlPointIndexBufferOffset:controlPointIndexOffset indirectBuffer:(id<MTLBuffer>)b.get()
                 indirectBufferOffset:indirectOffset];
        });
    }
    ORIG(void (*)(id, SEL, NSUInteger, id, NSUInteger, id, NSUInteger, id, NSUInteger))(
        self, _cmd, controlPoints, patchIndexBuffer, patchIndexOffset, controlPointIndexBuffer,
        controlPointIndexOffset, indirect, indirectOffset);
}

void R_drawMeshThreadgroups(id self, SEL _cmd, MTLSize groups, MTLSize objectThreads, MTLSize meshThreads) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:", self,
                          Args().size("threadgroupsPerGrid", groups).size("threadsPerObjectThreadgroup", objectThreads)
                                .size("threadsPerMeshThreadgroup", meshThreads).str());
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [sel = _cmd, groups, objectThreads, meshThreads](id<MTLRenderCommandEncoder> e) {
            ((void (*)(id, SEL, MTLSize, MTLSize, MTLSize))objc_msgSend)(e, sel, groups, objectThreads, meshThreads);
        });
    }
    ORIG(void (*)(id, SEL, MTLSize, MTLSize, MTLSize))(self, _cmd, groups, objectThreads, meshThreads);
}

void R_drawMeshThreads(id self, SEL _cmd, MTLSize threads, MTLSize objectThreads, MTLSize meshThreads) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:", self,
                          Args().size("threadsPerGrid", threads).size("threadsPerObjectThreadgroup", objectThreads)
                                .size("threadsPerMeshThreadgroup", meshThreads).str());
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [sel = _cmd, threads, objectThreads, meshThreads](id<MTLRenderCommandEncoder> e) {
            ((void (*)(id, SEL, MTLSize, MTLSize, MTLSize))objc_msgSend)(e, sel, threads, objectThreads, meshThreads);
        });
    }
    ORIG(void (*)(id, SEL, MTLSize, MTLSize, MTLSize))(self, _cmd, threads, objectThreads, meshThreads);
}

void R_drawMeshThreadgroupsIndirect(id self, SEL _cmd, id indirect, NSUInteger offset, MTLSize objectThreads,
                                    MTLSize meshThreads) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:"
                          "threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:", self,
                          Args().ref("indirectBuffer", indirect, "MTLBuffer").u("indirectBufferOffset", offset)
                                .size("threadsPerObjectThreadgroup", objectThreads)
                                .size("threadsPerMeshThreadgroup", meshThreads).str());
        }
    }
    if (Measuring(reentry)) {
        LogDraw(self, [sel = _cmd, b = Strong(indirect), offset, objectThreads, meshThreads](id<MTLRenderCommandEncoder> e) {
            ((void (*)(id, SEL, id, NSUInteger, MTLSize, MTLSize))objc_msgSend)(e, sel, b.get(), offset, objectThreads, meshThreads);
        });
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, MTLSize, MTLSize))(self, _cmd, indirect, offset, objectThreads, meshThreads);
}

// --------------------------------------------------------------------------------------------
// MTLRenderCommandEncoder: object, mesh and tile stages

// MEASURE: whether the binding is kept for an overdraw measurement. The object and mesh stages
// feed the rasterizer; a tile stage belongs to a tile pipeline, which a measurement has no copy of.
#define STAGE_BYTES(FN, METHOD, STAGE, MEASURE)                                                    \
    void FN(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index) {           \
        Reentry reentry(self, _cmd);                                                               \
        if (Rec(reentry)) {                                                                        \
            RecordCommandWithBuffers(METHOD, self, BytesArgs(STAGE, bytes, length, index),         \
                                     {QueueBytesCapture(bytes, length)});                          \
        }                                                                                          \
        if (MEASURE && Measuring(reentry)) LogBytes(self, _cmd, bytes, length, index);             \
        ORIG(void (*)(id, SEL, const void *, NSUInteger, NSUInteger))(self, _cmd, bytes, length, index); \
    }

#define STAGE_BUFFER(FN, METHOD, MEASURE)                                                          \
    void FN(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger index) {                   \
        Reentry reentry(self, _cmd);                                                               \
        if (Rec(reentry)) {                                                                        \
            RecordCommandWithBuffers(METHOD, self,                                                 \
                                     Args().ref("buffer", buffer, "MTLBuffer").u("offset", offset) \
                                           .u("index", index).str(),                               \
                                     {QueueBufferCapture(self, buffer, offset, 0)});               \
        }                                                                                          \
        if (MEASURE && Measuring(reentry)) LogBuffer(self, _cmd, buffer, offset, index);           \
        ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, buffer, offset, index);    \
    }

#define STAGE_TEXTURE(FN, METHOD, MEASURE)                                                         \
    void FN(id self, SEL _cmd, id texture, NSUInteger index) {                                     \
        Reentry reentry(self, _cmd);                                                               \
        if (Rec(reentry)) {                                                                        \
            RecordCommand(METHOD, self,                                                            \
                          Args().ref("texture", texture, "MTLTexture").u("index", index).str());   \
        }                                                                                          \
        if (MEASURE && Measuring(reentry)) LogObject(self, _cmd, texture, index);                  \
        ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, texture, index);                       \
    }

#define STAGE_SAMPLER(FN, METHOD, MEASURE)                                                         \
    void FN(id self, SEL _cmd, id sampler, NSUInteger index) {                                     \
        Reentry reentry(self, _cmd);                                                               \
        if (Rec(reentry)) {                                                                        \
            RecordCommand(METHOD, self,                                                            \
                          Args().ref("sampler", sampler, "MTLSamplerState").u("index", index).str()); \
        }                                                                                          \
        if (MEASURE && Measuring(reentry)) LogObject(self, _cmd, sampler, index);                  \
        ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, sampler, index);                       \
    }

STAGE_BYTES(R_setObjectBytes, "setObjectBytes:length:atIndex:", "object", true)
STAGE_BUFFER(R_setObjectBuffer, "setObjectBuffer:offset:atIndex:", true)
STAGE_TEXTURE(R_setObjectTexture, "setObjectTexture:atIndex:", true)
STAGE_SAMPLER(R_setObjectSamplerState, "setObjectSamplerState:atIndex:", true)
STAGE_BYTES(R_setMeshBytes, "setMeshBytes:length:atIndex:", "mesh", true)
STAGE_BUFFER(R_setMeshBuffer, "setMeshBuffer:offset:atIndex:", true)
STAGE_TEXTURE(R_setMeshTexture, "setMeshTexture:atIndex:", true)
STAGE_SAMPLER(R_setMeshSamplerState, "setMeshSamplerState:atIndex:", true)
STAGE_BYTES(R_setTileBytes, "setTileBytes:length:atIndex:", "tile", false)
STAGE_BUFFER(R_setTileBuffer, "setTileBuffer:offset:atIndex:", false)
STAGE_TEXTURE(R_setTileTexture, "setTileTexture:atIndex:", false)
STAGE_SAMPLER(R_setTileSamplerState, "setTileSamplerState:atIndex:", false)

void R_setObjectThreadgroupMemoryLength(id self, SEL _cmd, NSUInteger length, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setObjectThreadgroupMemoryLength:atIndex:", self,
                      Args().u("length", length).u("index", index).str());
    }
    if (Measuring(reentry)) LogIndexed(self, _cmd, length, index);
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger))(self, _cmd, length, index);
}

void R_dispatchThreadsPerTile(id self, SEL _cmd, MTLSize threads) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_dispatchesThisFrame++;
        if (Recording()) {
            RecordCommand("dispatchThreadsPerTile:", self, Args().size("threadsPerTile", threads).str());
        }
    }
    ORIG(void (*)(id, SEL, MTLSize))(self, _cmd, threads);
}

void R_setThreadgroupMemoryLength(id self, SEL _cmd, NSUInteger length, NSUInteger offset, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setThreadgroupMemoryLength:offset:atIndex:", self,
                      Args().u("length", length).u("offset", offset).u("index", index).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))(self, _cmd, length, offset, index);
}

// --------------------------------------------------------------------------------------------
// MTLRenderCommandEncoder: residency, barriers, fences, tessellation, indirect command buffers

void R_useResource(id self, SEL _cmd, id resource, NSUInteger usage) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useResource:usage:", self,
                      Args().ref("resource", resource, "MTLResource").u("usage", usage).str());
    }
    if (Measuring(reentry)) LogUse(self, _cmd, resource, usage);
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, resource, usage);
}

void R_useResourceStages(id self, SEL _cmd, id resource, NSUInteger usage, NSUInteger stages) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useResource:usage:stages:", self,
                      Args().ref("resource", resource, "MTLResource").u("usage", usage).u("stages", stages).str());
    }
    if (Measuring(reentry)) LogUse(self, _cmd, resource, usage, stages);
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, resource, usage, stages);
}

void R_useResources(id self, SEL _cmd, const id *resources, NSUInteger count, NSUInteger usage) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useResources:count:usage:", self,
                      Args().refs("resources", resources, count, "MTLResource").u("count", count).u("usage", usage).str());
    }
    if (Measuring(reentry)) LogUseList(self, _cmd, resources, count, usage);
    ORIG(void (*)(id, SEL, const id *, NSUInteger, NSUInteger))(self, _cmd, resources, count, usage);
}

void R_useResourcesStages(id self, SEL _cmd, const id *resources, NSUInteger count, NSUInteger usage,
                          NSUInteger stages) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useResources:count:usage:stages:", self,
                      Args().refs("resources", resources, count, "MTLResource").u("count", count)
                            .u("usage", usage).u("stages", stages).str());
    }
    if (Measuring(reentry)) LogUseList(self, _cmd, resources, count, usage, stages);
    ORIG(void (*)(id, SEL, const id *, NSUInteger, NSUInteger, NSUInteger))(self, _cmd, resources, count, usage, stages);
}

void R_useHeap(id self, SEL _cmd, id heap) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("useHeap:", self, Args().ref("heap", heap, "MTLHeap").str());
    if (Measuring(reentry)) LogUse(self, _cmd, heap);
    ORIG(void (*)(id, SEL, id))(self, _cmd, heap);
}

void R_useHeapStages(id self, SEL _cmd, id heap, NSUInteger stages) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useHeap:stages:", self, Args().ref("heap", heap, "MTLHeap").u("stages", stages).str());
    }
    if (Measuring(reentry)) LogUse(self, _cmd, heap, stages);
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, heap, stages);
}

void R_useHeaps(id self, SEL _cmd, const id *heaps, NSUInteger count) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useHeaps:count:", self, Args().refs("heaps", heaps, count, "MTLHeap").u("count", count).str());
    }
    if (Measuring(reentry)) LogUseList(self, _cmd, heaps, count);
    ORIG(void (*)(id, SEL, const id *, NSUInteger))(self, _cmd, heaps, count);
}

void R_useHeapsStages(id self, SEL _cmd, const id *heaps, NSUInteger count, NSUInteger stages) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useHeaps:count:stages:", self,
                      Args().refs("heaps", heaps, count, "MTLHeap").u("count", count).u("stages", stages).str());
    }
    if (Measuring(reentry)) LogUseList(self, _cmd, heaps, count, stages);
    ORIG(void (*)(id, SEL, const id *, NSUInteger, NSUInteger))(self, _cmd, heaps, count, stages);
}

/**
 * An indirect command buffer's draws cannot be measured: the pipeline each command uses is in the
 * buffer, where a counting copy cannot replace it. They are reported as not counted.
 */
void LogIndirectCommands(id self) {
    LogOverdrawOp(self, [](id<MTLRenderCommandEncoder>, OverdrawReplay &r) { r.Skip(); });
}

void R_executeCommandsInBuffer(id self, SEL _cmd, id icb, NSRange range) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("executeCommandsInBuffer:withRange:", self,
                          Args().ref("indirectCommandBuffer", icb, "MTLIndirectCommandBuffer").range("range", range).str());
        }
    }
    if (Measuring(reentry)) LogIndirectCommands(self);
    ORIG(void (*)(id, SEL, id, NSRange))(self, _cmd, icb, range);
}

void R_executeCommandsIndirect(id self, SEL _cmd, id icb, id indirect, NSUInteger offset) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_drawsThisFrame++;
        if (Recording()) {
            RecordCommand("executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:", self,
                          Args().ref("indirectCommandBuffer", icb, "MTLIndirectCommandBuffer")
                                .ref("indirectBuffer", indirect, "MTLBuffer").u("indirectBufferOffset", offset).str());
        }
    }
    if (Measuring(reentry)) LogIndirectCommands(self);
    ORIG(void (*)(id, SEL, id, id, NSUInteger))(self, _cmd, icb, indirect, offset);
}

void R_memoryBarrierScope(id self, SEL _cmd, NSUInteger scope, NSUInteger after, NSUInteger before) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("memoryBarrierWithScope:afterStages:beforeStages:", self,
                      Args().u("scope", scope).u("afterStages", after).u("beforeStages", before).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))(self, _cmd, scope, after, before);
}

void R_memoryBarrierResources(id self, SEL _cmd, const id *resources, NSUInteger count, NSUInteger after,
                              NSUInteger before) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("memoryBarrierWithResources:count:afterStages:beforeStages:", self,
                      Args().refs("resources", resources, count, "MTLResource").u("count", count)
                            .u("afterStages", after).u("beforeStages", before).str());
    }
    ORIG(void (*)(id, SEL, const id *, NSUInteger, NSUInteger, NSUInteger))(self, _cmd, resources, count, after, before);
}

void R_updateFence(id self, SEL _cmd, id fence, NSUInteger stages) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("updateFence:afterStages:", self, Args().ref("fence", fence, "MTLFence").u("afterStages", stages).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, fence, stages);
}

void R_waitForFence(id self, SEL _cmd, id fence, NSUInteger stages) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("waitForFence:beforeStages:", self, Args().ref("fence", fence, "MTLFence").u("beforeStages", stages).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, fence, stages);
}

void R_textureBarrier(id self, SEL _cmd) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("textureBarrier", self, {});
    ORIG(void (*)(id, SEL))(self, _cmd);
}

void R_setTessellationFactorBuffer(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger instanceStride) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setTessellationFactorBuffer:offset:instanceStride:", self,
                                 Args().ref("buffer", buffer, "MTLBuffer").u("offset", offset)
                                       .u("instanceStride", instanceStride).str(),
                                 {QueueBufferCapture(self, buffer, offset, 0)});
    }
    if (Measuring(reentry)) LogBuffer(self, _cmd, buffer, offset, instanceStride);
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, buffer, offset, instanceStride);
}

void R_setTessellationFactorScale(id self, SEL _cmd, float scale) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("setTessellationFactorScale:", self, Args().d("scale", scale).str());
    if (Measuring(reentry)) {
        LogOverdrawOp(self, [scale](id<MTLRenderCommandEncoder> e, OverdrawReplay &) { [e setTessellationFactorScale:scale]; });
    }
    ORIG(void (*)(id, SEL, float))(self, _cmd, scale);
}

// --------------------------------------------------------------------------------------------
// MTLComputeCommandEncoder

void C_setComputePipelineState(id self, SEL _cmd, id state) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setComputePipelineState:", self,
                      Args().ref("pipeline", state, "MTLComputePipelineState").str());
    }
    ORIG(void (*)(id, SEL, id))(self, _cmd, state);
}

void C_setBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setBytes:length:atIndex:", self, BytesArgs("compute", bytes, length, index),
                                 {QueueBytesCapture(bytes, length)});
    }
    ORIG(void (*)(id, SEL, const void *, NSUInteger, NSUInteger))(self, _cmd, bytes, length, index);
}

void C_setBuffer(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setBuffer:offset:atIndex:", self,
                                 Args().ref("buffer", buffer, "MTLBuffer").u("offset", offset).u("index", index).str(),
                                 {QueueBufferCapture(self, buffer, offset, 0)});
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, buffer, offset, index);
}

void C_setBufferOffset(id self, SEL _cmd, NSUInteger offset, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setBufferOffset:atIndex:", self, Args().u("offset", offset).u("index", index).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger))(self, _cmd, offset, index);
}

void C_setBuffers(id self, SEL _cmd, const id *buffers, const NSUInteger *offsets, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommandWithBuffers("setBuffers:offsets:withRange:", self,
                                 Args().refs("buffers", buffers, range.length, "MTLBuffer")
                                       .uints("offsets", offsets, range.length).range("range", range).str(),
                                 QueueBuffers(self, buffers, offsets, range));
    }
    ORIG(void (*)(id, SEL, const id *, const NSUInteger *, NSRange))(self, _cmd, buffers, offsets, range);
}

void C_setTexture(id self, SEL _cmd, id texture, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setTexture:atIndex:", self, Args().ref("texture", texture, "MTLTexture").u("index", index).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, texture, index);
}

void C_setTextures(id self, SEL _cmd, const id *textures, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setTextures:withRange:", self,
                      Args().refs("textures", textures, range.length, "MTLTexture").range("range", range).str());
    }
    ORIG(void (*)(id, SEL, const id *, NSRange))(self, _cmd, textures, range);
}

void C_setSamplerState(id self, SEL _cmd, id sampler, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setSamplerState:atIndex:", self,
                      Args().ref("sampler", sampler, "MTLSamplerState").u("index", index).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, sampler, index);
}

void C_setSamplerStateLod(id self, SEL _cmd, id sampler, float lodMin, float lodMax, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setSamplerState:lodMinClamp:lodMaxClamp:atIndex:", self,
                      Args().ref("sampler", sampler, "MTLSamplerState").d("lodMinClamp", lodMin)
                            .d("lodMaxClamp", lodMax).u("index", index).str());
    }
    ORIG(void (*)(id, SEL, id, float, float, NSUInteger))(self, _cmd, sampler, lodMin, lodMax, index);
}

void C_setSamplerStates(id self, SEL _cmd, const id *samplers, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setSamplerStates:withRange:", self,
                      Args().refs("samplers", samplers, range.length, "MTLSamplerState").range("range", range).str());
    }
    ORIG(void (*)(id, SEL, const id *, NSRange))(self, _cmd, samplers, range);
}

void C_setThreadgroupMemoryLength(id self, SEL _cmd, NSUInteger length, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setThreadgroupMemoryLength:atIndex:", self, Args().u("length", length).u("index", index).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger))(self, _cmd, length, index);
}

void C_setImageblockWidth(id self, SEL _cmd, NSUInteger width, NSUInteger height) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setImageblockWidth:height:", self, Args().u("width", width).u("height", height).str());
    }
    ORIG(void (*)(id, SEL, NSUInteger, NSUInteger))(self, _cmd, width, height);
}

void C_setStageInRegion(id self, SEL _cmd, MTLRegion region) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("setStageInRegion:", self, Args().region("region", region).str());
    ORIG(void (*)(id, SEL, MTLRegion))(self, _cmd, region);
}

void C_setStageInRegionIndirect(id self, SEL _cmd, id indirect, NSUInteger offset) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setStageInRegionWithIndirectBuffer:indirectBufferOffset:", self,
                      Args().ref("indirectBuffer", indirect, "MTLBuffer").u("indirectBufferOffset", offset).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, indirect, offset);
}

void C_dispatchThreadgroups(id self, SEL _cmd, MTLSize groups, MTLSize perGroup) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_dispatchesThisFrame++;
        if (Recording()) {
            RecordCommand("dispatchThreadgroups:threadsPerThreadgroup:", self,
                          Args().size("threadgroupsPerGrid", groups).size("threadsPerThreadgroup", perGroup).str());
        }
    }
    ORIG(void (*)(id, SEL, MTLSize, MTLSize))(self, _cmd, groups, perGroup);
}

void C_dispatchThreads(id self, SEL _cmd, MTLSize threads, MTLSize perGroup) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_dispatchesThisFrame++;
        if (Recording()) {
            RecordCommand("dispatchThreads:threadsPerThreadgroup:", self,
                          Args().size("threadsPerGrid", threads).size("threadsPerThreadgroup", perGroup).str());
        }
    }
    ORIG(void (*)(id, SEL, MTLSize, MTLSize))(self, _cmd, threads, perGroup);
}

void C_dispatchThreadgroupsIndirect(id self, SEL _cmd, id indirect, NSUInteger offset, MTLSize perGroup) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_dispatchesThisFrame++;
        if (Recording()) {
            // MTLDispatchThreadgroupsIndirectArguments: three 32-bit words.
            RecordCommandWithBuffers("dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
                                     self,
                                     Args().ref("indirectBuffer", indirect, "MTLBuffer").u("indirectBufferOffset", offset)
                                           .size("threadsPerThreadgroup", perGroup).str(),
                                     {QueueBufferCapture(self, indirect, offset, 12)});
        }
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, MTLSize))(self, _cmd, indirect, offset, perGroup);
}

void C_updateFence(id self, SEL _cmd, id fence) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("updateFence:", self, Args().ref("fence", fence, "MTLFence").str());
    ORIG(void (*)(id, SEL, id))(self, _cmd, fence);
}

void C_waitForFence(id self, SEL _cmd, id fence) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("waitForFence:", self, Args().ref("fence", fence, "MTLFence").str());
    ORIG(void (*)(id, SEL, id))(self, _cmd, fence);
}

void C_useResource(id self, SEL _cmd, id resource, NSUInteger usage) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useResource:usage:", self, Args().ref("resource", resource, "MTLResource").u("usage", usage).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, resource, usage);
}

void C_useResources(id self, SEL _cmd, const id *resources, NSUInteger count, NSUInteger usage) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useResources:count:usage:", self,
                      Args().refs("resources", resources, count, "MTLResource").u("count", count).u("usage", usage).str());
    }
    ORIG(void (*)(id, SEL, const id *, NSUInteger, NSUInteger))(self, _cmd, resources, count, usage);
}

void C_useHeap(id self, SEL _cmd, id heap) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("useHeap:", self, Args().ref("heap", heap, "MTLHeap").str());
    ORIG(void (*)(id, SEL, id))(self, _cmd, heap);
}

void C_useHeaps(id self, SEL _cmd, const id *heaps, NSUInteger count) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("useHeaps:count:", self, Args().refs("heaps", heaps, count, "MTLHeap").u("count", count).str());
    }
    ORIG(void (*)(id, SEL, const id *, NSUInteger))(self, _cmd, heaps, count);
}

void C_executeCommandsInBuffer(id self, SEL _cmd, id icb, NSRange range) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_dispatchesThisFrame++;
        if (Recording()) {
            RecordCommand("executeCommandsInBuffer:withRange:", self,
                          Args().ref("indirectCommandBuffer", icb, "MTLIndirectCommandBuffer").range("range", range).str());
        }
    }
    ORIG(void (*)(id, SEL, id, NSRange))(self, _cmd, icb, range);
}

void C_executeCommandsIndirect(id self, SEL _cmd, id icb, id indirect, NSUInteger offset) {
    Reentry reentry(self, _cmd);
    if (reentry.outermost()) {
        g_dispatchesThisFrame++;
        if (Recording()) {
            RecordCommand("executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:", self,
                          Args().ref("indirectCommandBuffer", icb, "MTLIndirectCommandBuffer")
                                .ref("indirectBuffer", indirect, "MTLBuffer").u("indirectBufferOffset", offset).str());
        }
    }
    ORIG(void (*)(id, SEL, id, id, NSUInteger))(self, _cmd, icb, indirect, offset);
}

void C_memoryBarrierScope(id self, SEL _cmd, NSUInteger scope) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("memoryBarrierWithScope:", self, Args().u("scope", scope).str());
    ORIG(void (*)(id, SEL, NSUInteger))(self, _cmd, scope);
}

void C_memoryBarrierResources(id self, SEL _cmd, const id *resources, NSUInteger count) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("memoryBarrierWithResources:count:", self,
                      Args().refs("resources", resources, count, "MTLResource").u("count", count).str());
    }
    ORIG(void (*)(id, SEL, const id *, NSUInteger))(self, _cmd, resources, count);
}

void C_setAccelerationStructure(id self, SEL _cmd, id structure, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("setAccelerationStructure:atBufferIndex:", self,
                      Args().ref("accelerationStructure", structure, "MTLAccelerationStructure").u("index", index).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger))(self, _cmd, structure, index);
}

// --------------------------------------------------------------------------------------------
// MTLBlitCommandEncoder

void B_synchronizeResource(id self, SEL _cmd, id resource) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("synchronizeResource:", self, Args().ref("resource", resource, "MTLResource").str());
    }
    ORIG(void (*)(id, SEL, id))(self, _cmd, resource);
}

void B_synchronizeTexture(id self, SEL _cmd, id texture, NSUInteger slice, NSUInteger level) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("synchronizeTexture:slice:level:", self,
                      Args().ref("texture", texture, "MTLTexture").u("slice", slice).u("level", level).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, texture, slice, level);
}

void B_copyTextureToTexture(id self, SEL _cmd, id source, NSUInteger sourceSlice, NSUInteger sourceLevel,
                            MTLOrigin sourceOrigin, MTLSize sourceSize, id destination,
                            NSUInteger destinationSlice, NSUInteger destinationLevel, MTLOrigin destinationOrigin) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:"
                      "destinationSlice:destinationLevel:destinationOrigin:", self,
                      Args().ref("sourceTexture", source, "MTLTexture").u("sourceSlice", sourceSlice)
                            .u("sourceLevel", sourceLevel).origin("sourceOrigin", sourceOrigin)
                            .size("sourceSize", sourceSize).ref("destinationTexture", destination, "MTLTexture")
                            .u("destinationSlice", destinationSlice).u("destinationLevel", destinationLevel)
                            .origin("destinationOrigin", destinationOrigin).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger, MTLOrigin))(
        self, _cmd, source, sourceSlice, sourceLevel, sourceOrigin, sourceSize, destination,
        destinationSlice, destinationLevel, destinationOrigin);
}

std::string TextureToBufferArgs(id source, NSUInteger sourceSlice, NSUInteger sourceLevel, MTLOrigin sourceOrigin,
                                MTLSize sourceSize, id destination, NSUInteger offset, NSUInteger bytesPerRow,
                                NSUInteger bytesPerImage, const NSUInteger *options) {
    Args a;
    a.ref("sourceTexture", source, "MTLTexture").u("sourceSlice", sourceSlice).u("sourceLevel", sourceLevel)
     .origin("sourceOrigin", sourceOrigin).size("sourceSize", sourceSize)
     .ref("destinationBuffer", destination, "MTLBuffer").u("destinationOffset", offset)
     .u("destinationBytesPerRow", bytesPerRow).u("destinationBytesPerImage", bytesPerImage);
    if (options != nullptr) a.u("options", *options);
    return a.str();
}

void B_copyTextureToBuffer(id self, SEL _cmd, id source, NSUInteger sourceSlice, NSUInteger sourceLevel,
                           MTLOrigin sourceOrigin, MTLSize sourceSize, id destination, NSUInteger offset,
                           NSUInteger bytesPerRow, NSUInteger bytesPerImage) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:"
                      "destinationOffset:destinationBytesPerRow:destinationBytesPerImage:", self,
                      TextureToBufferArgs(source, sourceSlice, sourceLevel, sourceOrigin, sourceSize, destination,
                                          offset, bytesPerRow, bytesPerImage, nullptr));
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger, NSUInteger))(
        self, _cmd, source, sourceSlice, sourceLevel, sourceOrigin, sourceSize, destination, offset,
        bytesPerRow, bytesPerImage);
}

void B_copyTextureToBufferOptions(id self, SEL _cmd, id source, NSUInteger sourceSlice, NSUInteger sourceLevel,
                                  MTLOrigin sourceOrigin, MTLSize sourceSize, id destination, NSUInteger offset,
                                  NSUInteger bytesPerRow, NSUInteger bytesPerImage, NSUInteger options) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:"
                      "destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:", self,
                      TextureToBufferArgs(source, sourceSlice, sourceLevel, sourceOrigin, sourceSize, destination,
                                          offset, bytesPerRow, bytesPerImage, &options));
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger, NSUInteger,
                  NSUInteger))(self, _cmd, source, sourceSlice, sourceLevel, sourceOrigin, sourceSize,
                               destination, offset, bytesPerRow, bytesPerImage, options);
}

std::string BufferToTextureArgs(id source, NSUInteger offset, NSUInteger bytesPerRow, NSUInteger bytesPerImage,
                                MTLSize sourceSize, id destination, NSUInteger destinationSlice,
                                NSUInteger destinationLevel, MTLOrigin destinationOrigin, const NSUInteger *options) {
    Args a;
    a.ref("sourceBuffer", source, "MTLBuffer").u("sourceOffset", offset).u("sourceBytesPerRow", bytesPerRow)
     .u("sourceBytesPerImage", bytesPerImage).size("sourceSize", sourceSize)
     .ref("destinationTexture", destination, "MTLTexture").u("destinationSlice", destinationSlice)
     .u("destinationLevel", destinationLevel).origin("destinationOrigin", destinationOrigin);
    if (options != nullptr) a.u("options", *options);
    return a.str();
}

void B_copyBufferToTexture(id self, SEL _cmd, id source, NSUInteger offset, NSUInteger bytesPerRow,
                           NSUInteger bytesPerImage, MTLSize sourceSize, id destination,
                           NSUInteger destinationSlice, NSUInteger destinationLevel, MTLOrigin destinationOrigin) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:"
                      "destinationSlice:destinationLevel:destinationOrigin:", self,
                      BufferToTextureArgs(source, offset, bytesPerRow, bytesPerImage, sourceSize, destination,
                                          destinationSlice, destinationLevel, destinationOrigin, nullptr));
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger, NSUInteger, MTLSize, id, NSUInteger, NSUInteger, MTLOrigin))(
        self, _cmd, source, offset, bytesPerRow, bytesPerImage, sourceSize, destination, destinationSlice,
        destinationLevel, destinationOrigin);
}

void B_copyBufferToTextureOptions(id self, SEL _cmd, id source, NSUInteger offset, NSUInteger bytesPerRow,
                                  NSUInteger bytesPerImage, MTLSize sourceSize, id destination,
                                  NSUInteger destinationSlice, NSUInteger destinationLevel,
                                  MTLOrigin destinationOrigin, NSUInteger options) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:"
                      "destinationSlice:destinationLevel:destinationOrigin:options:", self,
                      BufferToTextureArgs(source, offset, bytesPerRow, bytesPerImage, sourceSize, destination,
                                          destinationSlice, destinationLevel, destinationOrigin, &options));
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger, NSUInteger, MTLSize, id, NSUInteger, NSUInteger, MTLOrigin,
                  NSUInteger))(self, _cmd, source, offset, bytesPerRow, bytesPerImage, sourceSize, destination,
                               destinationSlice, destinationLevel, destinationOrigin, options);
}

void B_copyBufferToBuffer(id self, SEL _cmd, id source, NSUInteger sourceOffset, id destination,
                          NSUInteger destinationOffset, NSUInteger size) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:", self,
                      Args().ref("sourceBuffer", source, "MTLBuffer").u("sourceOffset", sourceOffset)
                            .ref("destinationBuffer", destination, "MTLBuffer").u("destinationOffset", destinationOffset)
                            .u("size", size).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, id, NSUInteger, NSUInteger))(
        self, _cmd, source, sourceOffset, destination, destinationOffset, size);
}

void B_copyTextureToTextureWhole(id self, SEL _cmd, id source, id destination) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyFromTexture:toTexture:", self,
                      Args().ref("sourceTexture", source, "MTLTexture").ref("destinationTexture", destination, "MTLTexture").str());
    }
    ORIG(void (*)(id, SEL, id, id))(self, _cmd, source, destination);
}

void B_copyTextureSlices(id self, SEL _cmd, id source, NSUInteger sourceSlice, NSUInteger sourceLevel,
                         id destination, NSUInteger destinationSlice, NSUInteger destinationLevel,
                         NSUInteger sliceCount, NSUInteger levelCount) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:"
                      "sliceCount:levelCount:", self,
                      Args().ref("sourceTexture", source, "MTLTexture").u("sourceSlice", sourceSlice)
                            .u("sourceLevel", sourceLevel).ref("destinationTexture", destination, "MTLTexture")
                            .u("destinationSlice", destinationSlice).u("destinationLevel", destinationLevel)
                            .u("sliceCount", sliceCount).u("levelCount", levelCount).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger, NSUInteger, NSUInteger))(
        self, _cmd, source, sourceSlice, sourceLevel, destination, destinationSlice, destinationLevel,
        sliceCount, levelCount);
}

void B_generateMipmaps(id self, SEL _cmd, id texture) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("generateMipmapsForTexture:", self, Args().ref("texture", texture, "MTLTexture").str());
    }
    ORIG(void (*)(id, SEL, id))(self, _cmd, texture);
}

void B_fillBuffer(id self, SEL _cmd, id buffer, NSRange range, uint8_t value) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("fillBuffer:range:value:", self,
                      Args().ref("buffer", buffer, "MTLBuffer").range("range", range).u("value", value).str());
    }
    ORIG(void (*)(id, SEL, id, NSRange, uint8_t))(self, _cmd, buffer, range, value);
}

void B_updateFence(id self, SEL _cmd, id fence) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("updateFence:", self, Args().ref("fence", fence, "MTLFence").str());
    ORIG(void (*)(id, SEL, id))(self, _cmd, fence);
}

void B_waitForFence(id self, SEL _cmd, id fence) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) RecordCommand("waitForFence:", self, Args().ref("fence", fence, "MTLFence").str());
    ORIG(void (*)(id, SEL, id))(self, _cmd, fence);
}

void B_optimizeForGPU(id self, SEL _cmd, id texture) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("optimizeContentsForGPUAccess:", self, Args().ref("texture", texture, "MTLTexture").str());
    }
    ORIG(void (*)(id, SEL, id))(self, _cmd, texture);
}

void B_optimizeForGPUSlice(id self, SEL _cmd, id texture, NSUInteger slice, NSUInteger level) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("optimizeContentsForGPUAccess:slice:level:", self,
                      Args().ref("texture", texture, "MTLTexture").u("slice", slice).u("level", level).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, texture, slice, level);
}

void B_optimizeForCPU(id self, SEL _cmd, id texture) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("optimizeContentsForCPUAccess:", self, Args().ref("texture", texture, "MTLTexture").str());
    }
    ORIG(void (*)(id, SEL, id))(self, _cmd, texture);
}

void B_optimizeForCPUSlice(id self, SEL _cmd, id texture, NSUInteger slice, NSUInteger level) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("optimizeContentsForCPUAccess:slice:level:", self,
                      Args().ref("texture", texture, "MTLTexture").u("slice", slice).u("level", level).str());
    }
    ORIG(void (*)(id, SEL, id, NSUInteger, NSUInteger))(self, _cmd, texture, slice, level);
}

void B_resetCommands(id self, SEL _cmd, id icb, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("resetCommandsInBuffer:withRange:", self,
                      Args().ref("indirectCommandBuffer", icb, "MTLIndirectCommandBuffer").range("range", range).str());
    }
    ORIG(void (*)(id, SEL, id, NSRange))(self, _cmd, icb, range);
}

void B_copyIndirectCommandBuffer(id self, SEL _cmd, id source, NSRange range, id destination, NSUInteger index) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("copyIndirectCommandBuffer:sourceRange:destination:destinationIndex:", self,
                      Args().ref("source", source, "MTLIndirectCommandBuffer").range("sourceRange", range)
                            .ref("destination", destination, "MTLIndirectCommandBuffer").u("destinationIndex", index).str());
    }
    ORIG(void (*)(id, SEL, id, NSRange, id, NSUInteger))(self, _cmd, source, range, destination, index);
}

void B_optimizeIndirectCommandBuffer(id self, SEL _cmd, id icb, NSRange range) {
    Reentry reentry(self, _cmd);
    if (Rec(reentry)) {
        RecordCommand("optimizeIndirectCommandBuffer:withRange:", self,
                      Args().ref("indirectCommandBuffer", icb, "MTLIndirectCommandBuffer").range("range", range).str());
    }
    ORIG(void (*)(id, SEL, id, NSRange))(self, _cmd, icb, range);
}

}  // namespace

// --------------------------------------------------------------------------------------------
// Installation

void HookCommonEncoderMethods(Class cls) {
    Hook(cls, @selector(endEncoding), (IMP)E_endEncoding);
    Hook(cls, @selector(pushDebugGroup:), (IMP)E_pushDebugGroup);
    Hook(cls, @selector(popDebugGroup), (IMP)E_popDebugGroup);
    Hook(cls, @selector(insertDebugSignpost:), (IMP)E_insertDebugSignpost);
    Hook(cls, @selector(setLabel:), (IMP)Replaced_setLabel);
}

void HookRenderEncoderClass(id encoder) {
    if (encoder == nil) return;
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking render encoder class %s", class_getName(cls));
    HookCommonEncoderMethods(cls);
    Hook(cls, @selector(setRenderPipelineState:), (IMP)R_setRenderPipelineState);
    Hook(cls, @selector(setVertexBytes:length:atIndex:), (IMP)R_setVertexBytes);
    Hook(cls, @selector(setVertexBuffer:offset:atIndex:), (IMP)R_setVertexBuffer);
    Hook(cls, @selector(setVertexBufferOffset:atIndex:), (IMP)R_setVertexBufferOffset);
    Hook(cls, @selector(setVertexBuffers:offsets:withRange:), (IMP)R_setVertexBuffers);
    Hook(cls, @selector(setVertexTexture:atIndex:), (IMP)R_setVertexTexture);
    Hook(cls, @selector(setVertexTextures:withRange:), (IMP)R_setVertexTextures);
    Hook(cls, @selector(setVertexSamplerState:atIndex:), (IMP)R_setVertexSamplerState);
    Hook(cls, @selector(setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:), (IMP)R_setVertexSamplerStateLod);
    Hook(cls, @selector(setVertexSamplerStates:withRange:), (IMP)R_setVertexSamplerStates);
    Hook(cls, @selector(setViewport:), (IMP)R_setViewport);
    Hook(cls, @selector(setViewports:count:), (IMP)R_setViewports);
    Hook(cls, @selector(setFrontFacingWinding:), (IMP)R_setFrontFacingWinding);
    Hook(cls, @selector(setVertexAmplificationCount:viewMappings:), (IMP)R_setVertexAmplificationCount);
    Hook(cls, @selector(setCullMode:), (IMP)R_setCullMode);
    Hook(cls, @selector(setDepthClipMode:), (IMP)R_setDepthClipMode);
    Hook(cls, @selector(setDepthBias:slopeScale:clamp:), (IMP)R_setDepthBias);
    Hook(cls, @selector(setScissorRect:), (IMP)R_setScissorRect);
    Hook(cls, @selector(setScissorRects:count:), (IMP)R_setScissorRects);
    Hook(cls, @selector(setTriangleFillMode:), (IMP)R_setTriangleFillMode);
    Hook(cls, @selector(setFragmentBytes:length:atIndex:), (IMP)R_setFragmentBytes);
    Hook(cls, @selector(setFragmentBuffer:offset:atIndex:), (IMP)R_setFragmentBuffer);
    Hook(cls, @selector(setFragmentBufferOffset:atIndex:), (IMP)R_setFragmentBufferOffset);
    Hook(cls, @selector(setFragmentBuffers:offsets:withRange:), (IMP)R_setFragmentBuffers);
    Hook(cls, @selector(setFragmentTexture:atIndex:), (IMP)R_setFragmentTexture);
    Hook(cls, @selector(setFragmentTextures:withRange:), (IMP)R_setFragmentTextures);
    Hook(cls, @selector(setFragmentSamplerState:atIndex:), (IMP)R_setFragmentSamplerState);
    Hook(cls, @selector(setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:), (IMP)R_setFragmentSamplerStateLod);
    Hook(cls, @selector(setFragmentSamplerStates:withRange:), (IMP)R_setFragmentSamplerStates);
    Hook(cls, @selector(setBlendColorRed:green:blue:alpha:), (IMP)R_setBlendColor);
    Hook(cls, @selector(setDepthStencilState:), (IMP)R_setDepthStencilState);
    Hook(cls, @selector(setStencilReferenceValue:), (IMP)R_setStencilReferenceValue);
    Hook(cls, @selector(setStencilFrontReferenceValue:backReferenceValue:), (IMP)R_setStencilFrontBackReference);
    Hook(cls, @selector(setVisibilityResultMode:offset:), (IMP)R_setVisibilityResultMode);
    Hook(cls, @selector(setColorStoreAction:atIndex:), (IMP)R_setColorStoreAction);
    Hook(cls, @selector(setDepthStoreAction:), (IMP)R_setDepthStoreAction);
    Hook(cls, @selector(setStencilStoreAction:), (IMP)R_setStencilStoreAction);
    Hook(cls, @selector(setColorStoreActionOptions:atIndex:), (IMP)R_setColorStoreActionOptions);
    Hook(cls, @selector(setDepthStoreActionOptions:), (IMP)R_setDepthStoreActionOptions);
    Hook(cls, @selector(setStencilStoreActionOptions:), (IMP)R_setStencilStoreActionOptions);
    Hook(cls, @selector(drawPrimitives:vertexStart:vertexCount:), (IMP)R_drawPrimitives3);
    Hook(cls, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:), (IMP)R_drawPrimitives4);
    Hook(cls, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:), (IMP)R_drawPrimitives5);
    Hook(cls, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:),
         (IMP)R_drawIndexed5);
    Hook(cls, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:),
         (IMP)R_drawIndexed6);
    Hook(cls, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:
                                             baseVertex:baseInstance:),
         (IMP)R_drawIndexed8);
    Hook(cls, @selector(drawPrimitives:indirectBuffer:indirectBufferOffset:), (IMP)R_drawPrimitivesIndirect);
    Hook(cls, @selector(drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:
                                             indirectBufferOffset:),
         (IMP)R_drawIndexedIndirect);
    Hook(cls, @selector(drawPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:instanceCount:
                                   baseInstance:),
         (IMP)R_drawPatches);
    Hook(cls, @selector(drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:),
         (IMP)R_drawPatchesIndirect);
    Hook(cls, @selector(drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:
                                          controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:
                                          baseInstance:),
         (IMP)R_drawIndexedPatches);
    Hook(cls, @selector(drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:
                                          controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:),
         (IMP)R_drawIndexedPatchesIndirect);
    Hook(cls, sel_registerName("drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:"),
         (IMP)R_drawMeshThreadgroups);
    Hook(cls, sel_registerName("drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:"),
         (IMP)R_drawMeshThreads);
    Hook(cls, sel_registerName("drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:"
                               "threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:"),
         (IMP)R_drawMeshThreadgroupsIndirect);
    Hook(cls, sel_registerName("setObjectBytes:length:atIndex:"), (IMP)R_setObjectBytes);
    Hook(cls, sel_registerName("setObjectBuffer:offset:atIndex:"), (IMP)R_setObjectBuffer);
    Hook(cls, sel_registerName("setObjectTexture:atIndex:"), (IMP)R_setObjectTexture);
    Hook(cls, sel_registerName("setObjectSamplerState:atIndex:"), (IMP)R_setObjectSamplerState);
    Hook(cls, sel_registerName("setObjectThreadgroupMemoryLength:atIndex:"), (IMP)R_setObjectThreadgroupMemoryLength);
    Hook(cls, sel_registerName("setMeshBytes:length:atIndex:"), (IMP)R_setMeshBytes);
    Hook(cls, sel_registerName("setMeshBuffer:offset:atIndex:"), (IMP)R_setMeshBuffer);
    Hook(cls, sel_registerName("setMeshTexture:atIndex:"), (IMP)R_setMeshTexture);
    Hook(cls, sel_registerName("setMeshSamplerState:atIndex:"), (IMP)R_setMeshSamplerState);
    Hook(cls, @selector(setTileBytes:length:atIndex:), (IMP)R_setTileBytes);
    Hook(cls, @selector(setTileBuffer:offset:atIndex:), (IMP)R_setTileBuffer);
    Hook(cls, @selector(setTileTexture:atIndex:), (IMP)R_setTileTexture);
    Hook(cls, @selector(setTileSamplerState:atIndex:), (IMP)R_setTileSamplerState);
    Hook(cls, @selector(dispatchThreadsPerTile:), (IMP)R_dispatchThreadsPerTile);
    Hook(cls, @selector(setThreadgroupMemoryLength:offset:atIndex:), (IMP)R_setThreadgroupMemoryLength);
    Hook(cls, @selector(useResource:usage:), (IMP)R_useResource);
    Hook(cls, @selector(useResource:usage:stages:), (IMP)R_useResourceStages);
    Hook(cls, @selector(useResources:count:usage:), (IMP)R_useResources);
    Hook(cls, @selector(useResources:count:usage:stages:), (IMP)R_useResourcesStages);
    Hook(cls, @selector(useHeap:), (IMP)R_useHeap);
    Hook(cls, @selector(useHeap:stages:), (IMP)R_useHeapStages);
    Hook(cls, @selector(useHeaps:count:), (IMP)R_useHeaps);
    Hook(cls, @selector(useHeaps:count:stages:), (IMP)R_useHeapsStages);
    Hook(cls, @selector(executeCommandsInBuffer:withRange:), (IMP)R_executeCommandsInBuffer);
    Hook(cls, @selector(executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:), (IMP)R_executeCommandsIndirect);
    Hook(cls, @selector(memoryBarrierWithScope:afterStages:beforeStages:), (IMP)R_memoryBarrierScope);
    Hook(cls, @selector(memoryBarrierWithResources:count:afterStages:beforeStages:), (IMP)R_memoryBarrierResources);
    Hook(cls, @selector(updateFence:afterStages:), (IMP)R_updateFence);
    Hook(cls, @selector(waitForFence:beforeStages:), (IMP)R_waitForFence);
    Hook(cls, sel_registerName("textureBarrier"), (IMP)R_textureBarrier);
    Hook(cls, @selector(setTessellationFactorBuffer:offset:instanceStride:), (IMP)R_setTessellationFactorBuffer);
    Hook(cls, @selector(setTessellationFactorScale:), (IMP)R_setTessellationFactorScale);
}

void HookComputeEncoderClass(id encoder) {
    if (encoder == nil) return;
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking compute encoder class %s", class_getName(cls));
    HookCommonEncoderMethods(cls);
    Hook(cls, @selector(setComputePipelineState:), (IMP)C_setComputePipelineState);
    Hook(cls, @selector(setBytes:length:atIndex:), (IMP)C_setBytes);
    Hook(cls, @selector(setBuffer:offset:atIndex:), (IMP)C_setBuffer);
    Hook(cls, @selector(setBufferOffset:atIndex:), (IMP)C_setBufferOffset);
    Hook(cls, @selector(setBuffers:offsets:withRange:), (IMP)C_setBuffers);
    Hook(cls, @selector(setTexture:atIndex:), (IMP)C_setTexture);
    Hook(cls, @selector(setTextures:withRange:), (IMP)C_setTextures);
    Hook(cls, @selector(setSamplerState:atIndex:), (IMP)C_setSamplerState);
    Hook(cls, @selector(setSamplerState:lodMinClamp:lodMaxClamp:atIndex:), (IMP)C_setSamplerStateLod);
    Hook(cls, @selector(setSamplerStates:withRange:), (IMP)C_setSamplerStates);
    Hook(cls, @selector(setThreadgroupMemoryLength:atIndex:), (IMP)C_setThreadgroupMemoryLength);
    Hook(cls, @selector(setImageblockWidth:height:), (IMP)C_setImageblockWidth);
    Hook(cls, @selector(setStageInRegion:), (IMP)C_setStageInRegion);
    Hook(cls, @selector(setStageInRegionWithIndirectBuffer:indirectBufferOffset:), (IMP)C_setStageInRegionIndirect);
    Hook(cls, @selector(dispatchThreadgroups:threadsPerThreadgroup:), (IMP)C_dispatchThreadgroups);
    Hook(cls, @selector(dispatchThreads:threadsPerThreadgroup:), (IMP)C_dispatchThreads);
    Hook(cls, @selector(dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:),
         (IMP)C_dispatchThreadgroupsIndirect);
    Hook(cls, @selector(updateFence:), (IMP)C_updateFence);
    Hook(cls, @selector(waitForFence:), (IMP)C_waitForFence);
    Hook(cls, @selector(useResource:usage:), (IMP)C_useResource);
    Hook(cls, @selector(useResources:count:usage:), (IMP)C_useResources);
    Hook(cls, @selector(useHeap:), (IMP)C_useHeap);
    Hook(cls, @selector(useHeaps:count:), (IMP)C_useHeaps);
    Hook(cls, @selector(executeCommandsInBuffer:withRange:), (IMP)C_executeCommandsInBuffer);
    Hook(cls, @selector(executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:), (IMP)C_executeCommandsIndirect);
    Hook(cls, @selector(memoryBarrierWithScope:), (IMP)C_memoryBarrierScope);
    Hook(cls, @selector(memoryBarrierWithResources:count:), (IMP)C_memoryBarrierResources);
    Hook(cls, sel_registerName("setAccelerationStructure:atBufferIndex:"), (IMP)C_setAccelerationStructure);
}

void HookBlitEncoderClass(id encoder) {
    if (encoder == nil) return;
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking blit encoder class %s", class_getName(cls));
    HookCommonEncoderMethods(cls);
    Hook(cls, @selector(synchronizeResource:), (IMP)B_synchronizeResource);
    Hook(cls, @selector(synchronizeTexture:slice:level:), (IMP)B_synchronizeTexture);
    Hook(cls, @selector(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:
                                       destinationLevel:destinationOrigin:),
         (IMP)B_copyTextureToTexture);
    Hook(cls, @selector(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:
                                       destinationBytesPerRow:destinationBytesPerImage:),
         (IMP)B_copyTextureToBuffer);
    Hook(cls, @selector(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:
                                       destinationBytesPerRow:destinationBytesPerImage:options:),
         (IMP)B_copyTextureToBufferOptions);
    Hook(cls, @selector(copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:
                                      destinationSlice:destinationLevel:destinationOrigin:),
         (IMP)B_copyBufferToTexture);
    Hook(cls, @selector(copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:
                                      destinationSlice:destinationLevel:destinationOrigin:options:),
         (IMP)B_copyBufferToTextureOptions);
    Hook(cls, @selector(copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:), (IMP)B_copyBufferToBuffer);
    Hook(cls, @selector(copyFromTexture:toTexture:), (IMP)B_copyTextureToTextureWhole);
    Hook(cls, @selector(copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:sliceCount:
                                       levelCount:),
         (IMP)B_copyTextureSlices);
    Hook(cls, @selector(generateMipmapsForTexture:), (IMP)B_generateMipmaps);
    Hook(cls, @selector(fillBuffer:range:value:), (IMP)B_fillBuffer);
    Hook(cls, @selector(updateFence:), (IMP)B_updateFence);
    Hook(cls, @selector(waitForFence:), (IMP)B_waitForFence);
    Hook(cls, @selector(optimizeContentsForGPUAccess:), (IMP)B_optimizeForGPU);
    Hook(cls, @selector(optimizeContentsForGPUAccess:slice:level:), (IMP)B_optimizeForGPUSlice);
    Hook(cls, @selector(optimizeContentsForCPUAccess:), (IMP)B_optimizeForCPU);
    Hook(cls, @selector(optimizeContentsForCPUAccess:slice:level:), (IMP)B_optimizeForCPUSlice);
    Hook(cls, @selector(resetCommandsInBuffer:withRange:), (IMP)B_resetCommands);
    Hook(cls, @selector(copyIndirectCommandBuffer:sourceRange:destination:destinationIndex:),
         (IMP)B_copyIndirectCommandBuffer);
    Hook(cls, @selector(optimizeIndirectCommandBuffer:withRange:), (IMP)B_optimizeIndirectCommandBuffer);
}

void HookOtherEncoderClass(id encoder) {
    if (encoder == nil) return;
    Class cls = object_getClass(encoder);
    if (!FirstSighting(cls)) return;
    Log("hooking encoder class %s", class_getName(cls));
    HookCommonEncoderMethods(cls);
}

}  // namespace mtlinsp
