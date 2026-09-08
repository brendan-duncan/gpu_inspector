// Core layer state: per-instance / per-device data and dispatch table lookup.
#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vk_dispatch.gen.h"

namespace vkinsp { class CommandRecorder; }

#if defined(_WIN32)
// Exports are listed in layer.def (vk_layer.h already declares the negotiate prototype).
#define VKINSP_EXPORT extern "C"
#else
#define VKINSP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define VKINSP_LAYER_NAME "VK_LAYER_INSPECTOR_capture"
#define VKINSP_LAYER_DESCRIPTION "GPU Inspector Vulkan capture layer"
#define VKINSP_LAYER_IMPL_VERSION 1

namespace vkinsp {

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    InstanceDispatch dispatch;
    PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr = nullptr;
    uint32_t apiVersion = VK_API_VERSION_1_0;
    std::string appName;
    std::string engineName;
    std::vector<std::string> enabledExtensions;
    // The layer's own VK_EXT_debug_utils messenger (see validation.h); null when unavailable.
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    InstanceData* instance = nullptr;
    DeviceDispatch dispatch;
    PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr = nullptr;
    std::vector<std::string> enabledExtensions;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    // Queue -> queue family (from vkGetDeviceQueue), and transient command pools per family used
    // for live image readback (see image_readback.cpp).
    std::mutex queueMutex;
    std::unordered_map<VkQueue, uint32_t> queueFamilies;
    std::unordered_map<uint32_t, VkCommandPool> readbackPools;

    // Command recorders for buffers begun during a frame capture (see capture.cpp).
    std::shared_mutex recorderMutex;
    std::unordered_map<VkCommandBuffer, std::unique_ptr<CommandRecorder>> recorders;

    uint64_t frameIndex = 0;
    std::chrono::steady_clock::time_point lastPresent{};
    std::chrono::steady_clock::time_point lastReport{};
    double frameTimeAccumMs = 0;
    double frameTimeMinMs = 0;
    double frameTimeMaxMs = 0;
    uint32_t frameTimeCount = 0;
    // CPU time spent inside vkQueueSubmit* since the last report (the "CPU submit" meter line).
    std::atomic<uint64_t> submitNanos{0};

    // Returns the recorder for a command buffer that is being captured, else nullptr.
    // Cheap when no capture is active: a single relaxed atomic load.
    inline CommandRecorder* RecorderFor(VkCommandBuffer cb);
};

extern std::atomic<bool> g_captureActive;
CommandRecorder* LookupRecorder(DeviceData* dev, VkCommandBuffer cb);

inline CommandRecorder* DeviceData::RecorderFor(VkCommandBuffer cb) {
    if (!g_captureActive.load(std::memory_order_relaxed)) return nullptr;
    return LookupRecorder(this, cb);
}

// The loader stores a pointer to its dispatch table as the first word of every dispatchable
// object. All objects belonging to the same instance (VkInstance, VkPhysicalDevice) share one
// key, and all objects of the same device (VkDevice, VkQueue, VkCommandBuffer) share another.
inline void* DispatchKey(const void* dispatchableHandle) {
    return *reinterpret_cast<void* const*>(dispatchableHandle);
}

InstanceData* FindInstance(void* key);
DeviceData* FindDevice(void* key);
void RegisterInstance(void* key, std::unique_ptr<InstanceData> data);
void RegisterDevice(void* key, std::unique_ptr<DeviceData> data);
void UnregisterInstance(void* key);
void UnregisterDevice(void* key);

template <typename H>
inline InstanceData* GetInstanceData(H handle) {
    return FindInstance(DispatchKey(handle));
}

template <typename H>
inline DeviceData* GetDeviceData(H handle) {
    return FindDevice(DispatchKey(handle));
}

template <typename H>
inline InstanceDispatch* GetInstanceDispatch(H handle) {
    return &GetInstanceData(handle)->dispatch;
}

template <typename H>
inline DeviceDispatch* GetDeviceDispatch(H handle) {
    return &GetDeviceData(handle)->dispatch;
}

// Logging (enabled with VKINSP_LOG=1). Always goes to stderr and, on Windows, the debugger.
void Log(const char* fmt, ...);
bool LogEnabled();

// Layer configuration: VKINSP_* environment variables on desktop, or the matching system
// properties on Android (VKINSP_PORT -> debug.vkinsp.port), since an Android app inherits no
// environment; the inspector sets them with `adb shell setprop`. Returns "" when unset.
std::string ConfigValue(const char* envName);
// True when the setting is present and not "0".
bool ConfigFlag(const char* envName);

} // namespace vkinsp
