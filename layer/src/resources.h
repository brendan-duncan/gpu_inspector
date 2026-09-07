// Structured side data about resources the capture code needs at command-recording time:
// image formats and sizes, view ranges, framebuffer attachments, render pass final layouts.
// (The tracker keeps the JSON descriptors for the UI; this keeps the few fields the layer itself
// must act on, in native form.)
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "vk_commands.gen.h"

namespace vkinsp {

struct ImageInfo {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkExtent3D extent{};
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    bool swapchainImage = false;
    bool transferSrc = false;   // usage includes TRANSFER_SRC (we add it when we can)
};

struct ImageViewInfo {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageSubresourceRange range{};
};

struct BufferInfo {
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    bool transferSrc = false;
};

struct FramebufferInfo {
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkImageView> attachments;  // empty for imageless framebuffers
    uint32_t width = 0, height = 0, layers = 1;
    bool imageless = false;
};

struct RenderPassAttachment {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
};

struct SwapchainInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    uint32_t arrayLayers = 1;
};

struct RenderPassInfo {
    std::vector<RenderPassAttachment> attachments;
    // Per subpass: which attachments are color / depth targets (indices into attachments).
    std::vector<std::vector<uint32_t>> subpassColor;
    std::vector<int32_t> subpassDepth;  // -1 if none
};

class ResourceRegistry {
public:
    static ResourceRegistry& Get();

    void AddImage(VkImage image, const ImageInfo& info);
    void AddImageView(VkImageView view, const ImageViewInfo& info);
    void AddBuffer(VkBuffer buffer, const BufferInfo& info);
    void AddFramebuffer(VkFramebuffer fb, const FramebufferInfo& info);
    void AddRenderPass(VkRenderPass rp, const RenderPassInfo& info);
    void AddSwapchain(VkSwapchainKHR sc, const SwapchainInfo& info);

    bool GetImage(VkImage image, ImageInfo& out) const;
    bool GetImageView(VkImageView view, ImageViewInfo& out) const;
    bool GetBuffer(VkBuffer buffer, BufferInfo& out) const;
    bool GetFramebuffer(VkFramebuffer fb, FramebufferInfo& out) const;
    bool GetRenderPass(VkRenderPass rp, RenderPassInfo& out) const;
    bool GetSwapchain(VkSwapchainKHR sc, SwapchainInfo& out) const;

    // Called by the tracker when any object is destroyed.
    void OnDestroy(HandleType type, uint64_t handle);

private:
    mutable std::shared_mutex _mutex;
    std::unordered_map<uint64_t, ImageInfo> _images;
    std::unordered_map<uint64_t, ImageViewInfo> _views;
    std::unordered_map<uint64_t, BufferInfo> _buffers;
    std::unordered_map<uint64_t, FramebufferInfo> _framebuffers;
    std::unordered_map<uint64_t, RenderPassInfo> _renderPasses;
    std::unordered_map<uint64_t, SwapchainInfo> _swapchains;
};

} // namespace vkinsp
