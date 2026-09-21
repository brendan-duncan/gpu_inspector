// The graphics APIs the inspector knows, as one object per API.
//
// A capture names its API (`CaptureFrameResults.api`, a `.gpucap` manifest's `api`), and everything
// that has to treat one API differently from another asks the Backend registered under that name:
// how its commands are classified, what its objects are called, which measurements it can take
// and how. Vulkan, Direct3D 12 and Metal are built in; a plugin (docs/PLUGINS.md) registers another
// at start-up with registerBackend, from the backend module its plugin.json names. A capture of an
// API nobody registered still opens, on a backend that classifies nothing: its commands list flat,
// its objects and arguments read as they came.
//
// DOM-free: the MCP server (src/mcp/) asks the same registry.
import { D3D12_SETS } from "./d3d12/command_sets.js";
import { D3D12ResourceSource } from "./d3d12/frame_resources.js";
import { METAL_SETS } from "./metal/command_sets.js";
import { MetalResourceSource } from "./metal/frame_resources.js";
import { VULKAN_SETS } from "./vulkan/command_sets.js";
import { VulkanResourceSource } from "./vulkan/frame_resources.js";
import type { CommandSets } from "./command_sets.js";
import type { ResourceSource } from "./frame_graph.js";
import type { CaptureData } from "./capture_data.js";
import type { DrawState } from "./draw_state.js";
import type { FrameAnalysisDatabase, FrameFinding } from "./vulkan/frame_analysis.js";
import type { ObjectLookup, VulkanObject } from "./vulkan/vulkan_object.js";
import type { MemberLayout } from "./vulkan/buffer_layout.js";
import type { ArgValue, CaptureCommand } from "../shared/protocol.js";

/**
 * What the app can measure by replaying a capture of this API on this machine: the replay tool's
 * analyses (main/replay.ts). Each flag decides whether the matching button is offered.
 */
export interface BackendReplay {
  /** Measure Draws: per-draw GPU time and statistics. */
  draws: boolean;
  /** Per-shader ablation (the cost of each stage, measured by replaying variants). */
  shaders: boolean;
  /** The GPU's hardware counters. */
  hwCounters: boolean;
  overdraw: boolean;
  pixelHistory: boolean;
  /** Where one draw landed on its target (the draw overlays), and what its vertex shader wrote (VS Out). */
  drawOverlay: boolean;
  /** Export to C++: a project that replays the frame on its own. */
  exportCpp: boolean;
  /** Replaying the frame with a shader edited, for what the edit does to the render targets. */
  edits: boolean;
}

/**
 * What the capture library measures in the application while it captures, when asked in the
 * Capture request, rather than a replay measuring it afterwards. A capture that measured one of
 * these answers only for what it was asked, so the panels offer to capture again instead.
 */
export interface BackendLiveMeasurements {
  overdraw: boolean;
  pixelHistory: boolean;
  drawOverlay: boolean;
}

/** How an API spells the fixes the render graph's rules suggest. */
export interface BackendAdvice {
  /** Not storing a target the frame never reads again: "store op DONT_CARE". */
  discard: string;
  /** A sentence on merging a pass into the pass whose output it reads (Vulkan: subpasses and input attachments). */
  subpass: string;
  /** Keeping an intermediate target in tile memory: "TRANSIENT_ATTACHMENT usage with LAZILY_ALLOCATED memory". */
  transient: string;
}

/** One value in a plugin's command details: text, or something the host knows how to show. */
export type DetailValue =
  | string
  | number
  | boolean
  | null
  /** A tracked object: shown as a link to it. */
  | { object: number; text?: string }
  /** A captured texture (a CaptureTextureFrames `capture` id): shown as a thumbnail opening the image viewer. */
  | { texture: number; text?: string }
  /**
   * A captured buffer range (a CaptureBuffers id): shown as its size, opening its contents, typed
   * by `members` when there are some (each at the offset the driver reported; MemberLayout in
   * vulkan/buffer_layout.ts), else by `layout`, GLSL struct declarations laid out by `rules`
   * (std140 by default), the text the buffer view's Format button takes.
   */
  | { buffer: number; text?: string; layout?: string; rules?: "std140" | "std430"; members?: MemberLayout[]; blockName?: string; blockSize?: number }
  /** Serialized arguments, shown as the Arguments tree shows them. */
  | { args: ArgValue };

/**
 * A section of the command details a plugin adds, rendered by the host with its own widgets: the
 * plugin describes, the host draws, so a backend module needs no DOM and the MCP server can
 * report the same sections as text.
 */
export interface DetailSection {
  title: string;
  collapsed?: boolean;
  /** Label / value pairs. */
  rows?: [string, DetailValue][];
  /** A table: a header row, then rows of cells. */
  table?: { columns: string[]; rows: DetailValue[][] };
  /** Preformatted text (shader source, a log), highlighted as `language` when the host knows it. */
  code?: { text: string; language?: string };
  /** A plain note, muted. */
  note?: string;
}

/** What a backend's command details can look at. */
export interface DetailContext {
  data: CaptureData;
  db: ObjectLookup;
  /** An object's display name. */
  nameOf(id: number | null | undefined): string;
}

export interface Backend {
  /** The name captures carry in `api`: "vulkan", "d3d12", "metal", or a plugin's. */
  readonly id: string;
  /** How the API is named to people: "Vulkan", "Direct3D 12", "Metal", "OpenGL ES". */
  readonly displayName: string;
  /**
   * Prefixes of this API's object type names ("Vk", "MTL", "GL"), which tell whose an object is when
   * nothing else does, and which the short type name drops ("VkImage" lists as "Image").
   */
  readonly objectTypePrefixes: readonly string[];
  readonly sets: CommandSets;
  readonly replay: BackendReplay;
  readonly live: BackendLiveMeasurements;
  /** The call whose CPU time FrameStats' `submitMs` measures ("vkQueueSubmit"), for the Frame Bound card. */
  readonly submitCall?: string;
  /** Whether the backend is built into this app (Vulkan, D3D12 and Metal) rather than a plugin's. */
  readonly builtin?: boolean;
  /** Plugins: the plugin's directory and version, for the About box and bug reports. */
  readonly plugin?: { id: string; version: string; dir: string };

  /** What each command touches, for the render graph (frame_graph.ts). Without one the graph is empty. */
  resourceSource?(db: ObjectLookup): ResourceSource;
  /** This API's own frame analysis rules (the API-neutral rules run for every backend regardless). */
  analyzeFrame?(data: CaptureData, db: FrameAnalysisDatabase): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> };
  /**
   * The state bound at a draw or dispatch, for an API whose state the generic walk back through
   * the command stream cannot reconstruct (draw_state.ts): a capture library that attaches the
   * state to the draw itself answers from there. Vertex layouts go in `vertexInput` in the shape
   * of vkCmdSetVertexInputEXT's arguments, with VK_FORMAT_* names, which the mesh view reads.
   */
  drawState?(data: CaptureData, db: ObjectLookup, cmd: CaptureCommand): DrawState | null;
  /** A draw's vertex inputs by location ("position", "uv"), for the mesh view's columns; null to leave them unnamed. */
  vertexInputNames?(cmd: CaptureCommand): Map<number, string> | null;
  /** Sections to show for a command, above its Arguments (draws: the pipeline state, the shaders, the bindings). */
  commandDetails?(cmd: CaptureCommand, ctx: DetailContext): DetailSection[];
  /** This API's spelling of the render graph's advice (render_graph_analysis.ts); generic words where absent. */
  readonly advice?: Partial<BackendAdvice>;
  /** One line describing an object, beside its name in lists ("RGBA8 1920x1080", "vertex shader"). */
  objectSummary?(obj: VulkanObject): string | undefined;
  /** The GPU memory an object occupies, for the memory views; 0 or absent for objects that hold none. */
  objectBytes?(obj: VulkanObject): number;
}

const NO_REPLAY: BackendReplay = { draws: false, shaders: false, hwCounters: false, overdraw: false, pixelHistory: false, drawOverlay: false, exportCpp: false, edits: false };
const NOT_LIVE: BackendLiveMeasurements = { overdraw: false, pixelHistory: false, drawOverlay: false };

const NONE: ReadonlySet<string> = new Set();

/** The classification of an API nobody registered: nothing is a draw, a pass or a binding. */
export const EMPTY_SETS: CommandSets = {
  DRAW: NONE, DISPATCH: NONE, TRACE: NONE, PASS_BEGIN: NONE, PASS_END: NONE, LABEL_BEGIN: NONE, LABEL_END: NONE,
  SUBMIT: NONE, BIND_DESCRIPTOR: NONE, BIND_VERTEX: NONE, BIND_INDEX: NONE, PUSH_CONSTANT: NONE, INDIRECT: NONE,
  COMPUTE_PASS_END: NONE, RECORD_BEGIN: NONE, RECORD_END: NONE, BIND_PIPELINE: NONE,
  bindPointOf: () => "graphics",
  pipelineBindPointOf: () => "graphics",
  graphicsBindPoint: "graphics",
  vertexBuffersOf: () => [],
  indexBufferOf: () => null,
};

const BUILTIN: Backend[] = [
  {
    id: "vulkan",
    displayName: "Vulkan",
    objectTypePrefixes: ["Vk"],
    builtin: true,
    get sets() { return VULKAN_SETS; },
    // vkinsp_replay serves every analysis (docs/REPLAY.md).
    replay: { draws: true, shaders: true, hwCounters: true, overdraw: true, pixelHistory: true, drawOverlay: true, exportCpp: true, edits: true },
    submitCall: "vkQueueSubmit",
    live: NOT_LIVE,
    resourceSource: (db) => new VulkanResourceSource(db),
  },
  {
    id: "d3d12",
    displayName: "Direct3D 12",
    objectTypePrefixes: ["ID3D12", "IDXGI"],
    builtin: true,
    get sets() { return D3D12_SETS; },
    // dxinsp_replay measures draws, shaders and counters; the library itself measures the rest.
    replay: { ...NO_REPLAY, draws: true, shaders: true, hwCounters: true, exportCpp: true, edits: true },
    submitCall: "ExecuteCommandLists",
    live: { overdraw: true, pixelHistory: true, drawOverlay: true },
    resourceSource: (db) => new D3D12ResourceSource(db),
  },
  {
    id: "metal",
    displayName: "Metal",
    objectTypePrefixes: ["MTL", "CA"],
    builtin: true,
    get sets() { return METAL_SETS; },
    // mtlinsp_replay compares and exports; it serves no analyses.
    replay: { ...NO_REPLAY, exportCpp: true },
    submitCall: "commit",
    live: { overdraw: true, pixelHistory: true, drawOverlay: true },
    resourceSource: (db) => new MetalResourceSource(db),
  },
];

const registered = new Map<string, Backend>(BUILTIN.map((b) => [b.id, b]));
const unknown = new Map<string, Backend>();

/**
 * Adds a backend, or replaces the one registered under its id (a plugin cannot replace a built-in
 * one: that throws). Called by the plugin loader with what a plugin's `activate` returned.
 */
export function registerBackend(backend: Backend): void {
  if (!backend || typeof backend.id !== "string" || !backend.id) throw new Error("a backend needs an id");
  if (!backend.sets) throw new Error(`backend ${backend.id} has no command sets`);
  if (registered.get(backend.id)?.builtin) throw new Error(`"${backend.id}" is built in and cannot be replaced by a plugin`);
  // The plugin's object stays the prototype, so methods it defines on a class keep working.
  const wrapped: Backend = Object.setPrototypeOf({
    builtin: false,
    objectTypePrefixes: backend.objectTypePrefixes ?? [],
    replay: { ...NO_REPLAY, ...backend.replay },
    live: { ...NOT_LIVE, ...backend.live },
  }, backend);
  registered.set(backend.id, wrapped);
  unknown.delete(backend.id);
}

/** The backend for a capture's API; an unregistered API gets one that classifies nothing (see EMPTY_SETS). */
export function backendFor(api: string | undefined | null): Backend {
  const id = api || "vulkan";   // captures older than the field are Vulkan's
  const known = registered.get(id);
  if (known) return known;
  let b = unknown.get(id);
  if (!b) {
    b = { id, displayName: id, objectTypePrefixes: [], sets: EMPTY_SETS, replay: NO_REPLAY, live: NOT_LIVE };
    unknown.set(id, b);
  }
  return b;
}

/** Whether a backend was registered for `api` (a built-in one, or a plugin's). */
export function isKnownApi(api: string | undefined | null): boolean {
  return registered.has(api || "vulkan");
}

/** Every registered backend, built-in ones first. */
export function registeredBackends(): Backend[] {
  return [...registered.values()];
}

/** The backend whose objects carry type names like `type` ("GLTexture" -> the GLES plugin's), or null. */
export function backendForObjectType(type: string): Backend | null {
  let best: Backend | null = null;
  let length = 0;
  for (const b of registered.values()) {
    for (const p of b.objectTypePrefixes) {
      if (p.length > length && type.startsWith(p)) {
        best = b;
        length = p.length;
      }
    }
  }
  return best;
}

/** A type name without its API's prefix: "VkImage" -> "Image", "ID3D12Resource" -> "Resource". */
export function shortTypeName(type: string): string {
  const b = backendForObjectType(type);
  if (!b) return type;
  const prefix = b.objectTypePrefixes.filter((p) => type.startsWith(p)).sort((x, y) => y.length - x.length)[0] ?? "";
  // Metal keeps its prefix: "MTLTexture" reads better than "Texture" beside Metal's own documentation.
  if (b.id === "metal") return type;
  return type.length > prefix.length ? type.substring(prefix.length) : type;
}

/** The API's name as people know it, for messages: "Vulkan", "Direct3D 12", a plugin's. */
export function apiDisplayName(api: string | undefined | null): string {
  return backendFor(api).displayName;
}
