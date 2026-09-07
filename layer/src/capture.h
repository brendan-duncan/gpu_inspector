// Frame capture: records every command buffer recorded during the captured frame(s), freezes
// them at submit, and streams the resulting command list (plus resource readbacks) to the UI.
//
// State machine (driven by vkQueuePresentKHR, the frame boundary):
//   Idle --Capture request--> Armed --present--> Capturing --present x N--> finish --> Idle
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "command_recorder.h"

namespace vkinsp {

struct DeviceData;

struct CaptureOptions {
    uint32_t frameCount = 1;
    uint64_t maxBufferSize = 64 * 1024;       // per captured buffer range
    uint64_t maxTextureSize = 256ull << 20;   // skip render targets larger than this
    bool captureTextures = true;
};

// One command buffer executed by a submit, with its frozen command list.
struct SubmittedCommandBuffer {
    uint64_t commandBufferId = 0;
    std::shared_ptr<const CommandList> commands;   // null if the buffer was recorded before capture
};

struct CaptureSubmission {
    uint64_t queueId = 0;
    std::string method;         // vkQueueSubmit / vkQueueSubmit2 / vkQueuePresentKHR
    std::string args;           // serialized arguments
    int64_t result = 0;
    std::vector<SubmittedCommandBuffer> commandBuffers;
};

// A render target captured at the end of a pass (data lands in a staging buffer).
struct TextureCapture {
    uint64_t imageId = 0;
    uint64_t commandBufferId = 0;
    uint32_t passIndex = 0;       // per command buffer pass counter
    uint32_t attachment = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, depth = 1, layers = 1, mip = 0;
    VkDeviceSize size = 0;
    uint32_t stagingIndex = 0;    // staging chunk
    VkDeviceSize stagingOffset = 0;
    bool failed = false;
    std::string note;
};

class CaptureManager {
public:
    static CaptureManager& Get();

    void Request(const CaptureOptions& options);
    bool IsCapturing() const { return _capturing.load(std::memory_order_relaxed); }

    // When set, every command buffer is recorded even outside captures, so buffers recorded once
    // and resubmitted every frame (vkcube, many engines) still show their contents in a capture.
    void SetRecordAlways(bool on);
    bool RecordAlways() const { return _recordAlways.load(std::memory_order_relaxed); }

    // Hooks (called by generated forwarders / hooks.cpp).
    void OnBeginCommandBuffer(DeviceData* dev, VkCommandBuffer cb);
    void OnEndCommandBuffer(DeviceData* dev, VkCommandBuffer cb);
    void OnResetCommandBuffer(DeviceData* dev, VkCommandBuffer cb);
    void OnFreeCommandBuffer(DeviceData* dev, VkCommandBuffer cb);
    CommandRecorder* RecorderFor(DeviceData* dev, VkCommandBuffer cb);

    void OnSubmit(DeviceData* dev, VkQueue queue, const std::string& method, std::string args, int64_t result,
                  const std::vector<VkCommandBuffer>& commandBuffers);
    void OnPresent(DeviceData* dev, VkQueue queue, const VkPresentInfoKHR* info, VkResult result);

    // Render pass boundaries (recording time): note attachments, and at end inject readback copies.
    void OnBeginRenderPass(DeviceData* dev, CommandRecorder* rec, const VkRenderPassBeginInfo* info);
    void OnBeginRendering(DeviceData* dev, CommandRecorder* rec, const VkRenderingInfo* info);
    void OnEndPass(DeviceData* dev, CommandRecorder* rec);

private:
    CaptureManager() = default;

    enum class State { Idle, Armed, Capturing };

    void Start(DeviceData* dev);
    void Finish(DeviceData* dev);
    void SendCommands();
    void SendTextures(DeviceData* dev);
    void ReleaseStaging(DeviceData* dev);

    // Staging memory for readbacks, allocated on demand during the captured frame.
    struct StagingChunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
        void* mapped = nullptr;
    };
    bool AllocateStaging(DeviceData* dev, VkDeviceSize size, uint32_t& chunkIndex, VkDeviceSize& offset);
    void CaptureAttachment(DeviceData* dev, CommandRecorder* rec, uint32_t attachmentIndex, VkImageView view,
                           VkImageLayout layout);

    mutable std::mutex _mutex;
    std::atomic<bool> _capturing{false};
    std::atomic<bool> _recordAlways{false};
    State _state = State::Idle;
    CaptureOptions _options;
    uint32_t _framesLeft = 0;
    uint64_t _frameIndex = 0;

    std::vector<CaptureSubmission> _submissions;
    std::vector<TextureCapture> _textures;
    std::vector<StagingChunk> _staging;
    uint64_t _commandTotal = 0;
};

} // namespace vkinsp
