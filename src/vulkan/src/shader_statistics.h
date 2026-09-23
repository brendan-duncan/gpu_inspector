// Compiler statistics per pipeline: what the driver's shader compiler made of each stage.
//
// The Shader Flame Graph's cost is a model, and `analyze_shaders` counts SPIR-V instructions. The
// driver knows the truth — how many registers a stage ended up using, whether it spilled, how many
// instructions the hardware will actually run — and `VK_KHR_pipeline_executable_properties` is the
// portable way to ask for it. It is the same data Nsight shows per shader, and it explains a number
// the hardware counters already report: low occupancy is usually register pressure.
//
// The driver only keeps it when a pipeline is created with VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR,
// so the layer adds that flag to every pipeline it sees. That costs compile time and memory in the
// driver, so it is off unless asked for: VKINSP_SHADER_STATISTICS=1, or "Compiler statistics" in the
// launch dialog.
//
// What the statistics are called is entirely the driver's choice — the extension defines the
// mechanism, not the names — so the layer passes them through as it finds them rather than mapping
// them to names it has invented.
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace vkinsp
{

struct DeviceData;
struct InstanceData;

/** What a device asked for at creation, decided before vkCreateDevice. */
struct ShaderStatisticsSetup
{
    /** Our copy of the device's extension list, when the layer had to extend it. */
    std::vector<const char*> extensionNames;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    /** The statistics were asked for and the device can report them. */
    bool enabled = false;
    /** The layer changed the create info. */
    bool added = false;
};

/**
 * Adds `VK_KHR_pipeline_executable_properties` and its feature to a device being created, when the
 * statistics were asked for and the physical device offers them. `info` is the layer's copy.
 */
void PlanShaderStatistics(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info,
    ShaderStatisticsSetup& setup);

/** Whether the layer should add the capture flag and query the statistics on this device. */
bool ShaderStatisticsEnabled(const DeviceData* dev);

/**
 * Adds VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR to a pipeline's flags, without which the driver
 * keeps nothing to report. The create infos are copied into thread-local storage that stays valid
 * for the downstream call, the way the image and buffer pre-hooks do it.
 */
const VkGraphicsPipelineCreateInfo* CaptureStatisticsFlags(DeviceData* dev, uint32_t count, const VkGraphicsPipelineCreateInfo* infos);
const VkComputePipelineCreateInfo* CaptureStatisticsFlags(DeviceData* dev, uint32_t count, const VkComputePipelineCreateInfo* infos);
const VkRayTracingPipelineCreateInfoKHR* CaptureStatisticsFlags(DeviceData* dev, uint32_t count, const VkRayTracingPipelineCreateInfoKHR* infos);

/**
 * Asks the driver what it made of a pipeline and attaches the answer to the pipeline object, so the
 * UI can show it beside the stage's code. Called after the pipeline exists; quiet when the
 * statistics are off or the driver reports none.
 */
void CollectShaderStatistics(DeviceData* dev, VkPipeline pipeline);

} // namespace vkinsp
