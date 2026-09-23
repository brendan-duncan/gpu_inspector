// Spells values as Objective-C++ source, for Export to C++: the writing half of the visitor pair
// mtl_reflect.h describes, and the counterpart of the D3D12 replay's dx_source.h.
//
// A Metal descriptor is an object rather than a C struct, so the source it writes is a fresh
// descriptor and one assignment per property that is not already at the class's default — which is
// how the application wrote it, and it keeps a pipeline's description down to what it actually set.
// Objects are spelled as the variable the exporter named them, and bytes as a range of the
// exported project's data file, both through callbacks the exporter sets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#import <Foundation/Foundation.h>

#include "metal_enums.gen.h"

namespace mtlreplay
{

/** A table and its length, as the MTL_TABLE macro of mtl_reflect.h passes them. */
struct EnumTable
{
    const mtlinsp::EnumEntry* entries;
    size_t count;
    const char* type;
};

class Source
{
public:
    /** The variable an object is spelled as ("texture_12"), or "" for one the export has no name for. */
    std::function<std::string(id object)> objectName;
    /** A range of the data file, spelled as the expression that reads it. */
    std::function<std::string(const void* data, size_t size)> data;

    int indent = 1;
    std::string text;
    std::vector<std::string> notes;

    void Line(const std::string& statement);
    void Comment(const std::string& c) { Line("// " + c); }
    void Blank() { text += "\n"; }
    void Note(const std::string& n)
    {
        if (notes.size() < 1000)
            notes.push_back(n);
    }
    /** A fresh local name from a hint ("descriptor" -> "descriptor3"). */
    std::string Local(const std::string& hint);
    size_t Lines() const { return _lines; }
    size_t Statements() const { return _statements; }
    void ResetPart()
    {
        text.clear();
        _lines = _statements = 0;
        _locals = 0;
    }
    void Append(const Source& other);

    /** An object as the variable the exporter named it; "nil", with a note, for one it did not. */
    std::string Object(id object);
    std::string Data(const void* bytes, size_t size) const
    {
        return bytes && size && data ? data(bytes, size) : std::string("nullptr");
    }

    /** `MTLPixelFormatBGRA8Unorm`, or a cast of the number for a value the table does not name. */
    static std::string Enum(const EnumTable& table, int64_t value);
    /** `MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget`, unknown bits left in hex. */
    static std::string Flags(const EnumTable& table, uint64_t value);
    static std::string Uint(uint64_t v);
    static std::string Int(int64_t v) { return std::to_string(v); }
    static std::string Float(double v);
    /** An NSString literal, or `nil`. */
    static std::string NSString(std::string_view s, bool present);
    static std::string Bool(bool v) { return v ? "YES" : "NO"; }
    /** `MTLSizeMake(w, h, d)` and `MTLOriginMake(x, y, z)`. */
    static std::string Size(uint64_t w, uint64_t h, uint64_t d);
    static std::string Origin(uint64_t x, uint64_t y, uint64_t z);

private:
    unsigned _locals = 0;
    size_t _lines = 0;
    size_t _statements = 0;
};

/**
 * The writing visitor of mtl_reflect.h: one `descriptor.property = value;` per property that is
 * not already at the value a freshly allocated descriptor of the same class holds.
 *
 * Leaving the defaults out is not only brevity. A Metal descriptor has dozens of properties and an
 * application sets a handful; writing all of them would bury what the frame actually asked for,
 * which is the thing a bug report is trying to show.
 */
class EmitVisitor
{
public:
    EmitVisitor(Source& w, std::string path) : _w(w), _path(std::move(path)) {}

    void Uint(const char* n, uint64_t value, uint64_t dflt, void (^)(uint64_t))
    {
        if (value != dflt)
            Set(n, Source::Uint(value));
    }
    void Int(const char* n, int64_t value, int64_t dflt, void (^)(int64_t))
    {
        if (value != dflt)
            Set(n, Source::Int(value));
    }
    void Float(const char* n, double value, double dflt, void (^)(double))
    {
        if (value != dflt)
            Set(n, Source::Float(value));
    }
    void Bool(const char* n, bool value, bool dflt, void (^)(bool))
    {
        if (value != dflt)
            Set(n, Source::Bool(value));
    }
    void Enum(const char* n, int64_t value, int64_t dflt, const EnumTable& table, void (^)(int64_t))
    {
        if (value != dflt)
            Set(n, Source::Enum(table, value));
    }
    void Flags(const char* n, uint64_t value, uint64_t dflt, const EnumTable& table, void (^)(uint64_t))
    {
        if (value != dflt)
            Set(n, Source::Flags(table, value));
    }
    void Object(const char* n, id value, void (^)(id))
    {
        if (value != nil)
            Set(n, _w.Object(value));
    }
    void Label(NSString* value, void (^)(NSString*))
    {
        if (value != nil)
            Set("label", Source::NSString(value.UTF8String, true));
    }

private:
    void Set(const char* n, const std::string& value) { _w.Line(_path + n + " = " + value + ";"); }

    Source& _w;
    std::string _path;
};

} // namespace mtlreplay
