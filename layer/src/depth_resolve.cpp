#include "depth_resolve.h"

#include "command_recorder.h"
#include "format_info.h"
#include "layer.h"

#include <cstring>

namespace vkinsp {

static bool HasName(const char* const* names, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i)
        if (names[i] && strcmp(names[i], name) == 0) return true;
    return false;
}

static const VkBaseInStructure* ChainFind(const void* pNext, VkStructureType type) {
    for (auto* p = (const VkBaseInStructure*)pNext; p; p = p->pNext)
        if (p->sType == type) return p;
    return nullptr;
}

void PlanDynamicRendering(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, DynamicRenderingSetup& setup) {
    if (!inst || !inst->dispatch.GetPhysicalDeviceProperties) return;
    if (ConfigFlag("VKINSP_NO_REFRESH_EXTENSIONS")) return;   // the same switch: leave the device alone
    // The application's own feature structs decide when present (chaining a second copy is invalid).
    if (auto* v13 = ChainFind(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)) {
        setup.enabled = ((const VkPhysicalDeviceVulkan13Features*)v13)->dynamicRendering == VK_TRUE;
        return;
    }
    if (auto* dr = ChainFind(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)) {
        setup.enabled = ((const VkPhysicalDeviceDynamicRenderingFeatures*)dr)->dynamicRendering == VK_TRUE;
        return;
    }
    // On a 1.2 device the extension carries the feature (its own dependencies are core there);
    // older devices would need a chain of extensions and are left out.
    VkPhysicalDeviceProperties props{};
    inst->dispatch.GetPhysicalDeviceProperties(physicalDevice, &props);
    const uint32_t deviceApi = props.apiVersion;
    const uint32_t appApi = inst->apiVersion;
    const uint32_t api = deviceApi < appApi ? deviceApi : appApi;
    if (api < VK_API_VERSION_1_1) return;
    PFN_vkGetPhysicalDeviceFeatures2 features2 = inst->dispatch.GetPhysicalDeviceFeatures2
        ? inst->dispatch.GetPhysicalDeviceFeatures2 : (PFN_vkGetPhysicalDeviceFeatures2)inst->dispatch.GetPhysicalDeviceFeatures2KHR;
    if (!features2) return;
    VkPhysicalDeviceDynamicRenderingFeatures dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &dr;
    features2(physicalDevice, &f2);
    if (!dr.dynamicRendering) return;

    // Below 1.3 the extension carries the feature; below 1.2 its dependencies are extensions
    // too (multiview and maintenance2 are core in 1.1). Every one of them must be offered.
    if (api < VK_API_VERSION_1_3) {
        std::vector<const char*> needed{VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
        if (api < VK_API_VERSION_1_2) {
            needed.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
            needed.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        }
        if (!inst->dispatch.EnumerateDeviceExtensionProperties) return;
        uint32_t count = 0;
        if (inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS || !count) return;
        std::vector<VkExtensionProperties> ext(count);
        if (inst->dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, ext.data()) < VK_SUCCESS) return;
        for (const char* name : needed) {
            bool offered = false;
            for (uint32_t i = 0; i < count; ++i)
                if (strcmp(ext[i].extensionName, name) == 0) { offered = true; break; }
            if (!offered) return;
        }
        setup.extensionNames.assign(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
        for (const char* name : needed)
            if (!HasName(setup.extensionNames.data(), (uint32_t)setup.extensionNames.size(), name)) setup.extensionNames.push_back(name);
        info.ppEnabledExtensionNames = setup.extensionNames.data();
        info.enabledExtensionCount = (uint32_t)setup.extensionNames.size();
    }
    setup.features.dynamicRendering = VK_TRUE;
    setup.features.pNext = const_cast<void*>(info.pNext);
    info.pNext = &setup.features;
    setup.enabled = true;
    setup.added = true;
    Log("depth resolve: enabling dynamic rendering (%s)", api >= VK_API_VERSION_1_3 ? "core 1.3" : "VK_KHR_dynamic_rendering");
}

bool CanResolveDepth(DeviceData* dev) {
    return dev && dev->dynamicRendering && (dev->dispatch.CmdBeginRendering || dev->dispatch.CmdBeginRenderingKHR) &&
           (dev->dispatch.CmdEndRendering || dev->dispatch.CmdEndRenderingKHR);
}

bool CreateDepthResolveViews(DeviceData* dev, const PendingImageCopy& p, VkImageView* srcView, VkImageView* dstView) {
    const DeviceDispatch& d = dev->dispatch;
    *srcView = *dstView = VK_NULL_HANDLE;
    const VkImageViewType type = p.range.layerCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.viewType = type;
    ci.format = p.format;
    ci.image = p.image;
    ci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, p.range.baseMipLevel, 1, p.range.baseArrayLayer, p.range.layerCount};
    if (d.CreateImageView(dev->device, &ci, nullptr, srcView) != VK_SUCCESS) return false;
    ci.image = p.resolve;
    ci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, p.range.layerCount};
    if (d.CreateImageView(dev->device, &ci, nullptr, dstView) != VK_SUCCESS) {
        d.DestroyImageView(dev->device, *srcView, nullptr);
        *srcView = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void RecordDepthResolve(DeviceData* dev, VkCommandBuffer cb, const PendingImageCopy& p) {
    const DeviceDispatch& d = dev->dispatch;
    const VkImageAspectFlags aspects = FormatAspects(p.format);

    // The multisampled image becomes a depth attachment (its contents kept: loadOp LOAD).
    VkImageMemoryBarrier toAttach{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toAttach.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    toAttach.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toAttach.oldLayout = p.layout;
    toAttach.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    toAttach.srcQueueFamilyIndex = toAttach.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttach.image = p.image;
    toAttach.subresourceRange = p.range;
    VkImageMemoryBarrier target = toAttach;
    target.srcAccessMask = 0;
    target.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.image = p.resolve;
    target.subresourceRange = {aspects, 0, 1, 0, p.range.layerCount};
    VkImageMemoryBarrier both[2] = {toAttach, target};
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
                         0, nullptr, 0, nullptr, 2, both);

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = p.srcView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    depth.resolveImageView = p.dstView;
    depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {p.extent.width, p.extent.height}};
    ri.layerCount = p.range.layerCount;
    ri.pDepthAttachment = &depth;
    if (d.CmdBeginRendering) d.CmdBeginRendering(cb, &ri); else d.CmdBeginRenderingKHR(cb, &ri);
    if (d.CmdEndRendering) d.CmdEndRendering(cb); else d.CmdEndRenderingKHR(cb);

    // The resolved image goes to the copy; the multisampled one back to the application's layout.
    VkImageMemoryBarrier resolved = target;
    resolved.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    resolved.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    resolved.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    resolved.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier back = toAttach;
    back.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    back.newLayout = p.layout;
    VkImageMemoryBarrier after[2] = {resolved, back};
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 2, after);
}

}  // namespace vkinsp
