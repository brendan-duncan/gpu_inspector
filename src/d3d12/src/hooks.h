// Installing the hooks. The mechanism is in hook.h; this is what gets hooked.
//
// Each installer is called with the first object of its kind that appears and patches that
// object's vtable, so the tree is discovered from the entry points downwards. They are cheap to
// call repeatedly — each stops at the first sighting of a vtable — and every creation hook calls
// the installer for what it returned, so a wrapper class of the debug layer is found the same way
// as the runtime's own.
#pragma once

#include "common.h"

#include <initializer_list>

#include "hook.h"

namespace dxinsp
{

/** MinHook on D3D12CreateDevice and the CreateDXGIFactory family (main.cpp calls this once). */
bool InstallEntryPointHooks();

// Devices and what they create (hooks_device.cpp)
void HookDevice(ID3D12Device* device);
void HookCommandAllocator(ID3D12CommandAllocator* allocator);
void HookResource(ID3D12Resource* resource);
void HookHeap(ID3D12Heap* heap);
void HookDescriptorHeap(ID3D12DescriptorHeap* heap);
void HookRootSignature(ID3D12RootSignature* signature);
void HookPipelineState(ID3D12PipelineState* pipeline);
void HookStateObject(ID3D12StateObject* stateObject);
void HookFence(ID3D12Fence* fence);
/**
 * A fence opened from a handle another device or process shared (OpenSharedHandle), marked on the
 * fence itself so a later object at the same address is not taken for it. A queue waiting on one
 * is taking a resource back from whoever shared it: Dawn in Chrome, a WebGPU canvas from the
 * compositor, once per page frame.
 */
void MarkOpenedSharedFence(ID3D12Fence* fence);
bool IsOpenedSharedFence(ID3D12Fence* fence);
void HookQueryHeap(ID3D12QueryHeap* heap);
void HookCommandSignature(ID3D12CommandSignature* signature);
void HookPipelineLibrary(ID3D12PipelineLibrary* library);

// Command lists (hooks_command_list.cpp)
void HookCommandList(ID3D12GraphicsCommandList* list);

// Queues, swap chains and factories (hooks_queue.cpp)
void HookCommandQueue(ID3D12CommandQueue* queue);
void HookSwapChain(IDXGISwapChain* swapChain);
void HookFactory(IDXGIFactory* factory);
void HookAdapter(IDXGIAdapter* adapter);

/**
 * The methods every ID3D12Object has, which every installer above adds to its own: Release (a
 * count reaching zero untracks the object), SetName and SetPrivateData (labels). `count` is the
 * interface's slot count and `hooks` its own replacements (hooks_object.cpp).
 */
bool HookD3D12Object(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks);
/** The same for a DXGI object (IDXGIObject's SetPrivateData carries the debug name). */
bool HookDxgiObject(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks);

/**
 * Called by the Release hook when an object's count reached zero, before the entry is dropped:
 * every module that keeps state under the object forgets it (hooks_object.cpp dispatches on the
 * tracked type).
 */
void OnObjectDestroyed(void* object, const std::string& type);

/** The label hooks' result: the tracker's label from SetName / SetPrivateData(WKPDID_D3DDebugObjectName[W]). */
void OnObjectNamed(void* object, const std::string& name);

/** The device a device child belongs to, through GetDevice (the library's own call). */
ID3D12Device* DeviceOf(ID3D12DeviceChild* child);

}  // namespace dxinsp
