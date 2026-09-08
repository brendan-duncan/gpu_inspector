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
  /** Estimated display refresh interval while vsync is on (FIFO present modes); 0 without vsync. */
  refreshMs?: number;
  presentMode?: string;
  /** Refreshes in the interval that repeated the previous frame (dropped frames), and the
   *  layer's running total since the refresh estimate was made. */
  dropped?: number;
  droppedTotal?: number;
}
export interface PongMessage { action: "Pong" }

export interface ObjectBlobMessage {
  action: "ObjectBlob";
  id: number;
  index: number;
  size: number;
  __binary?: Uint8Array;
}

export interface CaptureFrameResultsMessage { action: "CaptureFrameResults"; frame: number; frames: number; count: number; batches: number }

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
}

export interface CaptureDescriptorBinding {
  binding: number;
  type: string;         // "VK_DESCRIPTOR_TYPE_..."
  stages?: string;
  descriptors: (CaptureDescriptor | null)[];
}

/** Snapshot of a descriptor set's contents taken when it was bound. */
export interface CaptureDescriptorSet {
  set: number;
  descriptorSet: HandleRef | null;   // null for push descriptors
  layout?: HandleRef | null;
  bindings: CaptureDescriptorBinding[];
}

export interface CaptureDescriptorSets {
  bindPoint: string;    // "VK_PIPELINE_BIND_POINT_..."
  sets: CaptureDescriptorSet[];
}

export interface CaptureChildCommand {
  method: string;
  args: ArgObject | null;
  children?: CaptureChildBuffer[];
  descriptors?: CaptureDescriptorSets;
  bufferData?: number[];
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
  args: ArgObject | null;
  result?: number;
  children?: CaptureChildBuffer[]; // secondary command buffers of vkCmdExecuteCommands
  /** Set on commands the UI inlined from a secondary command buffer: that buffer's object id. */
  secondary?: number;
  /** vkCmdBindDescriptorSets / vkCmdPushDescriptorSet: what the bound sets contained. */
  descriptors?: CaptureDescriptorSets;
  /** vkCmdBindVertexBuffers / vkCmdBindIndexBuffer / indirect draws: CaptureBuffers ids per bound buffer (0 = none). */
  bufferData?: number[];
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
  aspect: "color" | "depth";
  width: number;
  height: number;
  depth: number;
  layers: number;
  mip: number;
  size: number;
  error?: string;
  /** "sampled": an image bound by a descriptor set (read back once per view); absent = a render pass attachment. */
  kind?: "attachment" | "sampled";
  /** Sampled images: the id descriptors reference in `data`, the view, and the view's first layer. */
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
}

export interface CapturePassTimingsMessage { action: "CapturePassTimings"; timestampPeriodNs: number; count: number; passes: PassTiming[] }

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

export type LayerMessage =
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
  | ObjectBlobMessage
  | CaptureFrameResultsMessage
  | CaptureFrameCommandsMessage
  | CaptureTextureFramesMessage
  | CaptureTextureDataMessage
  | CaptureBuffersMessage
  | CaptureBufferDataMessage
  | CapturePassTimingsMessage
  | ShaderReplacedMessage
  | ImageDataMessage;

// ------------------------------------------------------------------------------------------
// UI -> Layer

export interface PingRequest { action: "Ping" }
/** Asks the layer to resend the live object snapshot (a window picking up a running session). */
export interface RequestSnapshotRequest { action: "RequestSnapshot" }
export interface RequestBlobRequest { action: "RequestBlob"; id: number; index: number }
/** Asks the layer to read back one subresource of a live VkImage (answered by ImageData). */
export interface RequestImageRequest { action: "RequestImage"; id: number; mip: number; layer: number }
/** Asks for the current contents of a VkDescriptorSet (answered by an ObjectUpdate carrying `bindings`). */
export interface RequestDescriptorSetRequest { action: "RequestDescriptorSet"; id: number }
export interface SettingsRequest { action: "Settings"; recordAlways?: boolean }
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
}

/** Live shader editing: rebuild a pipeline with one stage replaced by the given SPIR-V (base64). */
export interface ReplaceShaderRequest { action: "ReplaceShader"; pipeline: number; stage: string; spirv: string }
/** Drops the edit of one stage (or of every stage when `stage` is omitted). */
export interface RestoreShaderRequest { action: "RestoreShader"; pipeline: number; stage?: string }

export type UiRequest = PingRequest | RequestSnapshotRequest | RequestBlobRequest | RequestImageRequest | RequestDescriptorSetRequest
  | SettingsRequest | CaptureRequest | ReplaceShaderRequest | RestoreShaderRequest;

// ------------------------------------------------------------------------------------------
// Electron main <-> renderer

/** A capture taken automatically once the launched application connects. */
export interface QueuedCapture {
  /** "frame": capture frame `value` (0 = the first frame); "time": capture `value` seconds after connecting. */
  mode: "none" | "frame" | "time";
  value: number;
}

export interface LaunchConfig {
  /** "native": an executable on this machine. "android": a package on a device reached through adb. */
  target: "native" | "android";
  /** Executable path, or the package name for an Android target. */
  exe: string;
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
  /** Also enable VK_LAYER_KHRONOS_validation (native targets), whose messages the Inspect tab lists. */
  validation: boolean;
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
  /** launchDialog: open the launch dialog at startup, on the "native" or "android" target (testing aid). */
  debug: {
    select: string | null; capture: boolean; captureFrames: number; launchDialog: string | null;
    /** --debug-open=<file>: open a capture file at startup. --debug-save=<file>: save the debug capture there. */
    openCapture: string | null; saveCapture: string | null;
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
  spirv?: Uint8Array;
  /** Compiler output (errors and warnings). */
  log: string;
  /** The tool that ran, for the status line. */
  tool: string;
}
