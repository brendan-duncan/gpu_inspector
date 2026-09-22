#include "serialize.h"

#include "../gen/d3d11_enums.gen.h"
#include "formats.h"

namespace d3d11insp {

namespace {

#define KEY(name) w.Key(#name)
#define U(name) KEY(name); w.Uint(v.name)
#define I(name) KEY(name); w.Int(v.name)
#define F(name) KEY(name); w.Double(v.name)
#define B(name) KEY(name); w.Boolean(v.name != 0)
#define E(name, T) KEY(name); w.Enum(ToString_##T(EnumValue(v.name)), EnumValue(v.name))
#define FL(name, T) KEY(name); Flags_##T(w, (uint64_t)v.name)
#define FMT(name) KEY(name); w.Enum(ToString_DXGI_FORMAT(EnumValue(v.name)), EnumValue(v.name))
#define S(name) KEY(name); Write(w, v.name)

void WriteUsage(JsonWriter& w, DXGI_USAGE usage) {
    // DXGI_USAGE is a bag of #defines, not an enum: the bits the swap chain descriptions use.
    static const struct { DXGI_USAGE bit; const char* name; } kBits[] = {
        {DXGI_USAGE_SHADER_INPUT, "DXGI_USAGE_SHADER_INPUT"}, {DXGI_USAGE_RENDER_TARGET_OUTPUT, "DXGI_USAGE_RENDER_TARGET_OUTPUT"},
        {DXGI_USAGE_BACK_BUFFER, "DXGI_USAGE_BACK_BUFFER"}, {DXGI_USAGE_SHARED, "DXGI_USAGE_SHARED"},
        {DXGI_USAGE_READ_ONLY, "DXGI_USAGE_READ_ONLY"}, {DXGI_USAGE_DISCARD_ON_PRESENT, "DXGI_USAGE_DISCARD_ON_PRESENT"},
        {DXGI_USAGE_UNORDERED_ACCESS, "DXGI_USAGE_UNORDERED_ACCESS"},
    };
    std::string s;
    DXGI_USAGE rest = usage;
    for (const auto& b : kBits) {
        if (!(usage & b.bit)) continue;
        if (!s.empty()) s += " | ";
        s += b.name;
        rest &= ~b.bit;
    }
    if (rest) { if (!s.empty()) s += " | "; s += Hex(rest); }
    w.String(s.empty() ? "0" : s);
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// References and arrays

void WriteRef(JsonWriter& w, const void* object, const char* fallbackClass) {
    if (!object) { w.Null(); return; }
    if (Object* o = Find(object)) w.Ref(o->id, o->type.c_str());
    else w.UntrackedRef((uint64_t)(uintptr_t)object, fallbackClass);
}

void WriteRefs(JsonWriter& w, IUnknown* const* objects, UINT count) {
    if (!objects) { w.Null(); return; }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) WriteRef(w, objects[i]);
    w.EndArray();
}

void WriteUints(JsonWriter& w, const UINT* values, UINT count) {
    if (!values) { w.Null(); return; }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) w.Uint(values[i]);
    w.EndArray();
}

void WriteFloats(JsonWriter& w, const FLOAT* values, UINT count) {
    if (!values) { w.Null(); return; }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) w.Double(values[i]);
    w.EndArray();
}

// ------------------------------------------------------------------------------------------------
// DXGI

void Write(JsonWriter& w, const DXGI_SAMPLE_DESC& v) {
    w.BeginObject();
    U(Count);
    U(Quality);
    w.EndObject();
}

static void Write(JsonWriter& w, const DXGI_RATIONAL& v) {
    w.BeginObject();
    U(Numerator);
    U(Denominator);
    w.EndObject();
}

static void Write(JsonWriter& w, const DXGI_MODE_DESC& v) {
    w.BeginObject();
    U(Width);
    U(Height);
    S(RefreshRate);
    FMT(Format);
    E(ScanlineOrdering, DXGI_MODE_SCANLINE_ORDER);
    E(Scaling, DXGI_MODE_SCALING);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_DESC& v) {
    w.BeginObject();
    S(BufferDesc);
    S(SampleDesc);
    KEY(BufferUsage); WriteUsage(w, v.BufferUsage);
    U(BufferCount);
    KEY(OutputWindow); w.Pointer(v.OutputWindow);
    B(Windowed);
    E(SwapEffect, DXGI_SWAP_EFFECT);
    FL(Flags, DXGI_SWAP_CHAIN_FLAG);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_DESC1& v) {
    w.BeginObject();
    U(Width);
    U(Height);
    FMT(Format);
    B(Stereo);
    S(SampleDesc);
    KEY(BufferUsage); WriteUsage(w, v.BufferUsage);
    U(BufferCount);
    E(Scaling, DXGI_SCALING);
    E(SwapEffect, DXGI_SWAP_EFFECT);
    E(AlphaMode, DXGI_ALPHA_MODE);
    FL(Flags, DXGI_SWAP_CHAIN_FLAG);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC& v) {
    w.BeginObject();
    S(RefreshRate);
    E(ScanlineOrdering, DXGI_MODE_SCANLINE_ORDER);
    E(Scaling, DXGI_MODE_SCALING);
    B(Windowed);
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_PRESENT_PARAMETERS& v) {
    w.BeginObject();
    U(DirtyRectsCount);
    KEY(pDirtyRects); WriteRects(w, v.pDirtyRects, v.DirtyRectsCount);
    KEY(pScrollRect); if (v.pScrollRect) Write(w, *v.pScrollRect); else w.Null();
    KEY(pScrollOffset);
    if (v.pScrollOffset) { w.BeginObject(); w.Key("x"); w.Int(v.pScrollOffset->x); w.Key("y"); w.Int(v.pScrollOffset->y); w.EndObject(); } else w.Null();
    w.EndObject();
}

void Write(JsonWriter& w, const DXGI_ADAPTER_DESC& v) {
    w.BeginObject();
    KEY(Description); w.String(Narrow(v.Description));
    U(VendorId);
    U(DeviceId);
    U(SubSysId);
    U(Revision);
    U(DedicatedVideoMemory);
    U(DedicatedSystemMemory);
    U(SharedSystemMemory);
    w.EndObject();
}

// ------------------------------------------------------------------------------------------------
// Resources

void Write(JsonWriter& w, const D3D11_BUFFER_DESC& v) {
    w.BeginObject();
    U(ByteWidth);
    E(Usage, D3D11_USAGE);
    FL(BindFlags, D3D11_BIND_FLAG);
    FL(CPUAccessFlags, D3D11_CPU_ACCESS_FLAG);
    FL(MiscFlags, D3D11_RESOURCE_MISC_FLAG);
    U(StructureByteStride);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_TEXTURE1D_DESC& v) {
    w.BeginObject();
    U(Width);
    U(MipLevels);
    U(ArraySize);
    FMT(Format);
    E(Usage, D3D11_USAGE);
    FL(BindFlags, D3D11_BIND_FLAG);
    FL(CPUAccessFlags, D3D11_CPU_ACCESS_FLAG);
    FL(MiscFlags, D3D11_RESOURCE_MISC_FLAG);
    w.EndObject();
}

static void WriteTexture2D(JsonWriter& w, const D3D11_TEXTURE2D_DESC& v, const D3D11_TEXTURE_LAYOUT* layout) {
    w.BeginObject();
    U(Width);
    U(Height);
    U(MipLevels);
    U(ArraySize);
    FMT(Format);
    S(SampleDesc);
    E(Usage, D3D11_USAGE);
    FL(BindFlags, D3D11_BIND_FLAG);
    FL(CPUAccessFlags, D3D11_CPU_ACCESS_FLAG);
    FL(MiscFlags, D3D11_RESOURCE_MISC_FLAG);
    if (layout) { KEY(TextureLayout); w.Enum(ToString_D3D11_TEXTURE_LAYOUT(*layout), *layout); }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_TEXTURE2D_DESC& v) { WriteTexture2D(w, v, nullptr); }

void Write(JsonWriter& w, const D3D11_TEXTURE2D_DESC1& v) {
    D3D11_TEXTURE2D_DESC base;
    memcpy(&base, &v, sizeof(base));
    WriteTexture2D(w, base, &v.TextureLayout);
}

static void WriteTexture3D(JsonWriter& w, const D3D11_TEXTURE3D_DESC& v, const D3D11_TEXTURE_LAYOUT* layout) {
    w.BeginObject();
    U(Width);
    U(Height);
    U(Depth);
    U(MipLevels);
    FMT(Format);
    E(Usage, D3D11_USAGE);
    FL(BindFlags, D3D11_BIND_FLAG);
    FL(CPUAccessFlags, D3D11_CPU_ACCESS_FLAG);
    FL(MiscFlags, D3D11_RESOURCE_MISC_FLAG);
    if (layout) { KEY(TextureLayout); w.Enum(ToString_D3D11_TEXTURE_LAYOUT(*layout), *layout); }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_TEXTURE3D_DESC& v) { WriteTexture3D(w, v, nullptr); }

void Write(JsonWriter& w, const D3D11_TEXTURE3D_DESC1& v) {
    D3D11_TEXTURE3D_DESC base;
    memcpy(&base, &v, sizeof(base));
    WriteTexture3D(w, base, &v.TextureLayout);
}

void WriteInitialData(JsonWriter& w, const D3D11_SUBRESOURCE_DATA* data, UINT count) {
    if (!data) { w.Null(); return; }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) {
        w.BeginObject();
        w.Key("pSysMem"); w.Pointer(data[i].pSysMem);
        w.Key("SysMemPitch"); w.Uint(data[i].SysMemPitch);
        w.Key("SysMemSlicePitch"); w.Uint(data[i].SysMemSlicePitch);
        w.EndObject();
    }
    w.EndArray();
}

void Write(JsonWriter& w, const D3D11_BOX* p) {
    if (!p) { w.Null(); return; }
    const D3D11_BOX& v = *p;
    w.BeginObject();
    U(left);
    U(top);
    U(front);
    U(right);
    U(bottom);
    U(back);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_MAPPED_SUBRESOURCE& v) {
    w.BeginObject();
    KEY(pData); w.Pointer(v.pData);
    U(RowPitch);
    U(DepthPitch);
    w.EndObject();
}

// ------------------------------------------------------------------------------------------------
// Views

void Write(JsonWriter& w, const D3D11_SHADER_RESOURCE_VIEW_DESC* p) {
    if (!p) { w.Null(); return; }
    const D3D11_SHADER_RESOURCE_VIEW_DESC& v = *p;
    w.BeginObject();
    FMT(Format);
    E(ViewDimension, D3D_SRV_DIMENSION);
    switch (v.ViewDimension) {
        case D3D11_SRV_DIMENSION_BUFFER:
            KEY(Buffer); w.BeginObject();
            w.Key("FirstElement"); w.Uint(v.Buffer.FirstElement);
            w.Key("NumElements"); w.Uint(v.Buffer.NumElements);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_BUFFEREX:
            KEY(BufferEx); w.BeginObject();
            w.Key("FirstElement"); w.Uint(v.BufferEx.FirstElement);
            w.Key("NumElements"); w.Uint(v.BufferEx.NumElements);
            w.Key("Flags"); Flags_D3D11_BUFFEREX_SRV_FLAG(w, v.BufferEx.Flags);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURE1D:
            KEY(Texture1D); w.BeginObject();
            w.Key("MostDetailedMip"); w.Uint(v.Texture1D.MostDetailedMip);
            w.Key("MipLevels"); w.Uint(v.Texture1D.MipLevels);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
            KEY(Texture1DArray); w.BeginObject();
            w.Key("MostDetailedMip"); w.Uint(v.Texture1DArray.MostDetailedMip);
            w.Key("MipLevels"); w.Uint(v.Texture1DArray.MipLevels);
            w.Key("FirstArraySlice"); w.Uint(v.Texture1DArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture1DArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2D:
            KEY(Texture2D); w.BeginObject();
            w.Key("MostDetailedMip"); w.Uint(v.Texture2D.MostDetailedMip);
            w.Key("MipLevels"); w.Uint(v.Texture2D.MipLevels);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            KEY(Texture2DArray); w.BeginObject();
            w.Key("MostDetailedMip"); w.Uint(v.Texture2DArray.MostDetailedMip);
            w.Key("MipLevels"); w.Uint(v.Texture2DArray.MipLevels);
            w.Key("FirstArraySlice"); w.Uint(v.Texture2DArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture2DArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMS:
            KEY(Texture2DMS); w.BeginObject(); w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
            KEY(Texture2DMSArray); w.BeginObject();
            w.Key("FirstArraySlice"); w.Uint(v.Texture2DMSArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture2DMSArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURE3D:
            KEY(Texture3D); w.BeginObject();
            w.Key("MostDetailedMip"); w.Uint(v.Texture3D.MostDetailedMip);
            w.Key("MipLevels"); w.Uint(v.Texture3D.MipLevels);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURECUBE:
            KEY(TextureCube); w.BeginObject();
            w.Key("MostDetailedMip"); w.Uint(v.TextureCube.MostDetailedMip);
            w.Key("MipLevels"); w.Uint(v.TextureCube.MipLevels);
            w.EndObject();
            break;
        case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
            KEY(TextureCubeArray); w.BeginObject();
            w.Key("MostDetailedMip"); w.Uint(v.TextureCubeArray.MostDetailedMip);
            w.Key("MipLevels"); w.Uint(v.TextureCubeArray.MipLevels);
            w.Key("First2DArrayFace"); w.Uint(v.TextureCubeArray.First2DArrayFace);
            w.Key("NumCubes"); w.Uint(v.TextureCubeArray.NumCubes);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_SHADER_RESOURCE_VIEW_DESC1* p) {
    if (!p) { w.Null(); return; }
    // The same layout as the base description, plus a plane slice on the 2D dimensions.
    D3D11_SHADER_RESOURCE_VIEW_DESC base;
    memcpy(&base, p, sizeof(base));
    Write(w, &base);
}

void Write(JsonWriter& w, const D3D11_RENDER_TARGET_VIEW_DESC* p) {
    if (!p) { w.Null(); return; }
    const D3D11_RENDER_TARGET_VIEW_DESC& v = *p;
    w.BeginObject();
    FMT(Format);
    E(ViewDimension, D3D11_RTV_DIMENSION);
    switch (v.ViewDimension) {
        case D3D11_RTV_DIMENSION_BUFFER:
            KEY(Buffer); w.BeginObject();
            w.Key("FirstElement"); w.Uint(v.Buffer.FirstElement);
            w.Key("NumElements"); w.Uint(v.Buffer.NumElements);
            w.EndObject();
            break;
        case D3D11_RTV_DIMENSION_TEXTURE1D:
            KEY(Texture1D); w.BeginObject(); w.Key("MipSlice"); w.Uint(v.Texture1D.MipSlice); w.EndObject();
            break;
        case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
            KEY(Texture1DArray); w.BeginObject();
            w.Key("MipSlice"); w.Uint(v.Texture1DArray.MipSlice);
            w.Key("FirstArraySlice"); w.Uint(v.Texture1DArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture1DArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2D:
            KEY(Texture2D); w.BeginObject(); w.Key("MipSlice"); w.Uint(v.Texture2D.MipSlice); w.EndObject();
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
            KEY(Texture2DArray); w.BeginObject();
            w.Key("MipSlice"); w.Uint(v.Texture2DArray.MipSlice);
            w.Key("FirstArraySlice"); w.Uint(v.Texture2DArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture2DArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMS:
            KEY(Texture2DMS); w.BeginObject(); w.EndObject();
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
            KEY(Texture2DMSArray); w.BeginObject();
            w.Key("FirstArraySlice"); w.Uint(v.Texture2DMSArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture2DMSArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_RTV_DIMENSION_TEXTURE3D:
            KEY(Texture3D); w.BeginObject();
            w.Key("MipSlice"); w.Uint(v.Texture3D.MipSlice);
            w.Key("FirstWSlice"); w.Uint(v.Texture3D.FirstWSlice);
            w.Key("WSize"); w.Uint(v.Texture3D.WSize);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_RENDER_TARGET_VIEW_DESC1* p) {
    if (!p) { w.Null(); return; }
    D3D11_RENDER_TARGET_VIEW_DESC base;
    memcpy(&base, p, sizeof(base));
    Write(w, &base);
}

void Write(JsonWriter& w, const D3D11_DEPTH_STENCIL_VIEW_DESC* p) {
    if (!p) { w.Null(); return; }
    const D3D11_DEPTH_STENCIL_VIEW_DESC& v = *p;
    w.BeginObject();
    FMT(Format);
    E(ViewDimension, D3D11_DSV_DIMENSION);
    FL(Flags, D3D11_DSV_FLAG);
    switch (v.ViewDimension) {
        case D3D11_DSV_DIMENSION_TEXTURE1D:
            KEY(Texture1D); w.BeginObject(); w.Key("MipSlice"); w.Uint(v.Texture1D.MipSlice); w.EndObject();
            break;
        case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
            KEY(Texture1DArray); w.BeginObject();
            w.Key("MipSlice"); w.Uint(v.Texture1DArray.MipSlice);
            w.Key("FirstArraySlice"); w.Uint(v.Texture1DArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture1DArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2D:
            KEY(Texture2D); w.BeginObject(); w.Key("MipSlice"); w.Uint(v.Texture2D.MipSlice); w.EndObject();
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
            KEY(Texture2DArray); w.BeginObject();
            w.Key("MipSlice"); w.Uint(v.Texture2DArray.MipSlice);
            w.Key("FirstArraySlice"); w.Uint(v.Texture2DArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture2DArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMS:
            KEY(Texture2DMS); w.BeginObject(); w.EndObject();
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
            KEY(Texture2DMSArray); w.BeginObject();
            w.Key("FirstArraySlice"); w.Uint(v.Texture2DMSArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture2DMSArray.ArraySize);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_UNORDERED_ACCESS_VIEW_DESC* p) {
    if (!p) { w.Null(); return; }
    const D3D11_UNORDERED_ACCESS_VIEW_DESC& v = *p;
    w.BeginObject();
    FMT(Format);
    E(ViewDimension, D3D11_UAV_DIMENSION);
    switch (v.ViewDimension) {
        case D3D11_UAV_DIMENSION_BUFFER:
            KEY(Buffer); w.BeginObject();
            w.Key("FirstElement"); w.Uint(v.Buffer.FirstElement);
            w.Key("NumElements"); w.Uint(v.Buffer.NumElements);
            w.Key("Flags"); Flags_D3D11_BUFFER_UAV_FLAG(w, v.Buffer.Flags);
            w.EndObject();
            break;
        case D3D11_UAV_DIMENSION_TEXTURE1D:
            KEY(Texture1D); w.BeginObject(); w.Key("MipSlice"); w.Uint(v.Texture1D.MipSlice); w.EndObject();
            break;
        case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
            KEY(Texture1DArray); w.BeginObject();
            w.Key("MipSlice"); w.Uint(v.Texture1DArray.MipSlice);
            w.Key("FirstArraySlice"); w.Uint(v.Texture1DArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture1DArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_UAV_DIMENSION_TEXTURE2D:
            KEY(Texture2D); w.BeginObject(); w.Key("MipSlice"); w.Uint(v.Texture2D.MipSlice); w.EndObject();
            break;
        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
            KEY(Texture2DArray); w.BeginObject();
            w.Key("MipSlice"); w.Uint(v.Texture2DArray.MipSlice);
            w.Key("FirstArraySlice"); w.Uint(v.Texture2DArray.FirstArraySlice);
            w.Key("ArraySize"); w.Uint(v.Texture2DArray.ArraySize);
            w.EndObject();
            break;
        case D3D11_UAV_DIMENSION_TEXTURE3D:
            KEY(Texture3D); w.BeginObject();
            w.Key("MipSlice"); w.Uint(v.Texture3D.MipSlice);
            w.Key("FirstWSlice"); w.Uint(v.Texture3D.FirstWSlice);
            w.Key("WSize"); w.Uint(v.Texture3D.WSize);
            w.EndObject();
            break;
        default:
            break;
    }
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_UNORDERED_ACCESS_VIEW_DESC1* p) {
    if (!p) { w.Null(); return; }
    D3D11_UNORDERED_ACCESS_VIEW_DESC base;
    memcpy(&base, p, sizeof(base));
    Write(w, &base);
}

void Write(JsonWriter& w, const D3D11_SAMPLER_DESC& v) {
    w.BeginObject();
    E(Filter, D3D11_FILTER);
    E(AddressU, D3D11_TEXTURE_ADDRESS_MODE);
    E(AddressV, D3D11_TEXTURE_ADDRESS_MODE);
    E(AddressW, D3D11_TEXTURE_ADDRESS_MODE);
    F(MipLODBias);
    U(MaxAnisotropy);
    E(ComparisonFunc, D3D11_COMPARISON_FUNC);
    KEY(BorderColor); WriteFloats(w, v.BorderColor, 4);
    F(MinLOD);
    F(MaxLOD);
    w.EndObject();
}

// ------------------------------------------------------------------------------------------------
// Pipeline state

void Write(JsonWriter& w, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count) {
    if (!elements) { w.Null(); return; }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) {
        const D3D11_INPUT_ELEMENT_DESC& v = elements[i];
        w.BeginObject();
        KEY(SemanticName); if (v.SemanticName) w.String(v.SemanticName); else w.Null();
        U(SemanticIndex);
        FMT(Format);
        U(InputSlot);
        KEY(AlignedByteOffset);
        if (v.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT) w.String("D3D11_APPEND_ALIGNED_ELEMENT"); else w.Uint(v.AlignedByteOffset);
        E(InputSlotClass, D3D11_INPUT_CLASSIFICATION);
        U(InstanceDataStepRate);
        w.EndObject();
    }
    w.EndArray();
}

void Write(JsonWriter& w, const D3D11_SO_DECLARATION_ENTRY* entries, UINT count) {
    if (!entries) { w.Null(); return; }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) {
        const D3D11_SO_DECLARATION_ENTRY& v = entries[i];
        w.BeginObject();
        U(Stream);
        KEY(SemanticName); if (v.SemanticName) w.String(v.SemanticName); else w.Null();
        U(SemanticIndex);
        U(StartComponent);
        U(ComponentCount);
        U(OutputSlot);
        w.EndObject();
    }
    w.EndArray();
}

static void WriteTargetBlend(JsonWriter& w, const D3D11_RENDER_TARGET_BLEND_DESC& v) {
    w.BeginObject();
    B(BlendEnable);
    E(SrcBlend, D3D11_BLEND);
    E(DestBlend, D3D11_BLEND);
    E(BlendOp, D3D11_BLEND_OP);
    E(SrcBlendAlpha, D3D11_BLEND);
    E(DestBlendAlpha, D3D11_BLEND);
    E(BlendOpAlpha, D3D11_BLEND_OP);
    KEY(RenderTargetWriteMask); Flags_D3D11_COLOR_WRITE_ENABLE(w, v.RenderTargetWriteMask);
    w.EndObject();
}

static void WriteTargetBlend(JsonWriter& w, const D3D11_RENDER_TARGET_BLEND_DESC1& v) {
    w.BeginObject();
    B(BlendEnable);
    B(LogicOpEnable);
    E(SrcBlend, D3D11_BLEND);
    E(DestBlend, D3D11_BLEND);
    E(BlendOp, D3D11_BLEND_OP);
    E(SrcBlendAlpha, D3D11_BLEND);
    E(DestBlendAlpha, D3D11_BLEND);
    E(BlendOpAlpha, D3D11_BLEND_OP);
    E(LogicOp, D3D11_LOGIC_OP);
    KEY(RenderTargetWriteMask); Flags_D3D11_COLOR_WRITE_ENABLE(w, v.RenderTargetWriteMask);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_BLEND_DESC& v) {
    w.BeginObject();
    B(AlphaToCoverageEnable);
    B(IndependentBlendEnable);
    KEY(RenderTarget);
    w.BeginArray();
    for (UINT i = 0; i < 8; ++i) WriteTargetBlend(w, v.RenderTarget[i]);
    w.EndArray();
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_BLEND_DESC1& v) {
    w.BeginObject();
    B(AlphaToCoverageEnable);
    B(IndependentBlendEnable);
    KEY(RenderTarget);
    w.BeginArray();
    for (UINT i = 0; i < 8; ++i) WriteTargetBlend(w, v.RenderTarget[i]);
    w.EndArray();
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_RASTERIZER_DESC& v) {
    w.BeginObject();
    E(FillMode, D3D11_FILL_MODE);
    E(CullMode, D3D11_CULL_MODE);
    B(FrontCounterClockwise);
    I(DepthBias);
    F(DepthBiasClamp);
    F(SlopeScaledDepthBias);
    B(DepthClipEnable);
    B(ScissorEnable);
    B(MultisampleEnable);
    B(AntialiasedLineEnable);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_RASTERIZER_DESC1& v) {
    w.BeginObject();
    E(FillMode, D3D11_FILL_MODE);
    E(CullMode, D3D11_CULL_MODE);
    B(FrontCounterClockwise);
    I(DepthBias);
    F(DepthBiasClamp);
    F(SlopeScaledDepthBias);
    B(DepthClipEnable);
    B(ScissorEnable);
    B(MultisampleEnable);
    B(AntialiasedLineEnable);
    U(ForcedSampleCount);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_RASTERIZER_DESC2& v) {
    w.BeginObject();
    E(FillMode, D3D11_FILL_MODE);
    E(CullMode, D3D11_CULL_MODE);
    B(FrontCounterClockwise);
    I(DepthBias);
    F(DepthBiasClamp);
    F(SlopeScaledDepthBias);
    B(DepthClipEnable);
    B(ScissorEnable);
    B(MultisampleEnable);
    B(AntialiasedLineEnable);
    U(ForcedSampleCount);
    E(ConservativeRaster, D3D11_CONSERVATIVE_RASTERIZATION_MODE);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_DEPTH_STENCILOP_DESC& v) {
    w.BeginObject();
    E(StencilFailOp, D3D11_STENCIL_OP);
    E(StencilDepthFailOp, D3D11_STENCIL_OP);
    E(StencilPassOp, D3D11_STENCIL_OP);
    E(StencilFunc, D3D11_COMPARISON_FUNC);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_DEPTH_STENCIL_DESC& v) {
    w.BeginObject();
    B(DepthEnable);
    E(DepthWriteMask, D3D11_DEPTH_WRITE_MASK);
    E(DepthFunc, D3D11_COMPARISON_FUNC);
    B(StencilEnable);
    U(StencilReadMask);
    U(StencilWriteMask);
    S(FrontFace);
    S(BackFace);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_QUERY_DESC& v) {
    w.BeginObject();
    E(Query, D3D11_QUERY);
    FL(MiscFlags, D3D11_QUERY_MISC_FLAG);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_QUERY_DESC1& v) {
    w.BeginObject();
    E(Query, D3D11_QUERY);
    FL(MiscFlags, D3D11_QUERY_MISC_FLAG);
    E(ContextType, D3D11_CONTEXT_TYPE);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_COUNTER_DESC& v) {
    w.BeginObject();
    E(Counter, D3D11_COUNTER);
    U(MiscFlags);
    w.EndObject();
}

// ------------------------------------------------------------------------------------------------
// Commands

void Write(JsonWriter& w, const D3D11_VIEWPORT& v) {
    w.BeginObject();
    F(TopLeftX);
    F(TopLeftY);
    F(Width);
    F(Height);
    F(MinDepth);
    F(MaxDepth);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D11_RECT& v) {
    w.BeginObject();
    I(left);
    I(top);
    I(right);
    I(bottom);
    w.EndObject();
}

void WriteViewports(JsonWriter& w, const D3D11_VIEWPORT* v, UINT count) {
    if (!v) { w.Null(); return; }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) Write(w, v[i]);
    w.EndArray();
}

void WriteRects(JsonWriter& w, const D3D11_RECT* v, UINT count) {
    if (!v) { w.Null(); return; }
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) Write(w, v[i]);
    w.EndArray();
}

}  // namespace d3d11insp
