// What Metal commands read and write, for the render graph (../render_graph.ts). The walk that
// calls this is ../frame_graph.ts; the Vulkan counterpart is ../vulkan/frame_resources.ts.
//
// Metal makes two parts of this easier than Vulkan and one part harder. Easier: a render pass
// descriptor names its textures, levels and slices outright, with no render pass object,
// framebuffer and subpass list to reassemble; and an encoder *is* a pass, so a blit encoder's
// copies group themselves. Harder: there are no descriptor sets, so what a draw reads is whatever
// setVertexTexture:/setFragmentBuffer:/... left bound on the encoder, tracked per encoder here.
//
// Argument buffers are Metal's bindless path: the buffer is bound, but which resources it points
// at is in its bytes, not in the command stream. Those reads are counted as unresolved rather
// than guessed at, the same as Vulkan's descriptor buffers.
import { METAL_SETS } from "./command_sets.js";
import { isObject, num, refId, str } from "../vulkan/vulkan_object.js";
import type { ObjectLookup, VulkanObject } from "../vulkan/vulkan_object.js";
import type { NodeKind, RawAccess, RawResource } from "../render_graph.js";
import type { ResourceSource } from "../frame_graph.js";
import type { ArgObject, CaptureCommand } from "../../shared/protocol.js";

/** Encoder-creating selectors and the kind of node the pass becomes. */
const PASS_KINDS: Record<string, NodeKind> = {
  "renderCommandEncoderWithDescriptor:": "render",
  "parallelRenderCommandEncoderWithDescriptor:": "render",
  computeCommandEncoder: "compute",
  "computeCommandEncoderWithDescriptor:": "compute",
  "computeCommandEncoderWithDispatchType:": "compute",
  blitCommandEncoder: "transfer",
  "blitCommandEncoderWithDescriptor:": "transfer",
  resourceStateCommandEncoder: "transfer",
  "resourceStateCommandEncoderWithDescriptor:": "transfer",
  accelerationStructureCommandEncoder: "compute",
  "accelerationStructureCommandEncoderWithDescriptor:": "compute",
};

/**
 * Blit selectors and the ends they name. Metal spells the ends consistently — `sourceTexture` /
 * `destinationBuffer` and so on — so one entry per selector is enough, with the subresource read
 * from `sourceLevel` / `destinationSlice` when the selector carries them.
 */
const BLITS: Record<string, { verb: string; read?: string; write?: string; full?: boolean }> = {
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:":
    { verb: "copy", read: "sourceTexture", write: "destinationTexture" },
  "copyFromTexture:toTexture:": { verb: "copy", read: "sourceTexture", write: "destinationTexture", full: true },
  "copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:sliceCount:levelCount:":
    { verb: "copy", read: "sourceTexture", write: "destinationTexture", full: true },
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:":
    { verb: "copy", read: "sourceTexture", write: "destinationBuffer" },
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:":
    { verb: "copy", read: "sourceTexture", write: "destinationBuffer" },
  "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:":
    { verb: "copy", read: "sourceBuffer", write: "destinationTexture" },
  "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:options:":
    { verb: "copy", read: "sourceBuffer", write: "destinationTexture" },
  "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:": { verb: "copy", read: "sourceBuffer", write: "destinationBuffer" },
  "generateMipmapsForTexture:": { verb: "generate mipmaps", read: "texture", write: "texture", full: true },
  "fillBuffer:range:value:": { verb: "fill", write: "buffer", full: true },
};

/** Selectors that bind one texture to a stage, and the stage each names. */
const TEXTURE_BINDS: Record<string, string> = {
  "setVertexTexture:atIndex:": "vertex",
  "setFragmentTexture:atIndex:": "fragment",
  "setTexture:atIndex:": "compute",
  "setObjectTexture:atIndex:": "object",
  "setMeshTexture:atIndex:": "mesh",
  "setTileTexture:atIndex:": "tile",
};

/** The plural forms, which bind an array of textures over a range of indices. */
const TEXTURE_BINDS_MANY: Record<string, string> = {
  "setVertexTextures:withRange:": "vertex",
  "setFragmentTextures:withRange:": "fragment",
  "setTextures:withRange:": "compute",
  "setObjectTextures:withRange:": "object",
  "setMeshTextures:withRange:": "mesh",
  "setTileTextures:withRange:": "tile",
};

/** Binding a resource the encoder will reach through an argument buffer, whose contents are opaque here. */
const ARGUMENT_BUFFER_METHODS = new Set([
  "useResource:usage:", "useResource:usage:stages:", "useResources:count:usage:", "useResources:count:usage:stages:",
  "useHeap:", "useHeaps:count:", "useHeap:stages:", "useHeaps:count:stages:",
]);

/** What one encoder has bound. Encoders are the pass, so the state dies with them. */
interface EncoderState {
  textures: Map<string, number>;
  buffers: Map<string, number>;
  /** Resources reached through an argument buffer or heap: named, but not which parts are read. */
  opaque: number;
}

export class MetalResourceSource implements ResourceSource {
  private _db: ObjectLookup;
  /** Bound state per encoder; a command's `encoder` says which one it belongs to. */
  private _encoders = new Map<number, EncoderState>();
  private _resources = new Map<string, RawResource>();

  constructor(db: ObjectLookup) {
    this._db = db;
  }

  observe(cmd: CaptureCommand, _stream: string): void {
    const a = cmd.args;
    if (!a) return;
    const state = this._state(cmd);
    const single = TEXTURE_BINDS[cmd.method];
    if (single) {
      const id = refId(a.texture);
      if (id !== null) state.textures.set(`${single}:${num(a.index)}`, id);
      return;
    }
    const many = TEXTURE_BINDS_MANY[cmd.method];
    if (many && Array.isArray(a.textures)) {
      const first = isObject(a.range) ? num(a.range.location) : 0;
      a.textures.forEach((t, i) => {
        const id = refId(t);
        if (id !== null) state.textures.set(`${many}:${first + i}`, id);
      });
      return;
    }
    if (ARGUMENT_BUFFER_METHODS.has(cmd.method)) {
      state.opaque++;
      return;
    }
    if (METAL_SETS.BIND_STAGE_BUFFER?.has(cmd.method) && METAL_SETS.stageBuffersOf) {
      for (const b of METAL_SETS.stageBuffersOf(cmd)) {
        const id = refId(b.buffer);
        if (id !== null) state.buffers.set(`${b.stage}:${b.index}`, id);
      }
    }
  }

  passAccesses(cmd: CaptureCommand, ordinal: number): { kind: NodeKind; label: string; accesses: RawAccess[] } | null {
    const kind = PASS_KINDS[cmd.method] ?? "render";
    const a = cmd.args;
    const accesses: RawAccess[] = [];
    const targets: string[] = [];
    if (kind === "render" && a) {
      const colors = Array.isArray(a.colorAttachments) ? a.colorAttachments.filter(isObject) : [];
      for (const c of colors) this._attachment(c, "color", accesses, targets);
      for (const key of ["depthAttachment", "stencilAttachment"]) {
        const d = a[key];
        if (isObject(d)) this._attachment(d, key === "depthAttachment" ? "depth" : "stencil", accesses, targets);
      }
    }
    const encoder = this._db.getObject(cmd.encoder?.__id ?? null);
    const name = encoder?.label || targets.join(", ");
    const what = kind === "render" ? "Pass" : kind === "compute" ? "Compute" : "Blit";
    return { kind, label: `${what} ${ordinal}${name ? `: ${name}` : ""}`, accesses };
  }

  actionAccesses(cmd: CaptureCommand, _stream: string): { accesses: RawAccess[]; unresolved: number } {
    const state = this._state(cmd);
    const accesses: RawAccess[] = [];
    for (const [key, id] of state.textures) {
      const resource = this._textureResource(id, 0, 0);
      if (resource) accesses.push({ resource, mode: "read", usage: `${key.split(":")[0]} texture` });
    }
    for (const [key, id] of state.buffers) {
      const resource = this._bufferResource(id);
      const stage = key.split(":")[0];
      // A buffer bound to a stage is a uniform or a storage buffer depending only on how the
      // shader declares it, which is not in the command stream; both are reported as read, the
      // conservative direction for an edge (a false write would invent a dependency).
      if (resource) accesses.push({ resource, mode: "read", usage: `${stage} buffer` });
    }
    // The index buffer of an indexed draw is named on the draw itself, not bound beforehand.
    const index = METAL_SETS.indexBufferOf(cmd);
    if (index) {
      const resource = this._bufferResource(refId(index.buffer));
      if (resource) accesses.push({ resource, mode: "read", usage: "index buffer" });
    }
    for (const key of ["indirectBuffer", "patchIndexBuffer", "controlPointIndexBuffer"]) {
      const resource = this._bufferResource(refId(cmd.args?.[key]));
      if (resource) accesses.push({ resource, mode: "read", usage: "indirect buffer" });
    }
    return { accesses, unresolved: state.opaque };
  }

  transferAccesses(cmd: CaptureCommand): { label: string; accesses: RawAccess[] } | null {
    const spec = BLITS[cmd.method];
    const a = cmd.args;
    if (!spec || !a) return null;
    const accesses: RawAccess[] = [];
    const names: string[] = [];
    const add = (field: string, mode: "read" | "write"): void => {
      const id = refId(a[field]);
      if (id === null) return;
      const isTexture = field.toLowerCase().includes("texture");
      const level = num(a[mode === "read" ? "sourceLevel" : "destinationLevel"]);
      const slice = num(a[mode === "read" ? "sourceSlice" : "destinationSlice"]);
      const resource = isTexture ? this._textureResource(id, level, slice) : this._bufferResource(id);
      if (!resource) return;
      accesses.push({
        resource, mode, usage: `${spec.verb} ${mode === "read" ? "src" : "dst"}`,
        discards: mode === "write" && !!spec.full,
      });
      if (mode === "write") names.push(resource.label);
    };
    // generateMipmapsForTexture: names one texture as both ends, so read before write.
    if (spec.read) add(spec.read, "read");
    if (spec.write) add(spec.write, "write");
    const verb = spec.verb.charAt(0).toUpperCase() + spec.verb.slice(1);
    return { label: `${verb} → ${names.join(", ") || cmd.method}`, accesses };
  }

  computePassLabel(ordinal: number): string {
    return `Compute ${ordinal}`;
  }

  // ------------------------------------------------------------------------------- resources

  private _state(cmd: CaptureCommand): EncoderState {
    const id = cmd.encoder?.__id ?? 0;
    let state = this._encoders.get(id);
    if (!state) this._encoders.set(id, (state = { textures: new Map(), buffers: new Map(), opaque: 0 }));
    return state;
  }

  private _attachment(att: ArgObject, kind: string, accesses: RawAccess[], targets: string[]): void {
    const id = refId(att.texture);
    if (id === null) return;
    const resource = this._textureResource(id, num(att.level), num(att.slice));
    if (!resource) return;
    const load = str(att.loadAction).replace("MTLLoadAction", "");
    const store = str(att.storeAction).replace("MTLStoreAction", "");
    const stores = store === "Store" || store === "StoreAndMultisampleResolve" || store === "CustomSampleDepthStore";
    accesses.push({
      resource, mode: "write", usage: `${kind} attachment (${load.toLowerCase() || "unknown"}/${store.toLowerCase() || "unknown"})`,
      discards: load !== "Load", dropped: !stores && store !== "MultisampleResolve",
    });
    if (kind === "color") targets.push(resource.label);
    // A store action that resolves writes the resolve texture as well as (or instead of) the
    // attachment, and that is the one a later pass usually samples.
    const resolveId = refId(att.resolveTexture);
    if (resolveId !== null && store.includes("MultisampleResolve")) {
      const target = this._textureResource(resolveId, num(att.resolveLevel), num(att.resolveSlice));
      if (target) accesses.push({ resource: target, mode: "write", usage: "resolve target (discard/store)", discards: true });
    }
  }

  private _textureResource(textureId: number | null, level: number, slice: number): RawResource | null {
    if (textureId === null) return null;
    const object = this._db.getObject(textureId);
    if (!object) return null;
    const key = `image:${textureId}:m${level}:l${slice}`;
    const cached = this._resources.get(key);
    if (cached) return cached;
    // Metal objects carry their creation arguments flat, with no create-info struct around them.
    const d = object.args ?? {};
    const mips = num(d.mipmapLevelCount) || 1;
    const layers = num(d.arrayLength) || 1;
    const width = Math.max(1, num(d.width) >> level);
    const height = Math.max(1, num(d.height) >> level);
    const format = str(d.pixelFormat).replace("MTLPixelFormat", "");
    const sub = [mips > 1 ? `level ${level}` : "", layers > 1 ? `slice ${slice}` : ""].filter(Boolean).join(" ");
    const resource: RawResource = {
      key, objectId: textureId, type: "image",
      label: sub ? `${object.name} ${sub}` : object.name,
      detail: [width && height ? `${width}x${height}` : "", format].filter(Boolean).join("  "),
      bytes: width * height * bytesPerPixel(format),
      presented: isDrawableTexture(object),
    };
    this._resources.set(key, resource);
    return resource;
  }

  private _bufferResource(bufferId: number | null): RawResource | null {
    if (bufferId === null) return null;
    const object = this._db.getObject(bufferId);
    if (!object) return null;
    const key = `buffer:${bufferId}`;
    const cached = this._resources.get(key);
    if (cached) return cached;
    const size = num(object.args?.length);
    const resource: RawResource = {
      key, objectId: bufferId, type: "buffer", label: object.name,
      detail: size ? formatBytes(size) : "", bytes: size, presented: false,
    };
    this._resources.set(key, resource);
    return resource;
  }
}

/** A texture that came from a drawable: what the frame is for, so writing it is never pointless. */
function isDrawableTexture(object: VulkanObject): boolean {
  return object.cmd.includes("nextDrawable");
}

/** Rough bytes per pixel from the format name, for sorting the heaviest render targets first. */
function bytesPerPixel(format: string): number {
  const bits = [...format.matchAll(/[RGBADS](\d+)/g)].reduce((sum, m) => sum + Number(m[1]), 0);
  if (bits) return bits / 8;
  if (/BC|ETC|ASTC|EAC|PVRTC/.test(format)) return 1;
  return 4;
}

function formatBytes(bytes: number): string {
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}
