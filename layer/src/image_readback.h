// Live image readback for the Inspect panel: the UI asks for one mip level / array layer of a
// VkImage (RequestImage), the layer copies it into a host-visible buffer at the next frame
// boundary of the image's device and streams it back (ImageData).
//
// Reading an image requires knowing its current layout. LayoutTracker follows the transitions
// the application records (pipeline barriers, render pass final layouts, dynamic rendering
// attachment layouts) and applies them when the command buffers are submitted. One layout per
// image is kept; per-subresource tracking is not attempted.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vkinsp {

struct DeviceData;

class LayoutTracker {
public:
    static LayoutTracker& Get();

    // Recording side: transitions noted while a command buffer is recorded.
    void OnBeginCommandBuffer(VkCommandBuffer cb);
    void OnFreeCommandBuffer(VkCommandBuffer cb);
    void NoteTransition(VkCommandBuffer cb, VkImage image, VkImageLayout newLayout);
    // Secondary command buffers executed by a primary contribute their transitions.
    void OnExecuteCommands(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries);

    // Submission side: the recorded transitions take effect (in submission order).
    void OnSubmit(uint32_t count, const VkCommandBuffer* cbs);

    bool GetLayout(VkImage image, VkImageLayout& out) const;
    void OnDestroyImage(VkImage image);

private:
    struct Transition {
        VkImage image;
        VkImageLayout layout;
    };
    mutable std::mutex _mutex;
    std::unordered_map<VkCommandBuffer, std::vector<Transition>> _pending;
    std::unordered_map<VkImage, VkImageLayout> _layouts;
};

class ImageReadback {
public:
    static ImageReadback& Get();

    // From the UI (any thread): queue a readback of one subresource of a tracked image.
    void Request(uint64_t imageId, uint32_t mip, uint32_t layer);

    // Called just before vkQueuePresentKHR on the presenting queue: serves the pending requests
    // for that device. The queue is idle from the application's point of view (it is presenting),
    // so the copies are submitted there and waited for synchronously.
    void OnPresent(DeviceData* dev, VkQueue queue);

private:
    struct PendingRequest {
        uint64_t imageId;
        uint32_t mip;
        uint32_t layer;
    };
    void Serve(DeviceData* dev, VkQueue queue, const PendingRequest& r);
    void Fail(const PendingRequest& r, const char* why);

    std::mutex _mutex;
    std::vector<PendingRequest> _pending;
};

} // namespace vkinsp
