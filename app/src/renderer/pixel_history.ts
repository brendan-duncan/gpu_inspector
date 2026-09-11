// Pixel history: every pass start, clear and draw that touched one pixel of a render target, what
// each draw's fragments at the pixel met, and the value after each (RenderDoc's pixel history). A
// Vulkan capture is replayed for it (vkinsp_replay --pixel --pixel-data, replay/src/history.cpp); a
// Metal application follows the pixel while capturing (a capture with `pixelHistory`,
// metal/src/pixel_history.mm). Both write the same JSON; this reads it and says it in words, for the
// app's tab and the MCP server.
import type { ImageDataInfo } from "../shared/protocol.js";
import { decodeImage, decodeTexels, formatTexel } from "./vulkan/texture_decode.js";

/** The pixel a history follows: an image by its object id, and a subresource of it. */
export interface PixelRequest {
  image: number;
  x: number;
  y: number;
  mip?: number;
  layer?: number;
}

/** "load": a pass starting from what it loaded or cleared; "clear": vkCmdClearAttachments; "draw": a draw. */
export type PixelEventKind = "load" | "clear" | "draw";

export interface PixelEvent {
  kind: PixelEventKind;
  /** The command's index in the capture (the pass's begin for "load"). */
  command: number;
  method: string;
  /** "load": the attachment's load op. */
  detail: string;
  commandBuffer: number;
  frame: number;
  passIndex: number;
  /** The draw's pipeline (object id), 0 without. */
  pipeline: number;
  /** The pixel is outside the draw's scissor. */
  scissored: boolean;
  /** Bit per sample count below that was measured (1 covered ... 32 passed). */
  testsMeasured: number;
  /** Samples at the pixel: the draw's primitives with no culling and no tests. */
  covered: number;
  /** ... with the pipeline's culling. */
  facing: number;
  /** ... and its fragment shader (discards count). */
  shaded: number;
  /** ... with the depth test alone. */
  depthPassed: number;
  /** ... with the stencil test alone. */
  stencilPassed: number;
  /** ... with every test: what the draw wrote. */
  passed: number;
  /** The pixel's texel after the event, in the history's pixelFormat. */
  value: Uint8Array;
  /** The pass's depth at the pixel after the event, in depthFormat (empty without depth). */
  depth: Uint8Array;
}

export interface PixelHistory {
  device: string;
  image: number;
  x: number;
  y: number;
  mip: number;
  layer: number;
  /** The image the request named, when the history followed another (a Metal drawable of the next frame); else `image`. */
  requestedImage: number;
  /** Vulkan format names, as the replay read the texels. */
  pixelFormat: string;
  depthFormat: string;
  events: PixelEvent[];
  /** What the replay could not follow (passes left out, multisampled images). */
  notes: string[];
  /** What the replay could not rebuild of the capture (the first hundred). */
  problems: string[];
}

function hexBytes(text: unknown): Uint8Array {
  const s = typeof text === "string" ? text : "";
  const out = new Uint8Array(Math.floor(s.length / 2));
  for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
  return out;
}

/** Parses vkinsp_replay's --pixel-data JSON, or the history a Metal capture sent (already parsed). */
export function parsePixelHistory(input: Uint8Array | string | object): PixelHistory {
  let json: Record<string, unknown>;
  if (input instanceof Uint8Array || typeof input === "string") {
    const text = typeof input === "string" ? input : new TextDecoder().decode(input);
    try {
      json = JSON.parse(text) as Record<string, unknown>;
    } catch (e) {
      throw new Error(`The pixel history is not valid JSON: ${(e as Error).message}`);
    }
  } else {
    json = input as Record<string, unknown>;
  }
  if (json?.format !== "gpu-inspector-pixel-history") throw new Error("Not a pixel history.");
  const num = (v: unknown): number => (typeof v === "number" ? v : 0);
  const str = (v: unknown): string => (typeof v === "string" ? v : "");
  const events = (Array.isArray(json.events) ? json.events : []).map((raw): PixelEvent => {
    const e = raw as Record<string, unknown>;
    const kind = str(e.kind);
    return {
      kind: kind === "load" || kind === "clear" ? kind : "draw",
      command: num(e.command), method: str(e.method), detail: str(e.detail),
      commandBuffer: num(e.commandBuffer), frame: num(e.frame), passIndex: num(e.passIndex), pipeline: num(e.pipeline),
      scissored: e.scissored === true, testsMeasured: num(e.testsMeasured),
      covered: num(e.covered), facing: num(e.facing), shaded: num(e.shaded),
      depthPassed: num(e.depthPassed), stencilPassed: num(e.stencilPassed), passed: num(e.passed),
      value: hexBytes(e.value), depth: hexBytes(e.depth),
    };
  });
  const strings = (v: unknown): string[] => (Array.isArray(v) ? v.filter((s): s is string => typeof s === "string") : []);
  return {
    device: str(json.device), image: num(json.image),
    requestedImage: typeof json.requestedImage === "number" ? json.requestedImage : num(json.image), x: num(json.x), y: num(json.y), mip: num(json.mip), layer: num(json.layer),
    pixelFormat: str(json.pixelFormat), depthFormat: str(json.depthFormat), events,
    notes: strings(json.notes), problems: strings(json.problems),
  };
}

// ---------------------------------------------------------------------------------------------
// What a draw's fragments met

const MEASURED_COVERED = 1;
const MEASURED_FACING = 2;
const MEASURED_SHADED = 4;
const MEASURED_DEPTH = 8;
const MEASURED_STENCIL = 16;
const MEASURED_ALL = 32;

export type DrawOutcome =
  | "scissored" | "unmeasured" | "missed" | "culled" | "discarded"
  | "depth" | "stencil" | "depth-stencil" | "tests" | "wrote" | "covers";

/**
 * What a draw's fragments at the pixel met, from its sample counts: each count adds one step
 * (covering, facing, shading, the tests), so the first that is zero is where they stopped.
 */
export function drawOutcome(e: PixelEvent): DrawOutcome {
  const measured = (bit: number): boolean => (e.testsMeasured & bit) !== 0;
  if (e.scissored) return "scissored";
  if (!e.testsMeasured) return "unmeasured";
  if (measured(MEASURED_COVERED) && !e.covered) return "missed";
  if (measured(MEASURED_FACING) && !e.facing) return "culled";
  if (measured(MEASURED_SHADED) && !e.shaded) return "discarded";
  const depthFailed = measured(MEASURED_DEPTH) && !e.depthPassed;
  const stencilFailed = measured(MEASURED_STENCIL) && !e.stencilPassed;
  if (depthFailed && stencilFailed) return "depth-stencil";
  if (depthFailed) return "depth";
  if (stencilFailed) return "stencil";
  if (measured(MEASURED_ALL)) return e.passed ? "wrote" : "tests";
  return "covers";
}

export const OUTCOME_TEXT: Record<DrawOutcome, string> = {
  scissored: "outside the scissor",
  unmeasured: "not measured (the draw's pipeline could not be copied)",
  missed: "does not reach the pixel",
  culled: "culled",
  discarded: "discarded by the fragment shader",
  depth: "failed the depth test",
  stencil: "failed the stencil test",
  "depth-stencil": "failed the depth and stencil tests",
  tests: "failed the depth and stencil tests together",
  wrote: "wrote the pixel",
  covers: "covers the pixel",
};

/** Whether the event touched the pixel: a pass start, a clear, or a draw whose primitives reach it. */
export function touchesPixel(e: PixelEvent): boolean {
  if (e.kind !== "draw") return true;
  const outcome = drawOutcome(e);
  return outcome !== "scissored" && outcome !== "missed";
}

/** One line for an event: what it was and, for a draw, what its fragments met. */
export function eventSummary(e: PixelEvent): string {
  if (e.kind === "load") return `pass ${e.passIndex} begins (${e.detail.replace(/^(VK_ATTACHMENT_LOAD_OP_|MTLLoadAction)/, "") || "load"})`;
  if (e.kind === "clear") return `${e.method}: cleared`;
  const outcome = drawOutcome(e);
  const samples = outcome === "wrote" ? ` (${e.passed} sample${e.passed === 1 ? "" : "s"} passed)` : "";
  return `${e.method}: ${OUTCOME_TEXT[outcome]}${samples}`;
}

/** The sample counts behind a draw's outcome, for a tooltip. */
export function sampleCountsText(e: PixelEvent): string {
  if (e.kind !== "draw" || !e.testsMeasured) return "";
  const parts: string[] = [];
  const add = (bit: number, label: string, n: number): void => {
    if (e.testsMeasured & bit) parts.push(`${label} ${n}`);
  };
  add(MEASURED_COVERED, "covering", e.covered);
  add(MEASURED_FACING, "facing", e.facing);
  add(MEASURED_SHADED, "shaded", e.shaded);
  add(MEASURED_DEPTH, "passing depth", e.depthPassed);
  add(MEASURED_STENCIL, "passing stencil", e.stencilPassed);
  add(MEASURED_ALL, "passing every test", e.passed);
  return `Samples at the pixel: ${parts.join(", ")}. Each count adds one step to the one before, measured with depth and stencil writes off, against what the pass held before the draw.`;
}

// ---------------------------------------------------------------------------------------------
// Texel values

/** A texel as a 1x1 image the texture decoders read. */
function texelInfo(format: string, depth: boolean): ImageDataInfo {
  return { format, aspect: depth ? "depth" : "color", width: 1, height: 1 };
}

/** A texel's channels as "R: 0.78" lines; the bytes in hex for a format the decoders do not know. */
export function texelLines(format: string, bytes: Uint8Array, depth = false): string[] {
  if (!bytes.byteLength) return [];
  const tex = format ? decodeTexels(texelInfo(format, depth), bytes) : null;
  if (!tex) return [`bytes ${[...bytes].map((b) => b.toString(16).padStart(2, "0")).join(" ")}`];
  if (depth) return [`depth ${Number(tex.values[0].toPrecision(7))}`];
  return formatTexel(tex, 0, 0);
}

/** A texel's channel values as numbers (sRGB formats decoded to linear), or null when the format is unknown. */
export function texelValues(format: string, bytes: Uint8Array, depth = false): number[] | null {
  if (!bytes.byteLength || !format) return null;
  const tex = decodeTexels(texelInfo(format, depth), bytes);
  return tex ? Array.from(tex.values.slice(0, tex.channels)) : null;
}

/** A colour texel as a CSS colour, for a swatch; null for depth or an unknown format. */
export function texelCss(format: string, bytes: Uint8Array): string | null {
  if (!bytes.byteLength || !format) return null;
  const rgba = decodeImage(texelInfo(format, false), bytes);
  return rgba ? `rgba(${rgba[0]}, ${rgba[1]}, ${rgba[2]}, ${(rgba[3] / 255).toFixed(3)})` : null;
}
