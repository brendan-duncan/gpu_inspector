// Swap chains and the factories that make them. A swap chain is where a Direct3D 11 application's
// frames end (Present), and its back buffers are the textures the last pass of a frame draws into.
//
// No DXGI export is hooked: every factory object shares one vtable, so the factory a device's
// adapter belongs to, asked for at device creation, is patched once and the swap chains made
// through any factory are seen. The D3D12 library hooks the CreateDXGIFactory exports itself, and
// this library staying off them keeps the two from chaining on the same function.
#include "hooks.h"

#include "../gen/d3d11_enums.gen.h"
#include "../gen/d3d11_vtables.gen.h"
#include "capture.h"
#include "formats.h"
#include "serialize.h"
#include "server.h"
#include "state.h"

namespace d3d11insp
{

namespace
{

#define SWAP(name) Orig<PFN_IDXGISwapChain4_##name>(This, slot::IDXGISwapChain4_##name)
#define FACTORY(name) Orig<PFN_IDXGIFactory7_##name>(This, slot::IDXGIFactory7_##name)

/** The D3D11 device a swap chain was created for, or null when it was made for something else (a D3D12 queue). */
ID3D11Device* DeviceOf(IUnknown* pDevice)
{
    if (!pDevice)
        return nullptr;
    ID3D11Device* device = nullptr;
    ScopedInternal internal;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&device))) || !device)
        return nullptr;
    device->Release();
    return Find(device) ? device : nullptr;
}

/** The immediate context of the device a swap chain belongs to: the stream its Present is recorded on. */
Context* ContextOfSwapChain(IDXGISwapChain* swapChain)
{
    Object* o = Find(swapChain);
    if (!o || !o->device)
        return nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    {
        ScopedInternal internal;
        o->device->GetImmediateContext(&ctx);
        if (ctx)
            ctx->Release();
    }
    return ctx ? FindContext(ctx) : nullptr;
}

/** The back buffers, as many as the swap chain hands out (a flip-model chain gives only buffer 0 to D3D11). */
void TrackBackBuffers(IDXGISwapChain* swapChain)
{
    Object* sc = Find(swapChain);
    if (!sc)
        return;
    DXGI_SWAP_CHAIN_DESC desc{};
    ScopedInternal internal;
    swapChain->GetDesc(&desc);
    for (UINT i = 0; i < std::max(1u, desc.BufferCount); ++i)
    {
        ID3D11Texture2D* buffer = nullptr;
        if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer))) || !buffer)
            break;
        D3D11_TEXTURE2D_DESC td{};
        buffer->GetDesc(&td);
        if (!VtableHooked(buffer))
            HookDeviceChild(buffer, "ID3D11Texture2D", slot::ID3D11Texture2D1_GetDesc + 1, {});
        Object& o = Track(buffer, ObjKind::Texture2D, "ID3D11Texture2D", sc->id, "GetBuffer");
        if (o.args.empty())
        {
            JsonWriter w;
            Write(w, td);
            Describe(o, "pDesc", w.str());
            Describe(o, "Buffer", JsonInt(i));
            Describe(o, "swapChain", JsonRef(sc->id, "IDXGISwapChain"));
            o.format = td.Format;
            o.width = td.Width;
            o.height = td.Height;
            o.depth = td.ArraySize;
            o.mips = td.MipLevels;
            o.samples = td.SampleDesc.Count;
            o.usage = td.Usage;
            o.bindFlags = td.BindFlags;
            o.miscFlags = td.MiscFlags;
            o.swapChain = sc->id;
            const FormatInfo f = FormatOf(TypedFormat(td.Format, false));
            Describe(o, "format", JsonString(f.protocolName ? f.protocolName : "VK_FORMAT_UNDEFINED"));
            Announce(o);
        }
        buffer->Release();
    }
}

// ---------------------------------------------------------------------------------------------
// IDXGISwapChain

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain4* This, UINT SyncInterval, UINT Flags)
{
    if (Internal() || (Flags & DXGI_PRESENT_TEST))
        return SWAP(Present)(This, SyncInterval, Flags);
    Context* c = ContextOfSwapChain(This);
    if (c)
    {
        Args a;
        a.ref("swapChain", This, "IDXGISwapChain").u("SyncInterval", SyncInterval).u("Flags", Flags);
        BeforePresent(This, c, "Present", a.str());
    }
    HRESULT hr = SWAP(Present)(This, SyncInterval, Flags);
    if (c)
        AfterPresent(c);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain4* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (Internal() || (PresentFlags & DXGI_PRESENT_TEST))
        return SWAP(Present1)(This, SyncInterval, PresentFlags, pPresentParameters);
    Context* c = ContextOfSwapChain(This);
    if (c)
    {
        Args a;
        a.ref("swapChain", This, "IDXGISwapChain").u("SyncInterval", SyncInterval).u("PresentFlags", PresentFlags);
        if (pPresentParameters)
            Write(a.key("pPresentParameters"), *pPresentParameters);
        else
            a.null("pPresentParameters");
        BeforePresent(This, c, "Present1", a.str());
    }
    HRESULT hr = SWAP(Present1)(This, SyncInterval, PresentFlags, pPresentParameters);
    if (c)
        AfterPresent(c);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain4* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    const bool ours = !Internal() && Find(This) != nullptr;
    if (ours)
        UntrackBackBuffers(IdOf(This));
    HRESULT hr = SWAP(ResizeBuffers)(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    if (!ours)
        return hr;
    if (Object* o = Find(This))
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        {
            ScopedInternal internal;
            This->GetDesc(&desc);
        }
        JsonWriter w;
        Write(w, desc);
        Describe(*o, "pDesc", w.str());
        o->width = desc.BufferDesc.Width;
        o->height = desc.BufferDesc.Height;
        o->format = desc.BufferDesc.Format;
    }
    if (SUCCEEDED(hr))
        TrackBackBuffers(This);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers1(IDXGISwapChain4* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    const bool ours = !Internal() && Find(This) != nullptr;
    if (ours)
        UntrackBackBuffers(IdOf(This));
    HRESULT hr = SWAP(ResizeBuffers1)(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    if (ours && SUCCEEDED(hr))
        TrackBackBuffers(This);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_GetBuffer(IDXGISwapChain4* This, UINT Buffer, REFIID riid, void** ppSurface)
{
    HRESULT hr = SWAP(GetBuffer)(This, Buffer, riid, ppSurface);
    if (Internal() || FAILED(hr) || !ppSurface || !*ppSurface || !Find(This))
        return hr;
    // Whatever interface was asked for, the texture behind it is what is tracked.
    ID3D11Texture2D* tex = nullptr;
    {
        ScopedInternal internal;
        if (FAILED(((IUnknown*)*ppSurface)->QueryInterface(IID_PPV_ARGS(&tex))) || !tex)
            return hr;
        tex->Release();
    }
    if (!Find(tex))
        TrackBackBuffers(This);
    return hr;
}

// ---------------------------------------------------------------------------------------------
// IDXGIFactory

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory7* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    HRESULT hr = FACTORY(CreateSwapChain)(This, pDevice, pDesc, ppSwapChain);
    if (Internal() || FAILED(hr) || !ppSwapChain || !*ppSwapChain)
        return hr;
    ID3D11Device* device = DeviceOf(pDevice);
    if (!device)
        return hr;
    Args a;
    a.ref("pDevice", device, "ID3D11Device");
    if (pDesc)
        Write(a.key("pDesc"), *pDesc);
    else
        a.null("pDesc");
    OnSwapChainCreated(*ppSwapChain, device, "CreateSwapChain", a.str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory7* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    HRESULT hr = FACTORY(CreateSwapChainForHwnd)(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (Internal() || FAILED(hr) || !ppSwapChain || !*ppSwapChain)
        return hr;
    ID3D11Device* device = DeviceOf(pDevice);
    if (!device)
        return hr;
    Args a;
    a.ref("pDevice", device, "ID3D11Device").ptr("hWnd", hWnd);
    if (pDesc)
        Write(a.key("pDesc"), *pDesc);
    else
        a.null("pDesc");
    if (pFullscreenDesc)
        Write(a.key("pFullscreenDesc"), *pFullscreenDesc);
    else
        a.null("pFullscreenDesc");
    a.ptr("pRestrictToOutput", pRestrictToOutput);
    OnSwapChainCreated(*ppSwapChain, device, "CreateSwapChainForHwnd", a.str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForCoreWindow(IDXGIFactory7* This, IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    HRESULT hr = FACTORY(CreateSwapChainForCoreWindow)(This, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    if (Internal() || FAILED(hr) || !ppSwapChain || !*ppSwapChain)
        return hr;
    ID3D11Device* device = DeviceOf(pDevice);
    if (!device)
        return hr;
    Args a;
    a.ref("pDevice", device, "ID3D11Device").ptr("pWindow", pWindow);
    if (pDesc)
        Write(a.key("pDesc"), *pDesc);
    else
        a.null("pDesc");
    OnSwapChainCreated(*ppSwapChain, device, "CreateSwapChainForCoreWindow", a.str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForComposition(IDXGIFactory7* This, IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    HRESULT hr = FACTORY(CreateSwapChainForComposition)(This, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    if (Internal() || FAILED(hr) || !ppSwapChain || !*ppSwapChain)
        return hr;
    ID3D11Device* device = DeviceOf(pDevice);
    if (!device)
        return hr;
    Args a;
    a.ref("pDevice", device, "ID3D11Device");
    if (pDesc)
        Write(a.key("pDesc"), *pDesc);
    else
        a.null("pDesc");
    OnSwapChainCreated(*ppSwapChain, device, "CreateSwapChainForComposition", a.str());
    return hr;
}

}  // namespace

void UntrackBackBuffers(uint64_t swapChainId)
{
    LibraryState& s = State();
    std::lock_guard lock(s.mutex);
    std::vector<const void*> gone;
    for (const auto& [id, o] : s.objects)
        if (o->swapChain == swapChainId)
            gone.push_back(o->ptr);
    for (const void* p : gone)
        Untrack(p);
}

void HookSwapChain(IDXGISwapChain* swapChain)
{
    if (!swapChain || VtableHooked(swapChain))
        return;
    // IDXGISwapChain: 18 slots; 1: 29; 2: 36; 3: 40; 4: 41.
    static const VersionCount versions[] = {
        {&__uuidof(IDXGISwapChain4), slot::IDXGISwapChain4_Count},
        {&__uuidof(IDXGISwapChain3), slot::IDXGISwapChain4_ResizeBuffers1 + 1},
        {&__uuidof(IDXGISwapChain2), slot::IDXGISwapChain4_GetMatrixTransform + 1},
        {&__uuidof(IDXGISwapChain1), slot::IDXGISwapChain4_GetRotation + 1},
    };
    const uint32_t count = VtableCount(swapChain, versions, 4, slot::IDXGISwapChain4_GetLastPresentCount + 1);
    // clang-format off
    HookDxgiObject(swapChain, "IDXGISwapChain", count, {
        {slot::IDXGISwapChain4_Present, (void*)&Hook_Present},
        {slot::IDXGISwapChain4_Present1, (void*)&Hook_Present1},
        {slot::IDXGISwapChain4_ResizeBuffers, (void*)&Hook_ResizeBuffers},
        {slot::IDXGISwapChain4_ResizeBuffers1, (void*)&Hook_ResizeBuffers1},
        {slot::IDXGISwapChain4_GetBuffer, (void*)&Hook_GetBuffer},
    });
    // clang-format on
}

void HookFactory(IDXGIFactory* factory)
{
    if (!factory || VtableHooked(factory))
        return;
    // IDXGIFactory: 13 slots; 1: 15; 2: 25; 3: 26; 4: 28; 5: 29; 6: 30; 7: 32.
    static const VersionCount versions[] = {
        {&__uuidof(IDXGIFactory7), slot::IDXGIFactory7_Count},
        {&__uuidof(IDXGIFactory6), slot::IDXGIFactory7_EnumAdapterByGpuPreference + 1},
        {&__uuidof(IDXGIFactory5), slot::IDXGIFactory7_CheckFeatureSupport + 1},
        {&__uuidof(IDXGIFactory4), slot::IDXGIFactory7_EnumWarpAdapter + 1},
        {&__uuidof(IDXGIFactory3), slot::IDXGIFactory7_GetCreationFlags + 1},
        {&__uuidof(IDXGIFactory2), slot::IDXGIFactory7_CreateSwapChainForComposition + 1},
        {&__uuidof(IDXGIFactory1), slot::IDXGIFactory7_IsCurrent + 1},
    };
    const uint32_t count = VtableCount(factory, versions, 7, slot::IDXGIFactory7_CreateSoftwareAdapter + 1);
    // clang-format off
    HookVtable(factory, "IDXGIFactory", count, {
        {slot::IDXGIFactory7_CreateSwapChain, (void*)&Hook_CreateSwapChain},
        {slot::IDXGIFactory7_CreateSwapChainForHwnd, (void*)&Hook_CreateSwapChainForHwnd},
        {slot::IDXGIFactory7_CreateSwapChainForCoreWindow, (void*)&Hook_CreateSwapChainForCoreWindow},
        {slot::IDXGIFactory7_CreateSwapChainForComposition, (void*)&Hook_CreateSwapChainForComposition},
    });
    // clang-format on
}

void HookFactoryOfDevice(ID3D11Device* device)
{
    ScopedInternal internal;
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi.put()))) || !dxgi)
        return;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi->GetAdapter(adapter.put())) || !adapter)
        return;
    ComPtr<IDXGIFactory> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.put()))) || !factory)
        return;
    HookFactory(factory.get());
}

void OnSwapChainCreated(IDXGISwapChain* created, ID3D11Device* device, const char* cmd, const std::string& args)
{
    IDXGISwapChain* swapChain = nullptr;
    {
        ScopedInternal internal;
        if (FAILED(created->QueryInterface(IID_PPV_ARGS(&swapChain))) || !swapChain)
            return;
        swapChain->Release();
    }
    HookSwapChain(swapChain);
    Object& o = Track(swapChain, ObjKind::SwapChain, "IDXGISwapChain", IdOf(device), cmd);
    if (!o.args.empty())
        return;
    o.device = device;
    o.args.push_back({"createArgs", args});
    DXGI_SWAP_CHAIN_DESC desc{};
    {
        ScopedInternal internal;
        swapChain->GetDesc(&desc);
    }
    JsonWriter w;
    Write(w, desc);
    Describe(o, "pDesc", w.str());
    o.width = desc.BufferDesc.Width;
    o.height = desc.BufferDesc.Height;
    o.format = desc.BufferDesc.Format;
    o.bufferCount = desc.BufferCount;
    o.window = desc.OutputWindow;
    Announce(o);
    TrackBackBuffers(swapChain);
    LogAlways("%s -> swap chain %p (%ux%u %s, %u buffers)", cmd, (void*)swapChain, desc.BufferDesc.Width, desc.BufferDesc.Height, FormatName(desc.BufferDesc.Format), desc.BufferCount);
    // A device that presents is one the inspector can capture: the port opens here, not at device
    // creation, so a device an application makes for video decoding or a compositor never takes
    // the session's port from the API that actually draws.
    StartServer();
}

}  // namespace d3d11insp
