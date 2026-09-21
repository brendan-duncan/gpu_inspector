// Metal ray tracing: see raytracing.h for what this is and why it is the smallest of the three
// backends' ray tracing code.
//
// Properties added after macOS 11 (the deployment target) are read through `respondsToSelector:`
// rather than behind `@available` blocks, and the geometry descriptor classes are discriminated
// with NSClassFromString, so this file compiles and runs against any SDK and any macOS from 11 up
// without a thicket of version guards. reflection.mm reads Metal's binding objects the same way and
// for the same reason.
#include "raytracing.h"

#include "capture.h"
#include "formats.h"
#include "hooks.h"
#include "hooks_common.h"
#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"

#import <objc/message.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtlinsp {
namespace {

using vkinsp::JsonWriter;

// ---------------------------------------------------------------------------------------------
// Reading Metal objects through selectors

NSUInteger UIntProp(id object, const char *name) {
    SEL sel = sel_registerName(name);
    if (object == nil || ![object respondsToSelector:sel]) return 0;
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(object, sel);
}

BOOL BoolProp(id object, const char *name) {
    SEL sel = sel_registerName(name);
    if (object == nil || ![object respondsToSelector:sel]) return NO;
    return ((BOOL (*)(id, SEL))objc_msgSend)(object, sel);
}

float FloatProp(id object, const char *name) {
    SEL sel = sel_registerName(name);
    if (object == nil || ![object respondsToSelector:sel]) return 0.0f;
    return ((float (*)(id, SEL))objc_msgSend)(object, sel);
}

id ObjectProp(id object, const char *name) {
    SEL sel = sel_registerName(name);
    if (object == nil || ![object respondsToSelector:sel]) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(object, sel);
}

bool IsKind(id object, const char *className) {
    Class cls = NSClassFromString([NSString stringWithUTF8String:className]);
    return cls != nil && object != nil && [object isKindOfClass:cls];
}

// ---------------------------------------------------------------------------------------------
// What a structure's last build read
//
// Kept so a capture that begins after the build can read the same ranges back
// (ReadBackEarlierStructures). A buffer is held by its tracked **id**, not retained: the tracker
// holds every object weakly on purpose (tracker.h, LiveObject), and retaining an engine's whole
// static geometry for as long as the inspector is attached would change what the tool is measuring.
// An id whose object the application has since released reads back as nothing, which is the honest
// answer.

struct StructureInput {
    const char *field = "";   // a string literal: vertexBuffer, indexBuffer, instanceDescriptorBuffer, ...
    uint32_t geometry = 0;
    uint64_t bufferId = 0;
    uint64_t offset = 0;
    uint64_t size = 0;        // 0 for "to the end of the buffer"
};

struct StructureRecord {
    /** The last build's descriptor, already serialized, with no capture ids in it. */
    std::string descriptor;
    std::string method;
    std::string mode;
    uint64_t primitives = 0;
    std::vector<StructureInput> inputs;
    /** The capture the build was recorded in, which needs no second read-back of it. */
    uint64_t capture = 0;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, StructureRecord> g_structures;   // by the structure's object id
uint64_t g_readBackSerial = 0;

/** Every entry of a function table, so an ObjectUpdate can carry the whole of it each time. */
struct TableEntry {
    std::string function;      // the MTLFunctionHandle's own name
    std::string opaque;        // "triangle" or "curve" for a built-in intersection function
    uint64_t signature = 0;
    bool set = false;
};

struct TableRecord {
    NSUInteger functionCount = 0;
    std::vector<TableEntry> entries;
    /** Buffers bound to the table's own argument slots, by index. */
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> buffers;   // index -> {bufferId, offset}
    std::unordered_map<uint64_t, uint64_t> visibleTables;                  // index -> table object id
};

std::unordered_map<uint64_t, TableRecord> g_tables;   // by the table's object id

// ---------------------------------------------------------------------------------------------
// Serializing a descriptor

/**
 * One buffer an input names: the tracked buffer, where in it, and the contents when they were read.
 *
 * This is the shape the Vulkan layer and the D3D12 library arrive at after resolving an address
 * (`{deviceAddress, buffer, offset, capture}`); Metal starts here, because the descriptor holds the
 * buffer itself.
 */
void WriteInput(JsonWriter &w, const char *key, id buffer, NSUInteger offset, uint64_t size,
                id encoder, std::vector<StructureInput> *inputs, uint32_t geometry,
                std::vector<uint64_t> *captured) {
    if (buffer == nil) return;
    w.Key(key);
    w.BeginObject();
    w.Key("buffer");
    WriteRef(w, buffer, "MTLBuffer");
    w.Key("offset"); w.Uint(offset);
    if (size != 0) w.Key("size"), w.Uint(size);
    if (encoder != nil) {
        // Whole, not truncated to maxBufferSize: a build's geometry is the one thing in a capture
        // that is useless clipped — a mesh cut at 64 KB draws as a corner of itself.
        const uint64_t captureId = QueueBufferCapture(encoder, buffer, offset, size, /*whole=*/true);
        if (captureId != 0) {
            w.Key("capture"); w.Uint(captureId);
            if (captured != nullptr) captured->push_back(captureId);
        }
    }
    w.EndObject();
    if (inputs != nullptr) inputs->push_back({key, geometry, IdOf(buffer), offset, size});
}

/** Bytes one index takes, for sizing an index buffer from a triangle count. */
uint64_t IndexSize(NSUInteger indexType) {
    return indexType == MTLIndexTypeUInt16 ? 2 : 4;
}

/** The stride of one instance descriptor of each type, which is what an instance buffer is walked by. */
uint64_t InstanceDescriptorStride(NSUInteger type) {
    switch (type) {
        case 0: return 64;    // MTLAccelerationStructureInstanceDescriptor
        case 1: return 68;    // ...UserIDInstanceDescriptor
        case 2: return 60;    // ...MotionInstanceDescriptor (no transform; a keyframe range instead)
        case 3: return 80;    // MTLIndirectAccelerationStructureInstanceDescriptor
        case 4: return 76;    // MTLIndirectAccelerationStructureMotionInstanceDescriptor
        default: return 64;
    }
}

void WriteMotionKeyframes(JsonWriter &w, const char *key, id keyframes, uint64_t stride, uint64_t count,
                          id encoder, std::vector<StructureInput> *inputs, uint32_t geometry,
                          std::vector<uint64_t> *captured) {
    if (![keyframes isKindOfClass:[NSArray class]]) return;
    w.Key(key);
    w.BeginArray();
    for (id frame in (NSArray *)keyframes) {
        w.BeginObject();
        // Every keyframe is one buffer of the same geometry; they share the field name, which is why
        // an input's geometry index is not enough on its own to name one. The array order is.
        WriteInput(w, "buffer", ObjectProp(frame, "buffer"), UIntProp(frame, "offset"), stride * count,
                   encoder, inputs, geometry, captured);
        w.EndObject();
    }
    w.EndArray();
}

/** One geometry of a primitive structure; returns the primitives it holds. */
uint64_t WriteGeometry(JsonWriter &w, id g, uint32_t index, id encoder,
                       std::vector<StructureInput> *inputs, std::vector<uint64_t> *captured) {
    w.BeginObject();
    id label = ObjectProp(g, "label");
    if (label != nil) w.Key("label"), w.String([(NSString *)label UTF8String]);
    w.Key("intersectionFunctionTableOffset"); w.Uint(UIntProp(g, "intersectionFunctionTableOffset"));
    w.Key("opaque"); w.Boolean(BoolProp(g, "opaque"));
    w.Key("allowDuplicateIntersectionFunctionInvocation");
    w.Boolean(BoolProp(g, "allowDuplicateIntersectionFunctionInvocation"));

    uint64_t primitives = 0;
    const char *kind = "unknown";
    if (IsKind(g, "MTLAccelerationStructureTriangleGeometryDescriptor") ||
        IsKind(g, "MTLAccelerationStructureMotionTriangleGeometryDescriptor")) {
        const bool motion = IsKind(g, "MTLAccelerationStructureMotionTriangleGeometryDescriptor");
        kind = motion ? "motionTriangles" : "triangles";
        primitives = UIntProp(g, "triangleCount");
        const NSUInteger stride = UIntProp(g, "vertexStride");
        // vertexFormat arrived in macOS 13; before it, a vertex was three floats by definition.
        const NSUInteger format = [g respondsToSelector:sel_registerName("vertexFormat")]
                                ? UIntProp(g, "vertexFormat") : (NSUInteger)MTLAttributeFormatFloat3;
        const NSUInteger indexType = UIntProp(g, "indexType");
        w.Key("triangleCount"); w.Uint(primitives);
        w.Key("vertexStride"); w.Uint(stride);
        w.Key("vertexFormat"); WriteEnum(w, VertexFormatEnumName((MTLVertexFormat)format), format);
        // The same layout under the name the UI's vertex decoder knows: MTLAttributeFormat and
        // MTLVertexFormat are the same 42 values (formats.h), so a structure's vertices decode
        // through exactly what a draw's do.
        w.Key("vkFormat"); w.String(VertexFormatCanonicalName((MTLVertexFormat)format));
        w.Key("indexType"); WriteEnum(w, IndexTypeEnumName((MTLIndexType)indexType), indexType);
        if ([g respondsToSelector:sel_registerName("transformationMatrixLayout")]) {
            const NSUInteger layout = UIntProp(g, "transformationMatrixLayout");
            w.Key("transformationMatrixLayout");
            WriteEnum(w, MatrixLayoutEnumName(layout), layout);
        }
        if (motion) {
            // Metal gives no vertex count, so a keyframe's buffer is read to its end (size 0) the
            // same way a non-motion one is.
            WriteMotionKeyframes(w, "vertexBuffers", ObjectProp(g, "vertexBuffers"), 0, 0,
                                 encoder, inputs, index, captured);
        } else {
            // Unlike Vulkan's build, a Metal triangle geometry carries no maxVertex, so there is no
            // vertex count to size the buffer from: it is read from its offset to its end, which is
            // also what makes an indexed mesh come out whole.
            WriteInput(w, "vertexBuffer", ObjectProp(g, "vertexBuffer"), UIntProp(g, "vertexBufferOffset"),
                       0, encoder, inputs, index, captured);
        }
        if (indexType != 0 || ObjectProp(g, "indexBuffer") != nil) {
            WriteInput(w, "indexBuffer", ObjectProp(g, "indexBuffer"), UIntProp(g, "indexBufferOffset"),
                       primitives * 3 * IndexSize(indexType), encoder, inputs, index, captured);
        }
        WriteInput(w, "transformationMatrixBuffer", ObjectProp(g, "transformationMatrixBuffer"),
                   UIntProp(g, "transformationMatrixBufferOffset"), 0, encoder, inputs, index, captured);
    } else if (IsKind(g, "MTLAccelerationStructureBoundingBoxGeometryDescriptor") ||
               IsKind(g, "MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor")) {
        const bool motion = IsKind(g, "MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor");
        kind = motion ? "motionBoundingBoxes" : "boundingBoxes";
        primitives = UIntProp(g, "boundingBoxCount");
        const NSUInteger stride = UIntProp(g, "boundingBoxStride");
        w.Key("boundingBoxCount"); w.Uint(primitives);
        w.Key("boundingBoxStride"); w.Uint(stride);
        if (motion) {
            WriteMotionKeyframes(w, "boundingBoxBuffers", ObjectProp(g, "boundingBoxBuffers"),
                                 stride, primitives, encoder, inputs, index, captured);
        } else {
            WriteInput(w, "boundingBoxBuffer", ObjectProp(g, "boundingBoxBuffer"),
                       UIntProp(g, "boundingBoxBufferOffset"), stride * primitives,
                       encoder, inputs, index, captured);
        }
    } else if (IsKind(g, "MTLAccelerationStructureCurveGeometryDescriptor") ||
               IsKind(g, "MTLAccelerationStructureMotionCurveGeometryDescriptor")) {
        // Curves have no counterpart in Vulkan or D3D12 core, so nothing draws them yet; what they
        // were built from is still worth saying.
        const bool motion = IsKind(g, "MTLAccelerationStructureMotionCurveGeometryDescriptor");
        kind = motion ? "motionCurves" : "curves";
        primitives = UIntProp(g, "segmentCount");
        const NSUInteger indexType = UIntProp(g, "indexType");
        w.Key("segmentCount"); w.Uint(primitives);
        w.Key("segmentControlPointCount"); w.Uint(UIntProp(g, "segmentControlPointCount"));
        w.Key("controlPointCount"); w.Uint(UIntProp(g, "controlPointCount"));
        w.Key("controlPointStride"); w.Uint(UIntProp(g, "controlPointStride"));
        const NSUInteger cpFormat = UIntProp(g, "controlPointFormat");
        w.Key("controlPointFormat"); WriteEnum(w, VertexFormatEnumName((MTLVertexFormat)cpFormat), cpFormat);
        const NSUInteger curveType = UIntProp(g, "curveType");
        w.Key("curveType"); WriteEnum(w, CurveTypeEnumName(curveType), curveType);
        const NSUInteger basis = UIntProp(g, "curveBasis");
        w.Key("curveBasis"); WriteEnum(w, CurveBasisEnumName(basis), basis);
        const NSUInteger caps = UIntProp(g, "curveEndCaps");
        w.Key("curveEndCaps"); WriteEnum(w, CurveEndCapsEnumName(caps), caps);
        w.Key("indexType"); WriteEnum(w, IndexTypeEnumName((MTLIndexType)indexType), indexType);
        if (motion) {
            WriteMotionKeyframes(w, "controlPointBuffers", ObjectProp(g, "controlPointBuffers"), 0, 0,
                                 encoder, inputs, index, captured);
            WriteMotionKeyframes(w, "radiusBuffers", ObjectProp(g, "radiusBuffers"), 0, 0,
                                 encoder, inputs, index, captured);
        } else {
            WriteInput(w, "controlPointBuffer", ObjectProp(g, "controlPointBuffer"),
                       UIntProp(g, "controlPointBufferOffset"), 0, encoder, inputs, index, captured);
            WriteInput(w, "radiusBuffer", ObjectProp(g, "radiusBuffer"), UIntProp(g, "radiusBufferOffset"),
                       0, encoder, inputs, index, captured);
        }
        WriteInput(w, "indexBuffer", ObjectProp(g, "indexBuffer"), UIntProp(g, "indexBufferOffset"),
                   0, encoder, inputs, index, captured);
    }
    w.Key("kind"); w.String(kind);
    w.Key("primitiveCount"); w.Uint(primitives);

    // Per-primitive data, which an intersection function reads alongside the primitive itself. Only
    // when there is some: the stride and element size mean nothing without the buffer, and this
    // library's aim is to show what the application set rather than what Metal defaulted.
    id primitiveData = ObjectProp(g, "primitiveDataBuffer");
    if (primitiveData != nil) {
        const NSUInteger stride = UIntProp(g, "primitiveDataStride");
        w.Key("primitiveDataStride"); w.Uint(stride);
        w.Key("primitiveDataElementSize"); w.Uint(UIntProp(g, "primitiveDataElementSize"));
        WriteInput(w, "primitiveDataBuffer", primitiveData,
                   UIntProp(g, "primitiveDataBufferOffset"), stride * primitives,
                   encoder, inputs, index, captured);
    }
    w.EndObject();
    return primitives;
}

/**
 * A whole descriptor. `encoder` non-nil queues each input's contents for read-back and puts the
 * capture id beside the buffer; nil writes the shape alone, which is what a structure's `build`
 * update and its remembered inputs keep.
 */
std::string DescriptorJson(MTLAccelerationStructureDescriptor *descriptor, id encoder,
                           std::vector<StructureInput> *inputs, uint64_t *primitivesOut,
                           std::vector<uint64_t> *captured) {
    JsonWriter w;
    w.BeginObject();
    if (descriptor == nil) {
        w.EndObject();
        return std::move(w.str());
    }
    const NSUInteger usage = UIntProp(descriptor, "usage");
    w.Key("usage"); w.String(AccelerationStructureUsageFlags(usage));
    uint64_t primitives = 0;

    if (IsKind(descriptor, "MTLInstanceAccelerationStructureDescriptor")) {
        w.Key("kind"); w.String("instance");
        const NSUInteger count = UIntProp(descriptor, "instanceCount");
        const NSUInteger type = [descriptor respondsToSelector:sel_registerName("instanceDescriptorType")]
                              ? UIntProp(descriptor, "instanceDescriptorType") : 0;
        NSUInteger stride = UIntProp(descriptor, "instanceDescriptorStride");
        if (stride == 0) stride = InstanceDescriptorStride(type);
        primitives = count;
        w.Key("instanceCount"); w.Uint(count);
        w.Key("instanceDescriptorType");
        WriteEnum(w, InstanceDescriptorTypeEnumName(type), type);
        w.Key("instanceDescriptorStride"); w.Uint(stride);
        if ([descriptor respondsToSelector:sel_registerName("instanceTransformationMatrixLayout")]) {
            const NSUInteger layout = UIntProp(descriptor, "instanceTransformationMatrixLayout");
            w.Key("instanceTransformationMatrixLayout");
            WriteEnum(w, MatrixLayoutEnumName(layout), layout);
        }
        // The link a top level has and the other two APIs have to reconstruct: an instance's
        // `accelerationStructureIndex` indexes this array, so the bottom levels are named outright.
        w.Key("instancedAccelerationStructures");
        w.BeginArray();
        id instanced = ObjectProp(descriptor, "instancedAccelerationStructures");
        if ([instanced isKindOfClass:[NSArray class]]) {
            for (id s in (NSArray *)instanced) WriteRef(w, s, "MTLAccelerationStructure");
        }
        w.EndArray();
        WriteInput(w, "instanceDescriptorBuffer", ObjectProp(descriptor, "instanceDescriptorBuffer"),
                   UIntProp(descriptor, "instanceDescriptorBufferOffset"), stride * count,
                   encoder, inputs, 0, captured);
        // Only for a top level that actually animates its instances; without a transform buffer the
        // count is Metal's zero rather than anything the application said.
        id motionTransforms = ObjectProp(descriptor, "motionTransformBuffer");
        if (motionTransforms != nil) {
            w.Key("motionTransformCount"); w.Uint(UIntProp(descriptor, "motionTransformCount"));
            WriteInput(w, "motionTransformBuffer", motionTransforms,
                       UIntProp(descriptor, "motionTransformBufferOffset"), 0,
                       encoder, inputs, 0, captured);
        }
    } else if (IsKind(descriptor, "MTLIndirectInstanceAccelerationStructureDescriptor")) {
        // The indirect form takes its instance count from a buffer the GPU wrote, so the count here
        // is only the maximum the build may read.
        w.Key("kind"); w.String("instance");
        const NSUInteger maxCount = UIntProp(descriptor, "maxInstanceCount");
        const NSUInteger type = UIntProp(descriptor, "instanceDescriptorType");
        NSUInteger stride = UIntProp(descriptor, "instanceDescriptorStride");
        if (stride == 0) stride = InstanceDescriptorStride(type);
        primitives = maxCount;
        w.Key("indirect"); w.Boolean(true);
        w.Key("instanceCount"); w.Uint(maxCount);
        w.Key("maxInstanceCount"); w.Uint(maxCount);
        w.Key("instanceDescriptorType");
        WriteEnum(w, InstanceDescriptorTypeEnumName(type), type);
        w.Key("instanceDescriptorStride"); w.Uint(stride);
        WriteInput(w, "instanceDescriptorBuffer", ObjectProp(descriptor, "instanceDescriptorBuffer"),
                   UIntProp(descriptor, "instanceDescriptorBufferOffset"), stride * maxCount,
                   encoder, inputs, 0, captured);
        WriteInput(w, "instanceCountBuffer", ObjectProp(descriptor, "instanceCountBuffer"),
                   UIntProp(descriptor, "instanceCountBufferOffset"), sizeof(uint32_t),
                   encoder, inputs, 0, captured);
        WriteInput(w, "motionTransformBuffer", ObjectProp(descriptor, "motionTransformBuffer"),
                   UIntProp(descriptor, "motionTransformBufferOffset"), 0, encoder, inputs, 0, captured);
    } else {
        w.Key("kind"); w.String("primitive");
        if ([descriptor respondsToSelector:sel_registerName("motionKeyframeCount")]) {
            // Metal defaults this to 1, which means "not a motion structure"; only a real keyframe
            // count says anything, and the border modes and times mean nothing without one.
            const NSUInteger keyframes = UIntProp(descriptor, "motionKeyframeCount");
            if (keyframes > 1) {
                w.Key("motionKeyframeCount"); w.Uint(keyframes);
                const NSUInteger startMode = UIntProp(descriptor, "motionStartBorderMode");
                const NSUInteger endMode = UIntProp(descriptor, "motionEndBorderMode");
                w.Key("motionStartBorderMode");
                WriteEnum(w, MotionBorderModeEnumName(startMode), startMode);
                w.Key("motionEndBorderMode");
                WriteEnum(w, MotionBorderModeEnumName(endMode), endMode);
                w.Key("motionStartTime"); w.Double(FloatProp(descriptor, "motionStartTime"));
                w.Key("motionEndTime"); w.Double(FloatProp(descriptor, "motionEndTime"));
            }
        }
        w.Key("geometries");
        w.BeginArray();
        id geometries = ObjectProp(descriptor, "geometryDescriptors");
        if ([geometries isKindOfClass:[NSArray class]]) {
            uint32_t index = 0;
            for (id g in (NSArray *)geometries) primitives += WriteGeometry(w, g, index++, encoder, inputs, captured);
        }
        w.EndArray();
    }
    w.Key("primitiveCount"); w.Uint(primitives);
    w.EndObject();
    if (primitivesOut != nullptr) *primitivesOut = primitives;
    return std::move(w.str());
}

/** The flat list of a build's inputs, in the shape the UI's captureIdOf and ourInputs both read. */
std::string InputsJson(const std::vector<StructureInput> &inputs,
                       const std::vector<uint64_t> &captureIds) {
    JsonWriter w;
    w.BeginArray();
    for (size_t i = 0; i < inputs.size(); i++) {
        w.BeginObject();
        w.Key("geometry"); w.Uint(inputs[i].geometry);
        w.Key("field"); w.String(inputs[i].field);
        w.Key("buffer"); w.Uint(inputs[i].bufferId);
        w.Key("offset"); w.Uint(inputs[i].offset);
        if (i < captureIds.size() && captureIds[i] != 0) { w.Key("capture"); w.Uint(captureIds[i]); }
        w.EndObject();
    }
    w.EndArray();
    return std::move(w.str());
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Acceleration structures

void NoteAccelerationStructure(id structure, id parent, const char *cmd,
                               MTLAccelerationStructureDescriptor *descriptor) {
    if (structure == nil) return;
    Args a;
    // Its own size, which is what the structure costs in memory. Both other libraries have to ask
    // the driver for this (GetAccelerationStructureBuildSizesKHR, GetRaytracingAccelerationStructurePrebuildInfo)
    // and record it as the build's `resultSize`; Metal puts it on the object.
    a.u("size", UIntProp(structure, "size"));
    // An acceleration structure is an MTLResource, so it reports its heap placement like any other.
    WriteMemoryInfo(a, structure);
    // Its gpuResourceID, which is how an argument buffer and an indirect instance descriptor name it.
    WriteGpuIds(a, structure);
    if (descriptor != nil) {
        // Created from a descriptor: the inputs are known before any build, which is the one case
        // where a structure says what is in it without a build being captured. Most applications
        // use newAccelerationStructureWithSize: instead (test/path_tracer/metal does), so the
        // build remains the primary source.
        a.raw("descriptor", DescriptorJson(descriptor, nil, nullptr, nullptr, nullptr));
    }
    const uint64_t structureId = Track(structure, "MTLAccelerationStructure", cmd, parent, a.str());
    if (structureId == 0 || descriptor == nil) return;
    StructureRecord record;
    uint64_t primitives = 0;
    record.descriptor = DescriptorJson(descriptor, nil, &record.inputs, &primitives, nullptr);
    record.method = cmd;
    record.mode = "BUILD";
    record.primitives = primitives;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_structures[structureId] = std::move(record);
}

std::string NoteAccelerationStructureBuild(id encoder, const char *method, id destination,
                                           MTLAccelerationStructureDescriptor *descriptor,
                                           id scratch, NSUInteger scratchOffset, id source) {
    const bool refit = source != nil;
    std::vector<StructureInput> inputs;
    std::vector<uint64_t> captured;
    uint64_t primitives = 0;
    // One descriptor serialization serves the command and the structure: the command's carries the
    // capture ids, the structure's is the shape alone (see raytracing.h on why the ids go on the
    // command).
    const std::string withCaptures = DescriptorJson(descriptor, encoder, &inputs, &primitives, &captured);

    Args a;
    a.ref("accelerationStructure", destination, "MTLAccelerationStructure");
    if (refit) a.ref("sourceAccelerationStructure", source, "MTLAccelerationStructure");
    a.raw("descriptor", withCaptures);
    if (scratch != nil) a.ref("scratchBuffer", scratch, "MTLBuffer").u("scratchBufferOffset", scratchOffset);
    // The per-input capture ids, flat, keyed by geometry and field. The same shape the structure's
    // captureInputs carries, so one UI path reads a build in the frame and a build before it.
    a.raw("buildData", InputsJson(inputs, captured));

    const uint64_t structureId = IdOf(destination);
    if (structureId != 0) {
        // Nested under "build" on purpose. UpdateObject spreads its argument's *fields* into the
        // message and uses the key only to decide which update a later one replaces (tracker.h), so
        // the nesting has to be written here — and it has to be there, because `build` is the key
        // the shared views read a structure's last build under, whichever API recorded it
        // (renderer/acceleration_view.ts, structureFacts).
        JsonWriter w;
        w.BeginObject();
        w.Key("build");
        w.BeginObject();
        w.Key("method"); w.String(method);
        w.Key("mode"); w.String(refit ? "REFIT" : "BUILD");
        w.Key("resultSize"); w.Uint(UIntProp(destination, "size"));
        w.Key("primitiveCount"); w.Uint(primitives);
        w.Key("descriptor"); w.Raw(DescriptorJson(descriptor, nil, nullptr, nullptr, nullptr));
        w.EndObject();
        w.EndObject();
        UpdateObject(destination, "build", w.str());

        StructureRecord record;
        record.descriptor = DescriptorJson(descriptor, nil, &record.inputs, nullptr, nullptr);
        record.method = method;
        record.mode = refit ? "REFIT" : "BUILD";
        record.primitives = primitives;
        record.capture = Recording() ? CaptureSerial() : 0;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_structures[structureId] = std::move(record);
    }
    return a.str();
}

std::string NoteAccelerationStructureCopy(const char *method, id source, id destination,
                                          id buffer, NSUInteger offset) {
    Args a;
    if (destination != nil) a.ref("destinationAccelerationStructure", destination, "MTLAccelerationStructure");
    a.ref("sourceAccelerationStructure", source, "MTLAccelerationStructure");
    if (buffer != nil) a.ref("buffer", buffer, "MTLBuffer").u("offset", offset);

    // A copy gives the destination the source's contents, so it takes its build too — otherwise a
    // structure the application compacted would have nothing to show. The same reasoning as
    // NoteAccelerationStructureCopy in the D3D12 library.
    const uint64_t from = IdOf(source);
    const uint64_t to = IdOf(destination);
    if (from != 0 && to != 0) {
        StructureRecord copy;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_structures.find(from);
            if (it == g_structures.end()) return a.str();
            copy = it->second;
        }
        JsonWriter w;
        w.BeginObject();
        w.Key("build");
        w.BeginObject();
        w.Key("method"); w.String(method);
        w.Key("mode"); w.String(copy.mode.empty() ? "BUILD" : copy.mode.c_str());
        w.Key("copiedFrom"); w.Uint(from);
        w.Key("resultSize"); w.Uint(UIntProp(destination, "size"));
        w.Key("primitiveCount"); w.Uint(copy.primitives);
        w.Key("descriptor"); w.Raw(copy.descriptor);
        w.EndObject();
        w.EndObject();
        UpdateObject(destination, "build", w.str());
        std::lock_guard<std::mutex> lock(g_mutex);
        g_structures[to] = std::move(copy);
    }
    return a.str();
}

bool HasAccelerationStructures(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_structures.empty();
}

void ReadBackEarlierStructures(id encoder, uint64_t captureSerial) {
    std::vector<std::pair<uint64_t, StructureRecord>> pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_readBackSerial == captureSerial) return;
        g_readBackSerial = captureSerial;
        for (const auto &entry : g_structures) {
            // A structure built in this capture has the build itself, and needs no second read.
            if (entry.second.capture == captureSerial) continue;
            if (entry.second.inputs.empty()) continue;
            pending.push_back(entry);
        }
    }
    for (const auto &entry : pending) {
        const StructureRecord &record = entry.second;
        std::vector<uint64_t> captured;
        bool any = false;
        for (const StructureInput &in : record.inputs) {
            // Held weakly (see StructureInput): nil is a buffer the application has released, and
            // there is nothing left to read.
            id buffer = LiveObject(in.bufferId);
            const uint64_t captureId = buffer != nil
                ? QueueBufferCapture(encoder, buffer, in.offset, in.size, /*whole=*/true) : 0;
            captured.push_back(captureId);
            any = any || captureId != 0;
        }
        if (!any) continue;
        id structure = LiveObject(entry.first);
        if (structure == nil) continue;
        JsonWriter w;
        w.BeginObject();
        w.Key("captureInputs");
        w.BeginObject();
        w.Key("serial"); w.Uint(captureSerial);
        w.Key("method"); w.String(record.method);
        w.Key("mode"); w.String(record.mode.empty() ? "BUILD" : record.mode.c_str());
        w.Key("primitiveCount"); w.Uint(record.primitives);
        w.Key("descriptor"); w.Raw(record.descriptor);
        w.Key("inputs"); w.Raw(InputsJson(record.inputs, captured));
        w.EndObject();
        w.EndObject();
        UpdateObject(structure, "captureInputs", w.str());
    }
}

// ---------------------------------------------------------------------------------------------
// Function tables

void NoteFunctionTable(id table, id pipeline, const char *type, const char *cmd,
                       NSUInteger functionCount) {
    if (table == nil) return;
    Args a;
    a.u("functionCount", functionCount);
    if (pipeline != nil) a.ref("pipeline", pipeline, IdOf(pipeline) != 0 ? "MTLComputePipelineState" : "MTLComputePipelineState");
    WriteGpuIds(a, table);
    const uint64_t tableId = Track(table, type, cmd, pipeline, a.str());
    if (tableId == 0) return;
    TableRecord record;
    record.functionCount = functionCount;
    record.entries.resize(functionCount);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tables[tableId] = std::move(record);
    HookFunctionTableClass(table);
}

namespace {

/**
 * The whole table as an ObjectUpdate. Sent in full on every change rather than one entry at a
 * time: an update is keyed and last-write-wins (tracker.h), so a per-entry message would leave a
 * snapshot holding only the entry set last.
 */
void SendTable(id table, uint64_t tableId) {
    TableRecord record;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tables.find(tableId);
        if (it == g_tables.end()) return;
        record = it->second;
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("table");
    w.BeginObject();
    w.Key("functionCount"); w.Uint(record.functionCount);
    w.Key("entries");
    w.BeginArray();
    for (size_t i = 0; i < record.entries.size(); i++) {
        const TableEntry &e = record.entries[i];
        w.BeginObject();
        w.Key("index"); w.Uint(i);
        if (!e.set) {
            // Never set: a ray reaching this entry calls nothing, which is worth showing rather
            // than leaving the row out.
            w.Key("empty"); w.Boolean(true);
        } else if (!e.opaque.empty()) {
            w.Key("opaque"); w.String(e.opaque);
            w.Key("signature"); w.String(IntersectionFunctionSignatureFlags(e.signature));
        } else {
            w.Key("function"); w.String(e.function);
        }
        w.EndObject();
    }
    w.EndArray();
    w.Key("buffers");
    w.BeginArray();
    for (const auto &b : record.buffers) {
        w.BeginObject();
        w.Key("index"); w.Uint(b.first);
        w.Key("buffer"); w.Uint(b.second.first);
        w.Key("offset"); w.Uint(b.second.second);
        w.EndObject();
    }
    w.EndArray();
    w.Key("visibleFunctionTables");
    w.BeginArray();
    for (const auto &v : record.visibleTables) {
        w.BeginObject();
        w.Key("index"); w.Uint(v.first);
        w.Key("table"); w.Uint(v.second);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndObject();
    UpdateObject(table, "table", w.str());
}

/** Grows the entry list for an index past the count the descriptor asked for, which is a real bug. */
TableEntry *EntryAt(TableRecord &record, uint64_t index) {
    if (index >= 4096) return nullptr;   // a wild index, not a table this size
    if (index >= record.entries.size()) record.entries.resize(index + 1);
    return &record.entries[index];
}

}  // namespace

void NoteTableFunction(id table, NSUInteger index, id handle) {
    const uint64_t tableId = IdOf(table);
    if (tableId == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tables.find(tableId);
        if (it == g_tables.end()) return;
        TableEntry *entry = EntryAt(it->second, index);
        if (entry == nullptr) return;
        // An MTLFunctionHandle carries its own name, so an entry names the function it runs with no
        // side table to keep — where DXR needs a 32-byte identifier matched against the state
        // object's exports, and Vulkan a group handle matched against the pipeline's.
        id name = ObjectProp(handle, "name");
        entry->function = name != nil ? [(NSString *)name UTF8String] : "";
        entry->opaque.clear();
        entry->set = handle != nil;
    }
    SendTable(table, tableId);
}

void NoteTableOpaqueFunction(id table, NSUInteger index, NSUInteger signature, const char *what) {
    const uint64_t tableId = IdOf(table);
    if (tableId == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tables.find(tableId);
        if (it == g_tables.end()) return;
        TableEntry *entry = EntryAt(it->second, index);
        if (entry == nullptr) return;
        entry->function.clear();
        entry->opaque = what;
        entry->signature = signature;
        entry->set = true;
    }
    SendTable(table, tableId);
}

void NoteTableBuffer(id table, NSUInteger index, id buffer, NSUInteger offset) {
    const uint64_t tableId = IdOf(table);
    if (tableId == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tables.find(tableId);
        if (it == g_tables.end()) return;
        const uint64_t bufferId = IdOf(buffer);
        if (bufferId == 0) it->second.buffers.erase(index);
        else it->second.buffers[index] = {bufferId, offset};
    }
    SendTable(table, tableId);
}

void NoteTableVisibleTable(id table, NSUInteger index, id visible) {
    const uint64_t tableId = IdOf(table);
    if (tableId == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_tables.find(tableId);
        if (it == g_tables.end()) return;
        const uint64_t visibleId = IdOf(visible);
        if (visibleId == 0) it->second.visibleTables.erase(index);
        else it->second.visibleTables[index] = visibleId;
    }
    SendTable(table, tableId);
}

}  // namespace mtlinsp
