#include "replayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <unordered_set>

#include "exporter.h"
#include "format_info.h"
#include "hw_counters.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vkreplay {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    // The report of whatever is running: Setup's, then each frame's.
    ReplayReport* report = *static_cast<ReplayReport**>(user);
    if (report && (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))) {
        const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        report->validation.push_back(std::string(error ? "error: " : "warning: ") + (data && data->pMessage ? data->pMessage : ""));
    }
    return VK_FALSE;
}

} // namespace

Replayer::Replayer() {
    _ctx.resolve = [this](uint64_t id, std::string_view) { return Handle(id); };
    // Left out on purpose, or a shader module the file does not hold: pipelines take their code from their own SPIR-V payloads.
    _ctx.quiet = [this](uint64_t id, std::string_view className) {
        return _skipped.count(id) != 0 || (className == "VkShaderModule" && !_capture->Object(id));
    };
    _ctx.fns = &_fns;
    // Export to C++ decodes each command a second time, with the same handles but problems of its own:
    // what the replay reports it has reported already.
    _exportCtx.resolve = _ctx.resolve;
    _exportCtx.quiet = _ctx.quiet;
    _exportCtx.fns = &_fns;
}

Replayer::~Replayer() {
    DestroyAll();
    if (_library) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(_library));
#else
        dlclose(_library);
#endif
    }
}

void Replayer::Problem(std::string message) { _report->problems.push_back(std::move(message)); }

uint64_t Replayer::Handle(uint64_t id) const {
    auto it = _handles.find(id);
    return it == _handles.end() ? 0 : it->second;
}

void Replayer::Track(const std::string& type, uint64_t handle) {
    if (handle) _created.push_back({type, handle});
}

// ---------------------------------------------------------------------------------------------
// Instance and device

bool Replayer::LoadVulkan() {
#ifdef _WIN32
    _library = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa = _library ? (PFN_vkGetInstanceProcAddr)GetProcAddress(static_cast<HMODULE>(_library), "vkGetInstanceProcAddr") : nullptr;
#else
    _library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!_library) _library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr gipa = _library ? (PFN_vkGetInstanceProcAddr)dlsym(_library, "vkGetInstanceProcAddr") : nullptr;
#endif
    if (!gipa) {
        Problem("the Vulkan loader was not found");
        return false;
    }
    LoadGlobalFunctions(_fns, gipa);
    _fns.GetInstanceProcAddr = gipa;
    return _fns.CreateInstance != nullptr;
}

bool Replayer::CreateInstance() {
    uint32_t count = 0;
    _fns.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    _fns.EnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    auto hasExtension = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
    };
    std::vector<const char*> extensions;
    if (hasExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    // VK_KHR_swapchain on the device (for PRESENT_SRC_KHR layouts) requires the surface extension.
    if (hasExtension(VK_KHR_SURFACE_EXTENSION_NAME)) extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    // Hardware counters through NVIDIA's Nsight Perf SDK need extensions on the instance; ask for them
    // when the SDK is present and the analysis (or a served replay) may want them (hw_counters.cpp).
    if (_options.counters.enabled || _options.allFeatures) {
        if (nvperf::Load(_nvperfNote)) {
            uint32_t version = VK_API_VERSION_1_0;
            if (_fns.EnumerateInstanceVersion) _fns.EnumerateInstanceVersion(&version);
            std::vector<const char*> perf;
            nvperf::InstanceExtensions(version, perf);
            for (const char* name : perf)
                if (hasExtension(name) && std::none_of(extensions.begin(), extensions.end(), [&](const char* e) { return !std::strcmp(e, name); }))
                    extensions.push_back(name);
        }
    }

    std::vector<const char*> layers;
    if (_options.validation) {
        _fns.EnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> props(count);
        _fns.EnumerateInstanceLayerProperties(&count, props.data());
        const bool found = std::any_of(props.begin(), props.end(), [](const VkLayerProperties& l) { return !std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation"); });
        if (found) layers.push_back("VK_LAYER_KHRONOS_validation");
        else Problem("the validation layer is not installed (Vulkan SDK)");
    }

    uint32_t version = VK_API_VERSION_1_0;
    if (_fns.EnumerateInstanceVersion) _fns.EnumerateInstanceVersion(&version);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkinsp_replay";
    app.apiVersion = std::min(version, (uint32_t)VK_API_VERSION_1_3);
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = (uint32_t)extensions.size();
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = (uint32_t)layers.size();
    info.ppEnabledLayerNames = layers.data();
    VkResult r = _fns.CreateInstance(&info, nullptr, &_instance);
    if (r != VK_SUCCESS) {
        Problem("vkCreateInstance failed (" + std::to_string(r) + ")");
        return false;
    }
    LoadInstanceFunctions(_fns, _instance, _fns.GetInstanceProcAddr);
    if (_exporter) {
        // Debug utils and the surface extension are the replay's own needs; a profiler's extensions are not the frame's.
        std::vector<const char*> exported;
        for (const char* e : extensions)
            if (!std::strcmp(e, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) || !std::strcmp(e, VK_KHR_SURFACE_EXTENSION_NAME)) exported.push_back(e);
        _exporter->Instance(app.apiVersion, exported);
        _exporter->Name("VkInstance", (uint64_t)(uintptr_t)_instance, "instance");
    }
    if (!extensions.empty() && _fns.CreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        m.pfnUserCallback = DebugCallback;
        m.pUserData = &_report;
        _fns.CreateDebugUtilsMessengerEXT(_instance, &m, nullptr, &_messenger);
    }
    return true;
}

bool Replayer::CreateDevice() {
    // The captured device: its GPU's name, and the create info the application passed.
    const JValue* deviceObject = nullptr;
    if (const JValue* objects = _capture->Objects(); objects && objects->IsArray()) {
        for (uint32_t i = 0; i < objects->count && !deviceObject; ++i)
            if (Str(objects->items[i].Get("type")) == "VkDevice") deviceObject = &objects->items[i];
    }
    const JValue* deviceArgs = deviceObject ? deviceObject->Get("args") : nullptr;
    std::string capturedName;
    if (deviceArgs) {
        if (const JValue* props = deviceArgs->Get("properties")) capturedName = Str(props->Get("deviceName"));
    }

    uint32_t count = 0;
    _fns.EnumeratePhysicalDevices(_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    _fns.EnumeratePhysicalDevices(_instance, &count, physicals.data());
    if (physicals.empty()) {
        Problem("no Vulkan device");
        return false;
    }
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties chosenProps{};
    for (VkPhysicalDevice p : physicals) {
        VkPhysicalDeviceProperties props{};
        _fns.GetPhysicalDeviceProperties(p, &props);
        if (!capturedName.empty() && capturedName == props.deviceName) { chosen = p; chosenProps = props; break; }
        if (!chosen || (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && chosenProps.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
            chosen = p;
            chosenProps = props;
        }
    }
    _physical = chosen;
    _report->device = chosenProps.deviceName;
    if (!capturedName.empty() && capturedName != chosenProps.deviceName)
        Problem("captured on " + capturedName + ", replayed on " + chosenProps.deviceName);
    _fns.GetPhysicalDeviceMemoryProperties(_physical, &_memoryProperties);

    _fns.GetPhysicalDeviceQueueFamilyProperties(_physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    _fns.GetPhysicalDeviceQueueFamilyProperties(_physical, &count, families.data());
    _queueFamily = 0;
    for (uint32_t i = 0; i < families.size(); ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { _queueFamily = i; break; }
    }

    _fns.EnumerateDeviceExtensionProperties(_physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    _fns.EnumerateDeviceExtensionProperties(_physical, nullptr, &count, available.data());
    auto hasExtension = [&](std::string_view name) {
        return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return name == e.extensionName; });
    };

    Args_vkCreateDevice captured{};
    _ctx.where = "VkDevice";
    if (deviceArgs) {
        _handles[IdOf(deviceArgs->Get("physicalDevice"))] = (uint64_t)(uintptr_t)_physical;
        DecodeArgs(_ctx, *deviceArgs, captured);
    }

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<std::vector<float>> priorities;
    bool queuesValid = captured.pCreateInfo && captured.pCreateInfo->queueCreateInfoCount > 0;
    if (queuesValid) {
        for (uint32_t i = 0; i < captured.pCreateInfo->queueCreateInfoCount; ++i) {
            const VkDeviceQueueCreateInfo& q = captured.pCreateInfo->pQueueCreateInfos[i];
            if (q.queueFamilyIndex >= families.size() || q.queueCount > families[q.queueFamilyIndex].queueCount) queuesValid = false;
        }
    }
    if (queuesValid) {
        queues.assign(captured.pCreateInfo->pQueueCreateInfos, captured.pCreateInfo->pQueueCreateInfos + captured.pCreateInfo->queueCreateInfoCount);
        for (auto& q : queues) q.pNext = nullptr;
    } else {
        priorities.push_back({1.0f});
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = _queueFamily;
        q.queueCount = 1;
        q.pQueuePriorities = priorities.back().data();
        queues.push_back(q);
    }
    info.queueCreateInfoCount = (uint32_t)queues.size();
    info.pQueueCreateInfos = queues.data();

    std::vector<const char*> extensions;
    if (captured.pCreateInfo) {
        for (uint32_t i = 0; i < captured.pCreateInfo->enabledExtensionCount; ++i) {
            const char* name = captured.pCreateInfo->ppEnabledExtensionNames[i];
            if (name && hasExtension(name)) extensions.push_back(name);
            else if (name) Problem(std::string("device extension not available here: ") + name);
        }
    }
    // The mesh output view captures vertex shader outputs with transform feedback.
    // A replay serving many analyses asks for every feature one may use.
    const bool wantDrawStats = _options.drawStats || _options.allFeatures;
    const bool wantWireframe = (_options.overlay.enabled && _options.overlay.wireframe) || _options.allFeatures;
    const bool xfbExtension = (_options.mesh.enabled || _options.allFeatures) && hasExtension(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    if (xfbExtension && std::none_of(extensions.begin(), extensions.end(), [](const char* e) { return !std::strcmp(e, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME); }))
        extensions.push_back(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    // Render passes that end in PRESENT_SRC_KHR need the swapchain extension, with or without a surface.
    _hasSwapchainExtension = hasExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (_hasSwapchainExtension && std::none_of(extensions.begin(), extensions.end(), [](const char* e) { return !std::strcmp(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME); }))
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    info.enabledExtensionCount = (uint32_t)extensions.size();
    info.ppEnabledExtensionNames = extensions.data();
    if (captured.pCreateInfo) {
        info.pEnabledFeatures = captured.pCreateInfo->pEnabledFeatures;
        info.pNext = captured.pCreateInfo->pNext;
    }
    // Per-draw counters are pipeline statistics queries and overlay wireframes need line polygons:
    // features the application may not have enabled. They go into whichever form the capture used: a
    // chained VkPhysicalDeviceFeatures2 (which must stay the only one), else our own copy of pEnabledFeatures.
    VkPhysicalDeviceFeatures features{};
    // The pixel history's primitive-id pass needs one too (geometryShader, below).
    const bool wantPrimitiveId = _options.history.enabled || _options.allFeatures;
    if (wantDrawStats || wantWireframe || wantPrimitiveId) {
        VkPhysicalDeviceFeatures supported{};
        _fns.GetPhysicalDeviceFeatures(_physical, &supported);
        VkPhysicalDeviceFeatures2* features2 = nullptr;
        for (auto* s = (VkBaseOutStructure*)const_cast<void*>(info.pNext); s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) features2 = (VkPhysicalDeviceFeatures2*)s;
        VkPhysicalDeviceFeatures* ours = nullptr;
        if (features2) {
            ours = &features2->features;
        } else {
            if (info.pEnabledFeatures) features = *info.pEnabledFeatures;
            ours = &features;
            info.pEnabledFeatures = &features;
        }
        if (wantDrawStats && supported.pipelineStatisticsQuery) {
            ours->pipelineStatisticsQuery = VK_TRUE;
            _drawCountersAvailable = true;
        } else if (wantDrawStats) {
            _report->drawStatsNote = "this GPU has no pipeline statistics queries, so the draws carry timings only";
        }
        if (wantWireframe && supported.fillModeNonSolid) {
            ours->fillModeNonSolid = VK_TRUE;
            _wireframeAvailable = true;
        }
        // Samples passing each draw's depth and stencil tests: the layer cannot count them for a
        // pass that executes secondary command buffers, but here the query sits inside the
        // secondary, around one draw.
        if (wantDrawStats && supported.occlusionQueryPrecise) {
            ours->occlusionQueryPrecise = VK_TRUE;
            _drawSamplesAvailable = true;
        }
        // Which primitive of a draw won a pixel (history.cpp): gl_PrimitiveID in a fragment shader
        // is SPIR-V's Geometry capability, which needs this feature even with no geometry stage.
        if (wantPrimitiveId && supported.geometryShader) {
            ours->geometryShader = VK_TRUE;
            _primitiveIdAvailable = true;
        }
    }

    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfbFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    if (xfbExtension && _fns.GetPhysicalDeviceFeatures2) {
        VkPhysicalDeviceTransformFeedbackFeaturesEXT supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &supported;
        _fns.GetPhysicalDeviceFeatures2(_physical, &query);
        if (supported.transformFeedback) {
            // Into the capture's own features struct when its chain has one: a chain may hold each only once.
            VkPhysicalDeviceTransformFeedbackFeaturesEXT* existing = nullptr;
            for (auto* s = (VkBaseOutStructure*)const_cast<void*>(info.pNext); s; s = s->pNext)
                if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT) existing = (VkPhysicalDeviceTransformFeedbackFeaturesEXT*)s;
            if (existing) {
                existing->transformFeedback = VK_TRUE;
            } else {
                xfbFeatures.transformFeedback = VK_TRUE;
                xfbFeatures.pNext = const_cast<void*>(info.pNext);
                info.pNext = &xfbFeatures;
            }
            _xfbAvailable = true;
        }
    }

    // Hardware counters: NVIDIA's Nsight Perf SDK needs its device extensions; the portable path
    // needs VK_KHR_performance_query with its feature (hw_counters.cpp). Either only when asked.
    VkPhysicalDevicePerformanceQueryFeaturesKHR perfFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR};
    const bool wantCounters = _options.counters.enabled || _options.allFeatures;
    if (wantCounters) {
        if (nvperf::Load(_nvperfNote)) {
            std::vector<const char*> perf;
            nvperf::DeviceExtensions(_instance, _physical, _fns.GetInstanceProcAddr, perf);
            bool all = true;
            for (const char* name : perf) {
                if (!hasExtension(name)) { all = false; continue; }
                if (std::none_of(extensions.begin(), extensions.end(), [&](const char* e) { return !std::strcmp(e, name); })) extensions.push_back(name);
            }
            _nvperfReady = all;   // an NVIDIA device with every extension the SDK asked for
            if (!perf.empty() && !all) _nvperfNote = "this device is missing an extension the Nsight Perf SDK needs";
        }
        // The portable path, when the SDK is not the one (a non-NVIDIA GPU, or it did not load).
        if (!_nvperfReady && hasExtension(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME) && _fns.GetPhysicalDeviceFeatures2) {
            VkPhysicalDevicePerformanceQueryFeaturesKHR supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query.pNext = &supported;
            _fns.GetPhysicalDeviceFeatures2(_physical, &query);
            if (supported.performanceCounterQueryPools) {
                extensions.push_back(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);
                VkPhysicalDevicePerformanceQueryFeaturesKHR* existing = nullptr;
                for (auto* s = (VkBaseOutStructure*)const_cast<void*>(info.pNext); s; s = s->pNext)
                    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR) existing = (VkPhysicalDevicePerformanceQueryFeaturesKHR*)s;
                if (existing) {
                    existing->performanceCounterQueryPools = VK_TRUE;
                } else {
                    perfFeatures.performanceCounterQueryPools = VK_TRUE;
                    perfFeatures.pNext = const_cast<void*>(info.pNext);
                    info.pNext = &perfFeatures;
                }
                _perfQueryAvailable = true;
            }
        }
    }
    info.enabledExtensionCount = (uint32_t)extensions.size();
    info.ppEnabledExtensionNames = extensions.data();

    VkResult r = _fns.CreateDevice(_physical, &info, nullptr, &_device);
    if (r != VK_SUCCESS && (info.pNext || info.pEnabledFeatures)) {
        Problem("vkCreateDevice with the captured features failed (" + std::to_string(r) + "); retrying without them");
        info.pNext = nullptr;
        info.pEnabledFeatures = nullptr;
        // Nothing that needed a feature can be used now.
        _drawCountersAvailable = _drawSamplesAvailable = _wireframeAvailable = _xfbAvailable = false;
        _nvperfReady = _perfQueryAvailable = false;
        r = _fns.CreateDevice(_physical, &info, nullptr, &_device);
    }
    if (r != VK_SUCCESS) {
        _arena.Reset();
        Problem("vkCreateDevice failed (" + std::to_string(r) + ")");
        return false;
    }
    auto gdpa = (PFN_vkGetDeviceProcAddr)_fns.GetInstanceProcAddr(_instance, "vkGetDeviceProcAddr");
    LoadDeviceFunctions(_fns, _device, gdpa);
    _queueFamily = queues[0].queueFamilyIndex;
    _fns.GetDeviceQueue(_device, _queueFamily, 0, &_queue);
    if (_exporter) {
        // The create info the driver accepted: the capture's, less what this GPU lacks.
        _exporter->Device(capturedName, chosenProps.deviceName, info, _queueFamily);
        _exporter->Name("VkPhysicalDevice", (uint64_t)(uintptr_t)_physical, "physicalDevice");
        _exporter->Name("VkDevice", (uint64_t)(uintptr_t)_device, "device");
        _exporter->Name("VkQueue", (uint64_t)(uintptr_t)_queue, "queue");
    }

    // The captured create info was decoded into the arena, and the export above was the last to read it.
    _arena.Reset();

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = _queueFamily;
    _fns.CreateCommandPool(_device, &pool, nullptr, &_utilityPool);

    if (deviceObject) {
        _handles[IdOf(deviceArgs ? deviceArgs->Get("physicalDevice") : nullptr)] = (uint64_t)(uintptr_t)_physical;
        if (const JValue* id = deviceObject->Get("id")) _handles[id->Uint()] = (uint64_t)(uintptr_t)_device;
    }
    // An application with several devices (a second one for compute, a runtime's own) has each
    // replayed on this one: objects of different devices never refer to each other, so their
    // commands run side by side here, with the first device's features and queues.
    if (const JValue* objects = _capture->Objects(); objects && objects->IsArray()) {
        for (uint32_t i = 0; i < objects->count; ++i) {
            const JValue& o = objects->items[i];
            if (Str(o.Get("type")) != "VkDevice" || &o == deviceObject) continue;
            if (const JValue* id = o.Get("id")) _handles[id->Uint()] = (uint64_t)(uintptr_t)_device;
            if (const JValue* args = o.Get("args")) _handles[IdOf(args->Get("physicalDevice"))] = (uint64_t)(uintptr_t)_physical;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Memory and one-time submissions

bool Replayer::AllocateBound(VkMemoryRequirements requirements, VkMemoryPropertyFlags want, VkDeviceMemory& memory, bool track,
                             bool deviceAddress) {
    // A buffer with SHADER_DEVICE_ADDRESS usage needs memory that has an address.
    VkMemoryAllocateFlagsInfo addressFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    addressFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < _memoryProperties.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i))) continue;
            VkMemoryPropertyFlags flags = _memoryProperties.memoryTypes[i].propertyFlags;
            if (pass == 0 && (flags & want) != want) continue;
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            if (deviceAddress) info.pNext = &addressFlags;
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = i;
            if (_fns.AllocateMemory(_device, &info, nullptr, &memory) == VK_SUCCESS) {
                if (track) _memories.push_back(memory);
                return true;
            }
        }
    }
    return false;
}

bool Replayer::CreateStaging(VkDeviceSize size, Staging& staging, VkBufferUsageFlags usage) {
    staging = Staging{};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(size, 1);
    info.usage = usage;
    if (_fns.CreateBuffer(_device, &info, nullptr, &staging.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    _fns.GetBufferMemoryRequirements(_device, staging.buffer, &req);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool ok = false;
    // Staging memory is read on the CPU, so cached system memory first. The other host-visible types
    // are for writing: uncached memory (write-combined on NVIDIA) and device-local memory mapped across
    // PCIe (a resizable BAR) read back slower by orders of magnitude, which made comparing a frame's
    // render targets take seconds.
    for (int pass = 0; pass < 3 && !ok; ++pass) {
        for (uint32_t i = 0; i < _memoryProperties.memoryTypeCount && !ok; ++i) {
            const VkMemoryPropertyFlags flags = _memoryProperties.memoryTypes[i].propertyFlags;
            if (!(req.memoryTypeBits & (1u << i)) || (flags & want) != want) continue;
            if (pass == 0 && (!(flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) || (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) continue;
            if (pass == 1 && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = i;
            ok = _fns.AllocateMemory(_device, &alloc, nullptr, &memory) == VK_SUCCESS;
        }
    }
    if (!ok) {
        _fns.DestroyBuffer(_device, staging.buffer, nullptr);
        staging.buffer = VK_NULL_HANDLE;
        return false;
    }
    _fns.BindBufferMemory(_device, staging.buffer, memory, 0);
    _fns.MapMemory(_device, memory, 0, VK_WHOLE_SIZE, 0, &staging.mapped);
    staging.memory = memory;
    staging.size = size;
    return true;
}

void Replayer::DestroyStaging(Staging& staging) {
    if (staging.memory) {
        _fns.UnmapMemory(_device, staging.memory);
        _fns.FreeMemory(_device, staging.memory, nullptr);
    }
    if (staging.buffer) _fns.DestroyBuffer(_device, staging.buffer, nullptr);
    staging = Staging{};
}

bool Replayer::RunOneTime(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = _utilityPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (_fns.AllocateCommandBuffers(_device, &alloc, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    _fns.BeginCommandBuffer(cb, &begin);
    record(cb);
    _fns.EndCommandBuffer(cb);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VkResult r = _fns.QueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) _fns.QueueWaitIdle(_queue);
    _fns.FreeCommandBuffers(_device, _utilityPool, 1, &cb);
    return r == VK_SUCCESS;
}

void Replayer::UploadToBuffer(VkBuffer buffer, VkDeviceSize offset, const uint8_t* data, size_t size) {
    Staging staging;
    if (!CreateStaging(size, staging)) {
        Problem("could not allocate staging memory for an upload");
        return;
    }
    std::memcpy(staging.mapped, data, size);
    RunOneTime([&](VkCommandBuffer cb) {
        VkBufferCopy copy{0, offset, size};
        _fns.CmdCopyBuffer(cb, staging.buffer, buffer, 1, &copy);
    });
    DestroyStaging(staging);
}

void Replayer::TransitionSubresources(VkCommandBuffer cb, ImageRecord& image, const std::vector<VkImageLayout>& targets) {
    // One barrier per run of layers in a mip that share their current and target layouts.
    std::vector<VkImageMemoryBarrier> barriers;
    for (uint32_t m = 0; m < image.mips; ++m) {
        for (uint32_t l = 0; l < image.layers;) {
            const size_t i = (size_t)m * image.layers + l;
            const VkImageLayout from = image.layouts[i];
            const VkImageLayout to = i < targets.size() ? targets[i] : VK_IMAGE_LAYOUT_UNDEFINED;
            uint32_t end = l + 1;
            while (end < image.layers && image.layouts[i + end - l] == from &&
                   (i + end - l < targets.size() ? targets[i + end - l] : VK_IMAGE_LAYOUT_UNDEFINED) == to)
                ++end;
            if (to != VK_IMAGE_LAYOUT_UNDEFINED && to != from) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image.image;
                b.subresourceRange = {vkinsp::FormatAspects(image.format), m, 1, l, end - l};
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                barriers.push_back(b);
                for (uint32_t k = l; k < end; ++k) image.layouts[(size_t)m * image.layers + k] = to;
            }
            l = end;
        }
    }
    if (!barriers.empty())
        _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                (uint32_t)barriers.size(), barriers.data());
}

void Replayer::TransitionAll(VkCommandBuffer cb, ImageRecord& image, VkImageLayout to) {
    TransitionSubresources(cb, image, std::vector<VkImageLayout>(image.layouts.size(), to));
}

// ---------------------------------------------------------------------------------------------
// Objects

void Replayer::CreateObjects() {
    const JValue* objects = _capture->Objects();
    if (!objects || !objects->IsArray()) return;
    std::vector<const JValue*> ordered;
    for (uint32_t i = 0; i < objects->count; ++i) ordered.push_back(&objects->items[i]);
    std::sort(ordered.begin(), ordered.end(), [](const JValue* a, const JValue* b) { return a->Get("id")->Uint() < b->Get("id")->Uint(); });
    for (const JValue* o : ordered) CreateObject(*o);
}

uint64_t Replayer::CreateImage(uint64_t id, const VkImageCreateInfo& captured) {
    VkImageCreateInfo info = captured;
    // Every image is read and written by the replay; transient attachments allow attachment usages only.
    info.usage = (info.usage & ~VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The replay allocates plain memory of its own: external memory and DRM modifiers belong to the capturing machine.
    info.pNext = StripPNext(info.pNext, {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_NV,
                                         VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
                                         VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT});
    if (info.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) info.tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImage image = VK_NULL_HANDLE;
    VkResult r = _fns.CreateImage(_device, &info, nullptr, &image);
    if (r != VK_SUCCESS) {
        Problem("image " + std::to_string(id) + ": vkCreateImage failed (" + std::to_string(r) + ")");
        return 0;
    }
    VkMemoryRequirements req{};
    _fns.GetImageMemoryRequirements(_device, image, &req);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory) || _fns.BindImageMemory(_device, image, memory, 0) != VK_SUCCESS) {
        Problem("image " + std::to_string(id) + ": no memory");
        _fns.DestroyImage(_device, image, nullptr);
        return 0;
    }
    ImageRecord rec;
    rec.image = image;
    rec.format = info.format;
    rec.extent = info.extent;
    rec.mips = info.mipLevels;
    rec.layers = info.arrayLayers;
    rec.samples = info.samples;
    rec.storage = (info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    rec.layouts.assign((size_t)rec.mips * rec.layers, VK_IMAGE_LAYOUT_UNDEFINED);
    _images[id] = rec;
    if (_exporter) _exporter->CreateImage(id, image, info, _exportComment);
    return (uint64_t)image;
}

uint64_t Replayer::CreateBuffer(uint64_t id, const VkBufferCreateInfo& captured) {
    VkBufferCreateInfo info = captured;
    info.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.pNext = StripPNext(info.pNext, {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO});
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult r = _fns.CreateBuffer(_device, &info, nullptr, &buffer);
    if (r != VK_SUCCESS) {
        Problem("buffer " + std::to_string(id) + ": vkCreateBuffer failed (" + std::to_string(r) + ")");
        return 0;
    }
    VkMemoryRequirements req{};
    _fns.GetBufferMemoryRequirements(_device, buffer, &req);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const bool deviceAddress = (info.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    if (!AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory, true, deviceAddress) || _fns.BindBufferMemory(_device, buffer, memory, 0) != VK_SUCCESS) {
        Problem("buffer " + std::to_string(id) + ": no memory");
        _fns.DestroyBuffer(_device, buffer, nullptr);
        return 0;
    }
    _buffers[id] = {buffer, info.size};
    if (_exporter) _exporter->CreateBuffer(id, buffer, info);
    return (uint64_t)buffer;
}

VkShaderModule Replayer::ModuleFromBlob(const JValue& object, const std::string& blobName, const uint8_t** code, size_t* codeSize) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!_capture->Blob(object, blobName, data, size) || size < 4) return VK_NULL_HANDLE;
    if (code) *code = data;
    if (codeSize) *codeSize = size;
    auto* words = _arena.Make<uint32_t>(size / 4);
    std::memcpy(words, data, size / 4 * 4);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size / 4 * 4;
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (_fns.CreateShaderModule(_device, &info, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

uint64_t Replayer::CreatePipeline(const JValue& object, std::string_view cmd, uint32_t index, const JValue& args, size_t unresolvedBefore) {
    std::vector<VkShaderModule> temporary;
    std::vector<Exporter::StageModule> exportedModules;
    auto stageModule = [&](VkPipelineShaderStageCreateInfo& stage) {
        std::string name = std::string(StageName(stage.stage)) + ":" + (stage.pName ? stage.pName : "main");
        const uint8_t* code = nullptr;
        size_t codeSize = 0;
        VkShaderModule module = ModuleFromBlob(object, name, &code, &codeSize);
        if (module) {
            temporary.push_back(module);
            exportedModules.push_back({module, StageName(stage.stage), code, codeSize});
            stage.module = module;
            // Inline code (VkShaderModuleCreateInfo in pNext) is replaced by the payload's module.
            stage.pNext = nullptr;
        } else if (!stage.module) {
            Problem("pipeline " + std::to_string(object.Get("id")->Uint()) + ": no code for its " + name + " stage");
        }
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_UNKNOWN;
    if (cmd == "vkCreateGraphicsPipelines") {
        Args_vkCreateGraphicsPipelines a{};
        DecodeArgs(_ctx, args, a);
        if (!a.pCreateInfos || index >= a.createInfoCount || _ctx.unresolved != unresolvedBefore) return 0;
        VkGraphicsPipelineCreateInfo info = a.pCreateInfos[index];
        std::vector<VkPipelineShaderStageCreateInfo> stages(info.pStages, info.pStages + info.stageCount);
        for (auto& s : stages) stageModule(s);
        info.pStages = stages.data();
        info.flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        info.basePipelineHandle = VK_NULL_HANDLE;
        info.basePipelineIndex = -1;
        r = _fns.CreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (_exporter && pipeline) _exporter->CreatePipeline(object.Get("id")->Uint(), pipeline, std::string(cmd), &info, nullptr, nullptr, exportedModules);
    } else if (cmd == "vkCreateComputePipelines") {
        Args_vkCreateComputePipelines a{};
        DecodeArgs(_ctx, args, a);
        if (!a.pCreateInfos || index >= a.createInfoCount || _ctx.unresolved != unresolvedBefore) return 0;
        VkComputePipelineCreateInfo info = a.pCreateInfos[index];
        stageModule(info.stage);
        info.flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        info.basePipelineHandle = VK_NULL_HANDLE;
        info.basePipelineIndex = -1;
        r = _fns.CreateComputePipelines(_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (_exporter && pipeline) _exporter->CreatePipeline(object.Get("id")->Uint(), pipeline, std::string(cmd), nullptr, &info, nullptr, exportedModules);
    } else if (cmd == "vkCreateRayTracingPipelinesKHR") {
        Args_vkCreateRayTracingPipelinesKHR a{};
        DecodeArgs(_ctx, args, a);
        if (!a.pCreateInfos || index >= a.createInfoCount || _ctx.unresolved != unresolvedBefore) return 0;
        VkRayTracingPipelineCreateInfoKHR info = a.pCreateInfos[index];
        std::vector<VkPipelineShaderStageCreateInfo> stages(info.pStages, info.pStages + info.stageCount);
        for (auto& st : stages) stageModule(st);
        info.pStages = stages.data();
        info.flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        info.basePipelineHandle = VK_NULL_HANDLE;
        info.basePipelineIndex = -1;
        // A library the capture built from is not replayed, and a pipeline that only provides one
        // is of no use here either: both would need the libraries recreated first.
        info.pLibraryInfo = nullptr;
        info.pLibraryInterface = nullptr;
        if (!_fns.CreateRayTracingPipelinesKHR) {
            Problem("pipeline " + std::to_string(object.Get("id")->Uint())
                    + ": this device has no ray tracing pipelines");
        } else {
            r = _fns.CreateRayTracingPipelinesKHR(_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
            if (_exporter && pipeline) _exporter->CreatePipeline(object.Get("id")->Uint(), pipeline, std::string(cmd), nullptr, nullptr, &info, exportedModules);
        }
    } else {
        Problem("pipeline " + std::to_string(object.Get("id")->Uint()) + ": " + std::string(cmd) + " is not replayed yet");
    }
    for (VkShaderModule m : temporary) _fns.DestroyShaderModule(_device, m, nullptr);
    if (r != VK_SUCCESS && pipeline == VK_NULL_HANDLE) {
        if (cmd == "vkCreateGraphicsPipelines" || cmd == "vkCreateComputePipelines" || cmd == "vkCreateRayTracingPipelinesKHR")
            Problem("pipeline " + std::to_string(object.Get("id")->Uint()) + ": " + std::string(cmd) + " failed (" + std::to_string(r) + ")");
        return 0;
    }
    return (uint64_t)pipeline;
}

uint64_t Replayer::CreateShaderObject(const JValue& object, uint32_t index, const JValue& args, size_t unresolvedBefore) {
    const std::string id = std::to_string(object.Get("id")->Uint());
    if (!_fns.CreateShadersEXT) {
        Problem("shader " + id + ": vkCreateShadersEXT is not available on this device");
        return 0;
    }
    Args_vkCreateShadersEXT a{};
    DecodeArgs(_ctx, args, a);
    if (!a.pCreateInfos || index >= a.createInfoCount || _ctx.unresolved != unresolvedBefore) return 0;
    VkShaderCreateInfoEXT info = a.pCreateInfos[index];
    // Made one at a time, so a shader created linked with others is made unlinked.
    info.flags &= ~(VkShaderCreateFlagsEXT)VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
    // The code from the shader's payload, which the arguments may only summarize.
    const std::string name = std::string(StageName(info.stage)) + ":" + (info.pName ? info.pName : "main");
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (info.codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT && _capture->Blob(object, name, data, size) && size >= 20) {
        auto* code = _arena.Make<uint8_t>(size);
        std::memcpy(code, data, size);
        info.pCode = code;
        info.codeSize = size;
    } else if (info.codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT) {
        Problem("shader " + id + ": no code for its " + name + " stage");
        return 0;
    }
    VkShaderEXT shader = VK_NULL_HANDLE;
    const VkResult r = _fns.CreateShadersEXT(_device, 1, &info, nullptr, &shader);
    if (r != VK_SUCCESS || !shader) {
        Problem("shader " + id + ": vkCreateShadersEXT failed (" + std::to_string(r) + ")");
        return 0;
    }
    if (_exporter) _exporter->CreateShaderObject(object.Get("id")->Uint(), shader, info);
    return (uint64_t)shader;
}

void Replayer::CreateObject(const JValue& o) {
    const uint64_t id = o.Get("id") ? o.Get("id")->Uint() : 0;
    const std::string type = Str(o.Get("type"));
    const std::string cmd = Str(o.Get("cmd"));
    const uint32_t index = o.Get("index") ? (uint32_t)o.Get("index")->Uint() : 0;
    const JValue* args = o.Get("args");
    if (_handles.count(id)) return;  // the device and its physical device
    _ctx.where = type + " " + std::to_string(id);
    if (_options.trace) {
        std::fprintf(stderr, "object %llu %s (%s)\n", (unsigned long long)id, type.c_str(), cmd.c_str());
        std::fflush(stderr);
    }
    // An object whose arguments name objects the replay does not have is not created: the driver
    // would be handed null handles.
    const size_t unresolvedBefore = _ctx.unresolved;
    auto resolved = [&] { return _ctx.unresolved == unresolvedBefore; };
    uint64_t handle = 0;
    bool known = true;
    /** Set where an object is meant to be left out, so its absence is not reported as a failure. */
    bool quiet = false;
    VkDevice d = _device;

    if (type == "VkInstance") {
        handle = (uint64_t)(uintptr_t)_instance;
    } else if (type == "VkPhysicalDevice") {
        handle = (uint64_t)(uintptr_t)_physical;
    } else if (type == "VkQueue") {
        uint32_t family = args && args->Get("queueFamilyIndex") ? (uint32_t)args->Get("queueFamilyIndex")->Uint() : _queueFamily;
        uint32_t qi = args && args->Get("queueIndex") ? (uint32_t)args->Get("queueIndex")->Uint() : 0;
        VkQueue q = VK_NULL_HANDLE;
        _fns.GetDeviceQueue(d, family, qi, &q);
        handle = (uint64_t)(uintptr_t)(q ? q : _queue);
        if (_exporter) _exporter->Queue(id, (VkQueue)(uintptr_t)handle, family, qi);
    } else if (type == "VkSurfaceKHR" || type == "VkSwapchainKHR" || type == "VkDeviceMemory" || type == "VkPipelineCache" ||
               type == "VkDebugUtilsMessengerEXT" || type == "VkDebugReportCallbackEXT" ||
               // Sets are written from their snapshots and template pushes pushed as writes (IssueCommand).
               type == "VkDescriptorUpdateTemplate" ||
               // A deferred operation is a host-side handle for work the replay does inline.
               type == "VkAccelerationStructureNV" || type == "VkDeferredOperationKHR") {
        _skipped.insert(id);
        _report->objectsSkipped++;
        if (_exporter) _exporter->Skipped(type, id, "left out on purpose: the frame does not need it to run again");
        return;
    } else if (type == "VkAccelerationStructureKHR") {
        // The structure sits in a buffer the replay already made; only the handle is new. Its
        // contents come from replaying the build that filled it, not from the capture — an
        // acceleration structure is opaque and there is nothing to copy.
        Args_vkCreateAccelerationStructureKHR a{};
        if (args) DecodeArgs(_ctx, *args, a);
        if (!a.pCreateInfo || !_fns.CreateAccelerationStructureKHR) {
            _skipped.insert(id);
            _report->objectsSkipped++;
            return;
        }
        VkAccelerationStructureCreateInfoKHR info = *a.pCreateInfo;
        // An address the capture asked for means nothing here, and asking for one again would need
        // the same address to be free. The replay lets the driver place it.
        info.createFlags &= ~VK_ACCELERATION_STRUCTURE_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_KHR;
        info.deviceAddress = 0;
        VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
        VkResult r = _fns.CreateAccelerationStructureKHR(_device, &info, nullptr, &structure);
        if (r != VK_SUCCESS || !structure) {
            Problem("VkAccelerationStructureKHR " + std::to_string(id) + " could not be created");
            _skipped.insert(id);
            _report->objectsSkipped++;
            return;
        }
        // Tracked by the common path below, with every other created object.
        handle = (uint64_t)(uintptr_t)structure;
        if (_exporter) _exporter->Create(type, id, handle, "vkCreateAccelerationStructureKHR", info, "placed by the driver, not at the captured address");
    } else if (type == "VkImage" && cmd == "vkGetSwapchainImagesKHR") {
        const JValue* swapchain = _capture->Object(o.Get("parent") ? o.Get("parent")->Uint() : 0);
        const JValue* sargs = swapchain ? swapchain->Get("args") : nullptr;
        Args_vkCreateSwapchainKHR a{};
        if (sargs) DecodeArgs(_ctx, *sargs, a);
        if (!a.pCreateInfo) {
            Problem("swapchain image " + std::to_string(id) + ": the swapchain is not in the capture");
            return;
        }
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = a.pCreateInfo->imageFormat;
        info.extent = {a.pCreateInfo->imageExtent.width, a.pCreateInfo->imageExtent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = std::max(1u, a.pCreateInfo->imageArrayLayers);
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = a.pCreateInfo->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        _exportComment = "an image of swapchain " + std::to_string(o.Get("parent") ? o.Get("parent")->Uint() : 0) +
                         ", as an ordinary image of its format and size";
        handle = CreateImage(id, info);
        _exportComment.clear();
    } else if (type == "VkImage" && cmd == "vkCreateImage") {
        Args_vkCreateImage a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo) handle = CreateImage(id, *a.pCreateInfo);
    } else if (type == "VkBuffer" && cmd == "vkCreateBuffer") {
        Args_vkCreateBuffer a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo) handle = CreateBuffer(id, *a.pCreateInfo);
    } else if (type == "VkImageView") {
        Args_vkCreateImageView a{};
        DecodeArgs(_ctx, *args, a);
        VkImageView view = VK_NULL_HANDLE;
        if (a.pCreateInfo && a.pCreateInfo->image && resolved() && _fns.CreateImageView(d, a.pCreateInfo, nullptr, &view) == VK_SUCCESS) {
            handle = (uint64_t)view;
            if (_exporter) _exporter->Create(type, id, handle, "vkCreateImageView", *a.pCreateInfo);
            ViewRecord rec;
            rec.image = IdOf(args->Get("pCreateInfo")->Get("image"));
            rec.range = a.pCreateInfo->subresourceRange;
            _views[id] = rec;
        }
    } else if (type == "VkBufferView") {
        Args_vkCreateBufferView a{};
        DecodeArgs(_ctx, *args, a);
        VkBufferView view = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateBufferView(d, a.pCreateInfo, nullptr, &view) == VK_SUCCESS) handle = (uint64_t)view;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateBufferView", *a.pCreateInfo);
    } else if (type == "VkSampler") {
        Args_vkCreateSampler a{};
        DecodeArgs(_ctx, *args, a);
        VkSampler s = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateSampler(d, a.pCreateInfo, nullptr, &s) == VK_SUCCESS) handle = (uint64_t)s;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateSampler", *a.pCreateInfo);
    } else if (type == "VkDescriptorSetLayout") {
        Args_vkCreateDescriptorSetLayout a{};
        DecodeArgs(_ctx, *args, a);
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateDescriptorSetLayout(d, a.pCreateInfo, nullptr, &l) == VK_SUCCESS) handle = (uint64_t)l;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateDescriptorSetLayout", *a.pCreateInfo);
    } else if (type == "VkPipelineLayout") {
        Args_vkCreatePipelineLayout a{};
        DecodeArgs(_ctx, *args, a);
        VkPipelineLayout l = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreatePipelineLayout(d, a.pCreateInfo, nullptr, &l) == VK_SUCCESS) handle = (uint64_t)l;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreatePipelineLayout", *a.pCreateInfo);
    } else if (type == "VkDescriptorPool") {
        Args_vkCreateDescriptorPool a{};
        DecodeArgs(_ctx, *args, a);
        VkDescriptorPool p = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateDescriptorPool(d, a.pCreateInfo, nullptr, &p) == VK_SUCCESS) handle = (uint64_t)p;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateDescriptorPool", *a.pCreateInfo);
    } else if (type == "VkCommandPool") {
        Args_vkCreateCommandPool a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo) {
            VkCommandPoolCreateInfo info = *a.pCreateInfo;
            info.flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            VkCommandPool p = VK_NULL_HANDLE;
            if (_fns.CreateCommandPool(d, &info, nullptr, &p) == VK_SUCCESS) handle = (uint64_t)p;
            if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateCommandPool", info, "its command buffers can be reset one at a time");
        }
    } else if (type == "VkCommandBuffer") {
        Args_vkAllocateCommandBuffers a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pAllocateInfo && a.pAllocateInfo->commandPool) {
            VkCommandBufferAllocateInfo info = *a.pAllocateInfo;
            info.commandBufferCount = 1;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            if (_fns.AllocateCommandBuffers(d, &info, &cb) == VK_SUCCESS) handle = (uint64_t)(uintptr_t)cb;
            if (handle && _exporter) _exporter->AllocateCommandBuffer(id, cb, info);
        }
    } else if (type == "VkDescriptorSet") {
        Args_vkAllocateDescriptorSets a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pAllocateInfo && index < a.pAllocateInfo->descriptorSetCount && resolved()) {
            VkDescriptorSetAllocateInfo info = *a.pAllocateInfo;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &a.pAllocateInfo->pSetLayouts[index];
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkResult r = _fns.AllocateDescriptorSets(d, &info, &set);
            if (r == VK_SUCCESS) handle = (uint64_t)set;
            if (handle && _exporter) _exporter->AllocateDescriptorSet(id, set, info);
            if (r != VK_SUCCESS) Problem("descriptor set " + std::to_string(id) + ": vkAllocateDescriptorSets failed (" + std::to_string(r) + ")");
        }
    } else if (type == "VkFence") {
        Args_vkCreateFence a{};
        DecodeArgs(_ctx, *args, a);
        VkFence f = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateFence(d, a.pCreateInfo, nullptr, &f) == VK_SUCCESS) handle = (uint64_t)f;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateFence", *a.pCreateInfo);
    } else if (type == "VkSemaphore") {
        Args_vkCreateSemaphore a{};
        DecodeArgs(_ctx, *args, a);
        VkSemaphore s = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateSemaphore(d, a.pCreateInfo, nullptr, &s) == VK_SUCCESS) handle = (uint64_t)s;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateSemaphore", *a.pCreateInfo);
    } else if (type == "VkEvent") {
        Args_vkCreateEvent a{};
        DecodeArgs(_ctx, *args, a);
        VkEvent e = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateEvent(d, a.pCreateInfo, nullptr, &e) == VK_SUCCESS) handle = (uint64_t)e;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateEvent", *a.pCreateInfo);
    } else if (type == "VkQueryPool") {
        Args_vkCreateQueryPool a{};
        DecodeArgs(_ctx, *args, a);
        VkQueryPool q = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateQueryPool(d, a.pCreateInfo, nullptr, &q) == VK_SUCCESS) handle = (uint64_t)q;
        if (handle && _exporter) _exporter->Create(type, id, handle, "vkCreateQueryPool", *a.pCreateInfo);
    } else if (type == "VkRenderPass" && cmd == "vkCreateRenderPass") {
        Args_vkCreateRenderPass a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo) {
            VkRenderPassCreateInfo info = *a.pCreateInfo;
            std::vector<VkAttachmentDescription> attachments(info.pAttachments, info.pAttachments + info.attachmentCount);
            RenderPassRecord rec;
            for (auto& att : attachments) {
                rec.initialLayouts.push_back(att.initialLayout);
                rec.finalLayouts.push_back(att.finalLayout);
                rec.loadOps.push_back(att.loadOp);
                rec.formats.push_back(att.format);
                // Store everything, as the capture layer does while capturing: the passes' results are read back.
                att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            if (info.subpassCount && info.pSubpasses[0].pDepthStencilAttachment && info.pSubpasses[0].pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
                rec.depthAttachment = (int)info.pSubpasses[0].pDepthStencilAttachment->attachment;
            for (auto* s = (const VkBaseInStructure*)info.pNext; s; s = s->pNext) {
                if (s->sType != VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) continue;
                const auto* mv = (const VkRenderPassMultiviewCreateInfo*)s;
                for (uint32_t k = 0; mv->pViewMasks && k < mv->subpassCount; ++k) rec.views = std::max(rec.views, ViewCount(mv->pViewMasks[k]));
            }
            info.pAttachments = attachments.data();
            VkRenderPass rp = VK_NULL_HANDLE;
            if (_fns.CreateRenderPass(d, &info, nullptr, &rp) == VK_SUCCESS) {
                handle = (uint64_t)rp;
                _renderPasses[id] = rec;
                if (_exporter) _exporter->Create(type, id, handle, "vkCreateRenderPass", info, "every attachment stored, so each pass's result can be read");
            }
        }
    } else if (type == "VkRenderPass" && (cmd == "vkCreateRenderPass2" || cmd == "vkCreateRenderPass2KHR")) {
        Args_vkCreateRenderPass2 a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo) {
            VkRenderPassCreateInfo2 info = *a.pCreateInfo;
            std::vector<VkAttachmentDescription2> attachments(info.pAttachments, info.pAttachments + info.attachmentCount);
            RenderPassRecord rec;
            for (auto& att : attachments) {
                rec.initialLayouts.push_back(att.initialLayout);
                rec.finalLayouts.push_back(att.finalLayout);
                rec.loadOps.push_back(att.loadOp);
                rec.formats.push_back(att.format);
                att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            if (info.subpassCount && info.pSubpasses[0].pDepthStencilAttachment && info.pSubpasses[0].pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
                rec.depthAttachment = (int)info.pSubpasses[0].pDepthStencilAttachment->attachment;
            for (uint32_t k = 0; k < info.subpassCount; ++k) rec.views = std::max(rec.views, ViewCount(info.pSubpasses[k].viewMask));
            info.pAttachments = attachments.data();
            VkRenderPass rp = VK_NULL_HANDLE;
            if (_fns.CreateRenderPass2(d, &info, nullptr, &rp) == VK_SUCCESS) {
                handle = (uint64_t)rp;
                _renderPasses[id] = rec;
                if (_exporter) _exporter->Create(type, id, handle, "vkCreateRenderPass2", info, "every attachment stored, so each pass's result can be read");
            }
        }
    } else if (type == "VkFramebuffer") {
        Args_vkCreateFramebuffer a{};
        DecodeArgs(_ctx, *args, a);
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateFramebuffer(d, a.pCreateInfo, nullptr, &fb) == VK_SUCCESS) {
            handle = (uint64_t)fb;
            if (_exporter) _exporter->Create(type, id, handle, "vkCreateFramebuffer", *a.pCreateInfo);
            _framebufferExtents[id] = {a.pCreateInfo->width, a.pCreateInfo->height};
            std::vector<uint64_t> views;
            if (const JValue* list = args->Get("pCreateInfo")->Get("pAttachments"); list && list->IsArray())
                for (uint32_t i = 0; i < list->count; ++i) views.push_back(IdOf(&list->items[i]));
            _framebufferViews[id] = std::move(views);
        }
    } else if (type == "VkShaderModule") {
        const uint8_t* blobCode = nullptr;
        size_t blobSize = 0;
        VkShaderModule m = ModuleFromBlob(o, "SPIR-V", &blobCode, &blobSize);
        if (m && _exporter) {
            VkShaderModuleCreateInfo exported{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            exported.codeSize = blobSize / 4 * 4;
            exported.pCode = reinterpret_cast<const uint32_t*>(blobCode);
            _exporter->Create(type, id, (uint64_t)m, "vkCreateShaderModule", exported);
        }
        // Whether the capture holds this module's code at all. A module created before the capture
        // started has no payload, and the layer summarizes an oversized pCode away (json_writer.h,
        // maxScalarArray), which every real shader exceeds — so there is nothing to build it from.
        const uint8_t* code = nullptr;
        size_t codeSize = 0;
        const bool hasCode = _capture->Blob(o, "SPIR-V", code, codeSize) && codeSize >= 4;
        if (!m && !hasCode && args) {
            // Only a shader small enough to have escaped the summary is still in the arguments;
            // decoding a summary would both report a problem and yield zeros, which is no module.
            const JValue* create = args->Get("pCreateInfo");
            const JValue* words = create ? create->Get("pCode") : nullptr;
            if (words && words->IsArray()) {
                Args_vkCreateShaderModule a{};
                DecodeArgs(_ctx, *args, a);
                if (a.pCreateInfo && a.pCreateInfo->pCode && a.pCreateInfo->codeSize >= 20 && a.pCreateInfo->pCode[0] == 0x07230203)
                    _fns.CreateShaderModule(d, a.pCreateInfo, nullptr, &m);
                if (m && _exporter) _exporter->Create(type, id, (uint64_t)m, "vkCreateShaderModule", *a.pCreateInfo);
            }
        }
        handle = (uint64_t)m;
        if (!m) {
            // A pipeline is created from its own stages' payloads, so a module the capture has no
            // code for is left out quietly. One whose code the driver rejected is worth reporting.
            _skipped.insert(id);
            quiet = !hasCode;
        }
    } else if (type == "VkPipeline") {
        if (args) handle = CreatePipeline(o, cmd, index, *args, unresolvedBefore);
    } else if (type == "VkShaderEXT") {
        if (args) handle = CreateShaderObject(o, index, *args, unresolvedBefore);
    } else {
        known = false;
    }
    _arena.Reset();

    if (!known) {
        Problem(type + " " + std::to_string(id) + " (" + cmd + ") is not replayed yet");
        _report->objectsSkipped++;
        if (_exporter) _exporter->Skipped(type, id, "left out: the replay does not make objects of this type yet");
        return;
    }
    if (!handle) {
        if (!quiet)
            Problem(type + " " + std::to_string(id) + " (" + cmd + ") " +
                    (resolved() ? "could not be created" : "was not created: it names objects the replay does not have"));
        _report->objectsSkipped++;
        if (_exporter)
            _exporter->Skipped(type, id, quiet ? "left out: the capture holds no code for it, and pipelines carry their own"
                                       : resolved() ? "left out: the replay could not create it"
                                                    : "left out: it names objects the replay does not have");
        return;
    }
    _handles[id] = handle;
    if (type != "VkInstance" && type != "VkPhysicalDevice" && type != "VkQueue") Track(type, handle);
    _report->objectsCreated++;
}

void Replayer::DestroyAll() {
    if (_device) {
        _fns.DeviceWaitIdle(_device);
        ReleaseTransients();
        if (_countModule) _fns.DestroyShaderModule(_device, _countModule, nullptr);
        _countModule = VK_NULL_HANDLE;
        if (_primitiveIdModule) _fns.DestroyShaderModule(_device, _primitiveIdModule, nullptr);
        _primitiveIdModule = VK_NULL_HANDLE;
        for (auto it = _created.rbegin(); it != _created.rend(); ++it) {
            const std::string& t = it->type;
            uint64_t h = it->handle;
            if (t == "VkImage") _fns.DestroyImage(_device, (VkImage)h, nullptr);
            else if (t == "VkBuffer") _fns.DestroyBuffer(_device, (VkBuffer)h, nullptr);
            else if (t == "VkImageView") _fns.DestroyImageView(_device, (VkImageView)h, nullptr);
            else if (t == "VkBufferView") _fns.DestroyBufferView(_device, (VkBufferView)h, nullptr);
            else if (t == "VkSampler") _fns.DestroySampler(_device, (VkSampler)h, nullptr);
            else if (t == "VkDescriptorSetLayout") _fns.DestroyDescriptorSetLayout(_device, (VkDescriptorSetLayout)h, nullptr);
            else if (t == "VkPipelineLayout") _fns.DestroyPipelineLayout(_device, (VkPipelineLayout)h, nullptr);
            else if (t == "VkDescriptorPool") _fns.DestroyDescriptorPool(_device, (VkDescriptorPool)h, nullptr);
            else if (t == "VkCommandPool") _fns.DestroyCommandPool(_device, (VkCommandPool)h, nullptr);
            else if (t == "VkFence") _fns.DestroyFence(_device, (VkFence)h, nullptr);
            else if (t == "VkSemaphore") _fns.DestroySemaphore(_device, (VkSemaphore)h, nullptr);
            else if (t == "VkEvent") _fns.DestroyEvent(_device, (VkEvent)h, nullptr);
            else if (t == "VkQueryPool") _fns.DestroyQueryPool(_device, (VkQueryPool)h, nullptr);
            else if (t == "VkRenderPass") _fns.DestroyRenderPass(_device, (VkRenderPass)h, nullptr);
            else if (t == "VkFramebuffer") _fns.DestroyFramebuffer(_device, (VkFramebuffer)h, nullptr);
            else if (t == "VkShaderModule") _fns.DestroyShaderModule(_device, (VkShaderModule)h, nullptr);
            else if (t == "VkPipeline") _fns.DestroyPipeline(_device, (VkPipeline)h, nullptr);
            else if (t == "VkShaderEXT" && _fns.DestroyShaderEXT) _fns.DestroyShaderEXT(_device, (VkShaderEXT)h, nullptr);
            else if (t == "VkAccelerationStructureKHR" && _fns.DestroyAccelerationStructureKHR)
                _fns.DestroyAccelerationStructureKHR(_device, (VkAccelerationStructureKHR)h, nullptr);
            // Command buffers and descriptor sets go with their pools.
        }
        // The build scratch is the replay's own, not the capture's, so it is not in _created.
        // Everything it outgrew goes with it: those buffers were kept alive only until the
        // submissions holding their addresses had run.
        for (const auto& [buffer, memory] : _retiredScratch) {
            _fns.DestroyBuffer(_device, buffer, nullptr);
            _fns.FreeMemory(_device, memory, nullptr);
        }
        _retiredScratch.clear();
        if (_scratch) _fns.DestroyBuffer(_device, _scratch, nullptr);
        if (_scratchMemory) _fns.FreeMemory(_device, _scratchMemory, nullptr);
        _scratch = VK_NULL_HANDLE;
        _scratchMemory = VK_NULL_HANDLE;
        _scratchSize = 0;
        _scratchUsed = 0;
        if (_bindingTable) _fns.DestroyBuffer(_device, _bindingTable, nullptr);
        if (_bindingTableMemory) _fns.FreeMemory(_device, _bindingTableMemory, nullptr);
        _bindingTable = VK_NULL_HANDLE;
        _bindingTableMemory = VK_NULL_HANDLE;
        _bindingTableMapped = nullptr;
        _bindingTableSize = 0;
        for (VkDeviceMemory m : _memories) _fns.FreeMemory(_device, m, nullptr);
        if (_utilityPool) _fns.DestroyCommandPool(_device, _utilityPool, nullptr);
        _fns.DestroyDevice(_device, nullptr);
    }
    if (_messenger) _fns.DestroyDebugUtilsMessengerEXT(_instance, _messenger, nullptr);
    if (_instance) _fns.DestroyInstance(_instance, nullptr);
    _created.clear();
    _memories.clear();
    _device = VK_NULL_HANDLE;
    _instance = VK_NULL_HANDLE;
    _messenger = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------------------------
// Frame-start state

void Replayer::ComputeInitialLayouts() {
    // The first layout the frame expects each subresource in: the old layout of its first barrier,
    // the layout a descriptor snapshot binds it with, the initial layout of a pass that renders to
    // it, or the layout a transfer or clear names. Mips and layers of one image can start apart (a
    // mip chain being built, one layer of an array bound for sampling).
    const JValue* commands = _capture->Commands();
    if (!commands || !commands->IsArray()) return;
    auto layoutOf = [&](const JValue* v) { return (VkImageLayout)DecodeEnum_VkImageLayout(_ctx, v); };
    auto u32 = [](const JValue* v, uint32_t fallback) { return v ? (uint32_t)v->Uint() : fallback; };
    auto note = [&](uint64_t id, uint32_t baseMip, uint32_t mips, uint32_t baseLayer, uint32_t layers, VkImageLayout layout) {
        auto it = _images.find(id);
        if (it == _images.end() || layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
        const ImageRecord& image = it->second;
        if (baseMip >= image.mips || baseLayer >= image.layers) return;
        mips = std::min(mips, image.mips - baseMip);        // VK_REMAINING_* included
        layers = std::min(layers, image.layers - baseLayer);
        auto& layouts = _initialLayouts[id];
        layouts.resize((size_t)image.mips * image.layers, VK_IMAGE_LAYOUT_UNDEFINED);
        for (uint32_t m = baseMip; m < baseMip + mips; ++m)
            for (uint32_t l = baseLayer; l < baseLayer + layers; ++l)
                if (layouts[(size_t)m * image.layers + l] == VK_IMAGE_LAYOUT_UNDEFINED) layouts[(size_t)m * image.layers + l] = layout;
    };
    auto noteRange = [&](uint64_t id, const JValue* range, VkImageLayout layout) {
        if (range)
            note(id, u32(range->Get("baseMipLevel"), 0), u32(range->Get("levelCount"), 1), u32(range->Get("baseArrayLayer"), 0),
                 u32(range->Get("layerCount"), 1), layout);
    };
    auto noteLayers = [&](uint64_t id, const JValue* layers, VkImageLayout layout) {
        if (layers) note(id, u32(layers->Get("mipLevel"), 0), 1, u32(layers->Get("baseArrayLayer"), 0), u32(layers->Get("layerCount"), 1), layout);
    };
    auto noteView = [&](uint64_t viewId, VkImageLayout layout) {
        auto it = _views.find(viewId);
        if (it == _views.end()) return;
        const VkImageSubresourceRange& r = it->second.range;
        note(it->second.image, r.baseMipLevel, r.levelCount, r.baseArrayLayer, r.layerCount, layout);
    };
    // Copies, blits and resolves between images, and transfers between an image and a buffer: the
    // arguments themselves, or the info struct of the commands' 2 forms.
    static const char* const kTransferInfos[] = {"pCopyImageInfo", "pBlitImageInfo", "pResolveImageInfo", "pCopyImageToBufferInfo",
                                                 "pCopyBufferToImageInfo"};
    auto noteTransfer = [&](const JValue* a) {
        const JValue* regions = a ? a->Get("pRegions") : nullptr;
        for (uint32_t k = 0; regions && regions->IsArray() && k < regions->count; ++k) {
            const JValue& r = regions->items[k];
            if (const JValue* src = a->Get("srcImage"))
                noteLayers(IdOf(src), r.Get("srcSubresource") ? r.Get("srcSubresource") : r.Get("imageSubresource"), layoutOf(a->Get("srcImageLayout")));
            if (const JValue* dst = a->Get("dstImage"))
                noteLayers(IdOf(dst), r.Get("dstSubresource") ? r.Get("dstSubresource") : r.Get("imageSubresource"), layoutOf(a->Get("dstImageLayout")));
        }
    };
    for (uint32_t i = 0; i < commands->count; ++i) {
        const JValue& c = commands->items[i];
        const std::string m = Str(c.Get("method"));
        const JValue* args = c.Get("args");
        if (!args) continue;
        if (m == "vkCmdPipelineBarrier" || m == "vkCmdPipelineBarrier2" || m == "vkCmdPipelineBarrier2KHR") {
            if (const JValue* list = args->Get(m == "vkCmdPipelineBarrier" ? "pImageMemoryBarriers" : "pBarrierInfo"); list) {
                const JValue* barriers = m == "vkCmdPipelineBarrier" ? list : list->Get("pImageMemoryBarriers");
                if (barriers && barriers->IsArray())
                    for (uint32_t b = 0; b < barriers->count; ++b)
                        noteRange(IdOf(barriers->items[b].Get("image")), barriers->items[b].Get("subresourceRange"),
                                  layoutOf(barriers->items[b].Get("oldLayout")));
            }
        }
        if (const JValue* d = c.Get("descriptors")) {
            if (const JValue* sets = d->Get("sets"); sets && sets->IsArray()) {
                for (uint32_t s = 0; s < sets->count; ++s) {
                    const JValue* bindings = sets->items[s].Get("bindings");
                    for (uint32_t b = 0; bindings && b < bindings->count; ++b) {
                        const JValue* list = bindings->items[b].Get("descriptors");
                        for (uint32_t k = 0; list && k < list->count; ++k) {
                            const JValue& desc = list->items[k];
                            noteView(IdOf(desc.Get("imageView")), layoutOf(desc.Get("imageLayout")));
                        }
                    }
                }
            }
        }
        if (IsBeginRenderPass(m)) {
            const JValue* begin = args->Get("pRenderPassBegin");
            const uint64_t rp = begin ? IdOf(begin->Get("renderPass")) : 0;
            const uint64_t fb = begin ? IdOf(begin->Get("framebuffer")) : 0;
            auto rit = _renderPasses.find(rp);
            auto fit = _framebufferViews.find(fb);
            if (rit != _renderPasses.end() && fit != _framebufferViews.end()) {
                // A pass requires its attachments in their initial layout whatever it loads or clears.
                for (size_t a = 0; a < fit->second.size() && a < rit->second.initialLayouts.size(); ++a)
                    noteView(fit->second[a], rit->second.initialLayouts[a]);
            }
        } else if (IsBeginRendering(m)) {
            const JValue* info = args->Get("pRenderingInfo");
            const bool resuming = info && Str(info->Get("flags")).find("VK_RENDERING_RESUMING_BIT") != std::string::npos;
            auto attachment = [&](const JValue* a) {
                if (!a || a->IsNull()) return;
                noteView(IdOf(a->Get("imageView")), layoutOf(a->Get("imageLayout")));
                noteView(IdOf(a->Get("resolveImageView")), layoutOf(a->Get("resolveImageLayout")));
            };
            if (info && !resuming) {
                if (const JValue* colors = info->Get("pColorAttachments"); colors && colors->IsArray())
                    for (uint32_t k = 0; k < colors->count; ++k) attachment(&colors->items[k]);
                attachment(info->Get("pDepthAttachment"));
                attachment(info->Get("pStencilAttachment"));
            }
        } else if (m == "vkCmdClearColorImage" || m == "vkCmdClearDepthStencilImage") {
            if (const JValue* ranges = args->Get("pRanges"); ranges && ranges->IsArray())
                for (uint32_t k = 0; k < ranges->count; ++k) noteRange(IdOf(args->Get("image")), &ranges->items[k], layoutOf(args->Get("imageLayout")));
        } else if (StartsWith(m, "vkCmdCopy") || StartsWith(m, "vkCmdBlitImage") || StartsWith(m, "vkCmdResolveImage")) {
            noteTransfer(args);
            for (const char* name : kTransferInfos) noteTransfer(args->Get(name));
        }
    }
    _arena.Reset();
}

void Replayer::UploadImageContents() {
    const JValue* textures = _capture->Textures();
    if (!textures || !textures->IsArray()) return;
    // Sampled images first: each is read back when the pass that binds it ends, so it may already hold
    // what the frame wrote there. What an image held when the frame first read it comes second, and wins.
    std::vector<const JValue*> ordered;
    for (const char* kind : {"sampled", "initial"})
        for (uint32_t i = 0; i < textures->count; ++i)
            if (const JValue* info = textures->items[i].Get("info"); info && Str(info->Get("kind")) == kind) ordered.push_back(&textures->items[i]);
    for (const JValue* texture : ordered) {
        const JValue& t = *texture;
        const JValue* info = t.Get("info");
        const bool initial = Str(info->Get("kind")) == "initial";
        if (info->Get("error")) {
            if (initial)
                Problem("image " + std::to_string(info->Get("id")->Uint()) + ": what it held at the start of the frame was not captured (" +
                        Str(info->Get("error")) + ")");
            continue;
        }
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!_capture->Payload(t.Get("payload"), data, size)) continue;
        auto it = _images.find(info->Get("id")->Uint());
        if (it == _images.end()) continue;
        ImageRecord& image = it->second;
        const uint32_t baseMip = (uint32_t)info->Get("mip")->Uint();
        const uint32_t mips = info->Get("mips") ? (uint32_t)info->Get("mips")->Uint() : 1;
        const uint32_t layers = std::max<uint32_t>(1, (uint32_t)info->Get("layers")->Uint());
        const uint32_t baseLayer = info->Get("baseLayer") ? (uint32_t)info->Get("baseLayer")->Uint() : 0;
        const VkImageAspectFlags aspect = AspectOf(Str(info->Get("aspect")));
        const vkinsp::FormatBlock block = vkinsp::FormatBlockInfo(image.format, aspect);
        if (!block.bytes) {
            Problem(std::string(initial ? "image " : "sampled image ") + std::to_string(info->Get("id")->Uint()) + ": its format cannot be uploaded");
            continue;
        }        std::vector<VkBufferImageCopy> regions;
        VkDeviceSize offset = 0;
        for (uint32_t m = baseMip; m < baseMip + mips && m < image.mips; ++m) {
            uint32_t w = std::max(1u, image.extent.width >> m);
            uint32_t h = std::max(1u, image.extent.height >> m);
            uint32_t dd = std::max(1u, image.extent.depth >> m);
            VkDeviceSize bytes = (VkDeviceSize)((w + block.width - 1) / block.width) * ((h + block.height - 1) / block.height) * block.bytes * std::max(dd, layers);
            if (offset + bytes > size) break;
            VkBufferImageCopy r{};
            r.bufferOffset = offset;
            r.imageSubresource = {aspect, m, baseLayer, layers};
            r.imageExtent = {w, h, dd};
            regions.push_back(r);
            offset += bytes;
        }
        if (regions.empty()) continue;
        Staging staging;
        if (!CreateStaging(offset, staging)) continue;
        std::memcpy(staging.mapped, data, (size_t)offset);
        RunOneTime([&](VkCommandBuffer cb) {
            TransitionAll(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            _fns.CmdCopyBufferToImage(cb, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
        });
        DestroyStaging(staging);
        if (_exporter) _exporter->UploadImage(info->Get("id")->Uint(), image.image, initial, regions, data, (size_t)offset);
        if (initial) _report->initialImagesUploaded++;
        else _report->texturesUploaded++;
    }
}

void Replayer::TransitionToInitialLayouts() {
    if (_exporter) _exporter->BeginInitialLayouts();
    RunOneTime([&](VkCommandBuffer cb) {
        for (auto& [id, image] : _images) {
            auto it = _initialLayouts.find(id);
            if (it == _initialLayouts.end()) continue;
            std::vector<VkImageLayout> targets = it->second;
            if (!_hasSwapchainExtension)
                for (VkImageLayout& t : targets)
                    if (t == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) t = VK_IMAGE_LAYOUT_UNDEFINED;
            if (_exporter) _exporter->InitialLayouts(id, image.image, targets);
            TransitionSubresources(cb, image, targets);
        }
    });
    if (_exporter) _exporter->EndInitialLayouts();
}

// ---------------------------------------------------------------------------------------------
// Commands

void Replayer::BuildGroups() {
    const JValue* commands = _capture->Commands();
    if (!commands || !commands->IsArray()) return;
    std::unordered_map<uint64_t, size_t> open;  // command buffer id -> index into _groups
    for (uint32_t i = 0; i < commands->count; ++i) {
        const JValue& c = commands->items[i];
        if (c.Get("secondary")) continue;
        const std::string m = Str(c.Get("method"));
        const uint64_t cb = IdOf(c.Get("object"));
        if (m == "vkBeginCommandBuffer") {
            open[cb] = _groups.size();
            _groups.push_back({cb, i, i, false});
        } else if (m == "vkEndCommandBuffer") {
            auto it = open.find(cb);
            if (it != open.end()) {
                _groups[it->second].last = i;
                open.erase(it);
            }
        }
    }
    for (auto& [cb, index] : open) {
        Problem("command buffer " + std::to_string(cb) + " is not ended in the capture");
        _groups[index].last = commands->count - 1;
    }
}

void Replayer::ApplyBufferData(const CommandGroup& group) {
    const JValue* commands = _capture->Commands();
    std::unordered_set<uint64_t> applied;
    auto apply = [&](uint64_t dataId) {
        if (!dataId || applied.count(dataId)) return;
        applied.insert(dataId);
        auto it = _bufferData.find(dataId);
        if (it == _bufferData.end()) return;
        const JValue* info = it->second->Get("info");
        if (!info || info->Get("error")) return;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!_capture->Payload(it->second->Get("payload"), data, size) || !size) return;
        auto bit = _buffers.find(info->Get("buffer")->Uint());
        if (bit == _buffers.end()) return;
        VkDeviceSize offset = info->Get("offset")->Uint();
        if (offset + size > bit->second.size) size = (size_t)(bit->second.size - offset);
        UploadToBuffer(bit->second.buffer, offset, data, size);
        if (_exporter) _exporter->UploadBuffer(bit->first, bit->second.buffer, offset, data, size);
        _report->bufferUploads++;
    };
    for (uint32_t i = group.first; i <= group.last && i < commands->count; ++i) {
        const JValue& c = commands->items[i];
        if (const JValue* list = c.Get("bufferData"); list && list->IsArray())
            for (uint32_t k = 0; k < list->count; ++k) apply(list->items[k].Uint());
        // An acceleration structure build reads its geometry from buffers named by address, and the
        // contents the layer read back for them are listed apart from bufferData because each entry
        // also says which buffer and offset it came from (src/vulkan/src/hooks.cpp). Without this the
        // replay builds a structure out of whatever those buffers happen to hold, and every ray misses.
        if (const JValue* list = c.Get("buildData"); list && list->IsArray()) {
            for (uint32_t k = 0; k < list->count; ++k) {
                const JValue* capture = list->items[k].Get("capture");
                if (capture) apply(capture->Uint());
            }
        }
        if (const JValue* d = c.Get("descriptors")) {
            const JValue* sets = d->Get("sets");
            for (uint32_t s = 0; sets && s < sets->count; ++s) {
                const JValue* bindings = sets->items[s].Get("bindings");
                for (uint32_t b = 0; bindings && b < bindings->count; ++b) {
                    const JValue* list = bindings->items[b].Get("descriptors");
                    for (uint32_t k = 0; list && k < list->count; ++k) {
                        const JValue& desc = list->items[k];
                        if (desc.Get("buffer") && desc.Get("data")) apply(desc.Get("data")->Uint());
                    }
                }
            }
        }
    }
}

void Replayer::ApplyDescriptorSnapshot(const JValue* descriptors) {
    const JValue* sets = descriptors ? descriptors->Get("sets") : nullptr;
    if (!sets || !sets->IsArray()) return;
    for (uint32_t s = 0; s < sets->count; ++s) {
        const JValue& set = sets->items[s];
        const uint64_t setId = IdOf(set.Get("descriptorSet"));
        const VkDescriptorSet handle = (VkDescriptorSet)Handle(setId);
        if (!setId || !handle) continue;  // push descriptors are recorded by their own command
        DescriptorWrites w;
        BuildDescriptorWrites(set, handle, w);
        auto& last = _descriptorContents[setId];
        if (last == w.key) continue;  // rewriting a bound set would invalidate the command buffers that bound it
        last = w.key;
        if (!w.writes.empty()) _fns.UpdateDescriptorSets(_device, (uint32_t)w.writes.size(), w.writes.data(), 0, nullptr);
        if (_exporter) _exporter->UpdateDescriptorSets(setId, w.writes);
    }
    _arena.Reset();
}


/**
 * A device address the capture recorded, as an address in *this* process.
 *
 * The number in the capture is the captured process's and means nothing here. What makes it
 * translatable is that the layer also recorded which buffer held it and how far in
 * (src/vulkan/src/hooks.cpp, WriteBuildAddress) — so the replay looks up its own buffer for that
 * object and asks the driver where it put it. Returns 0 when the capture recorded no buffer, which
 * is an address into memory the capture never resolved.
 */
VkDeviceAddress Replayer::RemapAddress(uint64_t bufferId, uint64_t offset) {
    if (!bufferId || !_fns.GetBufferDeviceAddress) return 0;
    const uint64_t id = bufferId;
    const uint64_t handle = Handle(id);
    if (!handle) return 0;
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = (VkBuffer)(uintptr_t)handle;
    const VkDeviceAddress base = _fns.GetBufferDeviceAddress(_device, &info);
    if (!base) return 0;
    return base + offset;
}

/**
 * Scratch memory for a build, big enough for every build in the frame. The capture's scratch address
 * is not remapped: scratch holds no input, only the driver's working space, so a fresh buffer of the
 * size the driver asks for is equivalent and avoids depending on a buffer the capture may not hold.
 */
bool Replayer::ReserveScratch(VkDeviceSize size, VkDeviceAddress& address) {
    if (!size || !_fns.GetBufferDeviceAddress) return false;
    // Comfortably above every minAccelerationStructureScratchOffsetAlignment in the wild.
    constexpr VkDeviceSize kAlign = 256;
    VkDeviceSize at = (_scratchUsed + kAlign - 1) & ~(kAlign - 1);
    if (!_scratch || at + size > _scratchSize) {
        // The old buffer is retired rather than freed: builds already recorded into this
        // submission hold addresses into it, and freeing it would leave them writing into memory
        // the driver has taken back.
        if (_scratch) _retiredScratch.emplace_back(_scratch, _scratchMemory);
        _scratch = VK_NULL_HANDLE;
        _scratchMemory = VK_NULL_HANDLE;
        _scratchSize = 0;
        // Twice what is asked for and never trivially small, so a frame of many builds allocates
        // once rather than retiring a buffer per build.
        constexpr VkDeviceSize kMinimum = 4u << 20;
        const VkDeviceSize doubled = (at + size) * 2;
        const VkDeviceSize want = doubled > kMinimum ? doubled : kMinimum;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = want;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (_fns.CreateBuffer(_device, &info, nullptr, &_scratch) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        _fns.GetBufferMemoryRequirements(_device, _scratch, &requirements);
        if (!AllocateBound(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _scratchMemory, false, true)) return false;
        if (_fns.BindBufferMemory(_device, _scratch, _scratchMemory, 0) != VK_SUCCESS) return false;
        _scratchSize = want;
        _scratchUsed = 0;
        at = 0;
    }
    VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bi.buffer = _scratch;
    const VkDeviceAddress base = _fns.GetBufferDeviceAddress(_device, &bi);
    if (!base) return false;
    address = base + at;
    _scratchUsed = at + size;
    return true;
}


/** Element `i` of a JSON array, or null when it is not an array or is shorter than that. */
static const JValue* ItemAt(const JValue* array, uint32_t i) {
    return array && array->IsArray() && i < array->count ? &array->items[i] : nullptr;
}


VkDeviceAddress Replayer::RemapStructureAddress(uint64_t capturedAddress) {
    if (!capturedAddress || !_fns.GetAccelerationStructureDeviceAddressKHR) return 0;
    if (!_structureAddressesBuilt) {
        _structureAddressesBuilt = true;
        // The layer records the address it handed out on every structure, which is the only way back
        // from an instance's reference to the object it names (src/vulkan/src/hooks.cpp).
        const JValue* objects = _capture->Objects();
        for (uint32_t i = 0; objects && objects->IsArray() && i < objects->count; ++i) {
            const JValue& o = objects->items[i];
            if (Str(o.Get("type")) != "VkAccelerationStructureKHR") continue;
            const JValue* updates = o.Get("updates");
            const JValue* address = updates ? updates->Get("deviceAddress") : nullptr;
            const JValue* id = o.Get("id");
            if (address && id) _structureAddresses[address->Uint()] = id->Uint();
        }
    }
    auto it = _structureAddresses.find(capturedAddress);
    if (it == _structureAddresses.end()) return 0;
    const uint64_t handle = Handle(it->second);
    if (!handle) return 0;
    VkAccelerationStructureDeviceAddressInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    info.accelerationStructure = (VkAccelerationStructureKHR)(uintptr_t)handle;
    return _fns.GetAccelerationStructureDeviceAddressKHR(_device, &info);
}

bool Replayer::PatchInstanceReferences(uint64_t bufferId, uint64_t offset, uint32_t captureId, uint32_t instances) {
    // VkAccelerationStructureInstanceKHR: 64 bytes, the reference the last 8 of them.
    constexpr size_t kStride = 64;
    constexpr size_t kReferenceAt = 56;
    auto it = _bufferData.find(captureId);
    if (it == _bufferData.end()) return false;
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!_capture->Payload(it->second->Get("payload"), data, size) || size < kStride) return false;
    auto buffer = _buffers.find(bufferId);
    if (buffer == _buffers.end()) return false;

    std::vector<uint8_t> patched(data, data + size);
    const size_t held = size / kStride;
    for (size_t i = 0; i < held && i < instances; ++i) {
        uint64_t captured = 0;
        memcpy(&captured, &patched[i * kStride + kReferenceAt], sizeof(captured));
        const VkDeviceAddress here = RemapStructureAddress(captured);
        if (!here) {
            Problem("left out: instance " + std::to_string(i) + " of the build names a bottom level the replay does not have");
            return false;
        }
        const uint64_t value = (uint64_t)here;
        memcpy(&patched[i * kStride + kReferenceAt], &value, sizeof(value));
    }
    // After ApplyBufferData, which uploaded the buffer as captured at the start of this group.
    UploadToBuffer(buffer->second.buffer, offset, patched.data(), patched.size());
    return true;
}

void Replayer::BuildAccelerationStructures(const JValue& command, const JValue& args, VkCommandBuffer cb) {
    if (!_fns.CmdBuildAccelerationStructuresKHR || !_fns.GetAccelerationStructureBuildSizesKHR) {
        Problem("left out: this device has no acceleration structure builds");
        return;
    }
    Args_vkCmdBuildAccelerationStructuresKHR a{};
    const size_t unresolvedBefore = _ctx.unresolved;
    DecodeArgs(_ctx, args, a);
    if (!a.pInfos || !a.infoCount || _ctx.unresolved != unresolvedBefore) {
        Problem("left out: the build names objects the replay does not have");
        return;
    }
    const JValue* ranges = args.Get("ppBuildRangeInfos");
    // Where the layer put what it resolved: one entry per address it could tie to a buffer
    // (src/vulkan/src/hooks.cpp). The addresses in the arguments themselves are the captured
    // process's and are not translatable on their own.
    const JValue* resolved = command.Get("buildData");
    auto entryFor = [&](uint32_t info, uint32_t geometry, std::string_view field) -> const JValue* {
        if (!resolved || !resolved->IsArray()) return nullptr;
        for (uint32_t k = 0; k < resolved->count; ++k) {
            const JValue& e = resolved->items[k];
            const JValue* f = e.Get("field");
            if (!f || f->Str() != field) continue;
            const JValue* ij = e.Get("info");
            const JValue* gj = e.Get("geometry");
            if ((ij ? ij->Uint() : 0) != info || (gj ? gj->Uint() : 0) != geometry) continue;
            return &e;
        }
        return nullptr;
    };
    auto addressFor = [&](uint32_t info, uint32_t geometry, std::string_view field) -> VkDeviceAddress {
        if (!resolved || !resolved->IsArray()) return 0;
        for (uint32_t k = 0; k < resolved->count; ++k) {
            const JValue& e = resolved->items[k];
            const JValue* f = e.Get("field");
            if (!f || f->Str() != field) continue;
            const JValue* ij = e.Get("info");
            const JValue* gj = e.Get("geometry");
            if ((ij ? ij->Uint() : 0) != info || (gj ? gj->Uint() : 0) != geometry) continue;
            const JValue* b = e.Get("buffer");
            const JValue* o = e.Get("offset");
            return RemapAddress(b ? b->Uint() : 0, o ? o->Uint() : 0);
        }
        return 0;
    };

    // The decoded geometries point into the decoder's arena and are rewritten in place: every
    // address in them is the captured process's and has to become one of this process's.
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> built(a.pInfos, a.pInfos + a.infoCount);
    std::vector<std::vector<VkAccelerationStructureGeometryKHR>> geometries(a.infoCount);
    VkDeviceSize scratchNeeded = 0;
    std::vector<VkDeviceSize> scratchAt(a.infoCount, 0);

    for (uint32_t i = 0; i < a.infoCount; ++i) {
        VkAccelerationStructureBuildGeometryInfoKHR& info = built[i];
        geometries[i].assign(info.geometryCount, VkAccelerationStructureGeometryKHR{});
        std::vector<uint32_t> counts(info.geometryCount, 0);
        for (uint32_t g = 0; g < info.geometryCount; ++g) {
            const VkAccelerationStructureGeometryKHR* source = info.pGeometries ? &info.pGeometries[g]
                                                             : info.ppGeometries ? info.ppGeometries[g] : nullptr;
            if (!source) continue;
            // How many primitives this geometry holds, read first: rewriting the instances below
            // needs it, and a count filled in afterwards would leave that loop with nothing to do.
            const JValue* rangeList = ItemAt(ranges, i);
            const JValue* range = rangeList && rangeList->IsArray() ? ItemAt(rangeList, g) : rangeList;
            const JValue* count = range ? range->Get("primitiveCount") : nullptr;
            counts[g] = count ? (uint32_t)count->Uint() : 0;
            VkAccelerationStructureGeometryKHR geometry = *source;
            if (geometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
                geometry.geometry.triangles.vertexData.deviceAddress = addressFor(i, g, "vertexData");
                geometry.geometry.triangles.indexData.deviceAddress = addressFor(i, g, "indexData");
                geometry.geometry.triangles.transformData.deviceAddress = addressFor(i, g, "transformData");
            } else if (geometry.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
                geometry.geometry.aabbs.data.deviceAddress = addressFor(i, g, "data");
            } else if (geometry.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
                geometry.geometry.instances.data.deviceAddress = addressFor(i, g, "data");
                // Every instance names its bottom level by the captured process's address, which
                // means nothing here, so the buffer is rewritten before the build reads it.
                const JValue* entry = entryFor(i, g, "data");
                const JValue* captureValue = entry ? entry->Get("capture") : nullptr;
                const JValue* bufferValue = entry ? entry->Get("buffer") : nullptr;
                const JValue* offsetValue = entry ? entry->Get("offset") : nullptr;
                if (!captureValue) {
                    Problem("left out: the build's instances are not in this capture, so the bottom levels they name cannot be found");
                    return;
                }
                if (!PatchInstanceReferences(bufferValue ? bufferValue->Uint() : 0,
                                             offsetValue ? offsetValue->Uint() : 0,
                                             (uint32_t)captureValue->Uint(), counts[g])) {
                    return;
                }
            }
            // An address the capture never resolved leaves the build reading nothing, which the
            // driver rejects: better to leave the build out than to issue one that cannot work.
            const bool missing =
                (geometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR && !geometry.geometry.triangles.vertexData.deviceAddress)
                || (geometry.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR && !geometry.geometry.aabbs.data.deviceAddress)
                || (geometry.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR && !geometry.geometry.instances.data.deviceAddress);
            if (missing) {
                Problem("left out: the build reads memory this capture did not resolve to a buffer");
                return;
            }
            geometries[i][g] = geometry;
        }
        info.pGeometries = geometries[i].data();
        info.ppGeometries = nullptr;
        info.srcAccelerationStructure = VK_NULL_HANDLE;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;   // an update needs a source built here first

        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        _fns.GetAccelerationStructureBuildSizesKHR(_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                                   counts.data(), &sizes);
        // Each build gets its own stretch of the shared scratch, aligned generously.
        constexpr VkDeviceSize kAlign = 256;
        scratchAt[i] = (scratchNeeded + kAlign - 1) & ~(kAlign - 1);
        scratchNeeded = scratchAt[i] + sizes.buildScratchSize;
    }

    VkDeviceAddress scratchBase = 0;
    if (scratchNeeded && !ReserveScratch(scratchNeeded, scratchBase)) {
        Problem("left out: the build's scratch memory could not be allocated");
        return;
    }
    for (uint32_t i = 0; i < a.infoCount; ++i) built[i].scratchData.deviceAddress = scratchBase + scratchAt[i];

    _fns.CmdBuildAccelerationStructuresKHR(cb, a.infoCount, built.data(), a.ppBuildRangeInfos);
}


/**
 * Replays one vkCmdTraceRaysKHR.
 *
 * A trace names its shaders through a table in memory whose records hold opaque handles the
 * *captured* driver gave out. Those handles name nothing here, so the table cannot be uploaded as
 * it was captured: the replay builds its own, copying each region's bytes and replacing every
 * record's handle with this pipeline's handle for the same group. Matching one to the other is what
 * the captured pipeline's own handle blob is for.
 */
void Replayer::TraceRays(const JValue& command, const JValue& args, VkCommandBuffer cb) {
    if (!_fns.CmdTraceRaysKHR || !_fns.GetRayTracingShaderGroupHandlesKHR) {
        Problem("left out: this device has no ray tracing pipelines");
        return;
    }
    const JValue* regions = command.Get("bindingTableData");
    if (!regions || !regions->IsArray() || !regions->count) {
        Problem("left out: the shader binding table's contents are not in this capture");
        return;
    }
    const JValue* pipelineObject = _capture->Object(_boundRayTracingPipeline);
    const uint64_t pipelineHandle = Handle(_boundRayTracingPipeline);
    if (!pipelineObject || !pipelineHandle) {
        Problem("left out: the ray tracing pipeline bound at the trace was not replayed");
        return;
    }

    // The handles the captured driver gave, kept on the pipeline by the layer, and the ones this
    // driver gives for the same groups. A record is matched by the first and rewritten with the
    // second.
    const uint8_t* capturedHandles = nullptr;
    size_t capturedSize = 0;
    if (!_capture->Blob(*pipelineObject, "group handles", capturedHandles, capturedSize) || !capturedSize) {
        Problem("left out: the pipeline's shader group handles are not in this capture");
        return;
    }
    const JValue* updates = pipelineObject->Get("updates");
    const JValue* declared = updates ? updates->Get("shaderGroupHandles") : nullptr;
    const JValue* sizeValue = declared ? declared->Get("handleSize") : nullptr;
    const size_t handleSize = sizeValue ? (size_t)sizeValue->Uint() : 0;
    if (!handleSize || capturedSize % handleSize) {
        Problem("left out: the pipeline's shader group handle size was not recorded");
        return;
    }
    const uint32_t groups = (uint32_t)(capturedSize / handleSize);

    std::vector<uint8_t> replayHandles(capturedSize, 0);
    if (_fns.GetRayTracingShaderGroupHandlesKHR(_device, (VkPipeline)(uintptr_t)pipelineHandle, 0, groups,
                                                replayHandles.size(), replayHandles.data()) != VK_SUCCESS) {
        Problem("left out: this driver would not give the pipeline's shader group handles");
        return;
    }

    // One buffer for every region, each starting at the alignment the device asks for.
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &rt;
    if (_fns.GetPhysicalDeviceProperties2) _fns.GetPhysicalDeviceProperties2(_physical, &properties);
    const VkDeviceSize align = rt.shaderGroupBaseAlignment ? rt.shaderGroupBaseAlignment : 64;

    struct Placed { std::string region; VkDeviceSize at = 0; std::vector<uint8_t> bytes; };
    std::vector<Placed> placed;
    VkDeviceSize total = 0;
    uint32_t rewritten = 0, unmatched = 0;
    for (uint32_t i = 0; i < regions->count; ++i) {
        const JValue& e = regions->items[i];
        const JValue* nameValue = e.Get("region");
        const JValue* captureValue = e.Get("capture");
        if (!nameValue || !captureValue) continue;
        auto it = _bufferData.find(captureValue->Uint());
        if (it == _bufferData.end()) continue;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!_capture->Payload(it->second->Get("payload"), data, size) || !size) continue;

        const std::string name(nameValue->Str());
        const JValue* region = args.Get(name == "raygen" ? "pRaygenShaderBindingTable"
                                      : name == "miss" ? "pMissShaderBindingTable"
                                      : name == "hit" ? "pHitShaderBindingTable" : "pCallableShaderBindingTable");
        const JValue* strideValue = region ? region->Get("stride") : nullptr;
        const VkDeviceSize stride = strideValue ? strideValue->Uint() : 0;
        if (!stride) continue;

        Placed p;
        p.region = name;
        p.bytes.assign(data, data + size);
        // Each record's handle becomes this driver's for the group the captured handle named. The
        // bytes after it are the application's own shader record data and are copied as they were.
        for (size_t at = 0; at + handleSize <= p.bytes.size(); at += (size_t)stride) {
            uint32_t group = UINT32_MAX;
            for (uint32_t g = 0; g < groups && group == UINT32_MAX; ++g) {
                if (!memcmp(&p.bytes[at], capturedHandles + (size_t)g * handleSize, handleSize)) group = g;
            }
            if (group == UINT32_MAX) {
                ++unmatched;
                continue;   // a handle this pipeline never gave out; left as it was
            }
            memcpy(&p.bytes[at], &replayHandles[(size_t)group * handleSize], handleSize);
            ++rewritten;
        }
        p.at = (total + align - 1) & ~(align - 1);
        total = p.at + p.bytes.size();
        placed.push_back(std::move(p));
    }
    if (!rewritten) {
        Problem("left out: no record of the shader binding table named a group of this pipeline");
        return;
    }
    if (unmatched) {
        Problem("the shader binding table has " + std::to_string(unmatched)
                + " record(s) whose handle this pipeline never gave out; they are replayed as captured");
    }
    if (!EnsureBindingTable(total)) {
        Problem("left out: the shader binding table could not be allocated");
        return;
    }
    for (const Placed& p : placed) memcpy(_bindingTableMapped + p.at, p.bytes.data(), p.bytes.size());

    VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bi.buffer = _bindingTable;
    const VkDeviceAddress base = _fns.GetBufferDeviceAddress(_device, &bi);

    Args_vkCmdTraceRaysKHR a{};
    const size_t unresolvedBefore = _ctx.unresolved;
    DecodeArgs(_ctx, args, a);
    if (_ctx.unresolved != unresolvedBefore) {
        Problem("left out: the trace names objects the replay does not have");
        return;
    }
    VkStridedDeviceAddressRegionKHR raygen{}, miss{}, hit{}, callable{};
    auto fill = [&](const VkStridedDeviceAddressRegionKHR* source, const char* name, VkStridedDeviceAddressRegionKHR& out) {
        if (source) out = *source;
        out.deviceAddress = 0;
        for (const Placed& p : placed) {
            if (p.region == name) out.deviceAddress = base + p.at;
        }
    };
    fill(a.pRaygenShaderBindingTable, "raygen", raygen);
    fill(a.pMissShaderBindingTable, "miss", miss);
    fill(a.pHitShaderBindingTable, "hit", hit);
    fill(a.pCallableShaderBindingTable, "callable", callable);
    if (!raygen.deviceAddress) {
        Problem("left out: the trace's raygen table is not in this capture");
        return;
    }
    _fns.CmdTraceRaysKHR(cb, &raygen, &miss, &hit, &callable, a.width, a.height, a.depth);
}

/** The replay's own shader binding table memory, host visible so the records can be written into it. */
bool Replayer::EnsureBindingTable(VkDeviceSize size) {
    if (size <= _bindingTableSize && _bindingTable) return true;
    if (_bindingTable) {
        _fns.DestroyBuffer(_device, _bindingTable, nullptr);
        _fns.FreeMemory(_device, _bindingTableMemory, nullptr);
        _bindingTable = VK_NULL_HANDLE;
        _bindingTableMemory = VK_NULL_HANDLE;
        _bindingTableMapped = nullptr;
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (_fns.CreateBuffer(_device, &info, nullptr, &_bindingTable) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    _fns.GetBufferMemoryRequirements(_device, _bindingTable, &requirements);
    if (!AllocateBound(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       _bindingTableMemory, false, true)) return false;
    if (_fns.BindBufferMemory(_device, _bindingTable, _bindingTableMemory, 0) != VK_SUCCESS) return false;
    void* mapped = nullptr;
    if (_fns.MapMemory(_device, _bindingTableMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
    _bindingTableMapped = (uint8_t*)mapped;
    _bindingTableSize = size;
    return true;
}

void Replayer::IssueCommand(ReplayFn fn, const JValue& command, const JValue& args, VkCommandBuffer cb, uint32_t index) {
    const std::string m = Str(command.Get("method"));
    // Only the frame's own commands are exported, not an analysis issuing one of them again.
    Exporter* const exporter = index != UINT32_MAX ? _exporter.get() : nullptr;
    // Ray tracing is not replayed: its pipelines and acceleration structures are not made, and
    // these commands name device addresses of the captured process's buffers, which mean nothing here.
    static const std::unordered_set<std::string> kRayTracing = {
        "vkCmdTraceRaysKHR", "vkCmdTraceRaysIndirectKHR", "vkCmdTraceRaysIndirect2KHR", "vkCmdTraceRaysNV",
        "vkCmdBuildAccelerationStructuresKHR", "vkCmdBuildAccelerationStructuresIndirectKHR", "vkCmdBuildAccelerationStructureNV",
        "vkCmdCopyAccelerationStructureKHR", "vkCmdCopyAccelerationStructureToMemoryKHR", "vkCmdCopyMemoryToAccelerationStructureKHR",
        "vkCmdCopyAccelerationStructureNV", "vkCmdSetRayTracingPipelineStackSizeKHR",
    };
    // The trace needs to know which pipeline it runs, which only the bind before it says.
    if (m == "vkCmdBindPipeline") {
        const JValue* point = args.Get("pipelineBindPoint");
        if (point && point->Str().find("RAY_TRACING") != std::string_view::npos) {
            _boundRayTracingPipeline = IdOf(args.Get("pipeline"));
        }
    }
    if (m == "vkCmdBuildAccelerationStructuresKHR") {
        BuildAccelerationStructures(command, args, cb);
        // A build names what it reads by device address, which the replay finds again at run time
        // (its own buffers' addresses, scratch of its own): nothing the source can spell as constants.
        if (exporter) exporter->NotExported(index, m, "acceleration structure builds are not exported yet (their device addresses are found at run time)");
        return;
    }
    if (m == "vkCmdTraceRaysKHR") {
        TraceRays(command, args, cb);
        if (exporter) exporter->NotExported(index, m, "ray traces are not exported yet (the shader binding table is rebuilt at run time)");
        return;
    }
    if (kRayTracing.count(m)) {
        _ctx.Problem("left out: ray tracing is not replayed yet");
        if (exporter) exporter->LeftOut(index, m, "the replay does not issue it");
        return;
    }
    if (!StartsWith(m, "vkCmdPushDescriptorSetWithTemplate")) {
        fn(_ctx, args, cb);
        if (exporter) {
            exporter->Command(index, m, args, _exportCtx);
            _exportCtx.problems.clear();
        }
        return;
    }
    // A push through an update template passes its descriptors as a pointer to application memory
    // the arguments cannot carry; the layer snapshots what was pushed ("descriptors"), pushed here as
    // plain writes.
    const JValue* info = args.Get("pPushDescriptorSetWithTemplateInfo");
    const JValue& a = info ? *info : args;
    const JValue* descriptors = command.Get("descriptors");
    const JValue* sets = descriptors ? descriptors->Get("sets") : nullptr;
    if (!sets || !sets->IsArray() || !sets->count) {
        _ctx.Problem("left out: the capture has no snapshot of the descriptors it pushed");
        return;
    }
    const VkPipelineLayout layout = (VkPipelineLayout)Handle(IdOf(a.Get("layout")));
    if (!layout) {
        _ctx.Problem("left out: it names a pipeline layout the replay does not have");
        return;
    }
    const VkPipelineBindPoint bindPoint = (VkPipelineBindPoint)DecodeEnum_VkPipelineBindPoint(_ctx, descriptors->Get("bindPoint"));
    const uint32_t set = a.Get("set") ? (uint32_t)a.Get("set")->Uint() : 0;
    DescriptorWrites w;
    BuildDescriptorWrites(sets->items[0], VK_NULL_HANDLE, w);
    if (w.writes.empty()) return;
    if (_fns.CmdPushDescriptorSetKHR) _fns.CmdPushDescriptorSetKHR(cb, bindPoint, layout, set, (uint32_t)w.writes.size(), w.writes.data());
    else if (_fns.CmdPushDescriptorSet) _fns.CmdPushDescriptorSet(cb, bindPoint, layout, set, (uint32_t)w.writes.size(), w.writes.data());
    else _ctx.Problem("left out: push descriptors are not available on this device");
    if (exporter && (_fns.CmdPushDescriptorSetKHR || _fns.CmdPushDescriptorSet))
        exporter->PushDescriptors(index, bindPoint, layout, set, w.writes, _fns.CmdPushDescriptorSetKHR != nullptr);
}

void Replayer::BuildDescriptorWrites(const JValue& set, VkDescriptorSet handle, DescriptorWrites& out) {
    {
        std::vector<VkWriteDescriptorSet>& writes = out.writes;
        auto& bufferInfos = out.buffers;
        auto& imageInfos = out.images;
        auto& viewInfos = out.views;
        std::string& key = out.key;
        const JValue* bindings = set.Get("bindings");
        for (uint32_t b = 0; bindings && b < bindings->count; ++b) {
            const JValue& binding = bindings->items[b];
            const VkDescriptorType type = (VkDescriptorType)DecodeEnum_VkDescriptorType(_ctx, binding.Get("type"));
            const uint32_t bindingIndex = (uint32_t)binding.Get("binding")->Uint();
            const JValue* list = binding.Get("descriptors");
            // Consecutive written elements become one write; unwritten ones (null) split the runs.
            uint32_t k = 0;
            while (list && k < list->count) {
                while (k < list->count && list->items[k].IsNull()) ++k;
                if (k >= list->count) break;
                uint32_t start = k;
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = handle;
                w.dstBinding = bindingIndex;
                w.dstArrayElement = start;
                w.descriptorType = type;
                auto buffers = std::make_unique<std::vector<VkDescriptorBufferInfo>>();
                auto images = std::make_unique<std::vector<VkDescriptorImageInfo>>();
                auto views = std::make_unique<std::vector<VkBufferView>>();
                auto structures = std::make_unique<std::vector<VkAccelerationStructureKHR>>();
                for (; k < list->count && !list->items[k].IsNull(); ++k) {
                    const JValue& desc = list->items[k];
                    if (desc.Get("accelerationStructure")) {
                        structures->push_back((VkAccelerationStructureKHR)Handle(IdOf(desc.Get("accelerationStructure"))));
                        key += "a" + std::to_string(IdOf(desc.Get("accelerationStructure"))) + ";";
                    } else if (desc.Get("buffer")) {
                        VkDescriptorBufferInfo bi{};
                        bi.buffer = (VkBuffer)Handle(IdOf(desc.Get("buffer")));
                        bi.offset = desc.Get("offset") ? desc.Get("offset")->Uint() : 0;
                        bi.range = desc.Get("range") ? desc.Get("range")->Uint() : VK_WHOLE_SIZE;
                        buffers->push_back(bi);
                        key += "b" + std::to_string(IdOf(desc.Get("buffer"))) + ":" + std::to_string(bi.offset) + ":" + std::to_string(bi.range) + ";";
                    } else if (desc.Get("bufferView")) {
                        views->push_back((VkBufferView)Handle(IdOf(desc.Get("bufferView"))));
                        key += "v" + std::to_string(IdOf(desc.Get("bufferView"))) + ";";
                    } else {
                        VkDescriptorImageInfo ii{};
                        ii.imageView = (VkImageView)Handle(IdOf(desc.Get("imageView")));
                        ii.imageLayout = (VkImageLayout)DecodeEnum_VkImageLayout(_ctx, desc.Get("imageLayout"));
                        if (!desc.Get("immutable") || !desc.Get("immutable")->boolean) ii.sampler = (VkSampler)Handle(IdOf(desc.Get("sampler")));
                        images->push_back(ii);
                        key += "i" + std::to_string(IdOf(desc.Get("imageView"))) + ":" + std::to_string(ii.imageLayout) + ":" + std::to_string(IdOf(desc.Get("sampler"))) + ";";
                    }
                }
                w.descriptorCount = k - start;
                if (!buffers->empty()) w.pBufferInfo = buffers->data();
                if (!images->empty()) w.pImageInfo = images->data();
                if (!views->empty()) w.pTexelBufferView = views->data();
                if (!structures->empty()) {
                    // A structure the replay did not make leaves the binding out rather than writing a null.
                    if (std::find(structures->begin(), structures->end(), VK_NULL_HANDLE) != structures->end()) continue;
                    auto info = std::make_unique<VkWriteDescriptorSetAccelerationStructureKHR>();
                    *info = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
                    info->accelerationStructureCount = (uint32_t)structures->size();
                    info->pAccelerationStructures = structures->data();
                    w.pNext = info.get();
                    out.structureWrites.push_back(std::move(info));
                    out.structures.push_back(std::move(structures));
                }
                bufferInfos.push_back(std::move(buffers));
                imageInfos.push_back(std::move(images));
                viewInfos.push_back(std::move(views));
                writes.push_back(w);
                key += "|" + std::to_string(bindingIndex) + "@" + std::to_string(start) + "|";
            }
        }
    }
}

void Replayer::BeginPass(const JValue& command, uint32_t index, uint64_t commandBuffer) {
    (void)command;
    (void)index;
    (void)commandBuffer;
}


void Replayer::InjectStorageReadbacks(VkCommandBuffer cb, const CommandGroup& group, std::vector<PendingReadback>& readbacks) {
    const JValue* textures = _capture->Textures();
    if (!textures || !textures->IsArray()) return;
    for (uint32_t i = 0; i < textures->count; ++i) {
        const JValue& t = textures->items[i];
        const JValue* info = t.Get("info");
        // Only images a shader could have written, read back in this command buffer. A sampled image
        // the frame only read is uploaded and would compare equal to itself, which says nothing.
        if (!info || Str(info->Get("kind")) != "sampled") continue;
        if (info->Get("error")) continue;
        const JValue* owner = info->Get("commandBuffer");
        if (!owner || owner->Uint() != group.commandBuffer) continue;
        auto it = _images.find(info->Get("id")->Uint());
        if (it == _images.end() || !it->second.storage) continue;
        const ImageRecord& image = it->second;
        if (image.samples != VK_SAMPLE_COUNT_1_BIT) continue;   // a multisampled storage image is not a thing

        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        const uint32_t width = (uint32_t)info->Get("width")->Uint();
        const uint32_t height = (uint32_t)info->Get("height")->Uint();
        const uint8_t* captured = nullptr;
        size_t capturedSize = 0;
        if (!_capture->Payload(t.Get("payload"), captured, capturedSize) || !capturedSize) continue;

        TargetComparison cmp;
        cmp.image = info->Get("id")->Uint();
        cmp.commandBuffer = group.commandBuffer;
        cmp.frame = info->Get("frame") ? (uint32_t)info->Get("frame")->Uint() : 0;
        cmp.passIndex = UINT32_MAX;          // not a pass's target: what the frame computed into it
        cmp.attachment = UINT32_MAX;
        cmp.format = Str(info->Get("format"));
        cmp.aspect = "storage";
        cmp.width = width;
        cmp.height = height;

        PendingReadback pending;
        pending.target = _report->targets.size();
        pending.texture = &t;
        if (!CreateStaging(capturedSize, pending.staging)) {
            cmp.note = "no staging memory";
            _report->targets.push_back(cmp);
            continue;
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;   // a storage image is written in GENERAL and left there
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                0, nullptr, 1, &b);
        VkBufferImageCopy copy{};
        copy.imageExtent = {std::max(1u, width), std::max(1u, height), 1};
        copy.imageSubresource = {aspect, 0, 0, 1};
        _fns.CmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.staging.buffer, 1, &copy);
        std::swap(b.oldLayout, b.newLayout);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                                0, nullptr, 1, &b);
        if (_exporter)
            _exporter->Readback(image.image, "image" + std::to_string(cmp.image) + "_cb" + std::to_string(group.commandBuffer) + "_storage", aspect, 0, 0, 1,
                                {std::max(1u, width), std::max(1u, height)}, VK_IMAGE_LAYOUT_GENERAL, image.samples, image.format, captured, capturedSize,
                                /* shaderWritten */ true);
        _report->targets.push_back(cmp);
        readbacks.push_back(pending);
    }
}

void Replayer::InjectReadbacks(VkCommandBuffer cb, const PassState& pass, std::vector<PendingReadback>& readbacks, const char* skipReason) {
    const JValue* textures = _capture->Textures();
    if (!textures || !textures->IsArray()) return;
    for (uint32_t i = 0; i < textures->count; ++i) {
        const JValue& t = textures->items[i];
        const JValue* info = t.Get("info");
        // Render pass attachments only: sampled images and frame-start contents are uploaded, not compared.
        if (!info || (info->Get("kind") && Str(info->Get("kind")) != "attachment")) continue;
        if (info->Get("commandBuffer")->Uint() != pass.commandBuffer || info->Get("passIndex")->Uint() != pass.index ||
            info->Get("frame")->Uint() != pass.frame)
            continue;
        TargetComparison cmp;
        cmp.image = info->Get("id")->Uint();
        cmp.commandBuffer = pass.commandBuffer;
        cmp.frame = pass.frame;
        cmp.passIndex = pass.index;
        cmp.attachment = (uint32_t)info->Get("attachment")->Uint();
        cmp.format = Str(info->Get("format"));
        cmp.aspect = Str(info->Get("aspect"));
        cmp.width = (uint32_t)info->Get("width")->Uint();
        cmp.height = (uint32_t)info->Get("height")->Uint();
        const size_t target = _report->targets.size();
        auto skip = [&](std::string why) {
            cmp.note = std::move(why);
            _report->targets.push_back(cmp);
        };
        if (skipReason) { skip(skipReason); continue; }
        if (info->Get("error")) { skip("the capture's read-back failed"); continue; }
        const bool resolve = info->Get("resolve") && info->Get("resolve")->boolean;
        const std::vector<uint64_t>& views = resolve ? pass.resolveViews : pass.views;
        const std::vector<VkImageLayout>& layouts = resolve ? pass.resolveLayouts : pass.layouts;
        if (cmp.attachment >= views.size()) { skip("the replayed pass has no such attachment"); continue; }
        auto vit = _views.find(views[cmp.attachment]);
        if (vit == _views.end()) { skip("the attachment's view was not replayed"); continue; }
        auto iit = _images.find(vit->second.image);
        if (iit == _images.end()) { skip("the attachment's image was not replayed"); continue; }
        const ImageRecord& image = iit->second;
        const VkImageAspectFlags aspect = AspectOf(cmp.aspect);
        const VkDeviceSize size = info->Get("size")->Uint();
        const VkImageLayout layout = cmp.attachment < layouts.size() ? layouts[cmp.attachment] : VK_IMAGE_LAYOUT_UNDEFINED;
        const uint32_t mip = vit->second.range.baseMipLevel;
        const uint32_t baseLayer = vit->second.range.baseArrayLayer;
        const uint32_t layers = std::max<uint32_t>(1, (uint32_t)info->Get("layers")->Uint());
        if (image.samples != VK_SAMPLE_COUNT_1_BIT && layers > 1) { skip("multisampled layered targets are not compared yet"); continue; }
        PendingReadback pending;
        pending.target = target;
        pending.texture = &t;
        if (!CreateStaging(size, pending.staging)) { skip("no staging memory"); continue; }
        VkBufferImageCopy copy{};
        copy.imageExtent = {std::max(1u, image.extent.width >> mip), std::max(1u, image.extent.height >> mip), 1};
        if (_exporter) {
            const uint8_t* captured = nullptr;
            size_t capturedSize = 0;
            if (_capture->Payload(t.Get("payload"), captured, capturedSize) && capturedSize)
                _exporter->Readback(image.image,
                                    "image" + std::to_string(cmp.image) + "_cb" + std::to_string(cmp.commandBuffer) + "_pass" + std::to_string(cmp.passIndex) +
                                        "_att" + std::to_string(cmp.attachment) + (resolve ? "_resolve" : "") + (cmp.aspect == "color" ? "" : "_" + cmp.aspect),
                                    aspect, mip, baseLayer, layers, {copy.imageExtent.width, copy.imageExtent.height}, layout, image.samples, image.format,
                                    captured, capturedSize);
        }
        if (image.samples != VK_SAMPLE_COUNT_1_BIT) {
            // The capture read it back through a resolve (sample zero for depth), and so does the replay.
            std::string why;
            const VkImage resolved = ResolveTarget(cb, image, aspect, mip, baseLayer, layout, why);
            if (!resolved) {
                DestroyStaging(pending.staging);
                skip(why);
                continue;
            }
            copy.imageSubresource = {aspect, 0, 0, 1};
            _fns.CmdCopyImageToBuffer(cb, resolved, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.staging.buffer, 1, &copy);
        } else {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image.image;
            b.subresourceRange = {vkinsp::FormatAspects(image.format), mip, 1, baseLayer, layers};
            b.oldLayout = layout;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            copy.imageSubresource = {aspect, mip, baseLayer, layers};
            _fns.CmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.staging.buffer, 1, &copy);
            std::swap(b.oldLayout, b.newLayout);
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
        _report->targets.push_back(cmp);
        readbacks.push_back(pending);
    }
}

VkImage Replayer::ResolveTarget(VkCommandBuffer cb, const ImageRecord& image, VkImageAspectFlags aspect, uint32_t mip, uint32_t baseLayer,
                                VkImageLayout layout, std::string& why) {
    const VkExtent2D extent{std::max(1u, image.extent.width >> mip), std::max(1u, image.extent.height >> mip)};
    const VkImageAspectFlags aspects = vkinsp::FormatAspects(image.format);
    const VkImageSubresourceRange range{aspects, mip, 1, baseLayer, 1};
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        // Attachment usage only so the transient image may have its view.
        const TransientImage resolved = CreateTransientImage(image.format, extent,
                                                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!resolved.image) {
            why = "no memory to resolve the multisampled target into";
            return VK_NULL_HANDLE;
        }
        Barrier(cb, image.image, range, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Barrier(cb, resolved.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, baseLayer, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {extent.width, extent.height, 1};
        _fns.CmdResolveImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolved.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        Barrier(cb, image.image, range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
        Barrier(cb, resolved.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        return resolved.image;
    }

    // Depth: a subpass that resolves sample zero into a single-sampled attachment (VK_KHR_depth_stencil_resolve,
    // core in 1.2), which is the value the capture layer's depth resolve reads back.
    if (!_fns.CreateRenderPass2) {
        why = "resolving multisampled depth needs Vulkan 1.2 render passes";
        return VK_NULL_HANDLE;
    }
    VkRenderPass& rp = _depthResolvePasses[{image.format, image.samples}];
    if (!rp) {
        VkAttachmentDescription2 attachments[2]{};
        for (VkAttachmentDescription2& a : attachments) {
            a.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            a.format = image.format;
            a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        attachments[0].samples = image.samples;
        attachments[0].loadOp = attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference2 source{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspects};
        VkAttachmentReference2 target{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspects};
        VkSubpassDescriptionDepthStencilResolve resolve{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        resolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        resolve.stencilResolveMode = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        resolve.pDepthStencilResolveAttachment = &target;
        VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        subpass.pNext = &resolve;
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &source;
        VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        info.attachmentCount = 2;
        info.pAttachments = attachments;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        if (_fns.CreateRenderPass2(_device, &info, nullptr, &rp) == VK_SUCCESS) Track("VkRenderPass", (uint64_t)rp);
        else rp = VK_NULL_HANDLE;
    }
    const TransientImage resolved = CreateTransientImage(image.format, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView source = VK_NULL_HANDLE;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = range;
    if (resolved.image) _fns.CreateImageView(_device, &view, nullptr, &source);
    if (!rp || !resolved.view || !source) {
        if (source) _fns.DestroyImageView(_device, source, nullptr);
        why = "the multisampled depth target could not be resolved";
        return VK_NULL_HANDLE;
    }
    // Released with the pass's other transient objects once the submission has run.
    _transientImages.push_back({VK_NULL_HANDLE, source, VK_NULL_HANDLE});
    VkImageView views[2] = {source, resolved.view};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = rp;
    fb.attachmentCount = 2;
    fb.pAttachments = views;
    fb.width = extent.width;
    fb.height = extent.height;
    fb.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (_fns.CreateFramebuffer(_device, &fb, nullptr, &framebuffer) != VK_SUCCESS) {
        why = "the multisampled depth target could not be resolved";
        return VK_NULL_HANDLE;
    }
    _transientFramebuffers.push_back(framebuffer);
    Barrier(cb, image.image, range, layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = rp;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, extent};
    _fns.CmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    _fns.CmdEndRenderPass(cb);
    Barrier(cb, image.image, range, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, layout);
    return resolved.image;
}

void Replayer::CompareReadbacks(std::vector<PendingReadback>& readbacks) {
    for (PendingReadback& p : readbacks) {
        TargetComparison& cmp = _report->targets[p.target];
        const uint8_t* captured = nullptr;
        size_t capturedSize = 0;
        if (!_capture->Payload(p.texture->Get("payload"), captured, capturedSize)) {
            cmp.note = "the capture has no pixels for this target";
        } else {
            VkFormat format = (VkFormat)DecodeEnum_VkFormat(_ctx, p.texture->Get("info")->Get("format"));
            const VkImageAspectFlags aspect = AspectOf(cmp.aspect);
            const uint32_t texel = std::max<uint32_t>(1, vkinsp::FormatBlockInfo(format, aspect).bytes);
            const size_t size = std::min<size_t>(capturedSize, (size_t)p.staging.size);
            const auto* replayed = static_cast<const uint8_t*>(p.staging.mapped);
            // A 24-bit depth aspect is copied as 32 bits whose top byte is undefined.
            const bool d24 = aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32);
            const uint32_t compared = d24 ? 3 : texel;
            cmp.compared = true;
            cmp.bytes = size;
            cmp.texels = size / texel;
            for (size_t t = 0; t + texel <= size; t += texel) {
                bool differs = false;
                for (uint32_t k = 0; k < compared; ++k) {
                    const uint32_t delta = (uint32_t)std::abs((int)captured[t + k] - (int)replayed[t + k]);
                    if (delta) {
                        differs = true;
                        cmp.maxByteDelta = std::max(cmp.maxByteDelta, delta);
                    }
                }
                if (differs) cmp.differingTexels++;
            }
            if (capturedSize != p.staging.size) cmp.note = "sizes differ: captured " + std::to_string(capturedSize) + ", replayed " + std::to_string(p.staging.size);
            if (_options.keepPixels) {
                cmp.captured.assign(captured, captured + size);
                cmp.replayed.assign(replayed, replayed + size);
            }
        }
        DestroyStaging(p.staging);
    }
    readbacks.clear();
    _arena.Reset();
}

void Replayer::RecordSecondaries(size_t executeIndex, const JValue& execute, uint32_t frame, uint64_t commandBuffer, uint32_t passIndex) {
    // The capture inlines each secondary command buffer's recording after the vkCmdExecuteCommands
    // that ran it (commands with "secondary": its id): record them into the replay's secondaries.
    const JValue* commands = _capture->Commands();
    const JValue* list = execute.Get("args") ? execute.Get("args")->Get("pCommandBuffers") : nullptr;
    for (uint32_t s = 0; list && s < list->count; ++s) {
        const uint64_t id = IdOf(&list->items[s]);
        VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)Handle(id);
        if (!cb) continue;
        bool begun = false;
        StreamState stream;   // a secondary starts with nothing bound
        for (uint32_t i = (uint32_t)executeIndex + 1; i < commands->count; ++i) {
            const JValue& c = commands->items[i];
            const JValue* sec = c.Get("secondary");
            if (!sec) break;
            if (sec->Uint() != id) continue;
            const std::string m = Str(c.Get("method"));
            const JValue* args = c.Get("args");
            _ctx.where = "command " + std::to_string(i) + " " + m;
            if (_options.trace) {
                std::fprintf(stderr, "command %u %s (secondary %llu)\n", i, m.c_str(), (unsigned long long)id);
                std::fflush(stderr);
            }
            if (m == "vkBeginCommandBuffer") {
                Args_vkBeginCommandBuffer a{};
                if (args) DecodeArgs(_ctx, *args, a);
                _fns.BeginCommandBuffer(cb, a.pBeginInfo);
                if (_exporter && a.pBeginInfo) _exporter->BeginCommandBuffer(id, cb, *a.pBeginInfo, i, i, true);
                begun = true;
            } else if (m == "vkEndCommandBuffer") {
                if (begun) _fns.EndCommandBuffer(cb);
                if (begun && _exporter) _exporter->EndCommandBuffer(cb);
                begun = false;
            } else if (ReplayFn fn = FindReplayCommand(m); fn && args && begun) {
                ApplyDescriptorSnapshot(c.Get("descriptors"));
                if (_options.history.enabled) NoteHistoryBindings(c.Get("descriptors"));
                NoteStreamCommand(stream, m, *args, i);
                if (_options.ablation.enabled && IsAction(m)) IssueAblation(cb, i, m, *args, frame, commandBuffer, passIndex, stream);
                // Per-draw timing and counters: an engine that records its draws into secondaries
                // (a Unity player records every one) has them measured here rather than above.
                const bool measure = _options.drawStats && _drawQueryCapacity && IsAction(m);
                const int drawSlot = measure ? BeginDrawQuery(cb, i, frame, commandBuffer, passIndex) : -1;
                // Hardware counters: a draw's range, here too (a Unity player records every draw in a secondary).
                const bool countDraw = _options.counters.enabled && _hw && IsAction(m);
                const int counterRange = countDraw ? BeginCounterDraw(cb, i, frame, commandBuffer, passIndex) : -1;
                IssueCommand(fn, c, *args, cb, i);
                if (countDraw) EndCounterDraw(cb, counterRange);
                if (drawSlot >= 0) EndDrawQuery(cb, drawSlot);
                if (StartsWith(m, "vkCmdBeginQuery")) ++_appQueryDepth;
                else if (StartsWith(m, "vkCmdEndQuery") && _appQueryDepth) --_appQueryDepth;
                _report->commandsRecorded++;
            }
            _arena.Reset();
        }
        if (begun) _fns.EndCommandBuffer(cb);
        if (begun && _exporter) _exporter->EndCommandBuffer(cb);
    }
}

void Replayer::RecordGroup(CommandGroup& group, std::vector<PendingReadback>& readbacks, std::vector<PendingOverdraw>& overdraws,
                           std::vector<PendingHistory>& histories) {
    const JValue* commands = _capture->Commands();
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)Handle(group.commandBuffer);
    group.used = true;
    if (!cb) {
        Problem("command buffer " + std::to_string(group.commandBuffer) + " was not replayed");
        return;
    }
    ApplyBufferData(group);
    // The pixel history's staging for this command buffer's writes outside its render passes is
    // made when the first of them is met (history.cpp).
    _historyDirect = -1;
    // What a command buffer binds is its own: the next one starts from nothing bound.
    _historyStorageBinds.clear();
    const JValue& beginCommand = commands->items[group.first];
    _ctx.where = "command " + std::to_string(group.first) + " vkBeginCommandBuffer";
    Args_vkBeginCommandBuffer begin{};
    if (const JValue* args = beginCommand.Get("args")) DecodeArgs(_ctx, *args, begin);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (begin.pBeginInfo) beginInfo = *begin.pBeginInfo;
    _fns.ResetCommandBuffer(cb, 0);
    _fns.BeginCommandBuffer(cb, &beginInfo);
    if (_exporter) _exporter->BeginCommandBuffer(group.commandBuffer, cb, beginInfo, group.first, group.last, false);
    // The submission's first command buffer resets the pools its actions write into.
    if (_options.drawStats && _drawQueryCapacity && _drawSlot == 0) ResetDrawQueries(cb);
    if (_options.ablation.enabled) ResetAblationQueries(cb, group);
    _arena.Reset();

    PassState pass;
    StreamState stream;
    uint32_t passCount = 0;
    // Set when a pass's begin was left out (it names objects the replay does not have): the
    // commands inside the pass are invalid without it and are left out up to its end.
    bool skippingPass = false;
    const uint32_t frame = beginCommand.Get("frame") ? (uint32_t)beginCommand.Get("frame")->Uint() : 0;
    for (uint32_t i = group.first + 1; i < group.last; ++i) {
        const JValue& c = commands->items[i];
        if (c.Get("secondary")) continue;
        const std::string m = Str(c.Get("method"));
        const JValue* args = c.Get("args");
        _ctx.where = "command " + std::to_string(i) + " " + m;
        if (_options.trace) {
            std::fprintf(stderr, "command %u %s%s\n", i, m.c_str(), skippingPass ? " (left out with its pass)" : "");
            std::fflush(stderr);
        }
        if (!args) continue;
        if (skippingPass) {
            if (_exporter) _exporter->LeftOut(i, m, "with its pass, whose begin names objects the replay does not have");
            if (IsEndPass(m)) {
                if (_options.compareTargets) InjectReadbacks(cb, pass, readbacks, "the replay left this pass out");
                pass.active = false;
                skippingPass = false;
            }
            continue;
        }
        const size_t unresolvedBefore = _ctx.unresolved;

        if (IsBeginRenderPass(m)) {
            pass = PassState{};
            pass.active = true;
            pass.index = passCount++;
            pass.frame = frame;
            pass.commandBuffer = group.commandBuffer;
            pass.beginIndex = i;
            const JValue* beginInfo2 = args->Get("pRenderPassBegin");
            const uint64_t rp = beginInfo2 ? IdOf(beginInfo2->Get("renderPass")) : 0;
            const uint64_t fb = beginInfo2 ? IdOf(beginInfo2->Get("framebuffer")) : 0;
            auto fit = _framebufferViews.find(fb);
            if (fit != _framebufferViews.end()) pass.views = fit->second;
            if (auto eit = _framebufferExtents.find(fb); eit != _framebufferExtents.end()) pass.extent = eit->second;
            pass.renderPass = rp;
            auto rit = _renderPasses.find(rp);
            _passViews = rit != _renderPasses.end() ? rit->second.views : 1;
            if (rit != _renderPasses.end()) {
                pass.layouts = rit->second.finalLayouts;
                pass.formats = rit->second.formats;
                pass.loadOps = rit->second.loadOps;
                pass.startLayouts = rit->second.initialLayouts;
                const int d = rit->second.depthAttachment;
                if (d >= 0 && (size_t)d < pass.views.size()) {
                    auto vit = _views.find(pass.views[d]);
                    if (vit != _views.end()) {
                        pass.depthImage = vit->second.image;
                        pass.depthRange = vit->second.range;
                        pass.depthFormat = rit->second.formats[d];
                        pass.depthLoadOp = rit->second.loadOps[d];
                        pass.depthLayoutBefore = rit->second.initialLayouts[d];
                        const JValue* clears = beginInfo2 ? beginInfo2->Get("pClearValues") : nullptr;
                        if (clears && clears->IsArray() && (uint32_t)d < clears->count) {
                            if (const JValue* ds = clears->items[d].Get("depthStencil")) {
                                pass.depthClear.depth = (float)(ds->Get("depth") ? ds->Get("depth")->Double() : 1.0);
                                pass.depthClear.stencil = ds->Get("stencil") ? (uint32_t)ds->Get("stencil")->Uint() : 0;
                            }
                        }
                    }
                }
            }
            if (const JValue* clears = beginInfo2 ? beginInfo2->Get("pClearValues") : nullptr; clears && clears->IsArray())
                for (uint32_t k = 0; k < clears->count; ++k) pass.clearValues.push_back(ClearValueOf(clears->items[k]));
            // What the pass starts from is copied before it begins; not for a pass the replay leaves out.
            const bool overlay = PassHoldsAny(i, _options.overlay.commands);
            if ((_options.overdraw || overlay || _options.history.enabled) && ArgsResolve(m, *args)) {
                if (_options.overdraw || overlay) PrepareOverdraw(cb, pass);
                if (_options.history.enabled) PrepareHistory(cb, pass, histories);
            }
        } else if (IsBeginRendering(m)) {
            // Dynamic rendering: store every attachment (the recorder cannot), and note the attachments.
            pass = PassState{};
            pass.active = true;
            pass.index = passCount++;
            pass.frame = frame;
            pass.commandBuffer = group.commandBuffer;
            pass.beginIndex = i;
            Args_vkCmdBeginRendering a{};
            DecodeArgs(_ctx, *args, a);
            if (_ctx.unresolved != unresolvedBefore) {
                Problem("command " + std::to_string(i) + ": the pass was left out, with the commands inside it (it names objects the replay does not have)");
                if (_exporter) _exporter->LeftOut(i, m, "it names objects the replay does not have");
                skippingPass = true;
            } else if (a.pRenderingInfo) {
                VkRenderingInfo info = *a.pRenderingInfo;
                _passViews = ViewCount(info.viewMask);
                std::vector<VkRenderingAttachmentInfo> colors(info.pColorAttachments, info.pColorAttachments + info.colorAttachmentCount);
                VkRenderingAttachmentInfo depth{}, stencil{};
                const JValue* ri = args->Get("pRenderingInfo");
                // Notes an attachment: its view, layout, format, load and clear; its index, -1 without a view.
                auto note = [&](const VkRenderingAttachmentInfo& att, const JValue* json) -> int {
                    if (!att.imageView) return -1;
                    const uint64_t viewId = IdOf(json ? json->Get("imageView") : nullptr);
                    auto vit = _views.find(viewId);
                    auto iit = vit != _views.end() ? _images.find(vit->second.image) : _images.end();
                    pass.views.push_back(viewId);
                    pass.layouts.push_back(att.imageLayout);
                    pass.resolveViews.push_back(att.resolveMode != VK_RESOLVE_MODE_NONE ? IdOf(json ? json->Get("resolveImageView") : nullptr) : 0);
                    pass.resolveLayouts.push_back(att.resolveImageLayout);
                    pass.formats.push_back(iit != _images.end() ? iit->second.format : VK_FORMAT_UNDEFINED);
                    pass.loadOps.push_back(att.loadOp);
                    pass.startLayouts.push_back(att.imageLayout);
                    pass.clearValues.push_back(att.clearValue);
                    return (int)pass.views.size() - 1;
                };
                const JValue* colorJson = ri ? ri->Get("pColorAttachments") : nullptr;
                for (size_t k = 0; k < colors.size(); ++k) {
                    colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    pass.dynamicColorSlots.push_back(note(colors[k], colorJson && k < colorJson->count ? &colorJson->items[k] : nullptr));
                }
                info.pColorAttachments = colors.data();
                if (info.pDepthAttachment) {
                    depth = *info.pDepthAttachment;
                    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    info.pDepthAttachment = &depth;
                    pass.dynamicDepth = note(depth, ri ? ri->Get("pDepthAttachment") : nullptr);
                }
                if (info.pStencilAttachment) {
                    stencil = *info.pStencilAttachment;
                    stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    info.pStencilAttachment = &stencil;
                    if (!info.pDepthAttachment || stencil.imageView != depth.imageView) pass.dynamicStencil = note(stencil, ri ? ri->Get("pStencilAttachment") : nullptr);
                    else pass.dynamicStencil = pass.dynamicDepth;
                }
                const bool overlay = PassHoldsAny(i, _options.overlay.commands);
                if (_options.overdraw || overlay || PassHoldsAny(i, _options.mesh.commands) || _options.history.enabled) {
                    pass.extent = {(uint32_t)std::max(0, info.renderArea.offset.x) + info.renderArea.extent.width,
                                   (uint32_t)std::max(0, info.renderArea.offset.y) + info.renderArea.extent.height};
                    const JValue* depthJson = ri ? ri->Get("pDepthAttachment") : nullptr;
                    if (info.pDepthAttachment && info.pDepthAttachment->imageView && depthJson) {
                        auto vit = _views.find(IdOf(depthJson->Get("imageView")));
                        auto iit = vit != _views.end() ? _images.find(vit->second.image) : _images.end();
                        if (iit != _images.end()) {
                            pass.depthImage = vit->second.image;
                            pass.depthRange = vit->second.range;
                            pass.depthFormat = iit->second.format;
                            pass.depthLoadOp = info.pDepthAttachment->loadOp;
                            pass.depthLayoutBefore = info.pDepthAttachment->imageLayout;
                            pass.depthClear = info.pDepthAttachment->clearValue.depthStencil;
                        }
                    }
                    if (_options.overdraw || overlay) PrepareOverdraw(cb, pass);
                    if (_options.history.enabled) PrepareHistory(cb, pass, histories);
                }
                // Hardware counters: the pass's range wraps its draws (hw_counters.cpp); pushed before
                // the pass begins so it stays outside the render pass instance.
                if (_options.counters.enabled && _hw) BeginCounterPass(cb, pass);
                _fns.CmdBeginRendering(cb, &info);
                if (_exporter) _exporter->CmdBeginRendering(i, info);
                _report->commandsRecorded++;
            }
            _arena.Reset();
            continue;
        }

        if (m == "vkCmdBindDescriptorSets" || m == "vkCmdBindDescriptorSets2" || m == "vkCmdBindDescriptorSets2KHR")
            ApplyDescriptorSnapshot(c.Get("descriptors"));
        // Which bound set holds the followed pixel's image to be written (history.cpp).
        if (_options.history.enabled) NoteHistoryBindings(c.Get("descriptors"));
        if (m == "vkCmdExecuteCommands") RecordSecondaries(i, c, frame, group.commandBuffer, pass.active ? pass.index : UINT32_MAX);

        ReplayFn fn = FindReplayCommand(m);
        if (!fn) {
            Problem("command " + std::to_string(i) + ": " + m + " is not replayed");
            if (_exporter) _exporter->LeftOut(i, m, "the replay does not record it");
            continue;
        }
        NoteStreamCommand(stream, m, *args, i);
        // Hardware counters: this render pass's range is pushed before its begin command runs, so it
        // stays outside the render pass instance (hw_counters.cpp).
        if (_options.counters.enabled && _hw && IsBeginRenderPass(m) && pass.active) BeginCounterPass(cb, pass);
        if (_options.ablation.enabled && IsAction(m) && _ctx.unresolved == unresolvedBefore && ArgsResolve(m, *args))
            IssueAblation(cb, i, m, *args, frame, group.commandBuffer, pass.active ? pass.index : UINT32_MAX, stream);
        // Per-draw timing and counters: the action is issued between the queries (draw_stats.cpp).
        const bool measure = _options.drawStats && _drawQueryCapacity && IsAction(m);
        const int drawSlot = measure ? BeginDrawQuery(cb, i, frame, group.commandBuffer, pass.active ? pass.index : UINT32_MAX) : -1;
        // Hardware counters: each draw's range, nested inside its pass's (hw_counters.cpp).
        const bool countDraw = _options.counters.enabled && _hw && IsAction(m);
        const int counterRange = countDraw ? BeginCounterDraw(cb, i, frame, group.commandBuffer, pass.active ? pass.index : UINT32_MAX) : -1;
        // Pixel history outside the render passes: what a clear, a copy, a blit, a resolve or a
        // dispatch did to the pixel, read straight out of the image once the command has run
        // (history.cpp). A watched command is read before it as well, since nothing but the value
        // says whether it wrote the pixel at all.
        const DirectWrite direct = _options.history.enabled && !pass.active && _ctx.unresolved == unresolvedBefore
            ? HistoryDirectWrite(m, *args) : DirectWrite{};
        IssueCommand(fn, c, *args, cb, i);
        if (direct.writes) HistoryDirectPixel(cb, group, direct, histories, i, m, frame);
        if (countDraw) EndCounterDraw(cb, counterRange);
        if (drawSlot >= 0) EndDrawQuery(cb, drawSlot);
        // The capture's own queries: a statistics query of ours must not begin inside one.
        if (StartsWith(m, "vkCmdBeginQuery")) ++_appQueryDepth;
        else if (StartsWith(m, "vkCmdEndQuery") && _appQueryDepth) --_appQueryDepth;
        _arena.Reset();
        if (_ctx.unresolved != unresolvedBefore) {
            if (IsBeginRenderPass(m)) {
                Problem("command " + std::to_string(i) + ": the pass was left out, with the commands inside it (it names objects the replay does not have)");
                skippingPass = true;
            }
            continue;
        }
        _report->commandsRecorded++;

        if (IsEndPass(m) && pass.active) {
            // Hardware counters: the pass's range closes after its end command, outside the render pass.
            if (_options.counters.enabled && _hw) EndCounterPass(cb);
            if (_options.compareTargets) InjectReadbacks(cb, pass, readbacks);
            // Before the overdraw, which draws into the copy of the pass's starting depth the overlays copy from.
            if (_options.overlay.enabled && pass.extent.width) RecordOverlay(cb, group, pass, i);
            if (_options.mesh.enabled && pass.extent.width) RecordMesh(cb, group, pass, i);
            if (_options.overdraw) RecordOverdraw(cb, group, pass, i, overdraws);
            if (_options.history.enabled) RecordHistory(cb, group, pass, i, histories);
            pass.active = false;
            _passViews = 1;
        }
    }
    // What the frame computed into an image rather than drew into a target: a trace or a
    // dispatch writes a storage image, which is no pass's attachment and so is compared here.
    if (_options.compareTargets) InjectStorageReadbacks(cb, group, readbacks);
    _fns.EndCommandBuffer(cb);
    if (_exporter) _exporter->EndCommandBuffer(cb);
}

void Replayer::ReplayCommands() {
    const JValue* commands = _capture->Commands();
    if (!commands || !commands->IsArray()) return;
    BuildGroups();
    for (uint32_t i = 0; i < commands->count; ++i) {
        const JValue& c = commands->items[i];
        const std::string m = Str(c.Get("method"));
        if (m != "vkQueueSubmit" && m != "vkQueueSubmit2" && m != "vkQueueSubmit2KHR") continue;
        const JValue* args = c.Get("args");
        if (!args) continue;
        VkQueue queue = (VkQueue)(uintptr_t)Handle(IdOf(args->Get("queue")));
        if (!queue) queue = _queue;
        // Hardware counters: every submission goes to the profiler's queue, the one its session is on.
        if (_options.counters.enabled && _hw) queue = _queue;
        std::vector<PendingReadback> readbacks;
        std::vector<PendingOverdraw> overdraws;
        std::vector<PendingHistory> histories;
        std::vector<VkCommandBuffer> cbs;
        if (_exporter) _exporter->BeginSubmission(i, m);
        _drawSlot = 0;
        _pendingDraws.clear();
        _pendingDrawSlots.clear();
        // The submission before this one has been waited on, so its builds have finished with the
        // scratch they were given and the next frame's can start from the beginning of it again.
        _scratchUsed = 0;
        const JValue* submits = args->Get("pSubmits");
        for (uint32_t s = 0; submits && s < submits->count; ++s) {
            const JValue& submit = submits->items[s];
            std::vector<uint64_t> ids;
            if (m == "vkQueueSubmit") {
                if (const JValue* list = submit.Get("pCommandBuffers"))
                    for (uint32_t k = 0; k < list->count; ++k) ids.push_back(IdOf(&list->items[k]));
            } else if (const JValue* list = submit.Get("pCommandBufferInfos")) {
                for (uint32_t k = 0; k < list->count; ++k) ids.push_back(IdOf(list->items[k].Get("commandBuffer")));
            }
            for (uint64_t id : ids) {
                auto group = std::find_if(_groups.begin(), _groups.end(), [&](const CommandGroup& g) { return g.commandBuffer == id && !g.used && g.first > i; });
                if (group == _groups.end()) {
                    Problem("submission " + std::to_string(i) + ": command buffer " + std::to_string(id) + " has no recording in the capture (recorded before it started?)");
                    continue;
                }
                RecordGroup(*group, readbacks, overdraws, histories);
                if (VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)Handle(id)) cbs.push_back(cb);
            }
        }
        if (cbs.empty()) {
            CompleteHistory(histories);
            CompleteDrawStats(false);
            CompleteAblation(false);
            CompleteOverlay(false);
            CompleteMesh(false);
            ReleaseTransients();
            continue;
        }
        // One submission for the call's command buffers, without the application's semaphores and fence.
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        // Hardware counters, KHR path: the counter pass index for this round is chained in (hw_counters.cpp).
        info.pNext = _submitNext;
        info.commandBufferCount = (uint32_t)cbs.size();
        info.pCommandBuffers = cbs.data();
        if (_exporter) _exporter->Submit(queue, cbs);
        VkResult r = _fns.QueueSubmit(queue, 1, &info, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) {
            Problem("submission " + std::to_string(i) + ": vkQueueSubmit failed (" + std::to_string(r) + ")");
        } else {
            _fns.QueueWaitIdle(queue);
            _report->submissions++;
        }
        CompleteDrawStats(r == VK_SUCCESS);
        CompleteAblation(r == VK_SUCCESS);
        CompareReadbacks(readbacks);
        CompleteHistory(histories);
        CompleteOverlay(r == VK_SUCCESS);
        CompleteMesh(r == VK_SUCCESS);
        CompleteOverdraw(overdraws);
    }
    for (const CommandGroup& g : _groups)
        if (!g.used) Problem("command buffer " + std::to_string(g.commandBuffer) + " was recorded but its submission is not in the capture");
}

// ---------------------------------------------------------------------------------------------

bool Replayer::Run(const CaptureFile& capture, const ReplayOptions& options, ReplayReport& report) {
    if (!Setup(capture, options, report)) return false;
    // One frame, into the same report (which starts from Setup's).
    RunFrame(options, report);
    return true;
}

bool Replayer::Setup(const CaptureFile& capture, const ReplayOptions& options, ReplayReport& report) {
    _capture = &capture;
    _options = options;
    _setupOptions = options;
    _report = &report;
    if (!options.exportDir.empty()) {
        report.exported.requested = true;
        report.exported.directory = options.exportDir;
        _exporter = std::make_unique<Exporter>(options.exportDir, capture);
        if (!_exporter->Open(report.exported.error)) {
            Problem("export to C++: " + report.exported.error);
            _exporter.reset();
        }
    }
    if (!LoadVulkan() || !CreateInstance() || !CreateDevice()) {
        for (auto& p : _ctx.problems) report.problems.push_back(p);
        _ctx.problems.clear();
        return false;
    }
    if (const JValue* buffers = capture.Buffers(); buffers && buffers->IsArray())
        for (uint32_t i = 0; i < buffers->count; ++i)
            if (const JValue* info = buffers->items[i].Get("info")) _bufferData[info->Get("id")->Uint()] = &buffers->items[i];
    CreateObjects();
    ComputeInitialLayouts();
    for (auto& p : _ctx.problems) report.problems.push_back(p);
    _ctx.problems.clear();
    _setupReport = report;
    _setupDone = true;
    return true;
}

void Replayer::RunFrame(const ReplayOptions& requested, ReplayReport& report) {
    if (!_setupDone) return;
    const CaptureFile& capture = *_capture;
    // The analysis is the request's; what the device was created with stays Setup's.
    ReplayOptions options = requested;
    options.validation = _setupOptions.validation;
    options.allFeatures = _setupOptions.allFeatures;
    _options = options;
    if (&report != &_setupReport) report = _setupReport;
    _report = &report;
    if (_frameRun) ResetFrameState();
    _frameRun = true;

    if (options.history.enabled) {
        report.history.requested = true;
        report.history.image = options.history.image;
        report.history.x = options.history.x;
        report.history.y = options.history.y;
        report.history.mip = options.history.mip;
        report.history.layer = options.history.layer;
    }

    UploadImageContents();
    TransitionToInitialLayouts();

    // Hardware counters are their own analysis: the frame is replayed once per collection pass the
    // counters need, and nothing else runs (hw_counters.cpp).
    if (options.counters.enabled) {
        _options.compareTargets = false;   // the counter ranges are all this replay does
        if (PrepareCounters()) {
            if (options.counters.list) {
                ListCounters();
            } else {
                // Replays the backend may need before it has decoded every collection pass; it stops
                // the loop itself as soon as it has (EndCounterRound), so this is only a bound.
                const uint32_t rounds = std::max(2u, CounterRounds());
                // Each round re-uploads the frame's contents, so a large capture takes a while and
                // the report is only printed at the end: say where it is on the way.
                std::fprintf(stderr, "hardware counters: up to %u replays of the frame\n", rounds);
                for (uint32_t round = 0; round < rounds; ++round) {
                    if (round > 0) {
                        ResetFrameState();
                        UploadImageContents();
                        TransitionToInitialLayouts();
                    }
                    _hwRound = round;
                    if (!BeginCounterRound()) break;
                    std::fprintf(stderr, "  replay %u of at most %u\n", round + 1, rounds);
                    std::fflush(stderr);
                    ReplayCommands();
                    if (!EndCounterRound()) break;
                }
                CompleteCounters();
            }
        }
        DestroyCounters();
        for (auto& p : _ctx.problems) report.problems.push_back(p);
        _ctx.problems.clear();
        return;
    }

    if (options.drawStats) PrepareDrawStats();
    const bool ablating = options.ablation.enabled && PrepareAblation();
    ReplayCommands();
    DestroyDrawStats();
    DestroyAblation();
    // A target the frame did not reach (or a device that cannot time it) still gets an answer.
    for (const auto& target : options.ablation.targets) {
        if (std::any_of(report.ablations.begin(), report.ablations.end(), [&](const AblationResult& a) { return a.command == target.command; })) continue;
        AblationResult missing;
        missing.command = target.command;
        missing.stage = target.stage;
        for (const auto& v : target.variants) missing.variants.push_back({v.name});
        const JValue* commands = capture.Commands();
        missing.note = !ablating ? "the replay's queue writes no timestamps"
                     : !commands || target.command >= commands->count ? "the capture has no command " + std::to_string(target.command)
                     : "command " + std::to_string(target.command) + " was not replayed";
        report.ablations.push_back(std::move(missing));
    }
    if (options.history.enabled && !_historyPasses && !_historyWrites) {
        report.history.notes.push_back("nothing the replay ran writes image " + std::to_string(options.history.image) + " at mip " +
                                       std::to_string(options.history.mip) + ", layer " + std::to_string(options.history.layer) +
                                       ": no render pass renders to it, and no clear, copy, blit, resolve or dispatch touched that pixel of it");
    }
    // A draw asked for that no replayed pass holds still gets an answer.
    for (uint32_t command : options.overlay.commands) {
        if (std::any_of(report.overlays.begin(), report.overlays.end(), [&](const OverlayResult& o) { return o.command == command; })) continue;
        OverlayResult missing;
        missing.command = command;
        const JValue* commands = capture.Commands();
        if (commands && command < commands->count) missing.method = Str(commands->items[command].Get("method"));
        missing.note = !commands || command >= commands->count ? "the capture has no command " + std::to_string(command)
                     : "command " + std::to_string(command) + " is not in a render pass the replay drew";
        report.overlays.push_back(std::move(missing));
    }
    for (uint32_t command : options.mesh.commands) {
        if (std::any_of(report.meshes.begin(), report.meshes.end(), [&](const MeshResult& m) { return m.command == command; })) continue;
        MeshResult missing;
        missing.command = command;
        const JValue* commands = capture.Commands();
        if (commands && command < commands->count) missing.method = Str(commands->items[command].Get("method"));
        missing.note = !commands || command >= commands->count ? "the capture has no command " + std::to_string(command)
                     : "command " + std::to_string(command) + " is not in a render pass the replay drew";
        report.meshes.push_back(std::move(missing));
    }
    for (auto& p : _ctx.problems) report.problems.push_back(p);
    _ctx.problems.clear();
    // Export to C++: the frame is the last thing the project holds. The objects were exported as
    // Setup created them, so one project is written per Setup and a later frame exports nothing.
    if (_exporter) {
        if (!_exporter->Finish(report, report.exported)) Problem("export to C++: " + report.exported.error);
        _exporter.reset();
    }
}

void Replayer::ResetFrameState() {
    // The frame's command buffers are recorded again: their pools let go of the last recording.
    for (const Created& c : _created)
        if (c.type == "VkCommandPool") _fns.ResetCommandPool(_device, (VkCommandPool)c.handle, 0);
    // Images start where a fresh device's do: contents cleared (new memory reads as zero), layout undefined
    // until the uploads and the initial layouts put them where the frame expects them.
    RunOneTime([&](VkCommandBuffer cb) {
        for (auto& [id, image] : _images) {
            std::fill(image.layouts.begin(), image.layouts.end(), VK_IMAGE_LAYOUT_UNDEFINED);
            TransitionAll(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkImageAspectFlags aspects = vkinsp::FormatAspects(image.format);
            const VkImageSubresourceRange range{aspects, 0, image.mips, 0, image.layers};
            if (aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
                const VkClearDepthStencilValue zero{0.0f, 0};
                _fns.CmdClearDepthStencilImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            } else if (vkinsp::FormatBlockInfo(image.format, VK_IMAGE_ASPECT_COLOR_BIT).width == 1) {
                // Block-compressed images cannot be cleared; they are sampled, and uploaded again.
                const VkClearColorValue zero{};
                _fns.CmdClearColorImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            }
        }
    });
    _groups.clear();
    _historyPasses = 0;
    _historyWrites = 0;
    _historyDirect = -1;
    _historyStorageBinds.clear();
    _appQueryDepth = 0;
    _passViews = 1;
    _overlayTarget = UINT32_MAX;
    _meshTarget = nullptr;
}

} // namespace vkreplay
