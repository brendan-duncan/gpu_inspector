// Spells decoded D3D12 values as C++ source, for Export to C++: the visitor of dx_reflect.h that
// writes. A struct is declared zeroed and its members assigned one per line, which is what works
// for D3D12's unions (a barrier's Transition, a view's Texture2D: anonymous, so they have no
// designated initializer) and keeps a pipeline's description to the members that are set. A member
// left at zero is not written.
//
// Objects are spelled as the variable the exporter named them, a GPU address as its buffer's
// address plus an offset, a descriptor handle as a slot of its heap, and bytecode as a range of the
// exported project's data file: all through callbacks the exporter sets.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "dx_reflect.h"

namespace dxreplay
{

class Source
{
public:
    std::function<std::string(IUnknown*)> objectName;
    std::function<std::string(D3D12_GPU_VIRTUAL_ADDRESS)> addressExpr;
    std::function<std::string(D3D12_CPU_DESCRIPTOR_HANDLE)> cpuHandleExpr;
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

    std::string Object(IUnknown* object);
    std::string Address(D3D12_GPU_VIRTUAL_ADDRESS address);
    std::string CpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle);
    std::string Data(const void* bytes, size_t size) { return bytes && size && data ? data(bytes, size) : std::string("nullptr"); }
    static std::string Enum(const dxinsp::EnumEntry* table, size_t count, int64_t value, const char* type);
    static std::string Flags(const dxinsp::EnumEntry* table, size_t count, uint64_t value, const char* type);
    // The same with DX_TABLE's argument order (table, count, type), then the value.
    template <typename T>
    static std::string Enum(const dxinsp::EnumEntry* table, size_t count, const char* type, T value)
    {
        return Enum(table, count, (int64_t)value, type);
    }
    template <typename T>
    static std::string Flags(const dxinsp::EnumEntry* table, size_t count, const char* type, T value)
    {
        return Flags(table, count, (uint64_t)value, type);
    }
    static std::string Uint(uint64_t v);
    static std::string Int(int64_t v) { return std::to_string(v); }
    static std::string Float(float v);
    static std::string String(const char* s);

private:
    unsigned _locals = 0;
    size_t _lines = 0;
    size_t _statements = 0;
};

/** The C++ name of a type an array is declared with. */
template <typename T>
struct TypeName;
// clang-format off
#define DX_TYPE_NAME(T) template <> struct TypeName<T> { static constexpr const char* value = #T; };
DX_TYPE_NAME(D3D12_INPUT_ELEMENT_DESC)
DX_TYPE_NAME(D3D12_SO_DECLARATION_ENTRY)
DX_TYPE_NAME(D3D12_DESCRIPTOR_RANGE)
DX_TYPE_NAME(D3D12_DESCRIPTOR_RANGE1)
DX_TYPE_NAME(D3D12_ROOT_PARAMETER)
DX_TYPE_NAME(D3D12_ROOT_PARAMETER1)
DX_TYPE_NAME(D3D12_STATIC_SAMPLER_DESC)
DX_TYPE_NAME(D3D12_INDIRECT_ARGUMENT_DESC)
DX_TYPE_NAME(D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS)
DX_TYPE_NAME(D3D12_RECT)
DX_TYPE_NAME(D3D12_VIEWPORT)
DX_TYPE_NAME(D3D12_RESOURCE_BARRIER)
DX_TYPE_NAME(D3D12_VERTEX_BUFFER_VIEW)
DX_TYPE_NAME(D3D12_INDEX_BUFFER_VIEW)
DX_TYPE_NAME(D3D12_STREAM_OUTPUT_BUFFER_VIEW)
DX_TYPE_NAME(D3D12_RENDER_PASS_RENDER_TARGET_DESC)
DX_TYPE_NAME(D3D12_RENDER_PASS_DEPTH_STENCIL_DESC)
DX_TYPE_NAME(D3D12_HEAP_PROPERTIES)
DX_TYPE_NAME(D3D12_RESOURCE_DESC)
DX_TYPE_NAME(D3D12_CLEAR_VALUE)
DX_TYPE_NAME(D3D12_BOX)
DX_TYPE_NAME(D3D12_TEXTURE_COPY_LOCATION)
DX_TYPE_NAME(D3D12_SHADER_RESOURCE_VIEW_DESC)
DX_TYPE_NAME(D3D12_UNORDERED_ACCESS_VIEW_DESC)
DX_TYPE_NAME(D3D12_RENDER_TARGET_VIEW_DESC)
DX_TYPE_NAME(D3D12_DEPTH_STENCIL_VIEW_DESC)
DX_TYPE_NAME(D3D12_CONSTANT_BUFFER_VIEW_DESC)
DX_TYPE_NAME(D3D12_SAMPLER_DESC)
DX_TYPE_NAME(D3D12_GRAPHICS_PIPELINE_STATE_DESC)
DX_TYPE_NAME(D3D12_COMPUTE_PIPELINE_STATE_DESC)
DX_TYPE_NAME(D3D12_VERSIONED_ROOT_SIGNATURE_DESC)
DX_TYPE_NAME(D3D12_DESCRIPTOR_HEAP_DESC)
DX_TYPE_NAME(D3D12_COMMAND_QUEUE_DESC)
DX_TYPE_NAME(D3D12_QUERY_HEAP_DESC)
DX_TYPE_NAME(D3D12_COMMAND_SIGNATURE_DESC)
DX_TYPE_NAME(D3D12_DISCARD_REGION)
#undef DX_TYPE_NAME
// clang-format on

class Emitter
{
public:
    Emitter(Source& w, std::string path) : _w(w), _path(std::move(path)) {}

    template <typename T>
    void Int(const char* n, T& f)
    {
        if (!f)
            return;
        if (std::is_signed_v<T>)
            Set(n, Source::Int((int64_t)f));
        else
            Set(n, Source::Uint((uint64_t)f));
    }
    void Float(const char* n, float& f)
    {
        if (f != 0.0f)
            Set(n, Source::Float(f));
    }
    void Bool(const char* n, BOOL& f)
    {
        if (f)
            Set(n, "TRUE");
    }
    template <typename T>
    void Enum(const char* n, T& f, const dxinsp::EnumEntry* t, size_t c, const char* type)
    {
        if ((int64_t)f)
            Set(n, Source::Enum(t, c, (int64_t)f, type));
    }
    template <typename T>
    void Flags(const char* n, T& f, const dxinsp::EnumEntry* t, size_t c, const char* type)
    {
        if (!(uint64_t)f)
            return;
        // A flags member narrower than its enum (RenderTargetWriteMask is a UINT8) takes a cast.
        const std::string value = Source::Flags(t, c, (uint64_t)f, type);
        Set(n, std::is_enum_v<T> ? value : "(" + std::string(IntegerName<T>()) + ")(" + value + ")");
    }
    template <typename T>
    void Struct(const char* n, T& f)
    {
        Emitter e(_w, _path + n + ".");
        Reflect(e, f);
    }
    Emitter Nested(const char* n) { return Emitter(_w, _path + n + "."); }
    template <typename T>
    void Object(const char* n, T*& f, const char*)
    {
        if (f)
            Set(n, _w.Object(f));
    }

    template <typename T, typename C>
    void Array(const char* n, const T*& p, const char* countName, C& count)
    {
        if (!p || !count)
            return;
        const std::string var = _w.Local(Stem(n));
        _w.Line(std::string(TypeName<T>::value) + " " + var + "[" + std::to_string((uint64_t)count) + "] = {};");
        for (size_t i = 0; i < (size_t)count; ++i)
        {
            Emitter e(_w, var + "[" + std::to_string(i) + "].");
            Reflect(e, const_cast<T&>(p[i]));
        }
        Set(countName, Source::Uint((uint64_t)count));
        Set(n, var);
    }
    template <typename T, typename C>
    void Ints(const char* n, const T*& p, const char* countName, C& count, const char* type)
    {
        if (!p || !count)
            return;
        const std::string var = _w.Local(Stem(n));
        std::string items;
        for (size_t i = 0; i < (size_t)count; ++i)
            items += (i ? ", " : "") + Source::Uint((uint64_t)p[i]);
        _w.Line("const " + std::string(type) + " " + var + "[] = {" + items + "};");
        Set(countName, Source::Uint((uint64_t)count));
        Set(n, var);
    }
    void FixedFloats(const char* n, float* f, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
            if (f[i] != 0.0f)
                _w.Line(_path + n + "[" + std::to_string(i) + "] = " + Source::Float(f[i]) + ";");
    }
    template <typename T>
    void FixedStructs(const char* n, T* f, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
        {
            Emitter e(_w, _path + n + "[" + std::to_string(i) + "].");
            Reflect(e, f[i]);
        }
    }
    template <typename T, typename C>
    void FixedEnums(const char* n, T* f, size_t size, C& used, const dxinsp::EnumEntry* t, size_t c, const char* type)
    {
        for (size_t i = 0; i < size && i < (size_t)used; ++i)
            if ((int64_t)f[i])
                _w.Line(_path + n + "[" + std::to_string(i) + "] = " + Source::Enum(t, c, (int64_t)f[i], type) + ";");
    }
    void String(const char* n, LPCSTR& f)
    {
        if (f)
            Set(n, Source::String(f));
    }
    void Address(const char* n, D3D12_GPU_VIRTUAL_ADDRESS& f)
    {
        if (f)
            Set(n, _w.Address(f));
    }
    void CpuHandle(const char* n, D3D12_CPU_DESCRIPTOR_HANDLE& f)
    {
        if (f.ptr)
            Set(n, _w.CpuHandle(f));
    }
    void ComponentMapping(const char* n, UINT& f)
    {
        Set(n, f == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING ? std::string("D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING") : Source::Uint(f));
    }
    void Bytecode(const char* n, D3D12_SHADER_BYTECODE& f, const char*)
    {
        if (f.pShaderBytecode && f.BytecodeLength)
            Set(n, "{" + _w.Data(f.pShaderBytecode, f.BytecodeLength) + ", " + std::to_string(f.BytecodeLength) + "}");
    }

private:
    void Set(const char* n, const std::string& value) { _w.Line(_path + n + " = " + value + ";"); }
    /** pParameters -> parameters */
    static std::string Stem(const char* n)
    {
        std::string s = n;
        if (s.size() > 1 && s[0] == 'p' && s[1] >= 'A' && s[1] <= 'Z')
            s.erase(0, 1);
        if (!s.empty())
            s[0] = (char)tolower((unsigned char)s[0]);
        return s;
    }
    template <typename T>
    static const char* IntegerName()
    {
        return sizeof(T) == 1 ? "UINT8" : sizeof(T) == 2 ? "UINT16"
            : sizeof(T) == 8                             ? "UINT64"
                                                         : "UINT";
    }

    Source& _w;
    std::string _path;
};

/** Declares `Type name = {};`, assigns the members that are set, and returns the name. */
template <typename T>
std::string EmitStruct(Source& w, const std::string& hint, const T& value)
{
    const std::string name = w.Local(hint);
    w.Line(std::string(TypeName<T>::value) + " " + name + " = {};");
    Emitter e(w, name + ".");
    Reflect(e, const_cast<T&>(value));
    return name;
}

/** Declares an array of structs and returns its name; "nullptr" when empty. */
template <typename T>
std::string EmitStructs(Source& w, const std::string& hint, const T* items, size_t count)
{
    if (!items || !count)
        return "nullptr";
    const std::string name = w.Local(hint);
    w.Line(std::string(TypeName<T>::value) + " " + name + "[" + std::to_string(count) + "] = {};");
    for (size_t i = 0; i < count; ++i)
    {
        Emitter e(w, name + "[" + std::to_string(i) + "].");
        Reflect(e, const_cast<T&>(items[i]));
    }
    return name;
}

} // namespace dxreplay
