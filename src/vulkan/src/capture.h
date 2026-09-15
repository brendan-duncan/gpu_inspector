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
#include "resources.h"

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
    VkDevice device = VK_NULL_HANDLE;   // whose query pools hold the results
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
    VkDevice device = VK_NULL_HANDLE;   // whose staging chunk holds the data
    uint32_t stagingIndex = 0;    // staging chunk of that device
    VkDeviceSize stagingOffset = 0;
    bool failed = false;
    std::string note;
    // Sampled / storage images bound by descriptor sets (rather than render pass attachments):
    // referenced from the descriptor by `captureId`, copied when the binding pass ends.
    bool sampled = false;
    // What an image held when the frame first read it (CaptureManager::SnapshotImageRead):
    // referenced from the reading command's "imageData" by `captureId`, one mip per capture.
    bool initial = false;
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
    VkDevice device = VK_NULL_HANDLE;   // whose staging chunk holds the data
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

/**
 * What about a render pass about to begin limits what the capture can record around it (the
 * pre-hooks of hooks.cpp work it out from the begin info). `multiview`: a query active across it
 * writes one result per view. `secondaries`: it may execute secondary command buffers, which it can
 * only do with queries active when the device inherits queries. `suspending` / `resuming`: dynamic
 * rendering split across command buffers, between whose parts nothing may be recorded at all.
 */
struct PassShape {
    bool multiview = false;
    bool secondaries = false;
    bool suspending = false;
    bool resuming = false;
};

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

    // `readBack`: command buffers of the submission whose attachments ReadBackSubmitted already read.
    void OnSubmit(DeviceData* dev, VkQueue queue, const std::string& method, std::string args, int64_t result,
                  const std::vector<VkCommandBuffer>& commandBuffers, const std::vector<VkCommandBuffer>& readBack = {});
    /**
     * A command buffer recorded before the capture began, whose passes hold no read-back copies: its
     * attachments are read after it runs. The submit hooks split a submission after each such buffer
     * (hooks.cpp), so a later buffer of the same submission cannot overwrite what it rendered first.
     */
    bool NeedsSubmitReadBack(DeviceData* dev, VkCommandBuffer cb);
    // Reads back such a buffer's attachments, once the part of the submission holding it was submitted.
    void ReadBackSubmitted(DeviceData* dev, VkQueue queue, VkCommandBuffer cb);
    // A frame ended: after a present (`info`), or without one (info null: the frame-boundary
    // substitutes of layer.cpp). Only the device the capture started on counts the captured
    // frames; another device's present is recorded in the frame that device is in.
    void OnFrameEnd(DeviceData* dev, VkQueue queue, const VkPresentInfoKHR* info, VkResult result);
    // A device is about to be destroyed: a capture started on it finishes now, and a capture
    // using it as well releases what it holds on it (its read-backs are reported as lost).
    void OnDestroyDevice(DeviceData* dev);

    // Render pass boundaries (recording time): note attachments, and at end inject readback copies.
    // OnBeforePass runs before the begin command (pre-hook): it resets a query pair and writes the
    // pass's begin timestamp, which must happen outside the render pass.
    void OnBeforePass(DeviceData* dev, CommandRecorder* rec, const PassShape& shape = {});
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
    // `whole`: not truncated to maxBufferSize (the source of a copy, which a replay must write whole).
    uint32_t QueueBufferCapture(DeviceData* dev, CommandRecorder* rec, VkBuffer buffer, VkDeviceSize offset,
                                VkDeviceSize size, bool whole = false);
    // Queues a readback of the subresource an image view covers (its base mip, all its layers),
    // once per view per capture. Returns the texture capture id to reference from the descriptor
    // JSON, or 0 when nothing is captured. `layout` is the layout the descriptor promises.
    uint32_t QueueImageCapture(DeviceData* dev, CommandRecorder* rec, VkImageView view, VkImageLayout layout);

    // Frame-start contents. A capture only reads back what the frame shows on its way (render
    // targets, bound buffers and images), which is not enough to replay it: a pass that loads an
    // attachment, or a copy from an image, reads what earlier frames left there. The first read of
    // each subresource in a capture that nothing in the capture wrote whole before takes a copy of
    // it (texture kind "initial"), recorded before the reading command, outside a render pass. The
    // ids go into `ids`, for the command's "imageData". Stencil and multisampled contents are not
    // taken (neither can be read back into something a replay could upload).
    void SnapshotImageRead(DeviceData* dev, CommandRecorder* rec, VkImage image, VkImageAspectFlags aspect, uint32_t baseMip,
                           uint32_t mipCount, uint32_t baseLayer, uint32_t layerCount, VkImageLayout layout,
                           std::vector<uint32_t>& ids);
    // A write that replaces whole subresources (a clear, a copy over the whole extent, a pass that
    // does not load and renders everywhere): a later read of them in the capture takes no copy.
    void NoteImageWrite(VkImage image, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer,
                        uint32_t layerCount);
    // A render pass attachment about to begin: a snapshot when it loads, a whole write when it
    // does not and the render area covers it.
    void OnAttachmentBegin(DeviceData* dev, CommandRecorder* rec, VkImageView view, VkImageAspectFlags aspects, VkAttachmentLoadOp loadOp,
                           VkImageLayout layout, const VkRect2D& renderArea, std::vector<uint32_t>& ids);

private:
    CaptureManager() = default;

    enum class State { Idle, Armed, Capturing };

    void Start(DeviceData* dev);
    void Finish(DeviceData* dev);
    void SendCommands();
    void SendTextures(DeviceData* dev);
    void SendBuffers(DeviceData* dev);
    void SendPassTimings();
    void FlushBufferCopies(DeviceData* dev, CommandRecorder* rec);
    void FlushImageCopies(DeviceData* dev, CommandRecorder* rec);
    // The capture record and pending copies of `tc.mip` .. + `mips` of an image (tc carries the
    // kind, the first layer, the layer count and the aspect); the texture capture id, of a failed
    // record when the copy cannot be made.
    uint32_t QueueImageCopy(DeviceData* dev, CommandRecorder* rec, VkImage image, const ImageInfo& img, TextureCapture tc,
                            uint32_t mips, VkImageLayout layout);
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
    struct ResolveImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    /**
     * What a capture holds on one device: the query pools its passes are timed and counted with,
     * the staging chunks and resolve images of its read-backs, and the frame it was at when it first
     * took part. Every device an application records on gets one, since none of these can be used
     * by another device's command buffers.
     */
    struct DeviceCapture {
        DeviceData* dev = nullptr;
        uint64_t startFrame = 0;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        uint32_t queryCount = 0;
        std::atomic<uint32_t> queriesUsed{0};
        VkQueryPool statsPool = VK_NULL_HANDLE;
        uint32_t statsCount = 0;
        std::atomic<uint32_t> statsUsed{0};
        VkQueryPool occlusionPool = VK_NULL_HANDLE;
        uint32_t occlusionCount = 0;
        std::atomic<uint32_t> occlusionUsed{0};
        std::vector<StagingChunk> staging;          // guarded by _mutex
        std::vector<ResolveImage> resolveImages;    // guarded by _mutex
        std::vector<VkImageView> resolveViews;      // guarded by _mutex (depth_resolve.h)
        // The copies queued inside passes suspended at the end of their command buffer, which
        // nothing may follow: recorded by the part that resumes them, once it has ended.
        std::mutex suspendedMutex;
        std::vector<PendingBufferCopy> suspendedCopies;
        std::vector<PendingImageCopy> suspendedImages;
    };
    void CreateQueryPools(DeviceCapture& dc);
    // Maps every device's staging chunks, once the GPU is done, for SendTextures and SendBuffers.
    void MapStaging();
    const StagingChunk* StagingOf(VkDevice device, uint32_t index);
    // The capture's record for a device, made (with its query pools) the first time the device
    // takes part; null when no capture is in progress.
    DeviceCapture* CaptureFor(DeviceData* dev);
    DeviceCapture* FindCapture(VkDevice device);
    // A submission's frame ordinal on its device.
    uint32_t FrameOf(DeviceData* dev);
    void SendPassTimings(DeviceCapture& dc, JsonWriter& w, uint32_t& sent, uint32_t& counted, size_t& total);
    void ReleaseDevice(DeviceCapture& dc);
    bool AllocateStaging(DeviceData* dev, VkDeviceSize size, uint32_t& chunkIndex, VkDeviceSize& offset,
                         VkBuffer* bufferOut = nullptr);
    // A render target's read-back: one texture per aspect (ReadBackAspects: colour, or depth and
    // then stencil, each its own copy and texture entry).
    void CaptureAttachment(DeviceData* dev, CommandRecorder* rec, uint32_t attachmentIndex, VkImageView view,
                           VkImageLayout layout, bool resolveTarget = false);
    static std::vector<VkImageAspectFlagBits> ReadBackAspects(VkImageView view);
    // The capture record and the copy for one aspect of one attachment (staging, resolve image);
    // false with the failed record already listed. The copy is recorded by the caller into its
    // command buffer.
    bool PrepareAttachment(DeviceData* dev, uint64_t commandBufferId, uint32_t passIndex, uint32_t layerCount,
                           uint32_t attachmentIndex, VkImageView view, VkImageLayout layout, bool resolveTarget,
                           VkImageAspectFlagBits aspect, TextureCapture& tc, PendingImageCopy& p);
    // The frame-start state of one aspect of one subresource (_imageStates; called with _mutex held).
    uint8_t& SubresourceState(VkImage image, const ImageInfo& img, VkImageAspectFlagBits aspect, uint32_t mip, uint32_t layer);
    // Attachments of the passes a submitted command buffer recorded before the capture began,
    // copied by a command buffer of the layer's submitted right after the application's.
    void ReadBackAfterSubmit(DeviceData* dev, VkQueue queue, CommandRecorder* rec, uint64_t commandBufferId, uint32_t frame);
    // Single-sampled images that multisampled captures are resolved into; freed with the staging.
    bool AllocateResolveImage(DeviceData* dev, const ImageInfo& img, uint32_t mip, uint32_t layers, VkImage* out);
    bool PrepareDepthResolve(DeviceData* dev, PendingImageCopy& p);

    mutable std::mutex _mutex;
    std::atomic<bool> _capturing{false};
    std::atomic<uint32_t> _storeAllPasses{0};
    std::atomic<uint32_t> _postSubmitReadbacks{0};
    std::atomic<uint32_t> _suspendedPasses{0};
    std::atomic<bool> _recordAlways{false};
    // Some device of the process has presented: frames are the presents', not another device's
    // substitute boundaries.
    std::atomic<bool> _presentSeen{false};
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
    // Frame-start contents: per image, per aspect and subresource (SubresourceState), whether the
    // capture has read it (and taken its contents) or written it whole first.
    enum SubresourceState : uint8_t { kUntouched = 0, kRead = 1, kWritten = 2 };
    std::unordered_map<uint64_t, std::vector<uint8_t>> _imageStates;

    // Every device taking part in the capture (pass profiling pools, staging), and the one the
    // capture started on, whose frames it counts.
    std::mutex _devicesMutex;
    std::unordered_map<VkDevice, std::unique_ptr<DeviceCapture>> _devices;
    VkDevice _homeDevice = VK_NULL_HANDLE;
    std::vector<PassTiming> _passTimings;
    uint64_t _commandTotal = 0;
};

} // namespace vkinsp
