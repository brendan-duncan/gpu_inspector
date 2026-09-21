// The in-app HUD: the application's own frame time, drawn over the frame it is about to present.
//
// What it draws and how the text becomes rectangles is in hud_text.h, which has no Vulkan in it
// and is shared with the D3D12 and Metal libraries. This file is only the Vulkan way of putting
// those rectangles on the screen.
//
// Drawn from vkQueuePresentKHR, after the application has finished with the swapchain image and
// transitioned it to VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, and before the present itself. The render
// pass loads the image in that layout and leaves it in that layout, so nothing about the
// application's own rendering changes.
//
// Synchronization is the part worth reading twice. The presentation engine is told to wait on the
// semaphores the application passed in VkPresentInfoKHR, which its rendering signaled -- so
// submitting the overlay on the same queue is *not* enough to order it before the present:
// the semaphore is already signaled by then, and the presentation engine may start reading the
// image while the overlay is still drawing into it. Instead the overlay's submission waits on the
// application's present semaphores and signals one of its own, and the present is given that one
// in their place (Draw returns the rewritten VkPresentInfoKHR). The frame is then ordered
// rendering -> overlay -> present with no queue-wide stall.
//
// Resources are per device and per swapchain, and the ring of command buffers below is what lets
// the overlay be recorded again each frame without waiting for the previous one: a slot is reused
// only once its fence says the GPU has finished with it.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hud_text.h"

namespace vkinsp {

struct DeviceData;

class Hud {
public:
    static Hud& Get();

    /** From the UI (any thread), or VKINSP_HUD at startup. */
    void SetEnabled(bool on);
    bool Enabled() const { return _enabled.load(std::memory_order_relaxed); }

    /**
     * Draws the HUD over the images this present is about to show.
     *
     * Returns true when it drew, in which case `out` is the present info to pass down instead of
     * `in`: the same present with its wait semaphores replaced by the single semaphore the
     * overlay's submission signals. `waits` owns that array for as long as `out` is used.
     *
     * Returns false when there is nothing to draw or anything at all went wrong, and the caller
     * presents `in` unchanged -- a HUD that cannot be drawn must never cost the application its
     * frame.
     */
    bool Draw(DeviceData* dev, VkQueue queue, const VkPresentInfoKHR* in, VkPresentInfoKHR& out,
              std::vector<VkSemaphore>& waits);

    /** The swapchain's images, in index order, from vkGetSwapchainImagesKHR. */
    void OnSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t count, const VkImage* images);
    void OnDestroySwapchain(DeviceData* dev, VkSwapchainKHR swapchain);
    /** Frees everything owned on this device; called before the device itself goes away. */
    void OnDestroyDevice(DeviceData* dev);

private:
    // Everything needed to draw into one swapchain: recreated when the swapchain is.
    struct SwapchainResources {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> framebuffers;
        bool usable = false;      // the swapchain can be drawn into (color attachment usage)
        bool complained = false;  // "cannot draw into this swapchain" has been logged once
    };

    // One in-flight overlay submission. Four of them: enough that the CPU never waits on a fence
    // in practice, since the overlay of frame N has long finished by the time frame N+4 presents.
    struct Frame {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore done = VK_NULL_HANDLE;
        VkBuffer vertices = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
        bool submitted = false;
    };

    struct DeviceResources {
        VkDevice device = VK_NULL_HANDLE;
        VkShaderModule vert = VK_NULL_HANDLE;
        VkShaderModule frag = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        uint32_t family = UINT32_MAX;
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<Frame> frames;
        size_t next = 0;
        std::unordered_map<uint64_t, SwapchainResources> swapchains;
        bool failed = false;   // setup failed once; do not try again on this device
        // When the HUD last drew, which is one fixed point in the application's frame loop and so
        // measures whole frames. See UpdateTiming for why DeviceData::lastPresent does not.
        std::chrono::steady_clock::time_point lastDraw{};
        // FramePause::Generation() at the previous draw. An interval measured across a change in
        // it spans a pause, and is the length of that pause rather than of a frame.
        uint64_t pauseGeneration = 0;
        // Frame interval smoothed over the last second, so the number on the screen is readable
        // rather than flickering every frame, with the extremes of that window beside it.
        double smoothedMs = 0;
        double minMs = 0;
        double maxMs = 0;
        double windowMs = 0;
        uint32_t windowFrames = 0;
        double shownMinMs = 0;
        double shownMaxMs = 0;
    };

    DeviceResources* Resources(DeviceData* dev, VkQueue queue);
    bool EnsureSwapchain(DeviceData* dev, DeviceResources& r, VkSwapchainKHR sc, SwapchainResources*& out);
    bool EnsureVertexBuffer(DeviceData* dev, DeviceResources& r, Frame& f, VkDeviceSize bytes);
    void DestroySwapchain(DeviceData* dev, SwapchainResources& s);
    void UpdateTiming(DeviceData* dev, DeviceResources& r);

    std::atomic<bool> _enabled{false};
    std::mutex _mutex;
    std::unordered_map<VkDevice, DeviceResources> _devices;
    // Swapchain images arrive from vkGetSwapchainImagesKHR, which may be called before the HUD is
    // ever switched on, so they are kept here and picked up when the swapchain is first drawn into.
    std::unordered_map<uint64_t, std::vector<VkImage>> _pendingImages;
};

} // namespace vkinsp
