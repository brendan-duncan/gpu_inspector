// Hooks on the objects a frame passes through on its way to the screen: the command queue, where
// the recorded lists are submitted; the DXGI factory, where swap chains and adapters come from;
// the swap chain, whose Present is the frame boundary and whose back buffers are the resources
// the application draws into; and the adapter, hooked only so its Release and name are seen.
//
// The DXGI objects are one class each implementing every version of their interface, so a
// factory is patched with IDXGIFactory7's slot count and a swap chain with IDXGISwapChain4's —
// after asking the object which version it really has, so a shorter vtable is never written past.
#include "hooks.h"

#include "capture.h"
#include "d3d12_vtables.gen.h"
#include "device_info.h"
#include "device_removed.h"
#include "json.h"
#include "resources.h"
#include "serialize.h"
#include "shader_edit.h"
#include "tracker.h"
#include "validation.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxinsp {
namespace {

inline CaptureManager& Cap() { return CaptureManager::Get(); }

/** The slot count of the highest interface version the object answers a QueryInterface for. */
struct VersionCount {
    const IID* iid;
    uint32_t count;
};

uint32_t VtableCount(IUnknown* object, const VersionCount* versions, size_t n, uint32_t fallback) {
    ScopedInternal internal;
    for (size_t i = 0; i < n; ++i) {
        IUnknown* p = nullptr;
        if (SUCCEEDED(object->QueryInterface(*versions[i].iid, (void**)&p)) && p) {
            p->Release();
            return versions[i].count;
        }
    }
    return fallback;
}

// ---------------------------------------------------------------------------------------------
// Swap chain back buffers. GetBuffer hands out the same ID3D12Resource for the same index every
// time, owned by the swap chain; they are tracked as its children at creation and again after
// every ResizeBuffers, which destroys the old set. Not AddRef'd (the swap chain keeps them alive
// for as long as they are tracked).

std::mutex g_backBufferMutex;
std::unordered_map<IDXGISwapChain*, std::vector<ID3D12Resource*>> g_backBuffers;

bool KnownBackBuffer(IDXGISwapChain* swapChain, ID3D12Resource* buffer) {
    std::lock_guard<std::mutex> lock(g_backBufferMutex);
    auto it = g_backBuffers.find(swapChain);
    if (it == g_backBuffers.end()) return false;
    for (ID3D12Resource* b : it->second)
        if (b == buffer) return true;
    return false;
}

/** Hooks and tracks one back buffer under the swap chain, and notes it in the side table. */
void TrackBackBuffer(IDXGISwapChain* swapChain, UINT index, ID3D12Resource* buffer) {
    HookResource(buffer);
    D3D12_RESOURCE_DESC desc;
    {
        ScopedInternal internal;
        desc = buffer->GetDesc();
    }
    Args args;
    args.u("Buffer", index);
    Write(args.key("pDesc"), desc);
    Tracker::Get().Track(buffer, "ID3D12Resource", "GetBuffer", swapChain, args.str(), index);
    ResourceTracker::Get().OnCreated(buffer, desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_PRESENT, nullptr, 0, true);
    std::lock_guard<std::mutex> lock(g_backBufferMutex);
    g_backBuffers[swapChain].push_back(buffer);
}

/** Every buffer of the swap chain (a bitblt-model chain answers only for index 0; the rest are skipped). */
void TrackBackBuffers(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    {
        ScopedInternal internal;
        if (FAILED(swapChain->GetDesc(&desc))) return;
    }
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        ID3D12Resource* buffer = nullptr;
        {
            ScopedInternal internal;
            if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer))) || !buffer) continue;
        }
        if (!KnownBackBuffer(swapChain, buffer)) TrackBackBuffer(swapChain, i, buffer);
        ScopedInternal internal;
        buffer->Release();
    }
}

/** Before ResizeBuffers: the old buffers are about to be destroyed by the swap chain. */
void UntrackBackBuffers(IDXGISwapChain* swapChain) {
    std::vector<ID3D12Resource*> buffers;
    {
        std::lock_guard<std::mutex> lock(g_backBufferMutex);
        auto it = g_backBuffers.find(swapChain);
        if (it == g_backBuffers.end()) return;
        buffers.swap(it->second);
    }
    for (ID3D12Resource* buffer : buffers) {
        ResourceTracker::Get().OnReleased(buffer);
        Tracker::Get().Untrack(buffer);
    }
}

/** The D3D12 queue a swap chain is created on (pDevice is the queue), or null when it is not a D3D12 swap chain. */
ID3D12CommandQueue* QueueOf(IUnknown* device) {
    if (!device) return nullptr;
    ScopedInternal internal;
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) return nullptr;
    queue->Release();
    return queue;
}

/** A swap chain that came out of one of the factory's creation methods on a D3D12 queue. */
void RegisterSwapChain(IUnknown* created, ID3D12CommandQueue* queue, const char* method, std::string args) {
    IDXGISwapChain* swapChain = nullptr;
    {
        ScopedInternal internal;
        if (FAILED(created->QueryInterface(IID_PPV_ARGS(&swapChain))) || !swapChain) return;
        swapChain->Release();
    }
    HookSwapChain(swapChain);
    Tracker::Get().Track(swapChain, "IDXGISwapChain", method, DeviceOf(queue), std::move(args));
    Cap().OnSwapChainCreated(swapChain, queue);
    TrackBackBuffers(swapChain);
    Log("%s -> swap chain %p on queue %p", method, (void*)swapChain, (void*)queue);
}

// ---------------------------------------------------------------------------------------------
// ID3D12CommandQueue

double g_qpcToMs = 0;

void STDMETHODCALLTYPE Hook_ExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    auto orig = Orig<PFN_ID3D12CommandQueue_ExecuteCommandLists>(This, slot::ID3D12CommandQueue_ExecuteCommandLists);
    if (Internal()) return orig(This, NumCommandLists, ppCommandLists);
    if (g_qpcToMs == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcToMs = 1000.0 / (double)f.QuadPart;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    orig(This, NumCommandLists, ppCommandLists);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * g_qpcToMs;
    Log("queue %p ExecuteCommandLists(%u) %.3f ms", (void*)This, NumCommandLists, ms);
    for (UINT i = 0; ppCommandLists && i < NumCommandLists; ++i)
        if (ppCommandLists[i]) ResourceTracker::Get().OnListExecuted(ppCommandLists[i]);
    ID3D12Device* device = DeviceOf(This);
    AddSubmitTime(device, ms);
    // For a device that never presents, this submission may be its frame boundary; then the
    // per-frame chores a present would do run here instead (frame timing, validation, retired
    // shader edits). See CaptureManager::OnExecuteCommandLists.
    if (Cap().OnExecuteCommandLists(This, NumCommandLists, ppCommandLists, ms)) {
        OnFrameNoPresent(device);
        ValidationLog::Get().Poll(Cap().FrameCounter());
        ShaderEditor::Get().OnPresent();
    }
}

HRESULT STDMETHODCALLTYPE Hook_Signal(ID3D12CommandQueue* This, ID3D12Fence* pFence, UINT64 Value) {
    auto orig = Orig<PFN_ID3D12CommandQueue_Signal>(This, slot::ID3D12CommandQueue_Signal);
    HRESULT hr = orig(This, pFence, Value);
    if (!Internal()) Log("queue %p Signal(fence %p, %llu) -> %s", (void*)This, (void*)pFence, (unsigned long long)Value, HrText(hr).c_str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Wait(ID3D12CommandQueue* This, ID3D12Fence* pFence, UINT64 Value) {
    auto orig = Orig<PFN_ID3D12CommandQueue_Wait>(This, slot::ID3D12CommandQueue_Wait);
    HRESULT hr = orig(This, pFence, Value);
    if (!Internal()) Log("queue %p Wait(fence %p, %llu) -> %s", (void*)This, (void*)pFence, (unsigned long long)Value, HrText(hr).c_str());
    return hr;
}

// ---------------------------------------------------------------------------------------------
// IDXGISwapChain: the frame boundary, and the back buffers

/** After a present of a D3D12 swap chain: the frame counters, the capture, validation polling, retired shader edits. */
void AfterPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, HRESULT hr) {
    ID3D12CommandQueue* queue = Cap().PresentQueue(swapChain);
    ID3D12Device* device = queue ? DeviceOf(queue) : nullptr;
    // Present is where a removal is usually noticed, long after the command that caused it, so this
    // runs before anything else and even for a test present: what the GPU was doing is in DRED and
    // only DRED, and nothing further in the frame will work anyway (device_removed.h).
    if (IsDeviceRemoved(hr) || SimulateDeviceRemoved()) {
        OnDeviceRemoved(device, "IDXGISwapChain::Present");
        return;
    }
    // A DXGI_PRESENT_TEST asks whether presenting would work; nothing was shown.
    if (flags & DXGI_PRESENT_TEST) return;
    if (!queue) return;
    OnFramePresented(device, swapChain, syncInterval, flags, hr);
    Cap().OnPresent(device, swapChain, queue);
    ValidationLog::Get().Poll(Cap().FrameCounter());
    ShaderEditor::Get().OnPresent();
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain4* This, UINT SyncInterval, UINT Flags) {
    auto orig = Orig<PFN_IDXGISwapChain4_Present>(This, slot::IDXGISwapChain4_Present);
    if (Internal()) return orig(This, SyncInterval, Flags);
    HRESULT hr = orig(This, SyncInterval, Flags);
    AfterPresent(This, SyncInterval, Flags, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain4* This, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    auto orig = Orig<PFN_IDXGISwapChain4_Present1>(This, slot::IDXGISwapChain4_Present1);
    if (Internal()) return orig(This, SyncInterval, PresentFlags, pPresentParameters);
    HRESULT hr = orig(This, SyncInterval, PresentFlags, pPresentParameters);
    AfterPresent(This, SyncInterval, PresentFlags, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain4* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    auto orig = Orig<PFN_IDXGISwapChain4_ResizeBuffers>(This, slot::IDXGISwapChain4_ResizeBuffers);
    if (Internal()) return orig(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    bool known = Tracker::Get().IdOf(This) != 0;
    if (known) UntrackBackBuffers(This);
    HRESULT hr = orig(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    Log("swap chain %p ResizeBuffers(%u, %ux%u) -> %s", (void*)This, BufferCount, Width, Height, HrText(hr).c_str());
    // Whatever GetBuffer answers now is the current set, the new one or the old on failure.
    if (known) TrackBackBuffers(This);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers1(IDXGISwapChain4* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    auto orig = Orig<PFN_IDXGISwapChain4_ResizeBuffers1>(This, slot::IDXGISwapChain4_ResizeBuffers1);
    if (Internal()) return orig(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    bool known = Tracker::Get().IdOf(This) != 0;
    if (known) UntrackBackBuffers(This);
    HRESULT hr = orig(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    Log("swap chain %p ResizeBuffers1(%u, %ux%u) -> %s", (void*)This, BufferCount, Width, Height, HrText(hr).c_str());
    if (known) TrackBackBuffers(This);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_GetBuffer(IDXGISwapChain4* This, UINT Buffer, REFIID riid, void** ppSurface) {
    auto orig = Orig<PFN_IDXGISwapChain4_GetBuffer>(This, slot::IDXGISwapChain4_GetBuffer);
    if (Internal()) return orig(This, Buffer, riid, ppSurface);
    HRESULT hr = orig(This, Buffer, riid, ppSurface);
    if (FAILED(hr) || !ppSurface || !*ppSurface || !Tracker::Get().IdOf(This)) return hr;
    // A buffer the creation could not retrieve is tracked the first time the application gets it.
    ID3D12Resource* buffer = nullptr;
    {
        ScopedInternal internal;
        if (FAILED(((IUnknown*)*ppSurface)->QueryInterface(IID_PPV_ARGS(&buffer))) || !buffer) return hr;
        buffer->Release();
    }
    if (!KnownBackBuffer(This, buffer)) TrackBackBuffer(This, Buffer, buffer);
    return hr;
}

// ---------------------------------------------------------------------------------------------
// IDXGIFactory: swap chains and adapters

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory7* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    auto orig = Orig<PFN_IDXGIFactory7_CreateSwapChain>(This, slot::IDXGIFactory7_CreateSwapChain);
    if (Internal()) return orig(This, pDevice, pDesc, ppSwapChain);
    HRESULT hr = orig(This, pDevice, pDesc, ppSwapChain);
    if (FAILED(hr) || !ppSwapChain || !*ppSwapChain) return hr;
    ID3D12CommandQueue* queue = QueueOf(pDevice);
    if (!queue) return hr;
    Args args;
    args.ref("pDevice", queue, "ID3D12CommandQueue");
    if (pDesc) Write(args.key("pDesc"), *pDesc); else args.null("pDesc");
    RegisterSwapChain(*ppSwapChain, queue, "CreateSwapChain", args.str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory7* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    auto orig = Orig<PFN_IDXGIFactory7_CreateSwapChainForHwnd>(This, slot::IDXGIFactory7_CreateSwapChainForHwnd);
    if (Internal()) return orig(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    HRESULT hr = orig(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (FAILED(hr) || !ppSwapChain || !*ppSwapChain) return hr;
    ID3D12CommandQueue* queue = QueueOf(pDevice);
    if (!queue) return hr;
    Args args;
    args.ref("pDevice", queue, "ID3D12CommandQueue").ptr("hWnd", hWnd);
    if (pDesc) Write(args.key("pDesc"), *pDesc); else args.null("pDesc");
    if (pFullscreenDesc) Write(args.key("pFullscreenDesc"), *pFullscreenDesc); else args.null("pFullscreenDesc");
    args.ptr("pRestrictToOutput", pRestrictToOutput);
    RegisterSwapChain(*ppSwapChain, queue, "CreateSwapChainForHwnd", args.str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForCoreWindow(IDXGIFactory7* This, IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    auto orig = Orig<PFN_IDXGIFactory7_CreateSwapChainForCoreWindow>(This, slot::IDXGIFactory7_CreateSwapChainForCoreWindow);
    if (Internal()) return orig(This, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    HRESULT hr = orig(This, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    if (FAILED(hr) || !ppSwapChain || !*ppSwapChain) return hr;
    ID3D12CommandQueue* queue = QueueOf(pDevice);
    if (!queue) return hr;
    Args args;
    args.ref("pDevice", queue, "ID3D12CommandQueue").ptr("pWindow", pWindow);
    if (pDesc) Write(args.key("pDesc"), *pDesc); else args.null("pDesc");
    args.ptr("pRestrictToOutput", pRestrictToOutput);
    RegisterSwapChain(*ppSwapChain, queue, "CreateSwapChainForCoreWindow", args.str());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForComposition(IDXGIFactory7* This, IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    auto orig = Orig<PFN_IDXGIFactory7_CreateSwapChainForComposition>(This, slot::IDXGIFactory7_CreateSwapChainForComposition);
    if (Internal()) return orig(This, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    HRESULT hr = orig(This, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    if (FAILED(hr) || !ppSwapChain || !*ppSwapChain) return hr;
    ID3D12CommandQueue* queue = QueueOf(pDevice);
    if (!queue) return hr;
    Args args;
    args.ref("pDevice", queue, "ID3D12CommandQueue");
    if (pDesc) Write(args.key("pDesc"), *pDesc); else args.null("pDesc");
    args.ptr("pRestrictToOutput", pRestrictToOutput);
    RegisterSwapChain(*ppSwapChain, queue, "CreateSwapChainForComposition", args.str());
    return hr;
}

/** An adapter returned through a REFIID/void** pair: hooked through the IDXGIAdapter it also is. */
void HookAdapterUnknown(void* object) {
    if (!object) return;
    IDXGIAdapter* adapter = nullptr;
    {
        ScopedInternal internal;
        if (FAILED(((IUnknown*)object)->QueryInterface(IID_PPV_ARGS(&adapter))) || !adapter) return;
        adapter->Release();
    }
    HookAdapter(adapter);
}

HRESULT STDMETHODCALLTYPE Hook_EnumAdapters(IDXGIFactory7* This, UINT Adapter, IDXGIAdapter** ppAdapter) {
    auto orig = Orig<PFN_IDXGIFactory7_EnumAdapters>(This, slot::IDXGIFactory7_EnumAdapters);
    if (Internal()) return orig(This, Adapter, ppAdapter);
    HRESULT hr = orig(This, Adapter, ppAdapter);
    if (SUCCEEDED(hr) && ppAdapter && *ppAdapter) HookAdapter(*ppAdapter);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_EnumAdapters1(IDXGIFactory7* This, UINT Adapter, IDXGIAdapter1** ppAdapter) {
    auto orig = Orig<PFN_IDXGIFactory7_EnumAdapters1>(This, slot::IDXGIFactory7_EnumAdapters1);
    if (Internal()) return orig(This, Adapter, ppAdapter);
    HRESULT hr = orig(This, Adapter, ppAdapter);
    if (SUCCEEDED(hr) && ppAdapter && *ppAdapter) HookAdapter(*ppAdapter);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_EnumAdapterByLuid(IDXGIFactory7* This, LUID AdapterLuid, REFIID riid, void** ppvAdapter) {
    auto orig = Orig<PFN_IDXGIFactory7_EnumAdapterByLuid>(This, slot::IDXGIFactory7_EnumAdapterByLuid);
    if (Internal()) return orig(This, AdapterLuid, riid, ppvAdapter);
    HRESULT hr = orig(This, AdapterLuid, riid, ppvAdapter);
    if (SUCCEEDED(hr) && ppvAdapter) HookAdapterUnknown(*ppvAdapter);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_EnumAdapterByGpuPreference(IDXGIFactory7* This, UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void** ppvAdapter) {
    auto orig = Orig<PFN_IDXGIFactory7_EnumAdapterByGpuPreference>(This, slot::IDXGIFactory7_EnumAdapterByGpuPreference);
    if (Internal()) return orig(This, Adapter, GpuPreference, riid, ppvAdapter);
    HRESULT hr = orig(This, Adapter, GpuPreference, riid, ppvAdapter);
    if (SUCCEEDED(hr) && ppvAdapter) HookAdapterUnknown(*ppvAdapter);
    return hr;
}

}  // namespace

void HookCommandQueue(ID3D12CommandQueue* queue) {
    HookD3D12Object(queue, "ID3D12CommandQueue", slot::ID3D12CommandQueue_Count, {
        {slot::ID3D12CommandQueue_ExecuteCommandLists, (void*)&Hook_ExecuteCommandLists},
        {slot::ID3D12CommandQueue_Signal, (void*)&Hook_Signal},
        {slot::ID3D12CommandQueue_Wait, (void*)&Hook_Wait},
    });
}

void HookSwapChain(IDXGISwapChain* swapChain) {
    if (!swapChain || VtableHooked(swapChain)) return;
    // IDXGISwapChain: 18 slots; 1: 24; 2: 36; 3: 40; 4: 41.
    static const VersionCount versions[] = {
        {&__uuidof(IDXGISwapChain4), slot::IDXGISwapChain4_Count},
        {&__uuidof(IDXGISwapChain3), slot::IDXGISwapChain4_ResizeBuffers1 + 1},
        {&__uuidof(IDXGISwapChain2), slot::IDXGISwapChain4_GetMatrixTransform + 1},
        {&__uuidof(IDXGISwapChain1), slot::IDXGISwapChain4_IsTemporaryMonoSupported + 1},
    };
    uint32_t count = VtableCount(swapChain, versions, 4, slot::IDXGISwapChain4_GetLastPresentCount + 1);
    HookDxgiObject(swapChain, "IDXGISwapChain", count, {
        {slot::IDXGISwapChain4_Present, (void*)&Hook_Present},
        {slot::IDXGISwapChain4_Present1, (void*)&Hook_Present1},
        {slot::IDXGISwapChain4_ResizeBuffers, (void*)&Hook_ResizeBuffers},
        {slot::IDXGISwapChain4_ResizeBuffers1, (void*)&Hook_ResizeBuffers1},
        {slot::IDXGISwapChain4_GetBuffer, (void*)&Hook_GetBuffer},
    });
}

void HookFactory(IDXGIFactory* factory) {
    if (!factory || VtableHooked(factory)) return;
    // IDXGIFactory: 13 slots; 1: 14; 2: 25; 3: 26; 4: 28; 5: 29; 6: 30; 7: 32.
    static const VersionCount versions[] = {
        {&__uuidof(IDXGIFactory7), slot::IDXGIFactory7_Count},
        {&__uuidof(IDXGIFactory6), slot::IDXGIFactory7_EnumAdapterByGpuPreference + 1},
        {&__uuidof(IDXGIFactory5), slot::IDXGIFactory7_CheckFeatureSupport + 1},
        {&__uuidof(IDXGIFactory4), slot::IDXGIFactory7_EnumWarpAdapter + 1},
        {&__uuidof(IDXGIFactory3), slot::IDXGIFactory7_GetCreationFlags + 1},
        {&__uuidof(IDXGIFactory2), slot::IDXGIFactory7_CreateSwapChainForComposition + 1},
        {&__uuidof(IDXGIFactory1), slot::IDXGIFactory7_IsCurrent + 1},
    };
    uint32_t count = VtableCount(factory, versions, 7, slot::IDXGIFactory7_EnumAdapters1 + 1);
    HookDxgiObject(factory, "IDXGIFactory", count, {
        {slot::IDXGIFactory7_CreateSwapChain, (void*)&Hook_CreateSwapChain},
        {slot::IDXGIFactory7_CreateSwapChainForHwnd, (void*)&Hook_CreateSwapChainForHwnd},
        {slot::IDXGIFactory7_CreateSwapChainForCoreWindow, (void*)&Hook_CreateSwapChainForCoreWindow},
        {slot::IDXGIFactory7_CreateSwapChainForComposition, (void*)&Hook_CreateSwapChainForComposition},
        {slot::IDXGIFactory7_EnumAdapters, (void*)&Hook_EnumAdapters},
        {slot::IDXGIFactory7_EnumAdapters1, (void*)&Hook_EnumAdapters1},
        {slot::IDXGIFactory7_EnumAdapterByLuid, (void*)&Hook_EnumAdapterByLuid},
        {slot::IDXGIFactory7_EnumAdapterByGpuPreference, (void*)&Hook_EnumAdapterByGpuPreference},
    });
}

void HookAdapter(IDXGIAdapter* adapter) {
    if (!adapter || VtableHooked(adapter)) return;
    // IDXGIAdapter: 10 slots; 1: 11; 2: 12; 3: 18; 4: 19.
    static const VersionCount versions[] = {
        {&__uuidof(IDXGIAdapter4), slot::IDXGIAdapter4_Count},
        {&__uuidof(IDXGIAdapter3), slot::IDXGIAdapter4_UnregisterVideoMemoryBudgetChangeNotification + 1},
        {&__uuidof(IDXGIAdapter2), slot::IDXGIAdapter4_GetDesc2 + 1},
        {&__uuidof(IDXGIAdapter1), slot::IDXGIAdapter4_GetDesc1 + 1},
    };
    uint32_t count = VtableCount(adapter, versions, 4, slot::IDXGIAdapter4_CheckInterfaceSupport + 1);
    HookDxgiObject(adapter, "IDXGIAdapter", count, {});
}

}  // namespace dxinsp
