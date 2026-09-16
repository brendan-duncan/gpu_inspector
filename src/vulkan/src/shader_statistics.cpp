#include "shader_statistics.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "json_writer.h"
#include "layer.h"
#include "tracker.h"

namespace vkinsp {

namespace {

/** The first structure of a pNext chain with this type, or null. */
const VkBaseInStructure* ChainFind(const void* pNext, VkStructureType type) {
    for (auto* n = static_cast<const VkBaseInStructure*>(pNext); n; n = n->pNext)
        if (n->sType == type) return n;
    return nullptr;
}

/** The shader stages an executable covers, as words rather than a bit mask. */
void StageNames(JsonWriter& w, VkShaderStageFlags stages) {
    static const struct { VkShaderStageFlagBits bit; const char* name; } kStages[] = {
        {VK_SHADER_STAGE_VERTEX_BIT, "vertex"},
        {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "tessellation control"},
        {VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "tessellation evaluation"},
        {VK_SHADER_STAGE_GEOMETRY_BIT, "geometry"},
        {VK_SHADER_STAGE_FRAGMENT_BIT, "fragment"},
        {VK_SHADER_STAGE_COMPUTE_BIT, "compute"},
        {VK_SHADER_STAGE_TASK_BIT_EXT, "task"},
        {VK_SHADER_STAGE_MESH_BIT_EXT, "mesh"},
        {VK_SHADER_STAGE_RAYGEN_BIT_KHR, "raygen"},
        {VK_SHADER_STAGE_ANY_HIT_BIT_KHR, "any hit"},
        {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "closest hit"},
        {VK_SHADER_STAGE_MISS_BIT_KHR, "miss"},
        {VK_SHADER_STAGE_INTERSECTION_BIT_KHR, "intersection"},
        {VK_SHADER_STAGE_CALLABLE_BIT_KHR, "callable"},
    };
    w.BeginArray();
    for (const auto& s : kStages)
        if (stages & s.bit) w.String(s.name);
    w.EndArray();
}

/** One statistic's value, whichever of the four kinds the driver chose. */
void StatisticValue(JsonWriter& w, const VkPipelineExecutableStatisticKHR& s) {
    switch (s.format) {
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: w.Boolean(s.value.b32 != VK_FALSE); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: w.Int(s.value.i64); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: w.Uint(s.value.u64); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: w.Double(s.value.f64); break;
        default: w.Null(); break;
    }
}

/**
 * Copies create infos into thread-local storage with the capture flag added. The storage lives until
 * the next call on this thread, which is past the downstream vkCreate*Pipelines that reads it.
 */
template <typename Info>
const Info* WithCaptureFlag(uint32_t count, const Info* infos) {
    thread_local std::vector<Info> copies;
    copies.assign(infos, infos + count);
    for (Info& ci : copies) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    return copies.data();
}

} // namespace

void PlanShaderStatistics(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info,
                          ShaderStatisticsSetup& setup) {
    if (!ConfigFlag("VKINSP_SHADER_STATISTICS")) return;
    if (!inst || !inst->dispatch.EnumerateDeviceExtensionProperties) return;
    uint32_t count = 0;
    inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    if (count) inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data());
    const bool has = std::any_of(available.begin(), available.end(), [](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) == 0;
    });
    if (!has) {
        Log("compiler statistics: this device has no %s", VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        return;
    }
    // The feature has to be on as well; without it the queries are invalid.
    PFN_vkGetPhysicalDeviceFeatures2 features2 = inst->dispatch.GetPhysicalDeviceFeatures2
        ? inst->dispatch.GetPhysicalDeviceFeatures2 : (PFN_vkGetPhysicalDeviceFeatures2)inst->dispatch.GetPhysicalDeviceFeatures2KHR;
    if (!features2) return;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR supported{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &supported;
    features2(physicalDevice, &f2);
    if (!supported.pipelineExecutableInfo) {
        Log("compiler statistics: this device does not support pipelineExecutableInfo");
        return;
    }
    // A chain may hold each structure only once, so where the application already asks for this
    // feature the layer adds nothing. Its answer is taken as given rather than overwritten: the
    // chain is the application's memory, and turning a feature on behind its back is not the
    // layer's to do.
    if (auto* existing = (const VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR*)ChainFind(
            info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR)) {
        if (!existing->pipelineExecutableInfo) {
            Log("compiler statistics: the application chains pipelineExecutableInfo=false, so they stay off");
            return;
        }
        setup.enabled = true;
        Log("compiler statistics: on (the application enabled pipelineExecutableInfo itself)");
        return;
    }
    setup.features.pipelineExecutableInfo = VK_TRUE;
    setup.features.pNext = const_cast<void*>(info.pNext);
    info.pNext = &setup.features;
    setup.extensionNames.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
    const bool already = std::any_of(setup.extensionNames.begin(), setup.extensionNames.end(), [](const char* e) {
        return std::strcmp(e, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) == 0;
    });
    if (!already) setup.extensionNames.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    info.ppEnabledExtensionNames = setup.extensionNames.data();
    info.enabledExtensionCount = (uint32_t)setup.extensionNames.size();
    setup.enabled = true;
    setup.added = true;
    Log("compiler statistics: on (%s)", VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
}

bool ShaderStatisticsEnabled(const DeviceData* dev) {
    return dev && dev->shaderStatistics && dev->dispatch.GetPipelineExecutablePropertiesKHR;
}

const VkGraphicsPipelineCreateInfo* CaptureStatisticsFlags(DeviceData* dev, uint32_t count, const VkGraphicsPipelineCreateInfo* infos) {
    if (!ShaderStatisticsEnabled(dev) || !infos || !count) return infos;
    return WithCaptureFlag(count, infos);
}

const VkComputePipelineCreateInfo* CaptureStatisticsFlags(DeviceData* dev, uint32_t count, const VkComputePipelineCreateInfo* infos) {
    if (!ShaderStatisticsEnabled(dev) || !infos || !count) return infos;
    return WithCaptureFlag(count, infos);
}

const VkRayTracingPipelineCreateInfoKHR* CaptureStatisticsFlags(DeviceData* dev, uint32_t count, const VkRayTracingPipelineCreateInfoKHR* infos) {
    if (!ShaderStatisticsEnabled(dev) || !infos || !count) return infos;
    return WithCaptureFlag(count, infos);
}

void CollectShaderStatistics(DeviceData* dev, VkPipeline pipeline) {
    if (!ShaderStatisticsEnabled(dev) || !pipeline) return;
    // Objects are addressed by the tracker's own id, which the pipeline has only once it is tracked.
    const uint64_t id = Tracker::Get().Resolve(HT_VkPipeline, (uint64_t)(uintptr_t)pipeline);
    if (!id) return;
    const DeviceDispatch& d = dev->dispatch;
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = pipeline;
    uint32_t executables = 0;
    if (d.GetPipelineExecutablePropertiesKHR(dev->device, &pi, &executables, nullptr) != VK_SUCCESS || !executables) return;
    std::vector<VkPipelineExecutablePropertiesKHR> props(executables, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    for (auto& p : props) p.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    if (d.GetPipelineExecutablePropertiesKHR(dev->device, &pi, &executables, props.data()) < VK_SUCCESS) return;

    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    // "executables" rather than "shaders": one executable can cover several stages, which is how a
    // driver reports stages it merged (a vertex and a geometry stage compiled into one program).
    w.Key("executables"); w.BeginArray();
    bool any = false;
    for (uint32_t e = 0; e < executables; ++e) {
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pipeline;
        ei.executableIndex = e;
        uint32_t count = 0;
        if (!d.GetPipelineExecutableStatisticsKHR ||
            d.GetPipelineExecutableStatisticsKHR(dev->device, &ei, &count, nullptr) != VK_SUCCESS || !count) {
            continue;
        }
        std::vector<VkPipelineExecutableStatisticKHR> stats(count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        for (auto& s : stats) s.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        if (d.GetPipelineExecutableStatisticsKHR(dev->device, &ei, &count, stats.data()) < VK_SUCCESS) continue;

        any = true;
        w.BeginObject();
        w.Key("name"); w.String(props[e].name);
        w.Key("description"); w.String(props[e].description);
        w.Key("stages"); StageNames(w, props[e].stages);
        w.Key("subgroupSize"); w.Uint(props[e].subgroupSize);
        w.Key("statistics"); w.BeginArray();
        for (uint32_t i = 0; i < count; ++i) {
            w.BeginObject();
            w.Key("name"); w.String(stats[i].name);
            w.Key("description"); w.String(stats[i].description);
            w.Key("value"); StatisticValue(w, stats[i]);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    if (!any) return;   // the driver offered executables but no statistics for any of them
    Tracker::Get().Update(id, "executables", w.str());
}

} // namespace vkinsp
