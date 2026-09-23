#include "mtl_raytracing.h"

#include <string>
#include <vector>

#include "mtl_reflect.h"
#include "mtl_source.h"

namespace mtlreplay
{
namespace
{

/** One buffer an input names, as the capture wrote it: `{buffer: <ref>, offset, size}`. */
struct Input
{
    id<MTLBuffer> buffer = nil;
    NSUInteger offset = 0;
    /** The reference was there but resolved to nothing: the build cannot be made. */
    bool missing = false;
};

Input ReadInput(const Decoder& d, const char* key)
{
    Input in;
    const Decoder nested = d.Nested(key);
    if (!nested.Json())
        return in;
    if (nested.ObjectId("buffer") == 0)
        return in;
    in.buffer = (id<MTLBuffer>)nested.Object("buffer");
    in.offset = (NSUInteger)nested.Uint("offset");
    in.missing = in.buffer == nil;
    return in;
}

/**
 * Sets a transform's matrix layout by key: the property and MTLMatrixLayout are macOS 15 SDK
 * additions, so naming them directly breaks a build against an older SDK. The capture reads it
 * the same way.
 */
void SetMatrixLayout(id descriptor, const char* key, const Decoder& d)
{
    NSString* name = @(key);
    NSString* setter = [NSString stringWithFormat:@"set%@%@:",
                                 [[name substringToIndex:1] uppercaseString],
                                 [name substringFromIndex:1]];
    if (![descriptor respondsToSelector:NSSelectorFromString(setter)])
        return;
    [descriptor setValue:@(d.Enum(key, MTL_TABLE(MTLMatrixLayout))) forKey:name];
}

/** The exported expression for an input's buffer, and its offset. */
std::string InputName(Source& w, const Decoder& d, const char* key, NSUInteger& offset)
{
    const Decoder nested = d.Nested(key);
    offset = nested.Json() ? (NSUInteger)nested.Uint("offset") : 0;
    if (!nested.Json())
        return "nil";
    id buffer = nested.Object("buffer");
    const std::string name = buffer != nil && w.objectName ? w.objectName(buffer) : std::string();
    return name.empty() ? "nil" : name;
}

/**
 * A geometry's attribute format, under either name the capture may have written it.
 *
 * `vertexFormat` on a triangle geometry and `controlPointFormat` on a curve one are
 * `MTLAttributeFormat`s, but the capture writes them through the namer it has for `MTLVertexFormat`
 * — the two enums are the same 42 values under two names (src/metal/src/formats.h), so the value is
 * right and only the spelling differs. Trying both tables is cheaper than teaching the capture a
 * second namer for the same numbers.
 */
int64_t AttributeFormat(const Decoder& d, const char* key, int64_t fallback)
{
    const int64_t attribute = d.Enum(key, MTL_TABLE(MTLAttributeFormat), -1);
    if (attribute >= 0)
        return attribute;
    const int64_t vertex = d.Enum(key, MTL_TABLE(MTLVertexFormat), -1);
    return vertex >= 0 ? vertex : fallback;
}

/** Whether the capture's geometry JSON says this kind is a motion one. */
bool IsMotion(const std::string& kind)
{
    return kind.compare(0, 6, "motion") == 0;
}

void FillCommonGeometry(MTLAccelerationStructureGeometryDescriptor* g, const Decoder& d)
{
    g.intersectionFunctionTableOffset = (NSUInteger)d.Uint("intersectionFunctionTableOffset");
    g.opaque = d.Bool("opaque");
    g.allowDuplicateIntersectionFunctionInvocation = d.Bool("allowDuplicateIntersectionFunctionInvocation");
    if (@available(macOS 12.0, *))
    {
        if (NSString* label = d.NSStr("label"))
            g.label = label;
    }
    // Per-primitive data an intersection function reads beside the primitive. Only set when the
    // capture recorded a buffer: the stride and element size mean nothing without one, and Metal
    // validates them against it.
    if (@available(macOS 13.0, *))
    {
        const Input data = ReadInput(d, "primitiveDataBuffer");
        if (data.buffer != nil)
        {
            g.primitiveDataBuffer = data.buffer;
            g.primitiveDataBufferOffset = data.offset;
            g.primitiveDataStride = (NSUInteger)d.Uint("primitiveDataStride");
            g.primitiveDataElementSize = (NSUInteger)d.Uint("primitiveDataElementSize");
        }
    }
}

MTLAccelerationStructureGeometryDescriptor* TriangleGeometry(const Decoder& d, bool motion,
    std::string& error)
{
    const MTLIndexType indexType = (MTLIndexType)d.Enum("indexType", MTL_TABLE(MTLIndexType));
    const Input index = ReadInput(d, "indexBuffer");
    const Input transform = ReadInput(d, "transformationMatrixBuffer");
    if (motion)
    {
        if (@available(macOS 13.0, *))
        {
            MTLAccelerationStructureMotionTriangleGeometryDescriptor* g =
                [MTLAccelerationStructureMotionTriangleGeometryDescriptor descriptor];
            NSMutableArray* keyframes = [NSMutableArray array];
            const JValue* buffers = d.Get("vertexBuffers");
            for (uint32_t i = 0; buffers && i < buffers->count; ++i)
            {
                const Input in = ReadInput(d.At(&buffers->items[i]), "buffer");
                if (in.buffer == nil)
                {
                    error = "a motion triangle keyframe's vertex buffer is not in the replay";
                    return nil;
                }
                MTLMotionKeyframeData* frame = [MTLMotionKeyframeData data];
                frame.buffer = in.buffer;
                frame.offset = in.offset;
                [keyframes addObject:frame];
            }
            g.vertexBuffers = keyframes;
            g.triangleCount = (NSUInteger)d.Uint("triangleCount");
            g.vertexStride = (NSUInteger)d.Uint("vertexStride");
            g.indexType = indexType;
            if (index.buffer != nil)
            {
                g.indexBuffer = index.buffer;
                g.indexBufferOffset = index.offset;
            }
            if (@available(macOS 14.0, *))
            {
                g.vertexFormat = (MTLAttributeFormat)AttributeFormat(d, "vertexFormat", MTLAttributeFormatFloat3);
            }
            if (@available(macOS 15.0, *))
            {
                if (transform.buffer != nil)
                {
                    g.transformationMatrixBuffer = transform.buffer;
                    g.transformationMatrixBufferOffset = transform.offset;
                    SetMatrixLayout(g, "transformationMatrixLayout", d);
                }
            }
            FillCommonGeometry(g, d);
            return g;
        }
        error = "motion triangle geometry needs macOS 13";
        return nil;
    }

    MTLAccelerationStructureTriangleGeometryDescriptor* g =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    const Input vertices = ReadInput(d, "vertexBuffer");
    if (vertices.buffer == nil)
    {
        error = "the triangle geometry's vertex buffer is not in the replay";
        return nil;
    }
    g.vertexBuffer = vertices.buffer;
    g.vertexBufferOffset = vertices.offset;
    g.triangleCount = (NSUInteger)d.Uint("triangleCount");
    g.vertexStride = (NSUInteger)d.Uint("vertexStride");
    g.indexType = indexType;
    if (index.buffer != nil)
    {
        g.indexBuffer = index.buffer;
        g.indexBufferOffset = index.offset;
    }
    // vertexFormat arrived in macOS 13; before it a vertex was three floats by definition, which is
    // what the capture writes for an older one.
    if (@available(macOS 13.0, *))
    {
        g.vertexFormat = (MTLAttributeFormat)AttributeFormat(d, "vertexFormat", MTLAttributeFormatFloat3);
    }
    if (@available(macOS 15.0, *))
    {
        if (transform.buffer != nil)
        {
            g.transformationMatrixBuffer = transform.buffer;
            g.transformationMatrixBufferOffset = transform.offset;
            SetMatrixLayout(g, "transformationMatrixLayout", d);
        }
    }
    FillCommonGeometry(g, d);
    return g;
}

MTLAccelerationStructureGeometryDescriptor* BoundingBoxGeometry(const Decoder& d, bool motion,
    std::string& error)
{
    if (motion)
    {
        if (@available(macOS 13.0, *))
        {
            MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor* g =
                [MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor descriptor];
            NSMutableArray* keyframes = [NSMutableArray array];
            const JValue* buffers = d.Get("boundingBoxBuffers");
            for (uint32_t i = 0; buffers && i < buffers->count; ++i)
            {
                const Input in = ReadInput(d.At(&buffers->items[i]), "buffer");
                if (in.buffer == nil)
                {
                    error = "a motion bounding box keyframe's buffer is not in the replay";
                    return nil;
                }
                MTLMotionKeyframeData* frame = [MTLMotionKeyframeData data];
                frame.buffer = in.buffer;
                frame.offset = in.offset;
                [keyframes addObject:frame];
            }
            g.boundingBoxBuffers = keyframes;
            g.boundingBoxCount = (NSUInteger)d.Uint("boundingBoxCount");
            g.boundingBoxStride = (NSUInteger)d.Uint("boundingBoxStride");
            FillCommonGeometry(g, d);
            return g;
        }
        error = "motion bounding box geometry needs macOS 13";
        return nil;
    }

    MTLAccelerationStructureBoundingBoxGeometryDescriptor* g =
        [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
    const Input boxes = ReadInput(d, "boundingBoxBuffer");
    if (boxes.buffer == nil)
    {
        error = "the bounding box geometry's buffer is not in the replay";
        return nil;
    }
    g.boundingBoxBuffer = boxes.buffer;
    g.boundingBoxBufferOffset = boxes.offset;
    g.boundingBoxCount = (NSUInteger)d.Uint("boundingBoxCount");
    const NSUInteger stride = (NSUInteger)d.Uint("boundingBoxStride");
    if (stride != 0)
        g.boundingBoxStride = stride;
    FillCommonGeometry(g, d);
    return g;
}

MTLAccelerationStructureGeometryDescriptor* CurveGeometry(const Decoder& d, bool motion,
    std::string& error)
{
    if (@available(macOS 14.0, *))
    {
        const NSUInteger segmentCount = (NSUInteger)d.Uint("segmentCount");
        const NSUInteger segmentControlPointCount = (NSUInteger)d.Uint("segmentControlPointCount");
        const NSUInteger controlPointCount = (NSUInteger)d.Uint("controlPointCount");
        const NSUInteger controlPointStride = (NSUInteger)d.Uint("controlPointStride");
        const MTLAttributeFormat controlPointFormat =
            (MTLAttributeFormat)AttributeFormat(d, "controlPointFormat", MTLAttributeFormatFloat3);
        const MTLCurveType curveType = (MTLCurveType)d.Enum("curveType", MTL_TABLE(MTLCurveType));
        const MTLCurveBasis curveBasis = (MTLCurveBasis)d.Enum("curveBasis", MTL_TABLE(MTLCurveBasis));
        const MTLCurveEndCaps endCaps = (MTLCurveEndCaps)d.Enum("curveEndCaps", MTL_TABLE(MTLCurveEndCaps));
        const MTLIndexType indexType = (MTLIndexType)d.Enum("indexType", MTL_TABLE(MTLIndexType));
        const Input index = ReadInput(d, "indexBuffer");
        if (motion)
        {
            MTLAccelerationStructureMotionCurveGeometryDescriptor* g =
                [MTLAccelerationStructureMotionCurveGeometryDescriptor descriptor];
            auto keyframes = [&](const char* key, NSMutableArray* into) -> bool {
                const JValue* buffers = d.Get(key);
                for (uint32_t i = 0; buffers && i < buffers->count; ++i)
                {
                    const Input in = ReadInput(d.At(&buffers->items[i]), "buffer");
                    if (in.buffer == nil)
                        return false;
                    MTLMotionKeyframeData* frame = [MTLMotionKeyframeData data];
                    frame.buffer = in.buffer;
                    frame.offset = in.offset;
                    [into addObject:frame];
                }
                return true;
            };
            NSMutableArray* controlPoints = [NSMutableArray array];
            NSMutableArray* radii = [NSMutableArray array];
            if (!keyframes("controlPointBuffers", controlPoints) || !keyframes("radiusBuffers", radii))
            {
                error = "a motion curve keyframe's buffer is not in the replay";
                return nil;
            }
            g.controlPointBuffers = controlPoints;
            g.radiusBuffers = radii;
            g.segmentCount = segmentCount;
            g.segmentControlPointCount = segmentControlPointCount;
            g.controlPointCount = controlPointCount;
            g.controlPointStride = controlPointStride;
            g.controlPointFormat = controlPointFormat;
            g.curveType = curveType;
            g.curveBasis = curveBasis;
            g.curveEndCaps = endCaps;
            g.indexType = indexType;
            if (index.buffer != nil)
            {
                g.indexBuffer = index.buffer;
                g.indexBufferOffset = index.offset;
            }
            FillCommonGeometry(g, d);
            return g;
        }
        MTLAccelerationStructureCurveGeometryDescriptor* g =
            [MTLAccelerationStructureCurveGeometryDescriptor descriptor];
        const Input controlPoints = ReadInput(d, "controlPointBuffer");
        const Input radius = ReadInput(d, "radiusBuffer");
        if (controlPoints.buffer == nil)
        {
            error = "the curve geometry's control point buffer is not in the replay";
            return nil;
        }
        g.controlPointBuffer = controlPoints.buffer;
        g.controlPointBufferOffset = controlPoints.offset;
        if (radius.buffer != nil)
        {
            g.radiusBuffer = radius.buffer;
            g.radiusBufferOffset = radius.offset;
        }
        g.segmentCount = segmentCount;
        g.segmentControlPointCount = segmentControlPointCount;
        g.controlPointCount = controlPointCount;
        g.controlPointStride = controlPointStride;
        g.controlPointFormat = controlPointFormat;
        g.curveType = curveType;
        g.curveBasis = curveBasis;
        g.curveEndCaps = endCaps;
        g.indexType = indexType;
        if (index.buffer != nil)
        {
            g.indexBuffer = index.buffer;
            g.indexBufferOffset = index.offset;
        }
        FillCommonGeometry(g, d);
        return g;
    }
    error = "curve geometry needs macOS 14";
    return nil;
}

MTLAccelerationStructureGeometryDescriptor* Geometry(const Decoder& d, std::string& error)
{
    const std::string kind = d.Str("kind");
    const bool motion = IsMotion(kind);
    if (kind == "triangles" || kind == "motionTriangles")
        return TriangleGeometry(d, motion, error);
    if (kind == "boundingBoxes" || kind == "motionBoundingBoxes")
        return BoundingBoxGeometry(d, motion, error);
    if (kind == "curves" || kind == "motionCurves")
        return CurveGeometry(d, motion, error);
    error = "geometry of kind '" + kind + "' is not rebuilt";
    return nil;
}

MTLAccelerationStructureDescriptor* PrimitiveDescriptor(const Decoder& d, std::string& error)
{
    NSMutableArray* geometries = [NSMutableArray array];
    const JValue* list = d.Get("geometries");
    for (uint32_t i = 0; list && i < list->count; ++i)
    {
        MTLAccelerationStructureGeometryDescriptor* g = Geometry(d.At(&list->items[i]), error);
        if (g == nil)
            return nil;
        [geometries addObject:g];
    }

    // A keyframe count above one makes it a motion structure, which is a different descriptor class
    // holding the same geometries. The capture only writes the count when it is above one, so its
    // absence is "not a motion structure" rather than "unknown" (src/metal/src/raytracing.mm).
    const NSUInteger keyframes = (NSUInteger)d.Uint("motionKeyframeCount");
    if (keyframes > 1)
    {
        if (@available(macOS 13.0, *))
        {
            MTLPrimitiveAccelerationStructureDescriptor* motion =
                [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            motion.geometryDescriptors = geometries;
            motion.motionKeyframeCount = keyframes;
            motion.motionStartBorderMode =
                (MTLMotionBorderMode)d.Enum("motionStartBorderMode", MTL_TABLE(MTLMotionBorderMode));
            motion.motionEndBorderMode =
                (MTLMotionBorderMode)d.Enum("motionEndBorderMode", MTL_TABLE(MTLMotionBorderMode));
            motion.motionStartTime = (float)d.Double("motionStartTime");
            motion.motionEndTime = (float)d.Double("motionEndTime", 1.0);
            return motion;
        }
        error = "a motion acceleration structure needs macOS 13";
        return nil;
    }
    MTLPrimitiveAccelerationStructureDescriptor* primitive =
        [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    primitive.geometryDescriptors = geometries;
    return primitive;
}

MTLAccelerationStructureDescriptor* InstanceDescriptor(const Decoder& d, std::string& error)
{
    const Input instances = ReadInput(d, "instanceDescriptorBuffer");
    if (instances.buffer == nil)
    {
        error = "the top level's instance descriptor buffer is not in the replay";
        return nil;
    }
    // Held as a plain integer: the enum *type* is macOS 12, so naming it outside a guard is a
    // warning even where the value is only used inside one.
    const int64_t type = d.Enum("instanceDescriptorType",
        MTL_TABLE(MTLAccelerationStructureInstanceDescriptorType));
    const NSUInteger stride = (NSUInteger)d.Uint("instanceDescriptorStride");

    // The indirect form takes its instance count from a buffer the GPU wrote, so the count in the
    // capture is only the maximum a build may read.
    if (d.Bool("indirect"))
    {
        if (@available(macOS 14.0, *))
        {
            MTLIndirectInstanceAccelerationStructureDescriptor* indirect =
                [MTLIndirectInstanceAccelerationStructureDescriptor descriptor];
            const Input count = ReadInput(d, "instanceCountBuffer");
            if (count.buffer == nil)
            {
                error = "the indirect top level's instance count buffer is not in the replay";
                return nil;
            }
            indirect.instanceDescriptorBuffer = instances.buffer;
            indirect.instanceDescriptorBufferOffset = instances.offset;
            indirect.instanceCountBuffer = count.buffer;
            indirect.instanceCountBufferOffset = count.offset;
            indirect.maxInstanceCount = (NSUInteger)d.Uint("maxInstanceCount");
            indirect.instanceDescriptorType = (MTLAccelerationStructureInstanceDescriptorType)type;
            if (stride != 0)
                indirect.instanceDescriptorStride = stride;
            const Input transforms = ReadInput(d, "motionTransformBuffer");
            if (transforms.buffer != nil)
            {
                indirect.motionTransformBuffer = transforms.buffer;
                indirect.motionTransformBufferOffset = transforms.offset;
            }
            return indirect;
        }
        error = "an indirect instance acceleration structure needs macOS 14";
        return nil;
    }

    MTLInstanceAccelerationStructureDescriptor* top =
        [MTLInstanceAccelerationStructureDescriptor descriptor];
    // The bottom levels, in the order an instance's `accelerationStructureIndex` indexes them: this
    // is the link the other two APIs have to reconstruct from device addresses and Metal states
    // outright, and the order is the whole of its meaning — one missing entry shifts every instance
    // after it onto the wrong mesh, so a hole refuses rather than builds something wrong.
    NSMutableArray* instanced = [NSMutableArray array];
    const JValue* list = d.Get("instancedAccelerationStructures");
    for (uint32_t i = 0; list && i < list->count; ++i)
    {
        const uint64_t bottomId = IdOf(&list->items[i]);
        id structure = d.Resolve(bottomId);
        if (structure == nil)
        {
            error = "bottom level " + std::to_string(bottomId) + " of the top level is not in the replay, so its "
                                                                 "instances cannot be indexed";
            return nil;
        }
        [instanced addObject:structure];
    }
    top.instancedAccelerationStructures = instanced;
    top.instanceDescriptorBuffer = instances.buffer;
    top.instanceDescriptorBufferOffset = instances.offset;
    top.instanceCount = (NSUInteger)d.Uint("instanceCount");
    if (stride != 0)
        top.instanceDescriptorStride = stride;
    if (@available(macOS 12.0, *))
    {
        top.instanceDescriptorType = (MTLAccelerationStructureInstanceDescriptorType)type;
        const Input transforms = ReadInput(d, "motionTransformBuffer");
        if (transforms.buffer != nil)
        {
            top.motionTransformBuffer = transforms.buffer;
            top.motionTransformBufferOffset = transforms.offset;
            top.motionTransformCount = (NSUInteger)d.Uint("motionTransformCount");
        }
    }
    SetMatrixLayout(top, "instanceTransformationMatrixLayout", d);
    return top;
}

}  // namespace

MTLAccelerationStructureDescriptor* BuildDescriptor(const Decoder& d, std::string& error)
{
    if (!d.Json())
    {
        error = "the build recorded no descriptor";
        return nil;
    }
    const std::string kind = d.Str("kind");
    MTLAccelerationStructureDescriptor* descriptor = nil;
    if (kind == "instance")
        descriptor = InstanceDescriptor(d, error);
    else if (kind == "primitive")
        descriptor = PrimitiveDescriptor(d, error);
    else
        error = "a descriptor of kind '" + kind + "' is not rebuilt";
    if (descriptor == nil)
        return nil;
    descriptor.usage = (MTLAccelerationStructureUsage)
                           d.Flags("usage", MTL_TABLE(MTLAccelerationStructureUsage));
    return descriptor;
}

id LinkedFunctions(const Decoder& d, std::vector<std::pair<std::string, id>>& out)
{
    const Decoder linked = d.Nested("linkedFunctions");
    if (!linked.Json())
        return nil;
    if (@available(macOS 11.0, *))
    {
    }
    else
    {
        return nil;
    }
    MTLLinkedFunctions* result = [MTLLinkedFunctions linkedFunctions];
    bool any = false;
    // The three lists differ in how each function is compiled, not in what it is, so each is read
    // into the property of the same name and every one of them contributes a name to `out`.
    auto read = [&](const char* key, void (^assign)(NSArray*)) {
        const JValue* list = linked.Get(key);
        if (!list || !list->IsArray() || list->count == 0)
            return;
        NSMutableArray* functions = [NSMutableArray array];
        for (uint32_t i = 0; i < list->count; ++i)
        {
            const Decoder entry = linked.At(&list->items[i]);
            id function = entry.Object("function");
            const std::string name = entry.Str("name");
            if (function == nil)
                continue;
            [functions addObject:function];
            if (!name.empty())
                out.push_back({name, function});
        }
        if (functions.count == 0)
            return;
        assign(functions);
        any = true;
    };
    read("functions", ^(NSArray* f) {
        result.functions = f;
    });
    read("binaryFunctions", ^(NSArray* f) {
        result.binaryFunctions = f;
    });
    if (@available(macOS 12.0, *))
    {
        read("privateFunctions", ^(NSArray* f) {
            result.privateFunctions = f;
        });
    }
    return any ? result : nil;
}

std::string WriteLinkedFunctionsSource(Source& w, const Decoder& d)
{
    const Decoder linked = d.Nested("linkedFunctions");
    if (!linked.Json())
        return "";
    std::vector<std::pair<std::string, std::string>> lists;  // property -> array local
    auto read = [&](const char* key) {
        const JValue* list = linked.Get(key);
        if (!list || !list->IsArray() || list->count == 0)
            return;
        std::vector<std::string> names;
        for (uint32_t i = 0; i < list->count; ++i)
        {
            id function = linked.At(&list->items[i]).Object("function");
            const std::string name = function != nil && w.objectName ? w.objectName(function) : std::string();
            if (!name.empty())
                names.push_back(name);
        }
        if (names.empty())
            return;
        const std::string local = w.Local(key);
        std::string items;
        for (size_t i = 0; i < names.size(); ++i)
            items += (i ? ", " : "") + names[i];
        w.Line("NSArray *" + local + " = @[" + items + "];");
        lists.push_back({key, local});
    };
    read("functions");
    read("binaryFunctions");
    read("privateFunctions");
    if (lists.empty())
        return "";
    const std::string var = w.Local("linked");
    w.Line("MTLLinkedFunctions *" + var + " = [MTLLinkedFunctions linkedFunctions];");
    for (const auto& [property, local] : lists)
        w.Line(var + "." + property + " = " + local + ";");
    return var;
}

// ---------------------------------------------------------------------------------------------
// Export to C++
//
// The same walk, writing statements instead of setting properties. Deliberately a second pass over
// the JSON rather than a reflection of the objects built above: an exported project has to compile
// against whatever SDK the person building it has, and what is written here is the plain form of
// each property — `@available` in the replay decides what *this* machine can build, which is a
// different question from what the exported source should say.

namespace
{

/** `descriptor.geometryDescriptors`-worth of statements for one geometry, into local `g`. */
bool WriteGeometrySource(Source& w, const std::string& var, const Decoder& d, std::string& error)
{
    const std::string kind = d.Str("kind");
    NSUInteger offset = 0;
    auto set = [&](const std::string& property, const std::string& value) {
        w.Line(var + "." + property + " = " + value + ";");
    };
    auto uint = [&](const char* key) { return std::to_string(d.Uint(key)); };

    if (kind == "triangles")
    {
        w.Line("MTLAccelerationStructureTriangleGeometryDescriptor *" + var +
            " = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];");
        const std::string vertices = InputName(w, d, "vertexBuffer", offset);
        if (vertices == "nil")
        {
            error = "the triangle geometry's vertex buffer has no name in the export";
            return false;
        }
        set("vertexBuffer", vertices);
        set("vertexBufferOffset", std::to_string(offset));
        set("triangleCount", uint("triangleCount"));
        set("vertexStride", uint("vertexStride"));
        set("indexType", Source::Enum(MTL_TABLE(MTLIndexType), d.Enum("indexType", MTL_TABLE(MTLIndexType))));
        const std::string index = InputName(w, d, "indexBuffer", offset);
        if (index != "nil")
        {
            set("indexBuffer", index);
            set("indexBufferOffset", std::to_string(offset));
        }
    }
    else if (kind == "boundingBoxes")
    {
        w.Line("MTLAccelerationStructureBoundingBoxGeometryDescriptor *" + var +
            " = [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];");
        const std::string boxes = InputName(w, d, "boundingBoxBuffer", offset);
        if (boxes == "nil")
        {
            error = "the bounding box geometry's buffer has no name in the export";
            return false;
        }
        set("boundingBoxBuffer", boxes);
        set("boundingBoxBufferOffset", std::to_string(offset));
        set("boundingBoxCount", uint("boundingBoxCount"));
        if (d.Uint("boundingBoxStride") != 0)
            set("boundingBoxStride", uint("boundingBoxStride"));
    }
    else
    {
        // Curves and the motion kinds: the exported project would need the newer SDK and a keyframe
        // array per buffer, which is more transcription than a repro case has ever needed. The
        // replay still builds them; only the export leaves them out, and says so.
        error = "geometry of kind '" + kind + "' is not written to the export";
        return false;
    }
    set("intersectionFunctionTableOffset", uint("intersectionFunctionTableOffset"));
    set("opaque", d.Bool("opaque") ? "YES" : "NO");
    set("allowDuplicateIntersectionFunctionInvocation",
        d.Bool("allowDuplicateIntersectionFunctionInvocation") ? "YES" : "NO");
    return true;
}

}  // namespace

bool WriteDescriptorSource(Source& w, const std::string& var, const Decoder& d, std::string& error)
{
    if (!d.Json())
    {
        error = "the build recorded no descriptor";
        return false;
    }
    const std::string kind = d.Str("kind");
    const std::string usage = Source::Flags(MTL_TABLE(MTLAccelerationStructureUsage),
        d.Flags("usage", MTL_TABLE(MTLAccelerationStructureUsage)));
    if (kind == "primitive")
    {
        if (d.Uint("motionKeyframeCount") > 1)
        {
            error = "a motion acceleration structure is not written to the export";
            return false;
        }
        const std::string local = w.Local("primitives");
        w.Line("MTLPrimitiveAccelerationStructureDescriptor *" + local +
            " = [MTLPrimitiveAccelerationStructureDescriptor descriptor];");
        const std::string array = w.Local("geometries");
        w.Line("NSMutableArray *" + array + " = [NSMutableArray array];");
        const JValue* list = d.Get("geometries");
        for (uint32_t i = 0; list && i < list->count; ++i)
        {
            const std::string g = w.Local("geometry");
            if (!WriteGeometrySource(w, g, d.At(&list->items[i]), error))
                return false;
            w.Line("[" + array + " addObject:" + g + "];");
        }
        w.Line(local + ".geometryDescriptors = " + array + ";");
        w.Line(local + ".usage = " + usage + ";");
        w.Line(var + " = " + local + ";");
        return true;
    }
    if (kind != "instance")
    {
        error = "a descriptor of kind '" + kind + "' is not written to the export";
        return false;
    }
    if (d.Bool("indirect"))
    {
        error = "an indirect instance acceleration structure is not written to the export";
        return false;
    }
    const std::string local = w.Local("instances");
    w.Line("MTLInstanceAccelerationStructureDescriptor *" + local +
        " = [MTLInstanceAccelerationStructureDescriptor descriptor];");
    const std::string array = w.Local("instanced");
    w.Line("NSMutableArray *" + array + " = [NSMutableArray array];");
    const JValue* list = d.Get("instancedAccelerationStructures");
    for (uint32_t i = 0; list && i < list->count; ++i)
    {
        id structure = d.Resolve(IdOf(&list->items[i]));
        const std::string name = structure != nil && w.objectName ? w.objectName(structure) : std::string();
        if (name.empty())
        {
            error = "a bottom level of the top level has no name in the export";
            return false;
        }
        w.Line("[" + array + " addObject:" + name + "];");
    }
    NSUInteger offset = 0;
    const std::string buffer = InputName(w, d, "instanceDescriptorBuffer", offset);
    if (buffer == "nil")
    {
        error = "the top level's instance descriptor buffer has no name in the export";
        return false;
    }
    w.Line(local + ".instancedAccelerationStructures = " + array + ";");
    w.Line(local + ".instanceDescriptorBuffer = " + buffer + ";");
    w.Line(local + ".instanceDescriptorBufferOffset = " + std::to_string(offset) + ";");
    w.Line(local + ".instanceCount = " + std::to_string(d.Uint("instanceCount")) + ";");
    if (d.Uint("instanceDescriptorStride") != 0)
    {
        w.Line(local + ".instanceDescriptorStride = " + std::to_string(d.Uint("instanceDescriptorStride")) + ";");
    }
    w.Line(local + ".instanceDescriptorType = " +
        Source::Enum(MTL_TABLE(MTLAccelerationStructureInstanceDescriptorType),
            d.Enum("instanceDescriptorType",
                MTL_TABLE(MTLAccelerationStructureInstanceDescriptorType))) +
        ";");
    w.Line(local + ".usage = " + usage + ";");
    w.Line(var + " = " + local + ";");
    return true;
}

} // namespace mtlreplay
