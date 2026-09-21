// The command half of the Metal replay (mtl_replayer.h): opening and closing encoders, the
// commands of each encoder kind, and the render targets read back and compared.
//
// Every command has the shape the D3D12 replay's dispatch has (dx_replayer.cpp): decode the
// arguments once into locals, send the message, then spell the same locals as source. Decoding
// once is what keeps the exported program doing what the replay did rather than what the JSON
// said — and it is why this is one chain rather than a table.
#include "mtl_replayer.h"

#include <algorithm>
#include <cstring>

#include "formats.h"

#include "mtl_exporter.h"
#include "mtl_raytracing.h"
#include "mtl_reflect.h"

namespace mtlreplay {
namespace {

/** A JSON array of object references, resolved to the replay's objects. */
std::vector<id> ObjectArray(const Decoder& d, const char* key) {
    std::vector<id> out;
    const JValue* array = d.Get(key);
    if (!array || !array->IsArray()) return out;
    out.reserve(array->count);
    for (uint32_t i = 0; i < array->count; ++i) out.push_back(d.Resolve(IdOf(&array->items[i])));
    return out;
}

/**
 * A target the pass would discard is stored instead, so there is something to read back.
 *
 * The same rule the capture applied when it took the frame (ForceStore in
 * src/metal/src/hooks_command_buffer.mm), and it has to be the same: a target the capture read is
 * one the capture forced, and a target it declined to force it also declined to read.
 */
void ForceStore(MTLRenderPassAttachmentDescriptor* a) {
    if (a == nil || a.texture == nil || a.storeAction != MTLStoreActionDontCare) return;
    if (a.texture.storageMode == MTLStorageModeMemoryless || a.texture.sampleCount > 1) return;
    a.storeAction = MTLStoreActionStore;
}

std::vector<uint64_t> UintArray(const Decoder& d, const char* key) {
    std::vector<uint64_t> out;
    const JValue* array = d.Get(key);
    if (!array || !array->IsArray()) return out;
    out.reserve(array->count);
    for (uint32_t i = 0; i < array->count; ++i) out.push_back(array->items[i].Uint());
    return out;
}

} // namespace

// ------------------------------------------------------------------------------------------
// Encoders

MTLRenderPassDescriptor* MtlReplayer::BuildRenderPass(const Decoder& d) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassDescriptor* defaults = [MTLRenderPassDescriptor renderPassDescriptor];
    FillVisitor fill(d);
    Reflect(fill, pass, defaults);

    const JValue* colors = d.Get("colorAttachments");
    if (colors && colors->IsArray()) {
        for (uint32_t i = 0; i < colors->count; ++i) {
            const Decoder a(&colors->items[i], _env);
            const NSUInteger slot = (NSUInteger)a.Uint("index");
            FillVisitor f(a);
            Reflect(f, pass.colorAttachments[slot], defaults.colorAttachments[slot]);
            if (a.Has("clearColor")) pass.colorAttachments[slot].clearColor = a.ClearColor("clearColor");
            ForceStore(pass.colorAttachments[slot]);
        }
    }
    const Decoder depth = d.Nested("depthAttachment");
    if (depth.Json()) {
        FillVisitor f(depth);
        Reflect(f, pass.depthAttachment, defaults.depthAttachment);
        if (depth.Has("clearDepth")) pass.depthAttachment.clearDepth = depth.Double("clearDepth");
        ForceStore(pass.depthAttachment);
    }
    const Decoder stencil = d.Nested("stencilAttachment");
    if (stencil.Json()) {
        FillVisitor f(stencil);
        Reflect(f, pass.stencilAttachment, defaults.stencilAttachment);
        if (stencil.Has("clearStencil")) pass.stencilAttachment.clearStencil = (uint32_t)stencil.Uint("clearStencil");
        ForceStore(pass.stencilAttachment);
    }

    return pass;
}

bool MtlReplayer::OpenEncoder(const std::string& m, const Decoder& d, uint32_t index, uint64_t encoderId) {
    if (!_commandBuffer) return false;

    // A parallel encoder's sub-encoder: the only encoder opened on another encoder rather than on
    // the command buffer, and the only one whose end is not a pass's (E_endEncoding in
    // src/metal/src/hooks_encoders.mm records no endEncoding for it).
    const bool sub = m == "renderCommandEncoder";
    if (!sub && _encoder) EndEncoder();

    OpenPass pass;
    pass.encoderId = encoderId;
    id encoder = nil;
    std::string kind;

    if (m == "renderCommandEncoderWithDescriptor:" || m == "parallelRenderCommandEncoderWithDescriptor:") {
        MTLRenderPassDescriptor* descriptor = BuildRenderPass(d);
        pass.renderPass = descriptor;
        pass.parallel = m[0] == 'p';
        pass.passIndex = _passCounter++;
        encoder = pass.parallel ? (id)[_commandBuffer parallelRenderCommandEncoderWithDescriptor:descriptor]
                                : (id)[_commandBuffer renderCommandEncoderWithDescriptor:descriptor];
        kind = pass.parallel ? "id<MTLParallelRenderCommandEncoder>" : "id<MTLRenderCommandEncoder>";
        if (_x && encoder) {
            pass.variable = "encoder" + std::to_string(index);
            const std::string call = pass.parallel ? "parallelRenderCommandEncoderWithDescriptor:"
                                                   : "renderCommandEncoderWithDescriptor:";
            MTLRenderPassDescriptor* defaults = [MTLRenderPassDescriptor renderPassDescriptor];
            // The encoder is declared outside the block the descriptor is built in: the block keeps
            // each pass's locals from colliding, and the encoder has to outlive it — every command
            // of the pass is sent to it.
            DeclareEncoder(kind, pass.variable);
            _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) {
                const std::string pv = w.Local("pass");
                w.Line("MTLRenderPassDescriptor* " + pv + " = [MTLRenderPassDescriptor renderPassDescriptor];");
                EmitVisitor emit(w, pv + ".");
                Reflect(emit, descriptor, defaults);
                if (HasColorAttachments(d)) {
                    const JValue* colors = d.Get("colorAttachments");
                    for (uint32_t i = 0; i < colors->count; ++i) {
                        const NSUInteger slot = (NSUInteger)Decoder(&colors->items[i], _env).Uint("index");
                        const std::string path = pv + ".colorAttachments[" + std::to_string(slot) + "].";
                        EmitVisitor e(w, path);
                        Reflect(e, descriptor.colorAttachments[slot], defaults.colorAttachments[slot]);
                        const MTLClearColor c = descriptor.colorAttachments[slot].clearColor;
                        if (descriptor.colorAttachments[slot].loadAction == MTLLoadActionClear) {
                            w.Line(path + "clearColor = MTLClearColorMake(" + Source::Float(c.red) + ", " +
                                   Source::Float(c.green) + ", " + Source::Float(c.blue) + ", " +
                                   Source::Float(c.alpha) + ");");
                        }
                    }
                }
                if (descriptor.depthAttachment.texture) {
                    EmitVisitor e(w, pv + ".depthAttachment.");
                    Reflect(e, descriptor.depthAttachment, defaults.depthAttachment);
                    w.Line(pv + ".depthAttachment.clearDepth = " +
                           Source::Float(descriptor.depthAttachment.clearDepth) + ";");
                }
                if (descriptor.stencilAttachment.texture) {
                    EmitVisitor e(w, pv + ".stencilAttachment.");
                    Reflect(e, descriptor.stencilAttachment, defaults.stencilAttachment);
                    w.Line(pv + ".stencilAttachment.clearStencil = " +
                           Source::Uint(descriptor.stencilAttachment.clearStencil) + ";");
                }
                w.Line(pass.variable + " = [" + _commandBufferVar + " " + call + pv + "];");
            });
        }
    } else if (sub) {
        id<MTLParallelRenderCommandEncoder> parent = (id<MTLParallelRenderCommandEncoder>)_encoder;
        if (!parent || ![parent conformsToProtocol:@protocol(MTLParallelRenderCommandEncoder)]) {
            LeftOut(index, m, "renderCommandEncoder came from no parallel encoder");
            return true;
        }
        _parallel = _encoder;
        _parallelPass = _pass;
        pass.parentId = _parallelPass.encoderId;
        pass.passIndex = _parallelPass.passIndex;
        pass.renderPass = _parallelPass.renderPass;
        encoder = [parent renderCommandEncoder];
        kind = "id<MTLRenderCommandEncoder>";
        if (_x && encoder) {
            pass.variable = "encoder" + std::to_string(index);
            DeclareEncoder(kind, pass.variable);
            _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) {
                w.Line(pass.variable + " = [" + _parallelPass.variable + " renderCommandEncoder];");
            });
        }
    } else if (m.compare(0, 20, "computeCommandEncoder") == 0 || m == "computeCommandEncoder") {
        pass.passIndex = _passCounter++;
        MTLComputePassDescriptor* descriptor = [MTLComputePassDescriptor computePassDescriptor];
        MTLComputePassDescriptor* defaults = [MTLComputePassDescriptor computePassDescriptor];
        FillVisitor f(d);
        Reflect(f, descriptor, defaults);
        encoder = [_commandBuffer computeCommandEncoderWithDescriptor:descriptor];
        kind = "id<MTLComputeCommandEncoder>";
        if (_x && encoder) {
            pass.variable = "encoder" + std::to_string(index);
            DeclareEncoder(kind, pass.variable);
            _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) {
                const std::string pv = w.Local("pass");
                w.Line("MTLComputePassDescriptor* " + pv + " = [MTLComputePassDescriptor computePassDescriptor];");
                EmitVisitor emit(w, pv + ".");
                Reflect(emit, descriptor, defaults);
                w.Line(pass.variable + " = [" + _commandBufferVar +
                       " computeCommandEncoderWithDescriptor:" + pv + "];");
            });
        }
    } else if (m.compare(0, 18, "blitCommandEncoder") == 0) {
        pass.passIndex = _passCounter++;
        encoder = [_commandBuffer blitCommandEncoder];
        kind = "id<MTLBlitCommandEncoder>";
        if (_x && encoder) {
            pass.variable = "encoder" + std::to_string(index);
            DeclareEncoder(kind, pass.variable);
            _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) {
                w.Line(pass.variable + " = [" + _commandBufferVar + " blitCommandEncoder];");
            });
        }
    } else if (m.compare(0, 35, "accelerationStructureCommandEncoder") == 0) {
        // The builds, refits and copies the capture records (src/metal/src/raytracing.mm). Opened
        // without the descriptor form's pass descriptor: that exists to give the pass a timing
        // slot, which the replay has no use for.
        pass.passIndex = _passCounter++;
        if (@available(macOS 11.0, *)) {
            encoder = [_commandBuffer accelerationStructureCommandEncoder];
            kind = "id<MTLAccelerationStructureCommandEncoder>";
            if (_x && encoder) {
                pass.variable = "encoder" + std::to_string(index);
                DeclareEncoder(kind, pass.variable);
                _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) {
                    w.Line(pass.variable + " = [" + _commandBufferVar + " accelerationStructureCommandEncoder];");
                });
            }
        } else {
            LeftOut(index, m, "acceleration structures need macOS 11");
            return true;
        }
    } else {
        // A resource state encoder: a pass the capture records no commands in yet, so opening one
        // would add an empty encoder and nothing else.
        LeftOut(index, m, "the replay does not open this kind of encoder");
        _passCounter++;
        return true;
    }

    if (!encoder) {
        LeftOut(index, m, "the encoder was not created");
        return true;
    }
    _encoder = encoder;
    _pass = pass;
    if (_x) _x->CountCommand();
    return true;
}

/**
 * An encoder's variable, as a global of the exported project rather than a local of `Frame`.
 *
 * `Frame` is cut into parts, and a part is a function, so a pass with more commands than a part
 * holds — an engine frame has many — would declare its encoder in one function and send to it from
 * the next. A global is in scope wherever the split lands.
 */
void MtlReplayer::DeclareEncoder(const std::string& type, const std::string& name) {
    if (_x) _x->Global(type, name);
}

/** Whether the pass descriptor's JSON names any color attachment. */
bool MtlReplayer::HasColorAttachments(const Decoder& d) const {
    const JValue* colors = d.Get("colorAttachments");
    return colors && colors->IsArray() && colors->count;
}

// ------------------------------------------------------------------------------------------
// Commands on the command buffer

bool MtlReplayer::CommandBufferCommand(const std::string& m, const Decoder& d, uint32_t index) {
    if (m == "commit") {
        Commit(false);
        return true;
    }
    if (m == "enqueue" || m == "waitUntilCompleted" || m == "waitUntilScheduled") {
        // Scheduling, not work: the replay commits in order and waits for every buffer at the end.
        return true;
    }
    if (m == "present" || m.compare(0, 16, "presentDrawable:") == 0) {
        LeftOut(index, m, "an exported frame has no window to present to");
        return true;
    }
    if (m == "setLabel:" && !_encoder) {
        if (NSString* label = d.NSStr("label")) {
            _commandBuffer.label = label;
            if (_x) {
                _x->Block(MtlExporter::Frame, "", [&](Source& w) {
                    w.Line(_commandBufferVar + ".label = " + Source::NSString(label.UTF8String, true) + ";");
                });
            }
        }
        return true;
    }
    if (m == "encodeSignalEvent:value:" || m == "encodeWaitForEvent:value:") {
        id<MTLEvent> event = (id<MTLEvent>)d.Object("event");
        const uint64_t value = d.Uint("value");
        if (!event) {
            LeftOut(index, m, "the event is not in the replay");
            return true;
        }
        const bool signal = m[6] == 'S';
        if (signal) [_commandBuffer encodeSignalEvent:event value:value];
        else [_commandBuffer encodeWaitForEvent:event value:value];
        if (_x) {
            const std::string call = signal ? "encodeSignalEvent:" : "encodeWaitForEvent:";
            const std::string name = _x->NameOf(event);
            _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) {
                w.Line("[" + _commandBufferVar + " " + call + name + " value:" + Source::Uint(value) + "];");
            });
            _x->CountCommand();
        }
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------
// Commands every encoder has

bool MtlReplayer::CommonCommand(const std::string& m, const Decoder& d, uint32_t index) {
    id<MTLCommandEncoder> e = (id<MTLCommandEncoder>)_encoder;
    const std::string var = _pass.variable;
    auto emit = [&](const std::string& statement) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) { w.Line(statement); });
        _x->CountCommand();
    };

    if (m == "endEncoding") {
        EndEncoder();
        if (_x) _x->CountCommand();
        return true;
    }
    if (m == "setLabel:") {
        NSString* label = d.NSStr("label");
        if (label) {
            e.label = label;
            emit(var + ".label = " + Source::NSString(label.UTF8String, true) + ";");
        }
        return true;
    }
    if (m == "pushDebugGroup:") {
        NSString* label = d.NSStr("label");
        [e pushDebugGroup:label ?: @""];
        emit("[" + var + " pushDebugGroup:" + Source::NSString(d.Str("label"), true) + "];");
        return true;
    }
    if (m == "popDebugGroup") {
        [e popDebugGroup];
        emit("[" + var + " popDebugGroup];");
        return true;
    }
    if (m == "insertDebugSignpost:") {
        NSString* label = d.NSStr("label");
        [e insertDebugSignpost:label ?: @""];
        emit("[" + var + " insertDebugSignpost:" + Source::NSString(d.Str("label"), true) + "];");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------
// Render

bool MtlReplayer::RenderCommand(const std::string& m, const Decoder& d, uint32_t index) {
    id<MTLRenderCommandEncoder> e = (id<MTLRenderCommandEncoder>)_encoder;
    const std::string var = _pass.variable;
    auto emit = [&](const std::string& statement) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) { w.Line(statement); });
        _x->CountCommand();
    };
    auto emitWith = [&](const std::function<void(Source&)>& body) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", body);
        _x->CountCommand();
    };

    // ---- state
    if (m == "setRenderPipelineState:") {
        id<MTLRenderPipelineState> p = (id<MTLRenderPipelineState>)d.Object("pipeline");
        if (!p) { LeftOut(index, m, "the pipeline is not in the replay"); return true; }
        [e setRenderPipelineState:p];
        emit("[" + var + " setRenderPipelineState:" + ExportName(p) + "];");
        return true;
    }
    if (m == "setDepthStencilState:") {
        id<MTLDepthStencilState> s = (id<MTLDepthStencilState>)d.Object("depthStencilState");
        [e setDepthStencilState:s];
        emit("[" + var + " setDepthStencilState:" + ExportName(s) + "];");
        return true;
    }
    if (m == "setCullMode:") {
        const MTLCullMode v = (MTLCullMode)d.Enum("cullMode", MTL_TABLE(MTLCullMode));
        [e setCullMode:v];
        emit("[" + var + " setCullMode:" + Source::Enum(MTL_TABLE(MTLCullMode), v) + "];");
        return true;
    }
    if (m == "setFrontFacingWinding:") {
        const MTLWinding v = (MTLWinding)d.Enum("winding", MTL_TABLE(MTLWinding));
        [e setFrontFacingWinding:v];
        emit("[" + var + " setFrontFacingWinding:" + Source::Enum(MTL_TABLE(MTLWinding), v) + "];");
        return true;
    }
    if (m == "setTriangleFillMode:") {
        const MTLTriangleFillMode v = (MTLTriangleFillMode)d.Enum("fillMode", MTL_TABLE(MTLTriangleFillMode));
        [e setTriangleFillMode:v];
        emit("[" + var + " setTriangleFillMode:" + Source::Enum(MTL_TABLE(MTLTriangleFillMode), v) + "];");
        return true;
    }
    if (m == "setDepthClipMode:") {
        const MTLDepthClipMode v = (MTLDepthClipMode)d.Enum("depthClipMode", MTL_TABLE(MTLDepthClipMode));
        [e setDepthClipMode:v];
        emit("[" + var + " setDepthClipMode:" + Source::Enum(MTL_TABLE(MTLDepthClipMode), v) + "];");
        return true;
    }
    if (m == "setDepthBias:slopeScale:clamp:") {
        const float bias = (float)d.Double("depthBias"), slope = (float)d.Double("slopeScale"), clamp = (float)d.Double("clamp");
        [e setDepthBias:bias slopeScale:slope clamp:clamp];
        emit("[" + var + " setDepthBias:" + Source::Float(bias) + " slopeScale:" + Source::Float(slope) +
             " clamp:" + Source::Float(clamp) + "];");
        return true;
    }
    if (m == "setBlendColorRed:green:blue:alpha:") {
        const float r = (float)d.Double("red"), g = (float)d.Double("green"), b = (float)d.Double("blue"), a = (float)d.Double("alpha");
        [e setBlendColorRed:r green:g blue:b alpha:a];
        emit("[" + var + " setBlendColorRed:" + Source::Float(r) + " green:" + Source::Float(g) +
             " blue:" + Source::Float(b) + " alpha:" + Source::Float(a) + "];");
        return true;
    }
    if (m == "setStencilReferenceValue:") {
        const uint32_t v = (uint32_t)d.Uint("referenceValue");
        [e setStencilReferenceValue:v];
        emit("[" + var + " setStencilReferenceValue:" + Source::Uint(v) + "];");
        return true;
    }
    if (m == "setStencilFrontReferenceValue:backReferenceValue:") {
        const uint32_t f = (uint32_t)d.Uint("frontReferenceValue"), b = (uint32_t)d.Uint("backReferenceValue");
        [e setStencilFrontReferenceValue:f backReferenceValue:b];
        emit("[" + var + " setStencilFrontReferenceValue:" + Source::Uint(f) + " backReferenceValue:" + Source::Uint(b) + "];");
        return true;
    }
    if (m == "setViewport:") {
        const MTLViewport v = d.Viewport("viewport");
        [e setViewport:v];
        emit("[" + var + " setViewport:(MTLViewport){" + Source::Float(v.originX) + ", " + Source::Float(v.originY) +
             ", " + Source::Float(v.width) + ", " + Source::Float(v.height) + ", " + Source::Float(v.znear) + ", " +
             Source::Float(v.zfar) + "}];");
        return true;
    }
    if (m == "setScissorRect:") {
        const MTLScissorRect r = d.Scissor("rect");
        [e setScissorRect:r];
        emit("[" + var + " setScissorRect:(MTLScissorRect){" + Source::Uint(r.x) + ", " + Source::Uint(r.y) + ", " +
             Source::Uint(r.width) + ", " + Source::Uint(r.height) + "}];");
        return true;
    }
    if (m == "setViewports:count:" || m == "setScissorRects:count:") {
        const bool viewports = m[3] == 'V';
        const JValue* list = d.Get(viewports ? "viewports" : "rects");
        std::vector<MTLViewport> vs;
        std::vector<MTLScissorRect> rs;
        if (list && list->IsArray()) {
            for (uint32_t i = 0; i < list->count; ++i) {
                const Decoder one(&list->items[i], _env);
                if (viewports) vs.push_back(one.Viewport("viewport"));
                else rs.push_back(one.Scissor("rect"));
            }
        }
        if (viewports) [e setViewports:vs.data() count:vs.size()];
        else [e setScissorRects:rs.data() count:rs.size()];
        emitWith([&](Source& w) {
            const std::string name = w.Local(viewports ? "viewports" : "rects");
            std::string items;
            if (viewports) {
                for (size_t i = 0; i < vs.size(); ++i) {
                    items += (i ? ", " : "") + std::string("{") + Source::Float(vs[i].originX) + ", " +
                             Source::Float(vs[i].originY) + ", " + Source::Float(vs[i].width) + ", " +
                             Source::Float(vs[i].height) + ", " + Source::Float(vs[i].znear) + ", " +
                             Source::Float(vs[i].zfar) + "}";
                }
                w.Line("MTLViewport " + name + "[] = {" + items + "};");
                w.Line("[" + var + " setViewports:" + name + " count:" + Source::Uint(vs.size()) + "];");
            } else {
                for (size_t i = 0; i < rs.size(); ++i) {
                    items += (i ? ", " : "") + std::string("{") + Source::Uint(rs[i].x) + ", " +
                             Source::Uint(rs[i].y) + ", " + Source::Uint(rs[i].width) + ", " +
                             Source::Uint(rs[i].height) + "}";
                }
                w.Line("MTLScissorRect " + name + "[] = {" + items + "};");
                w.Line("[" + var + " setScissorRects:" + name + " count:" + Source::Uint(rs.size()) + "];");
            }
        });
        return true;
    }
    if (m == "setVisibilityResultMode:offset:") {
        const MTLVisibilityResultMode mode = (MTLVisibilityResultMode)d.Enum("mode", MTL_TABLE(MTLVisibilityResultMode));
        const uint64_t offset = d.Uint("offset");
        [e setVisibilityResultMode:mode offset:offset];
        emit("[" + var + " setVisibilityResultMode:" + Source::Enum(MTL_TABLE(MTLVisibilityResultMode), mode) +
             " offset:" + Source::Uint(offset) + "];");
        return true;
    }
    if (m == "setVertexAmplificationCount:viewMappings:") {
        const uint64_t count = d.Uint("count");
        [e setVertexAmplificationCount:count viewMappings:nullptr];
        emit("[" + var + " setVertexAmplificationCount:" + Source::Uint(count) + " viewMappings:nullptr];");
        return true;
    }
    if (m == "setTessellationFactorScale:") {
        const float s = (float)d.Double("scale");
        [e setTessellationFactorScale:s];
        emit("[" + var + " setTessellationFactorScale:" + Source::Float(s) + "];");
        return true;
    }
    if (m == "setTessellationFactorBuffer:offset:instanceStride:") {
        id<MTLBuffer> b = (id<MTLBuffer>)d.Object("buffer");
        const uint64_t offset = d.Uint("offset"), stride = d.Uint("instanceStride");
        [e setTessellationFactorBuffer:b offset:offset instanceStride:stride];
        emit("[" + var + " setTessellationFactorBuffer:" + ExportName(b) + " offset:" + Source::Uint(offset) +
             " instanceStride:" + Source::Uint(stride) + "];");
        return true;
    }
    if (m.compare(0, 19, "setColorStoreAction") == 0 || m.compare(0, 19, "setDepthStoreAction") == 0 ||
        m.compare(0, 21, "setStencilStoreAction") == 0) {
        // The store actions a pass rewrote mid-flight. The replay keeps every target stored, so it
        // has something to read back, and says so rather than following a DontCare.
        LeftOut(index, m, "the replay stores every target so it can compare it");
        return true;
    }
    if (m == "setStageInRegion:" || m == "setStageInRegionWithIndirectBuffer:indirectBufferOffset:" ||
        m == "setImageblockWidth:height:" || m == "dispatchThreadsPerTile:") {
        // Tile shading, which the macOS SDK does not put on MTLRenderCommandEncoder. A capture of
        // an application that uses it records these; the replay says so rather than guessing.
        LeftOut(index, m, "tile shading is not on the render encoder this SDK builds against");
        return true;
    }
    if (m == "textureBarrier") {
        LeftOut(index, m, "textureBarrier is deprecated and has no replacement the replay can issue");
        return true;
    }

    // ---- stage bindings: setVertex*/setFragment*, which differ only in the selector
    for (const char* stage : {"Vertex", "Fragment"}) {
        const std::string prefix = std::string("set") + stage;
        if (m.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = m.substr(prefix.size());
        const bool vertex = stage[0] == 'V';

        if (rest == "Buffer:offset:atIndex:") {
            id<MTLBuffer> b = (id<MTLBuffer>)d.Object("buffer");
            const uint64_t offset = d.Uint("offset"), slot = d.Uint("index");
            if (vertex) [e setVertexBuffer:b offset:offset atIndex:slot];
            else [e setFragmentBuffer:b offset:offset atIndex:slot];
            emit("[" + var + " set" + stage + "Buffer:" + ExportName(b) + " offset:" + Source::Uint(offset) +
                 " atIndex:" + Source::Uint(slot) + "];");
            return true;
        }
        if (rest == "BufferOffset:atIndex:") {
            const uint64_t offset = d.Uint("offset"), slot = d.Uint("index");
            if (vertex) [e setVertexBufferOffset:offset atIndex:slot];
            else [e setFragmentBufferOffset:offset atIndex:slot];
            emit("[" + var + " set" + stage + "BufferOffset:" + Source::Uint(offset) + " atIndex:" +
                 Source::Uint(slot) + "];");
            return true;
        }
        if (rest == "Bytes:length:atIndex:") {
            std::vector<uint8_t> bytes;
            d.Bytes("pValues", bytes);
            const uint64_t slot = d.Uint("index");
            const uint64_t size = bytes.empty() ? d.Uint("size") : bytes.size();
            if (bytes.empty()) { LeftOut(index, m, "the capture holds none of the inline bytes"); return true; }
            if (vertex) [e setVertexBytes:bytes.data() length:(NSUInteger)size atIndex:slot];
            else [e setFragmentBytes:bytes.data() length:(NSUInteger)size atIndex:slot];
            if (_x) {
                const std::string where = _x->Data(bytes.data(), bytes.size());
                emit("[" + var + " set" + stage + "Bytes:" + where + " length:" + Source::Uint(size) +
                     " atIndex:" + Source::Uint(slot) + "];");
            }
            return true;
        }
        if (rest == "Texture:atIndex:") {
            id<MTLTexture> t = (id<MTLTexture>)d.Object("texture");
            const uint64_t slot = d.Uint("index");
            if (vertex) [e setVertexTexture:t atIndex:slot];
            else [e setFragmentTexture:t atIndex:slot];
            emit("[" + var + " set" + stage + "Texture:" + ExportName(t) + " atIndex:" + Source::Uint(slot) + "];");
            return true;
        }
        if (rest == "SamplerState:atIndex:") {
            id<MTLSamplerState> s = (id<MTLSamplerState>)d.Object("sampler");
            const uint64_t slot = d.Uint("index");
            if (vertex) [e setVertexSamplerState:s atIndex:slot];
            else [e setFragmentSamplerState:s atIndex:slot];
            emit("[" + var + " set" + stage + "SamplerState:" + ExportName(s) + " atIndex:" + Source::Uint(slot) + "];");
            return true;
        }
        if (rest == "SamplerState:lodMinClamp:lodMaxClamp:atIndex:") {
            id<MTLSamplerState> s = (id<MTLSamplerState>)d.Object("sampler");
            const float lo = (float)d.Double("lodMinClamp"), hi = (float)d.Double("lodMaxClamp");
            const uint64_t slot = d.Uint("index");
            if (vertex) [e setVertexSamplerState:s lodMinClamp:lo lodMaxClamp:hi atIndex:slot];
            else [e setFragmentSamplerState:s lodMinClamp:lo lodMaxClamp:hi atIndex:slot];
            emit("[" + var + " set" + stage + "SamplerState:" + ExportName(s) + " lodMinClamp:" + Source::Float(lo) +
                 " lodMaxClamp:" + Source::Float(hi) + " atIndex:" + Source::Uint(slot) + "];");
            return true;
        }
        if (rest == "Buffers:offsets:withRange:" || rest == "Textures:withRange:" || rest == "SamplerStates:withRange:") {
            const NSRange range = d.Range("range");
            const char* key = rest[0] == 'B' ? "buffers" : rest[0] == 'T' ? "textures" : "samplers";
            std::vector<id> objects = ObjectArray(d, key);
            std::vector<uint64_t> offsets = UintArray(d, "offsets");
            objects.resize(range.length, nil);
            offsets.resize(range.length, 0);
            const char* elementType = rest[0] == 'B' ? "id<MTLBuffer>" : rest[0] == 'T' ? "id<MTLTexture>" : "id<MTLSamplerState>";
            if (rest[0] == 'B') {
                std::vector<NSUInteger> ns(offsets.begin(), offsets.end());
                if (vertex) [e setVertexBuffers:(const id<MTLBuffer>*)objects.data() offsets:ns.data() withRange:range];
                else [e setFragmentBuffers:(const id<MTLBuffer>*)objects.data() offsets:ns.data() withRange:range];
            } else if (rest[0] == 'T') {
                if (vertex) [e setVertexTextures:(const id<MTLTexture>*)objects.data() withRange:range];
                else [e setFragmentTextures:(const id<MTLTexture>*)objects.data() withRange:range];
            } else {
                if (vertex) [e setVertexSamplerStates:(const id<MTLSamplerState>*)objects.data() withRange:range];
                else [e setFragmentSamplerStates:(const id<MTLSamplerState>*)objects.data() withRange:range];
            }
            emitWith([&](Source& w) {
                const std::string name = w.Local("bindings");
                std::string items;
                for (size_t i = 0; i < objects.size(); ++i) items += (i ? ", " : "") + w.Object(objects[i]);
                w.Line(std::string(elementType) + " " + name + "[] = {" + items + "};");
                const std::string rangeExpr = "NSMakeRange(" + Source::Uint(range.location) + ", " +
                                              Source::Uint(range.length) + ")";
                if (rest[0] == 'B') {
                    const std::string off = w.Local("offsets");
                    std::string values;
                    for (size_t i = 0; i < offsets.size(); ++i) values += (i ? ", " : "") + Source::Uint(offsets[i]);
                    w.Line("const NSUInteger " + off + "[] = {" + values + "};");
                    w.Line("[" + var + " set" + stage + "Buffers:" + name + " offsets:" + off +
                           " withRange:" + rangeExpr + "];");
                } else {
                    w.Line("[" + var + " set" + stage + (rest[0] == 'T' ? "Textures:" : "SamplerStates:") + name +
                           " withRange:" + rangeExpr + "];");
                }
            });
            return true;
        }
    }
    if (m == "setObjectThreadgroupMemoryLength:atIndex:") {
        const uint64_t length = d.Uint("length"), slot = d.Uint("index");
        // Mesh shading, which arrived in macOS 13; an older system simply has no such draw to replay.
        if (@available(macOS 13.0, *)) {
            [e setObjectThreadgroupMemoryLength:length atIndex:slot];
        } else {
            LeftOut(index, m, "object shaders need macOS 13");
            return true;
        }
        emit("[" + var + " setObjectThreadgroupMemoryLength:" + Source::Uint(length) + " atIndex:" +
             Source::Uint(slot) + "];");
        return true;
    }

    // ---- draws
    if (m.compare(0, 14, "drawPrimitives") == 0) {
        const MTLPrimitiveType type = (MTLPrimitiveType)d.Enum("primitiveType", MTL_TABLE(MTLPrimitiveType));
        if (m == "drawPrimitives:indirectBuffer:indirectBufferOffset:") {
            id<MTLBuffer> b = (id<MTLBuffer>)d.Object("indirectBuffer");
            const uint64_t offset = d.Uint("indirectBufferOffset");
            if (!b) { LeftOut(index, m, "the indirect buffer is not in the replay"); return true; }
            [e drawPrimitives:type indirectBuffer:b indirectBufferOffset:offset];
            emit("[" + var + " drawPrimitives:" + Source::Enum(MTL_TABLE(MTLPrimitiveType), type) +
                 " indirectBuffer:" + ExportName(b) + " indirectBufferOffset:" + Source::Uint(offset) + "];");
            return true;
        }
        const uint64_t start = d.Uint("vertexStart"), count = d.Uint("vertexCount");
        const uint64_t instances = d.Uint("instanceCount", 1), base = d.Uint("baseInstance");
        [e drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:base];
        emit("[" + var + " drawPrimitives:" + Source::Enum(MTL_TABLE(MTLPrimitiveType), type) + " vertexStart:" +
             Source::Uint(start) + " vertexCount:" + Source::Uint(count) + " instanceCount:" +
             Source::Uint(instances) + " baseInstance:" + Source::Uint(base) + "];");
        return true;
    }
    if (m.compare(0, 21, "drawIndexedPrimitives") == 0) {
        const MTLPrimitiveType type = (MTLPrimitiveType)d.Enum("primitiveType", MTL_TABLE(MTLPrimitiveType));
        const MTLIndexType indexType = (MTLIndexType)d.Enum("indexType", MTL_TABLE(MTLIndexType));
        id<MTLBuffer> indexBuffer = (id<MTLBuffer>)d.Object("indexBuffer");
        const uint64_t indexOffset = d.Uint("indexBufferOffset");
        if (!indexBuffer) { LeftOut(index, m, "the index buffer is not in the replay"); return true; }
        if (m.find("indirectBuffer") != std::string::npos) {
            id<MTLBuffer> b = (id<MTLBuffer>)d.Object("indirectBuffer");
            const uint64_t offset = d.Uint("indirectBufferOffset");
            if (!b) { LeftOut(index, m, "the indirect buffer is not in the replay"); return true; }
            [e drawIndexedPrimitives:type indexType:indexType indexBuffer:indexBuffer
                 indexBufferOffset:indexOffset indirectBuffer:b indirectBufferOffset:offset];
            emit("[" + var + " drawIndexedPrimitives:" + Source::Enum(MTL_TABLE(MTLPrimitiveType), type) +
                 " indexType:" + Source::Enum(MTL_TABLE(MTLIndexType), indexType) + " indexBuffer:" +
                 ExportName(indexBuffer) + " indexBufferOffset:" + Source::Uint(indexOffset) + " indirectBuffer:" +
                 ExportName(b) + " indirectBufferOffset:" + Source::Uint(offset) + "];");
            return true;
        }
        const uint64_t count = d.Uint("indexCount"), instances = d.Uint("instanceCount", 1);
        const int64_t baseVertex = d.Int("baseVertex");
        const uint64_t baseInstance = d.Uint("baseInstance");
        [e drawIndexedPrimitives:type indexCount:count indexType:indexType indexBuffer:indexBuffer
             indexBufferOffset:indexOffset instanceCount:instances baseVertex:baseVertex baseInstance:baseInstance];
        emit("[" + var + " drawIndexedPrimitives:" + Source::Enum(MTL_TABLE(MTLPrimitiveType), type) +
             " indexCount:" + Source::Uint(count) + " indexType:" + Source::Enum(MTL_TABLE(MTLIndexType), indexType) +
             " indexBuffer:" + ExportName(indexBuffer) + " indexBufferOffset:" + Source::Uint(indexOffset) +
             " instanceCount:" + Source::Uint(instances) + " baseVertex:" + Source::Int(baseVertex) +
             " baseInstance:" + Source::Uint(baseInstance) + "];");
        return true;
    }
    if (m.compare(0, 11, "drawPatches") == 0 || m.compare(0, 18, "drawIndexedPatches") == 0) {
        const bool indexed = m[4] == 'I';
        const uint64_t points = d.Uint("numberOfPatchControlPoints");
        id<MTLBuffer> patchIndex = (id<MTLBuffer>)d.Object("patchIndexBuffer");
        const uint64_t patchIndexOffset = d.Uint("patchIndexBufferOffset");
        id<MTLBuffer> controlPoints = (id<MTLBuffer>)d.Object("controlPointIndexBuffer");
        const uint64_t controlPointsOffset = d.Uint("controlPointIndexBufferOffset");
        if (m.find("indirectBuffer") != std::string::npos) {
            id<MTLBuffer> b = (id<MTLBuffer>)d.Object("indirectBuffer");
            const uint64_t offset = d.Uint("indirectBufferOffset");
            if (!b) { LeftOut(index, m, "the indirect buffer is not in the replay"); return true; }
            if (indexed) {
                [e drawIndexedPatches:points patchIndexBuffer:patchIndex patchIndexBufferOffset:patchIndexOffset
                    controlPointIndexBuffer:controlPoints controlPointIndexBufferOffset:controlPointsOffset
                             indirectBuffer:b indirectBufferOffset:offset];
                emit("[" + var + " drawIndexedPatches:" + Source::Uint(points) + " patchIndexBuffer:" +
                     ExportName(patchIndex) + " patchIndexBufferOffset:" + Source::Uint(patchIndexOffset) +
                     " controlPointIndexBuffer:" + ExportName(controlPoints) + " controlPointIndexBufferOffset:" +
                     Source::Uint(controlPointsOffset) + " indirectBuffer:" + ExportName(b) +
                     " indirectBufferOffset:" + Source::Uint(offset) + "];");
            } else {
                [e drawPatches:points patchIndexBuffer:patchIndex patchIndexBufferOffset:patchIndexOffset
                    indirectBuffer:b indirectBufferOffset:offset];
                emit("[" + var + " drawPatches:" + Source::Uint(points) + " patchIndexBuffer:" + ExportName(patchIndex) +
                     " patchIndexBufferOffset:" + Source::Uint(patchIndexOffset) + " indirectBuffer:" + ExportName(b) +
                     " indirectBufferOffset:" + Source::Uint(offset) + "];");
            }
            return true;
        }
        const uint64_t start = d.Uint("patchStart"), count = d.Uint("patchCount");
        const uint64_t instances = d.Uint("instanceCount", 1), baseInstance = d.Uint("baseInstance");
        if (indexed) {
            [e drawIndexedPatches:points patchStart:start patchCount:count patchIndexBuffer:patchIndex
               patchIndexBufferOffset:patchIndexOffset controlPointIndexBuffer:controlPoints
        controlPointIndexBufferOffset:controlPointsOffset instanceCount:instances baseInstance:baseInstance];
            emit("[" + var + " drawIndexedPatches:" + Source::Uint(points) + " patchStart:" + Source::Uint(start) +
                 " patchCount:" + Source::Uint(count) + " patchIndexBuffer:" + ExportName(patchIndex) +
                 " patchIndexBufferOffset:" + Source::Uint(patchIndexOffset) + " controlPointIndexBuffer:" +
                 ExportName(controlPoints) + " controlPointIndexBufferOffset:" + Source::Uint(controlPointsOffset) +
                 " instanceCount:" + Source::Uint(instances) + " baseInstance:" + Source::Uint(baseInstance) + "];");
        } else {
            [e drawPatches:points patchStart:start patchCount:count patchIndexBuffer:patchIndex
        patchIndexBufferOffset:patchIndexOffset instanceCount:instances baseInstance:baseInstance];
            emit("[" + var + " drawPatches:" + Source::Uint(points) + " patchStart:" + Source::Uint(start) +
                 " patchCount:" + Source::Uint(count) + " patchIndexBuffer:" + ExportName(patchIndex) +
                 " patchIndexBufferOffset:" + Source::Uint(patchIndexOffset) + " instanceCount:" +
                 Source::Uint(instances) + " baseInstance:" + Source::Uint(baseInstance) + "];");
        }
        return true;
    }
    if (m.compare(0, 9, "drawMesh") == 0 || m.compare(0, 8, "drawMesh") == 0) {
        LeftOut(index, m, "mesh shader draws are not replayed yet");
        return true;
    }
    if (m.compare(0, 21, "executeCommandsInBuffer") == 0) {
        LeftOut(index, m, "indirect command buffers are not replayed yet");
        return true;
    }

    // ---- residency and synchronization, which the render and compute encoders share
    return ResidencyCommand(m, d, index, var);
}

// ------------------------------------------------------------------------------------------
// Compute

bool MtlReplayer::ComputeCommand(const std::string& m, const Decoder& d, uint32_t index) {
    id<MTLComputeCommandEncoder> e = (id<MTLComputeCommandEncoder>)_encoder;
    const std::string var = _pass.variable;
    auto emit = [&](const std::string& statement) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) { w.Line(statement); });
        _x->CountCommand();
    };
    auto emitWith = [&](const std::function<void(Source&)>& body) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", body);
        _x->CountCommand();
    };

    if (m == "setComputePipelineState:") {
        id<MTLComputePipelineState> p = (id<MTLComputePipelineState>)d.Object("pipeline");
        if (!p) { LeftOut(index, m, "the pipeline is not in the replay"); return true; }
        // Kept so a texture bind can ask the reflection how the slot is accessed
        // (CompareWrittenTextures).
        _boundComputePipeline = d.ObjectId("pipeline");
        [e setComputePipelineState:p];
        emit("[" + var + " setComputePipelineState:" + ExportName(p) + "];");
        return true;
    }
    if (m == "setBuffer:offset:atIndex:") {
        id<MTLBuffer> b = (id<MTLBuffer>)d.Object("buffer");
        const uint64_t offset = d.Uint("offset"), slot = d.Uint("index");
        [e setBuffer:b offset:offset atIndex:slot];
        emit("[" + var + " setBuffer:" + ExportName(b) + " offset:" + Source::Uint(offset) + " atIndex:" +
             Source::Uint(slot) + "];");
        return true;
    }
    if (m == "setBufferOffset:atIndex:") {
        const uint64_t offset = d.Uint("offset"), slot = d.Uint("index");
        [e setBufferOffset:offset atIndex:slot];
        emit("[" + var + " setBufferOffset:" + Source::Uint(offset) + " atIndex:" + Source::Uint(slot) + "];");
        return true;
    }
    if (m == "setBytes:length:atIndex:") {
        std::vector<uint8_t> bytes;
        d.Bytes("pValues", bytes);
        if (bytes.empty()) { LeftOut(index, m, "the capture holds none of the inline bytes"); return true; }
        const uint64_t slot = d.Uint("index");
        [e setBytes:bytes.data() length:bytes.size() atIndex:slot];
        if (_x) {
            const std::string where = _x->Data(bytes.data(), bytes.size());
            emit("[" + var + " setBytes:" + where + " length:" + Source::Uint(bytes.size()) + " atIndex:" +
                 Source::Uint(slot) + "];");
        }
        return true;
    }
    if (m == "setTexture:atIndex:") {
        id<MTLTexture> t = (id<MTLTexture>)d.Object("texture");
        const uint64_t slot = d.Uint("index");
        [e setTexture:t atIndex:slot];
        // A writable texture bound to a compute encoder is something the frame *produces*, so what
        // it holds afterwards is worth comparing: the path tracer's traced image is one, and
        // without this a frame with no render pass compares nothing at all (CompareWrittenTextures).
        if (t != nil && (t.usage & MTLTextureUsageShaderWrite) != 0) {
            const std::string access = ComputeTextureAccess(_boundComputePipeline, slot);
            if (access == "readWrite") _encoderAccumulates = true;
            if (access != "readOnly") _encoderWrites.push_back(d.ObjectId("texture"));
        }
        emit("[" + var + " setTexture:" + ExportName(t) + " atIndex:" + Source::Uint(slot) + "];");
        return true;
    }
    if (m == "setSamplerState:atIndex:") {
        id<MTLSamplerState> s = (id<MTLSamplerState>)d.Object("sampler");
        const uint64_t slot = d.Uint("index");
        [e setSamplerState:s atIndex:slot];
        emit("[" + var + " setSamplerState:" + ExportName(s) + " atIndex:" + Source::Uint(slot) + "];");
        return true;
    }
    if (m == "setSamplerState:lodMinClamp:lodMaxClamp:atIndex:") {
        id<MTLSamplerState> s = (id<MTLSamplerState>)d.Object("sampler");
        const float lo = (float)d.Double("lodMinClamp"), hi = (float)d.Double("lodMaxClamp");
        const uint64_t slot = d.Uint("index");
        [e setSamplerState:s lodMinClamp:lo lodMaxClamp:hi atIndex:slot];
        emit("[" + var + " setSamplerState:" + ExportName(s) + " lodMinClamp:" + Source::Float(lo) + " lodMaxClamp:" +
             Source::Float(hi) + " atIndex:" + Source::Uint(slot) + "];");
        return true;
    }
    if (m == "setBuffers:offsets:withRange:" || m == "setTextures:withRange:" || m == "setSamplerStates:withRange:") {
        const NSRange range = d.Range("range");
        const char* key = m[3] == 'B' ? "buffers" : m[3] == 'T' ? "textures" : "samplers";
        std::vector<id> objects = ObjectArray(d, key);
        std::vector<uint64_t> offsets = UintArray(d, "offsets");
        objects.resize(range.length, nil);
        offsets.resize(range.length, 0);
        const char* elementType = m[3] == 'B' ? "id<MTLBuffer>" : m[3] == 'T' ? "id<MTLTexture>" : "id<MTLSamplerState>";
        if (m[3] == 'B') {
            std::vector<NSUInteger> ns(offsets.begin(), offsets.end());
            [e setBuffers:(const id<MTLBuffer>*)objects.data() offsets:ns.data() withRange:range];
        } else if (m[3] == 'T') {
            [e setTextures:(const id<MTLTexture>*)objects.data() withRange:range];
        } else {
            [e setSamplerStates:(const id<MTLSamplerState>*)objects.data() withRange:range];
        }
        emitWith([&](Source& w) {
            const std::string name = w.Local("bindings");
            std::string items;
            for (size_t i = 0; i < objects.size(); ++i) items += (i ? ", " : "") + w.Object(objects[i]);
            w.Line(std::string(elementType) + " " + name + "[] = {" + items + "};");
            const std::string rangeExpr = "NSMakeRange(" + Source::Uint(range.location) + ", " +
                                          Source::Uint(range.length) + ")";
            if (m[3] == 'B') {
                const std::string off = w.Local("offsets");
                std::string values;
                for (size_t i = 0; i < offsets.size(); ++i) values += (i ? ", " : "") + Source::Uint(offsets[i]);
                w.Line("const NSUInteger " + off + "[] = {" + values + "};");
                w.Line("[" + var + " setBuffers:" + name + " offsets:" + off + " withRange:" + rangeExpr + "];");
            } else {
                w.Line("[" + var + " set" + (m[3] == 'T' ? "Textures:" : "SamplerStates:") + name +
                       " withRange:" + rangeExpr + "];");
            }
        });
        return true;
    }
    if (m == "setThreadgroupMemoryLength:atIndex:" || m == "setThreadgroupMemoryLength:offset:atIndex:") {
        const uint64_t length = d.Uint("length"), slot = d.Uint("index");
        [e setThreadgroupMemoryLength:length atIndex:slot];
        emit("[" + var + " setThreadgroupMemoryLength:" + Source::Uint(length) + " atIndex:" + Source::Uint(slot) + "];");
        return true;
    }
    if (m == "setStageInRegion:") {
        const MTLRegion r = d.Region("region");
        [e setStageInRegion:r];
        emit("[" + var + " setStageInRegion:(MTLRegion){" + Source::Origin(r.origin.x, r.origin.y, r.origin.z) +
             ", " + Source::Size(r.size.width, r.size.height, r.size.depth) + "}];");
        return true;
    }
    if (m == "setStageInRegionWithIndirectBuffer:indirectBufferOffset:") {
        id<MTLBuffer> b = (id<MTLBuffer>)d.Object("indirectBuffer");
        const uint64_t offset = d.Uint("indirectBufferOffset");
        [e setStageInRegionWithIndirectBuffer:b indirectBufferOffset:offset];
        emit("[" + var + " setStageInRegionWithIndirectBuffer:" + ExportName(b) + " indirectBufferOffset:" +
             Source::Uint(offset) + "];");
        return true;
    }
    // The ray tracing bindings a compute encoder has. An acceleration structure and a function
    // table are ordinary objects to bind — unlike DXR's and Vulkan's, where the binding is a
    // buffer of identifiers the replay has to rewrite (mtl_raytracing.h).
    if (m == "setAccelerationStructure:atBufferIndex:") {
        id structure = d.Object("accelerationStructure");
        const uint64_t slot = d.Uint("index");
        if (structure == nil) { LeftOut(index, m, "the acceleration structure is not in the replay"); return true; }
        if (@available(macOS 11.0, *)) {
            [e setAccelerationStructure:(id<MTLAccelerationStructure>)structure atBufferIndex:(NSUInteger)slot];
        } else {
            LeftOut(index, m, "acceleration structures need macOS 11");
            return true;
        }
        emit("[" + var + " setAccelerationStructure:" + ExportName(structure) + " atBufferIndex:" +
             Source::Uint(slot) + "];");
        return true;
    }
    if (m == "setIntersectionFunctionTable:atBufferIndex:" || m == "setVisibleFunctionTable:atBufferIndex:") {
        const bool intersection = m.compare(0, 28, "setIntersectionFunctionTable") == 0;
        id table = d.Object(intersection ? "intersectionFunctionTable" : "visibleFunctionTable");
        const uint64_t slot = d.Uint("index");
        if (table == nil) { LeftOut(index, m, "the function table is not in the replay"); return true; }
        if (@available(macOS 11.0, *)) {
            if (intersection) {
                [e setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)table atBufferIndex:(NSUInteger)slot];
            } else {
                [e setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)table atBufferIndex:(NSUInteger)slot];
            }
        } else {
            LeftOut(index, m, "function tables need macOS 11");
            return true;
        }
        emit("[" + var + (intersection ? " setIntersectionFunctionTable:" : " setVisibleFunctionTable:") +
             ExportName(table) + " atBufferIndex:" + Source::Uint(slot) + "];");
        return true;
    }
    if (m == "setIntersectionFunctionTables:withBufferRange:" || m == "setVisibleFunctionTables:withBufferRange:") {
        const bool intersection = m.compare(0, 29, "setIntersectionFunctionTables") == 0;
        std::vector<id> tables = ObjectArray(d, intersection ? "intersectionFunctionTables" : "visibleFunctionTables");
        const NSRange range = d.Range("range");
        if (tables.empty()) { LeftOut(index, m, "none of the function tables are in the replay"); return true; }
        if (@available(macOS 11.0, *)) {
            if (intersection) {
                [e setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable>*)tables.data() withBufferRange:range];
            } else {
                [e setVisibleFunctionTables:(const id<MTLVisibleFunctionTable>*)tables.data() withBufferRange:range];
            }
        } else {
            LeftOut(index, m, "function tables need macOS 11");
            return true;
        }
        emitWith([&](Source& w) {
            const std::string name = w.Local("tables");
            std::string items;
            for (size_t i = 0; i < tables.size(); ++i) items += (i ? ", " : "") + w.Object(tables[i]);
            w.Line(std::string(intersection ? "id<MTLIntersectionFunctionTable> " : "id<MTLVisibleFunctionTable> ") +
                   name + "[] = {" + items + "};");
            w.Line("[" + var + (intersection ? " setIntersectionFunctionTables:" : " setVisibleFunctionTables:") +
                   name + " withBufferRange:NSMakeRange(" + Source::Uint(range.location) + ", " +
                   Source::Uint(range.length) + ")];");
        });
        return true;
    }
    if (m == "dispatchThreadgroups:threadsPerThreadgroup:") {
        const MTLSize groups = d.Size("threadgroupsPerGrid"), threads = d.Size("threadsPerThreadgroup");
        [e dispatchThreadgroups:groups threadsPerThreadgroup:threads];
        emit("[" + var + " dispatchThreadgroups:" + Source::Size(groups.width, groups.height, groups.depth) +
             " threadsPerThreadgroup:" + Source::Size(threads.width, threads.height, threads.depth) + "];");
        return true;
    }
    if (m == "dispatchThreads:threadsPerThreadgroup:") {
        const MTLSize grid = d.Size("threadsPerGrid"), threads = d.Size("threadsPerThreadgroup");
        [e dispatchThreads:grid threadsPerThreadgroup:threads];
        emit("[" + var + " dispatchThreads:" + Source::Size(grid.width, grid.height, grid.depth) +
             " threadsPerThreadgroup:" + Source::Size(threads.width, threads.height, threads.depth) + "];");
        return true;
    }
    if (m == "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:") {
        id<MTLBuffer> b = (id<MTLBuffer>)d.Object("indirectBuffer");
        const uint64_t offset = d.Uint("indirectBufferOffset");
        const MTLSize threads = d.Size("threadsPerThreadgroup");
        if (!b) { LeftOut(index, m, "the indirect buffer is not in the replay"); return true; }
        [e dispatchThreadgroupsWithIndirectBuffer:b indirectBufferOffset:offset threadsPerThreadgroup:threads];
        emit("[" + var + " dispatchThreadgroupsWithIndirectBuffer:" + ExportName(b) + " indirectBufferOffset:" +
             Source::Uint(offset) + " threadsPerThreadgroup:" +
             Source::Size(threads.width, threads.height, threads.depth) + "];");
        return true;
    }
    if (m == "dispatchThreadsPerTile:") {
        LeftOut(index, m, "a tile dispatch belongs to a render encoder the replay does not open");
        return true;
    }
    if (m == "memoryBarrierWithScope:" || m == "memoryBarrierWithScope:afterStages:beforeStages:") {
        const MTLBarrierScope scope = (MTLBarrierScope)d.Flags("scope", MTL_TABLE(MTLBarrierScope));
        [e memoryBarrierWithScope:scope];
        emit("[" + var + " memoryBarrierWithScope:" + Source::Flags(MTL_TABLE(MTLBarrierScope), scope) + "];");
        return true;
    }
    if (m == "memoryBarrierWithResources:count:" || m == "memoryBarrierWithResources:count:afterStages:beforeStages:") {
        std::vector<id> resources = ObjectArray(d, "resources");
        resources.erase(std::remove(resources.begin(), resources.end(), nil), resources.end());
        if (resources.empty()) { LeftOut(index, m, "none of the resources are in the replay"); return true; }
        [e memoryBarrierWithResources:(const id<MTLResource>*)resources.data() count:resources.size()];
        emitWith([&](Source& w) {
            const std::string name = w.Local("resources");
            std::string items;
            for (size_t i = 0; i < resources.size(); ++i) items += (i ? ", " : "") + w.Object(resources[i]);
            w.Line("id<MTLResource> " + name + "[] = {" + items + "};");
            w.Line("[" + var + " memoryBarrierWithResources:" + name + " count:" + Source::Uint(resources.size()) + "];");
        });
        return true;
    }
    return ResidencyCommand(m, d, index, var);
}

// ------------------------------------------------------------------------------------------
// Acceleration structures
//
// The builds, refits and copies an acceleration structure encoder recorded. The descriptor comes
// back from the capture's JSON (mtl_raytracing.h) — a Metal geometry names its buffers outright, so
// there is no address to remap and nothing to guess, which is the whole of why this is short.
//
// The scratch buffer is the replay's own, not the application's. A build's scratch requirement is
// the *driver's* answer to a descriptor, and this may be a different driver or a different GPU from
// the one captured: the application's buffer could be too small, and a build given too little
// scratch is undefined rather than an error. So the size is asked for here and a buffer of it made,
// which also means a capture whose scratch buffer was not read back still builds.

bool MtlReplayer::AccelerationCommand(const std::string& m, const Decoder& d, uint32_t index) {
    if (@available(macOS 11.0, *)) {
    } else {
        LeftOut(index, m, "acceleration structures need macOS 11");
        return true;
    }
    id<MTLAccelerationStructureCommandEncoder> e = (id<MTLAccelerationStructureCommandEncoder>)_encoder;
    const std::string var = _pass.variable;
    auto emit = [&](const std::string& statement) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) { w.Line(statement); });
        _x->CountCommand();
    };
    auto emitWith = [&](void (^body)(Source&)) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", body);
        _x->CountCommand();
    };

    const bool build = m.compare(0, 26, "buildAccelerationStructure") == 0;
    const bool refit = m.compare(0, 26, "refitAccelerationStructure") == 0;
    if (build || refit) {
        id destination = d.Object("accelerationStructure");
        if (destination == nil) { LeftOut(index, m, "the structure being built is not in the replay"); return true; }
        std::string error;
        MTLAccelerationStructureDescriptor* descriptor = BuildDescriptor(d.Nested("descriptor"), error);
        if (descriptor == nil) { LeftOut(index, m, error); return true; }

        // Scratch of this driver's size, not the application's (see above).
        const MTLAccelerationStructureSizes sizes = [_device accelerationStructureSizesWithDescriptor:descriptor];
        const NSUInteger scratchSize = refit ? std::max<NSUInteger>(sizes.refitScratchBufferSize, 1)
                                             : std::max<NSUInteger>(sizes.buildScratchBufferSize, 1);
        id<MTLBuffer> scratch = [_device newBufferWithLength:scratchSize options:MTLResourceStorageModePrivate];
        if (scratch == nil) { LeftOut(index, m, "no memory for the build's scratch buffer"); return true; }
        _scratch.push_back(scratch);

        if (build) {
            [e buildAccelerationStructure:(id<MTLAccelerationStructure>)destination
                               descriptor:descriptor
                            scratchBuffer:scratch
                      scratchBufferOffset:0];
        } else {
            // A refit with no destination rewrites the source in place, which is what the capture
            // recorded as the structure built; the source it refits from is named separately.
            id source = d.Has("sourceAccelerationStructure") ? d.Object("sourceAccelerationStructure") : destination;
            if (source == nil) { LeftOut(index, m, "the structure being refit is not in the replay"); return true; }
            id explicitDestination = source == destination ? nil : destination;
            if (m.find("options:") != std::string::npos) {
                if (@available(macOS 14.0, *)) {
                    [e refitAccelerationStructure:(id<MTLAccelerationStructure>)source
                                       descriptor:descriptor
                                      destination:(id<MTLAccelerationStructure>)explicitDestination
                                    scratchBuffer:scratch
                              scratchBufferOffset:0
                                          options:(MTLAccelerationStructureRefitOptions)d.Uint("options")];
                } else {
                    LeftOut(index, m, "a refit with options needs macOS 14");
                    return true;
                }
            } else {
                [e refitAccelerationStructure:(id<MTLAccelerationStructure>)source
                                   descriptor:descriptor
                                  destination:(id<MTLAccelerationStructure>)explicitDestination
                                scratchBuffer:scratch
                          scratchBufferOffset:0];
            }
        }

        emitWith(^(Source& w) {
            const std::string descriptorVar = w.Local("descriptor");
            w.Line("MTLAccelerationStructureDescriptor *" + descriptorVar + " = nil;");
            std::string why;
            Decoder nested = d.Nested("descriptor");
            if (!WriteDescriptorSource(w, descriptorVar, nested, why)) {
                w.Comment("left out: " + why);
                w.Note("command " + std::to_string(index) + " (" + m + "): " + why);
                return;
            }
            const std::string sizesVar = w.Local("sizes");
            const std::string scratchVar = w.Local("scratch");
            w.Line("MTLAccelerationStructureSizes " + sizesVar +
                   " = [device accelerationStructureSizesWithDescriptor:" + descriptorVar + "];");
            w.Line("id<MTLBuffer> " + scratchVar + " = [device newBufferWithLength:" + sizesVar +
                   (refit ? ".refitScratchBufferSize" : ".buildScratchBufferSize") +
                   " options:MTLResourceStorageModePrivate];");
            if (build) {
                w.Line("[" + var + " buildAccelerationStructure:" + w.Object(destination) +
                       " descriptor:" + descriptorVar + " scratchBuffer:" + scratchVar +
                       " scratchBufferOffset:0];");
            } else {
                id source = d.Has("sourceAccelerationStructure") ? d.Object("sourceAccelerationStructure") : destination;
                const std::string dest = source == destination ? std::string("nil") : w.Object(destination);
                w.Line("[" + var + " refitAccelerationStructure:" + w.Object(source) +
                       " descriptor:" + descriptorVar + " destination:" + dest +
                       " scratchBuffer:" + scratchVar + " scratchBufferOffset:0];");
            }
        });
        return true;
    }

    if (m == "copyAccelerationStructure:toAccelerationStructure:" ||
        m == "copyAndCompactAccelerationStructure:toAccelerationStructure:") {
        id source = d.Object("sourceAccelerationStructure");
        id destination = d.Object("destinationAccelerationStructure");
        if (source == nil || destination == nil) {
            LeftOut(index, m, "an acceleration structure is not in the replay");
            return true;
        }
        const bool compact = m.compare(0, 5, "copyA") == 0 && m.find("Compact") != std::string::npos;
        if (compact) {
            [e copyAndCompactAccelerationStructure:(id<MTLAccelerationStructure>)source
                           toAccelerationStructure:(id<MTLAccelerationStructure>)destination];
        } else {
            [e copyAccelerationStructure:(id<MTLAccelerationStructure>)source
                 toAccelerationStructure:(id<MTLAccelerationStructure>)destination];
        }
        emit("[" + var + (compact ? " copyAndCompactAccelerationStructure:" : " copyAccelerationStructure:") +
             ExportName(source) + " toAccelerationStructure:" + ExportName(destination) + "];");
        return true;
    }

    if (m.compare(0, 40, "writeCompactedAccelerationStructureSize:") == 0) {
        id structure = d.Object("accelerationStructure");
        id<MTLBuffer> buffer = (id<MTLBuffer>)d.Object("buffer");
        const uint64_t offset = d.Uint("offset");
        if (structure == nil || buffer == nil) {
            LeftOut(index, m, "the structure or the buffer is not in the replay");
            return true;
        }
        if (m.find("sizeDataType:") != std::string::npos) {
            if (@available(macOS 13.0, *)) {
                const MTLDataType type = (MTLDataType)d.Uint("sizeDataType", MTLDataTypeUInt);
                [e writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)structure
                                                 toBuffer:buffer
                                                   offset:(NSUInteger)offset
                                             sizeDataType:type];
                emit("[" + var + " writeCompactedAccelerationStructureSize:" + ExportName(structure) +
                     " toBuffer:" + ExportName(buffer) + " offset:" + Source::Uint(offset) +
                     " sizeDataType:(MTLDataType)" + Source::Uint((uint64_t)type) + "];");
                return true;
            }
            LeftOut(index, m, "writing a compacted size with a data type needs macOS 13");
            return true;
        }
        [e writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)structure
                                         toBuffer:buffer
                                           offset:(NSUInteger)offset];
        emit("[" + var + " writeCompactedAccelerationStructureSize:" + ExportName(structure) +
             " toBuffer:" + ExportName(buffer) + " offset:" + Source::Uint(offset) + "];");
        return true;
    }
    return ResidencyCommand(m, d, index, var);
}

// ------------------------------------------------------------------------------------------
// Blit

bool MtlReplayer::BlitCommand(const std::string& m, const Decoder& d, uint32_t index) {
    id<MTLBlitCommandEncoder> e = (id<MTLBlitCommandEncoder>)_encoder;
    const std::string var = _pass.variable;
    auto emit = [&](const std::string& statement) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) { w.Line(statement); });
        _x->CountCommand();
    };

    if (m == "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:") {
        id<MTLBuffer> from = (id<MTLBuffer>)d.Object("sourceBuffer");
        id<MTLBuffer> to = (id<MTLBuffer>)d.Object("destinationBuffer");
        const uint64_t fromOffset = d.Uint("sourceOffset"), toOffset = d.Uint("destinationOffset"), size = d.Uint("size");
        if (!from || !to) { LeftOut(index, m, "a buffer is not in the replay"); return true; }
        [e copyFromBuffer:from sourceOffset:fromOffset toBuffer:to destinationOffset:toOffset size:size];
        emit("[" + var + " copyFromBuffer:" + ExportName(from) + " sourceOffset:" + Source::Uint(fromOffset) +
             " toBuffer:" + ExportName(to) + " destinationOffset:" + Source::Uint(toOffset) + " size:" +
             Source::Uint(size) + "];");
        return true;
    }
    if (m == "copyFromTexture:toTexture:") {
        id<MTLTexture> from = (id<MTLTexture>)d.Object("sourceTexture");
        id<MTLTexture> to = (id<MTLTexture>)d.Object("destinationTexture");
        if (!from || !to) { LeftOut(index, m, "a texture is not in the replay"); return true; }
        [e copyFromTexture:from toTexture:to];
        emit("[" + var + " copyFromTexture:" + ExportName(from) + " toTexture:" + ExportName(to) + "];");
        return true;
    }
    if (m == "copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:") {
        id<MTLTexture> from = (id<MTLTexture>)d.Object("sourceTexture");
        id<MTLTexture> to = (id<MTLTexture>)d.Object("destinationTexture");
        const uint64_t fromSlice = d.Uint("sourceSlice"), fromLevel = d.Uint("sourceLevel");
        const uint64_t toSlice = d.Uint("destinationSlice"), toLevel = d.Uint("destinationLevel");
        const uint64_t slices = d.Uint("sliceCount", 1), levels = d.Uint("levelCount", 1);
        if (!from || !to) { LeftOut(index, m, "a texture is not in the replay"); return true; }
        [e copyFromTexture:from sourceSlice:fromSlice sourceLevel:fromLevel toTexture:to
          destinationSlice:toSlice destinationLevel:toLevel sliceCount:slices levelCount:levels];
        emit("[" + var + " copyFromTexture:" + ExportName(from) + " sourceSlice:" + Source::Uint(fromSlice) +
             " sourceLevel:" + Source::Uint(fromLevel) + " toTexture:" + ExportName(to) + " destinationSlice:" +
             Source::Uint(toSlice) + " destinationLevel:" + Source::Uint(toLevel) + " sliceCount:" +
             Source::Uint(slices) + " levelCount:" + Source::Uint(levels) + "];");
        return true;
    }
    if (m == "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:") {
        id<MTLTexture> from = (id<MTLTexture>)d.Object("sourceTexture");
        id<MTLTexture> to = (id<MTLTexture>)d.Object("destinationTexture");
        const uint64_t fromSlice = d.Uint("sourceSlice"), fromLevel = d.Uint("sourceLevel");
        const MTLOrigin fromOrigin = d.Origin("sourceOrigin");
        const MTLSize size = d.Size("sourceSize");
        const uint64_t toSlice = d.Uint("destinationSlice"), toLevel = d.Uint("destinationLevel");
        const MTLOrigin toOrigin = d.Origin("destinationOrigin");
        if (!from || !to) { LeftOut(index, m, "a texture is not in the replay"); return true; }
        [e copyFromTexture:from sourceSlice:fromSlice sourceLevel:fromLevel sourceOrigin:fromOrigin
                sourceSize:size toTexture:to destinationSlice:toSlice destinationLevel:toLevel
         destinationOrigin:toOrigin];
        emit("[" + var + " copyFromTexture:" + ExportName(from) + " sourceSlice:" + Source::Uint(fromSlice) +
             " sourceLevel:" + Source::Uint(fromLevel) + " sourceOrigin:" +
             Source::Origin(fromOrigin.x, fromOrigin.y, fromOrigin.z) + " sourceSize:" +
             Source::Size(size.width, size.height, size.depth) + " toTexture:" + ExportName(to) +
             " destinationSlice:" + Source::Uint(toSlice) + " destinationLevel:" + Source::Uint(toLevel) +
             " destinationOrigin:" + Source::Origin(toOrigin.x, toOrigin.y, toOrigin.z) + "];");
        return true;
    }
    if (m == "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:") {
        id<MTLBuffer> from = (id<MTLBuffer>)d.Object("sourceBuffer");
        id<MTLTexture> to = (id<MTLTexture>)d.Object("destinationTexture");
        const uint64_t offset = d.Uint("sourceOffset"), rowBytes = d.Uint("sourceBytesPerRow");
        const uint64_t imageBytes = d.Uint("sourceBytesPerImage");
        const MTLSize size = d.Size("sourceSize");
        const uint64_t slice = d.Uint("destinationSlice"), level = d.Uint("destinationLevel");
        const MTLOrigin origin = d.Origin("destinationOrigin");
        if (!from || !to) { LeftOut(index, m, "a resource is not in the replay"); return true; }
        [e copyFromBuffer:from sourceOffset:offset sourceBytesPerRow:rowBytes sourceBytesPerImage:imageBytes
               sourceSize:size toTexture:to destinationSlice:slice destinationLevel:level
        destinationOrigin:origin];
        emit("[" + var + " copyFromBuffer:" + ExportName(from) + " sourceOffset:" + Source::Uint(offset) +
             " sourceBytesPerRow:" + Source::Uint(rowBytes) + " sourceBytesPerImage:" + Source::Uint(imageBytes) +
             " sourceSize:" + Source::Size(size.width, size.height, size.depth) + " toTexture:" + ExportName(to) +
             " destinationSlice:" + Source::Uint(slice) + " destinationLevel:" + Source::Uint(level) +
             " destinationOrigin:" + Source::Origin(origin.x, origin.y, origin.z) + "];");
        return true;
    }
    if (m == "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:") {
        id<MTLTexture> from = (id<MTLTexture>)d.Object("sourceTexture");
        id<MTLBuffer> to = (id<MTLBuffer>)d.Object("destinationBuffer");
        const uint64_t slice = d.Uint("sourceSlice"), level = d.Uint("sourceLevel");
        const MTLOrigin origin = d.Origin("sourceOrigin");
        const MTLSize size = d.Size("sourceSize");
        const uint64_t offset = d.Uint("destinationOffset"), rowBytes = d.Uint("destinationBytesPerRow");
        const uint64_t imageBytes = d.Uint("destinationBytesPerImage");
        if (!from || !to) { LeftOut(index, m, "a resource is not in the replay"); return true; }
        [e copyFromTexture:from sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size
                  toBuffer:to destinationOffset:offset destinationBytesPerRow:rowBytes
     destinationBytesPerImage:imageBytes];
        emit("[" + var + " copyFromTexture:" + ExportName(from) + " sourceSlice:" + Source::Uint(slice) +
             " sourceLevel:" + Source::Uint(level) + " sourceOrigin:" + Source::Origin(origin.x, origin.y, origin.z) +
             " sourceSize:" + Source::Size(size.width, size.height, size.depth) + " toBuffer:" + ExportName(to) +
             " destinationOffset:" + Source::Uint(offset) + " destinationBytesPerRow:" + Source::Uint(rowBytes) +
             " destinationBytesPerImage:" + Source::Uint(imageBytes) + "];");
        return true;
    }
    if (m == "fillBuffer:range:value:") {
        id<MTLBuffer> b = (id<MTLBuffer>)d.Object("buffer");
        const NSRange range = d.Range("range");
        const uint8_t value = (uint8_t)d.Uint("value");
        if (!b) { LeftOut(index, m, "the buffer is not in the replay"); return true; }
        [e fillBuffer:b range:range value:value];
        emit("[" + var + " fillBuffer:" + ExportName(b) + " range:NSMakeRange(" + Source::Uint(range.location) + ", " +
             Source::Uint(range.length) + ") value:" + Source::Uint(value) + "];");
        return true;
    }
    if (m == "generateMipmapsForTexture:") {
        id<MTLTexture> t = (id<MTLTexture>)d.Object("texture");
        if (!t) { LeftOut(index, m, "the texture is not in the replay"); return true; }
        [e generateMipmapsForTexture:t];
        emit("[" + var + " generateMipmapsForTexture:" + ExportName(t) + "];");
        return true;
    }
    if (m == "synchronizeResource:" || m == "synchronizeTexture:slice:level:") {
        // Managed storage only, and the replay's resources are shared or private. Nothing to do,
        // and nothing wrong: what the application synchronized is already visible here.
        return true;
    }
    if (m.compare(0, 24, "optimizeContentsForCPU") == 0 || m.compare(0, 24, "optimizeContentsForGPU") == 0) {
        id<MTLTexture> t = (id<MTLTexture>)d.Object("texture");
        if (!t) { LeftOut(index, m, "the texture is not in the replay"); return true; }
        const bool cpu = m.find("CPU") != std::string::npos;
        if (m.find("slice") != std::string::npos) {
            const uint64_t slice = d.Uint("slice"), level = d.Uint("level");
            if (cpu) [e optimizeContentsForCPUAccess:t slice:slice level:level];
            else [e optimizeContentsForGPUAccess:t slice:slice level:level];
            emit("[" + var + " optimizeContentsFor" + (cpu ? "CPU" : "GPU") + "Access:" + ExportName(t) + " slice:" +
                 Source::Uint(slice) + " level:" + Source::Uint(level) + "];");
        } else {
            if (cpu) [e optimizeContentsForCPUAccess:t];
            else [e optimizeContentsForGPUAccess:t];
            emit("[" + var + " optimizeContentsFor" + (cpu ? "CPU" : "GPU") + "Access:" + ExportName(t) + "];");
        }
        return true;
    }
    if (m.compare(0, 21, "optimizeIndirectComm") == 0 || m.compare(0, 20, "resetCommandsInBuff") == 0 ||
        m.compare(0, 24, "copyIndirectCommandBuff") == 0) {
        LeftOut(index, m, "indirect command buffers are not replayed yet");
        return true;
    }
    return ResidencyCommand(m, d, index, var);
}

// ------------------------------------------------------------------------------------------
// Residency and fences, which render, compute and blit encoders share

bool MtlReplayer::ResidencyCommand(const std::string& m, const Decoder& d, uint32_t index, const std::string& var) {
    auto emit = [&](const std::string& statement) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& w) { w.Line(statement); });
        _x->CountCommand();
    };
    auto emitWith = [&](const std::function<void(Source&)>& body) {
        if (!_x) return;
        _x->Block(MtlExporter::Frame, "[" + std::to_string(index) + "]", body);
        _x->CountCommand();
    };
    // Residency and fences are on the render and compute encoders, not on MTLCommandEncoder, so
    // the message goes through whichever protocol this encoder answers to.
    id<MTLRenderCommandEncoder> render = [_encoder conformsToProtocol:@protocol(MTLRenderCommandEncoder)]
                                             ? (id<MTLRenderCommandEncoder>)_encoder : nil;
    id<MTLComputeCommandEncoder> compute = [_encoder conformsToProtocol:@protocol(MTLComputeCommandEncoder)]
                                               ? (id<MTLComputeCommandEncoder>)_encoder : nil;
    id<MTLBlitCommandEncoder> blit = [_encoder conformsToProtocol:@protocol(MTLBlitCommandEncoder)]
                                         ? (id<MTLBlitCommandEncoder>)_encoder : nil;

    if (m.compare(0, 11, "useResource") == 0) {
        const MTLResourceUsage usage = (MTLResourceUsage)d.Flags("usage", MTL_TABLE(MTLResourceUsage));
        std::vector<id> resources;
        if (m.compare(0, 12, "useResources") == 0) resources = ObjectArray(d, "resources");
        else if (id one = d.Object("resource")) resources.push_back(one);
        resources.erase(std::remove(resources.begin(), resources.end(), nil), resources.end());
        if (resources.empty()) { LeftOut(index, m, "none of the resources are in the replay"); return true; }
        for (id resource : resources) {
            if (render) [render useResource:(id<MTLResource>)resource usage:usage];
            else if (compute) [compute useResource:(id<MTLResource>)resource usage:usage];
        }
        emitWith([&](Source& w) {
            for (id resource : resources) {
                w.Line("[" + var + " useResource:" + w.Object(resource) + " usage:" +
                       Source::Flags(MTL_TABLE(MTLResourceUsage), usage) + "];");
            }
        });
        return true;
    }
    if (m.compare(0, 7, "useHeap") == 0) {
        std::vector<id> heaps;
        if (m.compare(0, 8, "useHeaps") == 0) heaps = ObjectArray(d, "heaps");
        else if (id one = d.Object("heap")) heaps.push_back(one);
        heaps.erase(std::remove(heaps.begin(), heaps.end(), nil), heaps.end());
        if (heaps.empty()) { LeftOut(index, m, "none of the heaps are in the replay"); return true; }
        for (id heap : heaps) {
            if (render) [render useHeap:(id<MTLHeap>)heap];
            else if (compute) [compute useHeap:(id<MTLHeap>)heap];
        }
        emitWith([&](Source& w) {
            for (id heap : heaps) w.Line("[" + var + " useHeap:" + w.Object(heap) + "];");
        });
        return true;
    }
    if (m.compare(0, 11, "updateFence") == 0 || m.compare(0, 12, "waitForFence") == 0) {
        id<MTLFence> fence = (id<MTLFence>)d.Object("fence");
        if (!fence) { LeftOut(index, m, "the fence is not in the replay"); return true; }
        const bool update = m[0] == 'u';
        // A render encoder's fences name the stages they cover; the others do not.
        if (render) {
            const MTLRenderStages stages = (MTLRenderStages)d.Flags(update ? "afterStages" : "beforeStages",
                                                                    MTL_TABLE(MTLRenderStages),
                                                                    (uint64_t)(MTLRenderStageVertex | MTLRenderStageFragment));
            if (update) [render updateFence:fence afterStages:stages];
            else [render waitForFence:fence beforeStages:stages];
            emit("[" + var + (update ? " updateFence:" : " waitForFence:") + ExportName(fence) +
                 (update ? " afterStages:" : " beforeStages:") +
                 Source::Flags(MTL_TABLE(MTLRenderStages), (uint64_t)stages) + "];");
        } else if (compute) {
            if (update) [compute updateFence:fence]; else [compute waitForFence:fence];
            emit("[" + var + (update ? " updateFence:" : " waitForFence:") + ExportName(fence) + "];");
        } else if (blit) {
            if (update) [blit updateFence:fence]; else [blit waitForFence:fence];
            emit("[" + var + (update ? " updateFence:" : " waitForFence:") + ExportName(fence) + "];");
        }
        return true;
    }
    return false;
}

/** The exporter's name for an object, or "nil" when there is no export. */
std::string MtlReplayer::ExportName(id object) const {
    if (!_x || object == nil) return "nil";
    const std::string name = _x->NameOf(object);
    return name.empty() ? "nil" : name;
}

// ------------------------------------------------------------------------------------------
// Read-back and comparison

void MtlReplayer::QueuePassReadbacks() {
    const auto it = _targets.find({_commandBufferId, _pass.passIndex});
    if (it == _targets.end() || !_commandBuffer) return;
    MTLRenderPassDescriptor* pass = _pass.renderPass;

    for (const JValue* entry : it->second) {
        const JValue* info = entry->Get("info");
        if (!info) continue;
        MtlTargetComparison c;
        c.texture = info->Get("id") ? info->Get("id")->Uint() : 0;
        c.commandBuffer = _commandBufferId;
        c.frame = info->Get("frame") ? (uint32_t)info->Get("frame")->Uint() : 0;
        c.passIndex = _pass.passIndex;
        c.attachment = info->Get("attachment") ? (uint32_t)info->Get("attachment")->Uint() : 0;
        c.format = info->Get("format") ? std::string(info->Get("format")->Str()) : std::string();
        c.aspect = info->Get("aspect") ? std::string(info->Get("aspect")->Str()) : std::string("color");
        c.width = info->Get("width") ? (uint32_t)info->Get("width")->Uint() : 0;
        c.height = info->Get("height") ? (uint32_t)info->Get("height")->Uint() : 0;

        if (info->Get("error")) {
            c.undefined = true;
            c.note = std::string("the capture could not read it: ") + std::string(info->Get("error")->Str());
            _readbacks.push_back({c, nil, 0, nullptr, 0});
            continue;
        }
        if (!pass) {
            c.note = "the pass is not a render pass in the replay";
            _readbacks.push_back({c, nil, 0, nullptr, 0});
            continue;
        }

        // What the capture read: the attachment, or its resolve when it is multisampled.
        MTLRenderPassAttachmentDescriptor* attachment =
            c.aspect == "depth" ? (MTLRenderPassAttachmentDescriptor*)pass.depthAttachment
          : c.aspect == "stencil" ? (MTLRenderPassAttachmentDescriptor*)pass.stencilAttachment
          : (MTLRenderPassAttachmentDescriptor*)pass.colorAttachments[c.attachment];
        id<MTLTexture> source = attachment.texture;
        NSUInteger level = attachment.level, slice = attachment.slice;
        if (source.sampleCount > 1) {
            if (!attachment.resolveTexture) {
                c.undefined = true;
                c.note = "a multisampled attachment with no resolve cannot be read";
                _readbacks.push_back({c, nil, 0, nullptr, 0});
                continue;
            }
            source = attachment.resolveTexture;
            level = attachment.resolveLevel;
            slice = attachment.resolveSlice;
        }
        if (!source) {
            c.note = "the attachment has no texture in the replay";
            _readbacks.push_back({c, nil, 0, nullptr, 0});
            continue;
        }

        MTLBlitOption options = MTLBlitOptionNone;
        const mtlinsp::PixelFormatInfo format = c.aspect == "depth"
            ? mtlinsp::DepthReadbackDetails(source.pixelFormat, &options)
            : mtlinsp::PixelFormatDetails(source.pixelFormat);
        if (!format.blockBytes) {
            c.note = "the replay has no layout for this pixel format";
            _readbacks.push_back({c, nil, 0, nullptr, 0});
            continue;
        }
        const uint32_t width = (uint32_t)std::max<NSUInteger>(1, source.width >> level);
        const uint32_t height = (uint32_t)std::max<NSUInteger>(1, source.height >> level);
        uint64_t rowBytes = 0;
        const uint64_t imageBytes = mtlinsp::PixelFormatImageSize(format, width, height, &rowBytes);
        if (!imageBytes) continue;

        Readback readback;
        readback.comparison = c;
        readback.rowBytes = rowBytes;
        readback.staging = [_device newBufferWithLength:(NSUInteger)imageBytes
                                                options:MTLResourceStorageModeShared];
        _capture.Payload(entry->Get("payload"), readback.captured, readback.capturedSize);

        id<MTLBlitCommandEncoder> blit = [_commandBuffer blitCommandEncoder];
        blit.label = @"mtlinsp_replay read-back";
        [blit copyFromTexture:source
                  sourceSlice:slice
                  sourceLevel:level
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(width, height, 1)
                     toBuffer:readback.staging
            destinationOffset:0
       destinationBytesPerRow:(NSUInteger)rowBytes
     destinationBytesPerImage:(NSUInteger)imageBytes
                      options:options];
        [blit endEncoding];
        _readbacks.push_back(readback);

        if (_x) {
            const std::string name = _x->NameOf(source);
            const std::string where = _x->Data(readback.captured, readback.capturedSize);
            const std::string label = "target " + std::to_string(c.texture) + " of pass " +
                                      std::to_string(c.passIndex);
            _x->Block(MtlExporter::Frame, label, [&](Source& w) {
                w.Line("ReadbackTexture(" + _commandBufferVar + ", " + name + ", \"" + label + "\", " +
                       Source::Uint(slice) + ", " + Source::Uint(level) + ", " + Source::Uint(width) + ", " +
                       Source::Uint(height) + ", " + Source::Uint(rowBytes) + ", " + Source::Uint(imageBytes) +
                       ", " + Source::Enum(MTL_TABLE(MTLBlitOption), (int64_t)options) + ", " + where + ", " +
                       Source::Uint(readback.capturedSize) + ");");
            });
            _x->CountTarget();
        }
    }
}

std::string MtlReplayer::ComputeTextureAccess(uint64_t pipelineId, uint64_t slot) const {
    const JValue* record = Record(pipelineId);
    const JValue* args = record ? record->Get("args") : nullptr;
    const JValue* reflection = args ? args->Get("reflection") : nullptr;
    const JValue* compute = reflection ? reflection->Get("compute") : nullptr;
    const JValue* textures = compute ? compute->Get("textures") : nullptr;
    if (!textures || !textures->IsArray()) return "";
    for (uint32_t i = 0; i < textures->count; ++i) {
        const JValue* entry = &textures->items[i];
        const JValue* index = entry->Get("index");
        if (!index || index->Uint() != slot) continue;
        const JValue* access = entry->Get("access");
        return access && access->IsString() ? std::string(access->Str()) : std::string();
    }
    return "";
}

void MtlReplayer::CompareWrittenTextures(MtlReplayReport& report) {
    (void)report;
    if (!_options.compareTargets || _writtenTextures.empty()) return;
    // A command buffer of the replay's own: every one of the frame's has been committed by now, and
    // the textures are compared as they stood when the frame finished.
    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    cb.label = @"mtlinsp_replay written textures";
    bool any = false;
    bool exportedAny = false;


    for (uint32_t i = 0; i < (_capture.Textures() ? _capture.Textures()->count : 0); ++i) {
        const JValue* entry = &_capture.Textures()->items[i];
        const JValue* info = entry->Get("info");
        if (!info) continue;
        const uint64_t textureId = info->Get("id") ? info->Get("id")->Uint() : 0;
        const auto written = _writtenTextures.find(textureId);
        if (written == _writtenTextures.end()) continue;
        if (info->Get("error") || !entry->Get("payload")) continue;
        // Only the top level: a kernel writes the level it was bound, and the capture reads that.
        if (info->Get("mip") && info->Get("mip")->Uint() != 0) continue;

        id<MTLTexture> texture = (id<MTLTexture>)Object(textureId);
        if (!texture) continue;
        const mtlinsp::PixelFormatInfo format = mtlinsp::PixelFormatDetails(texture.pixelFormat);
        if (!format.blockBytes) continue;
        const uint32_t width = (uint32_t)texture.width, height = (uint32_t)texture.height;
        uint64_t rowBytes = 0;
        const uint64_t imageBytes = mtlinsp::PixelFormatImageSize(format, width, height, &rowBytes);
        if (!imageBytes) continue;

        Readback readback;
        MtlTargetComparison& c = readback.comparison;
        c.texture = textureId;
        c.frame = info->Get("frame") ? (uint32_t)info->Get("frame")->Uint() : 0;
        c.commandBuffer = info->Get("commandBuffer") ? info->Get("commandBuffer")->Uint() : 0;
        c.passIndex = info->Get("passIndex") ? (uint32_t)info->Get("passIndex")->Uint() : 0;
        c.format = info->Get("format") ? std::string(info->Get("format")->Str()) : std::string();
        c.aspect = "color";
        c.width = width;
        c.height = height;
        if (!written->second) {
            // The kernel that wrote it also read a texture it writes, so the frame continues from
            // contents the capture does not hold. test/path_tracer/metal is exactly this: one more
            // sample into a running mean whose earlier samples no capture recorded, and its
            // tonemapped output is that mean tonemapped.
            c.undefined = true;
            c.note = "a compute pass of this frame accumulates into a texture it also reads, so what "
                     "it wrote continues from contents the capture does not hold: it cannot be reproduced";
            _readbacks.push_back({c, nil, 0, nullptr, 0});
            continue;
        }
        c.note = "written by a compute pass, compared after the frame";
        readback.rowBytes = rowBytes;
        readback.staging = [_device newBufferWithLength:(NSUInteger)imageBytes
                                                options:MTLResourceStorageModeShared];
        _capture.Payload(entry->Get("payload"), readback.captured, readback.capturedSize);
        if (!readback.staging || !readback.captured) continue;

        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        blit.label = @"mtlinsp_replay read-back";
        [blit copyFromTexture:texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(width, height, 1)
                     toBuffer:readback.staging
            destinationOffset:0
       destinationBytesPerRow:(NSUInteger)rowBytes
     destinationBytesPerImage:(NSUInteger)imageBytes
                      options:MTLBlitOptionNone];
        [blit endEncoding];
        _readbacks.push_back(readback);
        any = true;

        if (_x) {
            const std::string name = _x->NameOf(texture);
            const std::string where = _x->Data(readback.captured, readback.capturedSize);
            const std::string label = "storage texture " + std::to_string(textureId);
            // A command buffer of its own, as here: the frame's have all been committed by this
            // point, and asking a committed one for an encoder is a Metal assertion rather than an
            // error. Opened before the first of these and committed after the last.
            if (!exportedAny) {
                _x->Blank(MtlExporter::Frame);
                _x->Comment(MtlExporter::Frame, "The frame's storage textures, read back after it ran.");
                _x->Block(MtlExporter::Frame, "", [&](Source& w) {
                    w.Line(_commandBufferVar + " = [queue commandBuffer];");
                });
                exportedAny = true;
            }
            _x->Block(MtlExporter::Frame, label, [&](Source& w) {
                w.Line("ReadbackTexture(" + _commandBufferVar + ", " + name + ", \"" + label + "\", 0, 0, " +
                       Source::Uint(width) + ", " + Source::Uint(height) + ", " + Source::Uint(rowBytes) +
                       ", " + Source::Uint(imageBytes) + ", MTLBlitOptionNone, " + where + ", " +
                       Source::Uint(readback.capturedSize) + ");");
            });
            _x->CountTarget();
        }
    }
    if (exportedAny && _x) {
        _x->Block(MtlExporter::Frame, "", [&](Source& w) {
            w.Line("[" + _commandBufferVar + " commit];");
        });
        _x->Block(MtlExporter::Frame, "", [&](Source& w) {
            w.Line("[" + _commandBufferVar + " waitUntilCompleted];");
        });
    }
    if (!any) return;
    [cb commit];
    _committed.push_back(cb);
}

void MtlReplayer::CompleteReadbacks(MtlReplayReport& report) {
    for (Readback& r : _readbacks) {
        MtlTargetComparison& c = r.comparison;
        if (r.staging && r.captured && r.capturedSize) {
            const uint8_t* replayed = (const uint8_t*)r.staging.contents;
            const size_t size = std::min<size_t>(r.capturedSize, (size_t)r.staging.length);
            c.compared = true;
            c.texels = (uint64_t)c.width * c.height;
            // Bytes per texel from what was actually read, so a format the tables size differently
            // than the capture did still compares the same bytes against each other.
            const size_t texelBytes = c.texels ? std::max<size_t>(1, size / (size_t)c.texels) : 1;
            for (size_t i = 0; i < size; i += texelBytes) {
                const size_t width = std::min(texelBytes, size - i);
                if (std::memcmp(replayed + i, r.captured + i, width) == 0) continue;
                ++c.differingTexels;
                for (size_t b = i; b < i + width; ++b) {
                    c.maxByteDelta = std::max<uint32_t>(c.maxByteDelta,
                                                        (uint32_t)std::abs((int)replayed[b] - (int)r.captured[b]));
                }
            }
            if (_options.keepPixels) {
                c.captured.assign(r.captured, r.captured + size);
                c.replayed.assign(replayed, replayed + size);
            }
        } else if (!c.undefined && c.note.empty()) {
            c.note = "nothing was read back";
        }
        report.comparisons.push_back(std::move(c));
    }
}

} // namespace mtlreplay
