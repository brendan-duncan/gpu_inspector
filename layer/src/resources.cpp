#include "resources.h"

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
        case HT_VkImage: _images.erase(handle); break;
        case HT_VkImageView: _views.erase(handle); break;
        case HT_VkBuffer: _buffers.erase(handle); break;
        case HT_VkFramebuffer: _framebuffers.erase(handle); break;
        case HT_VkRenderPass: _renderPasses.erase(handle); break;
        case HT_VkSwapchainKHR: _swapchains.erase(handle); break;
        default: break;
    }
}

} // namespace vkinsp
