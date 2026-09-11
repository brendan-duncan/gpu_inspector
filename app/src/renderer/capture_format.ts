// Capture files: a frame capture saved to disk and reopened without the application, the
// counterpart of WebGPU Inspector's .wgpuc. One file holds everything the capture tab and the
// Inspect tab need: the objects the capture references (with their creation arguments, labels,
// updates and SPIR-V payloads), the command list, the read-back render targets and buffer
// ranges, and the pass timings.
//
// Layout: an ASCII "GPUCAP 1\n" line, a little-endian u32 with the length of the JSON manifest,
// the manifest, then the raw binary payloads (pixel data, buffer contents, SPIR-V) which the
// manifest references as [offset, length] into that area. Text editors can read the header and
// manifest, and payloads are stored as bytes rather than base64 so large captures stay compact.
//
// This is the format alone, with no session or DOM behind it, so the MCP server (src/mcp/) reads
// the files the UI writes. Saving a live capture, which fetches what the session does not hold
// yet from the layer, is capture_file.ts.
import { passKey, type CapturedBuffer, type CapturedOverdraw, type CapturedTexture } from "./capture_data.js";
import type { ArgObject, ArgValue, BlobInfo, CaptureApi, CaptureBufferInfo, CaptureCommand, CaptureTextureInfo, OverdrawMeasurement, PassTiming, StackFrame, ValidationMessage } from "../shared/protocol.js";

export const CAPTURE_FILE_EXTENSION = "gpucap";
export const CAPTURE_FILE_FILTERS = [{ name: "GPU Inspector captures", extensions: [CAPTURE_FILE_EXTENSION] }, { name: "All files", extensions: ["*"] }];

const MAGIC = "GPUCAP 1\n";
export const CAPTURE_FORMAT = "gpu-inspector-capture";
export const CAPTURE_VERSION = 1;

/** Where a payload sits in the file's binary area: [offset, length]. */
export type Payload = [number, number];

export interface CaptureFileBlob extends BlobInfo {
  payload?: Payload;
}

/** One object of the capture's object graph, as the layer reported it plus its later updates. */
export interface CaptureFileObject {
  id: number;
  parent: number;
  type: string;
  cmd: string;
  index: number;
  handle: string;
  label: string | null;
  args: ArgObject | null;
  blobs: CaptureFileBlob[];
  updates: Record<string, ArgValue>;
  /** Destroyed before the capture was saved (a ghost kept for the objects that reference it). */
  deleted: boolean;
}

export interface CaptureFileManifest {
  format: typeof CAPTURE_FORMAT;
  version: number;
  api: CaptureApi;
  application: string;
  savedAt: string;
  source: { name: string };
  frame: number;
  frames: number;
  /** The live frame interval and submit time when the capture was taken (Frame Bound card). */
  frameTimeMs: number;
  submitMs: number;
  /** Display refresh period while vsync was on (0 without), the Frame Bound budget. */
  refreshMs?: number;
  refreshSource?: string;
  /** The display's own refresh period when a source reported one (0 otherwise). */
  displayRefreshMs?: number;
  /** How the layer ended frames: "present", "wait" (vkWaitForFences) or "submit"; missing in older files. */
  frameBoundary?: string;
  objects: CaptureFileObject[];
  commands: CaptureCommand[];
  textures: { info: CaptureTextureInfo; payload?: Payload }[];
  buffers: { info: CaptureBufferInfo; payload?: Payload }[];
  passTimings: PassTiming[];
  /** Overdraw measurements with their per-pixel counts (absent when the capture did not measure overdraw). */
  overdraw?: { info: OverdrawMeasurement; payload?: Payload }[];
  /** The pixel a Metal capture followed (CapturePixelHistory's `history`), when it followed one. */
  pixelHistory?: Record<string, unknown>;
  /** Validation messages the session had received when the capture was saved. */
  validation?: ValidationMessage[];
  /** Symbolized frames of the addresses the commands' stacks carry, by address. */
  symbols?: Record<string, StackFrame>;
  /** Creation stacks of the referenced objects, by object id (absent when the layer collected none). */
  stacks?: Record<string, StackFrame[]>;
}

/** A parsed capture file, ready for CaptureData.load() and ObjectDatabase.loadObjects(). */
export interface LoadedCapture {
  manifest: CaptureFileManifest;
  validation: ValidationMessage[];
  objects: CaptureFileObject[];
  /** SPIR-V payloads keyed "objectId:blobIndex". */
  blobs: Map<string, Uint8Array>;
  commands: CaptureCommand[];
  textures: CapturedTexture[];
  buffers: Map<number, CapturedBuffer>;
  passTimings: Map<string, PassTiming>;
  overdraw: CapturedOverdraw[];
  pixelHistory: Record<string, unknown> | null;
  /** Files written before the field was real say "vulkan"; so does an absent one. */
  api: CaptureApi;
}

/** A file name for a capture: "<application>_frame_<N>.gpucap". */
export function captureFileName(source: string, frame: number, frames: number): string {
  const base = source.replace(/\.[^.]+$/, "").replace(/[^\w.-]+/g, "_").replace(/^_+|_+$/g, "") || "capture";
  return `${base}_frame_${frame}${frames > 1 ? `-${frame + frames - 1}` : ""}.${CAPTURE_FILE_EXTENSION}`;
}

/** The file's bytes: the header, the manifest, then `payloads` back to back in the order the manifest's offsets count them. */
export function encodeCaptureFile(manifest: CaptureFileManifest, payloads: Uint8Array[]): Uint8Array {
  const json = new TextEncoder().encode(JSON.stringify(manifest));
  const magic = new TextEncoder().encode(MAGIC);
  const payloadBytes = payloads.reduce((n, p) => n + p.byteLength, 0);
  const out = new Uint8Array(magic.byteLength + 4 + json.byteLength + payloadBytes);
  let pos = 0;
  out.set(magic, pos);
  pos += magic.byteLength;
  new DataView(out.buffer).setUint32(pos, json.byteLength, true);
  pos += 4;
  out.set(json, pos);
  pos += json.byteLength;
  for (const p of payloads) {
    out.set(p, pos);
    pos += p.byteLength;
  }
  return out;
}

/** Parses a capture file; throws with a readable message when it is not one. */
export function parseCaptureFile(bytes: Uint8Array): LoadedCapture {
  const magic = new TextEncoder().encode(MAGIC);
  if (bytes.byteLength < magic.byteLength + 4) throw new Error("The file is too short to be a capture.");
  for (let i = 0; i < magic.byteLength; i++) {
    if (bytes[i] !== magic[i]) throw new Error("Not a GPU Inspector capture file (bad header).");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const jsonLength = view.getUint32(magic.byteLength, true);
  const jsonStart = magic.byteLength + 4;
  const base = jsonStart + jsonLength;
  if (base > bytes.byteLength) throw new Error("The capture file is truncated.");
  let manifest: CaptureFileManifest;
  try {
    manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(jsonStart, base))) as CaptureFileManifest;
  } catch (e) {
    throw new Error(`The capture's manifest is not valid JSON: ${(e as Error).message}`);
  }
  if (manifest.format !== CAPTURE_FORMAT) throw new Error("Not a GPU Inspector capture file (unknown format).");
  if (manifest.version > CAPTURE_VERSION) throw new Error(`The capture was saved by a newer GPU Inspector (format version ${manifest.version}); this build reads version ${CAPTURE_VERSION}.`);
  const payload = (p: Payload | undefined): Uint8Array | null => {
    if (!p) return null;
    const [offset, length] = p;
    if (offset < 0 || length < 0 || base + offset + length > bytes.byteLength) throw new Error("The capture file is truncated (payload out of range).");
    return bytes.subarray(base + offset, base + offset + length);
  };

  const blobs = new Map<string, Uint8Array>();
  for (const o of manifest.objects ?? []) {
    (o.blobs ?? []).forEach((b, i) => {
      const data = payload(b.payload);
      if (data) blobs.set(`${o.id}:${i}`, data);
    });
  }
  const textures: CapturedTexture[] = (manifest.textures ?? []).map((t) => ({ info: t.info, data: payload(t.payload), canvas: null }));
  const buffers = new Map<number, CapturedBuffer>();
  for (const b of manifest.buffers ?? []) buffers.set(b.info.id, { info: b.info, data: payload(b.payload) });
  const passTimings = new Map<string, PassTiming>();
  for (const p of manifest.passTimings ?? []) passTimings.set(passKey(p.frame, p.commandBuffer, p.passIndex, p.kind === "compute"), p);
  const overdraw: CapturedOverdraw[] = (manifest.overdraw ?? []).map((o) => ({ info: o.info, data: payload(o.payload) }));
  const commands = (manifest.commands ?? []).map((c, i) => ({ ...c, index: i }));
  return { manifest, validation: manifest.validation ?? [], objects: manifest.objects ?? [], blobs, commands, textures, buffers, passTimings,
         overdraw, pixelHistory: manifest.pixelHistory ?? null, api: manifest.api ?? "vulkan" };
}
