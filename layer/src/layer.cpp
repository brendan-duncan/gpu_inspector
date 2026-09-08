// Layer core: loader negotiation, instance/device chaining, dispatch registry, logging,
// frame boundary, and the UI message handler.
//
// The layer structure follows the standard Khronos layer pattern (also used by RenderDoc's
// renderdoc/driver/vulkan/vk_layer.cpp): find the VkLayer*CreateInfo link in the pNext chain,
// fetch the next layer's GetProcAddr, advance the chain, and create the object downstream.

#include "layer.h"
#include "capture.h"
#include "descriptors.h"
#include "json_parse.h"
#include "shader_edit.h"
#include "tracker.h"
#include "transport.h"
#include "validation.h"
#include "image_readback.h"
#include "vk_commands.gen.h"
#include "vk_serialize.gen.h"

#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace vkinsp {

// ---------------------------------------------------------------------------------------------
// Configuration

std::string ConfigValue(const char* envName) {
#if defined(__ANDROID__)
    // VKINSP_PORT -> debug.vkinsp.port. "debug." properties can be set from `adb shell` without
    // root, which is how RenderDoc's Android layer takes its settings too.
    std::string name = "debug.vkinsp.";
    const char* suffix = strncmp(envName, "VKINSP_", 7) == 0 ? envName + 7 : envName;
    for (const char* p = suffix; *p; ++p) name.push_back((char)tolower((unsigned char)*p));
    char value[PROP_VALUE_MAX] = {};
    int len = __system_property_get(name.c_str(), value);
    return len > 0 ? std::string(value, (size_t)len) : std::string();
#else
    const char* v = getenv(envName);
    return v ? std::string(v) : std::string();
#endif
}

bool ConfigFlag(const char* envName) {
    std::string v = ConfigValue(envName);
    return !v.empty() && v != "0";
}

// ---------------------------------------------------------------------------------------------
// Logging

static int g_logEnabled = -1;

bool LogEnabled() {
    if (g_logEnabled < 0) g_logEnabled = ConfigFlag("VKINSP_LOG") ? 1 : 0;
    return g_logEnabled == 1;
}

// VKINSP_LOG_FILE=<path> appends the log to a file as well (GUI applications such as Unity
// players have no usable stderr).
static FILE* LogFile() {
    static FILE* file = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::string path = ConfigValue("VKINSP_LOG_FILE");
        if (!path.empty()) file = fopen(path.c_str(), "a");
    }
    return file;
}

void Log(const char* fmt, ...) {
    if (!LogEnabled()) return;
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
#if defined(__ANDROID__)
    // An Android app has no usable stderr; the inspector reads the "vkinsp" tag from logcat.
    __android_log_write(ANDROID_LOG_INFO, "vkinsp", buf);
#else
    fprintf(stderr, "[vkinsp] %s\n", buf);
    fflush(stderr);
#endif
    if (FILE* f = LogFile()) {
        fprintf(f, "[vkinsp] %s\n", buf);
        fflush(f);
    }
#if defined(_WIN32)
    OutputDebugStringA("[vkinsp] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#endif
}

// ---------------------------------------------------------------------------------------------
// Dispatch registry

namespace {
std::shared_mutex g_registryMutex;
std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;
} // namespace

InstanceData* FindInstance(void* key) {
    std::shared_lock lock(g_registryMutex);
    auto it = g_instances.find(key);
    return it == g_instances.end() ? nullptr : it->second.get();
}

DeviceData* FindDevice(void* key) {
    std::shared_lock lock(g_registryMutex);
    auto it = g_devices.find(key);
    return it == g_devices.end() ? nullptr : it->second.get();
}

void RegisterInstance(void* key, std::unique_ptr<InstanceData> data) {
    std::unique_lock lock(g_registryMutex);
    g_instances[key] = std::move(data);
}

void RegisterDevice(void* key, std::unique_ptr<DeviceData> data) {
    std::unique_lock lock(g_registryMutex);
    g_devices[key] = std::move(data);
}

void UnregisterInstance(void* key) {
    std::unique_lock lock(g_registryMutex);
    g_instances.erase(key);
}

void UnregisterDevice(void* key) {
    std::unique_lock lock(g_registryMutex);
    g_devices.erase(key);
}

// ---------------------------------------------------------------------------------------------
// UI messages

static void HandleUiMessage(const std::string& text) {
    JsonValue msg;
    if (!JsonParser::Parse(text, msg)) {
        Log("bad message from UI: %s", text.c_str());
        return;
    }
    std::string action = msg.GetString("action");
    Log("ui message: %s", action.c_str());
    if (action == "Ping") {
        Transport::Get().SendJson("{\"action\":\"Pong\"}");
    } else if (action == "RequestBlob") {
        uint64_t id = (uint64_t)msg.GetNumber("id");
        uint32_t index = (uint32_t)msg.GetNumber("index");
        auto blob = Tracker::Get().GetBlob(id, index);
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("ObjectBlob");
        w.Key("id"); w.Uint(id);
        w.Key("index"); w.Uint(index);
        w.Key("size"); w.Uint(blob ? blob->size() : 0);
        w.EndObject();
        if (blob) Transport::Get().SendBinary(std::move(w.str()), blob->data(), blob->size());
        else Transport::Get().SendJson(std::move(w.str()));
    }
    else if (action == "RequestDescriptorSet") {
        // Live contents of a descriptor set for the Inspect panel, as an ObjectUpdate the UI
        // merges into the object ("bindings", same shape as a capture's descriptor snapshot).
        uint64_t id = (uint64_t)msg.GetNumber("id");
        TrackedObject obj;
        DescriptorSetContents contents;
        bool tracked = Tracker::Get().FindById(id, obj) && obj.type == HT_VkDescriptorSet &&
                       DescriptorTracker::Get().GetSet((VkDescriptorSet)(uintptr_t)obj.handle, contents);
        JsonWriter w(&Tracker::Get());
        w.BeginObject();
        w.Key("action"); w.String("ObjectUpdate");
        w.Key("id"); w.Uint(id);
        w.Key("tracked"); w.Boolean(tracked);
        w.Key("layout"); w.Handle(HT_VkDescriptorSetLayout, "VkDescriptorSetLayout", (uint64_t)(uintptr_t)contents.layout);
        w.Key("bindings");
        uint32_t dynamicIndex = 0;
        WriteDescriptorBindingsJson(w, contents, nullptr, 0, dynamicIndex, nullptr);
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
    } else if (action == "RequestImage") {
        ImageReadback::Get().Request((uint64_t)msg.GetNumber("id"), (uint32_t)msg.GetNumber("mip"),
                                     (uint32_t)msg.GetNumber("layer"));
    } else if (action == "ReplaceShader" || action == "RestoreShader") {
        // Live shader editing (see shader_edit.h): {pipeline, stage: "VK_SHADER_STAGE_...", spirv: base64}.
        uint64_t pipeline = (uint64_t)msg.GetNumber("pipeline");
        std::string stageName = msg.GetString("stage");
        VkShaderStageFlagBits stage = (VkShaderStageFlagBits)0;
        static const struct { const char* name; VkShaderStageFlagBits bit; } kStages[] = {
            {"VK_SHADER_STAGE_VERTEX_BIT", VK_SHADER_STAGE_VERTEX_BIT},
            {"VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT", VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT},
            {"VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT", VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT},
            {"VK_SHADER_STAGE_GEOMETRY_BIT", VK_SHADER_STAGE_GEOMETRY_BIT},
            {"VK_SHADER_STAGE_FRAGMENT_BIT", VK_SHADER_STAGE_FRAGMENT_BIT},
            {"VK_SHADER_STAGE_COMPUTE_BIT", VK_SHADER_STAGE_COMPUTE_BIT},
            {"VK_SHADER_STAGE_TASK_BIT_EXT", VK_SHADER_STAGE_TASK_BIT_EXT},
            {"VK_SHADER_STAGE_MESH_BIT_EXT", VK_SHADER_STAGE_MESH_BIT_EXT},
            {"VK_SHADER_STAGE_RAYGEN_BIT_KHR", VK_SHADER_STAGE_RAYGEN_BIT_KHR},
            {"VK_SHADER_STAGE_ANY_HIT_BIT_KHR", VK_SHADER_STAGE_ANY_HIT_BIT_KHR},
            {"VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
            {"VK_SHADER_STAGE_MISS_BIT_KHR", VK_SHADER_STAGE_MISS_BIT_KHR},
            {"VK_SHADER_STAGE_INTERSECTION_BIT_KHR", VK_SHADER_STAGE_INTERSECTION_BIT_KHR},
            {"VK_SHADER_STAGE_CALLABLE_BIT_KHR", VK_SHADER_STAGE_CALLABLE_BIT_KHR},
        };
        for (auto& s : kStages) if (stageName == s.name) stage = s.bit;
        if (action == "RestoreShader") {
            ShaderEditor::Get().Restore(pipeline, stage);
        } else {
            std::vector<uint8_t> bytes;
            if (!stage || !DecodeBase64(msg.GetString("spirv"), bytes) || bytes.size() % 4) {
                JsonWriter w;
                w.BeginObject();
                w.Key("action"); w.String("ShaderReplaced");
                w.Key("pipeline"); w.Uint(pipeline);
                w.Key("stage"); w.String(stageName);
                w.Key("ok"); w.Boolean(false);
                w.Key("error"); w.String(stage ? "malformed SPIR-V payload" : "unknown shader stage");
                w.EndObject();
                Transport::Get().SendJson(std::move(w.str()));
            } else {
                std::vector<uint32_t> words(bytes.size() / 4);
                memcpy(words.data(), bytes.data(), bytes.size());
                ShaderEditor::Get().Replace(pipeline, stage, std::move(words));
            }
        }
    } else if (action == "RequestSnapshot") {
        // A UI window that picked up an already-connected session rebuilds its object list.
        Tracker::Get().SendSnapshot();
        ValidationLog::Get().SendSnapshot();
    } else if (action == "Settings") {
        if (const JsonValue* v = msg.Get("recordAlways")) CaptureManager::Get().SetRecordAlways(v->b);
    } else if (action == "Capture") {
        CaptureOptions o;
        o.frameCount = (uint32_t)msg.GetNumber("frameCount", 1);
        if (const JsonValue* v = msg.Get("atFrame")) { if (v->kind == JsonValue::Number && v->num >= 0) o.atFrame = (uint64_t)v->num; }
        if (const JsonValue* v = msg.Get("maxBufferSize")) o.maxBufferSize = (uint64_t)v->num;
        if (const JsonValue* v = msg.Get("maxTextureSize")) o.maxTextureSize = (uint64_t)v->num;
        if (const JsonValue* v = msg.Get("maxBufferTotal")) o.maxBufferTotal = (uint64_t)v->num;
        if (const JsonValue* v = msg.Get("maxImageTotal")) o.maxImageTotal = (uint64_t)v->num;
        o.captureTextures = msg.GetBool("captureTextures", true);
        o.captureBuffers = msg.GetBool("captureBuffers", true);
        o.captureImages = msg.GetBool("captureImages", true);
        o.profilePasses = msg.GetBool("profilePasses", true);
        CaptureManager::Get().Request(o);
    }
}

static void EnsureStarted() {
    static bool started = false;
    if (started) return;
    started = true;
    Transport::Get().Start();
    Transport::Get().SetMessageHandler(HandleUiMessage);
    if (ConfigFlag("VKINSP_RECORD_ALWAYS")) CaptureManager::Get().SetRecordAlways(true);
}

// ---------------------------------------------------------------------------------------------
// Chain helpers

static VkLayerInstanceCreateInfo* FindInstanceLinkInfo(const VkInstanceCreateInfo* ci) {
    auto* p = static_cast<const VkBaseInStructure*>(ci->pNext);
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto* li = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if (li->function == VK_LAYER_LINK_INFO) return li;
        }
        p = p->pNext;
    }
    return nullptr;
}

static VkLayerDeviceCreateInfo* FindDeviceLinkInfo(const VkDeviceCreateInfo* ci) {
    auto* p = static_cast<const VkBaseInStructure*>(ci->pNext);
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* li = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if (li->function == VK_LAYER_LINK_INFO) return li;
        }
        p = p->pNext;
    }
    return nullptr;
}

static const VkLayerProperties kLayerProperties = {
    VKINSP_LAYER_NAME,
    VK_HEADER_VERSION_COMPLETE,
    VKINSP_LAYER_IMPL_VERSION,
    VKINSP_LAYER_DESCRIPTION,
};

// ---------------------------------------------------------------------------------------------
// Instance

VKAPI_ATTR VkResult VKAPI_CALL layer_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator,
                                                     VkInstance* pInstance) {
    EnsureStarted();

    VkLayerInstanceCreateInfo* link = FindInstanceLinkInfo(pCreateInfo);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr nextGipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto nextCreateInstance = (PFN_vkCreateInstance)nextGipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!nextCreateInstance) return VK_ERROR_INITIALIZATION_FAILED;

    // Advance the chain for the next layer.
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    // Validation messages need VK_EXT_debug_utils; enable it for the application when it did not.
    VkInstanceCreateInfo createInfo = *pCreateInfo;
    std::vector<const char*> extensionNames;
    bool debugUtils = ValidationLog::EnsureDebugUtils(nextGipa, createInfo, extensionNames);

    VkResult res = nextCreateInstance(&createInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    auto data = std::make_unique<InstanceData>();
    data->instance = *pInstance;
    data->nextGetInstanceProcAddr = nextGipa;
    if (pCreateInfo->pApplicationInfo) {
        const VkApplicationInfo& ai = *pCreateInfo->pApplicationInfo;
        data->apiVersion = ai.apiVersion ? ai.apiVersion : VK_API_VERSION_1_0;
        if (ai.pApplicationName) data->appName = ai.pApplicationName;
        if (ai.pEngineName) data->engineName = ai.pEngineName;
    }
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
        data->enabledExtensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
    InitInstanceDispatch(*pInstance, nextGipa, data->dispatch);

    Log("vkCreateInstance app='%s' engine='%s' api=%u.%u.%u", data->appName.c_str(),
        data->engineName.c_str(), VK_API_VERSION_MAJOR(data->apiVersion),
        VK_API_VERSION_MINOR(data->apiVersion), VK_API_VERSION_PATCH(data->apiVersion));

    InstanceData* inst = data.get();
    RegisterInstance(DispatchKey(*pInstance), std::move(data));

    Tracker& t = Tracker::Get();
    JsonWriter& w = t.BeginArgs();
    ArgsToJson_vkCreateInstance(w, pCreateInfo, pAllocator, pInstance);
    t.OnCreate(HT_VkInstance, (uint64_t)(uintptr_t)*pInstance, HT_Count, 0, VkCmdId::CreateInstance, 0, w.str());
    t.EndArgs();
    if (debugUtils) ValidationLog::Get().CreateMessenger(inst);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL layer_vkDestroyInstance(VkInstance instance,
                                                  const VkAllocationCallbacks* pAllocator) {
    void* key = DispatchKey(instance);
    InstanceData* data = FindInstance(key);
    if (!data) return;
    Log("vkDestroyInstance");
    ValidationLog::Get().DestroyMessenger(data);
    Tracker::Get().OnDestroy(HT_VkInstance, (uint64_t)(uintptr_t)instance);
    PFN_vkDestroyInstance next = data->dispatch.DestroyInstance;
    next(instance, pAllocator);
    UnregisterInstance(key);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                                       VkLayerProperties* pProperties) {
    if (pProperties) {
        if (*pPropertyCount < 1) return VK_INCOMPLETE;
        pProperties[0] = kLayerProperties;
    }
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL layer_vkEnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    if (pLayerName && strcmp(pLayerName, VKINSP_LAYER_NAME) == 0) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL layer_vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
    *pApiVersion = VK_HEADER_VERSION_COMPLETE;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL layer_vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                                     uint32_t* pPropertyCount,
                                                                     VkLayerProperties* pProperties) {
    return layer_vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    if (pLayerName && strcmp(pLayerName, VKINSP_LAYER_NAME) == 0) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    return GetInstanceDispatch(physicalDevice)
        ->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

// ---------------------------------------------------------------------------------------------
// Device

VKAPI_ATTR VkResult VKAPI_CALL layer_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                   const VkDeviceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkDevice* pDevice) {
    InstanceData* instance = GetInstanceData(physicalDevice);
    VkLayerDeviceCreateInfo* link = FindDeviceLinkInfo(pCreateInfo);
    if (!instance || !link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr nextGipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr nextGdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto nextCreateDevice = (PFN_vkCreateDevice)nextGipa(instance->instance, "vkCreateDevice");
    if (!nextCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    VkResult res = nextCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    auto data = std::make_unique<DeviceData>();
    data->device = *pDevice;
    data->physicalDevice = physicalDevice;
    data->instance = instance;
    data->nextGetDeviceProcAddr = nextGdpa;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
        data->enabledExtensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
    InitDeviceDispatch(*pDevice, nextGdpa, data->dispatch);
    instance->dispatch.GetPhysicalDeviceProperties(physicalDevice, &data->properties);
    instance->dispatch.GetPhysicalDeviceMemoryProperties(physicalDevice, &data->memoryProperties);

    Log("vkCreateDevice '%s' queues=%u extensions=%u", data->properties.deviceName,
        pCreateInfo->queueCreateInfoCount, pCreateInfo->enabledExtensionCount);

    DeviceData* dev = data.get();
    RegisterDevice(DispatchKey(*pDevice), std::move(data));

    // Descriptor: the create arguments plus the physical device properties, which the UI shows
    // as the device's identity.
    Tracker& t = Tracker::Get();
    JsonWriter& w = t.BeginArgs();
    w.BeginObject();
    w.Key("physicalDevice"); w.Handle(HT_VkPhysicalDevice, "VkPhysicalDevice", (uint64_t)(uintptr_t)physicalDevice);
    w.Key("pCreateInfo"); ToJson(w, *pCreateInfo);
    w.Key("properties"); ToJson(w, dev->properties);
    w.EndObject();
    t.OnCreate(HT_VkDevice, (uint64_t)(uintptr_t)*pDevice, HT_VkPhysicalDevice, (uint64_t)(uintptr_t)physicalDevice,
               VkCmdId::CreateDevice, 0, w.str());
    t.EndArgs();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL layer_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    void* key = DispatchKey(device);
    DeviceData* data = FindDevice(key);
    if (!data) return;
    Log("vkDestroyDevice frames=%llu", (unsigned long long)data->frameIndex);
    Tracker::Get().OnDestroy(HT_VkDevice, (uint64_t)(uintptr_t)device);
    PFN_vkDestroyDevice next = data->dispatch.DestroyDevice;
    next(device, pAllocator);
    UnregisterDevice(key);
}

// ---------------------------------------------------------------------------------------------
// Object names

VKAPI_ATTR VkResult VKAPI_CALL layer_vkSetDebugUtilsObjectNameEXT(VkDevice device,
                                                                 const VkDebugUtilsObjectNameInfoEXT* pNameInfo) {
    DeviceData* data = GetDeviceData(device);
    VkResult res = VK_SUCCESS;
    if (data->dispatch.SetDebugUtilsObjectNameEXT)
        res = data->dispatch.SetDebugUtilsObjectNameEXT(device, pNameInfo);
    if (pNameInfo) {
        HandleType ht = HandleTypeFromObjectType(pNameInfo->objectType);
        if (ht != HT_Count) Tracker::Get().SetLabel(ht, pNameInfo->objectHandle, pNameInfo->pObjectName);
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL layer_vkDebugMarkerSetObjectNameEXT(VkDevice device,
                                                                  const VkDebugMarkerObjectNameInfoEXT* pNameInfo) {
    DeviceData* data = GetDeviceData(device);
    VkResult res = VK_SUCCESS;
    if (data->dispatch.DebugMarkerSetObjectNameEXT)
        res = data->dispatch.DebugMarkerSetObjectNameEXT(device, pNameInfo);
    if (pNameInfo) {
        // VkDebugReportObjectTypeEXT values match VkObjectType for all core object types.
        HandleType ht = HandleTypeFromObjectType((VkObjectType)pNameInfo->objectType);
        if (ht != HT_Count) Tracker::Get().SetLabel(ht, pNameInfo->object, pNameInfo->pObjectName);
    }
    return res;
}

// ---------------------------------------------------------------------------------------------
// Frame boundary

VKAPI_ATTR VkResult VKAPI_CALL layer_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceData* data = GetDeviceData(queue);
    // Live image readbacks go on this queue before the present, while the frame's images are in
    // their tracked layouts and the swapchain image is still owned by the application.
    ImageReadback::Get().OnPresent(data, queue);
    ShaderEditor::Get().OnPresent(data);
    VkResult res = data->dispatch.QueuePresentKHR(queue, pPresentInfo);
    data->frameIndex++;
    CaptureManager::Get().OnPresent(data, queue, pPresentInfo, res);
    ValidationLog::Get().SetFrame(data->frameIndex);

    // Frame timing, reported ten times per second: average, shortest and longest frame of the
    // interval (the UI's frame time meter plots the average and the longest).
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    if (data->lastPresent.time_since_epoch().count() != 0) {
        double ms = std::chrono::duration<double, std::milli>(now - data->lastPresent).count();
        if (data->frameTimeCount == 0) {
            data->frameTimeMinMs = ms;
            data->frameTimeMaxMs = ms;
        } else {
            if (ms < data->frameTimeMinMs) data->frameTimeMinMs = ms;
            if (ms > data->frameTimeMaxMs) data->frameTimeMaxMs = ms;
        }
        data->frameTimeAccumMs += ms;
        data->frameTimeCount++;
        double sinceReport = std::chrono::duration<double, std::milli>(now - data->lastReport).count();
        if (sinceReport >= 100.0 && Transport::Get().Connected()) {
            JsonWriter w;
            w.BeginObject();
            w.Key("action"); w.String("FrameStats");
            w.Key("frame"); w.Uint(data->frameIndex);
            w.Key("frameTimeMs"); w.Double(data->frameTimeAccumMs / data->frameTimeCount);
            w.Key("minMs"); w.Double(data->frameTimeMinMs);
            w.Key("maxMs"); w.Double(data->frameTimeMaxMs);
            w.Key("frames"); w.Uint(data->frameTimeCount);
            uint64_t submitNanos = data->submitNanos.exchange(0, std::memory_order_relaxed);
            w.Key("submitMs"); w.Double((double)submitNanos / 1e6 / data->frameTimeCount);
            w.EndObject();
            Transport::Get().SendJson(std::move(w.str()));
            data->frameTimeAccumMs = 0;
            data->frameTimeCount = 0;
            data->lastReport = now;
            ValidationLog::Get().Flush();
        }
    } else {
        data->lastReport = now;
    }
    data->lastPresent = now;
    return res;
}

// ---------------------------------------------------------------------------------------------
// GetProcAddr

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    DeviceData* data = GetDeviceData(device);
    if (!data) return nullptr;
    int level = 0;
    PFN_vkVoidFunction ours = LookupEntryPoint(pName, &level);
    PFN_vkVoidFunction next = data->nextGetDeviceProcAddr(device, pName);
    if (ours && level == 2) {
        // Only claim device functions the rest of the chain actually implements, so that
        // unsupported extensions still report as unsupported to the application.
        return next ? ours : nullptr;
    }
    return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    int level = 0;
    PFN_vkVoidFunction ours = LookupEntryPoint(pName, &level);

    if (ours && level == 0) return ours;  // global functions
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)layer_vkGetInstanceProcAddr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)layer_vkGetDeviceProcAddr;
    if (instance == VK_NULL_HANDLE) return nullptr;

    InstanceData* data = GetInstanceData(instance);
    if (!data) return nullptr;
    PFN_vkVoidFunction next = data->nextGetInstanceProcAddr(instance, pName);
    if (ours) return next ? ours : nullptr;
    return next;
}

} // namespace vkinsp

// ---------------------------------------------------------------------------------------------
// Exports

VKINSP_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = vkinsp::layer_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkinsp::layer_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

VKINSP_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return vkinsp::layer_vkGetInstanceProcAddr(instance, pName);
}

VKINSP_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return vkinsp::layer_vkGetDeviceProcAddr(device, pName);
}

#if defined(__ANDROID__)
// Android's loader (frameworks/native/vulkan/libvulkan/layers_extensions.cpp) has no manifests
// and does not call the negotiate function: it dlopens every libVkLayer*.so in the layer search
// directories and resolves the enumeration entry points by name, as RenderDoc's Android layer
// exports them too.
VKINSP_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return vkinsp::layer_vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VKINSP_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    return vkinsp::layer_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}

VKINSP_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return vkinsp::layer_vkEnumerateDeviceLayerProperties(physicalDevice, pPropertyCount, pProperties);
}

VKINSP_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    return vkinsp::layer_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}
#endif
