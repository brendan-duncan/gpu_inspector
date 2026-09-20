// What overdraw.cpp and pixel_history.cpp share: the calls of a render pass kept while capturing,
// the pass itself as the measurements need it, and the objects both make on the application's
// device (render textures, CPU descriptors, copies of the application's pipelines). Internal to the
// library; the hooks and the capture use overdraw.h.
#pragma once

#include "overdraw.h"
#include "shader_edit.h"

#include <memory>
#include <string>
#include <vector>

namespace dxinsp {

/** A pixel followed through a pass (pixel_history.cpp). */
struct HistoryPass;

/** A kept call: what it does, what state it sets, and the command the capture recorded it as. */
struct LoggedOp {
    PassOp op;
    OpKey key;
    uint32_t command = 0;
};

/** The calls of one command list kept while a measured capture records. */
struct ListOps {
    std::vector<LoggedOp> ops;
    /** Where the open pass's own calls begin; everything before it is the state it started from. */
    size_t passFirst = 0;
    /** The pass executed a bundle whose calls were not kept, so it cannot be issued again. */
    bool unknownBundle = false;
};

/** One attachment of a measured pass. */
struct PassAttachment {
    ID3D12Resource* resource = nullptr;   // the application's, not AddRef'd
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;   // the view's format, typed
    uint32_t mip = 0;
    uint32_t slice = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    bool readOnlyDepth = false;
    /** How the pass starts it: PRESERVE means a measurement's copy starts from the attachment. */
    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE beginAccess = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE stencilBeginAccess = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
    D3D12_CLEAR_VALUE clearValue{};
};

/** A render pass the capture measures: where it is, what it starts from, and what it needs. */
struct MeasuredPass {
    ID3D12Device* device = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t listId = 0;
    uint32_t passIndex = 0;
    uint32_t beginCommand = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t samples = 1;
    bool layered = false;
    /** The capture measures this pass's overdraw. */
    bool measureOverdraw = false;
    /** The capture measures a draw overlay, which is this pass's when the request names its index. */
    bool measureOverlay = false;
    /** Why the pass cannot be measured at all. */
    std::string note;
    std::vector<PassAttachment> colors;
    bool hasDepth = false;
    PassAttachment depth;
    /** Overdraw: a copy of the depth-stencil attachment as it was before the pass began. */
    ComPtr<ID3D12Resource> depthStart;
    /** Pixel history: the pass renders to the followed pixel. */
    std::shared_ptr<HistoryPass> history;
};

/** The application's objects a measurement holds until its command list has run. */
using KeepList = std::vector<ComPtr<IUnknown>>;

template <typename T>
inline void KeepObject(KeepList& keep, T* object) {
    if (!object) return;
    object->AddRef();
    keep.emplace_back(ComPtr<IUnknown>(static_cast<IUnknown*>(object)));
}

// ---------------------------------------------------------------------------------------------
// Shared helpers (overdraw.cpp)

/** A half float as the count targets hold it. */
float HalfToFloat(uint16_t h);

/** A resource barrier on one subresource, for the copies a measurement reads back. */
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t subresource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to);

/** A texture of the measurement's own, in `state`; null when it could not be created. */
ComPtr<ID3D12Resource> NewMeasurementTexture(ID3D12Device* device, DXGI_FORMAT format, uint32_t width, uint32_t height,
                                             uint32_t layers, bool depthStencil, D3D12_RESOURCE_STATES state,
                                             const D3D12_CLEAR_VALUE* clear);

/** A readback buffer of the measurement's own. */
ComPtr<ID3D12Resource> NewMeasurementReadback(ID3D12Device* device, uint64_t size);

/**
 * A CPU descriptor slot of the measurement's own heaps (RTV or DSV), never reused within a
 * capture, since the descriptor is read when the list is recorded and the list runs later.
 */
bool MeasurementDescriptor(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_CPU_DESCRIPTOR_HANDLE& out);

/** A record of the measurement's objects on a device, so they can be released with the capture. */
void KeepMeasurementObject(ID3D12Device* device, IUnknown* object);

/** The pixel shaders the measurements draw with, compiled once per container kind; null with `error`. */
const D3D12_SHADER_BYTECODE* CountingPixelShader(bool dxil, std::string& error);
const D3D12_SHADER_BYTECODE* CoverPixelShader(bool dxil, std::string& error);

/** The copies of the application's pipelines the measurements draw with, as they are cached. */
enum class VariantKind : uint64_t {
    Count = 0,     // overdraw: the counting pixel shader into one R16_FLOAT target, blended ONE + ONE
    Cover = 1,     // pixel history: a pixel shader that writes nothing, no culling, no tests
    Facing = 2,    // ... with the pipeline's culling
    Shaded = 3,    // ... with the pipeline's own pixel shader, so a discard shows
    Depth = 4,     // ... with the depth test alone
    Stencil = 5,   // ... with the stencil test alone
    Both = 6,      // ... with both
};

/** The cache key: what was changed, and the depth-stencil format the copy is built for. */
inline uint64_t VariantKey(VariantKind kind, DXGI_FORMAT depthFormat) {
    return (uint64_t)kind | ((uint64_t)(uint32_t)depthFormat << 8);
}

/**
 * A copy of the application's pipeline for a measurement, cached on it and released with it. The
 * pointer is borrowed: the caller keeps its own reference for as long as its list may run.
 */
ID3D12PipelineState* VariantOf(ID3D12PipelineState* pipeline, uint64_t key, const PipelineVariant& variant, std::string& error);

/**
 * The calls still in effect at the end of `ops`, in the order they were made: each call recorded,
 * less those a later call undid (OpKey). Issuing them leaves a list in the state the application's
 * calls left it in, without every call before.
 */
std::vector<const LoggedOp*> EffectiveOps(const std::vector<LoggedOp>& ops, size_t count);

/** Issues the ops on the list exactly as the application made them, for putting its state back. */
void ReissueState(ID3D12GraphicsCommandList* list, const std::vector<const LoggedOp*>& ops);

/** The recorded method of a command of the list, for a pixel history event ("" when unknown). */
std::string RecordedMethod(CommandRecorder* rec, uint32_t command);

// ---------------------------------------------------------------------------------------------
// Pixel history (pixel_history.cpp)

/** A capture starts: the pixel it follows. */
void StartPixelHistory(const PixelHistoryRequest& request);
/** Whether the pass renders to the followed pixel: the colour attachment index, or -1. */
int MatchPixelHistoryAttachment(const MeasuredPass& pass);
/** Before the pass begins: copies of its attachments with the followed pixel in them. */
void PreparePixelHistory(MeasuredPass& pass, int attachment);
/** After the application's pass has ended: the pass issued again one draw at a time at the pixel. */
void FollowPixel(MeasuredPass& pass, CommandRecorder* rec, const ListOps& ops);
/** A list ran in a frame: the events measured in it belong to that frame. */
void AssignPixelHistoryFrame(ID3D12GraphicsCommandList* list, uint32_t frame);

// ---------------------------------------------------------------------------------------------
// Draw overlays (draw_overlay.cpp)

/** A capture starts: the draw whose overlay it measures. */
void StartDrawOverlay(const DrawOverlayRequest& request);
/** Whether an overlay was asked for at all, which is what makes every pass keep its depth copy. */
bool DrawOverlayRequested();
/** Whether the request names this pass. */
bool MatchDrawOverlayPass(const MeasuredPass& pass);
/** After the application's pass has ended: the draw issued again on its own, three ways. */
void MeasureDrawOverlay(MeasuredPass& pass, CommandRecorder* rec, const ListOps& ops);
/** A list ran in a frame: the overlay measured in it belongs to that frame. */
void AssignDrawOverlayFrame(ID3D12GraphicsCommandList* list, uint32_t frame);

// ---------------------------------------------------------------------------------------------
// Mesh output (mesh_output.cpp)

/** A capture starts: the draw whose vertex shader outputs it streams out. */
void StartMeshOutput(const MeshOutputRequest& request);
/** Whether mesh output was asked for at all, which is what makes every pass keep its calls. */
bool MeshOutputRequested();
/** Whether the request names this pass. */
bool MatchMeshOutputPass(const MeasuredPass& pass);
/** After the application's pass has ended: the draw issued again with stream output bound. */
void MeasureMeshOutput(MeasuredPass& pass, CommandRecorder* rec, const ListOps& ops);
/** A list ran in a frame: the mesh measured in it belongs to that frame. */
void AssignMeshOutputFrame(ID3D12GraphicsCommandList* list, uint32_t frame);

}  // namespace dxinsp
