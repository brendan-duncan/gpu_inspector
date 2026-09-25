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

namespace dxinsp
{

/**
 * The Capture message's `pixelHistory`: one pixel of one render target followed through the
 * captured frame (pixel_history.cpp). `texture` is an ID3D12Resource object id from an earlier
 * capture; a swap chain's back buffer (or one no longer alive) follows whichever back buffer the
 * captured frame renders into, the way a Metal capture follows the next drawable.
 */
struct PixelHistoryRequest
{
    bool enabled = false;
    uint64_t texture = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t mip = 0;
    uint32_t layer = 0;
};

/**
 * One draw's overlay, measured while the frame records (draw_overlay.cpp). The draw is named by
 * its pass and its ordinal within that pass rather than by a command index: the measurement happens
 * while the *next* frame records, whose commands are numbered again from the start.
 */
struct DrawOverlayRequest
{
    bool enabled = false;
    uint32_t passIndex = 0;
    uint32_t drawIndex = 0;
};

/** One draw's vertex shader outputs, streamed out while the frame records (mesh_output.cpp). */
struct MeshOutputRequest
{
    bool enabled = false;
    uint32_t passIndex = 0;
    uint32_t drawIndex = 0;
    /** Vertex records kept; a draw that writes more is reported as truncated. */
    uint32_t maxVertices = 200000;
};

struct CaptureOptions
{
    uint32_t frameCount = 1;
    /** Frame (the present counter) to start at; UINT64_MAX = the next frame. A frame already passed captures the next one. */
    uint64_t atFrame = UINT64_MAX;
    uint64_t maxBufferSize = 64 * 1024;       // per captured buffer range (longer ranges are truncated)
    uint64_t maxBufferTotal = 512ull << 20;   // stop capturing buffers past this many bytes per capture
    uint64_t maxTextureSize = 256ull << 20;   // skip render targets (and sampled textures) larger than this
    uint64_t maxImageTotal = 256ull << 20;    // stop capturing sampled textures past this many bytes
    /**
     * Stop reading render targets back past this many bytes in one capture. A frame of a few
     * passes never reaches it; a frame of thousands does, and without it the copies the capture
     * adds to the application's own command lists are more work than the frame itself — enough to
     * hang the GPU and take the application with it (a Unity URP frame here: 2,120 passes).
     */
    uint64_t maxTargetTotal = 512ull << 20;
    bool captureTextures = true;              // render targets
    bool captureBuffers = true;
    bool captureImages = true;                // textures bound through SRVs and UAVs
    bool profilePasses = true;                // timestamps, pipeline statistics and occlusion per pass
    /**
     * A timestamp pair, a pipeline statistics query and an occlusion query around every draw and
     * dispatch, not only around every pass (**Measure draws**). `vkinsp_replay --draws` measures
     * the same by replaying a Vulkan capture; here the queries go into the application's own list
     * as it records, so a list recorded before the capture began carries none.
     */
    bool drawTimings = false;
    bool stacktraces = false;                 // every recorded command carries the stack it was recorded from
    /** Draw every render pass again with a counting pixel shader, for its overdraw (overdraw.h). */
    bool overdraw = false;
    /** Follow one pixel of a render target through the frame (pixel_history.cpp). */
    PixelHistoryRequest pixelHistory;
    /** Measure where one draw of one pass landed (draw_overlay.cpp). */
    DrawOverlayRequest drawOverlay;
    /** Stream one draw's vertex shader outputs out (mesh_output.cpp). */
    MeshOutputRequest meshOutput;
};

class CaptureManager
{
public:
    static CaptureManager& Get();

    /** From the UI's Capture message: arms a capture for the next frame boundary (or `atFrame`). */
    void RequestCapture(const CaptureOptions& options);
    /** Recording frames right now (between the arming present and the finishing one). */
    bool IsCapturing() const { return _capturing.load(std::memory_order_acquire); }
    /** Counts the captures begun, so something done once per capture can tell a new one from the last. */
    uint64_t CaptureSerial() const { return _captureSerial.load(std::memory_order_acquire); }
    /** DXINSP_RECORD_ALWAYS or the UI's Settings: every list gets a recorder whether or not a capture is on. */
    bool RecordAlways() const;
    void SetRecordAlways(bool on);
    /** Whether lists being reset should get recorders: capturing, or record-always. */
    bool ShouldRecord() const { return _recordActive.load(std::memory_order_relaxed); }
    /** The present counter of the device that presented last (what `atFrame` counts). */
    uint64_t FrameCounter() const { return _frameCounter.load(std::memory_order_relaxed); }

    // --- Command lists -------------------------------------------------------------------------

    /** The recorder of a list being recorded, else nullptr. Cheap when nothing records: one atomic load. */
    CommandRecorder* RecorderFor(ID3D12GraphicsCommandList* list)
    {
        if (!ShouldRecord())
            return nullptr;
        if (CommandRecorder* rec = LookupRecorder(list))
            return rec;
        return Adopt(list);
    }
    /**
     * A list that is being recorded and has no recorder: it was reset before recording began. An
     * engine that keeps its lists in a pool resets one as soon as it has run (Unity does), frames
     * before it records into it again, so its Reset is long past when a capture is asked for, and
     * a frame of such lists was captured as nothing but "<unrecorded command list>". The recorder
     * is made here, at the first call seen, with a Reset that names no allocator.
     */
    CommandRecorder* Adopt(ID3D12GraphicsCommandList* list);
    /** The recorder a list already has, without making one: for Close, which is no reason to start recording a list. */
    CommandRecorder* RecorderIfAny(ID3D12GraphicsCommandList* list) { return ShouldRecord() ? LookupRecorder(list) : nullptr; }
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
     * The same for a list named rather than a recorder, and without asking whether anything is
     * recording now. A capture ends at a frame boundary, which on an engine that builds its lists
     * on worker threads (Unity does) is in the middle of several of them: the pass those lists
     * have open was begun while the capture ran and holds its queries, and a list closed with a
     * query still open fails with E_FAIL, which the application reads as a lost device.
     */
    void EndOpenPass(ID3D12GraphicsCommandList* list, bool synthetic);
    /**
     * Before EndRenderPass is forwarded: writes a split pass's end timestamp while the list is
     * still inside the pass region. Once the pass is suspended the runtime takes nothing, and
     * EndPass -- which runs after the forward -- is too late for it. A pass that is not split
     * writes its end timestamp there as before.
     */
    void EndSplitPassTimestamp(CommandRecorder* rec);
    /**
     * After OMSetRenderTargets / BeginRenderPass was forwarded and recorded: opens a pass on the
     * targets (already resolved through the descriptor tracker), reserving its queries and writing
     * the begin timestamp. Returns the pass index within the list.
     *
     * `split` says the pass is suspended across command lists (ActivePass::split): it is recorded
     * like any other, and nothing is added to it.
     */
    uint32_t BeginPass(CommandRecorder* rec, std::vector<BoundTarget> targets, bool renderPassApi, bool split = false);
    /** Before a dispatch is forwarded: opens a compute pass when no pass is open. */
    void OnBeforeDispatch(CommandRecorder* rec);
    /** Compute work that runs no shader of the application's (an acceleration structure build): timed as a dispatch is. */
    void OnBeforeComputeWork(CommandRecorder* rec);
    /** Before a command that closes a compute pass (barrier, event, bundle, render pass begin, Close): its end timestamp. */
    void OnComputePassEnd(CommandRecorder* rec);
    /** A draw or trace was recorded in the open pass (for its draw count). */
    void OnDraw(CommandRecorder* rec);
    /** A draw (graphics) or a dispatch: notes the heaps its root signature lets its shaders index directly (ListState::indexed). */
    void NoteIndexedHeaps(CommandRecorder* rec, bool compute);
    /**
     * Before a draw or dispatch is forwarded, when the capture measures draws (`drawTimings`):
     * reserves its queries and writes the begin timestamp. Returns the slot, or UINT32_MAX when
     * this one is not measured (not capturing, a bundle, a copy list, or the slots are used up).
     */
    uint32_t BeginDrawQueries(CommandRecorder* rec);
    /** After the draw was forwarded and recorded: its end timestamp, and the command it belongs to. */
    void EndDrawQueries(CommandRecorder* rec, uint32_t slot, bool dispatch);
    /**
     * The draw queries a list has taken, resolved into the readback buffer. Called where a resolve
     * is allowed: at the end of a pass and before Close, never inside a BeginRenderPass region.
     */
    void ResolveDrawQueries(CommandRecorder* rec);

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
     *
     * `whole`: not truncated. A vertex or index buffer view says exactly what a draw reads, and a
     * mesh cut at maxBufferSize draws as part of itself in a replay; the capture's total budget still
     * bounds it.
     */
    uint32_t QueueBufferCapture(CommandRecorder* rec, ID3D12Resource* buffer, UINT64 offset, UINT64 size, bool whole = false,
        bool afterSubmit = false);
    /** The same for a GPU virtual address range (resolved through the AddressMap); `size` 0 means to the buffer's end. */
    uint32_t QueueAddressCapture(CommandRecorder* rec, D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 size, bool whole = false);
    /**
     * The same, for a list that is already closed: the copy is made in the library's own list,
     * submitted right after the submission that executes `rec`'s (the path a suspended pass's copies
     * take). Counts as that list's for the frame it lands in. Call it before the submission is noted
     * (OnExecuteCommandLists does), or the entry never learns its frame.
     */
    uint32_t QueueAddressCaptureAfterSubmit(CommandRecorder* rec, D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 size);
    /**
     * Queues a texture bound through an SRV or UAV for read-back: every mip with all its slices,
     * once per resource per capture, under maxImageTotal. Returns the CaptureTextureInfo `capture`
     * id the descriptor carries in `data`, 0 when not read.
     */
    /**
     * `initialInto`: the texture as the frame found it (kind `initial`, once per capture), its copy
     * handed to the caller to run before a submission instead of recorded into the list
     * (BeforeExecuteCommandLists).
     */
    uint32_t QueueTextureCapture(CommandRecorder* rec, ID3D12Resource* texture, std::vector<std::function<void(ID3D12GraphicsCommandList*)>>* initialInto = nullptr);
    /**
     * Before a submission is forwarded, while the tracker still has each resource in the state the
     * submission finds it in: the textures its lists read before anything of the capture wrote
     * them, read back in a list of the capture's own run first on the queue. That is what a replay
     * starts them from -- a depth buffer a pass loads, a history texture the frame reads and then
     * overwrites -- where the read-backs taken at a pass's end or after the submission see what the
     * frame did to them.
     */
    void BeforeExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);

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
     * Heap slots as they are at a submission, as its extra (`heapDescriptors`): of the heaps its
     * lists let shaders index directly, the slots written since the capture last sent them (every
     * written slot the first time); and of every heap, the slots a table snapshot of its lists read
     * that were rewritten after it -- a volatile range may be, until the list runs. The GPU reads a
     * descriptor when it runs, which is what the heap holds at the submission; what the views name
     * is read back after it. Empty when there is nothing to send.
     */
    std::string IndexedHeapContents(UINT count, ID3D12CommandList* const* lists);
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
    std::atomic<uint64_t> _captureSerial{0};
    /** Recorders in the table, so a lookup made while nothing is recorded costs one atomic load. */
    std::atomic<size_t> _recorderCount{0};
    std::atomic<bool> _recordActive{false};
    std::atomic<uint64_t> _frameCounter{0};
};

}  // namespace dxinsp
