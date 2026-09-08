// Descriptor set contents. Vulkan descriptor sets are mutable and opaque, so to show what a
// draw had bound (WebGPU Inspector shows each bind group's entries) the layer follows every
// vkUpdateDescriptorSets / template update and, when a set is bound during a capture, writes a
// snapshot of its bindings into the vkCmdBindDescriptorSets command.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "json_writer.h"
#include "vk_commands.gen.h"

namespace vkinsp {

// One descriptor (one array element of a binding).
struct DescriptorEntry {
    bool written = false;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize range = 0;
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkBufferView bufferView = VK_NULL_HANDLE;
};

struct DescriptorBinding {
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkShaderStageFlags stages = 0;
    bool immutableSamplers = false;
    std::vector<DescriptorEntry> entries;   // descriptorCount elements
};

struct DescriptorSetContents {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<DescriptorBinding> bindings;  // sorted by binding number
};

struct DescriptorTemplateInfo {
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
    VkDescriptorUpdateTemplateType type = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
};

class DescriptorTracker {
public:
    static DescriptorTracker& Get();

    void OnCreateLayout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutCreateInfo* info);
    void OnAllocateSets(const VkDescriptorSetAllocateInfo* info, const VkDescriptorSet* sets);
    void OnUpdateSets(uint32_t writeCount, const VkWriteDescriptorSet* writes, uint32_t copyCount,
                      const VkCopyDescriptorSet* copies);
    void OnCreateTemplate(VkDescriptorUpdateTemplate tmpl, const VkDescriptorUpdateTemplateCreateInfo* info);
    void OnUpdateWithTemplate(VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void* data);
    void OnDestroy(HandleType type, uint64_t handle);

    bool GetSet(VkDescriptorSet set, DescriptorSetContents& out) const;
    bool GetLayout(VkDescriptorSetLayout layout, DescriptorSetContents& out) const;

    // Builds the bindings of a push descriptor set from its writes (no set object exists).
    static DescriptorSetContents FromWrites(uint32_t writeCount, const VkWriteDescriptorSet* writes);

private:
    DescriptorTracker() = default;
    void ApplyWrite(DescriptorSetContents& set, uint32_t binding, uint32_t arrayElement, VkDescriptorType type,
                    uint32_t count, const VkDescriptorImageInfo* images, const VkDescriptorBufferInfo* buffers,
                    const VkBufferView* views, size_t stride);

    mutable std::shared_mutex _mutex;
    std::unordered_map<uint64_t, DescriptorSetContents> _layouts;
    std::unordered_map<uint64_t, DescriptorSetContents> _sets;
    std::unordered_map<uint64_t, DescriptorTemplateInfo> _templates;
};

// Writes the JSON snapshot of one bound set:
//   {"set": N, "descriptorSet": {...}, "layout": {...}, "bindings": [{"binding", "type", "stages",
//    "descriptors": [{"buffer", "offset", "range", "dynamicOffset"?, "data"?} | {"imageView",
//    "sampler", "imageLayout"} | {"bufferView"} | null]}]}
// `dynamicOffsets` is advanced for every *_DYNAMIC descriptor; `dataIds` (same shape as the
// bindings' entries, 0 = none) are the buffer capture ids attached to buffer descriptors.
void WriteDescriptorSetJson(JsonWriter& w, uint32_t setIndex, VkDescriptorSet set, const DescriptorSetContents& contents,
                            const uint32_t* dynamicOffsets, uint32_t dynamicOffsetCount, uint32_t& dynamicIndex,
                            const std::vector<std::vector<uint32_t>>* dataIds);

// Writes just the bindings array of a set (the value of "bindings" above). Used for the live
// contents of a set shown in the Inspect panel, where there are no dynamic offsets or captures.
void WriteDescriptorBindingsJson(JsonWriter& w, const DescriptorSetContents& contents, const uint32_t* dynamicOffsets,
                                 uint32_t dynamicOffsetCount, uint32_t& dynamicIndex,
                                 const std::vector<std::vector<uint32_t>>* dataIds);

// Effective range of a buffer descriptor (VK_WHOLE_SIZE resolved against the buffer's size).
VkDeviceSize DescriptorBufferRange(const DescriptorEntry& e);

bool IsBufferDescriptor(VkDescriptorType t);
bool IsDynamicDescriptor(VkDescriptorType t);

} // namespace vkinsp
