// How the MCP server writes capture data for a model to read. Object references become
// `VkImage#12 "GBuffer"` rather than {__id, __class} records, inline payloads their byte count,
// fractions four significant digits, and every string and list is bounded, so an answer stays
// compact and one large capture cannot flood the context. Also the tools' argument parsing.
import { float16ToFloat32 } from "../renderer/utils/float.js";
import { isHandleRef, refId, type ObjectLookup } from "../renderer/vulkan/vulkan_object.js";
import type { ReflType, ScalarType } from "../renderer/vulkan/spirv_reflect.js";
import { vertexFormat } from "../renderer/vulkan/vk_format.js";
import type { CapturedTexture } from "../renderer/capture_data.js";
import type { FrameFinding } from "../renderer/vulkan/frame_analysis.js";
import type { ArgValue, StackFrame, ValidationMessage } from "../shared/protocol.js";
import type { Capture } from "./capture_store.js";
import type { ToolArgs, ToolResult } from "./stdio_server.js";

/** Longest answer; a longer one is cut with a note on how to ask for less. */
const MAX_TEXT = 80_000;
const MAX_ITEMS = 200;
const MAX_STRING = 4000;
/** Elements of an array read from a buffer before the rest are counted instead. */
const MAX_ARRAY = 64;

/** `VkImage#12`, with the object's name when it has one of its own: `VkImage#12 "GBuffer Albedo"`. */
export function refText(db: ObjectLookup, v: ArgValue | number | null | undefined): string | undefined {
  if (v === null || v === undefined || v === 0) return undefined;
  const id = typeof v === "number" ? v : refId(v);
  if (id === null) return undefined;
  const o = db.getObject(id);
  const type = o?.type ?? (isHandleRef(v as ArgValue) ? (v as { __class?: string }).__class ?? "object" : "object");
  const named = o && o.name !== `${o.shortType} ${o.id}` ? ` "${o.name}"` : "";
  return `${type}#${id}${named}`;
}

/** A capture value made readable: references as refText, base64 payloads as their size, long lists and strings cut. */
export function compact(value: unknown, db: ObjectLookup, depth = 0): unknown {
  if (value === null || value === undefined) return value;
  if (typeof value === "string") return clip(value, MAX_STRING);
  if (typeof value !== "object") return value;
  if (depth > 16) return "...";
  if (Array.isArray(value)) {
    const out = value.slice(0, MAX_ITEMS).map((v) => compact(v, db, depth + 1));
    if (value.length > MAX_ITEMS) out.push(`... ${value.length - MAX_ITEMS} more`);
    return out;
  }
  if (isHandleRef(value as ArgValue)) return refText(db, value as ArgValue);
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(value)) {
    if (k === "base64" && typeof v === "string") out.bytes = base64Bytes(v);
    else out[k] = compact(v, db, depth + 1);
  }
  return out;
}

function base64Bytes(s: string): number {
  const pad = s.endsWith("==") ? 2 : s.endsWith("=") ? 1 : 0;
  return Math.floor((s.length * 3) / 4) - pad;
}

export function clip(s: string, max: number): string {
  return s.length > max ? `${s.slice(0, max)}... (${s.length - max} more characters)` : s;
}

/** A measurement to four significant digits (one decimal from 100 up); null and undefined drop out of the answer. */
export function round(v: number | null | undefined): number | undefined {
  if (v === null || v === undefined) return undefined;
  if (!Number.isFinite(v) || Number.isInteger(v)) return v;
  return Math.abs(v) >= 100 ? Math.round(v * 10) / 10 : Number(v.toPrecision(4));
}

/** A tool answer as JSON text (undefined fields left out), cut with a note when it runs too long. */
export function jsonResult(value: unknown): ToolResult {
  let text = JSON.stringify(value, null, 1);
  if (text.length > MAX_TEXT) {
    text = `${text.slice(0, MAX_TEXT)}\n... the answer was cut at ${MAX_TEXT} of ${text.length} characters: ask for less (offset and limit, a filter, one item).`;
  }
  return { content: [{ type: "text", text }] };
}

// ---------------------------------------------------------------------------------------------
// Typed values

/**
 * The value of `type` in `view` at `offset`, as JSON: scalars as numbers (booleans as booleans),
 * vectors as arrays, matrices as arrays of columns, structs as objects, long arrays cut.
 * `budget.values` bounds how many scalars one answer reads.
 */
export function readTyped(type: ReflType, view: DataView, offset: number, budget = { values: 2048 }): unknown {
  if (budget.values <= 0) return "...";
  switch (type.kind) {
    case "scalar":
      budget.values--;
      return readScalar(view, offset, type);
    case "vector": {
      budget.values -= type.count;
      const out: unknown[] = [];
      for (let i = 0; i < type.count; i++) out.push(readScalar(view, offset + i * type.element.size, type.element));
      return out;
    }
    case "matrix": {
      budget.values -= type.columns * type.rows;
      const columns: unknown[][] = [];
      for (let c = 0; c < type.columns; c++) {
        const column: unknown[] = [];
        for (let r = 0; r < type.rows; r++) {
          const at = type.rowMajor ? offset + r * type.stride + c * type.element.size : offset + c * type.stride + r * type.element.size;
          column.push(readScalar(view, at, type.element));
        }
        columns.push(column);
      }
      return columns;
    }
    case "array": {
      // A runtime-sized array (count 0) runs to the end of the data.
      const count = type.count || (type.stride > 0 ? Math.max(0, Math.floor((view.byteLength - offset) / type.stride)) : 0);
      const out: unknown[] = [];
      for (let i = 0; i < Math.min(count, MAX_ARRAY) && budget.values > 0; i++) out.push(readTyped(type.element, view, offset + i * type.stride, budget));
      if (out.length < count) out.push(`... ${count - out.length} more`);
      return out;
    }
    case "struct": {
      const out: Record<string, unknown> = {};
      for (const m of type.members) {
        if (budget.values <= 0) {
          out["..."] = "cut: read the buffer with read_buffer for the rest";
          break;
        }
        out[m.name || `offset${m.offset}`] = readTyped(m.type, view, offset + m.offset, budget);
      }
      return out;
    }
    case "format": {
      const f = vertexFormat(type.format);
      if (!f) return `<${type.format}>`;
      if (offset + f.size > view.byteLength) return null;
      budget.values -= f.channels.length;
      const v = f.read(view, offset).map(tidy);
      return v.length === 1 ? v[0] : v;
    }
    case "opaque":
      return `<${type.name}>`;
  }
}

function readScalar(view: DataView, offset: number, s: ScalarType): number | boolean | null {
  if (offset < 0 || offset + s.size > view.byteLength) return null;
  switch (s.base) {
    case "float":
      return tidy(s.width === 16 ? float16ToFloat32(view.getUint16(offset, true)) : s.width === 64 ? view.getFloat64(offset, true) : view.getFloat32(offset, true));
    case "int":
      return s.width === 8 ? view.getInt8(offset) : s.width === 16 ? view.getInt16(offset, true) : s.width === 64 ? Number(view.getBigInt64(offset, true)) : view.getInt32(offset, true);
    case "uint":
      return s.width === 8 ? view.getUint8(offset) : s.width === 16 ? view.getUint16(offset, true) : s.width === 64 ? Number(view.getBigUint64(offset, true)) : view.getUint32(offset, true);
    case "bool":
      return (s.width === 8 ? view.getUint8(offset) : view.getUint32(offset, true)) !== 0;
  }
}

/** A float to the seven significant digits a float32 holds. */
export function tidy(v: number): number {
  return Number.isFinite(v) && !Number.isInteger(v) ? Number(v.toPrecision(7)) : v;
}

// ---------------------------------------------------------------------------------------------
// Arguments

export function stringArg(args: ToolArgs, name: string): string | undefined {
  const v = args[name];
  if (v === undefined || v === null || v === "") return undefined;
  if (typeof v !== "string") throw new Error(`${name} must be a string.`);
  return v;
}

export function requireString(args: ToolArgs, name: string): string {
  const v = stringArg(args, name);
  if (v === undefined) throw new Error(`${name} is required.`);
  return v;
}

export function intArg(args: ToolArgs, name: string, fallback: number, min = -Infinity, max = Infinity): number {
  const v = optionalInt(args, name);
  return v === undefined ? fallback : Math.min(max, Math.max(min, v));
}

export function optionalInt(args: ToolArgs, name: string): number | undefined {
  const v = args[name];
  if (v === undefined || v === null || v === "") return undefined;
  const n = typeof v === "string" ? Number(v) : v;
  if (typeof n !== "number" || !Number.isInteger(n)) throw new Error(`${name} must be an integer.`);
  return n;
}

export function requireInt(args: ToolArgs, name: string): number {
  const v = optionalInt(args, name);
  if (v === undefined) throw new Error(`${name} is required.`);
  return v;
}

export function numberArg(args: ToolArgs, name: string): number | undefined {
  const v = args[name];
  if (v === undefined || v === null || v === "") return undefined;
  const n = typeof v === "string" ? Number(v) : v;
  if (typeof n !== "number" || !Number.isFinite(n)) throw new Error(`${name} must be a number.`);
  return n;
}

export function boolArg(args: ToolArgs, name: string, fallback: boolean): boolean {
  const v = args[name];
  if (v === undefined || v === null) return fallback;
  if (typeof v === "boolean") return v;
  if (v === "true" || v === "false") return v === "true";
  throw new Error(`${name} must be true or false.`);
}

/** A case-insensitive regular expression argument. */
export function regexArg(args: ToolArgs, name: string): RegExp | undefined {
  const s = stringArg(args, name);
  if (s === undefined) return undefined;
  try {
    return new RegExp(s, "i");
  } catch (e) {
    throw new Error(`${name} is not a valid regular expression: ${(e as Error).message}`);
  }
}

export function enumArg<T extends string>(args: ToolArgs, name: string, values: readonly T[], fallback: T): T {
  const s = stringArg(args, name);
  if (s === undefined) return fallback;
  if (!values.includes(s as T)) throw new Error(`${name} must be one of ${values.join(", ")}.`);
  return s as T;
}

/** A tool's input schema: an object with these properties. */
export function schema(properties: Record<string, unknown>, required: string[] = []): Record<string, unknown> {
  return { type: "object", properties, ...(required.length ? { required } : {}) };
}

export const CAPTURE_PARAM = {
  type: "string",
  description: "An open capture's id (\"cap-1\"), or the path of a .gpucap file, which is opened on the way. Defaults to the capture opened most recently.",
};

export const PAGE_PARAMS = {
  offset: { type: "integer", minimum: 0, description: "Items to skip (default 0)." },
  limit: { type: "integer", minimum: 1, description: "Items to return." },
};

// ---------------------------------------------------------------------------------------------
// What several tools list the same way

export function findingBrief(c: Capture, f: FrameFinding): Record<string, unknown> {
  const pass = f.commandIndex !== undefined ? c.passOf(f.commandIndex) : -1;
  return {
    rule: f.rule, severity: f.severity, confidence: f.confidence, count: f.count > 1 ? f.count : undefined,
    command: f.commandIndex, pass: pass >= 0 ? pass : undefined, passLabel: pass >= 0 ? c.passName(pass) : undefined,
    message: f.message,
  };
}

export function validationBrief(c: Capture, v: ValidationMessage, maxMessage = 1500): Record<string, unknown> {
  return {
    severity: v.severity, id: v.idName ?? undefined, count: v.count > 1 ? v.count : undefined, frame: v.frame,
    command: c.commandOfValidation(v),
    objects: (v.objects ?? []).map((o) => (o.object && isHandleRef(o.object as ArgValue) ? refText(c.db, o.object as ArgValue) : `${o.class} ${o.handle}${o.name ? ` "${o.name}"` : ""}`)),
    message: clip(v.message, maxMessage),
  };
}

/** A recorded stack, innermost frame first, without the frames inside the Vulkan loader and layers. */
export function stackLines(frames: StackFrame[]): string[] {
  const lines = frames.filter((f) => !f.internal).map((f) => {
    const where = f.function ?? (f.module ? `${f.module}+0x${f.offset.toString(16)}` : f.address);
    return f.file ? `${where} (${f.file}:${f.line})` : where;
  });
  const hidden = frames.length - lines.length;
  if (hidden) lines.push(`(${hidden} frames inside the loader and layers left out)`);
  return lines;
}

/** A read-back image as list_textures and get_command list it. */
export function textureBrief(c: Capture, t: CapturedTexture): Record<string, unknown> {
  const info = t.info;
  const pass = c.passOfTexture(info);
  const sampled = info.kind === "sampled";
  return {
    texture: c.data.textures.indexOf(t), kind: sampled ? "sampled" : "attachment",
    image: refText(c.db, info.id), view: refText(c.db, info.view), format: info.format,
    size: `${info.width}x${info.height}${info.depth > 1 ? `x${info.depth}` : ""}`,
    layers: info.layers > 1 ? info.layers : undefined, mip: info.mip || undefined, mips: (info.mips ?? 1) > 1 ? info.mips : undefined,
    aspect: info.aspect, samples: (info.samples ?? 1) > 1 ? info.samples : undefined,
    attachment: sampled ? undefined : info.attachment, resolve: info.resolve || undefined,
    pass: pass >= 0 ? pass : undefined, frame: c.data.frames > 1 ? info.frame : undefined,
    bytes: t.data?.byteLength, error: info.error,
  };
}

/** One page of a list, with what the caller needs to ask for the next. */
export function page<T>(items: T[], args: ToolArgs, defaultLimit: number, maxLimit: number): { items: T[]; total: number; offset: number; nextOffset?: number } {
  const offset = intArg(args, "offset", 0, 0);
  const limit = intArg(args, "limit", defaultLimit, 1, maxLimit);
  const slice = items.slice(offset, offset + limit);
  return { items: slice, total: items.length, offset, ...(offset + slice.length < items.length ? { nextOffset: offset + slice.length } : {}) };
}
