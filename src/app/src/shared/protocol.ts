// Messages exchanged between the capture layer and the UI, and between the Electron main and
// renderer processes. The layer-side vocabulary follows WebGPU Inspector's actions.js.

export interface BlobInfo {
  name: string;
  size: number;
}

// Serialized Vulkan argument values: primitives, enum name strings, nested structs, arrays.
// Objects with special keys carry references and summaries:
//   {__id, __class}          tracked object reference (HandleRef)
//   {__handle, __class}      untracked raw handle (RawHandleRef)
//   {__bytes, base64?}       raw byte blob (inline when small)
//   {__count, __truncated}   oversized scalar array summary
export type ArgValue = null | boolean | number | string | ArgValue[] | ArgObject;
export interface ArgObject {
  [key: string]: ArgValue;
}

/** A tracked object reference inside serialized Vulkan arguments. */
export interface HandleRef extends ArgObject {
  __id: number;
  __class: string;
}

/** An untracked raw handle (e.g. an output parameter serialized before registration). */
export interface RawHandleRef extends ArgObject {
  __handle: string;
  __class: string;
}

// ------------------------------------------------------------------------------------------
// Layer -> UI

export interface SnapshotMessage { action: "Snapshot"; count: number }

export interface AddObjectMessage {
  action: "AddObject";
  id: number;
  parent: number;
  type: string;      // "VkImage"
  cmd: string;       // "vkCreateImage"
  index: number;
  handle: string;    // "0x..."
  label: string | null;
  args: ArgObject | null;
  blobs?: BlobInfo[];
}

export interface DeleteObjectsMessage { action: "DeleteObjects"; ids: number[] }
export interface ObjectSetLabelMessage { action: "ObjectSetLabel"; id: number; label: string }
export interface ObjectBlobsMessage { action: "ObjectBlobs"; id: number; blobs: BlobInfo[] }
export interface ObjectUpdateMessage { action: "ObjectUpdate"; id: number; [key: string]: ArgValue | string | number }
/** Frame timing over the last reporting interval (about 100 ms): average, extremes, frame count. */
export interface FrameStatsMessage {
  action: "FrameStats";
  frame: number;
  frameTimeMs: number;
  minMs?: number;
  maxMs?: number;
  frames?: number;
  /** CPU time per frame spent inside vkQueueSubmit. */
  submitMs?: number;
  /** Display refresh interval while vsync is on (FIFO present modes); 0 without vsync. */
  refreshMs?: number;
  /** Where refreshMs came from: "present_timing", "display_timing", "monitor" or "estimate" (frame intervals). */
  refreshSource?: string;
  /** The display's refresh period from a real source (0 when only the estimate is available), vsync or not. */
  displayRefreshMs?: number;
  /** Absent until the application presented once (an OpenXR application never does). */
  presentMode?: string;
  /** What ends a frame: "present", or for applications without a swapchain "wait" (their vkWaitForFences) or "submit"; "" while undecided. */
  frameBoundary?: string;
  /** Metal: the device's currentAllocatedSize, what the driver has set aside for the process. */
  allocatedBytes?: number;
  /** Metal: the device's recommendedMaxWorkingSetSize, what it can keep resident without paging. */
  workingSetBytes?: number;
  /** Refreshes in the interval that repeated the previous frame (dropped frames), and the
   *  layer's running total since the refresh estimate was made. */
  dropped?: number;
  droppedTotal?: number;
  /**
   * The dropped count was measured by the display rather than worked out from the refresh period
   * and the frame interval. D3D12 reads the swap chain's own refresh counters; the Vulkan layer has
   * no equivalent and estimates, which is worth saying next to the number.
   */
  droppedMeasured?: boolean;
}
export interface PongMessage { action: "Pong" }

/** One symbolized frame of a stack trace. */
export interface StackFrame {
  address: string;      // "0x..."
  module?: string;      // module file name
  function?: string;
  file?: string;
  line?: number;
  offset: number;       // from the module base (what a symbolizer on the unstripped module needs)
  /** Inside the Vulkan loader or a layer (hidden by default). */
  internal?: boolean;
  /** The UI already tried (or managed) to resolve it on the host from the unstripped module. */
  hostResolved?: boolean;
  /** The callers this frame's function was inlined into, innermost first (host symbolization with debug info). */
  inlinedInto?: { function?: string; file?: string; line?: number }[];
}

/** Answer to RequestStacktraces: the creation stacks of objects. */
export interface StacktracesMessage {
  action: "Stacktraces";
  /** Whether the layer collects creation stacks (the launch option). */
  available: boolean;
  stacks: { id: number; frames: StackFrame[] }[];
}

/** Answer to RequestSymbols: a frame per requested address, in request order. */
export interface SymbolsMessage {
  action: "Symbols";
  frames: StackFrame[];
}

export interface ObjectBlobMessage {
  action: "ObjectBlob";
  id: number;
  index: number;
  size: number;
  __binary?: Uint8Array;
}

/** Which graphics API produced a capture: it decides how the UI classifies the command names. */
export type CaptureApi = "vulkan" | "metal" | "d3d12";

export interface CaptureFrameResultsMessage {
  action: "CaptureFrameResults";
  frame: number;
  frames: number;
  count: number;
  batches: number;
  /** Absent from Vulkan captures, which predate the field. */
  api?: CaptureApi;
}

/** One descriptor of a binding in a bound descriptor set (null when never written). */
export interface CaptureDescriptor {
  buffer?: HandleRef | null;
  offset?: number;
  range?: number;
  /** Dynamic uniform/storage buffers: the offset passed to vkCmdBindDescriptorSets. */
  dynamicOffset?: number;
  /** Id of the CaptureBuffers entry holding the bound range's contents. */
  data?: number;
  imageView?: HandleRef | null;
  imageLayout?: string;
  sampler?: HandleRef | null;
  immutable?: boolean;
  bufferView?: HandleRef | null;
  /** A ray tracing acceleration structure binding: the structure the rays are traced against. */
  accelerationStructure?: HandleRef | null;
  /**
   * D3D12: a texture SRV/UAV names its resource and carries the view description (there is no view
   * object); `data` is the read-back's capture id as for `imageView`. A buffer view uses `buffer` /
   * `offset` / `range` above (the view description in `view`); a sampler descriptor has no object
   * and carries its description in `samplerDesc` with `sampler` null.
   */
  resource?: HandleRef | null;
  view?: ArgObject | null;
  samplerDesc?: ArgObject | null;
}

export interface CaptureDescriptorBinding {
  binding: number;
  /** "VK_DESCRIPTOR_TYPE_..."; D3D12: "D3D12_DESCRIPTOR_RANGE_TYPE_..." for a table's range, "D3D12_ROOT_PARAMETER_TYPE_..." for a root view. */
  type: string;
  stages?: string;
  descriptors: (CaptureDescriptor | null)[];
  /** D3D12: the shader register the binding's first descriptor is at and its register space (the reflection is keyed by them). */
  register?: number;
  space?: number;
}

/** Snapshot of a descriptor set's contents taken when it was bound. */
export interface CaptureDescriptorSet {
  /** The set index; D3D12: the root parameter index. */
  set: number;
  descriptorSet: HandleRef | null;   // null for push descriptors; D3D12: the descriptor heap, or null for a root view
  layout?: HandleRef | null;         // D3D12: the root signature
  bindings: CaptureDescriptorBinding[];
}

export interface CaptureDescriptorSets {
  bindPoint: string;    // "VK_PIPELINE_BIND_POINT_..."; D3D12: "graphics" | "compute"
  sets: CaptureDescriptorSet[];
}

export interface CaptureChildCommand {
  method: string;
  args: ArgObject | null;
  children?: CaptureChildBuffer[];
  descriptors?: CaptureDescriptorSets;
  bufferData?: number[];
  textureData?: number[];
  imageData?: number[];
  /** Position in the secondary command buffer's recording. */
  slot?: number;
  stack?: string[];
}

export interface CaptureChildBuffer {
  commandBuffer: number;
  commands: CaptureChildCommand[];
}

export interface CaptureCommand {
  index: number;
  frame: number;                  // frame ordinal within the capture (0-based)
  method: string;                 // "vkCmdDraw", "vkQueueSubmit", ...
  object: HandleRef | null;       // command buffer or queue
  /** Metal: the encoder the command was issued on, with an id that is never announced as an object. */
  encoder?: HandleRef;
  args: ArgObject | null;
  result?: number;
  children?: CaptureChildBuffer[]; // secondary command buffers of vkCmdExecuteCommands
  /**
   * vkCmdBuildAccelerationStructures*: the contents the layer read back for the addresses the
   * build named, as `{info, geometry, field, capture}` per resolved address
   * (src/vulkan/src/hooks.cpp). On the command rather than on the structure because a structure's
   * update is last-write-wins and an application that rebuilds every frame would overwrite it.
   */
  buildData?: ArgObject[];
  /**
   * vkCmdTraceRays*: the shader binding table read back at the trace, as `{region, capture}` per
   * region the layer could resolve (src/vulkan/src/hooks.cpp). Matching each record against the
   * pipeline's group handles is what says which shader a record runs.
   */
  bindingTableData?: ArgObject[];
  /** Set on commands the UI inlined from a secondary command buffer: that buffer's object id. */
  secondary?: number;
  /** vkCmdBindDescriptorSets / vkCmdPushDescriptorSet: what the bound sets contained. */
  descriptors?: CaptureDescriptorSets;
  /**
   * vkCmdBindVertexBuffers / vkCmdBindIndexBuffer / indirect draws: CaptureBuffers ids per bound buffer (0 = none).
   * vkCmdCopyBuffer / vkCmdCopyBufferToImage: the source range of each region, read whole, for a replay to write.
   */
  bufferData?: number[];
  /**
   * Vulkan passes that load attachments, and copies, blits and transfers from images: the CaptureTextureFrames `capture`
   * ids of what the images read held before the command (kind "initial"), for the parts nothing in the capture wrote first.
   */
  imageData?: number[];
  /**
   * Metal texture binds (`setFragmentTexture:atIndex:` and the rest): the CaptureTextureFrames
   * `capture` id of each bound texture's read-back contents, 0 where it was not read (the same
   * shape as `bufferData`). Vulkan reaches a sampled image through its descriptor instead.
   */
  textureData?: number[];
  /**
   * vkCmdBindPipeline / vkCmdBindShadersEXT recorded while a live shader edit was active: `args` name the
   * replacement that ran (its own object, with the edited code), and this the application's original
   * (per bound shader for shader objects, null where the application's own was bound).
   */
  replaced?: HandleRef | (HandleRef | null)[];
  /** Position in its command buffer's recording (what a ValidationMessage's `command` refers to). */
  slot?: number;
  /** Return addresses ("0x...", innermost first) of the call that recorded it (the "Stack traces" capture option). */
  stack?: string[];
}

export interface CaptureFrameCommandsMessage {
  action: "CaptureFrameCommands";
  frame: number;
  index: number;
  commands: CaptureCommand[];
}

export interface CaptureTextureInfo {
  id: number;             // VkImage object id
  frame: number;          // frame ordinal within the capture
  commandBuffer: number;  // command buffer object id
  passIndex: number;      // pass counter within that command buffer
  attachment: number;
  format: string;         // "VK_FORMAT_..."
  aspect: "color" | "depth" | "stencil";   // a depth-stencil attachment is two entries, one per aspect
  width: number;
  height: number;
  depth: number;
  layers: number;
  mip: number;
  /** Sampled images: mip levels in the data (mip .. mip + mips - 1, each with all layers, back to back); absent = 1. */
  mips?: number;
  size: number;
  error?: string;
  /** Multisampled images (> 1): read back through a resolve. */
  samples?: number;
  /** Dynamic rendering: this is the resolve target of attachment `attachment`. */
  resolve?: boolean;
  /**
   * "sampled": an image bound by a descriptor set (read back once per view). "initial": what an image held when the
   * frame first read it (a pass loading it, a copy from it), before anything in the capture wrote it: one mip, for
   * a replay to start from. Absent = a render pass attachment.
   */
  kind?: "attachment" | "sampled" | "initial";
  /** Sampled images and initial contents: the id descriptors and the reading command's `imageData` reference, the view, and the first layer. */
  capture?: number;
  view?: number;
  baseLayer?: number;
}

export interface CaptureTextureFramesMessage { action: "CaptureTextureFrames"; count: number; textures: CaptureTextureInfo[] }

/** What the texture decoder needs to know about a block of pixel data. */
export interface ImageDataInfo {
  format: string;
  aspect: "color" | "depth" | "stencil";
  width: number;
  height: number;
}

/** Reply to RequestImage: one mip level / array layer of a live image (all depth slices for 3D). */
export interface ImageDataMessage extends ImageDataInfo {
  action: "ImageData";
  id: number;
  mip: number;
  layer: number;
  depth: number;
  layers: number;
  size: number;
  error?: string;
  __binary?: Uint8Array;
}

export interface CaptureTextureDataMessage {
  action: "CaptureTextureData";
  id: number;
  frame: number;
  commandBuffer: number;
  passIndex: number;
  attachment: number;
  /** Which of a depth-stencil attachment's two entries (Vulkan; libraries that read depth only send none). */
  aspect?: "color" | "depth" | "stencil";
  /** Sampled images: the capture id (see CaptureTextureInfo.capture). */
  capture?: number;
  size: number;
  __binary?: Uint8Array;
}

/** A buffer range read back when it was bound during the capture. */
export interface CaptureBufferInfo {
  id: number;             // referenced by CaptureDescriptor.data and CaptureCommand.bufferData
  buffer: number;         // VkBuffer object id
  frame: number;
  commandBuffer: number;
  offset: number;
  size: number;           // bytes captured (0 on error)
  originalSize?: number;  // bytes bound, when the capture was truncated
  error?: string;
}

export interface CaptureBuffersMessage { action: "CaptureBuffers"; count: number; buffers: CaptureBufferInfo[] }

export interface CaptureBufferDataMessage {
  action: "CaptureBufferData";
  id: number;
  size: number;
  __binary?: Uint8Array;
}

/** GPU time of one render pass, from timestamp queries the layer wrote around it. */
export interface PassTiming {
  frame: number;
  commandBuffer: number;
  passIndex: number;
  /** "compute": a run of dispatches outside a render pass, with its own index sequence. Absent = render. */
  kind?: "render" | "compute";
  /** Start relative to the earliest timed pass of the capture. */
  startMs: number;
  durationMs: number;
  /** Metal render passes sampled at every stage boundary: the vertex and fragment stages' own spans (they overlap on a tile-based GPU). */
  vertexMs?: number;
  fragmentMs?: number;
  /** Metal: the statistic counter set's deltas over the pass (vertexInvocations, fragmentInvocations, ...). */
  counters?: Record<string, number>;
  /** Metal: the stage-utilization counter set's cycle deltas over the pass (totalCycles, vertexCycles, fragmentCycles, ...). */
  utilization?: Record<string, number>;
}

export interface CapturePassTimingsMessage {
  action: "CapturePassTimings";
  timestampPeriodNs: number;
  count: number;
  passes: PassTiming[];
  /**
   * The device tick `PassTiming.startMs` is measured from. With the CpuTimeline's calibration this
   * places a pass on the host clock — `gpuTicksToCpuMs(timeline, originTicks) + startMs` — which is
   * what lets the GPU lane be drawn beside the CPU lanes. Absent on a device whose clock was not
   * calibrated, and on the devices of a multi-device capture that are not the captured one.
   *
   * A string when the tick is past 2^53, which it is on a real device: the layer's writer quotes an
   * integer that a JSON number could not hold exactly (src/vulkan/src/json_writer.h). Pass it
   * through Number() before arithmetic — never `+`, which would concatenate.
   */
  originTicks?: number | string;
}

/**
 * The overdraw of one render pass: how many fragments landed on each pixel when the pass was drawn
 * again with a counting fragment shader (a Metal or D3D12 capture with `overdraw`,
 * src/metal/src/overdraw.h and src/d3d12/src/overdraw.h; vkinsp_replay --overdraw measures the same
 * for a Vulkan capture). Two per pass: with the pass's depth and stencil tests, and without.
 */
export interface OverdrawMeasurement {
  frame: number;
  commandBuffer: number;
  passIndex: number;
  /** true: the fragments that passed the pass's depth and stencil tests, in draw order, from what the pass started with; false: every rasterized fragment. */
  depthTested: boolean;
  /** false: the pass could not be measured, and `note` says why. Absent = measured. */
  measured?: boolean;
  width: number;
  height: number;
  fragments: number;
  coveredPixels: number;
  maxCount: number;
  draws: number;
  /** Draws that were not counted: a pipeline with no counting copy, an indirect command buffer's. */
  skippedDraws: number;
  /** Pixels by count: 1, 2, 3, 4, 5-8, 9-16, 17-32, 33 and more. */
  histogram: number[];
  /** Bytes of per-pixel counts in the CaptureOverdrawData that follows (u16 little endian, row by row); 0 without. */
  size: number;
  /** Vulkan (vkinsp_replay): the fragment shader invocations the capture's pipeline statistics measured for the pass. */
  capturedFragments?: number;
  note?: string;
}

export interface CaptureOverdrawMessage { action: "CaptureOverdraw"; count: number; passes: OverdrawMeasurement[] }

/**
 * One draw or dispatch of a D3D12 capture taken with `drawTimings`, measured by queries of its own
 * (src/d3d12/src/capture.cpp, SendDrawStats). The fields are `vkinsp_replay --draws`' (draw_stats.ts),
 * so both APIs' measurements read the same way: `passIndex` is 0xffffffff for a dispatch that is in
 * no render pass, and a draw's time is its share of a pass rather than what it costs alone.
 */
export interface CaptureDrawStat {
  command: number;
  frame: number;
  commandBuffer: number;
  passIndex: number;
  timed: boolean;
  ms: number;
  counted: boolean;
  vertexInvocations: number;
  primitives: number;
  fragmentInvocations: number;
  computeInvocations: number;
  sampled: boolean;
  samplesPassed: number;
}

/**
 * One draw's overlay, measured while a D3D12 capture recorded (src/d3d12/src/draw_overlay.cpp).
 * The mask follows in CaptureDrawOverlayData: one byte per pixel, row by row, of the same
 * OVERLAY_COVERED / OVERLAY_PASSED / OVERLAY_WIREFRAME bits `vkinsp_replay --overlay` writes.
 */
export interface CaptureDrawOverlayMessage {
  action: "CaptureDrawOverlay";
  command: number;
  method: string;
  frame: number;
  commandBuffer: number;
  passIndex: number;
  drawIndex: number;
  measured: boolean;
  width: number;
  height: number;
  fragments: number;
  pixelsCovered: number;
  pixelsPassed: number;
  pixelsRejected: number;
  /** Pixels the stencil test alone rejected, and pixels a face the draw's culling removed covered. */
  pixelsStencilRejected: number;
  pixelsBackFacing: number;
  depthTested: boolean;
  wireframe: boolean;
  /** The stencil test alone was drawn, and the run with nothing culled was. */
  stencilTested: boolean;
  backFaceTested: boolean;
  size: number;
  note?: string;
}

/**
 * What one draw's vertex shader wrote, streamed out while a D3D12 capture recorded
 * (src/d3d12/src/mesh_output.cpp). The records follow in CaptureMeshOutputData: `vertices` of
 * `stride` bytes, the layout `outputs` describes, as `vkinsp_replay --mesh` writes for Vulkan.
 */
export interface CaptureMeshOutputMessage {
  action: "CaptureMeshOutput";
  /** The command's slot in its list's recording, with the list (as CaptureDrawOverlay's). */
  command: number;
  commandBuffer: number;
  method: string;
  frame: number;
  passIndex: number;
  drawIndex: number;
  measured: boolean;
  /** The pipeline's primitive kind, as a D3D_PRIMITIVE_TOPOLOGY_* name (primitiveKind reads it). */
  topology: string;
  stride: number;
  vertices: number;
  truncated: boolean;
  outputs: { name: string; offset: number; components: number; base: "float" | "int" | "uint"; builtin?: string }[];
  size: number;
  note?: string;
}

export interface CaptureMeshOutputDataMessage {
  action: "CaptureMeshOutputData";
  command: number;
  commandBuffer: number;
  size: number;
  __binary?: Uint8Array;
}

export interface CaptureDrawOverlayDataMessage {
  action: "CaptureDrawOverlayData";
  /** The command's slot in its list's recording, with the list: the key CaptureDrawOverlay used. */
  command: number;
  commandBuffer: number;
  size: number;
  __binary?: Uint8Array;
}

export interface CaptureDrawStatsMessage {
  action: "CaptureDrawStats";
  count: number;
  draws: CaptureDrawStat[];
  /** What the measurement could not reach: the draws past the slot limit, or counters a render pass region ruled out. */
  note?: string;
}

/**
 * Metal and D3D12: the pixel a capture with `pixelHistory` followed through its frame
 * (src/metal/src/pixel_history.mm, src/d3d12/src/pixel_history.cpp), in the JSON
 * vkinsp_replay --pixel-data writes (renderer/pixel_history.ts parses it).
 */
export interface CapturePixelHistoryMessage { action: "CapturePixelHistory"; history: Record<string, unknown> }

export interface CaptureOverdrawDataMessage {
  action: "CaptureOverdrawData";
  frame: number;
  commandBuffer: number;
  passIndex: number;
  depthTested: boolean;
  size: number;
  __binary?: Uint8Array;
}

/**
 * The last message of a capture, after its commands, render targets, buffers and timings, whichever
 * of those it had: a client waiting for the capture (the MCP server) knows nothing more is coming.
 * Capture libraries built before it existed do not send it.
 */
export interface CaptureCompleteMessage { action: "CaptureComplete"; frame: number; frames: number }

/**
 * Whether the application is being held at its frame boundary (the capture libraries' frame_pause.h).
 * Sent whenever the state changes, including when the library changed it itself -- a capture
 * requested while paused resumes, since a paused application renders no frames to capture.
 */
export interface PauseStateMessage { action: "PauseState"; paused: boolean }

/** Answer to ReplaceShader / RestoreShader: whether the pipeline was rebuilt with the edit. */
export interface ShaderReplacedMessage {
  action: "ShaderReplaced";
  pipeline: number;
  stage: string;        // "vertex", "fragment", ... (the layer's stage names)
  ok: boolean;
  error?: string;
  note?: string;        // parts of the pipeline's create info the layer could not keep
  replacement?: number; // object id of the replacement pipeline
}

/** An object a validation message names: the tracked reference (or raw handle) and its debug name. */
export interface ValidationObjectRef {
  object: HandleRef | RawHandleRef | null;
  class: string;
  handle: string;
  name?: string;
}

export type ValidationSeverity = "error" | "warning" | "info" | "verbose";

/**
 * A message from the layer's VK_EXT_debug_utils messenger (the validation layer, or the driver).
 * The same message repeated is sent once; ValidationCount carries the repeat counts.
 */
export interface ValidationMessage {
  action: "ValidationMessage";
  key: number;
  severity: ValidationSeverity;
  types: string[];          // "validation" | "performance" | "general"
  idName: string | null;    // "VUID-..."
  idNumber: number;
  message: string;
  frame: number;
  count: number;
  objects: ValidationObjectRef[];
  queueLabels?: string[];
  cmdBufLabels?: string[];
  /** The command being recorded when the message fired (while the layer was recording it). */
  command?: { commandBuffer: number; slot: number };
}

export interface ValidationCountMessage {
  action: "ValidationCount";
  counts: [number, number][];   // [key, count]
  /** Unique messages the layer stopped keeping once its cap was reached. */
  dropped?: number;
}

/** One object still alive when its owner was destroyed. */
export interface LeakedObject {
  id: number;
  class: string;
  name: string | null;
  cmd: string;
}

/**
 * Sent just before a device or instance is destroyed with objects still alive under it (the
 * DeleteObjects for them follows). Objects the application cannot destroy and those freed with
 * their pool are not counted.
 */
export interface LeakReportMessage {
  action: "LeakReport";
  owner: number;
  ownerClass: string;
  count: number;
  byType: Record<string, number>;
  /** The first 2000 leaked objects, by id. */
  objects: LeakedObject[];
}

/**
 * The GPU stopped responding (VK_ERROR_DEVICE_LOST) and the layer read its breadcrumbs to say which
 * command it was running (src/vulkan/src/device_lost.h). Sent once per device.
 */
export interface DeviceLostMessage {
  action: "DeviceLost";
  /** The entry point that reported the loss. */
  call: string;
  /** Whether breadcrumbs were on; without them only `message` is meaningful. */
  breadcrumbs: boolean;
  /** Ordinals of the last action the GPU began and the last it finished. */
  lastBegun?: number;
  lastCompleted?: number;
  /** The command it was running when it stopped, empty when it had finished everything it began. */
  hungCommand?: string;
  lastCompletedCommand?: string;
  /** The diagnosis in words, ready to show. */
  message: string;
}

/** One command list the D3D12 runtime was tracking when the device went (DRED auto-breadcrumbs). */
export interface RemovedCommandList {
  /** The debug name the application gave it, or "(unnamed)". */
  commandList: string;
  commandQueue: string;
  /** How far into the list the GPU had got, and how long the list was. */
  operationsCompleted: number;
  operationsTotal: number;
  complete: boolean;
  /** The operation it had reached, when it had not finished: "DrawIndexedInstanced", "Dispatch". */
  stoppedAt?: string;
}

/** An object the runtime had allocated near a page-faulting address. */
export interface RemovedAllocation {
  name: string;
  /** "resource", "heap", "command list": what kind of object it was. */
  type: string;
}

/**
 * The D3D12 counterpart of DeviceLostMessage: the device was removed and the library read Device
 * Removed Extended Data for what the GPU was running (src/d3d12/src/device_removed.h). Sent once per
 * device.
 *
 * The runtime fills DRED in only once the device has really been removed, so `breadcrumbs` is false
 * — and `commandLists` empty — whenever it was asked before that, including under
 * DXINSP_SIMULATE_DEVICE_REMOVED. Then `message` is the whole answer.
 */
export interface DeviceRemovedMessage {
  action: "DeviceRemoved";
  /** The entry point that reported the removal, "IDXGISwapChain::Present". */
  call: string;
  /** Whether the runtime handed over breadcrumbs; without them only `message` is meaningful. */
  breadcrumbs: boolean;
  /** What the device gave as the reason, in words; absent when it reported none. */
  reason?: string;
  commandLists?: RemovedCommandList[];
  /** The address the GPU faulted on, when the removal was a page fault. */
  pageFaultAddress?: string;
  existingAllocations?: RemovedAllocation[];
  /** Freed near the faulting address: the classic use-after-free. */
  recentFreedAllocations?: RemovedAllocation[];
  /** The diagnosis in words, ready to show. */
  message: string;
}

/** One heap in a memory sample. `usage` and `budget` are absent where the driver reports neither. */
export interface MemorySampleHeap {
  /** Bytes this application holds from the heap. */
  allocated: number;
  /**
   * How many allocations that is. Absent on Metal, which reports the total directly
   * (`currentAllocatedSize`) and has nothing to count.
   */
  allocations?: number;
  /** The driver's view, counting every process: what is resident, and what this one may have. */
  usage?: number;
  budget?: number;
}

/**
 * One sample of memory use, sent with each frame report (src/vulkan/src/cpu_timeline.h,
 * src/d3d12/src/cpu_timeline.h). The object graph says what is held *now*; a series of these says
 * which way it is going, which is the difference between a leak, a pool refilling and a steady
 * renderer — indistinguishable at any one instant.
 */
export interface MemorySampleMessage {
  action: "MemorySample";
  /** The frame the sample was taken at, so memory can be plotted against frames rather than time. */
  frame: number;
  /** One per heap, in the order the device reports them (D3D12: local, then system). */
  heaps: MemorySampleHeap[];
}

/**
 * Per-frame timings from a running timing capture (src/vulkan/src/cpu_timeline.h). Sent in batches
 * on the frame report's interval: a frame report averages five or six frames together, and a hitch
 * is one frame, so this carries each of them.
 */
export interface TimingFramesMessage {
  action: "TimingFrames";
  /** Category names, in the order each frame's `categoryMs` is indexed by. */
  categories: string[];
  frames: { frame: number; durationMs: number; categoryMs: number[] }[];
}

/**
 * Allocations and frees from a running memory capture (src/vulkan/src/cpu_timeline.h,
 * src/d3d12/src/cpu_timeline.h), sent in batches on the frame report's interval. MemorySample is
 * the sum; these are what it is the sum of, which is what says whether a climbing total is one
 * allocation never freed or a thousand that mostly are.
 */
export interface MemoryEventsMessage {
  action: "MemoryEvents";
  /** Only in a capture's first message: what each heap held when it began, in MemorySample's order. */
  baseline?: { allocated: number; allocations: number }[];
  /** Events past the capture library's limit, counted rather than recorded. */
  dropped?: number;
  events: {
    frame: number;
    /** Milliseconds since the capture began. */
    ms: number;
    /** The allocation's object id (a VkDeviceMemory, an ID3D12Heap or a committed ID3D12Resource); 0 when unknown. */
    id: number;
    bytes: number;
    /** Index into the heaps, as MemorySample orders them. */
    heap: number;
    free?: boolean;
  }[];
}

/**
 * The application asked for a capture through include/gpu_inspector.h (`gpu_inspector_capture`).
 * The capture library passes the request on rather than acting on it: the capture bar's options are
 * the inspector's, and a tab has to be waiting for the capture's messages.
 */
export interface AppCaptureRequestMessage { action: "AppCaptureRequest"; frameCount: number }

/**
 * Call stacks sampled during a timing capture (src/vulkan/src/cpu_sampler.h), in batches on the
 * frame report's interval. A stack is sent once, under an id that means it for the whole capture.
 */
export interface TimingSamplesMessage {
  action: "TimingSamples";
  /** Milliseconds between samples, so a count of them is a time. */
  periodMs: number;
  /** Every thread sampled so far; a sample names one by its index here. */
  threads: { id: number; name?: string }[];
  /** The stacks this batch is the first to use: return addresses as "0x...", innermost first. */
  stacks: { id: number; addresses: string[] }[];
  /** [frame, thread index, stack id, 1 when the thread was running and 0 when it was blocked, samples]. */
  samples: [number, number, number, number, number][];
  /** Samples not recorded because the stack table was full. */
  dropped?: number;
}

/** One host-side call timed during a capture (src/vulkan/src/cpu_timeline.h). */
export interface CpuEvent {
  /** Index into CpuTimeline.threads. */
  thread: number;
  /** "submit", "present", "waitFences", "acquire", "waitIdle". */
  category: string;
  frame: number;
  /** Relative to the capture's origin, the same axis the calibration maps GPU times onto. */
  startMs: number;
  durationMs: number;
}

/** How to place a GPU timestamp on the CPU axis: hostMs + (ticks - deviceTicks) * period / 1e6. */
export interface CpuGpuCalibration {
  /** Like CapturePassTimings.originTicks, a string when past 2^53: Number() it before arithmetic. */
  deviceTicks: number | string;
  hostMs: number;
  timestampPeriod: number;
}

/**
 * Where a frame's CPU time went, beside where its GPU time went: the calls the layer timed on the
 * host during the capture, and (where the device has calibrated timestamps) the relation that puts
 * both on one axis.
 */
export interface CpuTimelineMessage {
  action: "CaptureCpuTimeline";
  /** The threads that made the calls, by their OS id; events index into this. */
  threads: number[];
  /** Events beyond the capture's cap, which are not recorded. */
  dropped?: number;
  calibration?: CpuGpuCalibration;
  events: CpuEvent[];
}

export type LayerMessage =
  | AppCaptureRequestMessage
  | CpuTimelineMessage
  | TimingFramesMessage
  | DeviceLostMessage
  | DeviceRemovedMessage
  | MemorySampleMessage
  | MemoryEventsMessage
  | TimingSamplesMessage
  | SnapshotMessage
  | ValidationMessage
  | ValidationCountMessage
  | LeakReportMessage
  | AddObjectMessage
  | DeleteObjectsMessage
  | ObjectSetLabelMessage
  | ObjectBlobsMessage
  | ObjectUpdateMessage
  | FrameStatsMessage
  | PongMessage
  | StacktracesMessage
  | SymbolsMessage
  | ObjectBlobMessage
  | CaptureFrameResultsMessage
  | CaptureFrameCommandsMessage
  | CaptureTextureFramesMessage
  | CaptureTextureDataMessage
  | CaptureBuffersMessage
  | CaptureBufferDataMessage
  | CapturePassTimingsMessage
  | CaptureOverdrawMessage
  | CaptureDrawStatsMessage
  | CaptureDrawOverlayMessage
  | CaptureDrawOverlayDataMessage
  | CaptureMeshOutputMessage
  | CaptureMeshOutputDataMessage
  | CaptureOverdrawDataMessage
  | CapturePixelHistoryMessage
  | ShaderReplacedMessage
  | PauseStateMessage
  | ImageDataMessage
  | GpuTraceMessage
  | CaptureCompleteMessage;

/** Answer to SaveGpuTrace (Metal): the .gputrace document was written, or why not. */
export interface GpuTraceMessage {
  action: "GpuTrace";
  ok: boolean;
  path: string;
  frame: number;
  error?: string;
}

// ------------------------------------------------------------------------------------------
// UI -> Layer

export interface PingRequest { action: "Ping" }
/**
 * Asks a capture library what application it is serving, for the attach list. It is answered
 * with a Target message and the connection is then closed, without the probe ever becoming the
 * library's client (src/vulkan/src/target_probe.h).
 */
export interface ProbeRequest { action: "Probe" }
/** Asks the layer to resend the live object snapshot (a window picking up a running session). */
export interface RequestSnapshotRequest { action: "RequestSnapshot" }
export interface RequestBlobRequest { action: "RequestBlob"; id: number; index: number }
/** Asks the layer to read back one subresource of a live VkImage (answered by ImageData). */
export interface RequestImageRequest { action: "RequestImage"; id: number; mip: number; layer: number }
/** Asks for the current contents of a VkDescriptorSet (answered by an ObjectUpdate carrying `bindings`). */
export interface RequestDescriptorSetRequest { action: "RequestDescriptorSet"; id: number }
export interface SettingsRequest { action: "Settings"; recordAlways?: boolean }
/** Asks for the symbolized creation stacks of objects (answered by Stacktraces). */
export interface RequestStacktracesRequest { action: "RequestStacktraces"; ids: number[] }
/** Asks the layer to symbolize addresses a capture's commands carry (answered by Symbols). */
export interface RequestSymbolsRequest { action: "RequestSymbols"; addresses: string[] }
/** Starts or stops a timing capture in the layer. */
export interface TimingCaptureRequest {
  action: "TimingCapture";
  start: boolean;
  /** Also sample every thread's call stack this many times a second (Windows capture libraries; ignored elsewhere). */
  sampleHz?: number;
}
/** Starts or stops a memory capture in the capture library (Vulkan and D3D12). */
export interface MemoryCaptureRequest { action: "MemoryCapture"; start: boolean }
/** Switches the in-app HUD on or off: the frame time drawn over the application's own window. */
export interface HudRequest { action: "Hud"; enabled: boolean }
/**
 * Live pause. `paused` holds the application at its frame boundary or lets it go; `step` instead
 * lets that many frames through and stays paused, which is how a single frame is stepped. The
 * library answers with PauseState either way.
 */
export interface PauseRequest { action: "Pause"; paused?: boolean; step?: number }

export interface CaptureRequest {
  action: "Capture";
  frameCount: number;
  /** Frame (the layer's present counter) to start at; omitted = the next frame. A frame already passed captures the next one. */
  atFrame?: number;
  /** Bytes captured per bound buffer range (longer ranges are truncated). */
  maxBufferSize?: number;
  /** Total buffer bytes captured per capture; further buffers are reported as errors. */
  maxBufferTotal?: number;
  maxTextureSize?: number;
  captureTextures?: boolean;
  /** Read back the buffers bound by descriptor sets, vertex/index bindings and indirect draws. */
  captureBuffers?: boolean;
  /** Read back the images bound by descriptor sets (once per image view). */
  captureImages?: boolean;
  /** Total image bytes captured per capture; further images are reported as errors. */
  maxImageTotal?: number;
  /** Write GPU timestamps around every render pass (CapturePassTimings). */
  profilePasses?: boolean;
  /** Every recorded command carries the stack it was recorded from. */
  stacktraces?: boolean;
  /**
   * Metal and D3D12: draw every render pass a second time with a counting fragment shader, for its
   * overdraw (CaptureOverdraw). The Vulkan layer ignores it; vkinsp_replay --overdraw measures a
   * Vulkan capture file.
   */
  overdraw?: boolean;
  /**
   * D3D12: a timestamp pair, a pipeline statistics query and an occlusion query around every draw
   * and dispatch, not only around every pass (CaptureDrawStats, **Measure draws**). The queries go
   * into the application's own command lists as they record, so a list recorded before the capture
   * began carries none. The Vulkan layer ignores it; `vkinsp_replay --draws` measures a Vulkan
   * capture file after the fact, and Metal does not measure draws at all.
   */
  drawTimings?: boolean;
  /**
   * D3D12: measure where one draw landed, for the render target tab's draw overlays
   * (CaptureDrawOverlay). The draw is named by its pass and its ordinal within that pass, not by a
   * command index: the measurement happens while this capture records, and its commands are
   * numbered from the start. The Vulkan layer ignores it; `vkinsp_replay --overlay` measures a
   * Vulkan capture file after the fact.
   */
  drawOverlay?: { passIndex: number; drawIndex: number };
  /**
   * D3D12: stream one draw's vertex shader outputs out, for the mesh view's VS Out
   * (CaptureMeshOutput). Named the same way a `drawOverlay` request is. The Vulkan layer ignores
   * it; `vkinsp_replay --mesh` measures a Vulkan capture file after the fact.
   */
  meshOutput?: { passIndex: number; drawIndex: number; maxVertices?: number };
  /**
   * Metal and D3D12: follow one pixel of a texture through the captured frame (CapturePixelHistory):
   * every pass that renders to it drawn again one draw at a time at that pixel. `texture` is an
   * object id from an earlier capture; a Metal drawable's, or a D3D12 swap chain's back buffer (or
   * one no longer alive), follows whichever one the frame renders into. The Vulkan layer ignores it;
   * vkinsp_replay --pixel follows a Vulkan capture file.
   */
  pixelHistory?: { texture: number; x: number; y: number; mip?: number; layer?: number };
}

/** Live shader editing: rebuild a pipeline with one stage replaced by the given SPIR-V (base64); D3D12: DXBC/DXIL bytecode in the same field. */
export interface ReplaceShaderRequest { action: "ReplaceShader"; pipeline: number; stage: string; spirv: string }
/** Drops the edit of one stage (or of every stage when `stage` is omitted). */
export interface RestoreShaderRequest { action: "RestoreShader"; pipeline: number; stage?: string }

/** Metal: asks the library to write the next frame as an Xcode .gputrace document (answered by GpuTrace). */
export interface SaveGpuTraceRequest { action: "SaveGpuTrace"; path?: string }

export type UiRequest = PingRequest | ProbeRequest | RequestSnapshotRequest | RequestBlobRequest | RequestImageRequest | RequestDescriptorSetRequest
  | SettingsRequest | CaptureRequest | ReplaceShaderRequest | RestoreShaderRequest
  | RequestStacktracesRequest | RequestSymbolsRequest | SaveGpuTraceRequest | TimingCaptureRequest | MemoryCaptureRequest
  | HudRequest | PauseRequest;

// ------------------------------------------------------------------------------------------
// Electron main <-> renderer

/** A capture taken automatically once the launched application connects. */
export interface QueuedCapture {
  /** "frame": capture frame `value` (0 = the first frame); "time": capture `value` seconds after connecting. */
  mode: "none" | "frame" | "time";
  value: number;
}

export interface LaunchConfig {
  /** "native": an executable on this machine. "android": a package on a device reached through adb.
   *  "implicit": nothing is started; the session waits for an application that the registered implicit
   *  layer connects (started with VKINSP_ENABLE=1 and VKINSP_PORT).
   *  "waitD3D12" (Windows): nothing is started either; dxinsp_launch.exe --watch waits for a process
   *  with `exe`'s image name to appear and injects the D3D12 capture library into it as it starts,
   *  which is what D3D12 has in place of an implicit layer (src/d3d12/README.md, docs/D3D12.md).
   *  "browser" (Windows): a page in a Chromium browser, which is a native launch whose arguments
   *  and `follow` the main process composes (main/browsers.ts): `exe` is the browser and `args` is
   *  the URL, and the capture library goes into the GPU process, where the page's WebGPU work is. */
  target: "native" | "android" | "implicit" | "waitD3D12" | "browser";
  /** Executable path, the package name for an Android target, the image name to wait for ("waitD3D12"), or the browser ("browser"). */
  exe: string;
  /** The command line, or the URL to open for a "browser" target. */
  args: string;
  cwd: string;
  /** Extra environment variables, one KEY=VALUE per line. */
  env: string;
  /** Android: the device serial (`adb -s`). */
  device: string;
  /** Android: the activity to start; empty for the package's launcher activity. */
  activity: string;
  port: number;
  log: boolean;
  recordAlways: boolean;
  /** Vulkan: GPU breadcrumbs, so a lost device names the command it was running (device_lost.h). */
  breadcrumbs?: boolean;
  /** Vulkan: the driver's compiler statistics per pipeline (shader_statistics.h). */
  shaderStatistics?: boolean;
  /** Also enable VK_LAYER_KHRONOS_validation (native targets), whose messages the Inspect tab lists. */
  validation: boolean;
  /** Directories holding the application's debug files (";"-separated): the unstripped libraries, for stack
   *  trace source lines, and the PDBs of D3D12 shaders built with `dxc -Zs`, for their HLSL. */
  symbolDirs?: string;
  /** Directories holding the shader sources (";"-separated), for modules with line information but no embedded text. */
  sourceRoots?: string;
  /** With `validation`: the validation layer's synchronization validation (hazards between commands and submissions). */
  syncValidation?: boolean;
  /** With `validation`: GPU-assisted validation, which checks descriptor indices and addresses the CPU cannot see. */
  gpuValidation?: boolean;
  /** Capture a stack trace at every object creation (VKINSP_STACKTRACES). */
  stacktraces: boolean;
  /**
   * Windows: also put the D3D12 capture library into the child processes the target starts whose
   * command line holds this text, for an application that renders in a process of its own making
   * (a browser's GPU process, "--type=gpu-process"; src/d3d12/launcher/main.cpp, follow mode).
   */
  follow?: string;
  capture: QueuedCapture;
}

/** "file": a capture loaded from disk; the session has no application behind it. */
export type ConnectionState = "disconnected" | "connecting" | "connected" | "launched" | "exited" | "error" | "file";

export interface StatusMessage {
  state: ConnectionState;
  detail: string;
}

/**
 * One inspected application. A session is created by launching an executable or by connecting
 * to a running one; it owns the target process (if launched) and the TCP connection to its
 * layer. Sessions live in the main process and are displayed by exactly one window at a time.
 */
export interface SessionInfo {
  id: number;
  /** Display name: executable base name plus arguments, or "port N" for connect-only sessions. */
  name: string;
  /** Launch configuration; null when the session was created by connecting to a running app. */
  config: LaunchConfig | null;
  /** Port actually used (may differ from the configured one when it was already taken). */
  port: number;
  pid: number | null;
  state: ConnectionState;
  detail: string;
  recordAlways: boolean;
  /** Recent log lines, so a window that picks the session up can show its history. */
  log: string[];
}

/** An Android device known to adb (main -> renderer, for the launch dialog). */
export interface AndroidDevice {
  serial: string;
  /** adb's state: "device" when usable; "unauthorized", "offline", ... otherwise. */
  state: string;
  model: string;
  /** Android API level and primary ABI; 0 / "" when the device could not be queried. */
  sdk: number;
  abi: string;
}

/**
 * The directory that names a browser install, for a session's name and a recent launch's:
 * "Chrome SxS" out of ...\Google\Chrome SxS\Application\chrome.exe, "Firefox Nightly" out of
 * ...\Firefox Nightlyirefox.exe, since Firefox keeps its executable one level up.
 */
export function browserInstallName(exe: string): string {
  const parts = exe.replace(/\\/g, "/").split("/").filter((p) => p.length);
  const up = parts[parts.length - 1]?.toLowerCase() === "firefox.exe" ? 2 : 3;
  return parts[parts.length - up] ?? parts[parts.length - 1] ?? "a browser";
}

/** A browser found on this machine (main/browsers.ts), for the launch dialog. */
export interface BrowserInstall {
  /** "Google Chrome Canary", "Firefox Nightly". */
  name: string;
  /** The executable. */
  path: string;
  /** Its build's version (Chromium's versioned directory, Firefox's application.ini); empty when unknown. */
  version: string;
  /**
   * Which engine it is, which decides how it is launched and which of its processes is followed:
   * Chromium's WebGPU is Dawn in a --type=gpu-process child, Firefox's is wgpu in its " gpu" one.
   */
  family: "chromium" | "firefox";
}

export interface AndroidDeviceList {
  /** The adb executable used, or null when none was found (error says so). */
  adb: string | null;
  devices: AndroidDevice[];
  /** Whether the Android layer (tools/build_android.py) is available to the app. */
  layer: boolean;
  error: string | null;
}

export interface SessionStatusMessage extends StatusMessage { sessionId: number }
export interface SessionLogMessage { sessionId: number; line: string }
export interface SessionMessages { sessionId: number; messages: LayerMessage[] }

/** Built-in UI themes (palettes in renderer/css/theme.css). */
export const THEMES = ["dark", "light"] as const;
export type ThemeName = (typeof THEMES)[number];

export interface AppConfig {
  /** Recently launched configurations, most recent first. */
  recents: LaunchConfig[];
  /** Recently saved or opened capture files, most recent first. */
  recentCaptures: string[];
  layerDir: string | null;
  /** The host platform (process.platform). "darwin" has no capture layer: Android targets and
   *  saved captures only, so the launch dialog offers neither local target there. */
  platform: string;
  /** The theme in effect (a persisted user setting). */
  theme: ThemeName;
  /** "main": the launcher window. "session": a window showing sessions moved out of the main window. */
  windowMode: "main" | "session";
  /** Sessions currently assigned to the window that asked. */
  sessions: SessionInfo[];
  /** The application version (package.json). */
  version: string;
  /** Whether this build can update itself (installed builds only, not `npm start`). */
  canUpdate: boolean;
  /** attachDialog: open the attach dialog at startup (--debug-attach-dialog), for a screenshot of the
   *  applications a capture library is serving.
   *  launchDialog: open the launch dialog at startup, on the "native", "android", "implicit" or
   *  "waitD3D12" target ("android:<text>" and "waitD3D12:<text>" prefill the name field) (testing aid). */
  debug: {
    select: string | null; capture: boolean; captureFrames: number; launchDialog: string | null; attachDialog: boolean;
    /** --debug-capture-stacks: the debug capture records command stack traces. */
    captureStacks: boolean;
    /**
     * --debug-capture-delay=<ms>: how long after the application connects the debug capture is
     * taken, 1500 by default. A real application is still on its loading screen then — a Unity
     * player spends its first seconds on the splash — so capturing a frame of the actual
     * application means waiting for it.
     */
    captureDelayMs: number | null;
    /**
     * --debug-capture-without=<list>: what the debug capture leaves out, of "textures", "buffers",
     * "images" and "profile" (comma separated), for measuring what each read-back costs on a frame
     * that is slow to capture.
     */
    captureWithout: string | null;
    /**
     * --debug-capture-with=<list>: options that are off by default and the debug capture turns on,
     * of "overdraw" and "stacks" (comma separated). Overdraw is the one that makes the capture
     * library compile pipelines of its own, which is how a case checks that the library's work is
     * kept out of the application's CPU timeline.
     */
    captureWith: string | null;
    /** Open the Stack trace section of selected commands and objects (symbolizes at once). */
    expandStacks: boolean;
    /** Start a session waiting for an application the implicit layer brings (--wait-for-app). */
    waitForApp: boolean;
    /** --debug-command=<index>: select that command of the debug capture or the opened file. */
    selectCommand: number | null;
    /** --debug-view=<stats|graph>: open that report of the capture instead of a command. */
    showView: string | null;
    /** --debug-timing=<ms>: run a timing capture for this long once connected, then stop. */
    timingMs?: number | null;
    /** --debug-memory=<ms>: run a memory capture for this long once connected, then stop. */
    memoryMs?: number | null;
    /** --debug-expand=<text>: open the selected command's section whose title contains that text. */
    expandSection: string | null;
    /** --debug-open=<file>: open a capture file at startup. --debug-save=<file>: save the debug capture there. */
    openCapture: string | null; saveCapture: string | null;
    /** --debug-export=<file>: write the report --debug-view opened to that standalone HTML file. */
    exportReport: string | null;
    /** --debug-export-cpp=<directory>: export the opened capture to C++ into a folder of its own there. */
    exportCpp: string | null;
  };
}

/** Options of the save-file dialog (InspectorApi.saveFile); `path` writes without a dialog. */
export interface SaveFileOptions {
  title?: string;
  defaultPath?: string;
  filters?: { name: string; extensions: string[] }[];
  path?: string;
}

export interface OpenFileOptions {
  title?: string;
  directory?: boolean;
  /** A name for this use of the dialog: it opens where it was last confirmed under that name, across sessions. */
  remember?: string;
  filters?: { name: string; extensions: string[] }[];
}

/** Progress of the application's self-update (main -> renderer, "inspector:update"). */
export type UpdateStatus =
  | { state: "checking" }
  | { state: "available"; version: string }
  | { state: "up-to-date"; version: string }
  | { state: "downloading"; percent: number }
  | { state: "downloaded"; version: string }
  | { state: "error"; message: string };

/**
 * An application a capture library is serving right now, as the attach list shows it
 * (src/main/target_probe.ts finds them by probing the ports).
 */
export interface InspectableTarget {
  port: number;
  /** "Vulkan", "D3D12" or "Metal"; "" from a capture library too old to answer a probe. */
  api: string;
  /** The application's own name for itself, empty when it has none (or has not started yet). */
  name: string;
  /** The executable's base name; the fallback identity, and never empty in practice. */
  exe: string;
  pid: number;
  /** Whether an inspector is attached already: attaching takes the connection from it. */
  busy: boolean;
}

/**
 * How the attach list names a target: its own name when it has one, else the executable, and both
 * when they differ. An application usually names itself after its executable, so the extension is
 * ignored in that comparison — "Game (Game.exe)" says nothing twice over.
 */
export function targetDisplayName(t: InspectableTarget): string {
  if (!t.name) return t.exe || `port ${t.port}`;
  if (!t.exe) return t.name;
  const name = t.name.toLowerCase();
  const exe = t.exe.toLowerCase();
  return name === exe || name === exe.replace(/\.[^.]*$/, "") ? t.exe : `${t.name} (${t.exe})`;
}

export interface LaunchResult {
  ok: boolean;
  sessionId?: number;
  pid?: number;
  port?: number;
  error?: string;
}

export interface ShaderTextResult {
  ok: boolean;
  text: string;
}

export type ShaderTextMode = "dis" | "glsl" | "hlsl" | "msl";

/** Source languages the shader editor compiles to SPIR-V (with the Vulkan SDK's tools). */
export type ShaderLanguage = "glsl" | "hlsl" | "spirv-asm";

export interface CompileShaderResult {
  ok: boolean;
  /** The compiled module: SPIR-V, or DXIL/DXBC bytecode from compileDxil. */
  spirv?: Uint8Array;
  /** Compiler output (errors and warnings). */
  log: string;
  /** The tool that ran, for the status line. */
  tool: string;
}

/** A SPIR-V module decompiled to GLSL and recompiled with line information, for the shader debugger. */
export interface DebugTranslationResult extends CompileShaderResult {
  /** spirv-cross's GLSL, when it got that far. */
  source?: string;
}

/** The implicit registration of the capture layer for this user (inspector:implicitLayer). */
export interface ImplicitLayerStatus {
  registered: boolean;
  /** The manifest the registration points at (or would). */
  manifest: string;
  error?: string;
}

/** VKINSP_ENABLE and VKINSP_PORT set for the user's account (main/implicit_layer.ts). */
export interface UserEnvironmentStatus {
  set: boolean;
  port: number | null;
  location: string;
  needsLogin?: boolean;
  error?: string;
}
