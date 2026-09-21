// The hooks every object gets, whatever it is: Release, whose count reaching zero is the object's
// death and the tracker's DeleteObjects; SetName and SetPrivateData, which carry the debug name
// the Inspect panel shows. One replacement serves every vtable, since IUnknown's and
// ID3D12Object's slots are the same in every D3D12 interface (IDXGIObject's differ, so DXGI
// objects get their own).
#include "cpu_timeline.h"
#include "hooks.h"

#include "capture.h"
#include "d3d12_vtables.gen.h"
#include "descriptors.h"
#include "device_info.h"
#include "image_readback.h"
#include "raytracing.h"
#include "resources.h"
#include "shader_edit.h"
#include "tracker.h"
#include "validation.h"

#include <initguid.h>
#include <d3dcommon.h>

#include <vector>

namespace dxinsp {

namespace {

ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* This) {
    // The type is looked up before the call: once the count is zero the object is gone.
    std::string type = Internal() ? std::string() : Tracker::Get().TypeOf(This);
    ULONG count = Orig<PFN_ID3D12Object_Release>(This, slot::ID3D12Object_Release)((ID3D12Object*)This);
    if (count == 0 && !type.empty()) OnObjectDestroyed(This, type);
    return count;
}

HRESULT STDMETHODCALLTYPE Hook_SetName(ID3D12Object* This, LPCWSTR name) {
    HRESULT hr = Orig<PFN_ID3D12Object_SetName>(This, slot::ID3D12Object_SetName)(This, name);
    if (!Internal() && name) OnObjectNamed(This, Narrow(name));
    return hr;
}

void NameFromPrivateData(void* object, REFGUID guid, UINT size, const void* data) {
    if (Internal() || !data || !size) return;
    if (guid == WKPDID_D3DDebugObjectName) {
        OnObjectNamed(object, std::string((const char*)data, size && ((const char*)data)[size - 1] == 0 ? size - 1 : size));
    } else if (guid == WKPDID_D3DDebugObjectNameW) {
        OnObjectNamed(object, Narrow((const wchar_t*)data, size / sizeof(wchar_t)));
    }
}

HRESULT STDMETHODCALLTYPE Hook_SetPrivateData(ID3D12Object* This, REFGUID guid, UINT size, const void* data) {
    HRESULT hr = Orig<PFN_ID3D12Object_SetPrivateData>(This, slot::ID3D12Object_SetPrivateData)(This, guid, size, data);
    if (SUCCEEDED(hr)) NameFromPrivateData(This, guid, size, data);
    return hr;
}

// IDXGIObject: QueryInterface 0, AddRef 1, Release 2, SetPrivateData 3, SetPrivateDataInterface 4, GetPrivateData 5, GetParent 6.
HRESULT STDMETHODCALLTYPE Hook_DxgiSetPrivateData(IDXGIObject* This, REFGUID guid, UINT size, const void* data) {
    HRESULT hr = Orig<PFN_IDXGISwapChain4_SetPrivateData>(This, slot::IDXGISwapChain4_SetPrivateData)((IDXGISwapChain4*)This, guid, size, data);
    if (SUCCEEDED(hr)) NameFromPrivateData(This, guid, size, data);
    return hr;
}

}  // namespace

bool HookD3D12Object(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks) {
    std::vector<SlotHook> all = {
        {slot::ID3D12Object_Release, (void*)&Hook_Release},
        {slot::ID3D12Object_SetName, (void*)&Hook_SetName},
        {slot::ID3D12Object_SetPrivateData, (void*)&Hook_SetPrivateData},
    };
    all.insert(all.end(), hooks.begin(), hooks.end());
    return HookVtable(object, interfaceName, count, std::initializer_list<SlotHook>(all.data(), all.data() + all.size()));
}

bool HookDxgiObject(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks) {
    std::vector<SlotHook> all = {
        {slot::IDXGISwapChain4_Release, (void*)&Hook_Release},
        {slot::IDXGISwapChain4_SetPrivateData, (void*)&Hook_DxgiSetPrivateData},
    };
    all.insert(all.end(), hooks.begin(), hooks.end());
    return HookVtable(object, interfaceName, count, std::initializer_list<SlotHook>(all.data(), all.data() + all.size()));
}

void OnObjectNamed(void* object, const std::string& name) {
    Tracker::Get().SetLabel(object, name);
    // A buffer's name is also the name of any acceleration structure in it (raytracing.h). Only a
    // resource can hold one, so the check is cheap for everything else.
    if (Tracker::Get().TypeOf(object) == "ID3D12Resource") OnResourceNamed(static_cast<ID3D12Resource*>(object), name);
}

void OnObjectDestroyed(void* object, const std::string& type) {
    // Every module that keeps state under the object, then the tracker (which streams the delete).
    // Whatever it held comes off the memory series' running total (cpu_timeline.h); quiet for an
    // object that held none.
    NoteMemoryReleased(object);
    if (type == "ID3D12Resource") {
        ResourceTracker::Get().OnReleased((ID3D12Resource*)object);
        // Before the AddressMap forgets it: that map is what says which structures were in it.
        ForgetStructuresIn((ID3D12Resource*)object);
        AddressMap::Get().Remove((ID3D12Resource*)object);
    } else if (type == "ID3D12DescriptorHeap") {
        DescriptorTracker::Get().OnHeapReleased((ID3D12DescriptorHeap*)object);
    } else if (type == "ID3D12RootSignature") {
        RootSignatures::Get().Forget((ID3D12RootSignature*)object);
    } else if (type == "ID3D12PipelineState") {
        ShaderEditor::Get().OnPipelineReleased((ID3D12PipelineState*)object);
    } else if (type == "ID3D12StateObject") {
        ForgetStateObject((ID3D12StateObject*)object);
    } else if (type == "ID3D12GraphicsCommandList") {
        CaptureManager::Get().OnListReleased((ID3D12GraphicsCommandList*)object);
        ResourceTracker::Get().OnListReleased((ID3D12CommandList*)object);
    } else if (type == "ID3D12CommandQueue") {
        OnQueueReleased((ID3D12CommandQueue*)object);
    } else if (type == "IDXGISwapChain") {
        CaptureManager::Get().OnSwapChainReleased((IDXGISwapChain*)object);
        Tracker::Get().UntrackWithChildren(object);
        return;
    } else if (type == "ID3D12Device") {
        Tracker::Get().SendLeakReport(object);
        ValidationLog::Get().OnDeviceReleased((ID3D12Device*)object);
        CaptureManager::Get().OnDeviceReleased((ID3D12Device*)object);
        dxinsp::OnDeviceReleased((ID3D12Device*)object);
        ResetRaytracing();
        Tracker::Get().UntrackWithChildren(object);
        return;
    }
    Tracker::Get().Untrack(object);
}

ID3D12Device* DeviceOf(ID3D12DeviceChild* child) {
    if (!child) return nullptr;
    ScopedInternal internal;
    ID3D12Device* device = nullptr;
    if (FAILED(child->GetDevice(IID_PPV_ARGS(&device))) || !device) return nullptr;
    // GetDevice AddRefs; the library holds no references of its own to the application's objects.
    device->Release();
    return device;
}

}  // namespace dxinsp
