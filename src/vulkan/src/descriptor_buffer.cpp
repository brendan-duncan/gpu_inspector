#include "descriptor_buffer.h"

#include <cstring>

#include "layer.h"
#include "resources.h"

namespace vkinsp
{

size_t DescriptorSizes::For(VkDescriptorType type) const
{
    switch (type)
    {
        case VK_DESCRIPTOR_TYPE_SAMPLER: return sampler;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return combinedImageSampler;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return sampledImage;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return storageImage;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return uniformTexelBuffer;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return storageTexelBuffer;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return uniformBuffer;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return storageBuffer;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return inputAttachment;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return accelerationStructure;
        default: return 0;   // inline uniform blocks hold their data, not descriptors
    }
}

DescriptorBufferTracker& DescriptorBufferTracker::Get()
{
    static DescriptorBufferTracker instance;
    return instance;
}

const DescriptorSizes& DescriptorBufferTracker::Sizes(DeviceData* dev)
{
    static const DescriptorSizes kNone;
    if (!dev || !dev->physicalDevice)
        return kNone;
    const uint64_t key = (uint64_t)(uintptr_t)dev->device;
    {
        std::shared_lock lock(_mutex);
        auto it = _sizes.find(key);
        if (it != _sizes.end())
            return it->second;
    }
    auto get = dev->instance ? dev->instance->dispatch.GetPhysicalDeviceProperties2 : nullptr;
    if (!get && dev->instance)
        get = dev->instance->dispatch.GetPhysicalDeviceProperties2KHR;

    DescriptorSizes s;
    if (get)
    {
        VkPhysicalDeviceDescriptorBufferPropertiesEXT db{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &db;
        get(dev->physicalDevice, &props);
        s.known = true;
        s.sampler = db.samplerDescriptorSize;
        s.combinedImageSampler = db.combinedImageSamplerDescriptorSize;
        s.sampledImage = db.sampledImageDescriptorSize;
        s.storageImage = db.storageImageDescriptorSize;
        s.uniformTexelBuffer = db.uniformTexelBufferDescriptorSize;
        s.storageTexelBuffer = db.storageTexelBufferDescriptorSize;
        s.uniformBuffer = db.uniformBufferDescriptorSize;
        s.storageBuffer = db.storageBufferDescriptorSize;
        s.inputAttachment = db.inputAttachmentDescriptorSize;
        s.accelerationStructure = db.accelerationStructureDescriptorSize;
    }
    std::unique_lock lock(_mutex);
    return _sizes.emplace(key, s).first->second;
}

void DescriptorBufferTracker::OnGetDescriptor(const VkDescriptorGetInfoEXT* info, size_t size, const void* data)
{
    if (!info || !data || !size)
        return;
    // A cap, so an application that remakes its descriptors every frame cannot grow this without
    // bound. The bytes are what identify a descriptor, so the table is naturally deduplicated:
    // asking for the same descriptor again writes the entry it already holds.
    constexpr size_t kMaxDescriptors = 1u << 16;

    DescriptorEntry e;
    e.written = true;
    const ResourceRegistry& reg = ResourceRegistry::Get();
    auto fromAddress = [&](const VkDescriptorAddressInfoEXT* a) {
        if (!a || !a->address)
            return;
        VkDeviceSize offset = 0, remaining = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (!reg.ResolveAddress(a->address, buffer, offset, remaining))
            return;
        e.buffer = buffer;
        e.offset = offset;
        e.range = a->range;
    };
    auto fromImage = [&](const VkDescriptorImageInfo* i, bool withSampler, bool withView) {
        if (!i)
            return;
        if (withView)
            e.imageView = i->imageView;
        if (withSampler)
            e.sampler = i->sampler;
        e.imageLayout = i->imageLayout;
    };

    switch (info->type)
    {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            if (info->data.pSampler)
                e.sampler = *info->data.pSampler;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: fromImage(info->data.pCombinedImageSampler, true, true); break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: fromImage(info->data.pSampledImage, false, true); break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: fromImage(info->data.pStorageImage, false, true); break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: fromImage(info->data.pInputAttachmentImage, false, true); break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: fromAddress(info->data.pUniformTexelBuffer); break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: fromAddress(info->data.pStorageTexelBuffer); break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: fromAddress(info->data.pUniformBuffer); break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: fromAddress(info->data.pStorageBuffer); break;
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            e.accelerationStructure = reg.StructureAt(info->data.accelerationStructure);
            break;
        default: return;
    }

    std::string key((const char*)data, size);
    std::unique_lock lock(_mutex);
    if (_descriptors.size() >= kMaxDescriptors && _descriptors.find(key) == _descriptors.end())
        return;
    _descriptors[std::move(key)] = e;
}

void DescriptorBufferTracker::OnBindBuffers(VkCommandBuffer cb, uint32_t count,
    const VkDescriptorBufferBindingInfoEXT* infos)
{
    if (!cb)
        return;
    const ResourceRegistry& reg = ResourceRegistry::Get();
    std::vector<BoundBuffer> bound(count);
    for (uint32_t i = 0; infos && i < count; ++i)
    {
        VkDeviceSize offset = 0, remaining = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        // The binding names memory by address, so it resolves the same way a ray tracing build's
        // geometry does; a buffer whose address was never asked for cannot be named and stays null.
        if (reg.ResolveAddress(infos[i].address, buffer, offset, remaining))
        {
            bound[i].buffer = buffer;
            bound[i].offset = offset;
        }
    }
    std::unique_lock lock(_mutex);
    _commandBuffers[(uint64_t)(uintptr_t)cb].buffers = std::move(bound);
}

void DescriptorBufferTracker::OnSetOffsets(VkCommandBuffer cb, VkPipelineBindPoint point, uint32_t firstSet,
    uint32_t setCount, const uint32_t* bufferIndices,
    const VkDeviceSize* offsets)
{
    if (!cb)
        return;
    std::unique_lock lock(_mutex);
    std::vector<SetOffset>& sets = _commandBuffers[(uint64_t)(uintptr_t)cb].sets[(int)point];
    if (sets.size() < (size_t)firstSet + setCount)
        sets.resize((size_t)firstSet + setCount);
    for (uint32_t i = 0; i < setCount; ++i)
    {
        SetOffset& s = sets[firstSet + i];
        s.bufferIndex = bufferIndices ? bufferIndices[i] : 0;
        s.offset = offsets ? offsets[i] : 0;
        s.set = true;
    }
}

bool DescriptorBufferTracker::SetSource(VkCommandBuffer cb, VkPipelineBindPoint point, uint32_t set,
    VkBuffer& buffer, VkDeviceSize& offset) const
{
    std::shared_lock lock(_mutex);
    auto c = _commandBuffers.find((uint64_t)(uintptr_t)cb);
    if (c == _commandBuffers.end())
        return false;
    auto s = c->second.sets.find((int)point);
    if (s == c->second.sets.end() || set >= s->second.size() || !s->second[set].set)
        return false;
    const SetOffset& so = s->second[set];
    if (so.bufferIndex >= c->second.buffers.size())
        return false;
    const BoundBuffer& b = c->second.buffers[so.bufferIndex];
    if (!b.buffer)
        return false;
    buffer = b.buffer;
    offset = b.offset + so.offset;
    return true;
}

bool DescriptorBufferTracker::LayoutSize(DeviceData* dev, VkDescriptorSetLayout layout, VkDeviceSize& out) const
{
    if (!dev || !layout || !dev->dispatch.GetDescriptorSetLayoutSizeEXT)
        return false;
    dev->dispatch.GetDescriptorSetLayoutSizeEXT(dev->device, layout, &out);
    return out != 0;
}

bool DescriptorBufferTracker::Decode(DeviceData* dev, VkDescriptorSetLayout layout, const uint8_t* bytes, size_t size,
    DescriptorSetContents& out) const
{
    if (!dev || !bytes || !size)
        return false;
    if (!DescriptorTracker::Get().GetLayout(layout, out))
        return false;
    auto& self = const_cast<DescriptorBufferTracker&>(*this);
    const DescriptorSizes& sizes = self.Sizes(dev);
    if (!sizes.known || !dev->dispatch.GetDescriptorSetLayoutBindingOffsetEXT)
        return false;
    out.layout = layout;

    std::shared_lock lock(_mutex);
    for (DescriptorBinding& b : out.bindings)
    {
        const size_t stride = sizes.For(b.type);
        if (!stride)
            continue;   // a type that holds no descriptor here (inline uniform data)
        VkDeviceSize at = 0;
        dev->dispatch.GetDescriptorSetLayoutBindingOffsetEXT(dev->device, layout, b.binding, &at);
        for (size_t k = 0; k < b.entries.size(); ++k)
        {
            const size_t start = (size_t)at + k * stride;
            if (start + stride > size)
                break;
            // Memory nothing was ever written into reads as zero, which is not a descriptor.
            bool anySet = false;
            for (size_t j = 0; j < stride && !anySet; ++j)
                anySet = bytes[start + j] != 0;
            if (!anySet)
                continue;
            auto it = _descriptors.find(std::string((const char*)bytes + start, stride));
            // A descriptor the layer never saw made stays unwritten: its bytes say nothing about
            // what they name, and a guess here would be a resource the draw may never have read.
            if (it == _descriptors.end())
                continue;
            b.entries[k] = it->second;
        }
    }
    return true;
}

void DescriptorBufferTracker::OnResetCommandBuffer(VkCommandBuffer cb)
{
    std::unique_lock lock(_mutex);
    _commandBuffers.erase((uint64_t)(uintptr_t)cb);
}

void DescriptorBufferTracker::OnDestroy(HandleType type, uint64_t handle)
{
    std::unique_lock lock(_mutex);
    if (type == HT_VkCommandBuffer)
        _commandBuffers.erase(handle);
    else if (type == HT_VkDevice)
        _sizes.erase(handle);
}

} // namespace vkinsp
