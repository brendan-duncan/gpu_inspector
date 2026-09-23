// Measurements taken while capturing, by issuing a render pass again into the application's own
// command list right after the application ends it, with the application's own pipelines,
// descriptor heaps and buffers as it bound them. `vkinsp_replay` rebuilds a Vulkan frame from a
// capture file to do the same (docs/REPLAY.md); nothing is rebuilt here, because the library is in
// the process. The Metal library measures the same way (src/metal/src/overdraw.h).
//
// While such a capture records, every command-list call that shapes what a pass rasterizes is also
// kept as a closure holding its arguments (hooks_command_list.cpp calls LogPassOp), with a key
// saying which earlier call it undoes. Two measurements use them:
//
// Overdraw (overdraw.cpp): every pipeline is replaced by a copy whose pixel shader returns 1.0 into
// an R16_FLOAT target blended ONE + ONE, so each pixel ends up holding how many fragments landed on
// it. Twice per pass:
//   * with the pass's depth-stencil state, against a copy of the depth-stencil attachment taken
//     before the pass began: the fragments that passed, in draw order;
//   * with no depth or stencil attachment and the tests off: every fragment the draws rasterized.
//
// Pixel history (pixel_history.cpp): one pixel of one render target, followed through every pass
// that renders to it, one draw at a time; see the file.
//
// A D3D12 command list carries its state across passes, so a measurement that binds its own
// pipeline and render target has to put the application's back: the calls in effect at the end of
// the pass are issued again once the measurement is drawn (OpKey, below).
#pragma once

#include "capture.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dxinsp
{

class CommandRecorder;
struct BoundTarget;

/**
 * A measurement's command list, as the recorded calls see it. The calls a measurement changes go
 * through here; the rest are issued exactly as the application made them.
 */
class PassReplay
{
public:
    /** The application bound a pipeline: a measurement binds its own copy of it instead. */
    virtual void SetPipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) = 0;
    /**
     * The application bound a root signature. A measurement that streams a vertex shader's outputs
     * out binds its own copy of it instead, since the copy carries the stream-output flag
     * (mesh_output.cpp); every other measurement takes the application's.
     */
    virtual void SetGraphicsRootSignature(ID3D12GraphicsCommandList* list, ID3D12RootSignature* signature)
    {
        list->SetGraphicsRootSignature(signature);
    }
    /** The application set scissor rectangles; the pixel history keeps its one-pixel scissor instead. */
    virtual void SetScissors(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RECT* rects)
    {
        list->RSSetScissorRects(count, rects);
    }
    /** A clear of a bound render target; a measurement clears its own copy of it, or nothing. */
    virtual void ClearTarget(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*) {}
    virtual void ClearDepthStencil(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
        const D3D12_RECT*) {}
    /** A draw, which the measurement issues through `draw` as many times as it needs, or not at all. */
    virtual void IssueDraw(ID3D12GraphicsCommandList* list, const std::function<void(ID3D12GraphicsCommandList*)>& draw) = 0;
    /** A draw that cannot be measured: an ExecuteIndirect, or a bundle whose calls were not recorded. */
    virtual void Skip() = 0;

protected:
    ~PassReplay() = default;
};

/** A command-list call recorded while capturing, issued again against a measurement's list. */
using PassOp = std::function<void(ID3D12GraphicsCommandList* list, PassReplay& replay)>;

/**
 * What a recorded call sets, so the state of the list at a point can be rebuilt from the calls
 * still in effect there rather than from every call before it:
 *   Draw     a draw: it sets no state;
 *   Action   an ordered call that sets no state either (a clear);
 *   Replace  sets what `name` names outright, undoing any earlier call with that name — and every
 *            call of the family `clears` (a root signature invalidates every root argument);
 *   Slot     binds slot `location` of the family `name`, undoing earlier binds of that slot;
 *   Range    binds slots location..location+length-1 of `name`, undoing earlier binds of each.
 * The names are string literals, compared by content.
 */
enum class OpPolicy : uint8_t
{
    Draw,
    Action,
    Replace,
    Slot,
    Range
};

struct OpKey
{
    OpPolicy policy = OpPolicy::Replace;
    const char* name = "";
    const char* clears = nullptr;
    uint32_t location = 0;
    uint32_t length = 1;

    static OpKey DrawCall() { return {OpPolicy::Draw, "", nullptr, 0, 0}; }
    static OpKey Ordered(const char* name) { return {OpPolicy::Action, name, nullptr, 0, 0}; }
    static OpKey Replace(const char* name, const char* clears = nullptr) { return {OpPolicy::Replace, name, clears, 0, 1}; }
    static OpKey Slot(const char* family, uint32_t index) { return {OpPolicy::Slot, family, nullptr, index, 1}; }
    static OpKey Range(const char* family, uint32_t first, uint32_t count) { return {OpPolicy::Range, family, nullptr, first, count}; }
};

/** The op families the keys above use, so a typo cannot silently stop an op from being undone. */
namespace ops
{
constexpr const char* kPipeline = "pipeline";
constexpr const char* kGraphicsRootSignature = "graphicsRootSignature";
constexpr const char* kGraphicsRoot = "graphicsRoot";        // Slot family: one per root parameter index
constexpr const char* kDescriptorHeaps = "descriptorHeaps";
constexpr const char* kTopology = "topology";
constexpr const char* kIndexBuffer = "indexBuffer";
constexpr const char* kVertexBuffers = "vertexBuffers";      // Range family: the vertex buffer slots
constexpr const char* kViewports = "viewports";
constexpr const char* kScissors = "scissors";
constexpr const char* kStencilRef = "stencilRef";
constexpr const char* kBlendFactor = "blendFactor";
constexpr const char* kDepthBounds = "depthBounds";
constexpr const char* kClear = "clear";
}  // namespace ops

/** Whether a capture that keeps passes for a measurement is recording. Checked before a hook builds its closure. */
bool PassRecordingActive();

/** Keeps a call made on a recorded command list, with the command slot the capture recorded it at. */
void LogPassOp(CommandRecorder* rec, OpKey key, PassOp op);

/** The list was reset: what was kept of it starts over. */
void OnMeasuredListReset(ID3D12GraphicsCommandList* list);
/** ClearState: everything the list had bound is gone, so nothing kept of it is still in effect. */
void OnMeasuredListClearState(ID3D12GraphicsCommandList* list);
/** A bundle ran in the list: its kept calls are inlined, so a pass that executes it can be issued again. */
void OnMeasuredBundle(ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList* bundle);
void OnMeasuredListReleased(ID3D12GraphicsCommandList* list);

/**
 * The copies each measurement starts from, taken while the list is outside a render-pass region
 * (the OMSetRenderTargets / BeginRenderPass hook) and before the application's first draw of the
 * pass: the depth-stencil attachment for the overdraw count, the followed pixel of every
 * attachment for the pixel history.
 */
void PrepareMeasuredPass(CommandRecorder* rec, const std::vector<BoundTarget>& targets);
/** The capture opened the pass: the measurement takes its index and the command it began at. */
void BeginMeasuredPass(CommandRecorder* rec);
/**
 * The application's pass has ended and its render targets have been read back: the measurements are
 * drawn into the same list. `insideRenderPass` says the list is still inside a BeginRenderPass
 * region (the application closed it without EndRenderPass), where a measurement cannot bind
 * anything of its own; such a pass is reported as unmeasurable.
 */
void EndMeasuredPass(CommandRecorder* rec, bool insideRenderPass);

/** A capture starts recording: what it measures, with nothing left from the last one. */
void StartMeasurements(bool overdraw, const PixelHistoryRequest& history, const DrawOverlayRequest& overlay,
    const MeshOutputRequest& mesh, uint64_t maxDataSize);
/** A list ran in a frame: the measurements drawn into it belong to that frame. */
void AssignMeasurementFrame(ID3D12GraphicsCommandList* list, uint32_t frame);
/** The capture's command lists have completed: CaptureOverdraw plus a CaptureOverdrawData frame per measurement. */
void SendOverdraw();
/** The same for the followed pixel: CapturePixelHistory, in the JSON vkinsp_replay --pixel-data writes. */
void SendPixelHistory();
/** The same for the measured draw: CaptureDrawOverlay and its mask (draw_overlay.cpp). */
void SendDrawOverlay();
/** The same for the streamed-out draw: CaptureMeshOutput and its vertex records (mesh_output.cpp). */
void SendMeshOutput();
/** The device is going away: the measurement's own objects on it are released. */
void OnMeasurementDeviceReleased(ID3D12Device* device);

}  // namespace dxinsp
