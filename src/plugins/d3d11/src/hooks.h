// Installing the hooks. The mechanism is in hook.h; this is what gets hooked.
//
// Each installer is called with the first object of its kind that appears and patches that
// object's vtable, so the tree is discovered from the entry points downwards. They are cheap to
// call repeatedly (each stops at the first sighting of a vtable), and every creation hook calls
// the installer for what it returned, so a wrapper class of the debug layer is found the same way
// as the runtime's own.
#pragma once

#include "common.h"
#include "hook.h"

#include <initializer_list>

namespace d3d11insp {

/** MinHook on D3D11CreateDevice and D3D11CreateDeviceAndSwapChain (platform_win32.cpp calls this once). */
bool InstallEntryPointHooks(HMODULE d3d11);

// Devices and what they create (hooks_device.cpp)
void HookDevice(ID3D11Device* device);
/** A device the entry points returned, tracked with its contexts; the factory it came from is hooked for swap chains. */
void OnDeviceCreated(ID3D11Device* device, ID3D11DeviceContext** immediate, IDXGIAdapter* adapter, const char* cmd, const std::string& args);

// Device contexts (hooks_context.cpp)
/**
 * The proxy the application is given for a context (the immediate one from the device, a deferred
 * one from CreateDeferredContext): a context's own vtable is rewritten by the runtime as the
 * pipeline state changes, so it cannot be patched, and every method is forwarded through an
 * object of the library's instead. The same proxy for the same context every time.
 */
ID3D11DeviceContext* WrapContext(ID3D11DeviceContext* context, ID3D11Device* device, const char* cmd);
/** A context is gone: its record and its proxy go too. */
void DestroyContext(ID3D11DeviceContext* real);
/** The device is gone, and its immediate context with it. */
void DestroyContextsOf(ID3D11Device* device);

// Swap chains and factories (hooks_dxgi.cpp)
void HookSwapChain(IDXGISwapChain* swapChain);
void HookFactory(IDXGIFactory* factory);
/** A swap chain made for a D3D11 device: tracked, its back buffers found, and Present hooked. */
void OnSwapChainCreated(IDXGISwapChain* swapChain, ID3D11Device* device, const char* cmd, const std::string& args);
/** The factory a device's adapter belongs to, hooked so the swap chains made through any factory are seen. */
void HookFactoryOfDevice(ID3D11Device* device);
/** The back buffers of a swap chain are forgotten: it was resized or destroyed. */
void UntrackBackBuffers(uint64_t swapChainId);

/**
 * The methods every ID3D11DeviceChild has, which every installer above adds to its own: Release
 * (a count reaching zero untracks the object) and SetPrivateData (the debug name). `count` is
 * the interface's slot count and `hooks` its own replacements (hooks_object.cpp).
 */
bool HookDeviceChild(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks);
/** The same for a DXGI object (IDXGIObject's SetPrivateData carries the debug name). */
bool HookDxgiObject(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks);

/** The Release hook found an object's count at zero: forgotten everywhere. */
void OnObjectReleased(void* object);
/** SetPrivateData(WKPDID_D3DDebugObjectName[W]): the object's label. */
void OnObjectNamed(void* object, const std::string& name);

}  // namespace d3d11insp
