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
    if (!supported.pipelineStatisticsQuery && !supported.occlusionQueryPrecise) {
        Log("pass counters: the device supports neither pipelineStatisticsQuery nor occlusionQueryPrecise");
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

    // Two features, each wanted and each maybe already on: `pipelineStatisticsQuery` for the pass
    // counters, `occlusionQueryPrecise` for the samples that passed the depth and stencil tests.
    const VkPhysicalDeviceFeatures* appFeatures = nullptr;
    const VkBaseInStructure* found = ChainFind(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    if (found) appFeatures = &((const VkPhysicalDeviceFeatures2*)found)->features;
    else if (info.pEnabledFeatures) appFeatures = info.pEnabledFeatures;
    const bool hasStats = appFeatures && appFeatures->pipelineStatisticsQuery;
    const bool hasOcclusion = appFeatures && appFeatures->occlusionQueryPrecise;
    const bool wantStats = supported.pipelineStatisticsQuery && !hasStats;
    const bool wantOcclusion = supported.occlusionQueryPrecise && !hasOcclusion;
    setup.enabled = hasStats || wantStats;
    setup.occlusion = hasOcclusion || wantOcclusion;
    if (!wantStats && !wantOcclusion) return;   // the application already enables what is wanted

    // An application that chains VkPhysicalDeviceFeatures2 must not also pass pEnabledFeatures,
    // so the two cases below are exclusive.
    if (found) {
        // Only replaceable at the head of the chain. Deeper, the node pointing at it belongs to
        // the application, and writing our copy's address into it would change what it sees.
        if ((const void*)found != info.pNext) {
            Log("pass counters: VkPhysicalDeviceFeatures2 is not at the head of the chain; skipped");
            setup.enabled = hasStats;
            setup.occlusion = hasOcclusion;
            return;
        }
        setup.features2 = *(const VkPhysicalDeviceFeatures2*)found;
        if (wantStats) setup.features2.features.pipelineStatisticsQuery = VK_TRUE;
        if (wantOcclusion) setup.features2.features.occlusionQueryPrecise = VK_TRUE;
        info.pNext = &setup.features2;
    } else {
        if (info.pEnabledFeatures) setup.features = *info.pEnabledFeatures;
        if (wantStats) setup.features.pipelineStatisticsQuery = VK_TRUE;
        if (wantOcclusion) setup.features.occlusionQueryPrecise = VK_TRUE;
        info.pEnabledFeatures = &setup.features;
    }
    setup.added = true;
    Log("pass counters: enabling%s%s", wantStats ? " pipelineStatisticsQuery" : "", wantOcclusion ? " occlusionQueryPrecise" : "");
}

} // namespace vkinsp
