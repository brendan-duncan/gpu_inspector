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
import { passKey, type CaptureData, type CapturedBuffer, type CapturedTexture } from "./capture_data.js";
import type { SessionContext } from "./session_panel.js";
import type { VulkanObject } from "./vulkan/vulkan_object.js";
import type { ArgObject, ArgValue, BlobInfo, CaptureBufferInfo, CaptureCommand, CaptureTextureInfo, PassTiming, ValidationMessage } from "../shared/protocol.js";

export const CAPTURE_FILE_EXTENSION = "gpucap";
export const CAPTURE_FILE_FILTERS = [{ name: "GPU Inspector captures", extensions: [CAPTURE_FILE_EXTENSION] }, { name: "All files", extensions: ["*"] }];

const MAGIC = "GPUCAP 1\n";
const FORMAT = "gpu-inspector-capture";
const VERSION = 1;
const BLOB_TIMEOUT_MS = 15000;

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
  format: typeof FORMAT;
  version: number;
  api: "vulkan";
  application: string;
  savedAt: string;
  source: { name: string };
  frame: number;
  frames: number;
  /** The live frame interval and submit time when the capture was taken (Frame Bound card). */
  frameTimeMs: number;
  submitMs: number;
  objects: CaptureFileObject[];
  commands: CaptureCommand[];
  textures: { info: CaptureTextureInfo; payload?: Payload }[];
  buffers: { info: CaptureBufferInfo; payload?: Payload }[];
  passTimings: PassTiming[];
  /** Validation messages the session had received when the capture was saved. */
  validation?: ValidationMessage[];
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
}

/** A file name for a capture: "<application>_frame_<N>.gpucap". */
export function captureFileName(source: string, frame: number, frames: number): string {
  const base = source.replace(/\.[^.]+$/, "").replace(/[^\w.-]+/g, "_").replace(/^_+|_+$/g, "") || "capture";
  return `${base}_frame_${frame}${frames > 1 ? `-${frame + frames - 1}` : ""}.${CAPTURE_FILE_EXTENSION}`;
}

/** One SPIR-V payload of an object: from the database's cache, else fetched from the layer. */
export function fetchBlob(session: SessionContext, object: VulkanObject, index: number): Promise<Uint8Array | null> {
  const db = session.database;
  const key = `${object.id}:${index}`;
  const cached = db.blobData.get(key);
  if (cached) return Promise.resolve(cached);
  if (!session.connected) return Promise.resolve(null);
  return new Promise((resolve) => {
    let done = false;
    const finish = (data: Uint8Array | null): void => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      db.onObjectBlob.disconnect(listener);
      resolve(data);
    };
    const listener = (id: number, idx: number, data: Uint8Array | null): void => {
      if (id === object.id && idx === index) finish(data);
    };
    const timer = setTimeout(() => finish(null), BLOB_TIMEOUT_MS);
    db.onObjectBlob.addListener(listener);
    void session.send({ action: "RequestBlob", id: object.id, index }).then((ok) => {
      if (!ok) finish(null);
    });
  });
}

/**
 * The objects a capture needs: everything its commands, descriptor snapshots, render targets
 * and buffer ranges reference, closed over dependencies and owners so every link resolves.
 */
function referencedObjects(session: SessionContext, data: CaptureData): VulkanObject[] {
  const db = session.database;
  const ids = new Set<number>();
  for (const c of data.commands) {
    if (c.object) ids.add(c.object.__id);
    if (c.secondary) ids.add(c.secondary);
    db.collectReferences(c.args, ids);
    db.collectReferences(c.descriptors, ids);
  }
  for (const t of data.textures) {
    ids.add(t.info.id);
    ids.add(t.info.commandBuffer);
  }
  for (const b of data.buffers.values()) {
    ids.add(b.info.buffer);
    ids.add(b.info.commandBuffer);
  }
  for (const v of db.validation) db.collectReferences(v.objects, ids);
  const out = new Map<number, VulkanObject>();
  const queue = [...ids];
  while (queue.length) {
    const id = queue.pop()!;
    if (out.has(id)) continue;
    const o = db.getObject(id);
    if (!o) continue;
    out.set(id, o);
    if (o.parentId && !out.has(o.parentId)) queue.push(o.parentId);
    for (const dep of o.dependencies) if (!out.has(dep.id)) queue.push(dep.id);
    const more = new Set<number>();
    db.collectReferences(o.updates, more);
    for (const m of more) if (!out.has(m)) queue.push(m);
  }
  return [...out.values()].sort((a, b) => a.id - b.id);
}

/** Serializes a capture (with the objects it references) into the file format. */
export async function serializeCapture(session: SessionContext, data: CaptureData, onProgress?: (text: string) => void): Promise<Uint8Array> {
  const payloads: Uint8Array[] = [];
  let payloadBytes = 0;
  const addPayload = (bytes: Uint8Array | null | undefined): Payload | undefined => {
    if (!bytes) return undefined;
    const p: Payload = [payloadBytes, bytes.byteLength];
    payloads.push(bytes);
    payloadBytes += bytes.byteLength;
    return p;
  };

  const objects = referencedObjects(session, data);
  const records: CaptureFileObject[] = [];
  let fetched = 0;
  const withBlobs = objects.filter((o) => o.blobs.length).length;
  for (const o of objects) {
    const blobs: CaptureFileBlob[] = [];
    for (let i = 0; i < o.blobs.length; i++) {
      const b = o.blobs[i];
      if (onProgress) onProgress(`saving: shader ${++fetched} of ${withBlobs}...`);
      const bytes = await fetchBlob(session, o, i);
      blobs.push({ name: b.name, size: b.size, ...(bytes ? { payload: addPayload(bytes) } : {}) });
    }
    records.push({
      id: o.id, parent: o.parentId, type: o.type, cmd: o.cmd, index: o.index, handle: o.handle, label: o.label || null,
      args: o.args, blobs, updates: o.updates, deleted: o.isDeleted,
    });
  }
  if (onProgress) onProgress("saving: writing...");

  const db = session.database;
  const manifest: CaptureFileManifest = {
    format: FORMAT, version: VERSION, api: "vulkan", application: "GPU Inspector", savedAt: new Date().toISOString(),
    source: { name: session.name },
    frame: data.frame, frames: data.frames, frameTimeMs: db.frameTimeMs, submitMs: db.submitMs,
    objects: records,
    // Secondary command buffers are already inlined into the list; their nested copies are dropped.
    commands: data.commands.map((c) => {
      const { children: _children, ...rest } = c;
      return rest;
    }),
    textures: data.textures.map((t) => ({ info: t.info, ...(t.data ? { payload: addPayload(t.data) } : {}) })),
    buffers: [...data.buffers.values()].map((b) => ({ info: b.info, ...(b.data ? { payload: addPayload(b.data) } : {}) })),
    passTimings: [...data.passTimings.values()],
    validation: db.validation,
  };

  const json = new TextEncoder().encode(JSON.stringify(manifest));
  const magic = new TextEncoder().encode(MAGIC);
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
  if (manifest.format !== FORMAT) throw new Error("Not a GPU Inspector capture file (unknown format).");
  if (manifest.version > VERSION) throw new Error(`The capture was saved by a newer GPU Inspector (format version ${manifest.version}); this build reads version ${VERSION}.`);
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
  const commands = (manifest.commands ?? []).map((c, i) => ({ ...c, index: i }));
  return { manifest, validation: manifest.validation ?? [], objects: manifest.objects ?? [], blobs, commands, textures, buffers, passTimings };
}
