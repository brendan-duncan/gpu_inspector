// The hooks every object gets, whatever it is: Release, whose count reaching zero is the object's
// death and the inspector's DeleteObjects; SetPrivateData, which carries the debug name the
// Inspect panel shows. One replacement serves every vtable, since IUnknown's and
// ID3D11DeviceChild's slots are the same in every device child interface (IDXGIObject's differ,
// so DXGI objects get their own).
#include "hooks.h"

#include "../gen/d3d11_vtables.gen.h"
#include "capture.h"
#include "state.h"

#include <initguid.h>
#include <d3dcommon.h>

#include <vector>

namespace d3d11insp
{

namespace
{

ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* This)
{
    // Whether it is tracked is looked up before the call: once the count is zero the object is gone.
    // A swap chain's back buffer is not: its count reaches zero as soon as the application lets
    // its own reference go, and the swap chain keeps it alive; it goes with the swap chain
    // (hooks_dxgi.cpp) or its ResizeBuffers.
    bool tracked = false, backBuffer = false;
    if (!Internal())
    {
        if (Object* o = Find(This))
        {
            tracked = true;
            backBuffer = o->swapChain != 0;
        }
    }
    ULONG count = Orig<PFN_ID3D11DeviceChild_Release>(This, slot::ID3D11DeviceChild_Release)((ID3D11DeviceChild*)This);
    if (count == 0 && tracked && !backBuffer)
        OnObjectReleased(This);
    return count;
}

void NameFromPrivateData(void* object, REFGUID guid, UINT size, const void* data)
{
    if (Internal() || !data || !size)
        return;
    if (guid == WKPDID_D3DDebugObjectName)
    {
        OnObjectNamed(object, std::string((const char*)data, size && ((const char*)data)[size - 1] == 0 ? size - 1 : size));
    }
    else if (guid == WKPDID_D3DDebugObjectNameW)
    {
        OnObjectNamed(object, Narrow((const wchar_t*)data, size / sizeof(wchar_t)));
    }
}

HRESULT STDMETHODCALLTYPE Hook_SetPrivateData(ID3D11DeviceChild* This, REFGUID guid, UINT size, const void* data)
{
    HRESULT hr = Orig<PFN_ID3D11DeviceChild_SetPrivateData>(This, slot::ID3D11DeviceChild_SetPrivateData)(This, guid, size, data);
    if (SUCCEEDED(hr))
        NameFromPrivateData(This, guid, size, data);
    return hr;
}

// IDXGIObject: QueryInterface 0, AddRef 1, Release 2, SetPrivateData 3, SetPrivateDataInterface 4, GetPrivateData 5, GetParent 6.
HRESULT STDMETHODCALLTYPE Hook_DxgiSetPrivateData(IDXGIObject* This, REFGUID guid, UINT size, const void* data)
{
    HRESULT hr = Orig<PFN_IDXGISwapChain4_SetPrivateData>(This, slot::IDXGISwapChain4_SetPrivateData)((IDXGISwapChain4*)This, guid, size, data);
    if (SUCCEEDED(hr))
        NameFromPrivateData(This, guid, size, data);
    return hr;
}

}  // namespace

bool HookDeviceChild(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks)
{
    std::vector<SlotHook> all = {
        {slot::ID3D11DeviceChild_Release, (void*)&Hook_Release},
        {slot::ID3D11DeviceChild_SetPrivateData, (void*)&Hook_SetPrivateData},
    };
    all.insert(all.end(), hooks.begin(), hooks.end());
    return HookVtable(object, interfaceName, count, std::initializer_list<SlotHook>(all.data(), all.data() + all.size()));
}

bool HookDxgiObject(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks)
{
    std::vector<SlotHook> all = {
        {slot::IDXGISwapChain4_Release, (void*)&Hook_Release},
        {slot::IDXGISwapChain4_SetPrivateData, (void*)&Hook_DxgiSetPrivateData},
    };
    all.insert(all.end(), hooks.begin(), hooks.end());
    return HookVtable(object, interfaceName, count, std::initializer_list<SlotHook>(all.data(), all.data() + all.size()));
}

void OnObjectReleased(void* object)
{
    if (Object* o = Find(object))
    {
        if (o->kind == ObjKind::SwapChain)
            UntrackBackBuffers(o->id);
    }
    OnObjectDestroyed(object);
    Untrack(object);
}

void OnObjectNamed(void* object, const std::string& name)
{
    if (Object* o = Find(object))
        SetLabel(*o, name);
}

}  // namespace d3d11insp
