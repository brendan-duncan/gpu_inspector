// Frame capture for Direct3D 11.
//
// Direct3D 11 has no command buffers on the immediate context and no passes, so the capture is the
// calls themselves, in the order each device context made them between two Presents, with what the
// inspector needs added:
//
//   Passes. A pass begins at the first draw into, or clear of, the bound render targets and ends
//   when OMSetRenderTargets binds different ones, at ClearState, at an executed command list, when
//   the event (BeginEvent) it began in ends, or at Present. The library records a BeginRenderPass
//   before the command that began it and an EndRenderPass before the one that ended it (neither is
//   a D3D11 call; `synthetic` says so), and copies the pass's render targets and depth target into
//   staging textures when it ends -- or at a DiscardView, which would throw them away first. A run
//   of dispatches with no render targets bound is a compute pass, which the inspector brackets
//   itself. Passes are numbered per context and per frame, which is how the inspector counts them.
//
//   State. A draw's state is the context's, not something bound on a command, so every draw and
//   dispatch carries a snapshot of what was bound (`state`: the shaders, each stage's constant
//   buffers, shader resources and samplers, the input assembler, the render targets, the
//   rasterizer, blend and depth-stencil state), followed from the application's Set* calls. The
//   buffers and textures it names are copied to staging resources there and then, once per
//   contents, and the snapshot refers to them by capture id.
//
//   Deferred contexts. A deferred context records into its own stream until FinishCommandList,
//   and the command list's commands become the `children` of the ExecuteCommandList that runs it
//   on the immediate context, where the inspector inlines them. Its copies were recorded into the
//   deferred context too, so they run with the list and hold what its draws read.
//
// The staging copies are mapped once the last frame is over, so the frame is not stalled mid-pass.
#pragma once

#include "state.h"

#include <gpu_inspector/sdk/json.h>

#include <string>
#include <vector>

namespace d3d11insp
{

struct CaptureOptions
{
    uint32_t frameCount = 1;
    /** The present count to start at; -1 for the next frame. */
    int64_t atFrame = -1;
    bool captureTextures = true;   // pass attachments
    bool captureBuffers = true;    // vertex, index, constant buffers
    bool captureImages = true;     // sampled textures
    bool profilePasses = true;     // timestamps around every pass
    size_t maxBufferSize = 64 * 1024;
    size_t maxBufferTotal = 64 * 1024 * 1024;
    size_t maxImageTotal = 256 * 1024 * 1024;
};

/** Arms a capture (the inspector's Capture request). */
void RequestCapture(const gpuinsp::sdk::JsonValue& msg);
/** Whether a capture is recording right now. */
bool Recording();

/** Records a call of the application's on `c`, with the state snapshot a draw was given. */
void Record(Context* c, const char* method, std::string args, std::string state = std::string());
/** Records a command that is no D3D11 call of the application's (a pass boundary). */
void RecordSynthetic(Context* c, const char* method, std::string args);

/** What a draw call draws, for the snapshot's read-backs. */
struct DrawParams
{
    bool indexed = false;
    UINT count = 0;          // vertices or indices
    UINT start = 0;          // the first vertex or index
    INT baseVertex = 0;
    UINT instances = 1;
    UINT startInstance = 0;
    bool indirect = false;
    bool drawAuto = false;
    ID3D11Buffer* argsBuffer = nullptr;
    UINT argsOffset = 0;
};

/** Before a draw: its pass begins if none is open, and its state is snapshotted; the snapshot is returned. */
std::string BeforeDraw(Context* c, const DrawParams& p);
/** After it: what it wrote (UAVs, stream-output targets) is new contents. */
void AfterDraw(Context* c);
std::string BeforeDispatch(Context* c, bool indirect, ID3D11Buffer* argsBuffer, UINT argsOffset);
void AfterDispatch(Context* c);

/** ClearRenderTargetView / ClearDepthStencilView / ClearView: a pass begins if the view is bound; what it cleared. */
void BeforeClear(Context* c, ID3D11View* view, bool depth, bool stencil);
/** DiscardView / DiscardResource: a pass target about to be thrown away is read back first. */
void BeforeDiscard(Context* c, ID3D11View* view, ID3D11Resource* resource);
/** OMSetRenderTargets was applied to the shadow state: a pass on other targets ends. */
void AfterSetRenderTargets(Context* c);
/** ClearState / an executed command list / FinishCommandList: the open pass ends. */
void EndOpenPass(Context* c);
/** BeginEvent / EndEvent (ID3DUserDefinedAnnotation): a pass that began inside the event ends with it. */
void AfterBeginEvent(Context* c);
void BeforeEndEvent(Context* c);

/** ExecuteCommandList on the immediate context: the list's recording becomes the command's children. */
void OnExecuteCommandList(Context* c, ID3D11CommandList* list, BOOL restoreState);
/** FinishCommandList on a deferred context: its recording so far is frozen into the list. */
void OnFinishCommandList(Context* c, ID3D11CommandList* list);

/** The contents of a resource changed (a map for writing, an update, a copy into it, a clear). */
void OnResourceWritten(ID3D11Resource* resource);

/** Present, before the real present: the frame's last pass ends and the present is recorded. */
void BeforePresent(IDXGISwapChain* swapChain, Context* c, const char* method, std::string args);
/** After it: frame statistics, and the capture starting or finishing on the frame boundary. */
void AfterPresent(Context* c);

/** An object is gone: whatever the capture kept under it is dropped. */
void OnObjectDestroyed(const void* ptr);

/** The inspector went away: a capture in progress is abandoned. */
void OnDisconnect();

}  // namespace d3d11insp
