#include "image_readback.h"

#include "format_info.h"
#include "layer.h"
#include "resources.h"
#include "tracker.h"
#include "transport.h"
#include "vk_serialize.gen.h"

#include <algorithm>
#include <cstring>

namespace vkinsp {

// ---------------------------------------------------------------------------------------------
// LayoutTracker

LayoutTracker& LayoutTracker::Get() {
    static LayoutTracker* instance = new LayoutTracker();
    return *instance;
}

void LayoutTracker::OnBeginCommandBuffer(VkCommandBuffer cb) {
    std::lock_guard lock(_mutex);
    _pending[cb].clear();
}

void LayoutTracker::OnFreeCommandBuffer(VkCommandBuffer cb) {
    std::lock_guard lock(_mutex);
    _pending.erase(cb);
}

void LayoutTracker::NoteTransition(VkCommandBuffer cb, VkImage image, VkImageLayout newLayout) {
    if (!image || newLayout == VK_IMAGE_LAYOUT_UNDEFINED) return;
    std::lock_guard lock(_mutex);
    _pending[cb].push_back({image, newLayout});
}

void LayoutTracker::OnExecuteCommands(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries) {
    std::lock_guard lock(_mutex);
    auto& dst = _pending[primary];
    for (uint32_t i = 0; secondaries && i < count; ++i) {
        auto it = _pending.find(secondaries[i]);
        if (it != _pending.end()) dst.insert(dst.end(), it->second.begin(), it->second.end());
    }
}

void LayoutTracker::OnSubmit(uint32_t count, const VkCommandBuffer* cbs) {
    std::lock_guard lock(_mutex);
    for (uint32_t i = 0; cbs && i < count; ++i) {
        auto it = _pending.find(cbs[i]);
        if (it == _pending.end()) continue;
        for (const Transition& t : it->second) _layouts[t.image] = t.layout;
    }
}

bool LayoutTracker::GetLayout(VkImage image, VkImageLayout& out) const {
    std::lock_guard lock(_mutex);
    auto it = _layouts.find(image);
    if (it == _layouts.end()) return false;
    out = it->second;
    return true;
}

void LayoutTracker::OnDestroyImage(VkImage image) {
    std::lock_guard lock(_mutex);
    _layouts.erase(image);
}

// ---------------------------------------------------------------------------------------------
// ImageReadback

ImageReadback& ImageReadback::Get() {
    static ImageReadback* instance = new ImageReadback();
    return *instance;
}

void ImageReadback::Request(uint64_t imageId, uint32_t mip, uint32_t layer) {
    std::lock_guard lock(_mutex);
    // A newer request for the same image replaces the older one.
    _pending.erase(std::remove_if(_pending.begin(), _pending.end(), [&](const PendingRequest& r) { return r.imageId == imageId; }),
                   _pending.end());
    _pending.push_back({imageId, mip, layer});
}

void ImageReadback::OnPresent(DeviceData* dev, VkQueue queue) {
    std::vector<PendingRequest> requests;
    {
        std::lock_guard lock(_mutex);
        if (_pending.empty()) return;
        requests.swap(_pending);
    }
    std::vector<PendingRequest> otherDevice;
    for (const PendingRequest& r : requests) {
        TrackedObject obj;
        ImageInfo img;
        if (Tracker::Get().FindById(r.imageId, obj) && obj.type == HT_VkImage &&
            ResourceRegistry::Get().GetImage((VkImage)(uintptr_t)obj.handle, img) && img.device != dev->device) {
            otherDevice.push_back(r);  // served when that device presents
            continue;
        }
        Serve(dev, queue, r);
    }
    if (!otherDevice.empty()) {
        std::lock_guard lock(_mutex);
        _pending.insert(_pending.end(), otherDevice.begin(), otherDevice.end());
    }
}

static void WriteHeader(JsonWriter& w, const ImageReadback* self, uint64_t id, uint32_t mip, uint32_t layer,
                        const ImageInfo* img, VkImageAspectFlags aspect, uint32_t width, uint32_t height,
                        uint32_t depth, uint64_t size, const char* error) {
    (void)self;
    w.BeginObject();
    w.Key("action"); w.String("ImageData");
    w.Key("id"); w.Uint(id);
    w.Key("mip"); w.Uint(mip);
    w.Key("layer"); w.Uint(layer);
    w.Key("width"); w.Uint(width);
    w.Key("height"); w.Uint(height);
    w.Key("depth"); w.Uint(depth);
    w.Key("layers"); w.Uint(1);
    if (img) {
        w.Key("format"); w.Enum(ToString_VkFormat(img->format), (int64_t)img->format);
    } else {
        w.Key("format"); w.String("VK_FORMAT_UNDEFINED");
    }
    w.Key("aspect"); w.String(aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? "depth" : aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? "stencil" : "color");
    w.Key("size"); w.Uint(size);
    if (error) { w.Key("error"); w.String(error); }
    w.EndObject();
}

void ImageReadback::Fail(const PendingRequest& r, const char* why) {
    JsonWriter w;
    WriteHeader(w, this, r.imageId, r.mip, r.layer, nullptr, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0, why);
    Transport::Get().SendJson(std::move(w.str()));
    Log("image readback %llu failed: %s", (unsigned long long)r.imageId, why);
}

static int FindHostMemoryType(DeviceData* dev, uint32_t typeBits) {
    for (int pass = 0; pass < 2; ++pass) {
        VkMemoryPropertyFlags want = pass == 0
            ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for (uint32_t i = 0; i < dev->memoryProperties.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (dev->memoryProperties.memoryTypes[i].propertyFlags & want) == want) return (int)i;
        }
    }
    return -1;
}

void ImageReadback::Serve(DeviceData* dev, VkQueue queue, const PendingRequest& r) {
    TrackedObject obj;
    if (!Tracker::Get().FindById(r.imageId, obj) || obj.type != HT_VkImage) return Fail(r, "image no longer exists");
    VkImage image = (VkImage)(uintptr_t)obj.handle;
    ImageInfo img;
    if (!ResourceRegistry::Get().GetImage(image, img)) return Fail(r, "image is not tracked");
    if (!img.transferSrc) return Fail(r, "image lacks TRANSFER_SRC usage");
    if (img.samples != VK_SAMPLE_COUNT_1_BIT) return Fail(r, "multisampled image (resolve not implemented yet)");

    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!LayoutTracker::Get().GetLayout(image, layout)) {
        if (img.swapchainImage) layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        else return Fail(r, "image layout unknown (no transition seen yet)");
    }
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED)
        return Fail(r, "image contents are undefined");

    const uint32_t mip = std::min(r.mip, img.mipLevels - 1);
    const uint32_t layer = std::min(r.layer, img.arrayLayers - 1);
    const uint32_t width = std::max(1u, img.extent.width >> mip);
    const uint32_t height = std::max(1u, img.extent.height >> mip);
    const uint32_t depth = std::max(1u, img.extent.depth >> mip);
    VkImageAspectFlags aspects = FormatAspects(img.format);
    VkImageAspectFlags aspect = aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT
                              : aspects & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT
                              : VK_IMAGE_ASPECT_COLOR_BIT;
    FormatBlock block = FormatBlockInfo(img.format, aspect);
    if (block.bytes == 0) return Fail(r, "unsupported format for readback");
    const VkDeviceSize size = (VkDeviceSize)((width + block.width - 1) / block.width) *
                              ((height + block.height - 1) / block.height) * depth * block.bytes;
    if (size > (256ull << 20)) return Fail(r, "image is larger than 256 MB");

    // Queue family of the presenting queue: needed for the command pool.
    uint32_t family = 0;
    {
        std::lock_guard lock(dev->queueMutex);
        auto it = dev->queueFamilies.find(queue);
        if (it == dev->queueFamilies.end()) return Fail(r, "unknown queue family for the present queue");
        family = it->second;
    }
    VkCommandPool pool = VK_NULL_HANDLE;
    {
        std::lock_guard lock(dev->queueMutex);
        auto it = dev->readbackPools.find(family);
        if (it != dev->readbackPools.end()) pool = it->second;
    }
    const DeviceDispatch& d = dev->dispatch;
    if (!pool) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = family;
        if (d.CreateCommandPool(dev->device, &pci, nullptr, &pool) != VK_SUCCESS) return Fail(r, "command pool creation failed");
        std::lock_guard lock(dev->queueMutex);
        dev->readbackPools[family] = pool;
    }

    // Host-visible staging buffer.
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d.CreateBuffer(dev->device, &bci, nullptr, &buffer) != VK_SUCCESS) return Fail(r, "staging buffer creation failed");
    VkMemoryRequirements req;
    d.GetBufferMemoryRequirements(dev->device, buffer, &req);
    int typeIndex = FindHostMemoryType(dev, req.memoryTypeBits);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)std::max(0, typeIndex);
    if (typeIndex < 0 || d.AllocateMemory(dev->device, &mai, nullptr, &memory) != VK_SUCCESS) {
        d.DestroyBuffer(dev->device, buffer, nullptr);
        return Fail(r, "staging memory allocation failed");
    }
    d.BindBufferMemory(dev->device, buffer, memory, 0);

    auto cleanup = [&]() {
        d.DestroyBuffer(dev->device, buffer, nullptr);
        d.FreeMemory(dev->device, memory, nullptr);
    };

    // Record: barrier to TRANSFER_SRC, copy, barrier back to the tracked layout.
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (d.AllocateCommandBuffers(dev->device, &ai, &cb) != VK_SUCCESS) {
        cleanup();
        return Fail(r, "command buffer allocation failed");
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.BeginCommandBuffer(cb, &bi);

    VkImageSubresourceRange range{aspects, mip, 1, layer, 1};
    VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = layout;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = image;
    toSrc.subresourceRange = range;
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {aspect, mip, layer, 1};
    region.imageExtent = {width, height, depth};
    d.CmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

    VkImageMemoryBarrier back = toSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = layout;
    VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.buffer = buffer;
    hostRead.size = VK_WHOLE_SIZE;
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &hostRead, 1, &back);
    d.EndCommandBuffer(cb);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    d.CreateFence(dev->device, &fci, nullptr, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult res = d.QueueSubmit(queue, 1, &si, fence);
    if (res == VK_SUCCESS) res = d.WaitForFences(dev->device, 1, &fence, VK_TRUE, 5000000000ull);
    d.DestroyFence(dev->device, fence, nullptr);
    d.FreeCommandBuffers(dev->device, pool, 1, &cb);
    if (res != VK_SUCCESS) {
        cleanup();
        return Fail(r, res == VK_TIMEOUT ? "readback timed out" : "readback submit failed");
    }

    void* mapped = nullptr;
    if (d.MapMemory(dev->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) {
        cleanup();
        return Fail(r, "staging memory map failed");
    }
    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mr.memory = memory;
    mr.size = VK_WHOLE_SIZE;
    d.InvalidateMappedMemoryRanges(dev->device, 1, &mr);

    JsonWriter h;
    WriteHeader(h, this, r.imageId, mip, layer, &img, aspect, width, height, depth, size, nullptr);
    Transport::Get().SendBinary(std::move(h.str()), mapped, (size_t)size);
    d.UnmapMemory(dev->device, memory);
    cleanup();
    Log("image readback %llu: mip %u layer %u %ux%u (%llu bytes)", (unsigned long long)r.imageId, mip, layer, width, height,
        (unsigned long long)size);
}

} // namespace vkinsp
