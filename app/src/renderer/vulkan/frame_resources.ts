// What Vulkan commands read and write, for the render graph (../render_graph.ts). The walk that
// calls this is ../frame_graph.ts; the Metal counterpart is ../metal/frame_resources.ts.
//
// Three kinds of command name a resource:
//
//   Pass begins, through their attachments (vulkan/pass_info.ts decodes them). The load op says
//   whether the pass reads what was there — a graph edge — or replaces it, and the store op
//   whether the result survives the pass at all.
//
//   Draws, dispatches and ray tracing launches, through the descriptor sets bound when they ran
//   (the layer snapshots each set's contents at the bind, so the capture has the images and
//   buffers themselves, not just the set handle) and through their vertex, index and indirect
//   buffers.
//
//   Transfers, which name a source and a destination outright.
//
// A read the capture cannot resolve is counted rather than guessed at: bindings through
// descriptor buffers or a bindless heap are not in the snapshot, and the graph says so instead of
// implying the pass reads nothing.
import { decodePass, imageOfView } from "./pass_info.js";
import { BIND_INDEX_METHODS, BIND_VERTEX_METHODS, DISPATCH_METHODS, INDIRECT_METHODS, TRACE_METHODS, bindPointOf } from "./command_sets.js";
import { isObject, num, refId, str } from "./vulkan_object.js";
import type { ObjectLookup, VulkanObject } from "./vulkan_object.js";
import type { NodeKind, RawAccess, RawResource } from "../render_graph.js";
import type { ResourceSource } from "../frame_graph.js";
import type { ArgObject, ArgValue, CaptureCommand, CaptureDescriptorSets } from "../../shared/protocol.js";

/** Transfer and clear commands, and the resources each names: [reads, writes]. */
const TRANSFERS: Record<string, { read: string[]; write: string[]; nested?: string; verb: string }> = {
  vkCmdCopyBuffer: { read: ["srcBuffer"], write: ["dstBuffer"], verb: "copy" },
  vkCmdCopyBuffer2: { read: ["srcBuffer"], write: ["dstBuffer"], nested: "pCopyBufferInfo", verb: "copy" },
  vkCmdCopyBuffer2KHR: { read: ["srcBuffer"], write: ["dstBuffer"], nested: "pCopyBufferInfo", verb: "copy" },
  vkCmdCopyImage: { read: ["srcImage"], write: ["dstImage"], verb: "copy" },
  vkCmdCopyImage2: { read: ["srcImage"], write: ["dstImage"], nested: "pCopyImageInfo", verb: "copy" },
  vkCmdCopyImage2KHR: { read: ["srcImage"], write: ["dstImage"], nested: "pCopyImageInfo", verb: "copy" },
  vkCmdCopyBufferToImage: { read: ["srcBuffer"], write: ["dstImage"], verb: "copy" },
  vkCmdCopyBufferToImage2: { read: ["srcBuffer"], write: ["dstImage"], nested: "pCopyBufferToImageInfo", verb: "copy" },
  vkCmdCopyBufferToImage2KHR: { read: ["srcBuffer"], write: ["dstImage"], nested: "pCopyBufferToImageInfo", verb: "copy" },
  vkCmdCopyImageToBuffer: { read: ["srcImage"], write: ["dstBuffer"], verb: "copy" },
  vkCmdCopyImageToBuffer2: { read: ["srcImage"], write: ["dstBuffer"], nested: "pCopyImageToBufferInfo", verb: "copy" },
  vkCmdCopyImageToBuffer2KHR: { read: ["srcImage"], write: ["dstBuffer"], nested: "pCopyImageToBufferInfo", verb: "copy" },
  vkCmdBlitImage: { read: ["srcImage"], write: ["dstImage"], verb: "blit" },
  vkCmdBlitImage2: { read: ["srcImage"], write: ["dstImage"], nested: "pBlitImageInfo", verb: "blit" },
  vkCmdBlitImage2KHR: { read: ["srcImage"], write: ["dstImage"], nested: "pBlitImageInfo", verb: "blit" },
  vkCmdResolveImage: { read: ["srcImage"], write: ["dstImage"], verb: "resolve copy" },
  vkCmdResolveImage2: { read: ["srcImage"], write: ["dstImage"], nested: "pResolveImageInfo", verb: "resolve copy" },
  vkCmdResolveImage2KHR: { read: ["srcImage"], write: ["dstImage"], nested: "pResolveImageInfo", verb: "resolve copy" },
  vkCmdUpdateBuffer: { read: [], write: ["dstBuffer"], verb: "update" },
  vkCmdFillBuffer: { read: [], write: ["dstBuffer"], verb: "fill" },
  vkCmdClearColorImage: { read: [], write: ["image"], verb: "clear" },
  vkCmdClearDepthStencilImage: { read: [], write: ["image"], verb: "clear" },
  vkCmdCopyQueryPoolResults: { read: [], write: ["dstBuffer"], verb: "query results" },
};

/** Transfer verbs that replace the whole destination, so nothing before them is depended on. */
const FULL_WRITE_VERBS = new Set(["clear", "fill", "update"]);

/** Binding through memory the capture does not snapshot: every action after one reads the unknown. */
const DESCRIPTOR_BUFFER_METHODS = new Set([
  "vkCmdBindDescriptorBuffersEXT", "vkCmdSetDescriptorBufferOffsetsEXT",
  "vkCmdSetDescriptorBufferOffsets2EXT", "vkCmdBindDescriptorBufferEmbeddedSamplersEXT",
]);

/** The sets a draw or dispatch reads, per stream and bind point: stream -> bind point -> set -> contents. */
type BoundSets = Map<string, Map<string, Map<number, CaptureDescriptorSets["sets"][number]>>>;

export class VulkanResourceSource implements ResourceSource {
  private _db: ObjectLookup;
  private _bound: BoundSets = new Map();
  /** Vertex and index buffers bound per stream: stream -> "v<binding>" / "index" -> buffer id. */
  private _buffers = new Map<string, Map<string, number>>();
  /** Streams that bound a descriptor buffer, whose contents the capture cannot see. */
  private _descriptorBuffers = new Set<string>();
  private _resources = new Map<string, RawResource>();

  constructor(db: ObjectLookup) {
    this._db = db;
  }

  observe(cmd: CaptureCommand, stream: string): void {
    if (cmd.descriptors) {
      let byPoint = this._bound.get(stream);
      if (!byPoint) this._bound.set(stream, (byPoint = new Map()));
      let sets = byPoint.get(cmd.descriptors.bindPoint);
      if (!sets) byPoint.set(cmd.descriptors.bindPoint, (sets = new Map()));
      for (const s of cmd.descriptors.sets) sets.set(s.set, s);
      return;
    }
    const a = cmd.args;
    if (!a) return;
    if (DESCRIPTOR_BUFFER_METHODS.has(cmd.method)) {
      this._descriptorBuffers.add(stream);
    } else if (BIND_VERTEX_METHODS.has(cmd.method) && Array.isArray(a.pBuffers)) {
      const first = num(a.firstBinding);
      a.pBuffers.forEach((b, i) => {
        const id = refId(b);
        if (id !== null) this._streamBuffers(stream).set(`v${first + i}`, id);
      });
    } else if (BIND_INDEX_METHODS.has(cmd.method)) {
      const id = refId(a.buffer);
      if (id !== null) this._streamBuffers(stream).set("index", id);
    }
  }

  passAccesses(cmd: CaptureCommand, ordinal: number): { kind: NodeKind; label: string; accesses: RawAccess[] } | null {
    const decoded = decodePass(cmd, this._db);
    if (!decoded) return null;
    const accesses: RawAccess[] = [];
    for (const att of decoded.attachments) {
      const resource = this._imageResource(att.imageId, att.mipLevel, att.baseLayer);
      if (!resource) continue;
      // A depth attachment has two sets of ops; the pass loads if either aspect does, and stores
      // if either does, since the graph has one edge for the whole subresource.
      const loads = att.loadOp === "LOAD" || (att.kind === "depth" && att.stencilLoadOp === "LOAD");
      const stores = att.storeOp === "STORE" || (att.kind === "depth" && att.stencilStoreOp === "STORE");
      const kind = att.kind === "resolve" ? "resolve target" : `${att.kind} attachment`;
      accesses.push({
        resource, mode: "write", usage: `${kind} (${loads ? "load" : att.loadOp === "CLEAR" ? "clear" : "discard"}/${stores ? "store" : "discard"})`,
        discards: !loads, dropped: !stores,
      });
      // Dynamic rendering names the resolve target inside the attachment it resolves, so it is not
      // an entry of its own; it is written all the same.
      const resolve = this._imageResource(imageOfView(this._db, att.resolveViewId), 0, 0);
      if (resolve) accesses.push({ resource: resolve, mode: "write", usage: "resolve target (discard/store)", discards: true });
    }
    return { kind: "render", label: this._passLabel(cmd, ordinal), accesses };
  }

  actionAccesses(cmd: CaptureCommand, stream: string): { accesses: RawAccess[]; unresolved: number } {
    const accesses: RawAccess[] = [];
    let unresolved = 0;
    const sets = this._bound.get(stream)?.get(bindPointOf(cmd.method));
    if (sets) {
      for (const set of sets.values()) {
        for (const binding of set.bindings) {
          const usage = descriptorUsage(binding.type);
          if (!usage) continue;
          for (const d of binding.descriptors) {
            if (!d) continue;
            const bufferId = refId(d.buffer);
            if (bufferId !== null) {
              const resource = this._bufferResource(bufferId);
              if (resource) accesses.push({ resource, mode: usage.write ? "readwrite" : "read", usage: usage.name });
              continue;
            }
            const viewId = refId(d.imageView);
            if (viewId !== null) {
              const resource = this._viewResource(viewId);
              if (resource) accesses.push({ resource, mode: usage.write ? "readwrite" : "read", usage: usage.name });
              continue;
            }
            // A binding of a type the graph tracks whose descriptor names nothing the capture
            // recorded: a null descriptor, or one written through a path it cannot see.
            if (!d.immutable && !d.sampler && !d.bufferView) unresolved++;
          }
        }
      }
    }
    // Descriptor buffers and shader objects keep their bindings in memory the capture does not
    // snapshot, so every action in that stream reads something the graph cannot name. A draw that
    // simply binds nothing is not counted: needing no descriptors is not the same as hiding them.
    if (this._descriptorBuffers.has(stream)) unresolved++;

    if (!DISPATCH_METHODS.has(cmd.method) && !TRACE_METHODS.has(cmd.method)) {
      // Vertex and index buffers: the geometry side of the graph, which is what links a compute
      // pass that skins or culls to the draws that then read what it produced.
      // Every vertex and index buffer bound in this stream, not only the bindings this draw's
      // pipeline reads, which is not knowable here without the pipeline's vertex input state.
      // Folded per pass, so a pass depends on the geometry it had bound while it ran.
      for (const [binding, id] of this._streamBuffers(stream)) {
        const resource = this._bufferResource(id);
        if (resource) accesses.push({ resource, mode: "read", usage: binding === "index" ? "index buffer" : "vertex buffer" });
      }
    }
    if (INDIRECT_METHODS.has(cmd.method) || cmd.method.includes("Indirect")) {
      for (const key of ["buffer", "countBuffer"]) {
        const resource = this._bufferResource(refId(cmd.args?.[key]));
        if (resource) accesses.push({ resource, mode: "read", usage: "indirect buffer" });
      }
    }
    return { accesses, unresolved };
  }

  transferAccesses(cmd: CaptureCommand): { label: string; accesses: RawAccess[] } | null {
    const spec = TRANSFERS[cmd.method];
    if (!spec || !cmd.args) return null;
    // The "2" forms of every transfer moved their arguments into an info struct; both spellings
    // name the ends the same way once that is unwrapped.
    const nested = spec.nested ? cmd.args[spec.nested] : null;
    const a: ArgObject = isObject(nested) ? nested : cmd.args;
    const accesses: RawAccess[] = [];
    const names: string[] = [];
    const add = (field: string, mode: "read" | "write"): void => {
      const value = a[field];
      const id = refId(value);
      if (id === null) return;
      const isImage = field.toLowerCase().includes("image");
      const sub = isImage ? copySubresource(a, mode) : null;
      const resource = sub ? this._imageResource(id, sub.mip, sub.layer) : this._bufferResource(id);
      if (!resource) return;
      accesses.push({
        resource, mode, usage: `${spec.verb} ${mode === "read" ? "src" : "dst"}`,
        // A clear, fill or update replaces everything it touches; a copy region may not, so it is
        // reported as preserving what was there and depending on the previous writer.
        discards: mode === "write" && FULL_WRITE_VERBS.has(spec.verb),
      });
      if (mode === "write") names.push(resource.label);
    };
    for (const f of spec.read) add(f, "read");
    for (const f of spec.write) add(f, "write");
    const verb = spec.verb.charAt(0).toUpperCase() + spec.verb.slice(1);
    return { label: `${verb} → ${names.join(", ") || cmd.method}`, accesses };
  }

  computePassLabel(ordinal: number): string {
    return `Compute ${ordinal}`;
  }

  private _streamBuffers(stream: string): Map<string, number> {
    let m = this._buffers.get(stream);
    if (!m) this._buffers.set(stream, (m = new Map()));
    return m;
  }

  // ------------------------------------------------------------------------------- resources

  private _viewResource(viewId: number): RawResource | null {
    const view = this._db.getObject(viewId)?.descriptor ?? null;
    const range = isObject(view?.subresourceRange) ? view.subresourceRange : null;
    return this._imageResource(imageOfView(this._db, viewId), num(range?.baseMipLevel), num(range?.baseArrayLayer));
  }

  private _imageResource(imageId: number | null, mip: number, layer: number): RawResource | null {
    if (imageId === null) return null;
    const object = this._db.getObject(imageId);
    if (!object) return null;
    const key = `image:${imageId}:m${mip}:l${layer}`;
    const cached = this._resources.get(key);
    if (cached) return cached;
    // A swapchain image was never created by the application, so it has no create info of its own:
    // its size and format are the swapchain's (the same fallback VulkanObject.summary makes).
    const swapchain = isSwapchainImage(object) ? this._db.getObject(object.parentId)?.descriptor ?? null : null;
    const d = object.descriptor;
    const extent = isObject(d?.extent) ? d.extent : isObject(swapchain?.imageExtent) ? swapchain.imageExtent : null;
    const mips = num(d?.mipLevels) || 1;
    const layers = num(d?.arrayLayers) || num(swapchain?.imageArrayLayers) || 1;
    const width = Math.max(1, num(extent?.width) >> mip);
    const height = Math.max(1, num(extent?.height) >> mip);
    const format = str(d?.format ?? swapchain?.imageFormat).replace("VK_FORMAT_", "");
    const sub = [mips > 1 ? `mip ${mip}` : "", layers > 1 ? `layer ${layer}` : ""].filter(Boolean).join(" ");
    const resource: RawResource = {
      key, objectId: imageId, type: "image",
      label: sub ? `${object.name} ${sub}` : object.name,
      detail: [width && height ? `${width}x${height}` : "", format].filter(Boolean).join("  "),
      bytes: width * height * bytesPerPixel(format),
      presented: isSwapchainImage(object),
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
    const size = num(object.descriptor?.size);
    const resource: RawResource = {
      key, objectId: bufferId, type: "buffer", label: object.name,
      detail: size ? formatBytes(size) : "", bytes: size, presented: false,
    };
    this._resources.set(key, resource);
    return resource;
  }

  private _passLabel(cmd: CaptureCommand, ordinal: number): string {
    const a = cmd.args;
    const begin = a && isObject(a.pRenderPassBegin) ? a.pRenderPassBegin : null;
    if (begin) {
      const rp = this._db.getObject(refId(begin.renderPass));
      const fb = this._db.getObject(refId(begin.framebuffer));
      return `Pass ${ordinal}: ${rp?.label || rp?.name || "?"}${fb?.label ? ` (${fb.label})` : ""}`;
    }
    const info = a && isObject(a.pRenderingInfo) ? a.pRenderingInfo : null;
    const colors = info && Array.isArray(info.pColorAttachments) ? info.pColorAttachments.length : 0;
    return `Pass ${ordinal}: rendering, ${colors} color attachment${colors === 1 ? "" : "s"}`;
  }
}

/** What a descriptor type means for the graph, or null for the ones that name no resource. */
function descriptorUsage(type: string): { name: string; write: boolean } | null {
  if (type.includes("STORAGE_IMAGE")) return { name: "storage image", write: true };
  if (type.includes("STORAGE_BUFFER")) return { name: "storage buffer", write: true };
  if (type.includes("STORAGE_TEXEL_BUFFER")) return { name: "storage texel buffer", write: true };
  if (type.includes("SAMPLED_IMAGE") || type.includes("COMBINED_IMAGE_SAMPLER")) return { name: "sampled", write: false };
  if (type.includes("INPUT_ATTACHMENT")) return { name: "input attachment", write: false };
  if (type.includes("UNIFORM_BUFFER") || type.includes("UNIFORM_TEXEL_BUFFER")) return { name: "uniform buffer", write: false };
  return null;   // samplers, inline uniform blocks, acceleration structures
}

/**
 * The mip level and array layer a transfer touches, from the first of its regions. A copy that
 * spans several mips is reported at the first, which is enough to keep the links of a mip chain
 * distinct without walking every region.
 */
function copySubresource(a: ArgObject, mode: "read" | "write"): { mip: number; layer: number } {
  const regions = firstArray(a, ["pRegions", "pImageBlits", "pBlits", "pImageCopies", "pBufferImageCopies", "pImageResolves"]);
  const region = regions && isObject(regions[0]) ? regions[0] : null;
  if (region) {
    // A copy between two images has a subresource per end; one between a buffer and an image has
    // the single `imageSubresource`, whichever end the image is.
    const sub = region[mode === "read" ? "srcSubresource" : "dstSubresource"] ?? region.imageSubresource;
    if (isObject(sub)) return { mip: num(sub.mipLevel), layer: num(sub.baseArrayLayer) };
  }
  // vkCmdClear*Image takes VkImageSubresourceRange instead, whose base mip has another name.
  const ranges = firstArray(a, ["pRanges"]);
  const range = ranges && isObject(ranges[0]) ? ranges[0] : null;
  if (range) return { mip: num(range.baseMipLevel), layer: num(range.baseArrayLayer) };
  return { mip: 0, layer: 0 };
}

function firstArray(a: ArgObject, keys: string[]): ArgValue[] | null {
  for (const key of keys) {
    const v = a[key];
    if (Array.isArray(v) && v.length) return v;
  }
  return null;
}

/** An image the presentation engine owns: what the frame is for, so writing it is never pointless. */
function isSwapchainImage(object: VulkanObject): boolean {
  return object.cmd === "vkGetSwapchainImagesKHR";
}

/** Rough bytes per pixel from the format name, for sorting the heaviest render targets first. */
function bytesPerPixel(format: string): number {
  const bits = [...format.matchAll(/[RGBADSEX](\d+)/g)].reduce((sum, m) => sum + Number(m[1]), 0);
  if (bits) return bits / 8;
  if (format.includes("BC") || format.includes("ETC") || format.includes("ASTC")) return 1;
  return 4;
}

function formatBytes(bytes: number): string {
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}
