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
#include <unordered_map>
#include <vector>

#include "command_recorder.h"

namespace vkinsp {

struct DeviceData;
struct ImageInfo;

struct CaptureOptions {
    uint32_t frameCount = 1;
    // Frame to start at (the device's present counter); UINT64_MAX = the next frame. A frame
    // that has already passed captures the next one.
    uint64_t atFrame = UINT64_MAX;
    uint64_t maxBufferSize = 64 * 1024;       // per captured buffer range (longer ranges are truncated)
    uint64_t maxBufferTotal = 512ull << 20;   // stop capturing buffers past this many bytes per capture
    uint64_t maxTextureSize = 256ull << 20;   // skip render targets (and sampled images) larger than this
    uint64_t maxImageTotal = 256ull << 20;    // stop capturing sampled / storage images past this many bytes
    bool captureTextures = true;
    bool captureBuffers = true;
    // Read back the images bound by descriptor sets (sampled, storage, input attachments), once
    // per image view per capture, so captures show what the shaders sampled.
    bool captureImages = true;
    // Write GPU timestamps around every render pass (VkQueryPool), reported as CapturePassTimings.
    bool profilePasses = true;
    bool stacktraces = false;   // every recorded command carries the stack it was recorded from
};

// GPU timing of one pass: a timestamp query pair, read back when the capture finishes.
struct PassTiming {
    uint32_t frame = UINT32_MAX;
    uint64_t commandBufferId = 0;
    uint32_t passIndex = 0;
    bool compute = false;          // a run of dispatches (its own index sequence) rather than a render pass
    uint32_t query = 0;            // begin query; end is query + 1
    uint32_t statsQuery = UINT32_MAX;  // pipeline statistics query over the pass, or none
    /** Occlusion query over the pass (samples that passed its depth and stencil tests), or none. */
    uint32_t occlusionQuery = UINT32_MAX;
};

// One command buffer executed by a submit, with its frozen command list.
struct SubmittedCommandBuffer {
    uint64_t commandBufferId = 0;
    std::shared_ptr<const CommandList> commands;   // null if the buffer was recorded before capture
};

struct CaptureSubmission {
    uint64_t queueId = 0;
    uint32_t frame = 0;         // frame ordinal within the capture (0-based)
    std::string method;         // vkQueueSubmit / vkQueueSubmit2 / vkQueuePresentKHR
    std::string args;           // serialized arguments
    int64_t result = 0;
    std::vector<SubmittedCommandBuffer> commandBuffers;
};

// A render target captured at the end of a pass (data lands in a staging buffer).
struct TextureCapture {
    uint64_t imageId = 0;
    uint32_t frame = UINT32_MAX;  // frame ordinal, assigned when its command buffer is submitted
    uint64_t commandBufferId = 0;
    uint32_t passIndex = 0;       // per command buffer pass counter
    uint32_t attachment = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, depth = 1, layers = 1, mip = 0;
    uint32_t mips = 1;            // sampled images: mip levels copied (mip .. mip + mips - 1), back to back
    VkDeviceSize size = 0;
    uint32_t samples = 1;         // > 1: a multisampled image, read back through a resolve
    bool resolveTarget = false;   // dynamic rendering: the attachment's resolve target
    uint32_t stagingIndex = 0;    // staging chunk
    VkDeviceSize stagingOffset = 0;
    bool failed = false;
    std::string note;
    // Sampled / storage images bound by descriptor sets (rather than render pass attachments):
    // referenced from the descriptor by `captureId`, copied when the binding pass ends.
    bool sampled = false;
    uint32_t captureId = 0;
    uint64_t viewId = 0;
    uint32_t baseLayer = 0;
    bool recorded = true;         // the copy command has been recorded (false until a deferred copy is flushed)
};

// A buffer range captured when it was bound (descriptor sets, vertex and index buffers,
// indirect arguments). Referenced from the binding command by its id.
struct BufferCapture {
    uint32_t id = 0;
    uint64_t bufferId = 0;
    uint32_t frame = UINT32_MAX;
    uint64_t commandBufferId = 0;   // command buffer the copy was recorded into
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;          // bytes copied
    VkDeviceSize originalSize = 0;  // bytes bound, when truncated to maxBufferSize
    uint32_t stagingIndex = 0;
    VkDeviceSize stagingOffset = 0;
    bool recorded = false;          // the copy command has been recorded
    bool failed = false;
    std::string note;
};

// A single-sampled image of the same format as `img`, sized for its mip `mip` with `layers`
// layers, with TRANSFER_SRC | TRANSFER_DST usage, for resolving a multisampled image before the
// copy to host memory. Device-local memory; the caller frees both once the GPU is done.
bool CreateResolveImage(DeviceData* dev, const ImageInfo& img, uint32_t mip, uint32_t layers, VkImage* image,
                        VkDeviceMemory* memory);

// Records the copy of one image subresource range into a staging buffer: a barrier to
// TRANSFER_SRC, the resolve into `p.resolve` for multisampled images, the copy, and barriers back
// to the image's layout and for host reads of the buffer.
void RecordImageCopy(DeviceData* dev, VkCommandBuffer cb, const PendingImageCopy& p);

class CaptureManager {
public:
    static CaptureManager& Get();

    void Request(const CaptureOptions& options);
    bool IsCapturing() const { return _capturing.load(std::memory_order_relaxed); }
    /** A pass of the capture began its store-everything copy (hooks.cpp), for the finishing log. */
    void NoteStoreAllPass() { _storeAllPasses.fetch_add(1, std::memory_order_relaxed); }

    // When set, every command buffer is recorded even outside captures, so buffers recorded once
    // and resubmitted every frame (vkcube, many engines) still show their contents in a capture.
    void SetRecordAlways(bool on);
    bool RecordAlways() const { return _recordAlways.load(std::memory_order_relaxed); }

    // Hooks (called by generated forwarders / hooks.cpp).
    void OnBeginCommandBuffer(DeviceData* dev, VkCommandBuffer cb, VkCommandBufferUsageFlags flags);
    void OnEndCommandBuffer(DeviceData* dev, VkCommandBuffer cb);
    void OnResetCommandBuffer(DeviceData* dev, VkCommandBuffer cb);
    void OnFreeCommandBuffer(DeviceData* dev, VkCommandBuffer cb);
    CommandRecorder* RecorderFor(DeviceData* dev, VkCommandBuffer cb);

    void OnSubmit(DeviceData* dev, VkQueue queue, const std::string& method, std::string args, int64_t result,
                  const std::vector<VkCommandBuffer>& commandBuffers);
    // A frame ended: after a present (`info`), or without one (info null: the frame-boundary
    // substitutes of layer.cpp).
    void OnFrameEnd(DeviceData* dev, VkQueue queue, const VkPresentInfoKHR* info, VkResult result);

    // Render pass boundaries (recording time): note attachments, and at end inject readback copies.
    // OnBeforePass runs before the begin command (pre-hook): it resets a query pair and writes the
    // pass's begin timestamp, which must happen outside the render pass.
    void OnBeforePass(DeviceData* dev, CommandRecorder* rec, bool multiview = false);
    /**
     * Ends the pass's occlusion query early and throws its result away: the application is about to
     * begin a query of its own, or to execute secondary command buffers, neither of which is valid
     * while ours is active. The pass keeps its timing and its other counters.
     */
    void DropOcclusion(DeviceData* dev, CommandRecorder* rec);
    void OnBeginRenderPass(DeviceData* dev, CommandRecorder* rec, const VkRenderPassBeginInfo* info);
    void OnBeginRendering(DeviceData* dev, CommandRecorder* rec, const VkRenderingInfo* info);
    void OnEndPass(DeviceData* dev, CommandRecorder* rec);
    // Compute passes: a dispatch outside a render pass opens one (pre-hook, so the begin
    // timestamp precedes the dispatch); barriers, event waits, render passes, debug labels,
    // secondary execution and the end of the command buffer close it (pre-hooks as well).
    void OnBeforeDispatch(DeviceData* dev, CommandRecorder* rec);
    void OnEndComputePass(DeviceData* dev, CommandRecorder* rec);
    // A primary executing secondaries takes over their pending buffer copies (recorded inside a
    // render pass, they can only be flushed by the primary at the end of that pass).
    void OnExecuteCommands(DeviceData* dev, CommandRecorder* rec, uint32_t count, const VkCommandBuffer* secondaries);

    // Queues a readback of [offset, offset + size) of a buffer bound by the command being
    // recorded. Returns the capture id to reference from the command's JSON, or 0 when nothing is
    // captured (no capture in progress, buffers disabled, empty range, budget exhausted).
    uint32_t QueueBufferCapture(DeviceData* dev, CommandRecorder* rec, VkBuffer buffer, VkDeviceSize offset,
                                VkDeviceSize size);
    // Queues a readback of the subresource an image view covers (its base mip, all its layers),
    // once per view per capture. Returns the texture capture id to reference from the descriptor
    // JSON, or 0 when nothing is captured. `layout` is the layout the descriptor promises.
    uint32_t QueueImageCapture(DeviceData* dev, CommandRecorder* rec, VkImageView view, VkImageLayout layout);

private:
    CaptureManager() = default;

    enum class State { Idle, Armed, Capturing };

    void Start(DeviceData* dev);
    void Finish(DeviceData* dev);
    void SendCommands();
    void SendTextures(DeviceData* dev);
    void SendBuffers(DeviceData* dev);
    void SendPassTimings(DeviceData* dev);
    void ReleaseStaging(DeviceData* dev);
    void FlushBufferCopies(DeviceData* dev, CommandRecorder* rec);
    void FlushImageCopies(DeviceData* dev, CommandRecorder* rec);
    void EnsureQueryPool(DeviceData* dev);
    void ReleaseQueryPool(DeviceData* dev);
    // Resets a query pair and writes its begin timestamp; UINT32_MAX when not profiling.
    uint32_t BeginTimestamp(DeviceData* dev, CommandRecorder* rec);
    // Resets and begins a pipeline statistics query over a render pass; UINT32_MAX when the
    // device has no such pool. Render passes only: the statistics include graphics stages, which
    // a compute-only queue may not support.
    uint32_t BeginPipelineStatistics(DeviceData* dev, CommandRecorder* rec);
    /**
     * An occlusion query over the pass: the samples that survived its depth and stencil tests,
     * which is what the `late-depth-rejection` rule weighs against the fragments shaded. Skipped
     * where the application has a query of its own open, since two cannot be active at once.
     */
    uint32_t BeginOcclusion(DeviceData* dev, CommandRecorder* rec);

    // Staging memory for readbacks, allocated on demand during the captured frame.
    struct StagingChunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
        void* mapped = nullptr;
    };
    bool AllocateStaging(DeviceData* dev, VkDeviceSize size, uint32_t& chunkIndex, VkDeviceSize& offset,
                         VkBuffer* bufferOut = nullptr);
    void CaptureAttachment(DeviceData* dev, CommandRecorder* rec, uint32_t attachmentIndex, VkImageView view,
                           VkImageLayout layout, bool resolveTarget = false);
    // The capture record and the copy for one attachment (staging, resolve image); false with the
    // failed record already listed. The copy is recorded by the caller into its command buffer.
    bool PrepareAttachment(DeviceData* dev, uint64_t commandBufferId, uint32_t passIndex, uint32_t layerCount,
                           uint32_t attachmentIndex, VkImageView view, VkImageLayout layout, bool resolveTarget,
                           TextureCapture& tc, PendingImageCopy& p);
    // Attachments of the passes a submitted command buffer recorded before the capture began,
    // copied by a command buffer of the layer's submitted right after the application's.
    void ReadBackAfterSubmit(DeviceData* dev, VkQueue queue, CommandRecorder* rec, uint64_t commandBufferId, uint32_t frame);
    // Single-sampled images that multisampled captures are resolved into; freed with the staging.
    bool AllocateResolveImage(DeviceData* dev, const ImageInfo& img, uint32_t mip, uint32_t layers, VkImage* out);
    struct ResolveImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    std::vector<ResolveImage> _resolveImages;
    // Views of a depth resolve (depth_resolve.h), freed with the staging.
    std::vector<VkImageView> _resolveViews;
    bool PrepareDepthResolve(DeviceData* dev, PendingImageCopy& p);

    mutable std::mutex _mutex;
    std::atomic<bool> _capturing{false};
    std::atomic<uint32_t> _storeAllPasses{0};
    std::atomic<uint32_t> _postSubmitReadbacks{0};
    std::atomic<bool> _recordAlways{false};
    // Frame an armed capture waits for (UINT64_MAX when not armed or waiting for the next present),
    // checked cheaply at every vkBeginCommandBuffer so frame 0 can be captured from its first command.
    std::atomic<uint64_t> _armedAtFrame{UINT64_MAX};
    State _state = State::Idle;
    CaptureOptions _options;
    uint32_t _framesLeft = 0;
    uint32_t _frameCount = 1;
    uint64_t _frameIndex = 0;

    std::vector<CaptureSubmission> _submissions;
    std::vector<TextureCapture> _textures;
    std::vector<BufferCapture> _buffers;
    uint64_t _bufferBytes = 0;
    uint32_t _nextBufferId = 1;
    // Sampled image captures: one per image view per capture, and the bytes taken so far.
    std::unordered_map<uint64_t, uint32_t> _imageCaptureByView;
    uint64_t _imageBytes = 0;

    // Pass profiling: one timestamp query pool per capture (created on the capturing device),
    // and beside it a pipeline statistics pool when the device has the feature (pipeline_stats.h).
    VkQueryPool _queryPool = VK_NULL_HANDLE;
    VkDevice _queryDevice = VK_NULL_HANDLE;
    uint32_t _queryCount = 0;
    std::atomic<uint32_t> _queriesUsed{0};
    VkQueryPool _statsPool = VK_NULL_HANDLE;
    uint32_t _statsCount = 0;
    std::atomic<uint32_t> _statsUsed{0};
    VkQueryPool _occlusionPool = VK_NULL_HANDLE;
    uint32_t _occlusionCount = 0;
    std::atomic<uint32_t> _occlusionUsed{0};
    /** Passes whose occlusion query had to end early (an application query, or secondaries). */
    std::atomic<uint32_t> _occlusionDropped{0};
    std::vector<PassTiming> _passTimings;
    std::vector<StagingChunk> _staging;
    uint64_t _commandTotal = 0;
};

} // namespace vkinsp
