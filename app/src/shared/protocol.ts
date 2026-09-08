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
export interface FrameStatsMessage { action: "FrameStats"; frame: number; frameTimeMs: number }
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

export type LayerMessage =
  | SnapshotMessage
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
  | ImageDataMessage;

// ------------------------------------------------------------------------------------------
// UI -> Layer

export interface PingRequest { action: "Ping" }
/** Asks the layer to resend the live object snapshot (a window picking up a running session). */
export interface RequestSnapshotRequest { action: "RequestSnapshot" }
export interface RequestBlobRequest { action: "RequestBlob"; id: number; index: number }
/** Asks the layer to read back one subresource of a live VkImage (answered by ImageData). */
export interface RequestImageRequest { action: "RequestImage"; id: number; mip: number; layer: number }
export interface SettingsRequest { action: "Settings"; recordAlways?: boolean }
export interface CaptureRequest {
  action: "Capture";
  frameCount: number;
  /** Bytes captured per bound buffer range (longer ranges are truncated). */
  maxBufferSize?: number;
  /** Total buffer bytes captured per capture; further buffers are reported as errors. */
  maxBufferTotal?: number;
  maxTextureSize?: number;
  captureTextures?: boolean;
  /** Read back the buffers bound by descriptor sets, vertex/index bindings and indirect draws. */
  captureBuffers?: boolean;
}

export type UiRequest = PingRequest | RequestSnapshotRequest | RequestBlobRequest | RequestImageRequest | SettingsRequest | CaptureRequest;

// ------------------------------------------------------------------------------------------
// Electron main <-> renderer

export interface LaunchConfig {
  exe: string;
  args: string;
  cwd: string;
  /** Extra environment variables, one KEY=VALUE per line. */
  env: string;
  port: number;
  log: boolean;
  recordAlways: boolean;
}

export type ConnectionState = "disconnected" | "connecting" | "connected" | "launched" | "exited" | "error";

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

export interface SessionStatusMessage extends StatusMessage { sessionId: number }
export interface SessionLogMessage { sessionId: number; line: string }
export interface SessionMessages { sessionId: number; messages: LayerMessage[] }

/** Built-in UI themes (palettes in renderer/css/theme.css). */
export const THEMES = ["dark", "light"] as const;
export type ThemeName = (typeof THEMES)[number];

export interface AppConfig {
  /** Recently launched configurations, most recent first. */
  recents: LaunchConfig[];
  layerDir: string | null;
  /** The theme in effect (a persisted user setting). */
  theme: ThemeName;
  /** "main": the launcher window. "session": a window showing sessions moved out of the main window. */
  windowMode: "main" | "session";
  /** Sessions currently assigned to the window that asked. */
  sessions: SessionInfo[];
  debug: { select: string | null; capture: boolean; captureFrames: number; launchDialog: boolean };
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
