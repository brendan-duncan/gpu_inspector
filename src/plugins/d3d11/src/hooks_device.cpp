// The entry points and the device: MinHook on D3D11CreateDevice and D3D11CreateDeviceAndSwapChain,
// then a vtable patch on the device they return, whose Create* methods are where every resource,
// view, shader and state object comes from. Each is tracked with the description it was made
// from, and the ones the capture needs to know more about (resources, views, input layouts,
// shaders) keep their facts in the object record (state.h).
#include "hooks.h"

#include "../gen/d3d11_enums.gen.h"
#include "../gen/d3d11_vtables.gen.h"
#include "capture.h"
#include "formats.h"
#include "serialize.h"
#include "server.h"
#include "shader_reflect.h"
#include "state.h"

#include <gpu_inspector/sdk/config.h>

#include <intrin.h>

#include <mutex>

namespace d3d11insp {

namespace {

#define DEV(name) Orig<PFN_ID3D11Device5_##name>(This, slot::ID3D11Device5_##name)

/** The device an object was made on, as the parent of everything it creates. */
uint64_t DeviceId(ID3D11Device* device) {
    return IdOf(device);
}

// ---------------------------------------------------------------------------------------------
// Resources

void DescribeResource(Object& o, DXGI_FORMAT format, UINT width, UINT height, UINT depth, UINT mips, UINT samples,
                      D3D11_USAGE usage, UINT bind, UINT cpu, UINT misc) {
    o.format = format;
    o.width = width;
    o.height = height;
    o.depth = depth;
    o.mips = mips ? mips : 1;
    o.samples = samples ? samples : 1;
    o.usage = usage;
    o.bindFlags = bind;
    o.cpuAccess = cpu;
    o.miscFlags = misc;
    const FormatInfo f = FormatOf(TypedFormat(format, (bind & D3D11_BIND_DEPTH_STENCIL) != 0));
    Describe(o, "format", JsonString(f.protocolName ? f.protocolName : "VK_FORMAT_UNDEFINED"));
}

UINT Texture1DMips(ID3D11Texture1D* t) {
    D3D11_TEXTURE1D_DESC d{};
    ScopedInternal internal;
    t->GetDesc(&d);
    return d.MipLevels ? d.MipLevels : 1;
}

UINT Texture2DMips(ID3D11Texture2D* t) {
    D3D11_TEXTURE2D_DESC d{};
    ScopedInternal internal;
    t->GetDesc(&d);
    return d.MipLevels ? d.MipLevels : 1;
}

UINT Texture3DMips(ID3D11Texture3D* t) {
    D3D11_TEXTURE3D_DESC d{};
    ScopedInternal internal;
    t->GetDesc(&d);
    return d.MipLevels ? d.MipLevels : 1;
}

void HookBuffer(ID3D11Buffer* b) {
    if (!b || VtableHooked(b)) return;
    HookDeviceChild(b, "ID3D11Buffer", slot::ID3D11Buffer_Count, {});
}

void HookTexture1D(ID3D11Texture1D* t) {
    if (!t || VtableHooked(t)) return;
    HookDeviceChild(t, "ID3D11Texture1D", slot::ID3D11Texture1D_Count, {});
}

void HookTexture2D(ID3D11Texture2D* t) {
    if (!t || VtableHooked(t)) return;
    static const VersionCount versions[] = {{&__uuidof(ID3D11Texture2D1), slot::ID3D11Texture2D1_Count}};
    HookDeviceChild(t, "ID3D11Texture2D", VtableCount(t, versions, 1, slot::ID3D11Texture2D1_GetDesc + 1), {});
}

void HookTexture3D(ID3D11Texture3D* t) {
    if (!t || VtableHooked(t)) return;
    static const VersionCount versions[] = {{&__uuidof(ID3D11Texture3D1), slot::ID3D11Texture3D1_Count}};
    HookDeviceChild(t, "ID3D11Texture3D", VtableCount(t, versions, 1, slot::ID3D11Texture3D1_GetDesc + 1), {});
}

HRESULT STDMETHODCALLTYPE Hook_CreateBuffer(ID3D11Device5* This, const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer) {
    HRESULT hr = DEV(CreateBuffer)(This, pDesc, pInitialData, ppBuffer);
    if (Internal() || FAILED(hr) || !ppBuffer || !*ppBuffer || !pDesc) return hr;
    HookBuffer(*ppBuffer);
    Object& o = Track(*ppBuffer, ObjKind::Buffer, "ID3D11Buffer", DeviceId(This), "CreateBuffer");
    if (!o.args.empty()) return hr;   // an object at a reused address, or one the runtime handed out twice
    JsonWriter w;
    Write(w, *pDesc);
    Describe(o, "pDesc", w.str());
    JsonWriter i;
    WriteInitialData(i, pInitialData, 1);
    Describe(o, "pInitialData", i.str());
    o.size = pDesc->ByteWidth;
    o.stride = pDesc->StructureByteStride;
    o.usage = pDesc->Usage;
    o.bindFlags = pDesc->BindFlags;
    o.cpuAccess = pDesc->CPUAccessFlags;
    o.miscFlags = pDesc->MiscFlags;
    Announce(o);
    Log("CreateBuffer -> %p (%u bytes)", (void*)*ppBuffer, pDesc->ByteWidth);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateTexture1D(ID3D11Device5* This, const D3D11_TEXTURE1D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture1D** ppTexture1D) {
    HRESULT hr = DEV(CreateTexture1D)(This, pDesc, pInitialData, ppTexture1D);
    if (Internal() || FAILED(hr) || !ppTexture1D || !*ppTexture1D || !pDesc) return hr;
    HookTexture1D(*ppTexture1D);
    Object& o = Track(*ppTexture1D, ObjKind::Texture1D, "ID3D11Texture1D", DeviceId(This), "CreateTexture1D");
    if (!o.args.empty()) return hr;
    JsonWriter w;
    Write(w, *pDesc);
    Describe(o, "pDesc", w.str());
    const UINT mips = Texture1DMips(*ppTexture1D);
    JsonWriter i;
    WriteInitialData(i, pInitialData, pInitialData ? mips * pDesc->ArraySize : 0);
    Describe(o, "pInitialData", i.str());
    DescribeResource(o, pDesc->Format, pDesc->Width, 1, pDesc->ArraySize, mips, 1, pDesc->Usage, pDesc->BindFlags, pDesc->CPUAccessFlags, pDesc->MiscFlags);
    Announce(o);
    return hr;
}

void RegisterTexture2D(ID3D11Device5* device, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* pInitialData, const char* cmd, const std::string& descJson) {
    HookTexture2D(tex);
    Object& o = Track(tex, ObjKind::Texture2D, "ID3D11Texture2D", DeviceId(device), cmd);
    if (!o.args.empty()) return;
    Describe(o, "pDesc", descJson);
    const UINT mips = Texture2DMips(tex);
    JsonWriter i;
    WriteInitialData(i, pInitialData, pInitialData ? mips * desc.ArraySize : 0);
    Describe(o, "pInitialData", i.str());
    DescribeResource(o, desc.Format, desc.Width, desc.Height, desc.ArraySize, mips, desc.SampleDesc.Count, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
    Announce(o);
    Log("%s -> %p (%ux%u %s)", cmd, (void*)tex, desc.Width, desc.Height, FormatName(desc.Format));
}

HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D(ID3D11Device5* This, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) {
    HRESULT hr = DEV(CreateTexture2D)(This, pDesc, pInitialData, ppTexture2D);
    if (Internal() || FAILED(hr) || !ppTexture2D || !*ppTexture2D || !pDesc) return hr;
    JsonWriter w;
    Write(w, *pDesc);
    RegisterTexture2D(This, *ppTexture2D, *pDesc, pInitialData, "CreateTexture2D", w.str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D1(ID3D11Device5* This, const D3D11_TEXTURE2D_DESC1* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D1** ppTexture2D) {
    HRESULT hr = DEV(CreateTexture2D1)(This, pDesc, pInitialData, ppTexture2D);
    if (Internal() || FAILED(hr) || !ppTexture2D || !*ppTexture2D || !pDesc) return hr;
    JsonWriter w;
    Write(w, *pDesc);
    D3D11_TEXTURE2D_DESC base;
    memcpy(&base, pDesc, sizeof(base));
    RegisterTexture2D(This, *ppTexture2D, base, pInitialData, "CreateTexture2D1", w.str());
    return hr;
}

void RegisterTexture3D(ID3D11Device5* device, ID3D11Texture3D* tex, const D3D11_TEXTURE3D_DESC& desc, const D3D11_SUBRESOURCE_DATA* pInitialData, const char* cmd, const std::string& descJson) {
    HookTexture3D(tex);
    Object& o = Track(tex, ObjKind::Texture3D, "ID3D11Texture3D", DeviceId(device), cmd);
    if (!o.args.empty()) return;
    Describe(o, "pDesc", descJson);
    const UINT mips = Texture3DMips(tex);
    JsonWriter i;
    WriteInitialData(i, pInitialData, pInitialData ? mips : 0);
    Describe(o, "pInitialData", i.str());
    DescribeResource(o, desc.Format, desc.Width, desc.Height, desc.Depth, mips, 1, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
    Announce(o);
}

HRESULT STDMETHODCALLTYPE Hook_CreateTexture3D(ID3D11Device5* This, const D3D11_TEXTURE3D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D** ppTexture3D) {
    HRESULT hr = DEV(CreateTexture3D)(This, pDesc, pInitialData, ppTexture3D);
    if (Internal() || FAILED(hr) || !ppTexture3D || !*ppTexture3D || !pDesc) return hr;
    JsonWriter w;
    Write(w, *pDesc);
    RegisterTexture3D(This, *ppTexture3D, *pDesc, pInitialData, "CreateTexture3D", w.str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateTexture3D1(ID3D11Device5* This, const D3D11_TEXTURE3D_DESC1* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D1** ppTexture3D) {
    HRESULT hr = DEV(CreateTexture3D1)(This, pDesc, pInitialData, ppTexture3D);
    if (Internal() || FAILED(hr) || !ppTexture3D || !*ppTexture3D || !pDesc) return hr;
    JsonWriter w;
    Write(w, *pDesc);
    D3D11_TEXTURE3D_DESC base;
    memcpy(&base, pDesc, sizeof(base));
    RegisterTexture3D(This, *ppTexture3D, base, pInitialData, "CreateTexture3D1", w.str());
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Views

/** The element size of a buffer view: the structure stride, 4 for a raw view, else the format's texel. */
UINT64 ElementBytes(const Object& buffer, DXGI_FORMAT format, bool raw) {
    if (buffer.stride) return buffer.stride;
    if (raw) return 4;
    const UINT bytes = FormatOf(format).bytes;
    return bytes ? bytes : 1;
}

/** Fills the view's facts from what it covers of its resource. */
void SetViewRange(Object& v, const Object* r, DXGI_FORMAT format, UINT mip, UINT firstSlice, UINT slices, UINT64 firstElement, UINT64 elements, bool raw, bool buffer) {
    v.viewFormat = format;
    v.mip = mip;
    v.firstSlice = firstSlice;
    v.sliceCount = slices;
    if (buffer && r) {
        const UINT64 bytes = ElementBytes(*r, format, raw);
        v.viewOffset = firstElement * bytes;
        v.viewSize = elements * bytes;
    }
}

/** The slice count a view asked for, resolved against the resource (-1 means "all remaining"). */
UINT SlicesOf(const Object* r, UINT first, UINT count) {
    const UINT total = r ? std::max(1u, r->depth) : 1;
    if (count == (UINT)-1) return first < total ? total - first : 1;
    return std::max(1u, count);
}

Object& RegisterView(ID3D11View* view, ObjKind kind, const char* type, ID3D11Device* device, ID3D11Resource* resource, const char* cmd) {
    Object& o = Track(view, kind, type, DeviceId(device), cmd);
    if (!o.args.empty()) return o;
    o.resource = resource;
    o.resourceId = IdOf(resource);
    Describe(o, "pResource", JsonRef(resource));
    return o;
}

void HookView(ID3D11View* view, const char* name, uint32_t count) {
    if (!view || VtableHooked(view)) return;
    HookDeviceChild(view, name, count, {});
}

HRESULT STDMETHODCALLTYPE Hook_CreateShaderResourceView(ID3D11Device5* This, ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView) {
    HRESULT hr = DEV(CreateShaderResourceView)(This, pResource, pDesc, ppSRView);
    if (Internal() || FAILED(hr) || !ppSRView || !*ppSRView) return hr;
    HookView(*ppSRView, "ID3D11ShaderResourceView", slot::ID3D11ShaderResourceView1_GetDesc + 1);
    Object& o = RegisterView(*ppSRView, ObjKind::ShaderResourceView, "ID3D11ShaderResourceView", This, pResource, "CreateShaderResourceView");
    if (o.args.size() != 1) return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    { ScopedInternal internal; (*ppSRView)->GetDesc(&d); }
    JsonWriter w;
    Write(w, pDesc ? pDesc : &d);
    Describe(o, "pDesc", w.str());
    const Object* r = Find(pResource);
    switch (d.ViewDimension) {
        case D3D11_SRV_DIMENSION_BUFFER: SetViewRange(o, r, d.Format, 0, 0, 1, d.Buffer.FirstElement, d.Buffer.NumElements, false, true); break;
        case D3D11_SRV_DIMENSION_BUFFEREX: SetViewRange(o, r, d.Format, 0, 0, 1, d.BufferEx.FirstElement, d.BufferEx.NumElements, (d.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0, true); break;
        case D3D11_SRV_DIMENSION_TEXTURE1D: SetViewRange(o, r, d.Format, d.Texture1D.MostDetailedMip, 0, 1, 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURE1DARRAY: SetViewRange(o, r, d.Format, d.Texture1DArray.MostDetailedMip, d.Texture1DArray.FirstArraySlice, SlicesOf(r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURE2D: SetViewRange(o, r, d.Format, d.Texture2D.MostDetailedMip, 0, 1, 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY: SetViewRange(o, r, d.Format, d.Texture2DArray.MostDetailedMip, d.Texture2DArray.FirstArraySlice, SlicesOf(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMS: SetViewRange(o, r, d.Format, 0, 0, 1, 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY: SetViewRange(o, r, d.Format, 0, d.Texture2DMSArray.FirstArraySlice, SlicesOf(r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize), 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURE3D: SetViewRange(o, r, d.Format, d.Texture3D.MostDetailedMip, 0, 1, 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURECUBE: SetViewRange(o, r, d.Format, d.TextureCube.MostDetailedMip, 0, 6, 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY: SetViewRange(o, r, d.Format, d.TextureCubeArray.MostDetailedMip, d.TextureCubeArray.First2DArrayFace, SlicesOf(r, d.TextureCubeArray.First2DArrayFace, d.TextureCubeArray.NumCubes == (UINT)-1 ? (UINT)-1 : d.TextureCubeArray.NumCubes * 6), 0, 0, false, false); break;
        default: SetViewRange(o, r, d.Format, 0, 0, 1, 0, 0, false, false); break;
    }
    Announce(o);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateShaderResourceView1(ID3D11Device5* This, ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc1, ID3D11ShaderResourceView1** ppSRView1) {
    HRESULT hr = DEV(CreateShaderResourceView1)(This, pResource, pDesc1, ppSRView1);
    if (Internal() || FAILED(hr) || !ppSRView1 || !*ppSRView1) return hr;
    HookView(*ppSRView1, "ID3D11ShaderResourceView", slot::ID3D11ShaderResourceView1_Count);
    Object& o = RegisterView(*ppSRView1, ObjKind::ShaderResourceView, "ID3D11ShaderResourceView", This, pResource, "CreateShaderResourceView1");
    if (o.args.size() != 1) return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC1 d{};
    { ScopedInternal internal; (*ppSRView1)->GetDesc1(&d); }
    JsonWriter w;
    Write(w, pDesc1 ? pDesc1 : &d);
    Describe(o, "pDesc", w.str());
    const Object* r = Find(pResource);
    switch (d.ViewDimension) {
        case D3D11_SRV_DIMENSION_BUFFER: SetViewRange(o, r, d.Format, 0, 0, 1, d.Buffer.FirstElement, d.Buffer.NumElements, false, true); break;
        case D3D11_SRV_DIMENSION_BUFFEREX: SetViewRange(o, r, d.Format, 0, 0, 1, d.BufferEx.FirstElement, d.BufferEx.NumElements, (d.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0, true); break;
        case D3D11_SRV_DIMENSION_TEXTURE2D: SetViewRange(o, r, d.Format, d.Texture2D.MostDetailedMip, 0, 1, 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY: SetViewRange(o, r, d.Format, d.Texture2DArray.MostDetailedMip, d.Texture2DArray.FirstArraySlice, SlicesOf(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_SRV_DIMENSION_TEXTURECUBE: SetViewRange(o, r, d.Format, d.TextureCube.MostDetailedMip, 0, 6, 0, 0, false, false); break;
        default: SetViewRange(o, r, d.Format, 0, 0, 1, 0, 0, false, false); break;
    }
    Announce(o);
    return hr;
}

void RegisterRenderTargetView(ID3D11Device5* This, ID3D11Resource* pResource, ID3D11RenderTargetView* view, const D3D11_RENDER_TARGET_VIEW_DESC& d, const std::string& descJson, const char* cmd) {
    HookView(view, "ID3D11RenderTargetView", slot::ID3D11RenderTargetView1_GetDesc + 1);
    Object& o = RegisterView(view, ObjKind::RenderTargetView, "ID3D11RenderTargetView", This, pResource, cmd);
    if (o.args.size() != 1) return;
    Describe(o, "pDesc", descJson);
    const Object* r = Find(pResource);
    switch (d.ViewDimension) {
        case D3D11_RTV_DIMENSION_BUFFER: SetViewRange(o, r, d.Format, 0, 0, 1, d.Buffer.FirstElement, d.Buffer.NumElements, false, true); break;
        case D3D11_RTV_DIMENSION_TEXTURE1D: SetViewRange(o, r, d.Format, d.Texture1D.MipSlice, 0, 1, 0, 0, false, false); break;
        case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: SetViewRange(o, r, d.Format, d.Texture1DArray.MipSlice, d.Texture1DArray.FirstArraySlice, SlicesOf(r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_RTV_DIMENSION_TEXTURE2D: SetViewRange(o, r, d.Format, d.Texture2D.MipSlice, 0, 1, 0, 0, false, false); break;
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: SetViewRange(o, r, d.Format, d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice, SlicesOf(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMS: SetViewRange(o, r, d.Format, 0, 0, 1, 0, 0, false, false); break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: SetViewRange(o, r, d.Format, 0, d.Texture2DMSArray.FirstArraySlice, SlicesOf(r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize), 0, 0, false, false); break;
        case D3D11_RTV_DIMENSION_TEXTURE3D: SetViewRange(o, r, d.Format, d.Texture3D.MipSlice, d.Texture3D.FirstWSlice, std::max(1u, d.Texture3D.WSize == (UINT)-1 ? 1u : d.Texture3D.WSize), 0, 0, false, false); break;
        default: SetViewRange(o, r, d.Format, 0, 0, 1, 0, 0, false, false); break;
    }
    Announce(o);
}

HRESULT STDMETHODCALLTYPE Hook_CreateRenderTargetView(ID3D11Device5* This, ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) {
    HRESULT hr = DEV(CreateRenderTargetView)(This, pResource, pDesc, ppRTView);
    if (Internal() || FAILED(hr) || !ppRTView || !*ppRTView) return hr;
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    { ScopedInternal internal; (*ppRTView)->GetDesc(&d); }
    JsonWriter w;
    Write(w, pDesc ? pDesc : &d);
    RegisterRenderTargetView(This, pResource, *ppRTView, d, w.str(), "CreateRenderTargetView");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateRenderTargetView1(ID3D11Device5* This, ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC1* pDesc1, ID3D11RenderTargetView1** ppRTView1) {
    HRESULT hr = DEV(CreateRenderTargetView1)(This, pResource, pDesc1, ppRTView1);
    if (Internal() || FAILED(hr) || !ppRTView1 || !*ppRTView1) return hr;
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    { ScopedInternal internal; (*ppRTView1)->GetDesc(&d); }
    JsonWriter w;
    if (pDesc1) Write(w, pDesc1); else Write(w, &d);
    RegisterRenderTargetView(This, pResource, *ppRTView1, d, w.str(), "CreateRenderTargetView1");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDepthStencilView(ID3D11Device5* This, ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView) {
    HRESULT hr = DEV(CreateDepthStencilView)(This, pResource, pDesc, ppDepthStencilView);
    if (Internal() || FAILED(hr) || !ppDepthStencilView || !*ppDepthStencilView) return hr;
    HookView(*ppDepthStencilView, "ID3D11DepthStencilView", slot::ID3D11DepthStencilView_Count);
    Object& o = RegisterView(*ppDepthStencilView, ObjKind::DepthStencilView, "ID3D11DepthStencilView", This, pResource, "CreateDepthStencilView");
    if (o.args.size() != 1) return hr;
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    { ScopedInternal internal; (*ppDepthStencilView)->GetDesc(&d); }
    JsonWriter w;
    Write(w, pDesc ? pDesc : &d);
    Describe(o, "pDesc", w.str());
    const Object* r = Find(pResource);
    o.readOnlyDepth = (d.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
    o.readOnlyStencil = (d.Flags & D3D11_DSV_READ_ONLY_STENCIL) != 0;
    switch (d.ViewDimension) {
        case D3D11_DSV_DIMENSION_TEXTURE1D: SetViewRange(o, r, d.Format, d.Texture1D.MipSlice, 0, 1, 0, 0, false, false); break;
        case D3D11_DSV_DIMENSION_TEXTURE1DARRAY: SetViewRange(o, r, d.Format, d.Texture1DArray.MipSlice, d.Texture1DArray.FirstArraySlice, SlicesOf(r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_DSV_DIMENSION_TEXTURE2D: SetViewRange(o, r, d.Format, d.Texture2D.MipSlice, 0, 1, 0, 0, false, false); break;
        case D3D11_DSV_DIMENSION_TEXTURE2DARRAY: SetViewRange(o, r, d.Format, d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice, SlicesOf(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMS: SetViewRange(o, r, d.Format, 0, 0, 1, 0, 0, false, false); break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY: SetViewRange(o, r, d.Format, 0, d.Texture2DMSArray.FirstArraySlice, SlicesOf(r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize), 0, 0, false, false); break;
        default: SetViewRange(o, r, d.Format, 0, 0, 1, 0, 0, false, false); break;
    }
    Announce(o);
    return hr;
}

void RegisterUnorderedAccessView(ID3D11Device5* This, ID3D11Resource* pResource, ID3D11UnorderedAccessView* view, const D3D11_UNORDERED_ACCESS_VIEW_DESC& d, const std::string& descJson, const char* cmd) {
    HookView(view, "ID3D11UnorderedAccessView", slot::ID3D11UnorderedAccessView1_GetDesc + 1);
    Object& o = RegisterView(view, ObjKind::UnorderedAccessView, "ID3D11UnorderedAccessView", This, pResource, cmd);
    if (o.args.size() != 1) return;
    Describe(o, "pDesc", descJson);
    const Object* r = Find(pResource);
    switch (d.ViewDimension) {
        case D3D11_UAV_DIMENSION_BUFFER: SetViewRange(o, r, d.Format, 0, 0, 1, d.Buffer.FirstElement, d.Buffer.NumElements, (d.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) != 0, true); break;
        case D3D11_UAV_DIMENSION_TEXTURE1D: SetViewRange(o, r, d.Format, d.Texture1D.MipSlice, 0, 1, 0, 0, false, false); break;
        case D3D11_UAV_DIMENSION_TEXTURE1DARRAY: SetViewRange(o, r, d.Format, d.Texture1DArray.MipSlice, d.Texture1DArray.FirstArraySlice, SlicesOf(r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_UAV_DIMENSION_TEXTURE2D: SetViewRange(o, r, d.Format, d.Texture2D.MipSlice, 0, 1, 0, 0, false, false); break;
        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY: SetViewRange(o, r, d.Format, d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice, SlicesOf(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize), 0, 0, false, false); break;
        case D3D11_UAV_DIMENSION_TEXTURE3D: SetViewRange(o, r, d.Format, d.Texture3D.MipSlice, d.Texture3D.FirstWSlice, std::max(1u, d.Texture3D.WSize == (UINT)-1 ? 1u : d.Texture3D.WSize), 0, 0, false, false); break;
        default: SetViewRange(o, r, d.Format, 0, 0, 1, 0, 0, false, false); break;
    }
    Announce(o);
}

HRESULT STDMETHODCALLTYPE Hook_CreateUnorderedAccessView(ID3D11Device5* This, ID3D11Resource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ID3D11UnorderedAccessView** ppUAView) {
    HRESULT hr = DEV(CreateUnorderedAccessView)(This, pResource, pDesc, ppUAView);
    if (Internal() || FAILED(hr) || !ppUAView || !*ppUAView) return hr;
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    { ScopedInternal internal; (*ppUAView)->GetDesc(&d); }
    JsonWriter w;
    Write(w, pDesc ? pDesc : &d);
    RegisterUnorderedAccessView(This, pResource, *ppUAView, d, w.str(), "CreateUnorderedAccessView");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateUnorderedAccessView1(ID3D11Device5* This, ID3D11Resource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC1* pDesc1, ID3D11UnorderedAccessView1** ppUAView1) {
    HRESULT hr = DEV(CreateUnorderedAccessView1)(This, pResource, pDesc1, ppUAView1);
    if (Internal() || FAILED(hr) || !ppUAView1 || !*ppUAView1) return hr;
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    { ScopedInternal internal; (*ppUAView1)->GetDesc(&d); }
    JsonWriter w;
    if (pDesc1) Write(w, pDesc1); else Write(w, &d);
    RegisterUnorderedAccessView(This, pResource, *ppUAView1, d, w.str(), "CreateUnorderedAccessView1");
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Input layouts and shaders

HRESULT STDMETHODCALLTYPE Hook_CreateInputLayout(ID3D11Device5* This, const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) {
    HRESULT hr = DEV(CreateInputLayout)(This, pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);
    if (Internal() || FAILED(hr) || !ppInputLayout || !*ppInputLayout) return hr;
    if (!VtableHooked(*ppInputLayout)) HookDeviceChild(*ppInputLayout, "ID3D11InputLayout", slot::ID3D11InputLayout_Count, {});
    Object& o = Track(*ppInputLayout, ObjKind::InputLayout, "ID3D11InputLayout", DeviceId(This), "CreateInputLayout");
    if (!o.args.empty()) return hr;
    JsonWriter w;
    Write(w, pInputElementDescs, NumElements);
    Describe(o, "pInputElementDescs", w.str());
    Describe(o, "BytecodeLength", JsonInt((int64_t)BytecodeLength));
    // Offsets resolved: an appended element starts at the next 4-byte boundary after the previous
    // element of its slot, which is what the read-backs and the mesh view need to know.
    UINT running[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    JsonWriter e;
    e.BeginArray();
    for (UINT i = 0; pInputElementDescs && i < NumElements; ++i) {
        const D3D11_INPUT_ELEMENT_DESC& d = pInputElementDescs[i];
        InputElement el;
        el.semanticName = d.SemanticName ? d.SemanticName : "";
        el.semanticIndex = d.SemanticIndex;
        el.format = d.Format;
        el.slot = d.InputSlot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ? d.InputSlot : 0;
        el.size = FormatOf(d.Format).bytes;
        el.offset = d.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? (running[el.slot] + 3) & ~3u : d.AlignedByteOffset;
        running[el.slot] = el.offset + el.size;
        el.perInstance = d.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA;
        el.stepRate = d.InstanceDataStepRate;
        e.BeginObject();
        e.Key("location"); e.Uint(i);
        e.Key("name"); e.String(el.semanticName + (el.semanticIndex ? std::to_string(el.semanticIndex) : std::string()));
        e.Key("slot"); e.Uint(el.slot);
        e.Key("offset"); e.Uint(el.offset);
        const FormatInfo f = FormatOf(d.Format);
        e.Key("format"); e.String(f.protocolName ? f.protocolName : "VK_FORMAT_UNDEFINED");
        e.Key("perInstance"); e.Boolean(el.perInstance);
        e.Key("stepRate"); e.Uint(el.stepRate);
        e.EndObject();
        o.elements.push_back(std::move(el));
    }
    e.EndArray();
    Describe(o, "elements", e.str());
    Announce(o);
    return hr;
}

/** Tracks a shader with its bytecode as a blob and its reflection in its description. */
void RegisterShader(ID3D11Device5* device, ID3D11DeviceChild* shader, const char* type, const char* cmd, const char* expectedStage,
                    const void* bytecode, SIZE_T length, ID3D11ClassLinkage* linkage) {
    if (!VtableHooked(shader)) HookDeviceChild(shader, type, slot::ID3D11VertexShader_Count, {});
    Object& o = Track(shader, ObjKind::Shader, type, DeviceId(device), cmd);
    if (!o.args.empty()) return;
    Describe(o, "BytecodeLength", JsonInt((int64_t)length));
    Describe(o, "pClassLinkage", JsonRef(linkage));
    ShaderInfo info = ReflectShader(bytecode, length);
    o.stage = info.stage.empty() || info.stage == "unknown" ? expectedStage : info.stage;
    Describe(o, "stage", JsonString(o.stage));
    if (!info.target.empty()) Describe(o, "target", JsonString(info.target));
    if (!info.reflectionJson.empty()) {
        // Keyed by stage, the shape the inspector reads reflection off any object in.
        Describe(o, "reflection", "{\"" + o.stage + "\":" + info.reflectionJson + "}");
    } else if (!info.error.empty()) {
        Describe(o, "reflectionError", JsonString(info.error));
    }
    if (bytecode && length) {
        AddBlob(o, o.stage + ":main", std::make_shared<std::vector<uint8_t>>((const uint8_t*)bytecode, (const uint8_t*)bytecode + length));
    }
    Announce(o);
    Log("%s -> %p (%s, %zu bytes)", cmd, (void*)shader, info.target.c_str(), (size_t)length);
}

#define SHADER_HOOK(Method, Type, Stage)                                                                                            \
    HRESULT STDMETHODCALLTYPE Hook_##Method(ID3D11Device5* This, const void* pShaderBytecode, SIZE_T BytecodeLength,                  \
                                            ID3D11ClassLinkage* pClassLinkage, Type** ppShader) {                                    \
        HRESULT hr = DEV(Method)(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppShader);                                    \
        if (Internal() || FAILED(hr) || !ppShader || !*ppShader) return hr;                                                          \
        RegisterShader(This, *ppShader, #Type, #Method, Stage, pShaderBytecode, BytecodeLength, pClassLinkage);                      \
        return hr;                                                                                                                   \
    }

SHADER_HOOK(CreateVertexShader, ID3D11VertexShader, "vertex")
SHADER_HOOK(CreatePixelShader, ID3D11PixelShader, "fragment")
SHADER_HOOK(CreateGeometryShader, ID3D11GeometryShader, "geometry")
SHADER_HOOK(CreateHullShader, ID3D11HullShader, "tess_control")
SHADER_HOOK(CreateDomainShader, ID3D11DomainShader, "tess_eval")
SHADER_HOOK(CreateComputeShader, ID3D11ComputeShader, "compute")

HRESULT STDMETHODCALLTYPE Hook_CreateGeometryShaderWithStreamOutput(ID3D11Device5* This, const void* pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries, const UINT* pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) {
    HRESULT hr = DEV(CreateGeometryShaderWithStreamOutput)(This, pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGeometryShader);
    if (Internal() || FAILED(hr) || !ppGeometryShader || !*ppGeometryShader) return hr;
    RegisterShader(This, *ppGeometryShader, "ID3D11GeometryShader", "CreateGeometryShaderWithStreamOutput", "geometry", pShaderBytecode, BytecodeLength, pClassLinkage);
    if (Object* o = Find(*ppGeometryShader)) {
        JsonWriter w;
        Write(w, pSODeclaration, NumEntries);
        Describe(*o, "pSODeclaration", w.str());
        JsonWriter s;
        WriteUints(s, pBufferStrides, NumStrides);
        Describe(*o, "pBufferStrides", s.str());
        Describe(*o, "RasterizedStream", JsonInt(RasterizedStream));
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// State objects

template <typename T, typename Desc>
Object* RegisterState(ID3D11Device5* device, T* state, ObjKind kind, const char* type, const char* cmd, uint32_t slots, const Desc* desc) {
    if (!state) return nullptr;
    if (!VtableHooked(state)) HookDeviceChild(state, type, slots, {});
    Object& o = Track(state, kind, type, DeviceId(device), cmd);
    if (!o.args.empty()) return nullptr;   // the runtime hands the same object out for an identical description
    if (desc) {
        JsonWriter w;
        Write(w, *desc);
        Describe(o, "pDesc", w.str());
    }
    return &o;
}

HRESULT STDMETHODCALLTYPE Hook_CreateBlendState(ID3D11Device5* This, const D3D11_BLEND_DESC* pBlendStateDesc, ID3D11BlendState** ppBlendState) {
    HRESULT hr = DEV(CreateBlendState)(This, pBlendStateDesc, ppBlendState);
    if (Internal() || FAILED(hr) || !ppBlendState) return hr;
    if (Object* o = RegisterState(This, *ppBlendState, ObjKind::BlendState, "ID3D11BlendState", "CreateBlendState", slot::ID3D11BlendState1_GetDesc + 1, pBlendStateDesc)) Announce(*o);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateBlendState1(ID3D11Device5* This, const D3D11_BLEND_DESC1* pBlendStateDesc, ID3D11BlendState1** ppBlendState) {
    HRESULT hr = DEV(CreateBlendState1)(This, pBlendStateDesc, ppBlendState);
    if (Internal() || FAILED(hr) || !ppBlendState) return hr;
    if (Object* o = RegisterState(This, *ppBlendState, ObjKind::BlendState, "ID3D11BlendState", "CreateBlendState1", slot::ID3D11BlendState1_Count, pBlendStateDesc)) Announce(*o);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDepthStencilState(ID3D11Device5* This, const D3D11_DEPTH_STENCIL_DESC* pDesc, ID3D11DepthStencilState** ppDepthStencilState) {
    HRESULT hr = DEV(CreateDepthStencilState)(This, pDesc, ppDepthStencilState);
    if (Internal() || FAILED(hr) || !ppDepthStencilState) return hr;
    if (Object* o = RegisterState(This, *ppDepthStencilState, ObjKind::DepthStencilState, "ID3D11DepthStencilState", "CreateDepthStencilState", slot::ID3D11DepthStencilState_Count, pDesc)) {
        if (pDesc) o->depthEnable = pDesc->DepthEnable != 0;
        Announce(*o);
    }
    return hr;
}

template <typename Desc>
void NoteRasterizer(Object* o, const Desc* d) {
    if (!o || !d) return;
    o->cullMode = d->CullMode;
    o->frontCounterClockwise = d->FrontCounterClockwise != 0;
    o->scissorEnable = d->ScissorEnable != 0;
}

HRESULT STDMETHODCALLTYPE Hook_CreateRasterizerState(ID3D11Device5* This, const D3D11_RASTERIZER_DESC* pDesc, ID3D11RasterizerState** ppRasterizerState) {
    HRESULT hr = DEV(CreateRasterizerState)(This, pDesc, ppRasterizerState);
    if (Internal() || FAILED(hr) || !ppRasterizerState) return hr;
    if (Object* o = RegisterState(This, *ppRasterizerState, ObjKind::RasterizerState, "ID3D11RasterizerState", "CreateRasterizerState", slot::ID3D11RasterizerState2_GetDesc + 1, pDesc)) { NoteRasterizer(o, pDesc); Announce(*o); }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateRasterizerState1(ID3D11Device5* This, const D3D11_RASTERIZER_DESC1* pDesc, ID3D11RasterizerState1** ppRasterizerState) {
    HRESULT hr = DEV(CreateRasterizerState1)(This, pDesc, ppRasterizerState);
    if (Internal() || FAILED(hr) || !ppRasterizerState) return hr;
    if (Object* o = RegisterState(This, *ppRasterizerState, ObjKind::RasterizerState, "ID3D11RasterizerState", "CreateRasterizerState1", slot::ID3D11RasterizerState2_GetDesc1 + 1, pDesc)) { NoteRasterizer(o, pDesc); Announce(*o); }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateRasterizerState2(ID3D11Device5* This, const D3D11_RASTERIZER_DESC2* pDesc, ID3D11RasterizerState2** ppRasterizerState) {
    HRESULT hr = DEV(CreateRasterizerState2)(This, pDesc, ppRasterizerState);
    if (Internal() || FAILED(hr) || !ppRasterizerState) return hr;
    if (Object* o = RegisterState(This, *ppRasterizerState, ObjKind::RasterizerState, "ID3D11RasterizerState", "CreateRasterizerState2", slot::ID3D11RasterizerState2_Count, pDesc)) { NoteRasterizer(o, pDesc); Announce(*o); }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSamplerState(ID3D11Device5* This, const D3D11_SAMPLER_DESC* pSamplerDesc, ID3D11SamplerState** ppSamplerState) {
    HRESULT hr = DEV(CreateSamplerState)(This, pSamplerDesc, ppSamplerState);
    if (Internal() || FAILED(hr) || !ppSamplerState) return hr;
    if (Object* o = RegisterState(This, *ppSamplerState, ObjKind::SamplerState, "ID3D11SamplerState", "CreateSamplerState", slot::ID3D11SamplerState_Count, pSamplerDesc)) Announce(*o);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateQuery(ID3D11Device5* This, const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) {
    HRESULT hr = DEV(CreateQuery)(This, pQueryDesc, ppQuery);
    if (Internal() || FAILED(hr) || !ppQuery) return hr;
    if (Object* o = RegisterState(This, *ppQuery, ObjKind::Query, "ID3D11Query", "CreateQuery", slot::ID3D11Query1_GetDesc + 1, pQueryDesc)) Announce(*o);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateQuery1(ID3D11Device5* This, const D3D11_QUERY_DESC1* pQueryDesc1, ID3D11Query1** ppQuery1) {
    HRESULT hr = DEV(CreateQuery1)(This, pQueryDesc1, ppQuery1);
    if (Internal() || FAILED(hr) || !ppQuery1) return hr;
    if (Object* o = RegisterState(This, *ppQuery1, ObjKind::Query, "ID3D11Query", "CreateQuery1", slot::ID3D11Query1_Count, pQueryDesc1)) Announce(*o);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePredicate(ID3D11Device5* This, const D3D11_QUERY_DESC* pPredicateDesc, ID3D11Predicate** ppPredicate) {
    HRESULT hr = DEV(CreatePredicate)(This, pPredicateDesc, ppPredicate);
    if (Internal() || FAILED(hr) || !ppPredicate) return hr;
    if (Object* o = RegisterState(This, *ppPredicate, ObjKind::Query, "ID3D11Predicate", "CreatePredicate", slot::ID3D11Predicate_Count, pPredicateDesc)) Announce(*o);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCounter(ID3D11Device5* This, const D3D11_COUNTER_DESC* pCounterDesc, ID3D11Counter** ppCounter) {
    HRESULT hr = DEV(CreateCounter)(This, pCounterDesc, ppCounter);
    if (Internal() || FAILED(hr) || !ppCounter) return hr;
    if (Object* o = RegisterState(This, *ppCounter, ObjKind::Query, "ID3D11Counter", "CreateCounter", slot::ID3D11Counter_Count, pCounterDesc)) Announce(*o);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateClassLinkage(ID3D11Device5* This, ID3D11ClassLinkage** ppLinkage) {
    HRESULT hr = DEV(CreateClassLinkage)(This, ppLinkage);
    if (Internal() || FAILED(hr) || !ppLinkage) return hr;
    if (Object* o = RegisterState<ID3D11ClassLinkage, D3D11_QUERY_DESC>(This, *ppLinkage, ObjKind::ClassLinkage, "ID3D11ClassLinkage", "CreateClassLinkage", slot::ID3D11ClassLinkage_Count, nullptr)) Announce(*o);
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Contexts

HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext(ID3D11Device5* This, UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext) {
    HRESULT hr = DEV(CreateDeferredContext)(This, ContextFlags, ppDeferredContext);
    if (Internal() || FAILED(hr) || !ppDeferredContext || !*ppDeferredContext) return hr;
    *ppDeferredContext = (ID3D11DeviceContext*)WrapContext(*ppDeferredContext, This, "CreateDeferredContext");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext1(ID3D11Device5* This, UINT ContextFlags, ID3D11DeviceContext1** ppDeferredContext) {
    HRESULT hr = DEV(CreateDeferredContext1)(This, ContextFlags, ppDeferredContext);
    if (Internal() || FAILED(hr) || !ppDeferredContext || !*ppDeferredContext) return hr;
    *ppDeferredContext = (ID3D11DeviceContext1*)WrapContext(*ppDeferredContext, This, "CreateDeferredContext1");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext2(ID3D11Device5* This, UINT ContextFlags, ID3D11DeviceContext2** ppDeferredContext) {
    HRESULT hr = DEV(CreateDeferredContext2)(This, ContextFlags, ppDeferredContext);
    if (Internal() || FAILED(hr) || !ppDeferredContext || !*ppDeferredContext) return hr;
    *ppDeferredContext = (ID3D11DeviceContext2*)WrapContext(*ppDeferredContext, This, "CreateDeferredContext2");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext3(ID3D11Device5* This, UINT ContextFlags, ID3D11DeviceContext3** ppDeferredContext) {
    HRESULT hr = DEV(CreateDeferredContext3)(This, ContextFlags, ppDeferredContext);
    if (Internal() || FAILED(hr) || !ppDeferredContext || !*ppDeferredContext) return hr;
    *ppDeferredContext = (ID3D11DeviceContext3*)WrapContext(*ppDeferredContext, This, "CreateDeferredContext3");
    return hr;
}

// The immediate context, however it is asked for: the proxy. The context is tracked at device
// creation, so this only substitutes.
#define GET_IMMEDIATE(Method, Type)                                                                 void STDMETHODCALLTYPE Hook_##Method(ID3D11Device5* This, Type** ppImmediateContext) {             DEV(Method)(This, ppImmediateContext);                                                          if (Internal() || !ppImmediateContext || !*ppImmediateContext) return;                          *ppImmediateContext = (Type*)WrapContext(*ppImmediateContext, This, #Method);               }
GET_IMMEDIATE(GetImmediateContext, ID3D11DeviceContext)
GET_IMMEDIATE(GetImmediateContext1, ID3D11DeviceContext1)
GET_IMMEDIATE(GetImmediateContext2, ID3D11DeviceContext2)
GET_IMMEDIATE(GetImmediateContext3, ID3D11DeviceContext3)

HRESULT STDMETHODCALLTYPE Hook_DeviceSetPrivateData(ID3D11Device5* This, REFGUID guid, UINT size, const void* data) {
    HRESULT hr = DEV(SetPrivateData)(This, guid, size, data);
    if (SUCCEEDED(hr) && !Internal() && data && size) {
        if (guid == WKPDID_D3DDebugObjectName) OnObjectNamed(This, std::string((const char*)data, ((const char*)data)[size - 1] == 0 ? size - 1 : size));
        else if (guid == WKPDID_D3DDebugObjectNameW) OnObjectNamed(This, Narrow((const wchar_t*)data, size / sizeof(wchar_t)));
    }
    return hr;
}

ULONG STDMETHODCALLTYPE Hook_DeviceRelease(ID3D11Device5* This) {
    const bool tracked = !Internal() && Find(This) != nullptr;
    ULONG count = DEV(Release)(This);
    if (count == 0 && tracked) {
        DestroyContextsOf(This);
        OnObjectReleased(This);
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// The entry points

typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

PFN_D3D11CreateDevice g_D3D11CreateDevice = nullptr;
PFN_D3D11CreateDeviceAndSwapChain g_D3D11CreateDeviceAndSwapChain = nullptr;

/**
 * Whether the caller of an entry point is ANGLE's libGLESv2.dll. ANGLE's Direct3D 11 backend is
 * what an Electron or Chromium application runs OpenGL ES on, and the OpenGL ES plugin captures
 * that; the device underneath it is left alone unless D3D11INSP_ANGLE=1 asks for it, so the two
 * libraries do not both open the session's port.
 */
bool CallerIsAngle(void* returnAddress) {
    static const bool wanted = gpuinsp::sdk::Config::Get().Flag("D3D11INSP_ANGLE");
    if (wanted) return false;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)returnAddress, &m) || !m) return false;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return _wcsicmp(base, L"libGLESv2.dll") == 0;
}

std::string CreateArgs(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, D3D_FEATURE_LEVEL level) {
    Args a;
    a.ptr("pAdapter", pAdapter);
    a.e("DriverType", ToString_D3D_DRIVER_TYPE(DriverType), DriverType);
    a.key("Flags"); Flags_D3D11_CREATE_DEVICE_FLAG(a.writer(), Flags);
    a.key("pFeatureLevels");
    a.writer().BeginArray();
    for (UINT i = 0; pFeatureLevels && i < FeatureLevels; ++i) a.writer().Enum(ToString_D3D_FEATURE_LEVEL(pFeatureLevels[i]), pFeatureLevels[i]);
    a.writer().EndArray();
    a.u("SDKVersion", SDKVersion);
    a.e("featureLevel", ToString_D3D_FEATURE_LEVEL(level), level);
    return a.str();
}

HRESULT WINAPI Hook_D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    void* caller = _ReturnAddress();
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = g_D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, &device, &level, &context);
    if (pFeatureLevel) *pFeatureLevel = level;
    if (ppDevice) *ppDevice = device; else if (device) device->Release();
    if (ppImmediateContext) *ppImmediateContext = context; else if (context) context->Release();
    if (Internal() || FAILED(hr) || !device) return hr;
    if (CallerIsAngle(caller)) {
        LogAlways("D3D11CreateDevice from ANGLE's libGLESv2.dll: the device is left to the OpenGL ES plugin (D3D11INSP_ANGLE=1 captures it)");
        return hr;
    }
    OnDeviceCreated(device, ppImmediateContext, pAdapter, "D3D11CreateDevice", CreateArgs(pAdapter, DriverType, Flags, pFeatureLevels, FeatureLevels, SDKVersion, level));
    return hr;
}

HRESULT WINAPI Hook_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    void* caller = _ReturnAddress();
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    HRESULT hr = g_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, pSwapChainDesc ? &swapChain : nullptr, &device, &level, &context);
    if (pFeatureLevel) *pFeatureLevel = level;
    if (ppDevice) *ppDevice = device; else if (device) device->Release();
    if (ppImmediateContext) *ppImmediateContext = context; else if (context) context->Release();
    if (ppSwapChain) *ppSwapChain = swapChain; else if (swapChain) swapChain->Release();
    if (Internal() || FAILED(hr) || !device) return hr;
    if (CallerIsAngle(caller)) {
        LogAlways("D3D11CreateDeviceAndSwapChain from ANGLE's libGLESv2.dll: the device is left to the OpenGL ES plugin (D3D11INSP_ANGLE=1 captures it)");
        return hr;
    }
    OnDeviceCreated(device, ppImmediateContext, pAdapter, "D3D11CreateDeviceAndSwapChain", CreateArgs(pAdapter, DriverType, Flags, pFeatureLevels, FeatureLevels, SDKVersion, level));
    if (swapChain && ppSwapChain) {
        Args a;
        a.ref("pDevice", device, "ID3D11Device");
        if (pSwapChainDesc) Write(a.key("pSwapChainDesc"), *pSwapChainDesc); else a.null("pSwapChainDesc");
        OnSwapChainCreated(swapChain, device, "D3D11CreateDeviceAndSwapChain", a.str());
    }
    return hr;
}

}  // namespace

bool InstallEntryPointHooks(HMODULE d3d11) {
    void* create = (void*)GetProcAddress(d3d11, "D3D11CreateDevice");
    void* createAndSwap = (void*)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!create || !createAndSwap) {
        LogAlways("d3d11.dll exports D3D11CreateDevice%s and D3D11CreateDeviceAndSwapChain%s: no Direct3D 11 capture", create ? "" : " (missing)", createAndSwap ? "" : " (missing)");
        return false;
    }
    bool ok = HookFunction(create, (void*)&Hook_D3D11CreateDevice, (void**)&g_D3D11CreateDevice, "D3D11CreateDevice");
    ok = HookFunction(createAndSwap, (void*)&Hook_D3D11CreateDeviceAndSwapChain, (void**)&g_D3D11CreateDeviceAndSwapChain, "D3D11CreateDeviceAndSwapChain") && ok;
    return EnableFunctionHooks() && ok;
}

void HookDevice(ID3D11Device* device) {
    if (!device || VtableHooked(device)) return;
    // ID3D11Device: 43 slots; 1: 54; 2: 58; 3: 68; 4: 69 (RegisterDeviceRemovedEvent, UnregisterDeviceRemoved); 5: 71.
    static const VersionCount versions[] = {
        {&__uuidof(ID3D11Device5), slot::ID3D11Device5_Count},
        {&__uuidof(ID3D11Device4), slot::ID3D11Device5_UnregisterDeviceRemoved + 1},
        {&__uuidof(ID3D11Device3), slot::ID3D11Device5_ReadFromSubresource + 1},
        {&__uuidof(ID3D11Device2), slot::ID3D11Device5_CheckMultisampleQualityLevels1 + 1},
        {&__uuidof(ID3D11Device1), slot::ID3D11Device5_OpenSharedResourceByName + 1},
    };
    const uint32_t count = VtableCount(device, versions, 5, slot::ID3D11Device5_GetExceptionMode + 1);
    HookVtable(device, "ID3D11Device", count, {
        {slot::ID3D11Device5_Release, (void*)&Hook_DeviceRelease},
        {slot::ID3D11Device5_SetPrivateData, (void*)&Hook_DeviceSetPrivateData},
        {slot::ID3D11Device5_CreateBuffer, (void*)&Hook_CreateBuffer},
        {slot::ID3D11Device5_CreateTexture1D, (void*)&Hook_CreateTexture1D},
        {slot::ID3D11Device5_CreateTexture2D, (void*)&Hook_CreateTexture2D},
        {slot::ID3D11Device5_CreateTexture3D, (void*)&Hook_CreateTexture3D},
        {slot::ID3D11Device5_CreateShaderResourceView, (void*)&Hook_CreateShaderResourceView},
        {slot::ID3D11Device5_CreateUnorderedAccessView, (void*)&Hook_CreateUnorderedAccessView},
        {slot::ID3D11Device5_CreateRenderTargetView, (void*)&Hook_CreateRenderTargetView},
        {slot::ID3D11Device5_CreateDepthStencilView, (void*)&Hook_CreateDepthStencilView},
        {slot::ID3D11Device5_CreateInputLayout, (void*)&Hook_CreateInputLayout},
        {slot::ID3D11Device5_CreateVertexShader, (void*)&Hook_CreateVertexShader},
        {slot::ID3D11Device5_CreateGeometryShader, (void*)&Hook_CreateGeometryShader},
        {slot::ID3D11Device5_CreateGeometryShaderWithStreamOutput, (void*)&Hook_CreateGeometryShaderWithStreamOutput},
        {slot::ID3D11Device5_CreatePixelShader, (void*)&Hook_CreatePixelShader},
        {slot::ID3D11Device5_CreateHullShader, (void*)&Hook_CreateHullShader},
        {slot::ID3D11Device5_CreateDomainShader, (void*)&Hook_CreateDomainShader},
        {slot::ID3D11Device5_CreateComputeShader, (void*)&Hook_CreateComputeShader},
        {slot::ID3D11Device5_CreateClassLinkage, (void*)&Hook_CreateClassLinkage},
        {slot::ID3D11Device5_CreateBlendState, (void*)&Hook_CreateBlendState},
        {slot::ID3D11Device5_CreateDepthStencilState, (void*)&Hook_CreateDepthStencilState},
        {slot::ID3D11Device5_CreateRasterizerState, (void*)&Hook_CreateRasterizerState},
        {slot::ID3D11Device5_CreateSamplerState, (void*)&Hook_CreateSamplerState},
        {slot::ID3D11Device5_CreateQuery, (void*)&Hook_CreateQuery},
        {slot::ID3D11Device5_CreatePredicate, (void*)&Hook_CreatePredicate},
        {slot::ID3D11Device5_CreateCounter, (void*)&Hook_CreateCounter},
        {slot::ID3D11Device5_CreateDeferredContext, (void*)&Hook_CreateDeferredContext},
        {slot::ID3D11Device5_GetImmediateContext, (void*)&Hook_GetImmediateContext},
        {slot::ID3D11Device5_GetImmediateContext1, (void*)&Hook_GetImmediateContext1},
        {slot::ID3D11Device5_GetImmediateContext2, (void*)&Hook_GetImmediateContext2},
        {slot::ID3D11Device5_GetImmediateContext3, (void*)&Hook_GetImmediateContext3},
        // ID3D11Device1
        {slot::ID3D11Device5_CreateDeferredContext1, (void*)&Hook_CreateDeferredContext1},
        {slot::ID3D11Device5_CreateBlendState1, (void*)&Hook_CreateBlendState1},
        {slot::ID3D11Device5_CreateRasterizerState1, (void*)&Hook_CreateRasterizerState1},
        // ID3D11Device2
        {slot::ID3D11Device5_CreateDeferredContext2, (void*)&Hook_CreateDeferredContext2},
        // ID3D11Device3
        {slot::ID3D11Device5_CreateTexture2D1, (void*)&Hook_CreateTexture2D1},
        {slot::ID3D11Device5_CreateTexture3D1, (void*)&Hook_CreateTexture3D1},
        {slot::ID3D11Device5_CreateRasterizerState2, (void*)&Hook_CreateRasterizerState2},
        {slot::ID3D11Device5_CreateShaderResourceView1, (void*)&Hook_CreateShaderResourceView1},
        {slot::ID3D11Device5_CreateUnorderedAccessView1, (void*)&Hook_CreateUnorderedAccessView1},
        {slot::ID3D11Device5_CreateRenderTargetView1, (void*)&Hook_CreateRenderTargetView1},
        {slot::ID3D11Device5_CreateQuery1, (void*)&Hook_CreateQuery1},
        {slot::ID3D11Device5_CreateDeferredContext3, (void*)&Hook_CreateDeferredContext3},
    });
}

void OnDeviceCreated(ID3D11Device* device, ID3D11DeviceContext** immediate, IDXGIAdapter* adapter, const char* cmd, const std::string& args) {
    HookDevice(device);
    Object& o = Track(device, ObjKind::Device, "ID3D11Device", 0, cmd);
    if (o.args.empty()) {
        o.args.push_back({"createArgs", args});
        // The adapter: the one given, else the device's own.
        ComPtr<IDXGIAdapter> owned;
        if (!adapter) {
            ScopedInternal internal;
            ComPtr<IDXGIDevice> dxgi;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgi.put()))) && dxgi) dxgi->GetAdapter(owned.put());
            adapter = owned.get();
        }
        if (adapter) {
            DXGI_ADAPTER_DESC ad{};
            ScopedInternal internal;
            if (SUCCEEDED(adapter->GetDesc(&ad))) {
                JsonWriter a;
                Write(a, ad);
                Describe(o, "adapter", a.str());
            }
        }
        Describe(o, "featureLevel", JsonString(ToString_D3D_FEATURE_LEVEL(device->GetFeatureLevel()) ? ToString_D3D_FEATURE_LEVEL(device->GetFeatureLevel()) : "?"));
        Announce(o);
        LogAlways("%s -> ID3D11Device %p", cmd, (void*)device);
    }
    // The immediate context, tracked whether or not the application asked for it; what the
    // application gets is the proxy.
    ID3D11DeviceContext* real = immediate ? *immediate : nullptr;
    if (!real) {
        ScopedInternal internal;
        device->GetImmediateContext(&real);
        if (real) real->Release();
    }
    if (real) {
        ID3D11DeviceContext* proxy = WrapContext(real, device, "GetImmediateContext");
        if (immediate && *immediate) *immediate = proxy;
    }
    HookFactoryOfDevice(device);
}

}  // namespace d3d11insp
