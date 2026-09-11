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

#include "format_info.h"
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
    auto* report = static_cast<ReplayReport*>(user);
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
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
    if (!extensions.empty() && _fns.CreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        m.pfnUserCallback = DebugCallback;
        m.pUserData = _report;
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

    VkResult r = _fns.CreateDevice(_physical, &info, nullptr, &_device);
    if (r != VK_SUCCESS && (info.pNext || info.pEnabledFeatures)) {
        Problem("vkCreateDevice with the captured features failed (" + std::to_string(r) + "); retrying without them");
        info.pNext = nullptr;
        info.pEnabledFeatures = nullptr;
        r = _fns.CreateDevice(_physical, &info, nullptr, &_device);
    }
    _arena.Reset();
    if (r != VK_SUCCESS) {
        Problem("vkCreateDevice failed (" + std::to_string(r) + ")");
        return false;
    }
    auto gdpa = (PFN_vkGetDeviceProcAddr)_fns.GetInstanceProcAddr(_instance, "vkGetDeviceProcAddr");
    LoadDeviceFunctions(_fns, _device, gdpa);
    _queueFamily = queues[0].queueFamilyIndex;
    _fns.GetDeviceQueue(_device, _queueFamily, 0, &_queue);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = _queueFamily;
    _fns.CreateCommandPool(_device, &pool, nullptr, &_utilityPool);

    if (deviceObject) {
        _handles[IdOf(deviceArgs ? deviceArgs->Get("physicalDevice") : nullptr)] = (uint64_t)(uintptr_t)_physical;
        if (const JValue* id = deviceObject->Get("id")) _handles[id->Uint()] = (uint64_t)(uintptr_t)_device;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Memory and one-time submissions

bool Replayer::AllocateBound(VkMemoryRequirements requirements, VkMemoryPropertyFlags want, VkDeviceMemory& memory, bool track) {
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < _memoryProperties.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i))) continue;
            VkMemoryPropertyFlags flags = _memoryProperties.memoryTypes[i].propertyFlags;
            if (pass == 0 && (flags & want) != want) continue;
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
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

bool Replayer::CreateStaging(VkDeviceSize size, Staging& staging) {
    staging = Staging{};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(size, 1);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (_fns.CreateBuffer(_device, &info, nullptr, &staging.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    _fns.GetBufferMemoryRequirements(_device, staging.buffer, &req);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool ok = false;
    for (uint32_t i = 0; i < _memoryProperties.memoryTypeCount && !ok; ++i) {
        if (!(req.memoryTypeBits & (1u << i)) || (_memoryProperties.memoryTypes[i].propertyFlags & want) != want) continue;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = i;
        ok = _fns.AllocateMemory(_device, &alloc, nullptr, &memory) == VK_SUCCESS;
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

void Replayer::Transition(VkCommandBuffer cb, const ImageRecord& image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.image;
    b.subresourceRange = {vkinsp::FormatAspects(image.format), 0, image.mips, 0, image.layers};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
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
    _images[id] = rec;
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
    if (!AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory) || _fns.BindBufferMemory(_device, buffer, memory, 0) != VK_SUCCESS) {
        Problem("buffer " + std::to_string(id) + ": no memory");
        _fns.DestroyBuffer(_device, buffer, nullptr);
        return 0;
    }
    _buffers[id] = {buffer, info.size};
    return (uint64_t)buffer;
}

VkShaderModule Replayer::ModuleFromBlob(const JValue& object, const std::string& blobName) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!_capture->Blob(object, blobName, data, size) || size < 4) return VK_NULL_HANDLE;
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
    auto stageModule = [&](VkPipelineShaderStageCreateInfo& stage) {
        std::string name = std::string(StageName(stage.stage)) + ":" + (stage.pName ? stage.pName : "main");
        VkShaderModule module = ModuleFromBlob(object, name);
        if (module) {
            temporary.push_back(module);
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
    } else {
        Problem("pipeline " + std::to_string(object.Get("id")->Uint()) + ": " + std::string(cmd) + " is not replayed yet");
    }
    for (VkShaderModule m : temporary) _fns.DestroyShaderModule(_device, m, nullptr);
    if (r != VK_SUCCESS && pipeline == VK_NULL_HANDLE) {
        if (cmd == "vkCreateGraphicsPipelines" || cmd == "vkCreateComputePipelines")
            Problem("pipeline " + std::to_string(object.Get("id")->Uint()) + ": " + std::string(cmd) + " failed (" + std::to_string(r) + ")");
        return 0;
    }
    return (uint64_t)pipeline;
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
    } else if (type == "VkSurfaceKHR" || type == "VkSwapchainKHR" || type == "VkDeviceMemory" || type == "VkPipelineCache" ||
               type == "VkDebugUtilsMessengerEXT" || type == "VkDebugReportCallbackEXT") {
        _skipped.insert(id);
        _report->objectsSkipped++;
        return;
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
        handle = CreateImage(id, info);
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
    } else if (type == "VkSampler") {
        Args_vkCreateSampler a{};
        DecodeArgs(_ctx, *args, a);
        VkSampler s = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateSampler(d, a.pCreateInfo, nullptr, &s) == VK_SUCCESS) handle = (uint64_t)s;
    } else if (type == "VkDescriptorSetLayout") {
        Args_vkCreateDescriptorSetLayout a{};
        DecodeArgs(_ctx, *args, a);
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateDescriptorSetLayout(d, a.pCreateInfo, nullptr, &l) == VK_SUCCESS) handle = (uint64_t)l;
    } else if (type == "VkPipelineLayout") {
        Args_vkCreatePipelineLayout a{};
        DecodeArgs(_ctx, *args, a);
        VkPipelineLayout l = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreatePipelineLayout(d, a.pCreateInfo, nullptr, &l) == VK_SUCCESS) handle = (uint64_t)l;
    } else if (type == "VkDescriptorPool") {
        Args_vkCreateDescriptorPool a{};
        DecodeArgs(_ctx, *args, a);
        VkDescriptorPool p = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateDescriptorPool(d, a.pCreateInfo, nullptr, &p) == VK_SUCCESS) handle = (uint64_t)p;
    } else if (type == "VkCommandPool") {
        Args_vkCreateCommandPool a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo) {
            VkCommandPoolCreateInfo info = *a.pCreateInfo;
            info.flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            VkCommandPool p = VK_NULL_HANDLE;
            if (_fns.CreateCommandPool(d, &info, nullptr, &p) == VK_SUCCESS) handle = (uint64_t)p;
        }
    } else if (type == "VkCommandBuffer") {
        Args_vkAllocateCommandBuffers a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pAllocateInfo && a.pAllocateInfo->commandPool) {
            VkCommandBufferAllocateInfo info = *a.pAllocateInfo;
            info.commandBufferCount = 1;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            if (_fns.AllocateCommandBuffers(d, &info, &cb) == VK_SUCCESS) handle = (uint64_t)(uintptr_t)cb;
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
            else Problem("descriptor set " + std::to_string(id) + ": vkAllocateDescriptorSets failed (" + std::to_string(r) + ")");
        }
    } else if (type == "VkFence") {
        Args_vkCreateFence a{};
        DecodeArgs(_ctx, *args, a);
        VkFence f = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateFence(d, a.pCreateInfo, nullptr, &f) == VK_SUCCESS) handle = (uint64_t)f;
    } else if (type == "VkSemaphore") {
        Args_vkCreateSemaphore a{};
        DecodeArgs(_ctx, *args, a);
        VkSemaphore s = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateSemaphore(d, a.pCreateInfo, nullptr, &s) == VK_SUCCESS) handle = (uint64_t)s;
    } else if (type == "VkEvent") {
        Args_vkCreateEvent a{};
        DecodeArgs(_ctx, *args, a);
        VkEvent e = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateEvent(d, a.pCreateInfo, nullptr, &e) == VK_SUCCESS) handle = (uint64_t)e;
    } else if (type == "VkQueryPool") {
        Args_vkCreateQueryPool a{};
        DecodeArgs(_ctx, *args, a);
        VkQueryPool q = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateQueryPool(d, a.pCreateInfo, nullptr, &q) == VK_SUCCESS) handle = (uint64_t)q;
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
            info.pAttachments = attachments.data();
            VkRenderPass rp = VK_NULL_HANDLE;
            if (_fns.CreateRenderPass(d, &info, nullptr, &rp) == VK_SUCCESS) {
                handle = (uint64_t)rp;
                _renderPasses[id] = rec;
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
            info.pAttachments = attachments.data();
            VkRenderPass rp = VK_NULL_HANDLE;
            if (_fns.CreateRenderPass2(d, &info, nullptr, &rp) == VK_SUCCESS) {
                handle = (uint64_t)rp;
                _renderPasses[id] = rec;
            }
        }
    } else if (type == "VkFramebuffer") {
        Args_vkCreateFramebuffer a{};
        DecodeArgs(_ctx, *args, a);
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (a.pCreateInfo && resolved() && _fns.CreateFramebuffer(d, a.pCreateInfo, nullptr, &fb) == VK_SUCCESS) {
            handle = (uint64_t)fb;
            _framebufferExtents[id] = {a.pCreateInfo->width, a.pCreateInfo->height};
            std::vector<uint64_t> views;
            if (const JValue* list = args->Get("pCreateInfo")->Get("pAttachments"); list && list->IsArray())
                for (uint32_t i = 0; i < list->count; ++i) views.push_back(IdOf(&list->items[i]));
            _framebufferViews[id] = std::move(views);
        }
    } else if (type == "VkShaderModule") {
        VkShaderModule m = ModuleFromBlob(o, "SPIR-V");
        if (!m && args) {
            Args_vkCreateShaderModule a{};
            DecodeArgs(_ctx, *args, a);
            if (a.pCreateInfo) _fns.CreateShaderModule(d, a.pCreateInfo, nullptr, &m);
        }
        handle = (uint64_t)m;
    } else if (type == "VkPipeline") {
        if (args) handle = CreatePipeline(o, cmd, index, *args, unresolvedBefore);
    } else {
        known = false;
    }
    _arena.Reset();

    if (!known) {
        Problem(type + " " + std::to_string(id) + " (" + cmd + ") is not replayed yet");
        _report->objectsSkipped++;
        return;
    }
    if (!handle) {
        Problem(type + " " + std::to_string(id) + " (" + cmd + ") " +
                (resolved() ? "could not be created" : "was not created: it names objects the replay does not have"));
        _report->objectsSkipped++;
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
            // Command buffers and descriptor sets go with their pools.
        }
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
    // The first layout the frame expects each image in: the old layout of its first barrier, the
    // layout a descriptor snapshot binds it with, or the initial layout of a pass that loads it.
    const JValue* commands = _capture->Commands();
    if (!commands || !commands->IsArray()) return;
    auto note = [&](uint64_t image, VkImageLayout layout) {
        if (image && layout != VK_IMAGE_LAYOUT_UNDEFINED && !_initialLayouts.count(image)) _initialLayouts[image] = layout;
    };
    auto layoutOf = [&](const JValue* v) { return (VkImageLayout)DecodeEnum_VkImageLayout(_ctx, v); };
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
                        note(IdOf(barriers->items[b].Get("image")), layoutOf(barriers->items[b].Get("oldLayout")));
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
                            const uint64_t view = IdOf(desc.Get("imageView"));
                            auto it = _views.find(view);
                            if (it != _views.end()) note(it->second.image, layoutOf(desc.Get("imageLayout")));
                        }
                    }
                }
            }
        }
        if (IsBeginRenderPass(m)) {
            const JValue* begin = args->Get(m == "vkCmdBeginRenderPass" ? "pRenderPassBegin" : "pRenderPassBegin");
            const uint64_t rp = begin ? IdOf(begin->Get("renderPass")) : 0;
            const uint64_t fb = begin ? IdOf(begin->Get("framebuffer")) : 0;
            auto rit = _renderPasses.find(rp);
            auto fit = _framebufferViews.find(fb);
            if (rit != _renderPasses.end() && fit != _framebufferViews.end()) {
                // A pass requires its attachments in their initial layout whatever it loads or clears.
                for (size_t a = 0; a < fit->second.size() && a < rit->second.initialLayouts.size(); ++a) {
                    auto vit = _views.find(fit->second[a]);
                    if (vit != _views.end()) note(vit->second.image, rit->second.initialLayouts[a]);
                }
            }
        }
    }
    _arena.Reset();
}

void Replayer::UploadSampledTextures() {
    const JValue* textures = _capture->Textures();
    if (!textures || !textures->IsArray()) return;
    for (uint32_t i = 0; i < textures->count; ++i) {
        const JValue& t = textures->items[i];
        const JValue* info = t.Get("info");
        if (!info || Str(info->Get("kind")) != "sampled" || info->Get("error")) continue;
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
        const VkImageAspectFlags aspect = Str(info->Get("aspect")) == "depth" ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        const vkinsp::FormatBlock block = vkinsp::FormatBlockInfo(image.format, aspect);
        if (!block.bytes) {
            Problem("sampled image " + std::to_string(info->Get("id")->Uint()) + ": its format cannot be uploaded");
            continue;
        }
        std::vector<VkBufferImageCopy> regions;
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
            Transition(cb, image, image.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            image.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            _fns.CmdCopyBufferToImage(cb, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
        });
        DestroyStaging(staging);
        _report->texturesUploaded++;
    }
}

void Replayer::TransitionToInitialLayouts() {
    RunOneTime([&](VkCommandBuffer cb) {
        for (auto& [id, image] : _images) {
            auto it = _initialLayouts.find(id);
            if (it == _initialLayouts.end() || it->second == image.layout) continue;
            if (it->second == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && !_hasSwapchainExtension) continue;
            Transition(cb, image, image.layout, it->second);
            image.layout = it->second;
        }
    });
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
        _report->bufferUploads++;
    };
    for (uint32_t i = group.first; i <= group.last && i < commands->count; ++i) {
        const JValue& c = commands->items[i];
        if (const JValue* list = c.Get("bufferData"); list && list->IsArray())
            for (uint32_t k = 0; k < list->count; ++k) apply(list->items[k].Uint());
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
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<std::unique_ptr<std::vector<VkDescriptorBufferInfo>>> bufferInfos;
        std::vector<std::unique_ptr<std::vector<VkDescriptorImageInfo>>> imageInfos;
        std::vector<std::unique_ptr<std::vector<VkBufferView>>> viewInfos;
        std::string key;
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
                for (; k < list->count && !list->items[k].IsNull(); ++k) {
                    const JValue& desc = list->items[k];
                    if (desc.Get("buffer")) {
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
                bufferInfos.push_back(std::move(buffers));
                imageInfos.push_back(std::move(images));
                viewInfos.push_back(std::move(views));
                writes.push_back(w);
                key += "|" + std::to_string(bindingIndex) + "@" + std::to_string(start) + "|";
            }
        }
        auto& last = _descriptorContents[setId];
        if (last == key) continue;  // rewriting a bound set would invalidate the command buffers that bound it
        last = key;
        if (!writes.empty()) _fns.UpdateDescriptorSets(_device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }
    _arena.Reset();
}

void Replayer::BeginPass(const JValue& command, uint32_t index, uint64_t commandBuffer) {
    (void)command;
    (void)index;
    (void)commandBuffer;
}

void Replayer::InjectReadbacks(VkCommandBuffer cb, const PassState& pass, std::vector<PendingReadback>& readbacks, const char* skipReason) {
    const JValue* textures = _capture->Textures();
    if (!textures || !textures->IsArray()) return;
    for (uint32_t i = 0; i < textures->count; ++i) {
        const JValue& t = textures->items[i];
        const JValue* info = t.Get("info");
        if (!info || Str(info->Get("kind")) == "sampled") continue;
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
        if (image.samples != VK_SAMPLE_COUNT_1_BIT) { skip("multisampled targets are not compared yet"); continue; }
        const VkImageAspectFlags aspect = cmp.aspect == "depth" ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        const VkDeviceSize size = info->Get("size")->Uint();
        PendingReadback pending;
        pending.target = target;
        pending.texture = &t;
        if (!CreateStaging(size, pending.staging)) { skip("no staging memory"); continue; }
        const VkImageLayout layout = cmp.attachment < layouts.size() ? layouts[cmp.attachment] : VK_IMAGE_LAYOUT_UNDEFINED;
        const uint32_t mip = vit->second.range.baseMipLevel;
        const uint32_t layers = std::max<uint32_t>(1, (uint32_t)info->Get("layers")->Uint());
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image;
        b.subresourceRange = {vkinsp::FormatAspects(image.format), mip, 1, vit->second.range.baseArrayLayer, layers};
        b.oldLayout = layout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {aspect, mip, vit->second.range.baseArrayLayer, layers};
        copy.imageExtent = {std::max(1u, image.extent.width >> mip), std::max(1u, image.extent.height >> mip), 1};
        _fns.CmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.staging.buffer, 1, &copy);
        std::swap(b.oldLayout, b.newLayout);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        _report->targets.push_back(cmp);
        readbacks.push_back(pending);
    }
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
            const VkImageAspectFlags aspect = cmp.aspect == "depth" ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
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

void Replayer::RecordSecondaries(size_t executeIndex, const JValue& execute) {
    // The capture inlines each secondary command buffer's recording after the vkCmdExecuteCommands
    // that ran it (commands with "secondary": its id): record them into the replay's secondaries.
    const JValue* commands = _capture->Commands();
    const JValue* list = execute.Get("args") ? execute.Get("args")->Get("pCommandBuffers") : nullptr;
    for (uint32_t s = 0; list && s < list->count; ++s) {
        const uint64_t id = IdOf(&list->items[s]);
        VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)Handle(id);
        if (!cb) continue;
        bool begun = false;
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
                begun = true;
            } else if (m == "vkEndCommandBuffer") {
                if (begun) _fns.EndCommandBuffer(cb);
                begun = false;
            } else if (ReplayFn fn = FindReplayCommand(m); fn && args && begun) {
                ApplyDescriptorSnapshot(c.Get("descriptors"));
                fn(_ctx, *args, cb);
                _report->commandsRecorded++;
            }
            _arena.Reset();
        }
        if (begun) _fns.EndCommandBuffer(cb);
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
    const JValue& beginCommand = commands->items[group.first];
    _ctx.where = "command " + std::to_string(group.first) + " vkBeginCommandBuffer";
    Args_vkBeginCommandBuffer begin{};
    if (const JValue* args = beginCommand.Get("args")) DecodeArgs(_ctx, *args, begin);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (begin.pBeginInfo) beginInfo = *begin.pBeginInfo;
    _fns.ResetCommandBuffer(cb, 0);
    _fns.BeginCommandBuffer(cb, &beginInfo);
    _arena.Reset();

    PassState pass;
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
            if (IsEndPass(m)) {
                InjectReadbacks(cb, pass, readbacks, "the replay left this pass out");
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
            if ((_options.overdraw || _options.history.enabled) && ArgsResolve(m, *args)) {
                if (_options.overdraw) PrepareOverdraw(cb, pass);
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
                skippingPass = true;
            } else if (a.pRenderingInfo) {
                VkRenderingInfo info = *a.pRenderingInfo;
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
                if (_options.overdraw || _options.history.enabled) {
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
                    if (_options.overdraw) PrepareOverdraw(cb, pass);
                    if (_options.history.enabled) PrepareHistory(cb, pass, histories);
                }
                _fns.CmdBeginRendering(cb, &info);
                _report->commandsRecorded++;
            }
            _arena.Reset();
            continue;
        }

        if (m == "vkCmdBindDescriptorSets" || m == "vkCmdBindDescriptorSets2" || m == "vkCmdBindDescriptorSets2KHR")
            ApplyDescriptorSnapshot(c.Get("descriptors"));
        if (m == "vkCmdExecuteCommands") RecordSecondaries(i, c);

        ReplayFn fn = FindReplayCommand(m);
        if (!fn) {
            Problem("command " + std::to_string(i) + ": " + m + " is not replayed");
            continue;
        }
        fn(_ctx, *args, cb);
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
            InjectReadbacks(cb, pass, readbacks);
            if (_options.overdraw) RecordOverdraw(cb, group, pass, i, overdraws);
            if (_options.history.enabled) RecordHistory(cb, group, pass, i, histories);
            pass.active = false;
        }
    }
    _fns.EndCommandBuffer(cb);
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
        std::vector<PendingReadback> readbacks;
        std::vector<PendingOverdraw> overdraws;
        std::vector<PendingHistory> histories;
        std::vector<VkCommandBuffer> cbs;
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
            ReleaseTransients();
            continue;
        }
        // One submission for the call's command buffers, without the application's semaphores and fence.
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = (uint32_t)cbs.size();
        info.pCommandBuffers = cbs.data();
        VkResult r = _fns.QueueSubmit(queue, 1, &info, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) {
            Problem("submission " + std::to_string(i) + ": vkQueueSubmit failed (" + std::to_string(r) + ")");
        } else {
            _fns.QueueWaitIdle(queue);
            _report->submissions++;
        }
        CompareReadbacks(readbacks);
        CompleteHistory(histories);
        CompleteOverdraw(overdraws);
    }
    for (const CommandGroup& g : _groups)
        if (!g.used) Problem("command buffer " + std::to_string(g.commandBuffer) + " was recorded but its submission is not in the capture");
}

// ---------------------------------------------------------------------------------------------

bool Replayer::Run(const CaptureFile& capture, const ReplayOptions& options, ReplayReport& report) {
    _capture = &capture;
    _options = options;
    _report = &report;
    if (!LoadVulkan() || !CreateInstance() || !CreateDevice()) {
        for (auto& p : _ctx.problems) report.problems.push_back(p);
        return false;
    }
    if (const JValue* buffers = capture.Buffers(); buffers && buffers->IsArray())
        for (uint32_t i = 0; i < buffers->count; ++i)
            if (const JValue* info = buffers->items[i].Get("info")) _bufferData[info->Get("id")->Uint()] = &buffers->items[i];

    if (options.history.enabled) {
        report.history.requested = true;
        report.history.image = options.history.image;
        report.history.x = options.history.x;
        report.history.y = options.history.y;
        report.history.mip = options.history.mip;
        report.history.layer = options.history.layer;
    }

    CreateObjects();
    ComputeInitialLayouts();
    UploadSampledTextures();
    TransitionToInitialLayouts();
    ReplayCommands();
    if (options.history.enabled && !_historyPasses) {
        report.history.notes.push_back("no replayed render pass renders to image " + std::to_string(options.history.image) + " at mip " +
                                       std::to_string(options.history.mip) + ", layer " + std::to_string(options.history.layer) +
                                       " (writes outside render passes are not followed yet)");
    }
    for (auto& p : _ctx.problems) report.problems.push_back(p);
    _ctx.problems.clear();
    return true;
}

} // namespace vkreplay
