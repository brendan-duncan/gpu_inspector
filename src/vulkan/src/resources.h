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
    /** Its device address once the application asked for one; 0 until then (see NoteBufferAddress). */
    VkDeviceAddress address = 0;
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
    // What the pass starts from: a loaded attachment's contents are taken while capturing
    // (CaptureManager::OnAttachmentBegin), in the layout the pass requires.
    VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentLoadOp stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // the stencil aspect's, taken apart from the depth's
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct SwapchainInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    uint32_t arrayLayers = 1;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    // The display refresh period queried for this swapchain (0 unknown) and its source.
    double refreshMs = 0;
    int refreshSource = 0;       // RefreshSource (refresh_rate.h)
};

struct RenderPassInfo {
    std::vector<RenderPassAttachment> attachments;
    // Per subpass: which attachments are color / depth targets (indices into attachments).
    std::vector<std::vector<uint32_t>> subpassColor;
    std::vector<int32_t> subpassDepth;  // -1 if none
    // Multiview: the layers the pass renders (the highest view bit + 1 over its subpasses), 0 without.
    uint32_t viewLayers = 0;
    // The layer's copy of the pass with every storeOp STORE, begun in place of the original
    // while capturing so DONT_CARE attachments can be read back (VK_NULL_HANDLE when the pass
    // stores everything anyway). See StoreAllRenderPass in hooks.cpp.
    VkRenderPass storeAll = VK_NULL_HANDLE;
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

    // ---------------------------------------------------------------------------------------
    // Device addresses.
    //
    // A ray tracing build names the geometry it reads by device address rather than by handle, so
    // without a way back from an address to the buffer holding it a build says nothing about what
    // it built (src/vulkan/src/hooks.cpp, NoteAccelerationStructureBuilds). The D3D12 library has
    // the same map for the same reason (src/d3d12/src/descriptors.h, AddressMap).
    //
    // Addresses are only known once the application asks for them, which it must do before it can
    // put one in a build, so recording them at that moment is enough.

    /** `vkGetBufferDeviceAddress` returned this address for this buffer. */
    void NoteBufferAddress(VkBuffer buffer, VkDeviceAddress address);
    /** `vkGetAccelerationStructureDeviceAddressKHR` returned this address for this structure. */
    void NoteStructureAddress(VkAccelerationStructureKHR structure, VkDeviceAddress address);

    /**
     * The buffer holding `address`, and how far into it. False when no buffer whose address was
     * asked for covers it — a buffer the application never took the address of, or memory that is
     * not a buffer at all.
     */
    bool ResolveAddress(VkDeviceAddress address, VkBuffer& buffer, VkDeviceSize& offset, VkDeviceSize& remaining) const;

    /** The acceleration structure at `address`, or null. This is how a top level names its bottom levels. */
    VkAccelerationStructureKHR StructureAt(VkDeviceAddress address) const;

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
    /** Buffer addresses in ascending order, so a lookup is a binary search over their ranges. */
    struct AddressRange {
        VkDeviceAddress address = 0;
        VkDeviceSize size = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
    };
    std::vector<AddressRange> _addresses;
    std::unordered_map<uint64_t, VkAccelerationStructureKHR> _structureAddresses;
};

} // namespace vkinsp
