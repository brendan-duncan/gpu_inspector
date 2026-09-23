#include "function_constants.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#import <Metal/Metal.h>

#include "json_writer.h"
#include "swizzle.h"

namespace mtlinsp
{
namespace
{

/** One value the application set, with the bytes it passed. */
struct ConstantValue
{
    // Set by index, by name, or both when a range was set by index and the shader names them.
    bool hasIndex = false;
    uint32_t index = 0;
    std::string name;
    MTLDataType type = MTLDataTypeNone;
    std::vector<uint8_t> bytes;
};

std::mutex g_mutex;
std::unordered_map<const void*, std::vector<ConstantValue>> g_values;
/** Function -> the values object it was specialized with, retained (see RememberFunctionConstants). */
std::unordered_map<const void*, id> g_functionValues;

/**
 * The scalar family a data type belongs to and its component count.
 *
 * A function constant may only be a scalar or a vector of one, so this is the whole of what can
 * arrive. The table is reflection.mm's, kept here rather than shared because that one is about a
 * buffer's layout and this one is about a value's bytes — they agree today and need not later.
 */
struct Scalar
{
    const char* base;
    uint32_t width;      // bits
};

bool ScalarOf(MTLDataType type, Scalar* scalar, uint32_t* count)
{
    struct Family
    {
        MTLDataType first;
        Scalar scalar;
    };
    static const Family kFamilies[] = {
        {MTLDataTypeFloat, {"float", 32}},
        {MTLDataTypeHalf, {"half", 16}},
        {MTLDataTypeInt, {"int", 32}},
        {MTLDataTypeUInt, {"uint", 32}},
        {MTLDataTypeShort, {"short", 16}},
        {MTLDataTypeUShort, {"ushort", 16}},
        {MTLDataTypeChar, {"char", 8}},
        {MTLDataTypeUChar, {"uchar", 8}},
        {MTLDataTypeBool, {"bool", 8}},
    };
    for (const Family& f : kFamilies)
    {
        // Each family is four consecutive values: the scalar, then the 2-, 3- and 4-vectors.
        if (type >= f.first && type < (MTLDataType)((NSUInteger)f.first + 4))
        {
            *scalar = f.scalar;
            *count = (uint32_t)((NSUInteger)type - (NSUInteger)f.first) + 1;
            return true;
        }
    }
    return false;
}

size_t ValueBytes(MTLDataType type)
{
    Scalar scalar;
    uint32_t count = 0;
    if (!ScalarOf(type, &scalar, &count))
        return 0;
    return (size_t)(scalar.width / 8) * count;
}

/** `float`, `float3`, `bool`: the type as the shader spells it. */
std::string TypeName(MTLDataType type)
{
    Scalar scalar;
    uint32_t count = 0;
    if (!ScalarOf(type, &scalar, &count))
        return "";
    return count > 1 ? std::string(scalar.base) + std::to_string(count) : scalar.base;
}

/** Half bits as a double, so a `half` constant reads as the number the shader will see. */
double Float16(uint16_t h)
{
    const int sign = (h & 0x8000) ? -1 : 1;
    const int exponent = (h >> 10) & 0x1f;
    const int fraction = h & 0x3ff;
    if (exponent == 0)
        return sign * ldexp((double)fraction / 1024.0, -14);
    if (exponent == 31)
        return fraction ? NAN : sign * INFINITY;
    return sign * ldexp(1.0 + (double)fraction / 1024.0, exponent - 15);
}

void WriteComponent(vkinsp::JsonWriter& w, const Scalar& scalar, const uint8_t* at)
{
    if (strcmp(scalar.base, "bool") == 0)
    {
        w.Boolean(*at != 0);
    }
    else if (strcmp(scalar.base, "float") == 0)
    {
        float value = 0;
        memcpy(&value, at, sizeof(value));
        w.Double(value);
    }
    else if (strcmp(scalar.base, "half") == 0)
    {
        uint16_t bits = 0;
        memcpy(&bits, at, sizeof(bits));
        w.Double(Float16(bits));
    }
    else if (scalar.base[0] == 'u')
    {
        uint64_t value = 0;
        memcpy(&value, at, scalar.width / 8);
        w.Uint(value);
    }
    else
    {
        int64_t value = 0;
        memcpy(&value, at, scalar.width / 8);
        // Sign-extend the narrow types, which memcpy left as their unsigned pattern.
        const int shift = 64 - (int)scalar.width;
        w.Int((value << shift) >> shift);
    }
}

void Remember(id self, MTLDataType type, const void* bytes, uint32_t index, bool hasIndex, NSString* name)
{
    const size_t size = ValueBytes(type);
    if (self == nil || bytes == nullptr || size == 0)
        return;
    ConstantValue value;
    value.hasIndex = hasIndex;
    value.index = index;
    if (name != nil && name.UTF8String != nullptr)
        value.name = name.UTF8String;
    value.type = type;
    const uint8_t* data = static_cast<const uint8_t*>(bytes);
    value.bytes.assign(data, data + size);

    std::lock_guard<std::mutex> lock(g_mutex);
    auto& list = g_values[(__bridge const void*)self];
    // Set again before the function was made: the last one is what the shader is built with.
    for (ConstantValue& existing : list)
    {
        const bool same = hasIndex ? (existing.hasIndex && existing.index == index)
                                   : (!value.name.empty() && existing.name == value.name);
        if (same)
        {
            existing = std::move(value);
            return;
        }
    }
    // A table this long is a shader with hundreds of variants, or a pointer being reused without
    // a dealloc ever being seen; either way there is nothing useful past it.
    if (list.size() < 512)
        list.push_back(std::move(value));
}

// --------------------------------------------------------------------------------------------
// The hooks

void FCV_setConstantValueAtIndex(id self, SEL _cmd, const void* value, MTLDataType type,
    NSUInteger index)
{
    Reentry reentry(self, _cmd);
    if (reentry.outermost())
        Remember(self, type, value, (uint32_t)index, true, nil);
    ((void (*)(id, SEL, const void*, MTLDataType, NSUInteger))reentry.original())(self, _cmd, value, type, index);
}

void FCV_setConstantValuesWithRange(id self, SEL _cmd, const void* values, MTLDataType type,
    NSRange range)
{
    Reentry reentry(self, _cmd);
    if (reentry.outermost())
    {
        const size_t stride = ValueBytes(type);
        const uint8_t* at = static_cast<const uint8_t*>(values);
        if (at != nullptr && stride != 0)
        {
            for (NSUInteger i = 0; i < range.length; i++)
            {
                Remember(self, type, at + i * stride, (uint32_t)(range.location + i), true, nil);
            }
        }
    }
    ((void (*)(id, SEL, const void*, MTLDataType, NSRange))reentry.original())(self, _cmd, values, type, range);
}

void FCV_setConstantValueWithName(id self, SEL _cmd, const void* value, MTLDataType type,
    NSString* name)
{
    Reentry reentry(self, _cmd);
    if (reentry.outermost())
        Remember(self, type, value, 0, false, name);
    ((void (*)(id, SEL, const void*, MTLDataType, NSString*))reentry.original())(self, _cmd, value, type, name);
}

void FCV_reset(id self, SEL _cmd)
{
    Reentry reentry(self, _cmd);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_values.erase((__bridge const void*)self);
    }
    ((void (*)(id, SEL))reentry.original())(self, _cmd);
}

/**
 * The entry goes when the object does. Without this, a freed object's values would answer for the
 * next one allocated at the same address — the pointer-reuse bug the tracker's own dealloc hook
 * exists to prevent, and these objects are made and released constantly.
 */
void FCV_dealloc(id self, SEL _cmd)
{
    Reentry reentry(self, _cmd);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_values.erase((__bridge const void*)self);
    }
    ((void (*)(id, SEL))reentry.original())(self, _cmd);
}

/** Installs the setters on the class an allocated values object actually has. */
void HookValuesClass(Class cls)
{
    if (cls == nullptr || !FirstSighting(cls))
        return;
    Log("hooking function constant values class %s", class_getName(cls));
    Hook(cls, @selector(setConstantValue:type:atIndex:), (IMP)FCV_setConstantValueAtIndex);
    Hook(cls, @selector(setConstantValues:type:withRange:), (IMP)FCV_setConstantValuesWithRange);
    Hook(cls, @selector(setConstantValue:type:withName:), (IMP)FCV_setConstantValueWithName);
    Hook(cls, @selector(reset), (IMP)FCV_reset);
    Hook(cls, sel_registerName("dealloc"), (IMP)FCV_dealloc);
}

id FCV_alloc(id self, SEL _cmd)
{
    Reentry reentry(self, _cmd);
    id values = ((id(*)(id, SEL))reentry.original())(self, _cmd);
    if (values != nil)
        HookValuesClass(object_getClass(values));
    return values;
}

id FCV_allocWithZone(id self, SEL _cmd, void* zone)
{
    Reentry reentry(self, _cmd);
    id values = ((id(*)(id, SEL, void*))reentry.original())(self, _cmd, zone);
    if (values != nil)
        HookValuesClass(object_getClass(values));
    return values;
}

}  // namespace

void HookFunctionConstantValues()
{
    Class cls = objc_getClass("MTLFunctionConstantValues");
    if (cls == nullptr || !FirstSighting(cls))
        return;
    Log("hooking %s, for the class its alloc hands out", class_getName(cls));
    // MTLFunctionConstantValues is a class cluster: `[[MTLFunctionConstantValues alloc] init]`
    // returns an instance of a private subclass, so hooking the public class hooks nothing any
    // object is of. Its alloc is hooked instead, and the class of what that returns is hooked from
    // the object — which is how every private Metal class in this library is found (swizzle.h).
    Class meta = object_getClass(cls);
    Hook(meta, @selector(alloc), (IMP)FCV_alloc);
    Hook(meta, @selector(allocWithZone:), (IMP)FCV_allocWithZone);
}

std::string FunctionConstantsJson(id values)
{
    if (values == nil)
        return "";
    std::vector<ConstantValue> list;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_values.find((__bridge const void*)values);
        if (it == g_values.end() || it->second.empty())
            return "";
        list = it->second;
    }
    vkinsp::JsonWriter w;
    w.BeginArray();
    for (const ConstantValue& c : list)
    {
        Scalar scalar;
        uint32_t count = 0;
        if (!ScalarOf(c.type, &scalar, &count))
            continue;
        w.BeginObject();
        if (c.hasIndex)
        {
            w.Key("index");
            w.Uint(c.index);
        }
        if (!c.name.empty())
        {
            w.Key("name");
            w.String(c.name);
        }
        w.Key("type");
        w.String(TypeName(c.type));
        w.Key("value");
        if (count == 1)
        {
            WriteComponent(w, scalar, c.bytes.data());
        }
        else
        {
            w.BeginArray();
            for (uint32_t i = 0; i < count; i++)
            {
                WriteComponent(w, scalar, c.bytes.data() + (size_t)i * (scalar.width / 8));
            }
            w.EndArray();
        }
        w.EndObject();
    }
    w.EndArray();
    return std::move(w.str());
}

void RememberFunctionConstants(id function, id values)
{
    if (function == nil || values == nil)
        return;
    id kept = [values retain];
    std::lock_guard<std::mutex> lock(g_mutex);
    id& slot = g_functionValues[(__bridge const void*)function];
    [slot release];
    slot = kept;
}

id FunctionConstantsOf(id function)
{
    if (function == nil)
        return nil;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_functionValues.find((__bridge const void*)function);
    return it != g_functionValues.end() ? it->second : nil;
}

}  // namespace mtlinsp
