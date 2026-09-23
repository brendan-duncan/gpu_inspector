// Reads the Metal capture library's JSON back into values: the filling half of the visitor pair
// mtl_reflect.h describes, and the counterpart of the D3D12 replay's dx_decode.h.
//
// Two things about the Metal capture's JSON shape the readers here absorb, so nothing above has to
// know about them (src/metal/src/hooks_common.h, hooks_descriptors.mm):
//
//   * an enum is written by name when formats.mm has one for it ("MTLPixelFormatBGRA8Unorm") and
//     as a bare number when it does not, so every read accepts both;
//   * a flag set is written in the short spelling ("ShaderRead|RenderTarget"), which the generated
//     tables resolve by suffix (EnumValue in metal_enums.gen.h), and sometimes as a number.
//
// An object reference is `{"__id", "__class"}`, which DecodeEnv resolves to the replay's own
// object, counting what it could not.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "json.h"

#include "mtl_source.h"

namespace mtlreplay
{

using vkreplay::JValue;

/** The capture id of a `{"__id", "__class"}` reference, or 0. */
inline uint64_t IdOf(const JValue* v)
{
    const JValue* id = v ? v->Get("__id") : nullptr;
    return id ? id->Uint() : 0;
}

struct DecodeEnv
{
    /** The replay's object for a capture id, or nil. */
    std::function<id(uint64_t captureId)> object;
    /** References that resolved to nothing: what names them is left out rather than handed a nil. */
    size_t unresolved = 0;
    std::vector<std::string> problems;
    std::string where;

    void Problem(const std::string& message)
    {
        problems.push_back(where.empty() ? message : where + ": " + message);
    }
};

class Decoder
{
public:
    Decoder(const JValue* json, DecodeEnv& env) : _j(json), _env(env) {}

    const JValue* Json() const { return _j; }
    bool Has(const char* name) const { return Get(name) != nullptr; }
    /** A member of this object, or null; also null for a JSON null, which reads as absent. */
    const JValue* Get(const char* name) const
    {
        const JValue* v = _j ? _j->Get(name) : nullptr;
        return v && !v->IsNull() ? v : nullptr;
    }
    /** A nested object as a decoder of its own. */
    Decoder Nested(const char* name) const { return Decoder(Get(name), _env); }
    Decoder At(const JValue* value) const { return Decoder(value, _env); }

    uint64_t Uint(const char* name, uint64_t fallback = 0) const
    {
        const JValue* v = Get(name);
        if (!v)
            return fallback;
        if (v->IsBool())
            return v->boolean ? 1 : 0;
        return v->Uint();
    }
    int64_t Int(const char* name, int64_t fallback = 0) const
    {
        const JValue* v = Get(name);
        if (!v)
            return fallback;
        if (v->IsBool())
            return v->boolean ? 1 : 0;
        return v->Int();
    }
    double Double(const char* name, double fallback = 0) const
    {
        const JValue* v = Get(name);
        return v ? v->Double() : fallback;
    }
    bool Bool(const char* name, bool fallback = false) const
    {
        const JValue* v = Get(name);
        if (!v)
            return fallback;
        return v->IsBool() ? v->boolean : v->Uint() != 0;
    }
    /** A string member, or "" (with `present` false) when it is absent or null. */
    std::string Str(const char* name, bool* present = nullptr) const
    {
        const JValue* v = Get(name);
        if (present)
            *present = v && v->IsString();
        return v && v->IsString() ? std::string(v->Str()) : std::string();
    }
    /** A string member as an NSString, or nil. */
    NSString* NSStr(const char* name) const
    {
        const JValue* v = Get(name);
        if (!v || !v->IsString())
            return nil;
        return [[NSString alloc] initWithBytes:v->text length:v->length encoding:NSUTF8StringEncoding];
    }

    int64_t Enum(const char* name, const EnumTable& table, int64_t fallback = 0) const
    {
        return ParseEnum(Get(name), table, fallback);
    }
    uint64_t Flags(const char* name, const EnumTable& table, uint64_t fallback = 0) const
    {
        return ParseFlags(Get(name), table, fallback);
    }

    /** The replay's object for a reference member; nil, counted as unresolved, when there is none. */
    id Object(const char* name) const { return Resolve(IdOf(Get(name))); }
    uint64_t ObjectId(const char* name) const { return IdOf(Get(name)); }
    /** `captureId` rather than `id`, which is Objective-C's own. */
    id Resolve(uint64_t captureId) const
    {
        if (!captureId)
            return nil;
        id object = _env.object ? _env.object(captureId) : nil;
        if (!object)
            ++_env.unresolved;
        return object;
    }

    /** A `{"__bytes", "base64"}` member's contents; false when it is absent or malformed. */
    bool Bytes(const char* name, std::vector<uint8_t>& out) const;

    // The shapes Args writes for Metal's small structs (src/metal/src/hooks_common.h).
    /** `{width, height, depth}` / `{x, y, z}`. */
    MTLSize Size(const char* name, MTLSize fallback = MTLSizeMake(0, 0, 0)) const;
    MTLOrigin Origin(const char* name, MTLOrigin fallback = MTLOriginMake(0, 0, 0)) const;
    /** `{origin, size}`. */
    MTLRegion Region(const char* name) const;
    /** `{location, length}`, which the capture writes for every range. */
    NSRange Range(const char* name) const;
    MTLViewport Viewport(const char* name) const;
    MTLScissorRect Scissor(const char* name) const;
    /** A `[r, g, b, a]` array (a pass's clearColor). */
    MTLClearColor ClearColor(const char* name) const;

    DecodeEnv& Env() const { return _env; }

    static int64_t ParseEnum(const JValue* v, const EnumTable& table, int64_t fallback = 0);
    static uint64_t ParseFlags(const JValue* v, const EnumTable& table, uint64_t fallback = 0);

private:
    const JValue* _j = nullptr;
    DecodeEnv& _env;
};

/**
 * The filling visitor of mtl_reflect.h: sets each property the capture's JSON has a value for, and
 * leaves the rest at whatever a freshly allocated descriptor holds — which is what the application
 * left them at, since the capture writes every property it read.
 */
class FillVisitor
{
public:
    explicit FillVisitor(const Decoder& d) : _d(d) {}

    void Uint(const char* n, uint64_t, uint64_t, void (^set)(uint64_t))
    {
        if (const JValue* v = _d.Get(n))
            set(v->IsBool() ? (v->boolean ? 1u : 0u) : v->Uint());
    }
    void Int(const char* n, int64_t, int64_t, void (^set)(int64_t))
    {
        if (const JValue* v = _d.Get(n))
            set(v->IsBool() ? (v->boolean ? 1 : 0) : v->Int());
    }
    void Float(const char* n, double, double, void (^set)(double))
    {
        if (const JValue* v = _d.Get(n))
            set(v->Double());
    }
    void Bool(const char* n, bool, bool, void (^set)(bool))
    {
        if (const JValue* v = _d.Get(n))
            set(v->IsBool() ? v->boolean : v->Uint() != 0);
    }
    void Enum(const char* n, int64_t, int64_t, const EnumTable& table, void (^set)(int64_t))
    {
        if (const JValue* v = _d.Get(n))
            set(Decoder::ParseEnum(v, table));
    }
    void Flags(const char* n, uint64_t, uint64_t, const EnumTable& table, void (^set)(uint64_t))
    {
        if (const JValue* v = _d.Get(n))
            set(Decoder::ParseFlags(v, table));
    }
    void Object(const char* n, id, void (^set)(id))
    {
        // A reference that resolves to nothing is left alone rather than set to nil: what the
        // property already holds is at worst the descriptor's default, and DecodeEnv counted it.
        const JValue* v = _d.Get(n);
        if (!v)
            return;
        if (id object = _d.Resolve(IdOf(v)))
            set(object);
    }
    void Label(NSString*, void (^set)(NSString*))
    {
        if (NSString* label = _d.NSStr("label"))
            set(label);
    }

private:
    const Decoder& _d;
};

} // namespace mtlreplay
