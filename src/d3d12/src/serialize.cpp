// JSON serializers for the D3D12 and DXGI structures the hooks record (serialize.h). Written by
// hand, one per struct, in the shape the generated Vulkan serializers give the UI: every member
// under its C name, enums by name, flags as "A | B", nested structs as objects, fixed arrays as
// arrays, Num*/p* pairs as arrays, object pointers as tracked references, and every union by its
// discriminator. Nothing here allocates beyond the writer's string.
#include "serialize.h"

#include "d3d12_enums.gen.h"
#include "formats.h"
#include "json.h"
#include "tracker.h"

#include <cstring>
#include <string>
#include <vector>

namespace dxinsp
{

// ---------------------------------------------------------------------------------------------
// Member helpers. Every writer is a sequence of these, one per member, under the C name; `v` is
// the struct and `w` the writer in every function they are used in.

// clang-format off
#define M_UINT(m)      w.Key(#m); w.Uint((uint64_t)v.m)
#define M_INT(m)       w.Key(#m); w.Int((int64_t)v.m)
#define M_BOOL(m)      w.Key(#m); w.Boolean(v.m != 0)
#define M_FLOAT(m)     w.Key(#m); w.Double((double)v.m)
#define M_ENUM(m, E)   w.Key(#m); w.Enum(ToString_##E(EnumValue(v.m)), EnumValue(v.m))
#define M_FLAGS(m, E)  w.Key(#m); Flags_##E(w, (uint64_t)v.m)
#define M_STR(m)       w.Key(#m); w.String(v.m)
#define M_WSTR(m)      w.Key(#m); WriteWide(w, v.m)
#define M_NESTED(m)    w.Key(#m); Write(w, v.m)
#define M_REF(m, T)    w.Key(#m); WriteRef(w, v.m, T)
#define M_ADDRESS(m)   w.Key(#m); WriteGpuAddress(w, v.m)
#define M_HANDLE(m)    w.Key(#m); WriteCpuHandle(w, v.m)
// clang-format on

static void WriteWide(JsonWriter& w, const wchar_t* s)
{
    if (!s)
        w.Null();
    else
        w.String(Narrow(s));
}

/** `count` wide strings (a state object's export lists). */
static void WriteWideArray(JsonWriter& w, UINT count, LPCWSTR const* strings)
{
    if (!strings)
    {
        w.Null();
        return;
    }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i)
        WriteWide(w, strings[i]);
    w.EndArray();
}

/** `count` structs at `items`, or null: the shape of every Num* + p* pair. */
template <typename T, typename Fn>
static void WriteArray(JsonWriter& w, UINT count, const T* items, Fn fn)
{
    if (!items)
    {
        w.Null();
        return;
    }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i)
        fn(items[i]);
    w.EndArray();
}

// Helpers for structs the header does not declare writers for (they only appear nested).
static void Write(JsonWriter& w, const DXGI_RATIONAL& v);
static void Write(JsonWriter& w, const DXGI_MODE_DESC& v);
static void Write(JsonWriter& w, const POINT& v);
static void Write(JsonWriter& w, const D3D12_MIP_REGION& v);
static void Write(JsonWriter& w, const D3D12_SO_DECLARATION_ENTRY& v);
static void Write(JsonWriter& w, const D3D12_VIEW_INSTANCE_LOCATION& v);
static void Write(JsonWriter& w, const D3D12_DESCRIPTOR_RANGE& v);
static void Write(JsonWriter& w, const D3D12_DESCRIPTOR_RANGE1& v);
static void Write(JsonWriter& w, const D3D12_ROOT_PARAMETER& v);
static void Write(JsonWriter& w, const D3D12_ROOT_PARAMETER1& v);
static void Write(JsonWriter& w, const D3D12_GLOBAL_BARRIER& v);
static void Write(JsonWriter& w, const D3D12_TEXTURE_BARRIER& v);
static void Write(JsonWriter& w, const D3D12_BUFFER_BARRIER& v);
static void Write(JsonWriter& w, const D3D12_BARRIER_SUBRESOURCE_RANGE& v);
static void Write(JsonWriter& w, const D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS& v);
static void Write(JsonWriter& w, const D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE& v);
void Write(JsonWriter& w, const D3D12_RAYTRACING_GEOMETRY_DESC& v);
static void Write(JsonWriter& w, const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& v);
static void Write(JsonWriter& w, const D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC& v);
static void Write(JsonWriter& w, const D3D12_NODE_CPU_INPUT& v);
static void Write(JsonWriter& w, const D3D12_PROGRAM_IDENTIFIER& v);
static void Write(JsonWriter& w, const D3D12_EXPORT_DESC& v);
static void Write(JsonWriter& w, const D3D12_NODE_ID& v);
static void Write(JsonWriter& w, const D3D12_NODE& v);

// After the declarations above, so the call inside sees every writer (the structs live in the
// global namespace, where argument-dependent lookup finds nothing of ours).
template <typename T>
static void WriteArray(JsonWriter& w, UINT count, const T* items)
{
    WriteArray(w, count, items, [&](const T& item) { Write(w, item); });
}

// ---------------------------------------------------------------------------------------------
// Scalars with a shape of their own

void Write(JsonWriter& w, const GUID& v)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
        (unsigned long)v.Data1, v.Data2, v.Data3, v.Data4[0], v.Data4[1], v.Data4[2], v.Data4[3],
        v.Data4[4], v.Data4[5], v.Data4[6], v.Data4[7]);
    w.String(buf);
}

void Write(JsonWriter& w, const LUID& v)
{
    w.BeginObject();
    w.Key("LowPart");
    w.String(Hex(v.LowPart));
    w.Key("HighPart");
    w.String(Hex((uint32_t)v.HighPart));
    w.EndObject();
}

void WriteFormats(JsonWriter& w, const DXGI_FORMAT* formats, uint32_t count)
{
    if (!formats)
    {
        w.Null();
        return;
    }
    w.BeginArray();
    for (uint32_t i = 0; i < count; ++i)
        w.Enum(ToString_DXGI_FORMAT(EnumValue(formats[i])), EnumValue(formats[i]));
    w.EndArray();
}

/** DXGI_USAGE is a set of #defines, not an enum, so the generator has no table for it. */
static void WriteDxgiUsage(JsonWriter& w, DXGI_USAGE usage)
{
    static const struct
    {
        DXGI_USAGE bit;
        const char* name;
    } kBits[] = {
        {DXGI_USAGE_SHADER_INPUT, "DXGI_USAGE_SHADER_INPUT"},
        {DXGI_USAGE_RENDER_TARGET_OUTPUT, "DXGI_USAGE_RENDER_TARGET_OUTPUT"},
        {DXGI_USAGE_BACK_BUFFER, "DXGI_USAGE_BACK_BUFFER"},
        {DXGI_USAGE_SHARED, "DXGI_USAGE_SHARED"},
        {DXGI_USAGE_READ_ONLY, "DXGI_USAGE_READ_ONLY"},
        {DXGI_USAGE_DISCARD_ON_PRESENT, "DXGI_USAGE_DISCARD_ON_PRESENT"},
        {DXGI_USAGE_UNORDERED_ACCESS, "DXGI_USAGE_UNORDERED_ACCESS"},
    };
    std::string out;
    DXGI_USAGE rest = usage;
    for (const auto& b : kBits)
    {
        if (!(usage & b.bit))
            continue;
        if (!out.empty())
            out += " | ";
        out += b.name;
        rest &= ~b.bit;
    }
    if (rest)
    {
        if (!out.empty())
            out += " | ";
        out += Hex(rest);
    }
    if (out.empty())
        out = "0";
    w.String(out);
}

/** The four components of a Shader4ComponentMapping, by name. */
static void WriteComponentMapping(JsonWriter& w, UINT mapping)
{
    w.BeginArray();
    for (UINT c = 0; c < 4; ++c)
    {
        int64_t m = (int64_t)D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(c, mapping);
        w.Enum(ToString_D3D12_SHADER_COMPONENT_MAPPING(m), m);
    }
    w.EndArray();
}

/** A depth-stencil format: what decides which arm of D3D12_CLEAR_VALUE's union is live. */
static bool IsDepthFormat(DXGI_FORMAT format)
{
    FormatInfo info = FormatOf(format);
    return info.depth || info.stencil;
}

// ---------------------------------------------------------------------------------------------
// DXGI

static void Write(JsonWriter& w, const DXGI_RATIONAL& v)
{
    w.BeginObject();
    M_UINT(Numerator);
    M_UINT(Denominator);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_SAMPLE_DESC& v)
{
    w.BeginObject();
    M_UINT(Count);
    M_UINT(Quality);
    w.EndObject();
}

static void Write(JsonWriter& w, const DXGI_MODE_DESC& v)
{
    w.BeginObject();
    M_UINT(Width);
    M_UINT(Height);
    M_NESTED(RefreshRate);
    M_ENUM(Format, DXGI_FORMAT);
    M_ENUM(ScanlineOrdering, DXGI_MODE_SCANLINE_ORDER);
    M_ENUM(Scaling, DXGI_MODE_SCALING);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_DESC& v)
{
    w.BeginObject();
    M_NESTED(BufferDesc);
    M_NESTED(SampleDesc);
    w.Key("BufferUsage");
    WriteDxgiUsage(w, v.BufferUsage);
    M_UINT(BufferCount);
    w.Key("OutputWindow");
    w.Pointer((const void*)v.OutputWindow);
    M_BOOL(Windowed);
    M_ENUM(SwapEffect, DXGI_SWAP_EFFECT);
    w.Key("Flags");
    WriteFlags(w, kEnum_DXGI_SWAP_CHAIN_FLAG, 13, v.Flags);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_DESC1& v)
{
    w.BeginObject();
    M_UINT(Width);
    M_UINT(Height);
    M_ENUM(Format, DXGI_FORMAT);
    M_BOOL(Stereo);
    M_NESTED(SampleDesc);
    w.Key("BufferUsage");
    WriteDxgiUsage(w, v.BufferUsage);
    M_UINT(BufferCount);
    M_ENUM(Scaling, DXGI_SCALING);
    M_ENUM(SwapEffect, DXGI_SWAP_EFFECT);
    M_ENUM(AlphaMode, DXGI_ALPHA_MODE);
    w.Key("Flags");
    WriteFlags(w, kEnum_DXGI_SWAP_CHAIN_FLAG, 13, v.Flags);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC& v)
{
    w.BeginObject();
    M_NESTED(RefreshRate);
    M_ENUM(ScanlineOrdering, DXGI_MODE_SCANLINE_ORDER);
    M_ENUM(Scaling, DXGI_MODE_SCALING);
    M_BOOL(Windowed);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_ADAPTER_DESC3& v)
{
    w.BeginObject();
    w.Key("Description");
    w.String(Narrow(v.Description, wcsnlen(v.Description, 128)));
    M_UINT(VendorId);
    M_UINT(DeviceId);
    M_UINT(SubSysId);
    M_UINT(Revision);
    M_UINT(DedicatedVideoMemory);
    M_UINT(DedicatedSystemMemory);
    M_UINT(SharedSystemMemory);
    M_NESTED(AdapterLuid);
    M_FLAGS(Flags, DXGI_ADAPTER_FLAG3);
    M_ENUM(GraphicsPreemptionGranularity, DXGI_GRAPHICS_PREEMPTION_GRANULARITY);
    M_ENUM(ComputePreemptionGranularity, DXGI_COMPUTE_PREEMPTION_GRANULARITY);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_ADAPTER_DESC1& v)
{
    w.BeginObject();
    w.Key("Description");
    w.String(Narrow(v.Description, wcsnlen(v.Description, 128)));
    M_UINT(VendorId);
    M_UINT(DeviceId);
    M_UINT(SubSysId);
    M_UINT(Revision);
    M_UINT(DedicatedVideoMemory);
    M_UINT(DedicatedSystemMemory);
    M_UINT(SharedSystemMemory);
    M_NESTED(AdapterLuid);
    w.Key("Flags");
    WriteFlags(w, kEnum_DXGI_ADAPTER_FLAG, 4, v.Flags);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RECT& v)
{
    w.BeginObject();
    M_INT(left);
    M_INT(top);
    M_INT(right);
    M_INT(bottom);
    w.EndObject();
}

static void Write(JsonWriter& w, const POINT& v)
{
    w.BeginObject();
    M_INT(x);
    M_INT(y);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_PRESENT_PARAMETERS& v)
{
    w.BeginObject();
    M_UINT(DirtyRectsCount);
    w.Key("pDirtyRects");
    WriteArray(w, v.DirtyRectsCount, v.pDirtyRects);
    w.Key("pScrollRect");
    if (v.pScrollRect)
        Write(w, *v.pScrollRect);
    else
        w.Null();
    w.Key("pScrollOffset");
    if (v.pScrollOffset)
        Write(w, *v.pScrollOffset);
    else
        w.Null();
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Resources and heaps

void Write(JsonWriter& w, const D3D12_HEAP_PROPERTIES& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_HEAP_TYPE);
    M_ENUM(CPUPageProperty, D3D12_CPU_PAGE_PROPERTY);
    M_ENUM(MemoryPoolPreference, D3D12_MEMORY_POOL);
    M_UINT(CreationNodeMask);
    M_UINT(VisibleNodeMask);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_HEAP_DESC& v)
{
    w.BeginObject();
    M_UINT(SizeInBytes);
    M_NESTED(Properties);
    M_UINT(Alignment);
    M_FLAGS(Flags, D3D12_HEAP_FLAGS);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RESOURCE_DESC& v)
{
    w.BeginObject();
    M_ENUM(Dimension, D3D12_RESOURCE_DIMENSION);
    M_UINT(Alignment);
    M_UINT(Width);
    M_UINT(Height);
    M_UINT(DepthOrArraySize);
    M_UINT(MipLevels);
    M_ENUM(Format, DXGI_FORMAT);
    M_NESTED(SampleDesc);
    M_ENUM(Layout, D3D12_TEXTURE_LAYOUT);
    M_FLAGS(Flags, D3D12_RESOURCE_FLAGS);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_MIP_REGION& v)
{
    w.BeginObject();
    M_UINT(Width);
    M_UINT(Height);
    M_UINT(Depth);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RESOURCE_DESC1& v)
{
    w.BeginObject();
    M_ENUM(Dimension, D3D12_RESOURCE_DIMENSION);
    M_UINT(Alignment);
    M_UINT(Width);
    M_UINT(Height);
    M_UINT(DepthOrArraySize);
    M_UINT(MipLevels);
    M_ENUM(Format, DXGI_FORMAT);
    M_NESTED(SampleDesc);
    M_ENUM(Layout, D3D12_TEXTURE_LAYOUT);
    M_FLAGS(Flags, D3D12_RESOURCE_FLAGS);
    M_NESTED(SamplerFeedbackMipRegion);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_VALUE& v)
{
    w.BeginObject();
    M_FLOAT(Depth);
    M_UINT(Stencil);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_CLEAR_VALUE* p, DXGI_FORMAT formatHint)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_CLEAR_VALUE& v = *p;
    // The union has no discriminator of its own: the format says which arm the application
    // filled, the resource's format standing in when the clear value's is UNKNOWN.
    DXGI_FORMAT format = v.Format != DXGI_FORMAT_UNKNOWN ? v.Format : formatHint;
    w.BeginObject();
    M_ENUM(Format, DXGI_FORMAT);
    if (IsDepthFormat(format))
    {
        M_NESTED(DepthStencil);
    }
    else
    {
        w.Key("Color");
        w.BeginArray();
        for (int i = 0; i < 4; ++i)
            w.Double(v.Color[i]);
        w.EndArray();
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RANGE* p)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_RANGE& v = *p;
    w.BeginObject();
    M_UINT(Begin);
    M_UINT(End);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_BOX* p)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_BOX& v = *p;
    w.BeginObject();
    M_UINT(left);
    M_UINT(top);
    M_UINT(front);
    M_UINT(right);
    M_UINT(bottom);
    M_UINT(back);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_SUBRESOURCE_FOOTPRINT& v)
{
    w.BeginObject();
    M_ENUM(Format, DXGI_FORMAT);
    M_UINT(Width);
    M_UINT(Height);
    M_UINT(Depth);
    M_UINT(RowPitch);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& v)
{
    w.BeginObject();
    M_UINT(Offset);
    M_NESTED(Footprint);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_TEXTURE_COPY_LOCATION& v)
{
    w.BeginObject();
    M_REF(pResource, "ID3D12Resource");
    M_ENUM(Type, D3D12_TEXTURE_COPY_TYPE);
    if (v.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
    {
        M_NESTED(PlacedFootprint);
    }
    else
    {
        M_UINT(SubresourceIndex);
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_TILED_RESOURCE_COORDINATE& v)
{
    w.BeginObject();
    M_UINT(X);
    M_UINT(Y);
    M_UINT(Z);
    M_UINT(Subresource);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_TILE_REGION_SIZE& v)
{
    w.BeginObject();
    M_UINT(NumTiles);
    M_BOOL(UseBox);
    M_UINT(Width);
    M_UINT(Height);
    M_UINT(Depth);
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Views and samplers

void Write(JsonWriter& w, const D3D12_CONSTANT_BUFFER_VIEW_DESC* p)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_CONSTANT_BUFFER_VIEW_DESC& v = *p;
    w.BeginObject();
    M_ADDRESS(BufferLocation);
    M_UINT(SizeInBytes);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_SHADER_RESOURCE_VIEW_DESC* p)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_SHADER_RESOURCE_VIEW_DESC& v = *p;
    w.BeginObject();
    M_ENUM(Format, DXGI_FORMAT);
    M_ENUM(ViewDimension, D3D12_SRV_DIMENSION);
    w.Key("Shader4ComponentMapping");
    WriteComponentMapping(w, v.Shader4ComponentMapping);
    switch (v.ViewDimension)
    {
        case D3D12_SRV_DIMENSION_BUFFER:
            w.Key("Buffer");
            w.BeginObject();
            w.Key("FirstElement");
            w.Uint(v.Buffer.FirstElement);
            w.Key("NumElements");
            w.Uint(v.Buffer.NumElements);
            w.Key("StructureByteStride");
            w.Uint(v.Buffer.StructureByteStride);
            w.Key("Flags");
            Flags_D3D12_BUFFER_SRV_FLAGS(w, v.Buffer.Flags);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1D:
            w.Key("Texture1D");
            w.BeginObject();
            w.Key("MostDetailedMip");
            w.Uint(v.Texture1D.MostDetailedMip);
            w.Key("MipLevels");
            w.Uint(v.Texture1D.MipLevels);
            w.Key("ResourceMinLODClamp");
            w.Double(v.Texture1D.ResourceMinLODClamp);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            w.Key("Texture1DArray");
            w.BeginObject();
            w.Key("MostDetailedMip");
            w.Uint(v.Texture1DArray.MostDetailedMip);
            w.Key("MipLevels");
            w.Uint(v.Texture1DArray.MipLevels);
            w.Key("FirstArraySlice");
            w.Uint(v.Texture1DArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture1DArray.ArraySize);
            w.Key("ResourceMinLODClamp");
            w.Double(v.Texture1DArray.ResourceMinLODClamp);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            w.Key("Texture2D");
            w.BeginObject();
            w.Key("MostDetailedMip");
            w.Uint(v.Texture2D.MostDetailedMip);
            w.Key("MipLevels");
            w.Uint(v.Texture2D.MipLevels);
            w.Key("PlaneSlice");
            w.Uint(v.Texture2D.PlaneSlice);
            w.Key("ResourceMinLODClamp");
            w.Double(v.Texture2D.ResourceMinLODClamp);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            w.Key("Texture2DArray");
            w.BeginObject();
            w.Key("MostDetailedMip");
            w.Uint(v.Texture2DArray.MostDetailedMip);
            w.Key("MipLevels");
            w.Uint(v.Texture2DArray.MipLevels);
            w.Key("FirstArraySlice");
            w.Uint(v.Texture2DArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture2DArray.ArraySize);
            w.Key("PlaneSlice");
            w.Uint(v.Texture2DArray.PlaneSlice);
            w.Key("ResourceMinLODClamp");
            w.Double(v.Texture2DArray.ResourceMinLODClamp);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS:
            w.Key("Texture2DMS");
            w.BeginObject();
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            w.Key("Texture2DMSArray");
            w.BeginObject();
            w.Key("FirstArraySlice");
            w.Uint(v.Texture2DMSArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture2DMSArray.ArraySize);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            w.Key("Texture3D");
            w.BeginObject();
            w.Key("MostDetailedMip");
            w.Uint(v.Texture3D.MostDetailedMip);
            w.Key("MipLevels");
            w.Uint(v.Texture3D.MipLevels);
            w.Key("ResourceMinLODClamp");
            w.Double(v.Texture3D.ResourceMinLODClamp);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            w.Key("TextureCube");
            w.BeginObject();
            w.Key("MostDetailedMip");
            w.Uint(v.TextureCube.MostDetailedMip);
            w.Key("MipLevels");
            w.Uint(v.TextureCube.MipLevels);
            w.Key("ResourceMinLODClamp");
            w.Double(v.TextureCube.ResourceMinLODClamp);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            w.Key("TextureCubeArray");
            w.BeginObject();
            w.Key("MostDetailedMip");
            w.Uint(v.TextureCubeArray.MostDetailedMip);
            w.Key("MipLevels");
            w.Uint(v.TextureCubeArray.MipLevels);
            w.Key("First2DArrayFace");
            w.Uint(v.TextureCubeArray.First2DArrayFace);
            w.Key("NumCubes");
            w.Uint(v.TextureCubeArray.NumCubes);
            w.Key("ResourceMinLODClamp");
            w.Double(v.TextureCubeArray.ResourceMinLODClamp);
            w.EndObject();
            break;
        case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE:
            w.Key("RaytracingAccelerationStructure");
            w.BeginObject();
            w.Key("Location");
            WriteGpuAddress(w, v.RaytracingAccelerationStructure.Location);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_UNORDERED_ACCESS_VIEW_DESC* p)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_UNORDERED_ACCESS_VIEW_DESC& v = *p;
    w.BeginObject();
    M_ENUM(Format, DXGI_FORMAT);
    M_ENUM(ViewDimension, D3D12_UAV_DIMENSION);
    switch (v.ViewDimension)
    {
        case D3D12_UAV_DIMENSION_BUFFER:
            w.Key("Buffer");
            w.BeginObject();
            w.Key("FirstElement");
            w.Uint(v.Buffer.FirstElement);
            w.Key("NumElements");
            w.Uint(v.Buffer.NumElements);
            w.Key("StructureByteStride");
            w.Uint(v.Buffer.StructureByteStride);
            w.Key("CounterOffsetInBytes");
            w.Uint(v.Buffer.CounterOffsetInBytes);
            w.Key("Flags");
            Flags_D3D12_BUFFER_UAV_FLAGS(w, v.Buffer.Flags);
            w.EndObject();
            break;
        case D3D12_UAV_DIMENSION_TEXTURE1D:
            w.Key("Texture1D");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture1D.MipSlice);
            w.EndObject();
            break;
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            w.Key("Texture1DArray");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture1DArray.MipSlice);
            w.Key("FirstArraySlice");
            w.Uint(v.Texture1DArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture1DArray.ArraySize);
            w.EndObject();
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2D:
            w.Key("Texture2D");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture2D.MipSlice);
            w.Key("PlaneSlice");
            w.Uint(v.Texture2D.PlaneSlice);
            w.EndObject();
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            w.Key("Texture2DArray");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture2DArray.MipSlice);
            w.Key("FirstArraySlice");
            w.Uint(v.Texture2DArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture2DArray.ArraySize);
            w.Key("PlaneSlice");
            w.Uint(v.Texture2DArray.PlaneSlice);
            w.EndObject();
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DMS:
            w.Key("Texture2DMS");
            w.BeginObject();
            w.EndObject();
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DMSARRAY:
            w.Key("Texture2DMSArray");
            w.BeginObject();
            w.Key("FirstArraySlice");
            w.Uint(v.Texture2DMSArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture2DMSArray.ArraySize);
            w.EndObject();
            break;
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            w.Key("Texture3D");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture3D.MipSlice);
            w.Key("FirstWSlice");
            w.Uint(v.Texture3D.FirstWSlice);
            w.Key("WSize");
            w.Uint(v.Texture3D.WSize);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RENDER_TARGET_VIEW_DESC* p)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_RENDER_TARGET_VIEW_DESC& v = *p;
    w.BeginObject();
    M_ENUM(Format, DXGI_FORMAT);
    M_ENUM(ViewDimension, D3D12_RTV_DIMENSION);
    switch (v.ViewDimension)
    {
        case D3D12_RTV_DIMENSION_BUFFER:
            w.Key("Buffer");
            w.BeginObject();
            w.Key("FirstElement");
            w.Uint(v.Buffer.FirstElement);
            w.Key("NumElements");
            w.Uint(v.Buffer.NumElements);
            w.EndObject();
            break;
        case D3D12_RTV_DIMENSION_TEXTURE1D:
            w.Key("Texture1D");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture1D.MipSlice);
            w.EndObject();
            break;
        case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
            w.Key("Texture1DArray");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture1DArray.MipSlice);
            w.Key("FirstArraySlice");
            w.Uint(v.Texture1DArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture1DArray.ArraySize);
            w.EndObject();
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2D:
            w.Key("Texture2D");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture2D.MipSlice);
            w.Key("PlaneSlice");
            w.Uint(v.Texture2D.PlaneSlice);
            w.EndObject();
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
            w.Key("Texture2DArray");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture2DArray.MipSlice);
            w.Key("FirstArraySlice");
            w.Uint(v.Texture2DArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture2DArray.ArraySize);
            w.Key("PlaneSlice");
            w.Uint(v.Texture2DArray.PlaneSlice);
            w.EndObject();
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMS:
            w.Key("Texture2DMS");
            w.BeginObject();
            w.EndObject();
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
            w.Key("Texture2DMSArray");
            w.BeginObject();
            w.Key("FirstArraySlice");
            w.Uint(v.Texture2DMSArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture2DMSArray.ArraySize);
            w.EndObject();
            break;
        case D3D12_RTV_DIMENSION_TEXTURE3D:
            w.Key("Texture3D");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture3D.MipSlice);
            w.Key("FirstWSlice");
            w.Uint(v.Texture3D.FirstWSlice);
            w.Key("WSize");
            w.Uint(v.Texture3D.WSize);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_VIEW_DESC* p)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_DEPTH_STENCIL_VIEW_DESC& v = *p;
    w.BeginObject();
    M_ENUM(Format, DXGI_FORMAT);
    M_ENUM(ViewDimension, D3D12_DSV_DIMENSION);
    M_FLAGS(Flags, D3D12_DSV_FLAGS);
    switch (v.ViewDimension)
    {
        case D3D12_DSV_DIMENSION_TEXTURE1D:
            w.Key("Texture1D");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture1D.MipSlice);
            w.EndObject();
            break;
        case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
            w.Key("Texture1DArray");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture1DArray.MipSlice);
            w.Key("FirstArraySlice");
            w.Uint(v.Texture1DArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture1DArray.ArraySize);
            w.EndObject();
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2D:
            w.Key("Texture2D");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture2D.MipSlice);
            w.EndObject();
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
            w.Key("Texture2DArray");
            w.BeginObject();
            w.Key("MipSlice");
            w.Uint(v.Texture2DArray.MipSlice);
            w.Key("FirstArraySlice");
            w.Uint(v.Texture2DArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture2DArray.ArraySize);
            w.EndObject();
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMS:
            w.Key("Texture2DMS");
            w.BeginObject();
            w.EndObject();
            break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
            w.Key("Texture2DMSArray");
            w.BeginObject();
            w.Key("FirstArraySlice");
            w.Uint(v.Texture2DMSArray.FirstArraySlice);
            w.Key("ArraySize");
            w.Uint(v.Texture2DMSArray.ArraySize);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_SAMPLER_DESC& v)
{
    w.BeginObject();
    M_ENUM(Filter, D3D12_FILTER);
    M_ENUM(AddressU, D3D12_TEXTURE_ADDRESS_MODE);
    M_ENUM(AddressV, D3D12_TEXTURE_ADDRESS_MODE);
    M_ENUM(AddressW, D3D12_TEXTURE_ADDRESS_MODE);
    M_FLOAT(MipLODBias);
    M_UINT(MaxAnisotropy);
    M_ENUM(ComparisonFunc, D3D12_COMPARISON_FUNC);
    w.Key("BorderColor");
    w.BeginArray();
    for (int i = 0; i < 4; ++i)
        w.Double(v.BorderColor[i]);
    w.EndArray();
    M_FLOAT(MinLOD);
    M_FLOAT(MaxLOD);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_SAMPLER_DESC2& v)
{
    w.BeginObject();
    M_ENUM(Filter, D3D12_FILTER);
    M_ENUM(AddressU, D3D12_TEXTURE_ADDRESS_MODE);
    M_ENUM(AddressV, D3D12_TEXTURE_ADDRESS_MODE);
    M_ENUM(AddressW, D3D12_TEXTURE_ADDRESS_MODE);
    M_FLOAT(MipLODBias);
    M_UINT(MaxAnisotropy);
    M_ENUM(ComparisonFunc, D3D12_COMPARISON_FUNC);
    // The border color union is discriminated by the UINT_BORDER_COLOR flag.
    if (v.Flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR)
    {
        w.Key("UintBorderColor");
        w.BeginArray();
        for (int i = 0; i < 4; ++i)
            w.Uint(v.UintBorderColor[i]);
        w.EndArray();
    }
    else
    {
        w.Key("FloatBorderColor");
        w.BeginArray();
        for (int i = 0; i < 4; ++i)
            w.Double(v.FloatBorderColor[i]);
        w.EndArray();
    }
    M_FLOAT(MinLOD);
    M_FLOAT(MaxLOD);
    M_FLAGS(Flags, D3D12_SAMPLER_FLAGS);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_VERTEX_BUFFER_VIEW& v)
{
    w.BeginObject();
    M_ADDRESS(BufferLocation);
    M_UINT(SizeInBytes);
    M_UINT(StrideInBytes);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_INDEX_BUFFER_VIEW& v)
{
    w.BeginObject();
    M_ADDRESS(BufferLocation);
    M_UINT(SizeInBytes);
    M_ENUM(Format, DXGI_FORMAT);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_STREAM_OUTPUT_BUFFER_VIEW& v)
{
    w.BeginObject();
    M_ADDRESS(BufferLocation);
    M_UINT(SizeInBytes);
    M_ADDRESS(BufferFilledSizeLocation);
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Pipelines

void Write(JsonWriter& w, const D3D12_SHADER_BYTECODE& v)
{
    // Never the bytes: they go into blobs. An absent stage is null; a present one carries its
    // size and a content hash (FNV-1a), so two pipelines sharing a shader can be told apart
    // from two with different ones of the same size.
    if (!v.pShaderBytecode || !v.BytecodeLength)
    {
        w.Null();
        return;
    }
    uint64_t hash = 14695981039346656037ull;
    const uint8_t* bytes = static_cast<const uint8_t*>(v.pShaderBytecode);
    for (size_t i = 0; i < v.BytecodeLength; ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"__bytes\":%llu,\"hash\":\"0x%016llx\"}", (unsigned long long)v.BytecodeLength,
        (unsigned long long)hash);
    w.Raw(buf);
}

void Write(JsonWriter& w, const D3D12_INPUT_ELEMENT_DESC& v)
{
    w.BeginObject();
    M_STR(SemanticName);
    M_UINT(SemanticIndex);
    M_ENUM(Format, DXGI_FORMAT);
    M_UINT(InputSlot);
    M_UINT(AlignedByteOffset);
    M_ENUM(InputSlotClass, D3D12_INPUT_CLASSIFICATION);
    M_UINT(InstanceDataStepRate);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_INPUT_LAYOUT_DESC& v)
{
    w.BeginObject();
    w.Key("pInputElementDescs");
    WriteArray(w, v.NumElements, v.pInputElementDescs);
    M_UINT(NumElements);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_SO_DECLARATION_ENTRY& v)
{
    w.BeginObject();
    M_UINT(Stream);
    M_STR(SemanticName);
    M_UINT(SemanticIndex);
    M_UINT(StartComponent);
    M_UINT(ComponentCount);
    M_UINT(OutputSlot);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_STREAM_OUTPUT_DESC& v)
{
    w.BeginObject();
    w.Key("pSODeclaration");
    WriteArray(w, v.NumEntries, v.pSODeclaration);
    M_UINT(NumEntries);
    w.Key("pBufferStrides");
    WriteArray(w, v.NumStrides, v.pBufferStrides, [&](UINT s) { w.Uint(s); });
    M_UINT(NumStrides);
    M_UINT(RasterizedStream);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RENDER_TARGET_BLEND_DESC& v)
{
    w.BeginObject();
    M_BOOL(BlendEnable);
    M_BOOL(LogicOpEnable);
    M_ENUM(SrcBlend, D3D12_BLEND);
    M_ENUM(DestBlend, D3D12_BLEND);
    M_ENUM(BlendOp, D3D12_BLEND_OP);
    M_ENUM(SrcBlendAlpha, D3D12_BLEND);
    M_ENUM(DestBlendAlpha, D3D12_BLEND);
    M_ENUM(BlendOpAlpha, D3D12_BLEND_OP);
    M_ENUM(LogicOp, D3D12_LOGIC_OP);
    w.Key("RenderTargetWriteMask");
    WriteFlags(w, kEnum_D3D12_COLOR_WRITE_ENABLE, 5, v.RenderTargetWriteMask);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_BLEND_DESC& v)
{
    w.BeginObject();
    M_BOOL(AlphaToCoverageEnable);
    M_BOOL(IndependentBlendEnable);
    w.Key("RenderTarget");
    w.BeginArray();
    for (int i = 0; i < 8; ++i)
        Write(w, v.RenderTarget[i]);
    w.EndArray();
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RASTERIZER_DESC& v)
{
    w.BeginObject();
    M_ENUM(FillMode, D3D12_FILL_MODE);
    M_ENUM(CullMode, D3D12_CULL_MODE);
    M_BOOL(FrontCounterClockwise);
    M_INT(DepthBias);
    M_FLOAT(DepthBiasClamp);
    M_FLOAT(SlopeScaledDepthBias);
    M_BOOL(DepthClipEnable);
    M_BOOL(MultisampleEnable);
    M_BOOL(AntialiasedLineEnable);
    M_UINT(ForcedSampleCount);
    M_ENUM(ConservativeRaster, D3D12_CONSERVATIVE_RASTERIZATION_MODE);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RASTERIZER_DESC1& v)
{
    w.BeginObject();
    M_ENUM(FillMode, D3D12_FILL_MODE);
    M_ENUM(CullMode, D3D12_CULL_MODE);
    M_BOOL(FrontCounterClockwise);
    M_FLOAT(DepthBias);
    M_FLOAT(DepthBiasClamp);
    M_FLOAT(SlopeScaledDepthBias);
    M_BOOL(DepthClipEnable);
    M_BOOL(MultisampleEnable);
    M_BOOL(AntialiasedLineEnable);
    M_UINT(ForcedSampleCount);
    M_ENUM(ConservativeRaster, D3D12_CONSERVATIVE_RASTERIZATION_MODE);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RASTERIZER_DESC2& v)
{
    w.BeginObject();
    M_ENUM(FillMode, D3D12_FILL_MODE);
    M_ENUM(CullMode, D3D12_CULL_MODE);
    M_BOOL(FrontCounterClockwise);
    M_FLOAT(DepthBias);
    M_FLOAT(DepthBiasClamp);
    M_FLOAT(SlopeScaledDepthBias);
    M_BOOL(DepthClipEnable);
    M_ENUM(LineRasterizationMode, D3D12_LINE_RASTERIZATION_MODE);
    M_UINT(ForcedSampleCount);
    M_ENUM(ConservativeRaster, D3D12_CONSERVATIVE_RASTERIZATION_MODE);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DEPTH_STENCILOP_DESC& v)
{
    w.BeginObject();
    M_ENUM(StencilFailOp, D3D12_STENCIL_OP);
    M_ENUM(StencilDepthFailOp, D3D12_STENCIL_OP);
    M_ENUM(StencilPassOp, D3D12_STENCIL_OP);
    M_ENUM(StencilFunc, D3D12_COMPARISON_FUNC);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DEPTH_STENCILOP_DESC1& v)
{
    w.BeginObject();
    M_ENUM(StencilFailOp, D3D12_STENCIL_OP);
    M_ENUM(StencilDepthFailOp, D3D12_STENCIL_OP);
    M_ENUM(StencilPassOp, D3D12_STENCIL_OP);
    M_ENUM(StencilFunc, D3D12_COMPARISON_FUNC);
    M_UINT(StencilReadMask);
    M_UINT(StencilWriteMask);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_DESC& v)
{
    w.BeginObject();
    M_BOOL(DepthEnable);
    M_ENUM(DepthWriteMask, D3D12_DEPTH_WRITE_MASK);
    M_ENUM(DepthFunc, D3D12_COMPARISON_FUNC);
    M_BOOL(StencilEnable);
    M_UINT(StencilReadMask);
    M_UINT(StencilWriteMask);
    M_NESTED(FrontFace);
    M_NESTED(BackFace);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_DESC1& v)
{
    w.BeginObject();
    M_BOOL(DepthEnable);
    M_ENUM(DepthWriteMask, D3D12_DEPTH_WRITE_MASK);
    M_ENUM(DepthFunc, D3D12_COMPARISON_FUNC);
    M_BOOL(StencilEnable);
    M_UINT(StencilReadMask);
    M_UINT(StencilWriteMask);
    M_NESTED(FrontFace);
    M_NESTED(BackFace);
    M_BOOL(DepthBoundsTestEnable);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_DESC2& v)
{
    w.BeginObject();
    M_BOOL(DepthEnable);
    M_ENUM(DepthWriteMask, D3D12_DEPTH_WRITE_MASK);
    M_ENUM(DepthFunc, D3D12_COMPARISON_FUNC);
    M_BOOL(StencilEnable);
    M_NESTED(FrontFace);
    M_NESTED(BackFace);
    M_BOOL(DepthBoundsTestEnable);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RT_FORMAT_ARRAY& v)
{
    w.BeginObject();
    w.Key("RTFormats");
    WriteFormats(w, v.RTFormats, v.NumRenderTargets < 8 ? v.NumRenderTargets : 8);
    M_UINT(NumRenderTargets);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_VIEW_INSTANCE_LOCATION& v)
{
    w.BeginObject();
    M_UINT(ViewportArrayIndex);
    M_UINT(RenderTargetArrayIndex);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_VIEW_INSTANCING_DESC& v)
{
    w.BeginObject();
    M_UINT(ViewInstanceCount);
    w.Key("pViewInstanceLocations");
    WriteArray(w, v.ViewInstanceCount, v.pViewInstanceLocations);
    M_FLAGS(Flags, D3D12_VIEW_INSTANCING_FLAGS);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_CACHED_PIPELINE_STATE& v)
{
    w.BeginObject();
    w.Key("pCachedBlob");
    if (v.pCachedBlob && v.CachedBlobSizeInBytes)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"__bytes\":%llu}", (unsigned long long)v.CachedBlobSizeInBytes);
        w.Raw(buf);
    }
    else
    {
        w.Null();
    }
    M_UINT(CachedBlobSizeInBytes);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& v)
{
    w.BeginObject();
    M_REF(pRootSignature, "ID3D12RootSignature");
    M_NESTED(VS);
    M_NESTED(PS);
    M_NESTED(DS);
    M_NESTED(HS);
    M_NESTED(GS);
    M_NESTED(StreamOutput);
    M_NESTED(BlendState);
    M_UINT(SampleMask);
    M_NESTED(RasterizerState);
    M_NESTED(DepthStencilState);
    M_NESTED(InputLayout);
    M_ENUM(IBStripCutValue, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
    M_ENUM(PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE);
    M_UINT(NumRenderTargets);
    w.Key("RTVFormats");
    WriteFormats(w, v.RTVFormats, v.NumRenderTargets < 8 ? v.NumRenderTargets : 8);
    M_ENUM(DSVFormat, DXGI_FORMAT);
    M_NESTED(SampleDesc);
    M_UINT(NodeMask);
    M_NESTED(CachedPSO);
    M_FLAGS(Flags, D3D12_PIPELINE_STATE_FLAGS);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_COMPUTE_PIPELINE_STATE_DESC& v)
{
    w.BeginObject();
    M_REF(pRootSignature, "ID3D12RootSignature");
    M_NESTED(CS);
    M_UINT(NodeMask);
    M_NESTED(CachedPSO);
    M_FLAGS(Flags, D3D12_PIPELINE_STATE_FLAGS);
    w.EndObject();
}

/**
 * The payload of the subobject at `p`, advancing `p` to the next one. The layout is CD3DX12's
 * (what the runtime parses): a D3D12_PIPELINE_STATE_SUBOBJECT_TYPE, then the payload at its own
 * natural alignment (a UINT sits right after the type, a struct holding a pointer eight bytes
 * in), and the whole subobject rounded up to pointer alignment. Null when the payload would
 * run past the end of the stream.
 */
template <typename T>
static const T* StreamPayload(const uint8_t*& p, const uint8_t* end)
{
    uintptr_t q = (uintptr_t)p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE);
    q = (q + alignof(T) - 1) & ~(uintptr_t)(alignof(T) - 1);
    if (q + sizeof(T) > (uintptr_t)end)
        return nullptr;
    const T* payload = reinterpret_cast<const T*>(q);
    q += sizeof(T);
    q = (q + sizeof(void*) - 1) & ~(uintptr_t)(sizeof(void*) - 1);
    p = reinterpret_cast<const uint8_t*>(q);
    return payload;
}

void Write(JsonWriter& w, const D3D12_PIPELINE_STATE_STREAM_DESC& v)
{
    w.BeginObject();
    M_UINT(SizeInBytes);
    const uint8_t* p = static_cast<const uint8_t*>(v.pPipelineStateSubobjectStream);
    const uint8_t* end = p ? p + v.SizeInBytes : nullptr;
    std::vector<std::string> types;
    // Each subobject under the member name the graphics/compute descs use, so the UI reads a
    // streamed pipeline the way it reads one made from a desc.
#define STREAM_SUBOBJECT(T, key, expr)            \
    {                                             \
        const auto* s = StreamPayload<T>(p, end); \
        if (!s)                                   \
        {                                         \
            p = end;                              \
            break;                                \
        }                                         \
        w.Key(key);                               \
        expr;                                     \
        break;                                    \
    }
    while (p && p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= end)
    {
        auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
        const char* name = ToString_D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(EnumValue(type));
        types.push_back(name ? std::string(name) : std::to_string((int)type));
        switch (type)
        {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                STREAM_SUBOBJECT(ID3D12RootSignature*, "pRootSignature", WriteRef(w, *s, "ID3D12RootSignature"));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: STREAM_SUBOBJECT(D3D12_SHADER_BYTECODE, "VS", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: STREAM_SUBOBJECT(D3D12_SHADER_BYTECODE, "PS", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: STREAM_SUBOBJECT(D3D12_SHADER_BYTECODE, "DS", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: STREAM_SUBOBJECT(D3D12_SHADER_BYTECODE, "HS", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: STREAM_SUBOBJECT(D3D12_SHADER_BYTECODE, "GS", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: STREAM_SUBOBJECT(D3D12_SHADER_BYTECODE, "CS", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: STREAM_SUBOBJECT(D3D12_SHADER_BYTECODE, "AS", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: STREAM_SUBOBJECT(D3D12_SHADER_BYTECODE, "MS", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
                STREAM_SUBOBJECT(D3D12_STREAM_OUTPUT_DESC, "StreamOutput", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: STREAM_SUBOBJECT(D3D12_BLEND_DESC, "BlendState", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: STREAM_SUBOBJECT(UINT, "SampleMask", w.Uint(*s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
                STREAM_SUBOBJECT(D3D12_RASTERIZER_DESC, "RasterizerState", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
                STREAM_SUBOBJECT(D3D12_RASTERIZER_DESC1, "RasterizerState", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
                STREAM_SUBOBJECT(D3D12_RASTERIZER_DESC2, "RasterizerState", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
                STREAM_SUBOBJECT(D3D12_DEPTH_STENCIL_DESC, "DepthStencilState", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
                STREAM_SUBOBJECT(D3D12_DEPTH_STENCIL_DESC1, "DepthStencilState", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
                STREAM_SUBOBJECT(D3D12_DEPTH_STENCIL_DESC2, "DepthStencilState", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
                STREAM_SUBOBJECT(D3D12_INPUT_LAYOUT_DESC, "InputLayout", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
                STREAM_SUBOBJECT(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, "IBStripCutValue",
                    w.Enum(ToString_D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(EnumValue(*s)), EnumValue(*s)));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
                STREAM_SUBOBJECT(D3D12_PRIMITIVE_TOPOLOGY_TYPE, "PrimitiveTopologyType",
                    w.Enum(ToString_D3D12_PRIMITIVE_TOPOLOGY_TYPE(EnumValue(*s)), EnumValue(*s)));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
            {
                const D3D12_RT_FORMAT_ARRAY* s = StreamPayload<D3D12_RT_FORMAT_ARRAY>(p, end);
                if (!s)
                {
                    p = end;
                    break;
                }
                w.Key("NumRenderTargets");
                w.Uint(s->NumRenderTargets);
                w.Key("RTVFormats");
                WriteFormats(w, s->RTFormats, s->NumRenderTargets < 8 ? s->NumRenderTargets : 8);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
                STREAM_SUBOBJECT(DXGI_FORMAT, "DSVFormat", w.Enum(ToString_DXGI_FORMAT(EnumValue(*s)), EnumValue(*s)));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
                STREAM_SUBOBJECT(DXGI_SAMPLE_DESC, "SampleDesc", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: STREAM_SUBOBJECT(UINT, "NodeMask", w.Uint(*s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
                STREAM_SUBOBJECT(D3D12_CACHED_PIPELINE_STATE, "CachedPSO", Write(w, *s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
                STREAM_SUBOBJECT(D3D12_PIPELINE_STATE_FLAGS, "Flags", Flags_D3D12_PIPELINE_STATE_FLAGS(w, (uint64_t)*s));
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
                STREAM_SUBOBJECT(D3D12_VIEW_INSTANCING_DESC, "ViewInstancing", Write(w, *s));
            default:
                // A type this build does not know has an unknown size: nothing after it can be
                // located, so the walk stops here (the type stays listed in "subobjects").
                p = end;
                break;
        }
    }
#undef STREAM_SUBOBJECT
    w.Key("subobjects");
    w.BeginArray();
    for (const std::string& t : types)
        w.String(t);
    w.EndArray();
    w.EndObject();
}

// --- State objects ----------------------------------------------------------------------------

static void Write(JsonWriter& w, const D3D12_EXPORT_DESC& v)
{
    w.BeginObject();
    M_WSTR(Name);
    M_WSTR(ExportToRename);
    M_FLAGS(Flags, D3D12_EXPORT_FLAGS);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_NODE_ID& v)
{
    w.BeginObject();
    M_WSTR(Name);
    M_UINT(ArrayIndex);
    w.EndObject();
}

/** A pointer to another subobject of the same description, as its index there (and its type). */
static void WriteSubobjectPointer(JsonWriter& w, const D3D12_STATE_OBJECT_DESC& desc, const D3D12_STATE_SUBOBJECT* s)
{
    if (!s)
    {
        w.Null();
        return;
    }
    w.BeginObject();
    if (desc.pSubobjects && s >= desc.pSubobjects && s < desc.pSubobjects + desc.NumSubobjects)
    {
        w.Key("index");
        w.Uint((uint64_t)(s - desc.pSubobjects));
    }
    w.Key("Type");
    w.Enum(ToString_D3D12_STATE_SUBOBJECT_TYPE(EnumValue(s->Type)), EnumValue(s->Type));
    w.EndObject();
}

/** The overrides every launch kind shares (they head all four override structs). */
static void WriteNodeOverrides(JsonWriter& w, const UINT* localRootArgumentsTableIndex, const BOOL* programEntry,
    const D3D12_NODE_ID* newName, const D3D12_NODE_ID* shareInputOf, UINT numOutputOverrides,
    const D3D12_NODE_OUTPUT_OVERRIDES* outputOverrides, const UINT* dispatchGrid,
    const UINT* maxDispatchGrid)
{
    w.Key("pLocalRootArgumentsTableIndex");
    if (localRootArgumentsTableIndex)
        w.Uint(*localRootArgumentsTableIndex);
    else
        w.Null();
    w.Key("pProgramEntry");
    if (programEntry)
        w.Boolean(*programEntry != 0);
    else
        w.Null();
    w.Key("pNewName");
    if (newName)
        Write(w, *newName);
    else
        w.Null();
    w.Key("pShareInputOf");
    if (shareInputOf)
        Write(w, *shareInputOf);
    else
        w.Null();
    if (dispatchGrid)
    {
        w.Key("pDispatchGrid");
        w.BeginArray();
        for (int i = 0; i < 3; ++i)
            w.Uint(dispatchGrid[i]);
        w.EndArray();
    }
    if (maxDispatchGrid)
    {
        w.Key("pMaxDispatchGrid");
        w.BeginArray();
        for (int i = 0; i < 3; ++i)
            w.Uint(maxDispatchGrid[i]);
        w.EndArray();
    }
    w.Key("NumOutputOverrides");
    w.Uint(numOutputOverrides);
    w.Key("pOutputOverrides");
    WriteArray(w, numOutputOverrides, outputOverrides, [&](const D3D12_NODE_OUTPUT_OVERRIDES& o) {
        w.BeginObject();
        w.Key("OutputIndex");
        w.Uint(o.OutputIndex);
        w.Key("pNewName");
        if (o.pNewName)
            Write(w, *o.pNewName);
        else
            w.Null();
        w.Key("pAllowSparseNodes");
        if (o.pAllowSparseNodes)
            w.Boolean(*o.pAllowSparseNodes != 0);
        else
            w.Null();
        w.Key("pMaxRecords");
        if (o.pMaxRecords)
            w.Uint(*o.pMaxRecords);
        else
            w.Null();
        w.Key("pMaxRecordsSharedWithOutputIndex");
        if (o.pMaxRecordsSharedWithOutputIndex)
            w.Uint(*o.pMaxRecordsSharedWithOutputIndex);
        else
            w.Null();
        w.EndObject();
    });
}

static void Write(JsonWriter& w, const D3D12_NODE& v)
{
    w.BeginObject();
    M_ENUM(NodeType, D3D12_NODE_TYPE);
    if (v.NodeType == D3D12_NODE_TYPE_SHADER)
    {
        w.Key("Shader");
        w.BeginObject();
        w.Key("Shader");
        WriteWide(w, v.Shader.Shader);
        w.Key("OverridesType");
        w.Enum(ToString_D3D12_NODE_OVERRIDES_TYPE(EnumValue(v.Shader.OverridesType)), EnumValue(v.Shader.OverridesType));
        switch (v.Shader.OverridesType)
        {
            case D3D12_NODE_OVERRIDES_TYPE_BROADCASTING_LAUNCH:
                if (const auto* o = v.Shader.pBroadcastingLaunchOverrides)
                {
                    w.Key("pBroadcastingLaunchOverrides");
                    w.BeginObject();
                    WriteNodeOverrides(w, o->pLocalRootArgumentsTableIndex, o->pProgramEntry, o->pNewName, o->pShareInputOf,
                        o->NumOutputOverrides, o->pOutputOverrides, o->pDispatchGrid, o->pMaxDispatchGrid);
                    w.EndObject();
                }
                break;
            case D3D12_NODE_OVERRIDES_TYPE_COALESCING_LAUNCH:
                if (const auto* o = v.Shader.pCoalescingLaunchOverrides)
                {
                    w.Key("pCoalescingLaunchOverrides");
                    w.BeginObject();
                    WriteNodeOverrides(w, o->pLocalRootArgumentsTableIndex, o->pProgramEntry, o->pNewName, o->pShareInputOf,
                        o->NumOutputOverrides, o->pOutputOverrides, nullptr, nullptr);
                    w.EndObject();
                }
                break;
            case D3D12_NODE_OVERRIDES_TYPE_THREAD_LAUNCH:
                if (const auto* o = v.Shader.pThreadLaunchOverrides)
                {
                    w.Key("pThreadLaunchOverrides");
                    w.BeginObject();
                    WriteNodeOverrides(w, o->pLocalRootArgumentsTableIndex, o->pProgramEntry, o->pNewName, o->pShareInputOf,
                        o->NumOutputOverrides, o->pOutputOverrides, nullptr, nullptr);
                    w.EndObject();
                }
                break;
            case D3D12_NODE_OVERRIDES_TYPE_COMMON_COMPUTE:
                if (const auto* o = v.Shader.pCommonComputeNodeOverrides)
                {
                    w.Key("pCommonComputeNodeOverrides");
                    w.BeginObject();
                    WriteNodeOverrides(w, o->pLocalRootArgumentsTableIndex, o->pProgramEntry, o->pNewName, o->pShareInputOf,
                        o->NumOutputOverrides, o->pOutputOverrides, nullptr, nullptr);
                    w.EndObject();
                }
                break;
            default:
                break;
        }
        w.EndObject();
    }
    w.EndObject();
}

/** One subobject of a state object: its type and the members of the description it points to. */
static void WriteStateSubobject(JsonWriter& w, const D3D12_STATE_OBJECT_DESC& desc, const D3D12_STATE_SUBOBJECT& s)
{
    w.BeginObject();
    w.Key("Type");
    w.Enum(ToString_D3D12_STATE_SUBOBJECT_TYPE(EnumValue(s.Type)), EnumValue(s.Type));
    if (!s.pDesc)
    {
        w.Key("pDesc");
        w.Null();
        w.EndObject();
        return;
    }
    switch (s.Type)
    {
        case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        {
            const auto& v = *static_cast<const D3D12_STATE_OBJECT_CONFIG*>(s.pDesc);
            M_FLAGS(Flags, D3D12_STATE_OBJECT_FLAGS);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
        {
            const auto& v = *static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(s.pDesc);
            M_REF(pGlobalRootSignature, "ID3D12RootSignature");
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
        {
            const auto& v = *static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(s.pDesc);
            M_REF(pLocalRootSignature, "ID3D12RootSignature");
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
        {
            const auto& v = *static_cast<const D3D12_NODE_MASK*>(s.pDesc);
            M_UINT(NodeMask);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
        {
            const auto& v = *static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s.pDesc);
            M_NESTED(DXILLibrary);
            M_UINT(NumExports);
            w.Key("pExports");
            WriteArray(w, v.NumExports, v.pExports);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
        {
            const auto& v = *static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc);
            M_REF(pExistingCollection, "ID3D12StateObject");
            M_UINT(NumExports);
            w.Key("pExports");
            WriteArray(w, v.NumExports, v.pExports);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            const auto& v = *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s.pDesc);
            w.Key("pSubobjectToAssociate");
            WriteSubobjectPointer(w, desc, v.pSubobjectToAssociate);
            M_UINT(NumExports);
            w.Key("pExports");
            WriteWideArray(w, v.NumExports, v.pExports);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            const auto& v = *static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s.pDesc);
            M_WSTR(SubobjectToAssociate);
            M_UINT(NumExports);
            w.Key("pExports");
            WriteWideArray(w, v.NumExports, v.pExports);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
        {
            const auto& v = *static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(s.pDesc);
            M_UINT(MaxPayloadSizeInBytes);
            M_UINT(MaxAttributeSizeInBytes);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
        {
            const auto& v = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(s.pDesc);
            M_UINT(MaxTraceRecursionDepth);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
        {
            const auto& v = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1*>(s.pDesc);
            M_UINT(MaxTraceRecursionDepth);
            M_FLAGS(Flags, D3D12_RAYTRACING_PIPELINE_FLAGS);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
        {
            const auto& v = *static_cast<const D3D12_HIT_GROUP_DESC*>(s.pDesc);
            M_WSTR(HitGroupExport);
            // The desc's own `Type` would collide with the subobject's, so it is the hit group's.
            w.Key("HitGroupType");
            w.Enum(ToString_D3D12_HIT_GROUP_TYPE(EnumValue(v.Type)), EnumValue(v.Type));
            M_WSTR(AnyHitShaderImport);
            M_WSTR(ClosestHitShaderImport);
            M_WSTR(IntersectionShaderImport);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH:
        {
            const auto& v = *static_cast<const D3D12_WORK_GRAPH_DESC*>(s.pDesc);
            M_WSTR(ProgramName);
            M_FLAGS(Flags, D3D12_WORK_GRAPH_FLAGS);
            M_UINT(NumEntrypoints);
            w.Key("pEntrypoints");
            WriteArray(w, v.NumEntrypoints, v.pEntrypoints);
            M_UINT(NumExplicitlyDefinedNodes);
            w.Key("pExplicitlyDefinedNodes");
            WriteArray(w, v.NumExplicitlyDefinedNodes, v.pExplicitlyDefinedNodes);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_GENERIC_PROGRAM:
        {
            const auto& v = *static_cast<const D3D12_GENERIC_PROGRAM_DESC*>(s.pDesc);
            M_WSTR(ProgramName);
            M_UINT(NumExports);
            w.Key("pExports");
            WriteWideArray(w, v.NumExports, v.pExports);
            M_UINT(NumSubobjects);
            w.Key("ppSubobjects");
            WriteArray(w, v.NumSubobjects, v.ppSubobjects, [&](const D3D12_STATE_SUBOBJECT* sub) { WriteSubobjectPointer(w, desc, sub); });
            break;
        }
        // The pipeline-state subobjects a generic program is assembled from, under the member
        // names of D3D12_GRAPHICS_PIPELINE_STATE_DESC (the types d3dx12_state_object.h uses).
        case D3D12_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            w.Key("StreamOutput");
            Write(w, *static_cast<const D3D12_STREAM_OUTPUT_DESC*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_BLEND:
            w.Key("BlendState");
            Write(w, *static_cast<const D3D12_BLEND_DESC*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            w.Key("SampleMask");
            w.Uint(*static_cast<const UINT*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_RASTERIZER:
            w.Key("RasterizerState");
            Write(w, *static_cast<const D3D12_RASTERIZER_DESC2*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            w.Key("DepthStencilState");
            Write(w, *static_cast<const D3D12_DEPTH_STENCIL_DESC*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            w.Key("DepthStencilState");
            Write(w, *static_cast<const D3D12_DEPTH_STENCIL_DESC1*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
            w.Key("DepthStencilState");
            Write(w, *static_cast<const D3D12_DEPTH_STENCIL_DESC2*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            w.Key("InputLayout");
            Write(w, *static_cast<const D3D12_INPUT_LAYOUT_DESC*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        {
            auto value = *static_cast<const D3D12_INDEX_BUFFER_STRIP_CUT_VALUE*>(s.pDesc);
            w.Key("IBStripCutValue");
            w.Enum(ToString_D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(EnumValue(value)), EnumValue(value));
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        {
            auto value = *static_cast<const D3D12_PRIMITIVE_TOPOLOGY_TYPE*>(s.pDesc);
            w.Key("PrimitiveTopologyType");
            w.Enum(ToString_D3D12_PRIMITIVE_TOPOLOGY_TYPE(EnumValue(value)), EnumValue(value));
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        {
            const auto& v = *static_cast<const D3D12_RT_FORMAT_ARRAY*>(s.pDesc);
            M_UINT(NumRenderTargets);
            w.Key("RTVFormats");
            WriteFormats(w, v.RTFormats, v.NumRenderTargets < 8 ? v.NumRenderTargets : 8);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        {
            auto value = *static_cast<const DXGI_FORMAT*>(s.pDesc);
            w.Key("DSVFormat");
            w.Enum(ToString_DXGI_FORMAT(EnumValue(value)), EnumValue(value));
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            w.Key("SampleDesc");
            Write(w, *static_cast<const DXGI_SAMPLE_DESC*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_FLAGS:
            w.Key("Flags");
            Flags_D3D12_PIPELINE_STATE_FLAGS(w, (uint64_t) * static_cast<const D3D12_PIPELINE_STATE_FLAGS*>(s.pDesc));
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            w.Key("ViewInstancing");
            Write(w, *static_cast<const D3D12_VIEW_INSTANCING_DESC*>(s.pDesc));
            break;
        default:
            // A type this build does not know: its description cannot be read safely.
            w.Key("pDesc");
            w.Pointer(s.pDesc);
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_STATE_OBJECT_DESC& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_STATE_OBJECT_TYPE);
    M_UINT(NumSubobjects);
    w.Key("pSubobjects");
    WriteArray(w, v.NumSubobjects, v.pSubobjects, [&](const D3D12_STATE_SUBOBJECT& s) { WriteStateSubobject(w, v, s); });
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Root signatures and descriptor heaps

static void Write(JsonWriter& w, const D3D12_DESCRIPTOR_RANGE& v)
{
    w.BeginObject();
    M_ENUM(RangeType, D3D12_DESCRIPTOR_RANGE_TYPE);
    M_UINT(NumDescriptors);
    M_UINT(BaseShaderRegister);
    M_UINT(RegisterSpace);
    M_UINT(OffsetInDescriptorsFromTableStart);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_DESCRIPTOR_RANGE1& v)
{
    w.BeginObject();
    M_ENUM(RangeType, D3D12_DESCRIPTOR_RANGE_TYPE);
    M_UINT(NumDescriptors);
    M_UINT(BaseShaderRegister);
    M_UINT(RegisterSpace);
    M_FLAGS(Flags, D3D12_DESCRIPTOR_RANGE_FLAGS);
    M_UINT(OffsetInDescriptorsFromTableStart);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_ROOT_PARAMETER& v)
{
    w.BeginObject();
    M_ENUM(ParameterType, D3D12_ROOT_PARAMETER_TYPE);
    switch (v.ParameterType)
    {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            w.Key("DescriptorTable");
            w.BeginObject();
            w.Key("NumDescriptorRanges");
            w.Uint(v.DescriptorTable.NumDescriptorRanges);
            w.Key("pDescriptorRanges");
            WriteArray(w, v.DescriptorTable.NumDescriptorRanges, v.DescriptorTable.pDescriptorRanges);
            w.EndObject();
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            w.Key("Constants");
            w.BeginObject();
            w.Key("ShaderRegister");
            w.Uint(v.Constants.ShaderRegister);
            w.Key("RegisterSpace");
            w.Uint(v.Constants.RegisterSpace);
            w.Key("Num32BitValues");
            w.Uint(v.Constants.Num32BitValues);
            w.EndObject();
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            w.Key("Descriptor");
            w.BeginObject();
            w.Key("ShaderRegister");
            w.Uint(v.Descriptor.ShaderRegister);
            w.Key("RegisterSpace");
            w.Uint(v.Descriptor.RegisterSpace);
            w.EndObject();
            break;
        default:
            break;
    }
    M_ENUM(ShaderVisibility, D3D12_SHADER_VISIBILITY);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_ROOT_PARAMETER1& v)
{
    w.BeginObject();
    M_ENUM(ParameterType, D3D12_ROOT_PARAMETER_TYPE);
    switch (v.ParameterType)
    {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            w.Key("DescriptorTable");
            w.BeginObject();
            w.Key("NumDescriptorRanges");
            w.Uint(v.DescriptorTable.NumDescriptorRanges);
            w.Key("pDescriptorRanges");
            WriteArray(w, v.DescriptorTable.NumDescriptorRanges, v.DescriptorTable.pDescriptorRanges);
            w.EndObject();
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            w.Key("Constants");
            w.BeginObject();
            w.Key("ShaderRegister");
            w.Uint(v.Constants.ShaderRegister);
            w.Key("RegisterSpace");
            w.Uint(v.Constants.RegisterSpace);
            w.Key("Num32BitValues");
            w.Uint(v.Constants.Num32BitValues);
            w.EndObject();
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            w.Key("Descriptor");
            w.BeginObject();
            w.Key("ShaderRegister");
            w.Uint(v.Descriptor.ShaderRegister);
            w.Key("RegisterSpace");
            w.Uint(v.Descriptor.RegisterSpace);
            w.Key("Flags");
            Flags_D3D12_ROOT_DESCRIPTOR_FLAGS(w, (uint64_t)v.Descriptor.Flags);
            w.EndObject();
            break;
        default:
            break;
    }
    M_ENUM(ShaderVisibility, D3D12_SHADER_VISIBILITY);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_STATIC_SAMPLER_DESC& v)
{
    w.BeginObject();
    M_ENUM(Filter, D3D12_FILTER);
    M_ENUM(AddressU, D3D12_TEXTURE_ADDRESS_MODE);
    M_ENUM(AddressV, D3D12_TEXTURE_ADDRESS_MODE);
    M_ENUM(AddressW, D3D12_TEXTURE_ADDRESS_MODE);
    M_FLOAT(MipLODBias);
    M_UINT(MaxAnisotropy);
    M_ENUM(ComparisonFunc, D3D12_COMPARISON_FUNC);
    M_ENUM(BorderColor, D3D12_STATIC_BORDER_COLOR);
    M_FLOAT(MinLOD);
    M_FLOAT(MaxLOD);
    M_UINT(ShaderRegister);
    M_UINT(RegisterSpace);
    M_ENUM(ShaderVisibility, D3D12_SHADER_VISIBILITY);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_STATIC_SAMPLER_DESC1& v)
{
    w.BeginObject();
    M_ENUM(Filter, D3D12_FILTER);
    M_ENUM(AddressU, D3D12_TEXTURE_ADDRESS_MODE);
    M_ENUM(AddressV, D3D12_TEXTURE_ADDRESS_MODE);
    M_ENUM(AddressW, D3D12_TEXTURE_ADDRESS_MODE);
    M_FLOAT(MipLODBias);
    M_UINT(MaxAnisotropy);
    M_ENUM(ComparisonFunc, D3D12_COMPARISON_FUNC);
    M_ENUM(BorderColor, D3D12_STATIC_BORDER_COLOR);
    M_FLOAT(MinLOD);
    M_FLOAT(MaxLOD);
    M_UINT(ShaderRegister);
    M_UINT(RegisterSpace);
    M_ENUM(ShaderVisibility, D3D12_SHADER_VISIBILITY);
    M_FLAGS(Flags, D3D12_SAMPLER_FLAGS);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_ROOT_SIGNATURE_DESC& v)
{
    w.BeginObject();
    M_UINT(NumParameters);
    w.Key("pParameters");
    WriteArray(w, v.NumParameters, v.pParameters);
    M_UINT(NumStaticSamplers);
    w.Key("pStaticSamplers");
    WriteArray(w, v.NumStaticSamplers, v.pStaticSamplers);
    M_FLAGS(Flags, D3D12_ROOT_SIGNATURE_FLAGS);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_ROOT_SIGNATURE_DESC1& v)
{
    w.BeginObject();
    M_UINT(NumParameters);
    w.Key("pParameters");
    WriteArray(w, v.NumParameters, v.pParameters);
    M_UINT(NumStaticSamplers);
    w.Key("pStaticSamplers");
    WriteArray(w, v.NumStaticSamplers, v.pStaticSamplers);
    M_FLAGS(Flags, D3D12_ROOT_SIGNATURE_FLAGS);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_ROOT_SIGNATURE_DESC2& v)
{
    w.BeginObject();
    M_UINT(NumParameters);
    w.Key("pParameters");
    WriteArray(w, v.NumParameters, v.pParameters);
    M_UINT(NumStaticSamplers);
    w.Key("pStaticSamplers");
    WriteArray(w, v.NumStaticSamplers, v.pStaticSamplers);
    M_FLAGS(Flags, D3D12_ROOT_SIGNATURE_FLAGS);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& v)
{
    w.BeginObject();
    M_ENUM(Version, D3D_ROOT_SIGNATURE_VERSION);
    switch (v.Version)
    {
        case D3D_ROOT_SIGNATURE_VERSION_1_0: M_NESTED(Desc_1_0); break;
        case D3D_ROOT_SIGNATURE_VERSION_1_1: M_NESTED(Desc_1_1); break;
        case D3D_ROOT_SIGNATURE_VERSION_1_2: M_NESTED(Desc_1_2); break;
        default: break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DESCRIPTOR_HEAP_DESC& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_DESCRIPTOR_HEAP_TYPE);
    M_UINT(NumDescriptors);
    M_FLAGS(Flags, D3D12_DESCRIPTOR_HEAP_FLAGS);
    M_UINT(NodeMask);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_COMMAND_QUEUE_DESC& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_COMMAND_LIST_TYPE);
    // Priority is an INT holding a D3D12_COMMAND_QUEUE_PRIORITY value: by name when it is one.
    M_ENUM(Priority, D3D12_COMMAND_QUEUE_PRIORITY);
    M_FLAGS(Flags, D3D12_COMMAND_QUEUE_FLAGS);
    M_UINT(NodeMask);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_QUERY_HEAP_DESC& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_QUERY_HEAP_TYPE);
    M_UINT(Count);
    M_UINT(NodeMask);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_INDIRECT_ARGUMENT_DESC& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_INDIRECT_ARGUMENT_TYPE);
    switch (v.Type)
    {
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            w.Key("VertexBuffer");
            w.BeginObject();
            w.Key("Slot");
            w.Uint(v.VertexBuffer.Slot);
            w.EndObject();
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            w.Key("Constant");
            w.BeginObject();
            w.Key("RootParameterIndex");
            w.Uint(v.Constant.RootParameterIndex);
            w.Key("DestOffsetIn32BitValues");
            w.Uint(v.Constant.DestOffsetIn32BitValues);
            w.Key("Num32BitValuesToSet");
            w.Uint(v.Constant.Num32BitValuesToSet);
            w.EndObject();
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            w.Key("ConstantBufferView");
            w.BeginObject();
            w.Key("RootParameterIndex");
            w.Uint(v.ConstantBufferView.RootParameterIndex);
            w.EndObject();
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            w.Key("ShaderResourceView");
            w.BeginObject();
            w.Key("RootParameterIndex");
            w.Uint(v.ShaderResourceView.RootParameterIndex);
            w.EndObject();
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
            w.Key("UnorderedAccessView");
            w.BeginObject();
            w.Key("RootParameterIndex");
            w.Uint(v.UnorderedAccessView.RootParameterIndex);
            w.EndObject();
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT:
            w.Key("IncrementingConstant");
            w.BeginObject();
            w.Key("RootParameterIndex");
            w.Uint(v.IncrementingConstant.RootParameterIndex);
            w.Key("DestOffsetIn32BitValues");
            w.Uint(v.IncrementingConstant.DestOffsetIn32BitValues);
            w.EndObject();
            break;
        default:
            // DRAW, DRAW_INDEXED, DISPATCH, INDEX_BUFFER_VIEW, DISPATCH_RAYS, DISPATCH_MESH: no parameters.
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_COMMAND_SIGNATURE_DESC& v)
{
    w.BeginObject();
    M_UINT(ByteStride);
    M_UINT(NumArgumentDescs);
    w.Key("pArgumentDescs");
    WriteArray(w, v.NumArgumentDescs, v.pArgumentDescs);
    M_UINT(NodeMask);
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Commands

void Write(JsonWriter& w, const D3D12_VIEWPORT& v)
{
    w.BeginObject();
    M_FLOAT(TopLeftX);
    M_FLOAT(TopLeftY);
    M_FLOAT(Width);
    M_FLOAT(Height);
    M_FLOAT(MinDepth);
    M_FLOAT(MaxDepth);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RESOURCE_BARRIER& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_RESOURCE_BARRIER_TYPE);
    M_FLAGS(Flags, D3D12_RESOURCE_BARRIER_FLAGS);
    switch (v.Type)
    {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            w.Key("Transition");
            w.BeginObject();
            w.Key("pResource");
            WriteRef(w, v.Transition.pResource, "ID3D12Resource");
            w.Key("Subresource");
            w.Uint(v.Transition.Subresource);
            w.Key("StateBefore");
            Flags_D3D12_RESOURCE_STATES(w, (uint64_t)v.Transition.StateBefore);
            w.Key("StateAfter");
            Flags_D3D12_RESOURCE_STATES(w, (uint64_t)v.Transition.StateAfter);
            w.EndObject();
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
            w.Key("Aliasing");
            w.BeginObject();
            w.Key("pResourceBefore");
            WriteRef(w, v.Aliasing.pResourceBefore, "ID3D12Resource");
            w.Key("pResourceAfter");
            WriteRef(w, v.Aliasing.pResourceAfter, "ID3D12Resource");
            w.EndObject();
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            w.Key("UAV");
            w.BeginObject();
            w.Key("pResource");
            WriteRef(w, v.UAV.pResource, "ID3D12Resource");
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_BARRIER_SUBRESOURCE_RANGE& v)
{
    w.BeginObject();
    M_UINT(IndexOrFirstMipLevel);
    M_UINT(NumMipLevels);
    M_UINT(FirstArraySlice);
    M_UINT(NumArraySlices);
    M_UINT(FirstPlane);
    M_UINT(NumPlanes);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_GLOBAL_BARRIER& v)
{
    w.BeginObject();
    M_FLAGS(SyncBefore, D3D12_BARRIER_SYNC);
    M_FLAGS(SyncAfter, D3D12_BARRIER_SYNC);
    M_FLAGS(AccessBefore, D3D12_BARRIER_ACCESS);
    M_FLAGS(AccessAfter, D3D12_BARRIER_ACCESS);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_TEXTURE_BARRIER& v)
{
    w.BeginObject();
    M_FLAGS(SyncBefore, D3D12_BARRIER_SYNC);
    M_FLAGS(SyncAfter, D3D12_BARRIER_SYNC);
    M_FLAGS(AccessBefore, D3D12_BARRIER_ACCESS);
    M_FLAGS(AccessAfter, D3D12_BARRIER_ACCESS);
    M_ENUM(LayoutBefore, D3D12_BARRIER_LAYOUT);
    M_ENUM(LayoutAfter, D3D12_BARRIER_LAYOUT);
    M_REF(pResource, "ID3D12Resource");
    M_NESTED(Subresources);
    M_FLAGS(Flags, D3D12_TEXTURE_BARRIER_FLAGS);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_BUFFER_BARRIER& v)
{
    w.BeginObject();
    M_FLAGS(SyncBefore, D3D12_BARRIER_SYNC);
    M_FLAGS(SyncAfter, D3D12_BARRIER_SYNC);
    M_FLAGS(AccessBefore, D3D12_BARRIER_ACCESS);
    M_FLAGS(AccessAfter, D3D12_BARRIER_ACCESS);
    M_REF(pResource, "ID3D12Resource");
    M_UINT(Offset);
    M_UINT(Size);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_BARRIER_GROUP& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_BARRIER_TYPE);
    M_UINT(NumBarriers);
    switch (v.Type)
    {
        case D3D12_BARRIER_TYPE_GLOBAL:
            w.Key("pGlobalBarriers");
            WriteArray(w, v.NumBarriers, v.pGlobalBarriers);
            break;
        case D3D12_BARRIER_TYPE_TEXTURE:
            w.Key("pTextureBarriers");
            WriteArray(w, v.NumBarriers, v.pTextureBarriers);
            break;
        case D3D12_BARRIER_TYPE_BUFFER:
            w.Key("pBufferBarriers");
            WriteArray(w, v.NumBarriers, v.pBufferBarriers);
            break;
        default: break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RENDER_PASS_BEGINNING_ACCESS& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE);
    switch (v.Type)
    {
        case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR:
            w.Key("Clear");
            w.BeginObject();
            w.Key("ClearValue");
            Write(w, &v.Clear.ClearValue, DXGI_FORMAT_UNKNOWN);
            w.EndObject();
            break;
        case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_RENDER:
        case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_SRV:
        case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_UAV:
            w.Key("PreserveLocal");
            w.BeginObject();
            w.Key("AdditionalWidth");
            w.Uint(v.PreserveLocal.AdditionalWidth);
            w.Key("AdditionalHeight");
            w.Uint(v.PreserveLocal.AdditionalHeight);
            w.EndObject();
            break;
        default:
            // DISCARD, PRESERVE, NO_ACCESS: no parameters.
            break;
    }
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS& v)
{
    w.BeginObject();
    M_UINT(SrcSubresource);
    M_UINT(DstSubresource);
    M_UINT(DstX);
    M_UINT(DstY);
    M_NESTED(SrcRect);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RENDER_PASS_ENDING_ACCESS& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE);
    switch (v.Type)
    {
        case D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE:
            w.Key("Resolve");
            w.BeginObject();
            w.Key("pSrcResource");
            WriteRef(w, v.Resolve.pSrcResource, "ID3D12Resource");
            w.Key("pDstResource");
            WriteRef(w, v.Resolve.pDstResource, "ID3D12Resource");
            w.Key("SubresourceCount");
            w.Uint(v.Resolve.SubresourceCount);
            w.Key("pSubresourceParameters");
            WriteArray(w, v.Resolve.SubresourceCount, v.Resolve.pSubresourceParameters);
            w.Key("Format");
            w.Enum(ToString_DXGI_FORMAT(EnumValue(v.Resolve.Format)), EnumValue(v.Resolve.Format));
            w.Key("ResolveMode");
            w.Enum(ToString_D3D12_RESOLVE_MODE(EnumValue(v.Resolve.ResolveMode)), EnumValue(v.Resolve.ResolveMode));
            w.Key("PreserveResolveSource");
            w.Boolean(v.Resolve.PreserveResolveSource != 0);
            w.EndObject();
            break;
        case D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE_LOCAL_RENDER:
        case D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE_LOCAL_SRV:
        case D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE_LOCAL_UAV:
            w.Key("PreserveLocal");
            w.BeginObject();
            w.Key("AdditionalWidth");
            w.Uint(v.PreserveLocal.AdditionalWidth);
            w.Key("AdditionalHeight");
            w.Uint(v.PreserveLocal.AdditionalHeight);
            w.EndObject();
            break;
        default:
            // DISCARD, PRESERVE, NO_ACCESS: no parameters.
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RENDER_PASS_RENDER_TARGET_DESC& v)
{
    w.BeginObject();
    M_HANDLE(cpuDescriptor);
    M_NESTED(BeginningAccess);
    M_NESTED(EndingAccess);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC& v)
{
    w.BeginObject();
    M_HANDLE(cpuDescriptor);
    M_NESTED(DepthBeginningAccess);
    M_NESTED(StencilBeginningAccess);
    M_NESTED(DepthEndingAccess);
    M_NESTED(StencilEndingAccess);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_GPU_VIRTUAL_ADDRESS_RANGE& v)
{
    w.BeginObject();
    M_ADDRESS(StartAddress);
    M_UINT(SizeInBytes);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& v)
{
    w.BeginObject();
    M_ADDRESS(StartAddress);
    M_UINT(SizeInBytes);
    M_UINT(StrideInBytes);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE& v)
{
    w.BeginObject();
    M_ADDRESS(StartAddress);
    M_UINT(StrideInBytes);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DISPATCH_RAYS_DESC& v)
{
    w.BeginObject();
    M_NESTED(RayGenerationShaderRecord);
    M_NESTED(MissShaderTable);
    M_NESTED(HitGroupTable);
    M_NESTED(CallableShaderTable);
    M_UINT(Width);
    M_UINT(Height);
    M_UINT(Depth);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& v)
{
    w.BeginObject();
    M_ADDRESS(Transform3x4);
    M_ENUM(IndexFormat, DXGI_FORMAT);
    M_ENUM(VertexFormat, DXGI_FORMAT);
    M_UINT(IndexCount);
    M_UINT(VertexCount);
    M_ADDRESS(IndexBuffer);
    M_NESTED(VertexBuffer);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_RAYTRACING_GEOMETRY_DESC& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_RAYTRACING_GEOMETRY_TYPE);
    M_FLAGS(Flags, D3D12_RAYTRACING_GEOMETRY_FLAGS);
    switch (v.Type)
    {
        case D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES:
            M_NESTED(Triangles);
            break;
        case D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS:
            w.Key("AABBs");
            w.BeginObject();
            w.Key("AABBCount");
            w.Uint(v.AABBs.AABBCount);
            w.Key("AABBs");
            Write(w, v.AABBs.AABBs);
            w.EndObject();
            break;
        case D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES:
            w.Key("OmmTriangles");
            w.BeginObject();
            w.Key("pTriangles");
            if (v.OmmTriangles.pTriangles)
                Write(w, *v.OmmTriangles.pTriangles);
            else
                w.Null();
            w.Key("pOmmLinkage");
            if (const auto* l = v.OmmTriangles.pOmmLinkage)
            {
                w.BeginObject();
                w.Key("OpacityMicromapIndexBuffer");
                Write(w, l->OpacityMicromapIndexBuffer);
                w.Key("OpacityMicromapIndexFormat");
                w.Enum(ToString_DXGI_FORMAT(EnumValue(l->OpacityMicromapIndexFormat)), EnumValue(l->OpacityMicromapIndexFormat));
                w.Key("OpacityMicromapBaseLocation");
                w.Uint(l->OpacityMicromapBaseLocation);
                w.Key("OpacityMicromapArray");
                WriteGpuAddress(w, l->OpacityMicromapArray);
                w.EndObject();
            }
            else
            {
                w.Null();
            }
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC& v)
{
    w.BeginObject();
    M_UINT(NumOmmHistogramEntries);
    w.Key("pOmmHistogram");
    WriteArray(w, v.NumOmmHistogramEntries, v.pOmmHistogram, [&](const D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY& e) {
        w.BeginObject();
        w.Key("Count");
        w.Uint(e.Count);
        w.Key("SubdivisionLevel");
        w.Uint(e.SubdivisionLevel);
        w.Key("Format");
        w.Enum(ToString_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT(EnumValue(e.Format)), EnumValue(e.Format));
        w.EndObject();
    });
    M_ADDRESS(InputBuffer);
    M_NESTED(PerOmmDescs);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& v)
{
    w.BeginObject();
    M_ADDRESS(DestAccelerationStructureData);
    w.Key("Inputs");
    w.BeginObject();
    {
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in = v.Inputs;
        w.Key("Type");
        w.Enum(ToString_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE(EnumValue(in.Type)), EnumValue(in.Type));
        w.Key("Flags");
        Flags_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(w, (uint64_t)in.Flags);
        w.Key("NumDescs");
        w.Uint(in.NumDescs);
        w.Key("DescsLayout");
        w.Enum(ToString_D3D12_ELEMENTS_LAYOUT(EnumValue(in.DescsLayout)), EnumValue(in.DescsLayout));
        // The union arm follows the structure type, and for a bottom level the layout says
        // whether the geometries are an array or an array of pointers.
        switch (in.Type)
        {
            case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL:
                w.Key("InstanceDescs");
                WriteGpuAddress(w, in.InstanceDescs);
                break;
            case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL:
                if (in.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS)
                {
                    w.Key("ppGeometryDescs");
                    WriteArray(w, in.NumDescs, in.ppGeometryDescs, [&](const D3D12_RAYTRACING_GEOMETRY_DESC* g) {
                        if (g)
                            Write(w, *g);
                        else
                            w.Null();
                    });
                }
                else
                {
                    w.Key("pGeometryDescs");
                    WriteArray(w, in.NumDescs, in.pGeometryDescs);
                }
                break;
            case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY:
                w.Key("pOpacityMicromapArrayDesc");
                if (in.pOpacityMicromapArrayDesc)
                    Write(w, *in.pOpacityMicromapArrayDesc);
                else
                    w.Null();
                break;
            default:
                break;
        }
    }
    w.EndObject();
    M_ADDRESS(SourceAccelerationStructureData);
    M_ADDRESS(ScratchAccelerationStructureData);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DISCARD_REGION* p)
{
    if (!p)
    {
        w.Null();
        return;
    }
    const D3D12_DISCARD_REGION& v = *p;
    w.BeginObject();
    M_UINT(NumRects);
    w.Key("pRects");
    WriteArray(w, v.NumRects, v.pRects);
    M_UINT(FirstSubresource);
    M_UINT(NumSubresources);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_SAMPLE_POSITION& v)
{
    w.BeginObject();
    M_INT(X);
    M_INT(Y);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER& v)
{
    w.BeginObject();
    M_ADDRESS(Dest);
    M_UINT(Value);
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_NODE_CPU_INPUT& v)
{
    w.BeginObject();
    M_UINT(EntrypointIndex);
    M_UINT(NumRecords);
    w.Key("pRecords");
    w.Pointer(v.pRecords);
    M_UINT(RecordStrideInBytes);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_DISPATCH_GRAPH_DESC& v)
{
    w.BeginObject();
    M_ENUM(Mode, D3D12_DISPATCH_MODE);
    switch (v.Mode)
    {
        case D3D12_DISPATCH_MODE_NODE_CPU_INPUT:
            M_NESTED(NodeCPUInput);
            break;
        case D3D12_DISPATCH_MODE_NODE_GPU_INPUT:
            M_ADDRESS(NodeGPUInput);
            break;
        case D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT:
        {
            const D3D12_MULTI_NODE_CPU_INPUT& m = v.MultiNodeCPUInput;
            w.Key("MultiNodeCPUInput");
            w.BeginObject();
            w.Key("NumNodeInputs");
            w.Uint(m.NumNodeInputs);
            // The node inputs are strided, not a plain array.
            w.Key("pNodeInputs");
            if (m.pNodeInputs)
            {
                w.BeginArray();
                const uint8_t* base = reinterpret_cast<const uint8_t*>(m.pNodeInputs);
                const UINT64 stride = m.NodeInputStrideInBytes ? m.NodeInputStrideInBytes : sizeof(D3D12_NODE_CPU_INPUT);
                for (UINT i = 0; i < m.NumNodeInputs; ++i)
                    Write(w, *reinterpret_cast<const D3D12_NODE_CPU_INPUT*>(base + i * stride));
                w.EndArray();
            }
            else
            {
                w.Null();
            }
            w.Key("NodeInputStrideInBytes");
            w.Uint(m.NodeInputStrideInBytes);
            w.EndObject();
            break;
        }
        case D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT:
            M_ADDRESS(MultiNodeGPUInput);
            break;
        default:
            break;
    }
    w.EndObject();
}

static void Write(JsonWriter& w, const D3D12_PROGRAM_IDENTIFIER& v)
{
    w.BeginObject();
    w.Key("OpaqueData");
    w.BeginArray();
    for (int i = 0; i < 4; ++i)
        w.String(Hex(v.OpaqueData[i]));
    w.EndArray();
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_SET_PROGRAM_DESC& v)
{
    w.BeginObject();
    M_ENUM(Type, D3D12_PROGRAM_TYPE);
    switch (v.Type)
    {
        case D3D12_PROGRAM_TYPE_GENERIC_PIPELINE:
            w.Key("GenericPipeline");
            w.BeginObject();
            w.Key("ProgramIdentifier");
            Write(w, v.GenericPipeline.ProgramIdentifier);
            w.EndObject();
            break;
        case D3D12_PROGRAM_TYPE_RAYTRACING_PIPELINE:
            w.Key("RaytracingPipeline");
            w.BeginObject();
            w.Key("ProgramIdentifier");
            Write(w, v.RaytracingPipeline.ProgramIdentifier);
            w.EndObject();
            break;
        case D3D12_PROGRAM_TYPE_WORK_GRAPH:
            w.Key("WorkGraph");
            w.BeginObject();
            w.Key("ProgramIdentifier");
            Write(w, v.WorkGraph.ProgramIdentifier);
            w.Key("Flags");
            Flags_D3D12_SET_WORK_GRAPH_FLAGS(w, (uint64_t)v.WorkGraph.Flags);
            w.Key("BackingMemory");
            Write(w, v.WorkGraph.BackingMemory);
            w.Key("NodeLocalRootArgumentsTable");
            Write(w, v.WorkGraph.NodeLocalRootArgumentsTable);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Device features

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS& v)
{
    w.BeginObject();
    M_BOOL(DoublePrecisionFloatShaderOps);
    M_BOOL(OutputMergerLogicOp);
    M_FLAGS(MinPrecisionSupport, D3D12_SHADER_MIN_PRECISION_SUPPORT);
    M_ENUM(TiledResourcesTier, D3D12_TILED_RESOURCES_TIER);
    M_ENUM(ResourceBindingTier, D3D12_RESOURCE_BINDING_TIER);
    M_BOOL(PSSpecifiedStencilRefSupported);
    M_BOOL(TypedUAVLoadAdditionalFormats);
    M_BOOL(ROVsSupported);
    M_ENUM(ConservativeRasterizationTier, D3D12_CONSERVATIVE_RASTERIZATION_TIER);
    M_UINT(MaxGPUVirtualAddressBitsPerResource);
    M_BOOL(StandardSwizzle64KBSupported);
    M_ENUM(CrossNodeSharingTier, D3D12_CROSS_NODE_SHARING_TIER);
    M_BOOL(CrossAdapterRowMajorTextureSupported);
    M_BOOL(VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation);
    M_ENUM(ResourceHeapTier, D3D12_RESOURCE_HEAP_TIER);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_ARCHITECTURE1& v)
{
    w.BeginObject();
    M_UINT(NodeIndex);
    M_BOOL(TileBasedRenderer);
    M_BOOL(UMA);
    M_BOOL(CacheCoherentUMA);
    M_BOOL(IsolatedMMU);
    w.EndObject();
}

}  // namespace dxinsp
