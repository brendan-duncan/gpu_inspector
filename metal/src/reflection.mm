#include "reflection.h"

#include "formats.h"
#include "json_writer.h"

#import <objc/message.h>

#include <algorithm>
#include <string>

namespace mtlinsp {
namespace {

// Two generations of the reflection API describe the same thing: MTLArgument (deprecated in
// macOS 13) and the MTLBinding protocols that replaced it. Their property names agree on
// everything used here — name, index, access, bufferDataType, bufferStructType and so on — and
// their type and access enumerations share values, so the bindings are read through `id` and
// the selectors, and the one difference (`isActive` against `used`) is checked for.

enum BindingType : NSUInteger {
    kBindingBuffer = 0,
    kBindingThreadgroupMemory = 1,
    kBindingTexture = 2,
    kBindingSampler = 3,
    kBindingImageblockData = 16,
    kBindingImageblock = 17,
    kBindingVisibleFunctionTable = 24,
    kBindingPrimitiveAccelerationStructure = 25,
    kBindingInstanceAccelerationStructure = 26,
    kBindingIntersectionFunctionTable = 27,
    kBindingObjectPayload = 34,
};

NSUInteger UIntProperty(id object, SEL selector) {
    if (object == nil || ![object respondsToSelector:selector]) return 0;
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(object, selector);
}

BOOL BoolProperty(id object, SEL selector) {
    if (object == nil || ![object respondsToSelector:selector]) return NO;
    return ((BOOL (*)(id, SEL))objc_msgSend)(object, selector);
}

id ObjectProperty(id object, SEL selector) {
    if (object == nil || ![object respondsToSelector:selector]) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(object, selector);
}

struct Scalar {
    const char *base;
    uint32_t width;
};

/** The scalar family a data type belongs to and its component count, or count 0. */
bool ScalarOf(MTLDataType type, Scalar *scalar, uint32_t *count) {
    struct Family {
        MTLDataType first;
        Scalar scalar;
    };
    static const Family kFamilies[] = {
        {MTLDataTypeFloat, {"float", 32}},  {MTLDataTypeHalf, {"float", 16}},
        {MTLDataTypeInt, {"int", 32}},      {MTLDataTypeUInt, {"uint", 32}},
        {MTLDataTypeShort, {"int", 16}},    {MTLDataTypeUShort, {"uint", 16}},
        {MTLDataTypeChar, {"int", 8}},      {MTLDataTypeUChar, {"uint", 8}},
        {MTLDataTypeBool, {"bool", 8}},     {(MTLDataType)81, {"int", 64}},   // Long
        {(MTLDataType)85, {"uint", 64}},                                       // ULong
        {(MTLDataType)121, {"float", 16}},                                     // BFloat
    };
    for (const Family &f : kFamilies) {
        if (type >= f.first && type < (MTLDataType)((NSUInteger)f.first + 4)) {
            *scalar = f.scalar;
            *count = (uint32_t)((NSUInteger)type - (NSUInteger)f.first) + 1;
            return true;
        }
    }
    return false;
}

/** Float2x2..Float4x4 and Half2x2..Half4x4: columns then rows, three rows per column count. */
bool MatrixOf(MTLDataType type, Scalar *scalar, uint32_t *columns, uint32_t *rows) {
    NSUInteger index = 0;
    if (type >= MTLDataTypeFloat2x2 && type <= MTLDataTypeFloat4x4) {
        *scalar = {"float", 32};
        index = (NSUInteger)type - (NSUInteger)MTLDataTypeFloat2x2;
    } else if (type >= MTLDataTypeHalf2x2 && type <= MTLDataTypeHalf4x4) {
        *scalar = {"float", 16};
        index = (NSUInteger)type - (NSUInteger)MTLDataTypeHalf2x2;
    } else {
        return false;
    }
    *columns = 2 + (uint32_t)(index / 3);
    *rows = 2 + (uint32_t)(index % 3);
    return true;
}

void WriteScalar(vkinsp::JsonWriter &w, const Scalar &s) {
    w.BeginObject();
    w.Key("kind"); w.String("scalar");
    w.Key("base"); w.String(s.base);
    w.Key("width"); w.Uint(s.width);
    w.Key("size"); w.Uint(s.width / 8);
    w.EndObject();
}

const char *OpaqueName(MTLDataType type) {
    switch ((NSUInteger)type) {
        case MTLDataTypeTexture: return "texture";
        case MTLDataTypeSampler: return "sampler";
        case MTLDataTypePointer: return "pointer";
        case 78: return "render_pipeline_state";
        case 79: return "compute_pipeline_state";
        case 80: return "indirect_command_buffer";
        case 115: return "visible_function_table";
        case 116: return "intersection_function_table";
        case 117: return "primitive_acceleration_structure";
        case 118: return "instance_acceleration_structure";
        default: return "opaque";
    }
}

uint64_t WriteType(vkinsp::JsonWriter &w, MTLDataType type, MTLStructType *structType,
                   MTLArrayType *arrayType, MTLPointerType *pointerType, uint64_t knownSize);

/** A struct, member by member. Its size is the end of its last member unless the caller knows it. */
uint64_t WriteStruct(vkinsp::JsonWriter &w, MTLStructType *s, const char *name, uint64_t knownSize) {
    uint64_t end = 0;
    w.BeginObject();
    w.Key("kind"); w.String("struct");
    w.Key("name"); w.String(name != nullptr ? name : "struct");
    w.Key("members"); w.BeginArray();
    for (MTLStructMember *m in s.members) {
        w.BeginObject();
        w.Key("name"); w.String(m.name == nil ? "" : m.name.UTF8String);
        w.Key("offset"); w.Uint(m.offset);
        w.Key("type");
        const uint64_t size = WriteType(w, m.dataType, m.structType, m.arrayType, m.pointerType, 0);
        end = std::max<uint64_t>(end, m.offset + size);
        w.EndObject();
    }
    w.EndArray();
    const uint64_t size = knownSize != 0 ? knownSize : end;
    w.Key("size"); w.Uint(size);
    w.EndObject();
    return size;
}

/** Writes one ReflType and returns its size in bytes (0 for opaque or runtime-sized). */
uint64_t WriteType(vkinsp::JsonWriter &w, MTLDataType type, MTLStructType *structType,
                   MTLArrayType *arrayType, MTLPointerType *pointerType, uint64_t knownSize) {
    Scalar scalar;
    uint32_t count = 0, columns = 0, rows = 0;
    if (type == MTLDataTypeStruct && structType != nil) {
        return WriteStruct(w, structType, nullptr, knownSize);
    }
    if (type == MTLDataTypeArray && arrayType != nil) {
        w.BeginObject();
        w.Key("kind"); w.String("array");
        w.Key("element");
        const uint64_t elementSize = WriteType(w, arrayType.elementType, arrayType.elementStructType,
                                               arrayType.elementArrayType, arrayType.elementPointerType, 0);
        const uint64_t stride = arrayType.stride != 0 ? arrayType.stride : elementSize;
        w.Key("count"); w.Uint(arrayType.arrayLength);
        w.Key("stride"); w.Uint(stride);
        w.Key("size"); w.Uint(stride * arrayType.arrayLength);
        w.EndObject();
        return stride * arrayType.arrayLength;
    }
    if (type == MTLDataTypePointer && pointerType != nil) {
        // A pointer inside an argument buffer: eight bytes holding a buffer's GPU address, which
        // the UI matches to the buffer whose range holds it (metal/argument_buffer.ts). Named
        // by what it points at; a nested argument buffer's struct comes along as `element`.
        w.BeginObject();
        w.Key("kind"); w.String("opaque");
        std::string name = "device ";
        Scalar s;
        uint32_t n = 0;
        if (pointerType.elementType == MTLDataTypeStruct) name += "struct";
        else if (ScalarOf(pointerType.elementType, &s, &n)) name += std::string(s.base) + (n > 1 ? std::to_string(n) : "");
        else name += "T";
        name += " *";
        w.Key("name"); w.String(name.c_str());
        w.Key("metal"); w.String("pointer");
        w.Key("size"); w.Uint(8);
        if (pointerType.elementIsArgumentBuffer) { w.Key("argumentBuffer"); w.Boolean(true); }
        if (pointerType.elementStructType != nil) {
            w.Key("element");
            WriteStruct(w, pointerType.elementStructType, nullptr, pointerType.dataSize);
        }
        w.EndObject();
        return 8;
    }
    if (MatrixOf(type, &scalar, &columns, &rows)) {
        // Metal matrices are column-major and each column is padded like the vector it is: a
        // three-row column takes four elements.
        const uint32_t element = scalar.width / 8;
        const uint32_t stride = (rows == 3 ? 4 : rows) * element;
        w.BeginObject();
        w.Key("kind"); w.String("matrix");
        w.Key("element"); WriteScalar(w, scalar);
        w.Key("columns"); w.Uint(columns);
        w.Key("rows"); w.Uint(rows);
        w.Key("stride"); w.Uint(stride);
        w.Key("rowMajor"); w.Boolean(false);
        w.Key("size"); w.Uint(stride * columns);
        w.EndObject();
        return stride * columns;
    }
    if (ScalarOf(type, &scalar, &count)) {
        if (count == 1) {
            WriteScalar(w, scalar);
            return scalar.width / 8;
        }
        w.BeginObject();
        w.Key("kind"); w.String("vector");
        w.Key("element"); WriteScalar(w, scalar);
        w.Key("count"); w.Uint(count);
        w.Key("size"); w.Uint(count * scalar.width / 8);
        w.EndObject();
        return count * scalar.width / 8;
    }
    // A texture, sampler, acceleration structure or function table: in an argument buffer an
    // eight-byte resource id, which the UI matches to the object that reported it.
    const char *opaque = OpaqueName(type);
    const bool handle = type == MTLDataTypeTexture || type == MTLDataTypeSampler || (NSUInteger)type >= 78;
    w.BeginObject();
    w.Key("kind"); w.String("opaque");
    w.Key("name"); w.String(opaque);
    if (handle) {
        w.Key("metal"); w.String(opaque);
        w.Key("size"); w.Uint(8);
    }
    w.EndObject();
    return handle ? 8 : 0;
}

const char *AccessName(NSUInteger access) {
    switch (access) {
        case 0: return "readOnly";
        case 1: return "readWrite";
        case 2: return "writeOnly";
        default: return "";
    }
}

/**
 * The bindings of one stage. `buffers` carries the layout: for a buffer argument declared as a
 * struct reference that is the struct; for `device float *data` it is a runtime-sized array of
 * the pointee, since the shader may index as far as the buffer goes.
 */
void WriteStage(vkinsp::JsonWriter &w, NSArray *bindings) {
    w.BeginObject();
    w.Key("buffers"); w.BeginArray();
    for (id b in bindings) {
        if (UIntProperty(b, @selector(type)) != kBindingBuffer) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(UIntProperty(b, @selector(index)));
        NSString *name = ObjectProperty(b, @selector(name));
        w.Key("name"); w.String(name == nil ? "" : name.UTF8String);
        w.Key("access"); w.String(AccessName(UIntProperty(b, @selector(access))));
        // MTLArgument spells it `isActive`, MTLBinding `isUsed` (the getter of `used`).
        const bool used = [b respondsToSelector:@selector(isActive)] ? BoolProperty(b, @selector(isActive))
                                                                       : BoolProperty(b, sel_registerName("isUsed"));
        w.Key("used"); w.Boolean(used);
        const uint64_t dataSize = UIntProperty(b, @selector(bufferDataSize));
        w.Key("dataSize"); w.Uint(dataSize);
        w.Key("alignment"); w.Uint(UIntProperty(b, @selector(bufferAlignment)));
        const MTLDataType dataType = (MTLDataType)UIntProperty(b, @selector(bufferDataType));
        MTLStructType *structType = ObjectProperty(b, @selector(bufferStructType));
        MTLPointerType *pointerType = ObjectProperty(b, @selector(bufferPointerType));
        w.Key("type");
        if (dataType == MTLDataTypeStruct && structType != nil) {
            WriteStruct(w, structType, name == nil ? nullptr : name.UTF8String, dataSize);
        } else if (dataType == MTLDataTypePointer && pointerType != nil && pointerType.elementStructType != nil) {
            // An argument buffer: the struct it encodes.
            WriteStruct(w, pointerType.elementStructType, name == nil ? nullptr : name.UTF8String,
                        pointerType.dataSize);
            if (pointerType.elementIsArgumentBuffer) { w.Key("argumentBuffer"); w.Boolean(true); }
        } else {
            // `device float4 *positions`: as many as the bound range holds.
            w.BeginObject();
            w.Key("kind"); w.String("array");
            w.Key("element");
            const uint64_t elementSize = WriteType(w, dataType, structType, nil, pointerType, 0);
            const uint64_t stride = dataSize != 0 ? dataSize : elementSize;
            w.Key("count"); w.Uint(0);
            w.Key("stride"); w.Uint(stride);
            w.Key("size"); w.Uint(0);
            w.EndObject();
        }
        w.EndObject();
    }
    w.EndArray();

    w.Key("textures"); w.BeginArray();
    for (id b in bindings) {
        if (UIntProperty(b, @selector(type)) != kBindingTexture) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(UIntProperty(b, @selector(index)));
        NSString *name = ObjectProperty(b, @selector(name));
        w.Key("name"); w.String(name == nil ? "" : name.UTF8String);
        w.Key("access"); w.String(AccessName(UIntProperty(b, @selector(access))));
        const MTLTextureType textureType = (MTLTextureType)UIntProperty(b, @selector(textureType));
        w.Key("textureType"); w.String(TextureTypeEnumName(textureType));
        Scalar scalar;
        uint32_t count = 0;
        const MTLDataType dataType = (MTLDataType)UIntProperty(b, @selector(textureDataType));
        w.Key("dataType");
        if (ScalarOf(dataType, &scalar, &count)) w.String(scalar.base); else w.Uint((uint64_t)dataType);
        const bool depth = [b respondsToSelector:@selector(isDepthTexture)] ? BoolProperty(b, @selector(isDepthTexture))
                                                                             : BoolProperty(b, @selector(depthTexture));
        w.Key("depth"); w.Boolean(depth);
        w.Key("arrayLength"); w.Uint(UIntProperty(b, @selector(arrayLength)));
        w.EndObject();
    }
    w.EndArray();

    w.Key("samplers"); w.BeginArray();
    for (id b in bindings) {
        if (UIntProperty(b, @selector(type)) != kBindingSampler) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(UIntProperty(b, @selector(index)));
        NSString *name = ObjectProperty(b, @selector(name));
        w.Key("name"); w.String(name == nil ? "" : name.UTF8String);
        w.Key("arrayLength"); w.Uint(UIntProperty(b, @selector(arrayLength)));
        w.EndObject();
    }
    w.EndArray();

    w.Key("threadgroup"); w.BeginArray();
    for (id b in bindings) {
        if (UIntProperty(b, @selector(type)) != kBindingThreadgroupMemory) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(UIntProperty(b, @selector(index)));
        NSString *name = ObjectProperty(b, @selector(name));
        w.Key("name"); w.String(name == nil ? "" : name.UTF8String);
        w.Key("dataSize"); w.Uint(UIntProperty(b, @selector(threadgroupMemoryDataSize)));
        w.Key("alignment"); w.Uint(UIntProperty(b, @selector(threadgroupMemoryAlignment)));
        w.EndObject();
    }
    w.EndArray();

    w.Key("other"); w.BeginArray();
    for (id b in bindings) {
        const NSUInteger type = UIntProperty(b, @selector(type));
        if (type == kBindingBuffer || type == kBindingTexture || type == kBindingSampler
            || type == kBindingThreadgroupMemory) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(UIntProperty(b, @selector(index)));
        NSString *name = ObjectProperty(b, @selector(name));
        w.Key("name"); w.String(name == nil ? "" : name.UTF8String);
        w.Key("bindingType"); w.Uint(type);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

/** The stage's bindings from whichever generation of the API the reflection object speaks. */
NSArray *StageBindings(id reflection, const char *bindingsSelector, const char *argumentsSelector) {
    NSArray *bindings = ObjectProperty(reflection, sel_registerName(bindingsSelector));
    if (bindings == nil) bindings = ObjectProperty(reflection, sel_registerName(argumentsSelector));
    return bindings;
}

void WriteStageIfAny(vkinsp::JsonWriter &w, const char *stage, NSArray *bindings) {
    if (bindings == nil || bindings.count == 0) return;
    w.Key(stage);
    WriteStage(w, bindings);
}

}  // namespace

std::string RenderReflectionJson(MTLRenderPipelineReflection *reflection) {
    if (reflection == nil) return {};
    vkinsp::JsonWriter w;
    w.BeginObject();
    WriteStageIfAny(w, "vertex", StageBindings(reflection, "vertexBindings", "vertexArguments"));
    WriteStageIfAny(w, "fragment", StageBindings(reflection, "fragmentBindings", "fragmentArguments"));
    WriteStageIfAny(w, "tile", StageBindings(reflection, "tileBindings", "tileArguments"));
    WriteStageIfAny(w, "object", StageBindings(reflection, "objectBindings", "objectArguments"));
    WriteStageIfAny(w, "mesh", StageBindings(reflection, "meshBindings", "meshArguments"));
    w.EndObject();
    return std::move(w.str());
}

std::string ComputeReflectionJson(MTLComputePipelineReflection *reflection) {
    if (reflection == nil) return {};
    vkinsp::JsonWriter w;
    w.BeginObject();
    WriteStageIfAny(w, "compute", StageBindings(reflection, "bindings", "arguments"));
    w.EndObject();
    return std::move(w.str());
}

}  // namespace mtlinsp
