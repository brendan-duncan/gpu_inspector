// Device-lost diagnostics: which command the GPU was running when it stopped responding.
//
// A GPU hang surfaces as VK_ERROR_DEVICE_LOST from a submit, a wait or a present, and that result
// says nothing about the cause: by the time the driver reports it the queue is gone. The standard
// answer is breadcrumbs — have the GPU write a marker into host-visible memory as it passes each
// command, so after the loss the host can read how far it got. Nsight Aftermath does this on
// NVIDIA; `VK_AMD_buffer_marker` is the vendor-neutral form of the same idea and is what this uses
// (NVIDIA's driver implements it too, so it is not AMD-only in practice).
//
// Two markers bracket every action: one written at the top of the pipe before it, one at the bottom
// after. On device loss the pair says whether the last command the GPU began also finished, which
// is the difference between "this draw hung" and "the hang was after this draw".
//
// It costs two extra GPU writes per draw and dispatch, so it is off unless asked for:
// VKINSP_BREADCRUMBS=1, or "Device-lost breadcrumbs" in the launch dialog.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vkinsp
{

struct DeviceData;
struct InstanceData;

/** What a device asked for at creation, decided before vkCreateDevice (see PlanBreadcrumbs). */
struct BreadcrumbSetup
{
    /** Our copy of the device's extension list, when the layer had to extend it. */
    std::vector<const char*> extensionNames;
    /** The application asked for breadcrumbs and the device can write buffer markers. */
    bool wanted = false;
    /** The layer added VK_AMD_buffer_marker to the device's extensions. */
    bool added = false;
};

/**
 * Adds `VK_AMD_buffer_marker` to a device being created when breadcrumbs are on and the physical
 * device offers it. `info` is the layer's copy of the application's create info.
 */
void PlanBreadcrumbs(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, BreadcrumbSetup& setup);

/** Allocates the marker buffer once the device exists; quiet no-op when breadcrumbs are off. */
void CreateBreadcrumbs(DeviceData* dev, const BreadcrumbSetup& setup);

/** Frees it with the device. */
void DestroyBreadcrumbs(DeviceData* dev);

/** Whether breadcrumbs are running on this device, so the generated entry points can skip the call. */
bool BreadcrumbsEnabled(const DeviceData* dev);

/**
 * Before an action is recorded: notes what it is and writes a marker at the top of the pipe.
 * Returns the marker's ordinal to pass to EndBreadcrumb, or 0 when breadcrumbs are off.
 */
uint32_t BeginBreadcrumb(DeviceData* dev, VkCommandBuffer cb, uint32_t cmdId);

/** After it: writes the same ordinal at the bottom of the pipe, so a finished action is known to be. */
void EndBreadcrumb(DeviceData* dev, VkCommandBuffer cb, uint32_t ordinal);

/**
 * A call returned VK_ERROR_DEVICE_LOST. Reads the breadcrumbs, writes the diagnosis to the log and
 * sends it to the UI. `call` is the entry point that reported it. Reports once per device: every
 * later call fails the same way and would repeat it.
 */
void OnDeviceLost(DeviceData* dev, const char* call, bool pretendIncomplete = false);

/**
 * VKINSP_SIMULATE_DEVICE_LOST=<n>[:hung]: reports a device loss after the nth queue submission
 * without one happening, so the diagnosis path can be exercised without hanging the GPU. The GPU is
 * usually idle at that moment, which reads as "finished everything"; `:hung` additionally reports
 * the last action as unfinished, which is what a real hang looks like and the only way to reach
 * that branch without hanging a GPU. Returns true when this submission should be treated as lost,
 * with `pretendIncomplete` set for `:hung`.
 */
bool SimulateDeviceLost(DeviceData* dev, bool* pretendIncomplete = nullptr);

} // namespace vkinsp
