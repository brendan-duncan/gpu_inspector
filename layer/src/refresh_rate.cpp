#include "refresh_rate.h"

#include "layer.h"

#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace vkinsp {

const char* RefreshSourceName(RefreshSource s) {
    switch (s) {
        case RefreshSource::PresentTiming: return "present_timing";
        case RefreshSource::DisplayTiming: return "display_timing";
        case RefreshSource::Monitor: return "monitor";
        case RefreshSource::Estimate: return "estimate";
        default: return "";
    }
}

static bool HasName(const char* const* names, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i)
        if (names[i] && strcmp(names[i], name) == 0) return true;
    return false;
}

bool AddSurfaceCapabilities2(VkInstanceCreateInfo& info, std::vector<const char*>& names) {
    if (HasName(info.ppEnabledExtensionNames, info.enabledExtensionCount, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) return false;
    names.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
    names.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    info.ppEnabledExtensionNames = names.data();
    info.enabledExtensionCount = (uint32_t)names.size();
    return true;
}

static bool ContainsValidationLayer(const std::string& list) {
    return list.find("VK_LAYER_KHRONOS_validation") != std::string::npos;
}

bool OldValidationLayerEnabled(PFN_vkGetInstanceProcAddr nextGipa, const VkInstanceCreateInfo& info) {
    bool enabled = false;
    for (uint32_t i = 0; i < info.enabledLayerCount; ++i)
        if (info.ppEnabledLayerNames[i] && ContainsValidationLayer(info.ppEnabledLayerNames[i])) enabled = true;
    // Layers the loader enabled from the environment (the launcher's way) are not in the list.
    if (!enabled) enabled = ContainsValidationLayer(ConfigValue("VK_INSTANCE_LAYERS")) || ContainsValidationLayer(ConfigValue("VK_LOADER_LAYERS_ENABLE"));
    if (!enabled) return false;
    // The next layer's vkEnumerateInstanceLayerProperties reports that layer itself.
    auto enumerate = (PFN_vkEnumerateInstanceLayerProperties)nextGipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    uint32_t count = 0;
    if (!enumerate || enumerate(&count, nullptr) != VK_SUCCESS || count == 0) return true;   // unknown age: careful
    std::vector<VkLayerProperties> props(count);
    if (enumerate(&count, props.data()) < VK_SUCCESS) return true;
    for (uint32_t i = 0; i < count; ++i) {
        if (!ContainsValidationLayer(props[i].layerName)) continue;
        const bool old = VK_API_VERSION_PATCH(props[i].specVersion) < VK_HEADER_VERSION;
        if (old) Log("refresh rate: validation layer %u.%u.%u is older than the layer's headers (1.x.%u); VK_EXT_present_timing stays off",
                     VK_API_VERSION_MAJOR(props[i].specVersion), VK_API_VERSION_MINOR(props[i].specVersion), VK_API_VERSION_PATCH(props[i].specVersion), VK_HEADER_VERSION);
        return old;
    }
    return true;
}

// Whether the chain already carries a struct of this type (the application's own features).
static bool ChainHas(const void* pNext, VkStructureType type) {
    for (auto* p = (const VkBaseInStructure*)pNext; p; p = p->pNext)
        if (p->sType == type) return true;
    return false;
}

void PlanRefreshSource(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, RefreshDeviceSetup& setup) {
    if (!inst || !inst->dispatch.EnumerateDeviceExtensionProperties) return;
    // VKINSP_NO_REFRESH_EXTENSIONS=1: leave the device as the application created it (the
    // monitor mode or the estimate then give the refresh period).
    if (ConfigFlag("VKINSP_NO_REFRESH_EXTENSIONS")) return;
    uint32_t count = 0;
    if (inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) return;
    std::vector<VkExtensionProperties> props(count);
    if (inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, props.data()) < VK_SUCCESS) return;
    auto offered = [&](const char* name) {
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(props[i].extensionName, name) == 0) return true;
        return false;
    };
    setup.extensionNames.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
    auto add = [&](const char* name) {
        if (!HasName(setup.extensionNames.data(), (uint32_t)setup.extensionNames.size(), name)) setup.extensionNames.push_back(name);
    };

    // VK_EXT_present_timing: needs its dependencies and the presentTiming feature (plus
    // presentId2, which it builds on). Only when the physical device reports the features and the
    // application did not chain its own copies of the feature structs.
    PFN_vkGetPhysicalDeviceFeatures2 features2 = inst->dispatch.GetPhysicalDeviceFeatures2
        ? inst->dispatch.GetPhysicalDeviceFeatures2 : (PFN_vkGetPhysicalDeviceFeatures2)inst->dispatch.GetPhysicalDeviceFeatures2KHR;
    if (features2 && inst->surfaceCapabilities2 && !inst->oldValidationLayer &&
        offered(VK_EXT_PRESENT_TIMING_EXTENSION_NAME) && offered(VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
        offered(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) &&
        !ChainHas(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT) &&
        !ChainHas(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR)) {
        VkPhysicalDevicePresentTimingFeaturesEXT timing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
        VkPhysicalDevicePresentId2FeaturesKHR id2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
        timing.pNext = &id2;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &timing;
        features2(physicalDevice, &f2);
        if (timing.presentTiming && id2.presentId2) {
            add(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
            add(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
            add(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
            setup.presentTimingFeatures.presentTiming = VK_TRUE;
            setup.presentId2Features.presentId2 = VK_TRUE;
            setup.presentId2Features.pNext = const_cast<void*>(info.pNext);
            setup.presentTimingFeatures.pNext = &setup.presentId2Features;
            info.pNext = &setup.presentTimingFeatures;
            setup.presentTiming = true;
        }
    }
    if (!setup.presentTiming && offered(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) {
        add(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        setup.displayTiming = true;
    }
    if (setup.presentTiming || setup.displayTiming) {
        info.ppEnabledExtensionNames = setup.extensionNames.data();
        info.enabledExtensionCount = (uint32_t)setup.extensionNames.size();
        Log("refresh rate: enabling %s", setup.presentTiming ? "VK_EXT_present_timing" : "VK_GOOGLE_display_timing");
    }
}

bool SurfaceSupportsPresentTiming(DeviceData* dev, VkSurfaceKHR surface) {
    if (!dev || !dev->presentTiming || !dev->instance->dispatch.GetPhysicalDeviceSurfaceCapabilities2KHR) return false;
    VkPresentTimingSurfaceCapabilitiesEXT timing{VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
    VkSurfaceCapabilities2KHR caps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
    caps.pNext = &timing;
    VkPhysicalDeviceSurfaceInfo2KHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
    info.surface = surface;
    if (dev->instance->dispatch.GetPhysicalDeviceSurfaceCapabilities2KHR(dev->physicalDevice, &info, &caps) != VK_SUCCESS) return false;
    return timing.presentTimingSupported == VK_TRUE;
}

#if defined(_WIN32)
// The refresh rate of the monitor showing the process's main window (its largest visible
// top-level window), in ms; 0 when unknown.
static double MonitorRefreshMs() {
    struct Best { HWND hwnd = nullptr; long area = 0; } best;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        Best* b = (Best*)param;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
        RECT r;
        if (!GetWindowRect(hwnd, &r)) return TRUE;
        long area = (long)(r.right - r.left) * (long)(r.bottom - r.top);
        if (area > b->area) { b->area = area; b->hwnd = hwnd; }
        return TRUE;
    }, (LPARAM)&best);
    if (!best.hwnd) return 0;
    HMONITOR monitor = MonitorFromWindow(best.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) return 0;
    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    // 0 and 1 mean "the hardware default": unknown to us. The mode frequency is an integer;
    // the NTSC-derived rates are reported rounded down (59 for 59.94 Hz).
    if (dm.dmDisplayFrequency < 2) return 0;
    double hz = (double)dm.dmDisplayFrequency;
    switch (dm.dmDisplayFrequency) {
        case 23: hz = 23.976; break;
        case 29: hz = 29.97; break;
        case 47: hz = 47.952; break;
        case 59: hz = 59.94; break;
        case 71: hz = 71.928; break;
        case 119: hz = 119.88; break;
        case 143: hz = 143.856; break;
        case 239: hz = 239.76; break;
        default: break;
    }
    return 1000.0 / hz;
}
#endif

double QueryRefreshMs(DeviceData* dev, VkSwapchainKHR swapchain, RefreshSource& source) {
    source = RefreshSource::Unknown;
    if (!dev) return 0;
    if (dev->presentTiming) {
        VkSwapchainTimingPropertiesEXT props{VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
        uint64_t counter = 0;
        VkResult r = dev->dispatch.GetSwapchainTimingPropertiesEXT
            ? dev->dispatch.GetSwapchainTimingPropertiesEXT(dev->device, swapchain, &props, &counter) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (r == VK_SUCCESS && props.refreshDuration > 0) {
            source = RefreshSource::PresentTiming;
            return (double)props.refreshDuration / 1e6;
        }
        Log("refresh rate: vkGetSwapchainTimingPropertiesEXT gave %d (refreshDuration %llu)", (int)r, (unsigned long long)props.refreshDuration);
    }
    if (dev->displayTiming && dev->dispatch.GetRefreshCycleDurationGOOGLE) {
        VkRefreshCycleDurationGOOGLE cycle{};
        if (dev->dispatch.GetRefreshCycleDurationGOOGLE(dev->device, swapchain, &cycle) == VK_SUCCESS && cycle.refreshDuration > 0) {
            source = RefreshSource::DisplayTiming;
            return (double)cycle.refreshDuration / 1e6;
        }
    }
#if defined(_WIN32)
    double ms = MonitorRefreshMs();
    if (ms > 0) {
        source = RefreshSource::Monitor;
        return ms;
    }
#endif
    return 0;
}

}  // namespace vkinsp
