// JSON serializers for the D3D11 and DXGI structures the hooks record: the descriptions an object
// is created from, the arguments of a command. Written by hand, one per struct, in the shape the
// inspector reads for every API: every member under its own name, enums by name
// (gen/d3d11_enums.gen.h), flags as "A | B", nested structs as objects, fixed arrays as arrays,
// `Num*`/`p*` pairs as arrays, and object pointers as tracked references. Shader bytecode is
// summarized ({"__bytes": N}); the bytes themselves go into blobs (state.h).
//
// Every writer takes the JsonWriter positioned at a value (after Key()) and writes one value.
#pragma once

#include "common.h"
#include "state.h"

namespace d3d11insp {

/** An enum as the value the generated name tables hold. */
template <typename T>
inline int64_t EnumValue(T v) { return (int64_t)(uint32_t)(int64_t)v; }

/** A tracked object reference: null for null, {__id, __class} when known, {__handle, __class} when not. */
void WriteRef(JsonWriter& w, const void* object, const char* fallbackClass = "ID3D11DeviceChild");
/** An array of `count` object pointers as references; null when the pointer is null. */
void WriteRefs(JsonWriter& w, IUnknown* const* objects, UINT count);
template <typename T>
inline void WriteRefs(JsonWriter& w, T* const* objects, UINT count) { WriteRefs(w, (IUnknown* const*)objects, count); }
void WriteUints(JsonWriter& w, const UINT* values, UINT count);
void WriteFloats(JsonWriter& w, const FLOAT* values, UINT count);

/** A command's arguments, built fluently and closed by str(). */
class Args {
public:
    Args() { w_.BeginObject(); }
    Args& u(const char* key, uint64_t value) { w_.Key(key); w_.Uint(value); return *this; }
    Args& i(const char* key, int64_t value) { w_.Key(key); w_.Int(value); return *this; }
    Args& d(const char* key, double value) { w_.Key(key); w_.Double(value); return *this; }
    Args& b(const char* key, bool value) { w_.Key(key); w_.Boolean(value); return *this; }
    Args& s(const char* key, const char* value) { w_.Key(key); if (value) w_.String(value); else w_.Null(); return *this; }
    Args& s(const char* key, const std::string& value) { w_.Key(key); w_.String(value); return *this; }
    Args& ws(const char* key, const wchar_t* value) { w_.Key(key); if (value) w_.String(Narrow(value)); else w_.Null(); return *this; }
    /** An enum by its generated name, the number when the table has none. */
    Args& e(const char* key, const char* name, int64_t value) { w_.Key(key); w_.Enum(name, value); return *this; }
    Args& ref(const char* key, const void* object, const char* fallbackClass = "ID3D11DeviceChild") { w_.Key(key); WriteRef(w_, object, fallbackClass); return *this; }
    Args& refs(const char* key, IUnknown* const* objects, UINT count) { w_.Key(key); WriteRefs(w_, objects, count); return *this; }
    template <typename T>
    Args& refs(const char* key, T* const* objects, UINT count) { return refs(key, (IUnknown* const*)objects, count); }
    Args& uints(const char* key, const UINT* values, UINT count) { w_.Key(key); WriteUints(w_, values, count); return *this; }
    Args& floats(const char* key, const FLOAT* values, UINT count) { w_.Key(key); WriteFloats(w_, values, count); return *this; }
    Args& ptr(const char* key, const void* p) { w_.Key(key); w_.Pointer(p); return *this; }
    Args& bytes(const char* key, const void* data, size_t length) { w_.Key(key); w_.Bytes(data, length); return *this; }
    Args& raw(const char* key, const std::string& json) { w_.Key(key); if (json.empty()) w_.Null(); else w_.Raw(json); return *this; }
    Args& null(const char* key) { w_.Key(key); w_.Null(); return *this; }
    /** Positions the writer at `key` for a nested value written through writer(). */
    JsonWriter& key(const char* key) { w_.Key(key); return w_; }
    JsonWriter& writer() { return w_; }
    /** Closes the object. The Args is spent afterwards. */
    std::string str() { w_.EndObject(); return std::move(w_.str()); }

private:
    JsonWriter w_;
};

// DXGI
void Write(JsonWriter& w, const DXGI_SAMPLE_DESC& v);
void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_DESC& v);
void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_DESC1& v);
void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC& v);
void Write(JsonWriter& w, const DXGI_PRESENT_PARAMETERS& v);
void Write(JsonWriter& w, const DXGI_ADAPTER_DESC& v);

// Resources
void Write(JsonWriter& w, const D3D11_BUFFER_DESC& v);
void Write(JsonWriter& w, const D3D11_TEXTURE1D_DESC& v);
void Write(JsonWriter& w, const D3D11_TEXTURE2D_DESC& v);
void Write(JsonWriter& w, const D3D11_TEXTURE2D_DESC1& v);
void Write(JsonWriter& w, const D3D11_TEXTURE3D_DESC& v);
void Write(JsonWriter& w, const D3D11_TEXTURE3D_DESC1& v);
/** The initial data of `count` subresources: how many bytes each carries, not the bytes. */
void WriteInitialData(JsonWriter& w, const D3D11_SUBRESOURCE_DATA* data, UINT count);
void Write(JsonWriter& w, const D3D11_BOX* v);
void Write(JsonWriter& w, const D3D11_MAPPED_SUBRESOURCE& v);

// Views and samplers
void Write(JsonWriter& w, const D3D11_SHADER_RESOURCE_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D11_SHADER_RESOURCE_VIEW_DESC1* v);
void Write(JsonWriter& w, const D3D11_RENDER_TARGET_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D11_RENDER_TARGET_VIEW_DESC1* v);
void Write(JsonWriter& w, const D3D11_DEPTH_STENCIL_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D11_UNORDERED_ACCESS_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D11_UNORDERED_ACCESS_VIEW_DESC1* v);
void Write(JsonWriter& w, const D3D11_SAMPLER_DESC& v);

// Pipeline state
void Write(JsonWriter& w, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count);
void Write(JsonWriter& w, const D3D11_SO_DECLARATION_ENTRY* entries, UINT count);
void Write(JsonWriter& w, const D3D11_BLEND_DESC& v);
void Write(JsonWriter& w, const D3D11_BLEND_DESC1& v);
void Write(JsonWriter& w, const D3D11_RASTERIZER_DESC& v);
void Write(JsonWriter& w, const D3D11_RASTERIZER_DESC1& v);
void Write(JsonWriter& w, const D3D11_RASTERIZER_DESC2& v);
void Write(JsonWriter& w, const D3D11_DEPTH_STENCIL_DESC& v);
void Write(JsonWriter& w, const D3D11_DEPTH_STENCILOP_DESC& v);
void Write(JsonWriter& w, const D3D11_QUERY_DESC& v);
void Write(JsonWriter& w, const D3D11_QUERY_DESC1& v);
void Write(JsonWriter& w, const D3D11_COUNTER_DESC& v);

// Commands
void Write(JsonWriter& w, const D3D11_VIEWPORT& v);
void Write(JsonWriter& w, const D3D11_RECT& v);
void WriteViewports(JsonWriter& w, const D3D11_VIEWPORT* v, UINT count);
void WriteRects(JsonWriter& w, const D3D11_RECT* v, UINT count);

}  // namespace d3d11insp
