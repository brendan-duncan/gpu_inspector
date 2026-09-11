// Saving a live capture as a capture file (the format itself is capture_format.ts): the objects
// the capture references are collected, their SPIR-V payloads, creation stacks and the commands'
// symbols are fetched from the layer when the session does not hold them yet, and everything is
// written into one buffer. The app saves its captures this way, and so do the MCP server's live
// sessions.
import type { CaptureData } from "./capture_data.js";
import { CAPTURE_FORMAT, CAPTURE_VERSION, encodeCaptureFile, type CaptureFileBlob, type CaptureFileManifest, type CaptureFileObject, type Payload } from "./capture_format.js";
import { requestStacks, resolveSymbols, type LayerSession } from "./stack_requests.js";
import type { VulkanObject } from "./vulkan/vulkan_object.js";
import type { StackFrame } from "../shared/protocol.js";

const BLOB_TIMEOUT_MS = 15000;

/** One SPIR-V payload of an object: from the database's cache, else fetched from the layer. */
export function fetchBlob(session: LayerSession, object: VulkanObject, index: number): Promise<Uint8Array | null> {
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
function referencedObjects(session: LayerSession, data: CaptureData): VulkanObject[] {
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

export interface SerializeOptions {
  onProgress?: (text: string) => void;
  /** For a replay (vkinsp_replay): the objects, commands and contents only, without symbols and creation stacks. */
  forReplay?: boolean;
  /** Symbolizes the commands' addresses; by default with what the layer resolves. */
  resolveSymbols?: (addresses: string[]) => Promise<Map<string, StackFrame>>;
}

/** Serializes a capture (with the objects it references) into the file format. */
export async function serializeCapture(session: LayerSession & { readonly name: string }, data: CaptureData, options: SerializeOptions = {}): Promise<Uint8Array> {
  const onProgress = options.onProgress;
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
  // Stack traces: the commands' addresses symbolized, and the objects' creation stacks.
  const db = session.database;
  const addresses = new Set<string>();
  for (const c of data.commands) for (const a of c.stack ?? []) addresses.add(a);
  let symbols: Record<string, StackFrame> | undefined;
  if (addresses.size && !options.forReplay) {
    if (onProgress) onProgress("saving: symbols...");
    const resolved = await (options.resolveSymbols ?? ((a: string[]) => resolveSymbols(session, a)))([...addresses]);
    symbols = {};
    for (const [a, f] of resolved) symbols[a] = f;
  }
  let stacks: Record<string, StackFrame[]> | undefined;
  if (!options.forReplay && db.stacksAvailable !== false && (session.connected || db.stacks.size)) {
    if (onProgress) onProgress("saving: stack traces...");
    const got = await requestStacks(session, objects.map((o) => o.id));
    if (got && (db.stacksAvailable as boolean | null) !== false) {  // the answer may have said "none collected"
      stacks = {};
      for (const [id, frames] of got) if (frames.length) stacks[id] = frames;
    }
  }
  if (onProgress) onProgress("saving: writing...");

  const manifest: CaptureFileManifest = {
    format: CAPTURE_FORMAT, version: CAPTURE_VERSION, api: data.api, application: "GPU Inspector", savedAt: new Date().toISOString(),
    source: { name: session.name },
    frame: data.frame, frames: data.frames, frameTimeMs: db.frameTimeMs, submitMs: db.submitMs, refreshMs: db.refreshMs, refreshSource: db.refreshSource,
    displayRefreshMs: db.displayRefreshMs, frameBoundary: db.frameBoundary,
    objects: records,
    // Secondary command buffers are already inlined into the list; their nested copies are dropped.
    commands: data.commands.map((c) => {
      const { children: _children, ...rest } = c;
      return rest;
    }),
    textures: data.textures.map((t) => ({ info: t.info, ...(t.data ? { payload: addPayload(t.data) } : {}) })),
    buffers: [...data.buffers.values()].map((b) => ({ info: b.info, ...(b.data ? { payload: addPayload(b.data) } : {}) })),
    passTimings: [...data.passTimings.values()],
    ...(data.overdraw.length ? { overdraw: data.overdraw.map((o) => ({ info: o.info, ...(o.data ? { payload: addPayload(o.data) } : {}) })) } : {}),
    validation: db.validation,
    ...(symbols ? { symbols } : {}),
    ...(stacks ? { stacks } : {}),
  };
  return encodeCaptureFile(manifest, payloads);
}
