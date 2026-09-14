// Frame capture: the command stream of one or more frames, streamed to the UI, with the render
// targets, the bound buffers and textures and the GPU time of every pass.
//
// The model is the Vulkan layer's (docs/ARCHITECTURE.md, "Frame capture"): the UI asks for a
// capture, the library arms at the next present, records every command list recorded during the
// frames that follow, freezes each list's commands when it is executed, and sends everything once
// the GPU has finished the last frame. Nothing is replayed.
//
// State machine, driven by Present (the frame boundary):
//   Idle --Capture request--> Armed --present--> Capturing --present x N--> Finish --> Idle
//
// The command list hooks (hooks_command_list.cpp) call into this from a handful of places: at
// Reset and Close, when render targets are bound, before a dispatch, before the commands that
// close a compute pass, when a root table or root view is bound, when vertex/index/indirect
// buffers are bound, and when a bundle is executed. The queue and swap chain hooks call it at
// ExecuteCommandLists and Present.
#pragma once

#include "command_recorder.h"
#include "descriptors.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dxinsp {

struct CaptureOptions {
    uint32_t frameCount = 1;
    /** Frame (the present counter) to start at; UINT64_MAX = the next frame. A frame already passed captures the next one. */
    uint64_t atFrame = UINT64_MAX;
    uint64_t maxBufferSize = 64 * 1024;       // per captured buffer range (longer ranges are truncated)
    uint64_t maxBufferTotal = 512ull << 20;   // stop capturing buffers past this many bytes per capture
    uint64_t maxTextureSize = 256ull << 20;   // skip render targets (and sampled textures) larger than this
    uint64_t maxImageTotal = 256ull << 20;    // stop capturing sampled textures past this many bytes
    bool captureTextures = true;              // render targets
    bool captureBuffers = true;
    bool captureImages = true;                // textures bound through SRVs and UAVs
    bool profilePasses = true;                // timestamps, pipeline statistics and occlusion per pass
    bool stacktraces = false;                 // every recorded command carries the stack it was recorded from
};

class CaptureManager {
public:
    static CaptureManager& Get();

    /** From the UI's Capture message: arms a capture for the next frame boundary (or `atFrame`). */
    void RequestCapture(const CaptureOptions& options);
    /** Recording frames right now (between the arming present and the finishing one). */
    bool IsCapturing() const { return _capturing.load(std::memory_order_acquire); }
    /** DXINSP_RECORD_ALWAYS or the UI's Settings: every list gets a recorder whether or not a capture is on. */
    bool RecordAlways() const;
    void SetRecordAlways(bool on);
    /** Whether lists being reset should get recorders: capturing, or record-always. */
    bool ShouldRecord() const { return _recordActive.load(std::memory_order_relaxed); }
    /** The present counter of the device that presented last (what `atFrame` counts). */
    uint64_t FrameCounter() const { return _frameCounter.load(std::memory_order_relaxed); }

    // --- Command lists -------------------------------------------------------------------------

    /** The recorder of a list being recorded, else nullptr. Cheap when nothing records: one atomic load. */
    CommandRecorder* RecorderFor(ID3D12GraphicsCommandList* list) {
        if (!ShouldRecord()) return nullptr;
        return LookupRecorder(list);
    }
    /** Reset (or CreateCommandList without an initial close): attaches or resets the recorder when recording. */
    void OnListReset(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_COMMAND_LIST_TYPE type, bool bundle,
                     ID3D12PipelineState* initialState);
    /** Before Close is forwarded: closes the open passes (their read-back and timestamps go into the list) and freezes. */
    void OnBeforeClose(ID3D12GraphicsCommandList* list);
    void OnListReleased(ID3D12GraphicsCommandList* list);

    // --- Passes ---------------------------------------------------------------------------------

    /**
     * Before OMSetRenderTargets / BeginRenderPass is forwarded: ends the pass the list has open,
     * recording the synthetic EndRenderTargets command and appending the read-back copies. Also
     * called before Close.
     */
    void EndPass(CommandRecorder* rec, bool synthetic);
    /**
     * After OMSetRenderTargets / BeginRenderPass was forwarded and recorded: opens a pass on the
     * targets (already resolved through the descriptor tracker), reserving its queries and writing
     * the begin timestamp. Returns the pass index within the list.
     */
    uint32_t BeginPass(CommandRecorder* rec, std::vector<BoundTarget> targets, bool renderPassApi);
    /** Before a dispatch is forwarded: opens a compute pass when no pass is open. */
    void OnBeforeDispatch(CommandRecorder* rec);
    /** Before a command that closes a compute pass (barrier, event, bundle, render pass begin, Close): its end timestamp. */
    void OnComputePassEnd(CommandRecorder* rec);
    /** A draw or trace was recorded in the open pass (for its draw count). */
    void OnDraw(CommandRecorder* rec);

    // --- Bindings ------------------------------------------------------------------------------

    /**
     * After SetGraphicsRootDescriptorTable / SetComputeRootDescriptorTable was recorded: attaches the
     * `descriptors` snapshot of the table (README.md) and queues its buffers and textures for read-back.
     */
    void SnapshotRootTable(CommandRecorder* rec, bool compute, uint32_t parameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE base);
    /** After SetGraphicsRoot{ConstantBuffer,ShaderResource,UnorderedAccess}View was recorded: the same for a root view. */
    void SnapshotRootView(CommandRecorder* rec, bool compute, uint32_t parameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address);
    /**
     * Queues a buffer range for read-back with the capture (truncated to maxBufferSize) and returns
     * the id the command carries in `bufferData` / a descriptor's `data`; 0 when it cannot be read.
     * The copy is recorded into the list at once, with the barriers the buffer's state needs, or
     * at the end of a BeginRenderPass region.
     */
    uint32_t QueueBufferCapture(CommandRecorder* rec, ID3D12Resource* buffer, UINT64 offset, UINT64 size);
    /** The same for a GPU virtual address range (resolved through the AddressMap); `size` 0 means to the buffer's end. */
    uint32_t QueueAddressCapture(CommandRecorder* rec, D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 size);
    /**
     * Queues a texture bound through an SRV or UAV for read-back: every mip with all its slices,
     * once per resource per capture, under maxImageTotal. Returns the CaptureTextureInfo `capture`
     * id the descriptor carries in `data`, 0 when not read.
     */
    uint32_t QueueTextureCapture(CommandRecorder* rec, ID3D12Resource* texture);

    // --- Queues and frames ---------------------------------------------------------------------

    /**
     * After ExecuteCommandLists was forwarded: freezes the lists' recordings in submission order,
     * notes the queue, and drives the frame boundary for a device that never presents (Dawn's
     * D3D12 device in Chrome renders into shared textures the compositor presents, so its own
     * IDXGISwapChain::Present is never called). Returns true when this submission was such a frame
     * boundary, so the caller runs the per-frame chores a present would (validation, shader edits,
     * frame timing). See "Frame boundary" in the README.
     */
    bool OnExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, double cpuMs);
    /** A bundle ran inside a list: its recording becomes the ExecuteBundle command's children. */
    void OnExecuteBundle(CommandRecorder* rec, ID3D12GraphicsCommandList* bundle);
    /**
     * The frame boundary, from Present: counts the frame, arms a pending capture, and finishes a
     * capture whose last frame just ended (waiting for the GPU, then streaming everything).
     */
    void OnPresent(ID3D12Device* device, IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);
    /** The queue a swap chain presents on (from its creation), or null. */
    ID3D12CommandQueue* PresentQueue(IDXGISwapChain* swapChain);
    void OnSwapChainCreated(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);
    void OnSwapChainReleased(IDXGISwapChain* swapChain);
    /** The device is going away: its capture-side objects are released. */
    void OnDeviceReleased(ID3D12Device* device);

private:
    CaptureManager() = default;
    CommandRecorder* LookupRecorder(ID3D12GraphicsCommandList* list);
    /**
     * A frame boundary on `device`: a present (its swap chain given) or, for a device that never
     * presents, an ExecuteCommandLists. Advances that device's frame count and runs the arm /
     * capture / finish state machine, whose frames are those of the device (and, on the present
     * path, the swap chain) the capture started on.
     */
    void EndFrame(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* swapChain, bool present);
    struct Impl;
    Impl* _impl = nullptr;
    Impl& impl();
    std::atomic<bool> _capturing{false};
    std::atomic<bool> _recordActive{false};
    std::atomic<uint64_t> _frameCounter{0};
};

}  // namespace dxinsp
