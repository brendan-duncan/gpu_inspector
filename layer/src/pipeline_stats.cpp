#include "pipeline_stats.h"

#include "layer.h"

namespace vkinsp {

const char* const kPipelineStatisticNames[kPipelineStatisticCount] = {
    // The first two are Vulkan's own; the rest are named as the Metal library names the same
    // quantities, so the UI reads one set of counters whichever API produced the capture.
    "inputAssemblyVertices",
    "inputAssemblyPrimitives",
    "vertexInvocations",
    "clipperInvocations",
    "clipperPrimitivesOut",
    "fragmentInvocations",
};

static const VkBaseInStructure* ChainFind(const void* pNext, VkStructureType type) {
    for (auto* p = (const VkBaseInStructure*)pNext; p; p = p->pNext)
        if (p->sType == type) return p;
    return nullptr;
}

/** Whether the device being created turns multiview on, in either of the two structs that carry it. */
static bool EnablesMultiview(const VkDeviceCreateInfo& info) {
    if (auto* mv = ChainFind(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES))
        if (((const VkPhysicalDeviceMultiviewFeatures*)mv)->multiview) return true;
    if (auto* v11 = ChainFind(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES))
        if (((const VkPhysicalDeviceVulkan11Features*)v11)->multiview) return true;
    return false;
}

void PlanPipelineStatistics(InstanceData* inst, VkPhysicalDevice physicalDevice,
                            VkDeviceCreateInfo& info, PipelineStatisticsSetup& setup) {
    if (!inst || !inst->dispatch.GetPhysicalDeviceFeatures) return;
    if (ConfigFlag("VKINSP_NO_PIPELINE_STATISTICS")) return;   // leave the device exactly as asked

    VkPhysicalDeviceFeatures supported{};
    inst->dispatch.GetPhysicalDeviceFeatures(physicalDevice, &supported);
    if (!supported.pipelineStatisticsQuery) {
        Log("pass counters: the device does not support pipelineStatisticsQuery");
        return;
    }
    // A query active across a multiview render pass writes one result per view and so needs that
    // many consecutive query indices, which is not known before the pass begins. Rather than
    // reserve for the worst case, an application that enables multiview goes without the counters
    // (its passes are still timed). Stereo XR is what this gives up.
    if (EnablesMultiview(info)) {
        Log("pass counters: the application enables multiview, whose passes need a query per view; skipped");
        return;
    }

    // An application that chains VkPhysicalDeviceFeatures2 must not also pass pEnabledFeatures,
    // so the two cases below are exclusive.
    if (const VkBaseInStructure* found = ChainFind(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
        const auto* app = (const VkPhysicalDeviceFeatures2*)found;
        if (app->features.pipelineStatisticsQuery) {
            setup.enabled = true;
            return;
        }
        // Only replaceable at the head of the chain. Deeper, the node pointing at it belongs to
        // the application, and writing our copy's address into it would change what it sees.
        if ((const void*)found != info.pNext) {
            Log("pass counters: VkPhysicalDeviceFeatures2 is not at the head of the chain; skipped");
            return;
        }
        setup.features2 = *app;
        setup.features2.features.pipelineStatisticsQuery = VK_TRUE;
        info.pNext = &setup.features2;
    } else {
        if (info.pEnabledFeatures) {
            if (info.pEnabledFeatures->pipelineStatisticsQuery) {
                setup.enabled = true;
                return;
            }
            setup.features = *info.pEnabledFeatures;
        }
        setup.features.pipelineStatisticsQuery = VK_TRUE;
        info.pEnabledFeatures = &setup.features;
    }
    setup.enabled = true;
    setup.added = true;
    Log("pass counters: enabling pipelineStatisticsQuery");
}

} // namespace vkinsp
