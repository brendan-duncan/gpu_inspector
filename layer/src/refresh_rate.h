// The display refresh period, from the best source the platform offers:
//   1. VK_EXT_present_timing: the swapchain's refreshDuration (the layer enables the extension,
//      its dependencies and the presentTiming feature at device creation when the physical
//      device offers them, and creates swapchains with the present-timing flag).
//   2. VK_GOOGLE_display_timing: vkGetRefreshCycleDurationGOOGLE (Android, some Linux drivers).
//   3. The monitor's current mode (Windows: the process's window, EnumDisplaySettings).
// Without any of these the frame-interval estimate in layer.cpp stands in.
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace vkinsp {

struct InstanceData;
struct DeviceData;

enum class RefreshSource { None, PresentTiming, DisplayTiming, Monitor, Estimate };
const char* RefreshSourceName(RefreshSource s);

// Adds VK_KHR_get_surface_capabilities2 (a dependency of VK_EXT_present_timing) to the instance
// extensions unless enabled; `names` owns the new list. Returns whether it was added: a layer
// below cannot tell us what the loader offers, so vkCreateInstance is tried with it and retried
// without on VK_ERROR_EXTENSION_NOT_PRESENT.
bool AddSurfaceCapabilities2(VkInstanceCreateInfo& info, std::vector<const char*>& names);

// Whether an enabled Khronos validation layer below us is older than the headers this layer
// was built with (its specVersion), in which case it does not know the newer extensions.
bool OldValidationLayerEnabled(PFN_vkGetInstanceProcAddr nextGipa, const VkInstanceCreateInfo& info);

// What to add to a device's create info for a refresh-period source: the extensions, and for
// VK_EXT_present_timing the feature structs to chain. Fills `info` (a copy of the application's)
// in place; `storage` keeps the new arrays alive for the call.
struct RefreshDeviceSetup {
    std::vector<const char*> extensionNames;
    VkPhysicalDevicePresentTimingFeaturesEXT presentTimingFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    VkPhysicalDevicePresentId2FeaturesKHR presentId2Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    bool presentTiming = false;
    bool displayTiming = false;
};
void PlanRefreshSource(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, RefreshDeviceSetup& storage);

// Whether a surface supports present timing (VK_EXT_present_timing), for the swapchain flag.
bool SurfaceSupportsPresentTiming(DeviceData* dev, VkSurfaceKHR surface);

// The swapchain's refresh period in milliseconds through the device's source, 0 when unknown.
double QueryRefreshMs(DeviceData* dev, VkSwapchainKHR swapchain, RefreshSource& source);

}  // namespace vkinsp
