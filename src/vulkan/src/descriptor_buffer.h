// Descriptor buffers (VK_EXT_descriptor_buffer).
//
// A descriptor buffer replaces descriptor sets with plain memory: the application asks the driver
// for a descriptor's bytes, puts them in a buffer it owns, and a draw names a set by an offset into
// that buffer rather than by a handle. Nothing about the bytes is defined — their size and layout
// belong to the implementation — so a capture that only recorded the commands would show a draw
// reading memory it could say nothing about, which is what it did before this file existed.
//
// What makes them readable is that `vkGetDescriptorEXT` is the *only* way to produce a descriptor:
// whatever an application later does with the bytes, it had to ask for them first, naming the
// buffer, image view or sampler it wanted one for. So the layer keeps what it saw go past — the
// bytes, and the resource they were made from — and decodes a descriptor buffer by looking its
// contents up in that table. A descriptor whose bytes were never seen (produced before the layer
// attached) stays unresolved rather than being guessed at.
//
// The decoded set is an ordinary DescriptorSetContents (descriptors.h), so a draw that binds
// through a descriptor buffer produces the same "descriptors" snapshot as one that binds a set,
// and the UI, the render graph and the replay need to know nothing about any of this.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "descriptors.h"
#include "vk_commands.gen.h"

namespace vkinsp
{

struct DeviceData;

/** Per-type descriptor sizes, read once from VkPhysicalDeviceDescriptorBufferPropertiesEXT. */
struct DescriptorSizes
{
    bool known = false;
    size_t sampler = 0;
    size_t combinedImageSampler = 0;
    size_t sampledImage = 0;
    size_t storageImage = 0;
    size_t uniformTexelBuffer = 0;
    size_t storageTexelBuffer = 0;
    size_t uniformBuffer = 0;
    size_t storageBuffer = 0;
    size_t inputAttachment = 0;
    size_t accelerationStructure = 0;

    /** The bytes one descriptor of this type occupies, or 0 when the type has no size here. */
    size_t For(VkDescriptorType type) const;
};

class DescriptorBufferTracker
{
public:
    static DescriptorBufferTracker& Get();

    /** Reads the device's descriptor sizes, once, the first time they are needed. */
    const DescriptorSizes& Sizes(DeviceData* dev);

    /** `vkGetDescriptorEXT` made `size` bytes at `data` for the resource `info` names. */
    void OnGetDescriptor(const VkDescriptorGetInfoEXT* info, size_t size, const void* data);

    /** `vkCmdBindDescriptorBuffersEXT`: the buffers this command buffer can take sets from. */
    void OnBindBuffers(VkCommandBuffer cb, uint32_t count, const VkDescriptorBufferBindingInfoEXT* infos);

    /** `vkCmdSetDescriptorBufferOffsetsEXT`: which of those, and where, each set comes from. */
    void OnSetOffsets(VkCommandBuffer cb, VkPipelineBindPoint point, uint32_t firstSet, uint32_t setCount,
        const uint32_t* bufferIndices, const VkDeviceSize* offsets);

    /**
     * Where set `set` currently comes from on this command buffer, as a buffer and an offset into
     * it. False when nothing has bound that set, or when the address named no buffer the layer
     * knows (an application that never asked for the buffer's device address).
     */
    bool SetSource(VkCommandBuffer cb, VkPipelineBindPoint point, uint32_t set, VkBuffer& buffer,
        VkDeviceSize& offset) const;

    /**
     * Builds a set's contents from `bytes`, the descriptor buffer's memory at the set's offset.
     * Entries whose bytes are all zero, or which match no descriptor the layer saw made, are left
     * unwritten. False when the layout is unknown or the device's descriptor sizes are not.
     */
    bool Decode(DeviceData* dev, VkDescriptorSetLayout layout, const uint8_t* bytes, size_t size,
        DescriptorSetContents& out) const;

    /** The bytes a set of this layout occupies, so the right amount is read and captured. */
    bool LayoutSize(DeviceData* dev, VkDescriptorSetLayout layout, VkDeviceSize& out) const;

    void OnResetCommandBuffer(VkCommandBuffer cb);
    void OnDestroy(HandleType type, uint64_t handle);

private:
    DescriptorBufferTracker() = default;

    /** One descriptor buffer bound to a command buffer, already resolved to a buffer. */
    struct BoundBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;        // where the binding's address sits in that buffer
    };
    struct SetOffset
    {
        uint32_t bufferIndex = 0;
        VkDeviceSize offset = 0;
        bool set = false;
    };
    struct CommandBufferState
    {
        std::vector<BoundBuffer> buffers;
        // Per bind point, since a command buffer can have different sets bound for each.
        std::unordered_map<int, std::vector<SetOffset>> sets;
    };

    mutable std::shared_mutex _mutex;
    /** Descriptor bytes -> the resource they were made from. Keyed by the bytes themselves. */
    std::unordered_map<std::string, DescriptorEntry> _descriptors;
    std::unordered_map<uint64_t, CommandBufferState> _commandBuffers;
    std::unordered_map<uint64_t, DescriptorSizes> _sizes;   // per device
};

} // namespace vkinsp
