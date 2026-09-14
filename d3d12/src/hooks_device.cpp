// The entry points and the device: MinHook on D3D12CreateDevice and the CreateDXGIFactory
// family, and the vtable patch on ID3D12Device that follows every object the device creates.
//
// Every replacement has the same shape (the Metal library's hooks_device.mm is the model):
// forward first, and only when the call was the application's (not Internal()) and succeeded,
// hook the vtable of what came back, track it with the call's arguments under their D3D12 names,
// and tell the modules that keep state under it. The vtable hooks of the objects created here
// that need nothing beyond Release and the name hooks (allocators, resources, heaps, fences,
// ...) are installed here too; command lists, queues, swap chains and factories are hooked in
// their own files and only called from here.
#include "hooks.h"

#include "capture.h"
#include "d3d12_enums.gen.h"
#include "d3d12_vtables.gen.h"
#include "descriptors.h"
#include "device_info.h"
#include "formats.h"
#include "image_readback.h"
#include "json.h"
#include "resources.h"
#include "serialize.h"
#include "shader_edit.h"
#include "shader_reflect.h"
#include "tracker.h"
#include "ui_messages.h"
#include "validation.h"

#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dxinsp {

namespace {

// The original of a device method, for forwarding: DEV(CreateCommandQueue)(This, pDesc, riid, out).
#define DEV(Method) Orig<PFN_ID3D12Device14_##Method>(This, slot::ID3D12Device14_##Method)
#define LIB(Method) Orig<PFN_ID3D12PipelineLibrary1_##Method>(This, slot::ID3D12PipelineLibrary1_##Method)

// ---------------------------------------------------------------------------------------------
// Helpers

/** The object seen as T (the same object, whichever interface the application asked for), or null. Holds no reference. */
template <typename T>
T* QueryAs(IUnknown* object) {
    if (!object) return nullptr;
    ScopedInternal internal;
    T* p = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&p))) || !p) return nullptr;
    // The library holds no references of its own to the application's objects.
    p->Release();
    return p;
}

/** The created object when the call succeeded and produced one, else null. */
template <typename T>
T* Result(HRESULT hr, void** out) {
    if (FAILED(hr) || !out || !*out) return nullptr;
    return static_cast<T*>(*out);
}

/**
 * The number of vtable entries the device actually has. The generated header names the latest
 * interface's count only; a runtime that implements an older ID3D12DeviceN has a shorter
 * vtable, and patching past its end would overwrite whatever follows it. The counts are the
 * slot after each version's last method (gen/d3d12_vtables.gen.h).
 */
uint32_t DeviceSlotCount(ID3D12Device* device) {
    struct Version {
        const IID* iid;
        uint32_t count;
    };
    static const Version versions[] = {
        {&__uuidof(ID3D12Device14), slot::ID3D12Device14_Count},
        {&__uuidof(ID3D12Device13), slot::ID3D12Device14_CreateRootSignatureFromSubobjectInLibrary},
        {&__uuidof(ID3D12Device12), slot::ID3D12Device14_OpenExistingHeapFromAddress1},
        {&__uuidof(ID3D12Device11), slot::ID3D12Device14_GetResourceAllocationInfo3},
        {&__uuidof(ID3D12Device10), slot::ID3D12Device14_CreateSampler2},
        {&__uuidof(ID3D12Device9), slot::ID3D12Device14_CreateCommittedResource3},
        {&__uuidof(ID3D12Device8), slot::ID3D12Device14_CreateShaderCacheSession},
        {&__uuidof(ID3D12Device7), slot::ID3D12Device14_GetResourceAllocationInfo2},
        {&__uuidof(ID3D12Device6), slot::ID3D12Device14_AddToStateObject},
        {&__uuidof(ID3D12Device5), slot::ID3D12Device14_SetBackgroundProcessingMode},
        {&__uuidof(ID3D12Device4), slot::ID3D12Device14_CreateLifetimeTracker},
        {&__uuidof(ID3D12Device3), slot::ID3D12Device14_CreateCommandList1},
        {&__uuidof(ID3D12Device2), slot::ID3D12Device14_OpenExistingHeapFromAddress},
        {&__uuidof(ID3D12Device1), slot::ID3D12Device14_CreatePipelineState},
    };
    ScopedInternal internal;
    for (const Version& v : versions) {
        IUnknown* p = nullptr;
        if (SUCCEEDED(device->QueryInterface(*v.iid, (void**)&p)) && p) {
            p->Release();
            return v.count;
        }
    }
    return slot::ID3D12Device14_CreatePipelineLibrary;   // ID3D12Device ends before ID3D12Device1's first method
}

/** The heap type of a heap a placed resource sits on (DEFAULT when it cannot be asked). */
D3D12_HEAP_TYPE HeapTypeOf(ID3D12Heap* heap) {
    if (!heap) return D3D12_HEAP_TYPE_DEFAULT;
    ScopedInternal internal;
    return heap->GetDesc().Properties.Type;
}

template <typename Desc>
void WriteDescArg(Args& a, const char* key, const Desc* desc) {
    if (desc) Write(a.key(key), *desc);
    else a.null(key);
}

void WriteStates(Args& a, const char* key, D3D12_RESOURCE_STATES states) {
    WriteFlags(a.key(key), kEnum_D3D12_RESOURCE_STATES, std::size(kEnum_D3D12_RESOURCE_STATES), (uint64_t)states);
}

template <typename Desc>
void WriteClearValue(Args& a, const D3D12_CLEAR_VALUE* clear, const Desc* desc) {
    Write(a.key("pOptimizedClearValue"), clear, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
}

void WriteHeapArgs(Args& a, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags) {
    WriteDescArg(a, "pHeapProperties", props);
    WriteFlags(a.key("HeapFlags"), kEnum_D3D12_HEAP_FLAGS, std::size(kEnum_D3D12_HEAP_FLAGS), (uint64_t)flags);
}

void WriteCastableFormats(Args& a, UINT32 count, const DXGI_FORMAT* formats) {
    a.u("NumCastableFormats", count);
    if (formats && count) WriteFormats(a.key("pCastableFormats"), formats, count);
    else a.null("pCastableFormats");
}

template <typename Desc>
std::string DescJson(const Desc* desc) {
    if (!desc) return std::string();
    JsonWriter w(&Tracker::Get());
    Write(w, *desc);
    return std::move(w.str());
}

// ---------------------------------------------------------------------------------------------
// Resources

/**
 * Everything a new resource gets, whichever of the nine creation calls made it: its vtable
 * hooked, the tracker entry, the state tracker's record (from the runtime's own description,
 * which has the mip count and alignment resolved) and, for a buffer, its address range.
 */
void OnResourceCreated(ID3D12Device* device, ID3D12Resource* resource, const char* cmd, std::string args,
                       D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState, ID3D12Heap* heap, UINT64 heapOffset) {
    HookResource(resource);
    Tracker::Get().Track(resource, "ID3D12Resource", cmd, device, std::move(args));
    D3D12_RESOURCE_DESC desc{};
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    {
        ScopedInternal internal;
        desc = resource->GetDesc();
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) address = resource->GetGPUVirtualAddress();
    }
    ResourceTracker::Get().OnCreated(resource, desc, heapType, initialState, heap, heapOffset);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && address) AddressMap::Get().Add(resource, address, desc.Width);
    Log("%s -> ID3D12Resource %p (%s, %llux%ux%u, %s)", cmd, (void*)resource,
        ToString_D3D12_RESOURCE_DIMENSION(desc.Dimension) ? ToString_D3D12_RESOURCE_DIMENSION(desc.Dimension) : "?",
        (unsigned long long)desc.Width, desc.Height, desc.DepthOrArraySize, FormatName(desc.Format));
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(ID3D12Device14* This, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
                                                       const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
                                                       const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommittedResource)(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riid, out);
    HRESULT hr = DEV(CreateCommittedResource)(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    WriteHeapArgs(a, pHeapProperties, HeapFlags);
    WriteDescArg(a, "pDesc", pDesc);
    WriteStates(a, "InitialResourceState", InitialResourceState);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    OnResourceCreated(This, resource, "CreateCommittedResource", a.str(),
                      pHeapProperties ? pHeapProperties->Type : D3D12_HEAP_TYPE_DEFAULT, InitialResourceState, nullptr, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource1(ID3D12Device14* This, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
                                                        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
                                                        REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommittedResource1)(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riid, out);
    HRESULT hr = DEV(CreateCommittedResource1)(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    WriteHeapArgs(a, pHeapProperties, HeapFlags);
    WriteDescArg(a, "pDesc", pDesc);
    WriteStates(a, "InitialResourceState", InitialResourceState);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    a.ptr("pProtectedSession", pProtectedSession);
    OnResourceCreated(This, resource, "CreateCommittedResource1", a.str(),
                      pHeapProperties ? pHeapProperties->Type : D3D12_HEAP_TYPE_DEFAULT, InitialResourceState, nullptr, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource2(ID3D12Device14* This, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
                                                        const D3D12_RESOURCE_DESC1* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
                                                        REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommittedResource2)(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riid, out);
    HRESULT hr = DEV(CreateCommittedResource2)(This, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    WriteHeapArgs(a, pHeapProperties, HeapFlags);
    WriteDescArg(a, "pDesc", pDesc);
    WriteStates(a, "InitialResourceState", InitialResourceState);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    a.ptr("pProtectedSession", pProtectedSession);
    OnResourceCreated(This, resource, "CreateCommittedResource2", a.str(),
                      pHeapProperties ? pHeapProperties->Type : D3D12_HEAP_TYPE_DEFAULT, InitialResourceState, nullptr, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource3(ID3D12Device14* This, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
                                                        const D3D12_RESOURCE_DESC1* pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
                                                        UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommittedResource3)(This, pHeapProperties, HeapFlags, pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riid, out);
    HRESULT hr = DEV(CreateCommittedResource3)(This, pHeapProperties, HeapFlags, pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    WriteHeapArgs(a, pHeapProperties, HeapFlags);
    WriteDescArg(a, "pDesc", pDesc);
    a.e("InitialLayout", ToString_D3D12_BARRIER_LAYOUT(InitialLayout), (int64_t)InitialLayout);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    a.ptr("pProtectedSession", pProtectedSession);
    WriteCastableFormats(a, NumCastableFormats, pCastableFormats);
    OnResourceCreated(This, resource, "CreateCommittedResource3", a.str(),
                      pHeapProperties ? pHeapProperties->Type : D3D12_HEAP_TYPE_DEFAULT, StateOfLayout(InitialLayout), nullptr, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(ID3D12Device14* This, ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
                                                    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** out) {
    if (Internal()) return DEV(CreatePlacedResource)(This, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, out);
    HRESULT hr = DEV(CreatePlacedResource)(This, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    a.ref("pHeap", pHeap, "ID3D12Heap").u("HeapOffset", HeapOffset);
    WriteDescArg(a, "pDesc", pDesc);
    WriteStates(a, "InitialState", InitialState);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    OnResourceCreated(This, resource, "CreatePlacedResource", a.str(), HeapTypeOf(pHeap), InitialState, pHeap, HeapOffset);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource1(ID3D12Device14* This, ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1* pDesc,
                                                     D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** out) {
    if (Internal()) return DEV(CreatePlacedResource1)(This, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, out);
    HRESULT hr = DEV(CreatePlacedResource1)(This, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    a.ref("pHeap", pHeap, "ID3D12Heap").u("HeapOffset", HeapOffset);
    WriteDescArg(a, "pDesc", pDesc);
    WriteStates(a, "InitialState", InitialState);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    OnResourceCreated(This, resource, "CreatePlacedResource1", a.str(), HeapTypeOf(pHeap), InitialState, pHeap, HeapOffset);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource2(ID3D12Device14* This, ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1* pDesc,
                                                     D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                     UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats, REFIID riid, void** out) {
    if (Internal()) return DEV(CreatePlacedResource2)(This, pHeap, HeapOffset, pDesc, InitialLayout, pOptimizedClearValue, NumCastableFormats, pCastableFormats, riid, out);
    HRESULT hr = DEV(CreatePlacedResource2)(This, pHeap, HeapOffset, pDesc, InitialLayout, pOptimizedClearValue, NumCastableFormats, pCastableFormats, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    a.ref("pHeap", pHeap, "ID3D12Heap").u("HeapOffset", HeapOffset);
    WriteDescArg(a, "pDesc", pDesc);
    a.e("InitialLayout", ToString_D3D12_BARRIER_LAYOUT(InitialLayout), (int64_t)InitialLayout);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    WriteCastableFormats(a, NumCastableFormats, pCastableFormats);
    OnResourceCreated(This, resource, "CreatePlacedResource2", a.str(), HeapTypeOf(pHeap), StateOfLayout(InitialLayout), pHeap, HeapOffset);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource(ID3D12Device14* This, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
                                                      const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateReservedResource)(This, pDesc, InitialState, pOptimizedClearValue, riid, out);
    HRESULT hr = DEV(CreateReservedResource)(This, pDesc, InitialState, pOptimizedClearValue, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    WriteDescArg(a, "pDesc", pDesc);
    WriteStates(a, "InitialState", InitialState);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    OnResourceCreated(This, resource, "CreateReservedResource", a.str(), D3D12_HEAP_TYPE_DEFAULT, InitialState, nullptr, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource1(ID3D12Device14* This, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
                                                       const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
                                                       REFIID riid, void** out) {
    if (Internal()) return DEV(CreateReservedResource1)(This, pDesc, InitialState, pOptimizedClearValue, pProtectedSession, riid, out);
    HRESULT hr = DEV(CreateReservedResource1)(This, pDesc, InitialState, pOptimizedClearValue, pProtectedSession, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    WriteDescArg(a, "pDesc", pDesc);
    WriteStates(a, "InitialState", InitialState);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    a.ptr("pProtectedSession", pProtectedSession);
    OnResourceCreated(This, resource, "CreateReservedResource1", a.str(), D3D12_HEAP_TYPE_DEFAULT, InitialState, nullptr, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource2(ID3D12Device14* This, const D3D12_RESOURCE_DESC* pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
                                                       const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
                                                       UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateReservedResource2)(This, pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riid, out);
    HRESULT hr = DEV(CreateReservedResource2)(This, pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riid, out);
    ID3D12Resource* resource = Result<ID3D12Resource>(hr, out);
    if (!resource) return hr;
    Args a;
    WriteDescArg(a, "pDesc", pDesc);
    a.e("InitialLayout", ToString_D3D12_BARRIER_LAYOUT(InitialLayout), (int64_t)InitialLayout);
    WriteClearValue(a, pOptimizedClearValue, pDesc);
    a.ptr("pProtectedSession", pProtectedSession);
    WriteCastableFormats(a, NumCastableFormats, pCastableFormats);
    OnResourceCreated(This, resource, "CreateReservedResource2", a.str(), D3D12_HEAP_TYPE_DEFAULT, StateOfLayout(InitialLayout), nullptr, 0);
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Heaps

void OnHeapCreated(ID3D12Device* device, ID3D12Heap* heap, const char* cmd, Args& a, const D3D12_HEAP_DESC* desc) {
    HookHeap(heap);
    if (desc) {
        Write(a.key("pDesc"), *desc);
    } else {
        // A heap opened from an address or a file mapping has no description of the application's: the runtime's.
        D3D12_HEAP_DESC actual{};
        {
            ScopedInternal internal;
            actual = heap->GetDesc();
        }
        Write(a.key("pDesc"), actual);
    }
    Tracker::Get().Track(heap, "ID3D12Heap", cmd, device, a.str());
    Log("%s -> ID3D12Heap %p", cmd, (void*)heap);
}

HRESULT STDMETHODCALLTYPE Hook_CreateHeap(ID3D12Device14* This, const D3D12_HEAP_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateHeap)(This, pDesc, riid, out);
    HRESULT hr = DEV(CreateHeap)(This, pDesc, riid, out);
    ID3D12Heap* heap = Result<ID3D12Heap>(hr, out);
    if (!heap) return hr;
    Args a;
    OnHeapCreated(This, heap, "CreateHeap", a, pDesc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateHeap1(ID3D12Device14* This, const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateHeap1)(This, pDesc, pProtectedSession, riid, out);
    HRESULT hr = DEV(CreateHeap1)(This, pDesc, pProtectedSession, riid, out);
    ID3D12Heap* heap = Result<ID3D12Heap>(hr, out);
    if (!heap) return hr;
    Args a;
    a.ptr("pProtectedSession", pProtectedSession);
    OnHeapCreated(This, heap, "CreateHeap1", a, pDesc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_OpenExistingHeapFromAddress(ID3D12Device14* This, const void* pAddress, REFIID riid, void** out) {
    if (Internal()) return DEV(OpenExistingHeapFromAddress)(This, pAddress, riid, out);
    HRESULT hr = DEV(OpenExistingHeapFromAddress)(This, pAddress, riid, out);
    ID3D12Heap* heap = Result<ID3D12Heap>(hr, out);
    if (!heap) return hr;
    Args a;
    a.ptr("pAddress", pAddress);
    OnHeapCreated(This, heap, "OpenExistingHeapFromAddress", a, nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_OpenExistingHeapFromAddress1(ID3D12Device14* This, const void* pAddress, SIZE_T size, REFIID riid, void** out) {
    if (Internal()) return DEV(OpenExistingHeapFromAddress1)(This, pAddress, size, riid, out);
    HRESULT hr = DEV(OpenExistingHeapFromAddress1)(This, pAddress, size, riid, out);
    ID3D12Heap* heap = Result<ID3D12Heap>(hr, out);
    if (!heap) return hr;
    Args a;
    a.ptr("pAddress", pAddress).u("size", size);
    OnHeapCreated(This, heap, "OpenExistingHeapFromAddress1", a, nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_OpenExistingHeapFromFileMapping(ID3D12Device14* This, HANDLE hFileMapping, REFIID riid, void** out) {
    if (Internal()) return DEV(OpenExistingHeapFromFileMapping)(This, hFileMapping, riid, out);
    HRESULT hr = DEV(OpenExistingHeapFromFileMapping)(This, hFileMapping, riid, out);
    ID3D12Heap* heap = Result<ID3D12Heap>(hr, out);
    if (!heap) return hr;
    Args a;
    a.ptr("hFileMapping", hFileMapping);
    OnHeapCreated(This, heap, "OpenExistingHeapFromFileMapping", a, nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_OpenSharedHandle(ID3D12Device14* This, HANDLE NTHandle, REFIID riid, void** out) {
    if (Internal()) return DEV(OpenSharedHandle)(This, NTHandle, riid, out);
    HRESULT hr = DEV(OpenSharedHandle)(This, NTHandle, riid, out);
    IUnknown* object = Result<IUnknown>(hr, out);
    if (!object) return hr;
    // Whatever was shared is tracked as if this device had created it.
    if (ID3D12Resource* resource = QueryAs<ID3D12Resource>(object)) {
        D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
        {
            ScopedInternal internal;
            D3D12_HEAP_PROPERTIES props{};
            D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
            if (SUCCEEDED(resource->GetHeapProperties(&props, &flags))) heapType = props.Type;
        }
        OnResourceCreated(This, resource, "OpenSharedHandle", Args().ptr("NTHandle", NTHandle).str(), heapType, D3D12_RESOURCE_STATE_COMMON, nullptr, 0);
    } else if (ID3D12Heap* heap = QueryAs<ID3D12Heap>(object)) {
        Args a;
        a.ptr("NTHandle", NTHandle);
        OnHeapCreated(This, heap, "OpenSharedHandle", a, nullptr);
    } else if (ID3D12Fence* fence = QueryAs<ID3D12Fence>(object)) {
        HookFence(fence);
        Tracker::Get().Track(fence, "ID3D12Fence", "OpenSharedHandle", This, Args().ptr("NTHandle", NTHandle).str());
        Log("OpenSharedHandle -> ID3D12Fence %p", (void*)fence);
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Pipelines
//
// A pipeline's descriptor goes out with two members serialize.h does not write: `reflection`
// (per stage, shader_reflect.h) and `shaders` (entry point, target, hash and size per stage),
// and the bytecode of every stage becomes a blob named "<stage>:<entry>", which is where the
// UI's shader views look for a Vulkan pipeline's SPIR-V.

struct StageBytecode {
    const char* stage;   // the UI's name: vertex, fragment, ...
    D3D12_SHADER_BYTECODE code;
};

std::vector<StageBytecode> StagesOf(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    return {{"vertex", d.VS}, {"fragment", d.PS}, {"tess_eval", d.DS}, {"tess_control", d.HS}, {"geometry", d.GS}};
}

std::vector<StageBytecode> StagesOf(const D3D12_COMPUTE_PIPELINE_STATE_DESC& d) {
    return {{"compute", d.CS}};
}

const char* StreamStageName(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: return "vertex";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: return "fragment";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: return "tess_eval";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: return "tess_control";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: return "geometry";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return "compute";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: return "task";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return "mesh";
        default: return nullptr;
    }
}

struct SubobjectLayout {
    size_t size;
    size_t align;
};

// The payload of each stream subobject type (d3dx12's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT:
// a UINT type, the payload at its own alignment, the whole padded to pointer alignment).
SubobjectLayout SubobjectLayoutOf(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
#define DXINSP_LAYOUT(T) return {sizeof(T), alignof(T)}
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: DXINSP_LAYOUT(ID3D12RootSignature*);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: DXINSP_LAYOUT(D3D12_SHADER_BYTECODE);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: DXINSP_LAYOUT(D3D12_STREAM_OUTPUT_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: DXINSP_LAYOUT(D3D12_BLEND_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: DXINSP_LAYOUT(UINT);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: DXINSP_LAYOUT(D3D12_RASTERIZER_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: DXINSP_LAYOUT(D3D12_DEPTH_STENCIL_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: DXINSP_LAYOUT(D3D12_INPUT_LAYOUT_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: DXINSP_LAYOUT(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: DXINSP_LAYOUT(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: DXINSP_LAYOUT(D3D12_RT_FORMAT_ARRAY);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: DXINSP_LAYOUT(DXGI_FORMAT);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: DXINSP_LAYOUT(DXGI_SAMPLE_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: DXINSP_LAYOUT(UINT);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: DXINSP_LAYOUT(D3D12_CACHED_PIPELINE_STATE);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: DXINSP_LAYOUT(D3D12_PIPELINE_STATE_FLAGS);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: DXINSP_LAYOUT(D3D12_DEPTH_STENCIL_DESC1);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: DXINSP_LAYOUT(D3D12_VIEW_INSTANCING_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: DXINSP_LAYOUT(D3D12_DEPTH_STENCIL_DESC2);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: DXINSP_LAYOUT(D3D12_RASTERIZER_DESC1);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: DXINSP_LAYOUT(D3D12_RASTERIZER_DESC2);
        default: return {0, 0};   // unknown: the walk cannot continue past it
    }
#undef DXINSP_LAYOUT
}

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/** The shader stages of a pipeline state stream, found by walking its subobjects. */
std::vector<StageBytecode> StagesOf(const D3D12_PIPELINE_STATE_STREAM_DESC& d) {
    std::vector<StageBytecode> out;
    const uint8_t* base = static_cast<const uint8_t*>(d.pPipelineStateSubobjectStream);
    if (!base) return out;
    size_t offset = 0;
    while (offset + sizeof(UINT) <= d.SizeInBytes) {
        UINT typeValue = 0;
        memcpy(&typeValue, base + offset, sizeof(typeValue));
        auto type = (D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)typeValue;
        SubobjectLayout layout = SubobjectLayoutOf(type);
        if (!layout.size) break;
        size_t data = AlignUp(offset + sizeof(UINT), layout.align);
        if (data + layout.size > d.SizeInBytes) break;
        if (const char* stage = StreamStageName(type)) {
            D3D12_SHADER_BYTECODE code{};
            memcpy(&code, base + data, sizeof(code));
            out.push_back({stage, code});
        }
        offset = AlignUp(data + layout.size, sizeof(void*));
    }
    return out;
}

/**
 * Tracks a pipeline (created by the device or loaded from a library) with its descriptor
 * extended by `reflection` and `shaders`, and attaches a blob per stage. `descJson` is the
 * descriptor as serialize.h wrote it (a closed object); the extra members are spliced in
 * before its final brace, so serialize.cpp needs no variant that takes them.
 */
void TrackPipeline(ID3D12Device* device, ID3D12PipelineState* pipeline, const char* cmd, const wchar_t* name,
                   std::string descJson, const std::vector<StageBytecode>& stages) {
    HookPipelineState(pipeline);
    JsonWriter reflection(&Tracker::Get());
    JsonWriter shaders(&Tracker::Get());
    reflection.BeginObject();
    shaders.BeginObject();
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> blobs;
    for (const StageBytecode& s : stages) {
        const auto* bytes = static_cast<const uint8_t*>(s.code.pShaderBytecode);
        size_t size = s.code.BytecodeLength;
        if (!bytes || !size) continue;
        ShaderInfo info;
        {
            // Reflection may load d3dcompiler/dxcompiler and is the library's own work.
            ScopedInternal internal;
            info = ReflectShader(bytes, size);
        }
        std::string entry = info.entryPoint.empty() ? "main" : info.entryPoint;
        if (!info.reflectionJson.empty()) {
            reflection.Key(s.stage);
            reflection.Raw(info.reflectionJson);
        }
        shaders.Key(s.stage);
        shaders.BeginObject();
        shaders.Key("entryPoint");
        shaders.String(entry);
        shaders.Key("target");
        shaders.String(info.target);
        shaders.Key("hash");
        shaders.String(Hex(ShaderHash(bytes, size)));
        shaders.Key("size");
        shaders.Uint(size);
        shaders.EndObject();
        blobs.emplace_back(std::string(s.stage) + ":" + entry, std::make_shared<std::vector<uint8_t>>(bytes, bytes + size));
    }
    reflection.EndObject();
    shaders.EndObject();
    if (!descJson.empty() && descJson.back() == '}') {
        descJson.pop_back();
        if (descJson.back() != '{') descJson += ',';
        descJson += "\"reflection\":" + reflection.str() + ",\"shaders\":" + shaders.str() + "}";
    }
    Args a;
    if (name) a.ws("pName", name);
    a.raw("pDesc", descJson);
    Tracker::Get().Track(pipeline, "ID3D12PipelineState", cmd, device, a.str());
    for (auto& b : blobs) Tracker::Get().AddBlob(pipeline, b.first, b.second);
    Log("%s -> ID3D12PipelineState %p (%zu shader stages)", cmd, (void*)pipeline, blobs.size());
}

HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(ID3D12Device14* This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateGraphicsPipelineState)(This, pDesc, riid, out);
    HRESULT hr = DEV(CreateGraphicsPipelineState)(This, pDesc, riid, out);
    ID3D12PipelineState* pipeline = Result<ID3D12PipelineState>(hr, out);
    if (!pipeline) return hr;
    TrackPipeline(This, pipeline, "CreateGraphicsPipelineState", nullptr, DescJson(pDesc), pDesc ? StagesOf(*pDesc) : std::vector<StageBytecode>());
    if (pDesc) ShaderEditor::Get().OnGraphicsPipelineCreated(This, pipeline, *pDesc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateComputePipelineState(ID3D12Device14* This, const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateComputePipelineState)(This, pDesc, riid, out);
    HRESULT hr = DEV(CreateComputePipelineState)(This, pDesc, riid, out);
    ID3D12PipelineState* pipeline = Result<ID3D12PipelineState>(hr, out);
    if (!pipeline) return hr;
    TrackPipeline(This, pipeline, "CreateComputePipelineState", nullptr, DescJson(pDesc), pDesc ? StagesOf(*pDesc) : std::vector<StageBytecode>());
    if (pDesc) ShaderEditor::Get().OnComputePipelineCreated(This, pipeline, *pDesc);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(ID3D12Device14* This, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return DEV(CreatePipelineState)(This, pDesc, riid, out);
    HRESULT hr = DEV(CreatePipelineState)(This, pDesc, riid, out);
    ID3D12PipelineState* pipeline = Result<ID3D12PipelineState>(hr, out);
    if (!pipeline) return hr;
    TrackPipeline(This, pipeline, "CreatePipelineState", nullptr, DescJson(pDesc), pDesc ? StagesOf(*pDesc) : std::vector<StageBytecode>());
    if (pDesc) ShaderEditor::Get().OnStreamPipelineCreated(This, pipeline, *pDesc);
    return hr;
}

// Pipeline libraries: a loaded pipeline is tracked like a created one, under the library's
// device. It is not handed to the shader editor: a pipeline out of a library has no rebuildable
// description (README.md, "Live requests").

HRESULT STDMETHODCALLTYPE Hook_LoadGraphicsPipeline(ID3D12PipelineLibrary1* This, LPCWSTR pName, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return LIB(LoadGraphicsPipeline)(This, pName, pDesc, riid, out);
    HRESULT hr = LIB(LoadGraphicsPipeline)(This, pName, pDesc, riid, out);
    ID3D12PipelineState* pipeline = Result<ID3D12PipelineState>(hr, out);
    if (!pipeline) return hr;
    TrackPipeline(DeviceOf(This), pipeline, "LoadGraphicsPipeline", pName, DescJson(pDesc), pDesc ? StagesOf(*pDesc) : std::vector<StageBytecode>());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_LoadComputePipeline(ID3D12PipelineLibrary1* This, LPCWSTR pName, const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return LIB(LoadComputePipeline)(This, pName, pDesc, riid, out);
    HRESULT hr = LIB(LoadComputePipeline)(This, pName, pDesc, riid, out);
    ID3D12PipelineState* pipeline = Result<ID3D12PipelineState>(hr, out);
    if (!pipeline) return hr;
    TrackPipeline(DeviceOf(This), pipeline, "LoadComputePipeline", pName, DescJson(pDesc), pDesc ? StagesOf(*pDesc) : std::vector<StageBytecode>());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_LoadPipeline(ID3D12PipelineLibrary1* This, LPCWSTR pName, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return LIB(LoadPipeline)(This, pName, pDesc, riid, out);
    HRESULT hr = LIB(LoadPipeline)(This, pName, pDesc, riid, out);
    ID3D12PipelineState* pipeline = Result<ID3D12PipelineState>(hr, out);
    if (!pipeline) return hr;
    TrackPipeline(DeviceOf(This), pipeline, "LoadPipeline", pName, DescJson(pDesc), pDesc ? StagesOf(*pDesc) : std::vector<StageBytecode>());
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePipelineLibrary(ID3D12Device14* This, const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid, void** out) {
    if (Internal()) return DEV(CreatePipelineLibrary)(This, pLibraryBlob, BlobLength, riid, out);
    HRESULT hr = DEV(CreatePipelineLibrary)(This, pLibraryBlob, BlobLength, riid, out);
    ID3D12PipelineLibrary* library = Result<ID3D12PipelineLibrary>(hr, out);
    if (!library) return hr;
    HookPipelineLibrary(library);
    Tracker::Get().Track(library, "ID3D12PipelineLibrary", "CreatePipelineLibrary", This, Args().u("BlobLength", BlobLength).str());
    Log("CreatePipelineLibrary -> ID3D12PipelineLibrary %p (%llu bytes)", (void*)library, (unsigned long long)BlobLength);
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Queues, allocators, command lists

void TrackQueue(ID3D12Device* device, ID3D12CommandQueue* queue, const char* cmd, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* creator) {
    HookCommandQueue(queue);
    Args a;
    WriteDescArg(a, "pDesc", desc);
    if (creator) Write(a.key("CreatorID"), *creator);
    Tracker::Get().Track(queue, "ID3D12CommandQueue", cmd, device, a.str());
    D3D12_COMMAND_LIST_TYPE type = desc ? desc->Type : D3D12_COMMAND_LIST_TYPE_DIRECT;
    OnQueueCreated(device, queue, type);
    Log("%s -> ID3D12CommandQueue %p (%s)", cmd, (void*)queue, ToString_D3D12_COMMAND_LIST_TYPE(type) ? ToString_D3D12_COMMAND_LIST_TYPE(type) : "?");
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue(ID3D12Device14* This, const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommandQueue)(This, pDesc, riid, out);
    HRESULT hr = DEV(CreateCommandQueue)(This, pDesc, riid, out);
    ID3D12CommandQueue* queue = Result<ID3D12CommandQueue>(hr, out);
    if (!queue) return hr;
    TrackQueue(This, queue, "CreateCommandQueue", pDesc, nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue1(ID3D12Device14* This, const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID CreatorID, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommandQueue1)(This, pDesc, CreatorID, riid, out);
    HRESULT hr = DEV(CreateCommandQueue1)(This, pDesc, CreatorID, riid, out);
    ID3D12CommandQueue* queue = Result<ID3D12CommandQueue>(hr, out);
    if (!queue) return hr;
    TrackQueue(This, queue, "CreateCommandQueue1", pDesc, &CreatorID);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandAllocator(ID3D12Device14* This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommandAllocator)(This, type, riid, out);
    HRESULT hr = DEV(CreateCommandAllocator)(This, type, riid, out);
    ID3D12CommandAllocator* allocator = Result<ID3D12CommandAllocator>(hr, out);
    if (!allocator) return hr;
    HookCommandAllocator(allocator);
    Tracker::Get().Track(allocator, "ID3D12CommandAllocator", "CreateCommandAllocator", This,
                         Args().e("type", ToString_D3D12_COMMAND_LIST_TYPE(type), (int64_t)type).str());
    Log("CreateCommandAllocator -> ID3D12CommandAllocator %p", (void*)allocator);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(ID3D12Device14* This, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
                                                 ID3D12PipelineState* pInitialState, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommandList)(This, nodeMask, type, pCommandAllocator, pInitialState, riid, out);
    HRESULT hr = DEV(CreateCommandList)(This, nodeMask, type, pCommandAllocator, pInitialState, riid, out);
    IUnknown* object = Result<IUnknown>(hr, out);
    if (!object) return hr;
    // A video list has no graphics interface, and nothing here records one.
    ID3D12GraphicsCommandList* list = QueryAs<ID3D12GraphicsCommandList>(object);
    if (!list) {
        Log("CreateCommandList: a list of type %d has no ID3D12GraphicsCommandList, not tracked", (int)type);
        return hr;
    }
    HookCommandList(list);
    Args a;
    a.u("nodeMask", nodeMask)
        .e("type", ToString_D3D12_COMMAND_LIST_TYPE(type), (int64_t)type)
        .ref("pCommandAllocator", pCommandAllocator, "ID3D12CommandAllocator")
        .ref("pInitialState", pInitialState, "ID3D12PipelineState");
    Tracker::Get().Track(list, "ID3D12GraphicsCommandList", "CreateCommandList", This, a.str());
    // The list comes out recording, as after a Reset.
    CaptureManager::Get().OnListReset(This, list, type, type == D3D12_COMMAND_LIST_TYPE_BUNDLE, pInitialState);
    ResourceTracker::Get().OnListReset(list);
    Log("CreateCommandList -> ID3D12GraphicsCommandList %p (%s)", (void*)list, ToString_D3D12_COMMAND_LIST_TYPE(type) ? ToString_D3D12_COMMAND_LIST_TYPE(type) : "?");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandList1(ID3D12Device14* This, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommandList1)(This, nodeMask, type, flags, riid, out);
    HRESULT hr = DEV(CreateCommandList1)(This, nodeMask, type, flags, riid, out);
    IUnknown* object = Result<IUnknown>(hr, out);
    if (!object) return hr;
    ID3D12GraphicsCommandList* list = QueryAs<ID3D12GraphicsCommandList>(object);
    if (!list) {
        Log("CreateCommandList1: a list of type %d has no ID3D12GraphicsCommandList, not tracked", (int)type);
        return hr;
    }
    HookCommandList(list);
    Args a;
    a.u("nodeMask", nodeMask).e("type", ToString_D3D12_COMMAND_LIST_TYPE(type), (int64_t)type);
    WriteFlags(a.key("flags"), kEnum_D3D12_COMMAND_LIST_FLAGS, std::size(kEnum_D3D12_COMMAND_LIST_FLAGS), (uint64_t)flags);
    Tracker::Get().Track(list, "ID3D12GraphicsCommandList", "CreateCommandList1", This, a.str());
    // Created closed: the recorder attaches at its first Reset.
    Log("CreateCommandList1 -> ID3D12GraphicsCommandList %p (%s)", (void*)list, ToString_D3D12_COMMAND_LIST_TYPE(type) ? ToString_D3D12_COMMAND_LIST_TYPE(type) : "?");
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Descriptor heaps and descriptors

HRESULT STDMETHODCALLTYPE Hook_CreateDescriptorHeap(ID3D12Device14* This, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateDescriptorHeap)(This, pDesc, riid, out);
    HRESULT hr = DEV(CreateDescriptorHeap)(This, pDesc, riid, out);
    ID3D12DescriptorHeap* heap = Result<ID3D12DescriptorHeap>(hr, out);
    if (!heap) return hr;
    HookDescriptorHeap(heap);
    Args a;
    WriteDescArg(a, "pDesc", pDesc);
    Tracker::Get().Track(heap, "ID3D12DescriptorHeap", "CreateDescriptorHeap", This, a.str());
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    if (pDesc) {
        desc = *pDesc;
    } else {
        ScopedInternal internal;
        desc = heap->GetDesc();
    }
    DescriptorTracker::Get().OnHeapCreated(This, heap, desc);
    Log("CreateDescriptorHeap -> ID3D12DescriptorHeap %p (%s x %u%s)", (void*)heap,
        ToString_D3D12_DESCRIPTOR_HEAP_TYPE(desc.Type) ? ToString_D3D12_DESCRIPTOR_HEAP_TYPE(desc.Type) : "?", desc.NumDescriptors,
        (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? ", shader visible" : "");
    return hr;
}

void STDMETHODCALLTYPE Hook_CreateConstantBufferView(ID3D12Device14* This, const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DEV(CreateConstantBufferView)(This, pDesc, DestDescriptor);
    if (Internal()) return;
    DescriptorRecord r;
    r.kind = DescriptorKind::CBV;
    r.hasDesc = pDesc != nullptr;
    if (pDesc) {
        r.address = pDesc->BufferLocation;
        r.size = pDesc->SizeInBytes;
    }
    DescriptorTracker::Get().Write(DestDescriptor, r);
}

void STDMETHODCALLTYPE Hook_CreateShaderResourceView(ID3D12Device14* This, ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DEV(CreateShaderResourceView)(This, pResource, pDesc, DestDescriptor);
    if (Internal()) return;
    DescriptorRecord r;
    r.kind = DescriptorKind::SRV;
    r.resource = pResource;
    r.hasDesc = pDesc != nullptr;
    if (pDesc) {
        r.srv = *pDesc;
        if (pDesc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
            r.accelerationStructure = true;
            r.address = pDesc->RaytracingAccelerationStructure.Location;
        }
    }
    DescriptorTracker::Get().Write(DestDescriptor, r);
}

void STDMETHODCALLTYPE Hook_CreateUnorderedAccessView(ID3D12Device14* This, ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
                                                      const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DEV(CreateUnorderedAccessView)(This, pResource, pCounterResource, pDesc, DestDescriptor);
    if (Internal()) return;
    DescriptorRecord r;
    r.kind = DescriptorKind::UAV;
    r.resource = pResource;
    r.counter = pCounterResource;
    r.hasDesc = pDesc != nullptr;
    if (pDesc) r.uav = *pDesc;
    DescriptorTracker::Get().Write(DestDescriptor, r);
}

void STDMETHODCALLTYPE Hook_CreateRenderTargetView(ID3D12Device14* This, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DEV(CreateRenderTargetView)(This, pResource, pDesc, DestDescriptor);
    if (Internal()) return;
    DescriptorRecord r;
    r.kind = DescriptorKind::RTV;
    r.resource = pResource;
    r.hasDesc = pDesc != nullptr;
    if (pDesc) r.rtv = *pDesc;
    DescriptorTracker::Get().Write(DestDescriptor, r);
}

void STDMETHODCALLTYPE Hook_CreateDepthStencilView(ID3D12Device14* This, ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DEV(CreateDepthStencilView)(This, pResource, pDesc, DestDescriptor);
    if (Internal()) return;
    DescriptorRecord r;
    r.kind = DescriptorKind::DSV;
    r.resource = pResource;
    r.hasDesc = pDesc != nullptr;
    if (pDesc) r.dsv = *pDesc;
    DescriptorTracker::Get().Write(DestDescriptor, r);
}

void STDMETHODCALLTYPE Hook_CreateSampler(ID3D12Device14* This, const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DEV(CreateSampler)(This, pDesc, DestDescriptor);
    if (Internal()) return;
    DescriptorRecord r;
    r.kind = DescriptorKind::Sampler;
    r.hasDesc = pDesc != nullptr;
    if (pDesc) {
        // One record shape for both sampler calls: the DESC2 is the DESC with flags.
        const D3D12_SAMPLER_DESC& s = *pDesc;
        r.sampler.Filter = s.Filter;
        r.sampler.AddressU = s.AddressU;
        r.sampler.AddressV = s.AddressV;
        r.sampler.AddressW = s.AddressW;
        r.sampler.MipLODBias = s.MipLODBias;
        r.sampler.MaxAnisotropy = s.MaxAnisotropy;
        r.sampler.ComparisonFunc = s.ComparisonFunc;
        memcpy(r.sampler.FloatBorderColor, s.BorderColor, sizeof(r.sampler.FloatBorderColor));
        r.sampler.MinLOD = s.MinLOD;
        r.sampler.MaxLOD = s.MaxLOD;
        r.sampler.Flags = D3D12_SAMPLER_FLAG_NONE;
    }
    DescriptorTracker::Get().Write(DestDescriptor, r);
}

void STDMETHODCALLTYPE Hook_CreateSampler2(ID3D12Device14* This, const D3D12_SAMPLER_DESC2* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DEV(CreateSampler2)(This, pDesc, DestDescriptor);
    if (Internal()) return;
    DescriptorRecord r;
    r.kind = DescriptorKind::Sampler;
    r.hasDesc = pDesc != nullptr;
    if (pDesc) r.sampler = *pDesc;
    DescriptorTracker::Get().Write(DestDescriptor, r);
}

void STDMETHODCALLTYPE Hook_CreateSamplerFeedbackUnorderedAccessView(ID3D12Device14* This, ID3D12Resource* pTargetedResource, ID3D12Resource* pFeedbackResource,
                                                                     D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    DEV(CreateSamplerFeedbackUnorderedAccessView)(This, pTargetedResource, pFeedbackResource, DestDescriptor);
    if (Internal()) return;
    // The view writes the feedback texture; the sampled one rides in the counter slot.
    DescriptorRecord r;
    r.kind = DescriptorKind::UAV;
    r.resource = pFeedbackResource;
    r.counter = pTargetedResource;
    r.hasDesc = false;
    DescriptorTracker::Get().Write(DestDescriptor, r);
}

void STDMETHODCALLTYPE Hook_CopyDescriptors(ID3D12Device14* This, UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                            const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                            const UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) {
    DEV(CopyDescriptors)(This, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges,
                         pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);
    if (Internal()) return;
    DescriptorTracker::Get().Copy(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges,
                                  pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes);
}

void STDMETHODCALLTYPE Hook_CopyDescriptorsSimple(ID3D12Device14* This, UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) {
    DEV(CopyDescriptorsSimple)(This, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);
    if (Internal()) return;
    DescriptorTracker::Get().CopySimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart);
}

// ---------------------------------------------------------------------------------------------
// Root signatures, fences, query heaps, command signatures, state objects, meta commands

HRESULT STDMETHODCALLTYPE Hook_CreateRootSignature(ID3D12Device14* This, UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateRootSignature)(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, out);
    HRESULT hr = DEV(CreateRootSignature)(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, out);
    ID3D12RootSignature* signature = Result<ID3D12RootSignature>(hr, out);
    if (!signature) return hr;
    HookRootSignature(signature);
    std::shared_ptr<const RootSignatureInfo> info = RootSignatures::Get().Register(signature, pBlobWithRootSignature, blobLengthInBytes);
    Args a;
    if (info) a.raw("pDesc", info->json);
    a.u("nodeMask", nodeMask).u("blobLength", blobLengthInBytes);
    Tracker::Get().Track(signature, "ID3D12RootSignature", "CreateRootSignature", This, a.str());
    Log("CreateRootSignature -> ID3D12RootSignature %p (%zu parameters%s)", (void*)signature, info ? info->parameters.size() : (size_t)0,
        info ? "" : ", blob not deserialized");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateRootSignatureFromSubobjectInLibrary(ID3D12Device14* This, UINT nodeMask, const void* pLibraryBlob, SIZE_T blobLengthInBytes,
                                                                         LPCWSTR subobjectName, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateRootSignatureFromSubobjectInLibrary)(This, nodeMask, pLibraryBlob, blobLengthInBytes, subobjectName, riid, out);
    HRESULT hr = DEV(CreateRootSignatureFromSubobjectInLibrary)(This, nodeMask, pLibraryBlob, blobLengthInBytes, subobjectName, riid, out);
    ID3D12RootSignature* signature = Result<ID3D12RootSignature>(hr, out);
    if (!signature) return hr;
    HookRootSignature(signature);
    std::shared_ptr<RootSignatureInfo> info = RootSignatures::ParseSubobject(pLibraryBlob, blobLengthInBytes, subobjectName);
    if (info) RootSignatures::Get().Register(signature, info);
    Args a;
    if (info) a.raw("pDesc", info->json);
    a.u("nodeMask", nodeMask).u("blobLength", blobLengthInBytes).ws("subobjectName", subobjectName);
    Tracker::Get().Track(signature, "ID3D12RootSignature", "CreateRootSignatureFromSubobjectInLibrary", This, a.str());
    Log("CreateRootSignatureFromSubobjectInLibrary -> ID3D12RootSignature %p%s", (void*)signature, info ? "" : " (subobject not deserialized)");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateFence(ID3D12Device14* This, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateFence)(This, InitialValue, Flags, riid, out);
    HRESULT hr = DEV(CreateFence)(This, InitialValue, Flags, riid, out);
    ID3D12Fence* fence = Result<ID3D12Fence>(hr, out);
    if (!fence) return hr;
    HookFence(fence);
    Args a;
    a.u("InitialValue", InitialValue);
    WriteFlags(a.key("Flags"), kEnum_D3D12_FENCE_FLAGS, std::size(kEnum_D3D12_FENCE_FLAGS), (uint64_t)Flags);
    Tracker::Get().Track(fence, "ID3D12Fence", "CreateFence", This, a.str());
    Log("CreateFence -> ID3D12Fence %p", (void*)fence);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateQueryHeap(ID3D12Device14* This, const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateQueryHeap)(This, pDesc, riid, out);
    HRESULT hr = DEV(CreateQueryHeap)(This, pDesc, riid, out);
    ID3D12QueryHeap* heap = Result<ID3D12QueryHeap>(hr, out);
    if (!heap) return hr;
    HookQueryHeap(heap);
    Args a;
    WriteDescArg(a, "pDesc", pDesc);
    Tracker::Get().Track(heap, "ID3D12QueryHeap", "CreateQueryHeap", This, a.str());
    Log("CreateQueryHeap -> ID3D12QueryHeap %p", (void*)heap);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandSignature(ID3D12Device14* This, const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateCommandSignature)(This, pDesc, pRootSignature, riid, out);
    HRESULT hr = DEV(CreateCommandSignature)(This, pDesc, pRootSignature, riid, out);
    ID3D12CommandSignature* signature = Result<ID3D12CommandSignature>(hr, out);
    if (!signature) return hr;
    HookCommandSignature(signature);
    Args a;
    WriteDescArg(a, "pDesc", pDesc);
    a.ref("pRootSignature", pRootSignature, "ID3D12RootSignature");
    Tracker::Get().Track(signature, "ID3D12CommandSignature", "CreateCommandSignature", This, a.str());
    Log("CreateCommandSignature -> ID3D12CommandSignature %p", (void*)signature);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateStateObject(ID3D12Device14* This, const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateStateObject)(This, pDesc, riid, out);
    HRESULT hr = DEV(CreateStateObject)(This, pDesc, riid, out);
    ID3D12StateObject* stateObject = Result<ID3D12StateObject>(hr, out);
    if (!stateObject) return hr;
    HookStateObject(stateObject);
    Args a;
    WriteDescArg(a, "pDesc", pDesc);
    Tracker::Get().Track(stateObject, "ID3D12StateObject", "CreateStateObject", This, a.str());
    Log("CreateStateObject -> ID3D12StateObject %p", (void*)stateObject);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_AddToStateObject(ID3D12Device14* This, const D3D12_STATE_OBJECT_DESC* pAddition, ID3D12StateObject* pStateObjectToGrowFrom, REFIID riid, void** out) {
    if (Internal()) return DEV(AddToStateObject)(This, pAddition, pStateObjectToGrowFrom, riid, out);
    HRESULT hr = DEV(AddToStateObject)(This, pAddition, pStateObjectToGrowFrom, riid, out);
    ID3D12StateObject* stateObject = Result<ID3D12StateObject>(hr, out);
    if (!stateObject) return hr;
    HookStateObject(stateObject);
    Args a;
    WriteDescArg(a, "pAddition", pAddition);
    a.ref("pStateObjectToGrowFrom", pStateObjectToGrowFrom, "ID3D12StateObject");
    Tracker::Get().Track(stateObject, "ID3D12StateObject", "AddToStateObject", This, a.str());
    Log("AddToStateObject -> ID3D12StateObject %p", (void*)stateObject);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateMetaCommand(ID3D12Device14* This, REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData,
                                                 SIZE_T CreationParametersDataSizeInBytes, REFIID riid, void** out) {
    if (Internal()) return DEV(CreateMetaCommand)(This, CommandId, NodeMask, pCreationParametersData, CreationParametersDataSizeInBytes, riid, out);
    HRESULT hr = DEV(CreateMetaCommand)(This, CommandId, NodeMask, pCreationParametersData, CreationParametersDataSizeInBytes, riid, out);
    ID3D12MetaCommand* command = Result<ID3D12MetaCommand>(hr, out);
    if (!command) return hr;
    // Only the ID3D12Object slots are patched, so the object's own methods need no count of their own.
    HookD3D12Object(command, "ID3D12MetaCommand", slot::ID3D12Object_Count, {});
    Args a;
    Write(a.key("CommandId"), CommandId);
    a.u("NodeMask", NodeMask).u("CreationParametersDataSizeInBytes", CreationParametersDataSizeInBytes);
    Tracker::Get().Track(command, "ID3D12MetaCommand", "CreateMetaCommand", This, a.str());
    Log("CreateMetaCommand -> ID3D12MetaCommand %p", (void*)command);
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Entry points

typedef HRESULT(WINAPI* PFN_CreateDevice)(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice);
typedef HRESULT(WINAPI* PFN_CreateFactory)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateFactory2)(UINT Flags, REFIID riid, void** ppFactory);

PFN_CreateDevice g_D3D12CreateDevice = nullptr;
PFN_CreateFactory g_CreateDXGIFactory = nullptr;
PFN_CreateFactory g_CreateDXGIFactory1 = nullptr;
PFN_CreateFactory2 g_CreateDXGIFactory2 = nullptr;

HRESULT WINAPI Hook_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    if (Internal()) return g_D3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    // Before the device exists, or the layer cannot attach to it.
    ValidationLog::Get().EnableDebugLayer();
    HRESULT hr = g_D3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    // A null ppDevice only asks whether the adapter supports the level.
    if (FAILED(hr) || !ppDevice || !*ppDevice) return hr;
    ID3D12Device* device = QueryAs<ID3D12Device>(static_cast<IUnknown*>(*ppDevice));
    if (!device) {
        Log("D3D12CreateDevice returned an object without ID3D12Device: not tracked");
        return hr;
    }
    HookDevice(device);
    RecordDeviceCreated(device, pAdapter, MinimumFeatureLevel);
    ValidationLog::Get().OnDeviceCreated(device);
    // The listener comes up once a D3D12 device exists (idempotent), so a Vulkan application
    // with this library injected leaves the port to the Vulkan layer.
    StartTracking();
    Log("D3D12CreateDevice -> ID3D12Device %p", (void*)device);
    return hr;
}

void OnFactoryCreated(const char* cmd, HRESULT hr, void** out) {
    IDXGIFactory* factory = QueryAs<IDXGIFactory>(Result<IUnknown>(hr, out));
    if (!factory) return;
    HookFactory(factory);
    Log("%s -> IDXGIFactory %p", cmd, (void*)factory);
}

HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    if (Internal()) return g_CreateDXGIFactory(riid, ppFactory);
    HRESULT hr = g_CreateDXGIFactory(riid, ppFactory);
    OnFactoryCreated("CreateDXGIFactory", hr, ppFactory);
    return hr;
}

HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    if (Internal()) return g_CreateDXGIFactory1(riid, ppFactory);
    HRESULT hr = g_CreateDXGIFactory1(riid, ppFactory);
    OnFactoryCreated("CreateDXGIFactory1", hr, ppFactory);
    return hr;
}

HRESULT WINAPI Hook_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    if (Internal()) return g_CreateDXGIFactory2(Flags, riid, ppFactory);
    HRESULT hr = g_CreateDXGIFactory2(Flags, riid, ppFactory);
    OnFactoryCreated("CreateDXGIFactory2", hr, ppFactory);
    return hr;
}

bool HookExport(HMODULE module, const char* name, void* replacement, void** original) {
    FARPROC target = GetProcAddress(module, name);
    if (!target) {
        LogAlways("%s is not exported: not hooked", name);
        return false;
    }
    return HookFunction((void*)target, replacement, original, name);
}

}  // namespace

// ---------------------------------------------------------------------------------------------

bool InstallEntryPointHooks() {
    // Loaded here so the exports exist to be patched before the application asks for them; the
    // system d3d12.dll stays the entry even when the Agility SDK's D3D12Core.dll does the work.
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d3d12 || !dxgi) {
        LogAlways("d3d12.dll or dxgi.dll could not be loaded (%lu)", GetLastError());
        return false;
    }
    if (!HookExport(d3d12, "D3D12CreateDevice", (void*)&Hook_D3D12CreateDevice, (void**)&g_D3D12CreateDevice)) return false;
    HookExport(dxgi, "CreateDXGIFactory", (void*)&Hook_CreateDXGIFactory, (void**)&g_CreateDXGIFactory);
    HookExport(dxgi, "CreateDXGIFactory1", (void*)&Hook_CreateDXGIFactory1, (void**)&g_CreateDXGIFactory1);
    HookExport(dxgi, "CreateDXGIFactory2", (void*)&Hook_CreateDXGIFactory2, (void**)&g_CreateDXGIFactory2);
    if (!EnableFunctionHooks()) return false;
    Log("entry point hooks installed");
    return true;
}

void HookDevice(ID3D12Device* device) {
    if (!device || VtableHooked(device)) return;
    HookD3D12Object(device, "ID3D12Device", DeviceSlotCount(device), {
        {slot::ID3D12Device14_CreateCommandQueue, (void*)&Hook_CreateCommandQueue},
        {slot::ID3D12Device14_CreateCommandAllocator, (void*)&Hook_CreateCommandAllocator},
        {slot::ID3D12Device14_CreateGraphicsPipelineState, (void*)&Hook_CreateGraphicsPipelineState},
        {slot::ID3D12Device14_CreateComputePipelineState, (void*)&Hook_CreateComputePipelineState},
        {slot::ID3D12Device14_CreateCommandList, (void*)&Hook_CreateCommandList},
        {slot::ID3D12Device14_CreateDescriptorHeap, (void*)&Hook_CreateDescriptorHeap},
        {slot::ID3D12Device14_CreateRootSignature, (void*)&Hook_CreateRootSignature},
        {slot::ID3D12Device14_CreateConstantBufferView, (void*)&Hook_CreateConstantBufferView},
        {slot::ID3D12Device14_CreateShaderResourceView, (void*)&Hook_CreateShaderResourceView},
        {slot::ID3D12Device14_CreateUnorderedAccessView, (void*)&Hook_CreateUnorderedAccessView},
        {slot::ID3D12Device14_CreateRenderTargetView, (void*)&Hook_CreateRenderTargetView},
        {slot::ID3D12Device14_CreateDepthStencilView, (void*)&Hook_CreateDepthStencilView},
        {slot::ID3D12Device14_CreateSampler, (void*)&Hook_CreateSampler},
        {slot::ID3D12Device14_CopyDescriptors, (void*)&Hook_CopyDescriptors},
        {slot::ID3D12Device14_CopyDescriptorsSimple, (void*)&Hook_CopyDescriptorsSimple},
        {slot::ID3D12Device14_CreateCommittedResource, (void*)&Hook_CreateCommittedResource},
        {slot::ID3D12Device14_CreateHeap, (void*)&Hook_CreateHeap},
        {slot::ID3D12Device14_CreatePlacedResource, (void*)&Hook_CreatePlacedResource},
        {slot::ID3D12Device14_CreateReservedResource, (void*)&Hook_CreateReservedResource},
        {slot::ID3D12Device14_OpenSharedHandle, (void*)&Hook_OpenSharedHandle},
        {slot::ID3D12Device14_CreateFence, (void*)&Hook_CreateFence},
        {slot::ID3D12Device14_CreateQueryHeap, (void*)&Hook_CreateQueryHeap},
        {slot::ID3D12Device14_CreateCommandSignature, (void*)&Hook_CreateCommandSignature},
        // ID3D12Device1
        {slot::ID3D12Device14_CreatePipelineLibrary, (void*)&Hook_CreatePipelineLibrary},
        // ID3D12Device2
        {slot::ID3D12Device14_CreatePipelineState, (void*)&Hook_CreatePipelineState},
        // ID3D12Device3
        {slot::ID3D12Device14_OpenExistingHeapFromAddress, (void*)&Hook_OpenExistingHeapFromAddress},
        {slot::ID3D12Device14_OpenExistingHeapFromFileMapping, (void*)&Hook_OpenExistingHeapFromFileMapping},
        // ID3D12Device4
        {slot::ID3D12Device14_CreateCommandList1, (void*)&Hook_CreateCommandList1},
        {slot::ID3D12Device14_CreateCommittedResource1, (void*)&Hook_CreateCommittedResource1},
        {slot::ID3D12Device14_CreateHeap1, (void*)&Hook_CreateHeap1},
        {slot::ID3D12Device14_CreateReservedResource1, (void*)&Hook_CreateReservedResource1},
        // ID3D12Device5
        {slot::ID3D12Device14_CreateMetaCommand, (void*)&Hook_CreateMetaCommand},
        {slot::ID3D12Device14_CreateStateObject, (void*)&Hook_CreateStateObject},
        // ID3D12Device7
        {slot::ID3D12Device14_AddToStateObject, (void*)&Hook_AddToStateObject},
        // ID3D12Device8
        {slot::ID3D12Device14_CreateCommittedResource2, (void*)&Hook_CreateCommittedResource2},
        {slot::ID3D12Device14_CreatePlacedResource1, (void*)&Hook_CreatePlacedResource1},
        {slot::ID3D12Device14_CreateSamplerFeedbackUnorderedAccessView, (void*)&Hook_CreateSamplerFeedbackUnorderedAccessView},
        // ID3D12Device9
        {slot::ID3D12Device14_CreateCommandQueue1, (void*)&Hook_CreateCommandQueue1},
        // ID3D12Device10
        {slot::ID3D12Device14_CreateCommittedResource3, (void*)&Hook_CreateCommittedResource3},
        {slot::ID3D12Device14_CreatePlacedResource2, (void*)&Hook_CreatePlacedResource2},
        {slot::ID3D12Device14_CreateReservedResource2, (void*)&Hook_CreateReservedResource2},
        // ID3D12Device11
        {slot::ID3D12Device14_CreateSampler2, (void*)&Hook_CreateSampler2},
        // ID3D12Device13
        {slot::ID3D12Device14_OpenExistingHeapFromAddress1, (void*)&Hook_OpenExistingHeapFromAddress1},
        // ID3D12Device14
        {slot::ID3D12Device14_CreateRootSignatureFromSubobjectInLibrary, (void*)&Hook_CreateRootSignatureFromSubobjectInLibrary},
    });
}

// The objects below need nothing beyond the common hooks (Release, SetName, SetPrivateData).
// Where an interface has versions, the vtable is only as long as the version the object
// implements: the count is the slot after the last method of that version, which is the
// first slot of the next version's methods.

void HookCommandAllocator(ID3D12CommandAllocator* allocator) {
    HookD3D12Object(allocator, "ID3D12CommandAllocator", slot::ID3D12CommandAllocator_Count, {});
}

void HookResource(ID3D12Resource* resource) {
    if (!resource || VtableHooked(resource)) return;
    uint32_t count = QueryAs<ID3D12Resource2>(resource)   ? slot::ID3D12Resource2_Count
                     : QueryAs<ID3D12Resource1>(resource) ? slot::ID3D12Resource2_GetDesc1
                                                          : slot::ID3D12Resource2_GetProtectedResourceSession;
    HookD3D12Object(resource, "ID3D12Resource", count, {});
}

void HookHeap(ID3D12Heap* heap) {
    if (!heap || VtableHooked(heap)) return;
    uint32_t count = QueryAs<ID3D12Heap1>(heap) ? slot::ID3D12Heap1_Count : slot::ID3D12Heap1_GetProtectedResourceSession;
    HookD3D12Object(heap, "ID3D12Heap", count, {});
}

void HookDescriptorHeap(ID3D12DescriptorHeap* heap) {
    HookD3D12Object(heap, "ID3D12DescriptorHeap", slot::ID3D12DescriptorHeap_Count, {});
}

void HookRootSignature(ID3D12RootSignature* signature) {
    HookD3D12Object(signature, "ID3D12RootSignature", slot::ID3D12RootSignature_Count, {});
}

void HookPipelineState(ID3D12PipelineState* pipeline) {
    HookD3D12Object(pipeline, "ID3D12PipelineState", slot::ID3D12PipelineState_Count, {});
}

void HookStateObject(ID3D12StateObject* stateObject) {
    HookD3D12Object(stateObject, "ID3D12StateObject", slot::ID3D12StateObject_Count, {});
}

void HookFence(ID3D12Fence* fence) {
    if (!fence || VtableHooked(fence)) return;
    uint32_t count = QueryAs<ID3D12Fence1>(fence) ? slot::ID3D12Fence1_Count : slot::ID3D12Fence1_GetCreationFlags;
    HookD3D12Object(fence, "ID3D12Fence", count, {});
}

void HookQueryHeap(ID3D12QueryHeap* heap) {
    HookD3D12Object(heap, "ID3D12QueryHeap", slot::ID3D12QueryHeap_Count, {});
}

void HookCommandSignature(ID3D12CommandSignature* signature) {
    HookD3D12Object(signature, "ID3D12CommandSignature", slot::ID3D12CommandSignature_Count, {});
}

void HookPipelineLibrary(ID3D12PipelineLibrary* library) {
    if (!library || VtableHooked(library)) return;
    // LoadPipeline is ID3D12PipelineLibrary1's; on a plain ID3D12PipelineLibrary the count
    // stops before it and HookVtable leaves that slot alone.
    uint32_t count = QueryAs<ID3D12PipelineLibrary1>(library) ? slot::ID3D12PipelineLibrary1_Count : slot::ID3D12PipelineLibrary1_LoadPipeline;
    HookD3D12Object(library, "ID3D12PipelineLibrary", count, {
        {slot::ID3D12PipelineLibrary1_LoadGraphicsPipeline, (void*)&Hook_LoadGraphicsPipeline},
        {slot::ID3D12PipelineLibrary1_LoadComputePipeline, (void*)&Hook_LoadComputePipeline},
        {slot::ID3D12PipelineLibrary1_LoadPipeline, (void*)&Hook_LoadPipeline},
    });
}

}  // namespace dxinsp
