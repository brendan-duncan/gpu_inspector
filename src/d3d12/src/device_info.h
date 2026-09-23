// What the adapter and the device offer, recorded on their objects the way the Vulkan layer
// records a physical device's properties and limits: the adapter's description, and the device's
// feature level and CheckFeatureSupport results as an ObjectUpdate named "features".
#pragma once

#include "common.h"

namespace dxinsp {

/**
 * After D3D12CreateDevice succeeded: tracks the adapter (IDXGIAdapter, from the pAdapter the
 * application passed or the device's LUID) and the device under it, with the adapter description,
 * the feature level asked for and reached, and the device's features.
 */
void RecordDeviceCreated(ID3D12Device* device, IUnknown* adapterArgument, D3D_FEATURE_LEVEL minimumFeatureLevel);

/** The adapter a device was created on (tracked), or null. */
IDXGIAdapter* AdapterOf(ID3D12Device* device);

/**
 * The device's Release reached zero (hooks_object.cpp, OnObjectDestroyed): drops its record, and
 * the reference the library holds on an adapter it enumerated itself (an application that
 * passed no adapter to D3D12CreateDevice).
 */
void OnDeviceReleased(ID3D12Device* device);

/** Frame statistics: the frame time of every present, reported every 100 ms (README.md, "Frame boundary"). */
/** `calledAtQpc`: QueryPerformanceCounter just before the Present call, for the present latency. */
void OnFramePresented(ID3D12Device* device, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, HRESULT result, LONGLONG calledAtQpc);
/** The same for a device that never presents, whose frames end at a submit (see the capture manager's OnExecuteCommandLists). */
void OnFrameNoPresent(ID3D12Device* device);
/** The CPU time inside ExecuteCommandLists, for FrameStats.submitMs. */
void AddSubmitTime(ID3D12Device* device, double milliseconds);

}  // namespace dxinsp
