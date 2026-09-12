// Pipeline statistics queries: the counters behind the GPU Bottlenecks report.
//
// The Metal capture library samples Metal's statistic counter set around every pass, and the
// numbers a bottleneck is described in are divisions of those: fragment shader invocations over
// the render target's pixels is overdraw, over primitives out of the clipper is fragments per
// primitive. Vulkan carries almost the same counters in VK_QUERY_TYPE_PIPELINE_STATISTICS, so a
// second query pool beside the timestamp one makes that report work for Vulkan captures too
// (docs/PROFILING.md).
//
// The catch is that the counters need the `pipelineStatisticsQuery` feature enabled at device
// creation, and an application that does not profile itself has no reason to ask for it. So the
// layer adds it, the way it already adds a refresh-period extension and dynamic rendering, with
// the same fallback: if the driver refuses the device with the addition, the device is created
// exactly as the application asked and the counters are simply absent.
#pragma once

#include <vulkan/vulkan.h>

namespace vkinsp {

struct InstanceData;

/**
 * The statistics the layer asks for, in the order VkQueryPipelineStatisticFlagBits defines them,
 * which is the order the results come back in.
 */
constexpr VkQueryPipelineStatisticFlags kPipelineStatistics =
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;

/** How many values each query writes: one per bit above. */
constexpr uint32_t kPipelineStatisticCount = 6;

/** Their names in the protocol, matching what the Metal library sends for the same quantities. */
extern const char* const kPipelineStatisticNames[kPipelineStatisticCount];

struct PipelineStatisticsSetup {
    /** Our copy of the application's features, when it passed them through pEnabledFeatures. */
    VkPhysicalDeviceFeatures features{};
    /** Our copy of its VkPhysicalDeviceFeatures2, when it chained one instead. */
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    bool enabled = false;   // the device will have pipelineStatisticsQuery (ours or the application's)
    /**
     * The device will have occlusionQueryPrecise, so a pass's occlusion query counts the samples
     * that passed its depth and stencil tests rather than answering "any" (`fragmentsPassed`).
     */
    bool occlusion = false;
    bool added = false;     // the layer changed the create info
};

/**
 * Enables `pipelineStatisticsQuery` and `occlusionQueryPrecise` on a device being created, or notes
 * that the application enables them itself. `info` is the layer's copy of its create info.
 */
void PlanPipelineStatistics(InstanceData* inst, VkPhysicalDevice physicalDevice,
                            VkDeviceCreateInfo& info, PipelineStatisticsSetup& setup);

} // namespace vkinsp
