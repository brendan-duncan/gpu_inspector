#include "mtl_decode.h"

#include <cstdlib>

#include "decode.h"   // vkreplay::DecodeBase64

namespace mtlreplay {

int64_t Decoder::ParseEnum(const JValue* v, const EnumTable& table, int64_t fallback) {
    if (!v || v->IsNull()) return fallback;
    if (v->IsNumber()) return v->Int();
    if (v->IsBool()) return v->boolean ? 1 : 0;
    if (!v->IsString()) return fallback;
    int64_t value = 0;
    if (mtlinsp::EnumValue(table.entries, table.count, v->Str(), value)) return value;
    // A value no table names is written as its number, sometimes as a string.
    //
    // A string that is not a number either is a *name* this table does not have: an enumerator from
    // a newer SDK than the replay was built against, or the same value written under another type's
    // name (the capture writes an acceleration structure's vertex format under its MTLVertexFormat
    // name, and the property is an MTLAttributeFormat). The caller's fallback is the right answer
    // there, and zero is very much the wrong one — for most Metal enums zero *is* a value, usually
    // "invalid", so a silent zero reads as a deliberate choice. That cost a day: a build given
    // MTLAttributeFormatInvalid vertices produces a structure with nothing in it, and every ray of
    // the frame misses, which looks exactly like a replay that worked.
    const std::string text(v->Str());
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 0);
    return end != text.c_str() ? (int64_t)parsed : fallback;
}

uint64_t Decoder::ParseFlags(const JValue* v, const EnumTable& table, uint64_t fallback) {
    if (!v || v->IsNull()) return fallback;
    if (v->IsNumber()) return v->Uint();
    if (v->IsBool()) return v->boolean ? 1 : 0;
    if (!v->IsString()) return fallback;
    std::string_view s = v->Str();
    uint64_t value = 0;
    while (!s.empty()) {
        const size_t bar = s.find('|');
        std::string_view token = s.substr(0, bar);
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        if (!token.empty()) {
            int64_t bit = 0;
            if (mtlinsp::EnumValue(table.entries, table.count, token, bit)) value |= (uint64_t)bit;
            else value |= std::strtoull(std::string(token).c_str(), nullptr, 0);
        }
        if (bar == std::string_view::npos) break;
        s.remove_prefix(bar + 1);
    }
    return value;
}

bool Decoder::Bytes(const char* name, std::vector<uint8_t>& out) const {
    const JValue* v = Get(name);
    if (!v) return false;
    const JValue* b64 = v->Get("base64");
    if (!b64 || !b64->IsString()) return false;
    if (!vkreplay::DecodeBase64(b64->Str(), out)) {
        _env.Problem(std::string("malformed base64 in ") + name);
        return false;
    }
    return true;
}

MTLSize Decoder::Size(const char* name, MTLSize fallback) const {
    const Decoder d = Nested(name);
    if (!d.Json()) return fallback;
    return MTLSizeMake((NSUInteger)d.Uint("width"), (NSUInteger)d.Uint("height"), (NSUInteger)d.Uint("depth"));
}

MTLOrigin Decoder::Origin(const char* name, MTLOrigin fallback) const {
    const Decoder d = Nested(name);
    if (!d.Json()) return fallback;
    return MTLOriginMake((NSUInteger)d.Uint("x"), (NSUInteger)d.Uint("y"), (NSUInteger)d.Uint("z"));
}

MTLRegion Decoder::Region(const char* name) const {
    const Decoder d = Nested(name);
    MTLRegion region = {};
    if (!d.Json()) return region;
    region.origin = d.Origin("origin");
    region.size = d.Size("size");
    return region;
}

NSRange Decoder::Range(const char* name) const {
    const Decoder d = Nested(name);
    if (!d.Json()) return NSMakeRange(0, 0);
    return NSMakeRange((NSUInteger)d.Uint("location"), (NSUInteger)d.Uint("length"));
}

MTLViewport Decoder::Viewport(const char* name) const {
    const Decoder d = Nested(name);
    MTLViewport v = {};
    if (!d.Json()) return v;
    v.originX = d.Double("originX");
    v.originY = d.Double("originY");
    v.width = d.Double("width");
    v.height = d.Double("height");
    v.znear = d.Double("znear");
    v.zfar = d.Double("zfar");
    return v;
}

MTLScissorRect Decoder::Scissor(const char* name) const {
    const Decoder d = Nested(name);
    MTLScissorRect r = {};
    if (!d.Json()) return r;
    r.x = (NSUInteger)d.Uint("x");
    r.y = (NSUInteger)d.Uint("y");
    r.width = (NSUInteger)d.Uint("width");
    r.height = (NSUInteger)d.Uint("height");
    return r;
}

MTLClearColor Decoder::ClearColor(const char* name) const {
    const JValue* v = Get(name);
    MTLClearColor c = MTLClearColorMake(0, 0, 0, 1);
    if (!v || !v->IsArray()) return c;
    double* channels[4] = {&c.red, &c.green, &c.blue, &c.alpha};
    for (uint32_t i = 0; i < v->count && i < 4; ++i) *channels[i] = v->items[i].Double();
    return c;
}

} // namespace mtlreplay
