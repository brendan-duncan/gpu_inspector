// Device-removed diagnostics: what the GPU was doing when it stopped responding.
//
// The D3D12 counterpart of the Vulkan layer's breadcrumbs (src/vulkan/src/device_lost.h), and it
// needs far less machinery, because D3D12 has the feature built in. Device Removed Extended Data
// asks the runtime to keep, for every command list, the render operations it has begun and
// finished, and to remember the virtual address of any page fault. All the library has to do is
// turn it on before the device exists and read it out afterwards; there are no markers to insert,
// so unlike the Vulkan side there is no per-draw cost and it is on by default.
//
// `DXGI_ERROR_DEVICE_REMOVED` by itself says nothing: the call that reports it is usually a Present
// long after the command that caused it. DRED turns that into "the GPU was executing this draw, in
// this command list", and for a page fault, the address and the resources that were allocated near
// it — which is how a use-after-free of a resource is identified.
#pragma once

#include <d3d12.h>

#include <string>

namespace dxinsp {

/**
 * Turns DRED on. Must run before the device is created, which is the only chance the runtime gives:
 * it decides then whether to keep breadcrumbs at all. `DXINSP_NO_DRED=1` leaves it off, for
 * measuring without it or working around a runtime that misbehaves with it.
 */
void EnableDeviceRemovedData();

/**
 * A call reported that the device has gone. Reads DRED and writes the diagnosis to the log and the
 * UI: the command lists that were executing, the last operation each reached, and the page fault's
 * address with the resources around it. Reports once per device; later calls fail the same way.
 *
 * `call` is the entry point that reported it. `device` may be null, in which case only the call is
 * reported, since DRED lives on the device.
 */
void OnDeviceRemoved(ID3D12Device* device, const char* call);

/**
 * Whether `hr` means the device has gone, so the caller can ask for the diagnosis. Covers the
 * removal codes a Present or a fence wait returns.
 */
bool IsDeviceRemoved(HRESULT hr);

/**
 * `DXINSP_SIMULATE_DEVICE_REMOVED=<n>` (default 1): report a removal at the nth present without one
 * happening, so the path can be exercised without hanging the GPU and tripping a driver reset.
 * Returns true exactly once.
 *
 * It reaches the report, not the data: the runtime hands out breadcrumbs and page faults only once
 * the device really has been removed, and answers DXGI_ERROR_NOT_CURRENTLY_AVAILABLE until then. So
 * a simulated removal says the breadcrumbs are not known, which is the truth, and only a real
 * removal names the operation.
 */
bool SimulateDeviceRemoved();

} // namespace dxinsp
