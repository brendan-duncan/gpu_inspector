// Multisampled depth read-back. vkCmdResolveImage resolves color only; a depth attachment is
// resolved by a render pass with a depth resolve attachment. The layer uses dynamic rendering
// for it (no render pass or framebuffer objects: an empty vkCmdBeginRendering/vkCmdEndRendering
// pair with the multisampled image as depth attachment, loadOp LOAD, and the single-sampled
// temporary image as its resolve target, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, which every
// implementation supports). Dynamic rendering is enabled for the application at device creation
// when the physical device offers it (core in 1.3, VK_KHR_dynamic_rendering on 1.2 devices).
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace vkinsp {

struct InstanceData;
struct DeviceData;
struct PendingImageCopy;

struct DynamicRenderingSetup {
    std::vector<const char*> extensionNames;
    VkPhysicalDeviceDynamicRenderingFeatures features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    bool enabled = false;   // usable after creation (added by the layer, or enabled by the application)
    bool added = false;     // the layer changed the create info
};

// Adds the dynamic rendering feature (and extension when needed) to a device's create info, or
// notes that the application enables it itself. `info` is a copy of the application's.
void PlanDynamicRendering(InstanceData* inst, VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& info, DynamicRenderingSetup& setup);

// Whether the device can resolve a multisampled depth image for read-back.
bool CanResolveDepth(DeviceData* dev);

// Image views for a depth resolve: the copied mip / layers of the multisampled image and the
// temporary image's layers. Destroyed by the caller once the GPU is done.
bool CreateDepthResolveViews(DeviceData* dev, const PendingImageCopy& p, VkImageView* srcView, VkImageView* dstView);

// Records the depth resolve of `p.image` (in `p.layout`, left in `p.layout`) into `p.resolve`,
// which is left in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL for the copy.
void RecordDepthResolve(DeviceData* dev, VkCommandBuffer cb, const PendingImageCopy& p);

}  // namespace vkinsp
