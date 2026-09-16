#include "resources.h"

#include <algorithm>

#include "descriptors.h"
#include "image_readback.h"
#include "shader_edit.h"

namespace vkinsp {

ResourceRegistry& ResourceRegistry::Get() {
    static ResourceRegistry* instance = new ResourceRegistry();
    return *instance;
}

#define VKINSP_KEY(h) ((uint64_t)(uintptr_t)(h))

void ResourceRegistry::AddImage(VkImage image, const ImageInfo& info) {
    std::unique_lock lock(_mutex);
    _images[VKINSP_KEY(image)] = info;
}

void ResourceRegistry::AddImageView(VkImageView view, const ImageViewInfo& info) {
    std::unique_lock lock(_mutex);
    _views[VKINSP_KEY(view)] = info;
}

void ResourceRegistry::AddBuffer(VkBuffer buffer, const BufferInfo& info) {
    std::unique_lock lock(_mutex);
    _buffers[VKINSP_KEY(buffer)] = info;
}

void ResourceRegistry::AddFramebuffer(VkFramebuffer fb, const FramebufferInfo& info) {
    std::unique_lock lock(_mutex);
    _framebuffers[VKINSP_KEY(fb)] = info;
}

void ResourceRegistry::AddRenderPass(VkRenderPass rp, const RenderPassInfo& info) {
    std::unique_lock lock(_mutex);
    _renderPasses[VKINSP_KEY(rp)] = info;
}

void ResourceRegistry::AddSwapchain(VkSwapchainKHR sc, const SwapchainInfo& info) {
    std::unique_lock lock(_mutex);
    _swapchains[VKINSP_KEY(sc)] = info;
}

template <typename M, typename T>
static bool Lookup(const M& m, uint64_t key, T& out) {
    auto it = m.find(key);
    if (it == m.end()) return false;
    out = it->second;
    return true;
}

bool ResourceRegistry::GetImage(VkImage image, ImageInfo& out) const {
    std::shared_lock lock(_mutex);
    return Lookup(_images, VKINSP_KEY(image), out);
}

bool ResourceRegistry::GetImageView(VkImageView view, ImageViewInfo& out) const {
    std::shared_lock lock(_mutex);
    return Lookup(_views, VKINSP_KEY(view), out);
}

bool ResourceRegistry::GetBuffer(VkBuffer buffer, BufferInfo& out) const {
    std::shared_lock lock(_mutex);
    return Lookup(_buffers, VKINSP_KEY(buffer), out);
}

bool ResourceRegistry::GetFramebuffer(VkFramebuffer fb, FramebufferInfo& out) const {
    std::shared_lock lock(_mutex);
    return Lookup(_framebuffers, VKINSP_KEY(fb), out);
}

bool ResourceRegistry::GetRenderPass(VkRenderPass rp, RenderPassInfo& out) const {
    std::shared_lock lock(_mutex);
    return Lookup(_renderPasses, VKINSP_KEY(rp), out);
}

bool ResourceRegistry::GetSwapchain(VkSwapchainKHR sc, SwapchainInfo& out) const {
    std::shared_lock lock(_mutex);
    return Lookup(_swapchains, VKINSP_KEY(sc), out);
}

void ResourceRegistry::OnDestroy(HandleType type, uint64_t handle) {
    std::unique_lock lock(_mutex);
    switch (type) {
        case HT_VkImage:
            _images.erase(handle);
            LayoutTracker::Get().OnDestroyImage((VkImage)(uintptr_t)handle);
            break;
        case HT_VkImageView: _views.erase(handle); break;
        case HT_VkBuffer: {
            _buffers.erase(handle);
            // The address goes with the buffer: the allocator hands the same range out again, and
            // a stale entry would resolve a later build to a buffer that no longer exists.
            const VkBuffer buffer = (VkBuffer)(uintptr_t)handle;
            _addresses.erase(std::remove_if(_addresses.begin(), _addresses.end(),
                                            [&](const AddressRange& r) { return r.buffer == buffer; }),
                             _addresses.end());
            break;
        }
        case HT_VkFramebuffer: _framebuffers.erase(handle); break;
        case HT_VkRenderPass: _renderPasses.erase(handle); break;
        case HT_VkSwapchainKHR: _swapchains.erase(handle); break;
        case HT_VkAccelerationStructureKHR: _structureAddresses.erase(handle); break;
        case HT_VkDescriptorSet:
        case HT_VkDescriptorSetLayout:
        case HT_VkDescriptorUpdateTemplate:
            DescriptorTracker::Get().OnDestroy(type, handle);
            break;
        case HT_VkPipeline:
            ShaderEditor::Get().OnDestroyPipeline(handle);
            break;
        case HT_VkShaderEXT:
            ShaderEditor::Get().OnDestroyShader(handle);
            break;
        default: break;
    }
}


// ---------------------------------------------------------------------------------------------
// Device addresses

void ResourceRegistry::NoteBufferAddress(VkBuffer buffer, VkDeviceAddress address) {
    if (!buffer || !address) return;
    std::unique_lock lock(_mutex);
    auto it = _buffers.find((uint64_t)(uintptr_t)buffer);
    if (it == _buffers.end()) return;
    it->second.address = address;
    const AddressRange range{address, it->second.size, buffer};
    // Kept sorted by address so a resolve is a binary search. An application asks for an address
    // once per buffer, so this is a handful of insertions rather than a hot path.
    auto at = std::lower_bound(_addresses.begin(), _addresses.end(), address,
                               [](const AddressRange& r, VkDeviceAddress a) { return r.address < a; });
    if (at != _addresses.end() && at->address == address) *at = range;
    else _addresses.insert(at, range);
}

void ResourceRegistry::NoteStructureAddress(VkAccelerationStructureKHR structure, VkDeviceAddress address) {
    if (!structure || !address) return;
    std::unique_lock lock(_mutex);
    _structureAddresses[(uint64_t)address] = structure;
}

bool ResourceRegistry::ResolveAddress(VkDeviceAddress address, VkBuffer& buffer, VkDeviceSize& offset,
                                      VkDeviceSize& remaining) const {
    if (!address) return false;
    std::shared_lock lock(_mutex);
    // The last range beginning at or before the address is the only one that can contain it.
    auto at = std::upper_bound(_addresses.begin(), _addresses.end(), address,
                               [](VkDeviceAddress a, const AddressRange& r) { return a < r.address; });
    if (at == _addresses.begin()) return false;
    --at;
    if (address >= at->address + at->size) return false;   // past the end of the nearest buffer
    buffer = at->buffer;
    offset = address - at->address;
    remaining = at->size - offset;
    return true;
}

VkAccelerationStructureKHR ResourceRegistry::StructureAt(VkDeviceAddress address) const {
    if (!address) return VK_NULL_HANDLE;
    std::shared_lock lock(_mutex);
    auto it = _structureAddresses.find((uint64_t)address);
    return it == _structureAddresses.end() ? VK_NULL_HANDLE : it->second;
}

} // namespace vkinsp
