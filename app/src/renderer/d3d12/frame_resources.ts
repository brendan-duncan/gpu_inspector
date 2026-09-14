// What D3D12 commands read and write, for the render graph (../render_graph.ts). The walk that
// calls this is ../frame_graph.ts; the Vulkan and Metal counterparts are ../vulkan/frame_resources.ts
// and ../metal/frame_resources.ts.
//
// D3D12 sits between the two. Like Vulkan it binds resources through descriptors, and the
// capture library snapshots every root descriptor table and root view at the bind in the shape
// of a Vulkan descriptor set (d3d12/README.md, "Bound buffers and textures"), so a draw's reads
// come from those snapshots. Like Metal a pass names its targets outright: OMSetRenderTargets
// carries each handle resolved to its resource and view. What D3D12 has that neither does is a
// pass with no load or store operations at all — OMSetRenderTargets keeps whatever the targets
// held, so such a pass always depends on the previous version and always stores. Only
// BeginRenderPass says DISCARD / CLEAR / PRESERVE and DISCARD / PRESERVE / RESOLVE.
//
// A root descriptor table the library could not read (a heap it did not follow, a table set in
// a bundle before its root signature) arrives with no bindings and is counted as unresolved
// rather than as reading nothing.
import { D3D12_SETS } from "./command_sets.js";
import { d3d12TextureShape, d3d12ViewSubresource, isD3D12Texture } from "./d3d12_object.js";
import { dxgiFormatBytes, dxgiFormatShort } from "./dxgi_format.js";
import { isObject, num, refId, str } from "../vulkan/vulkan_object.js";
import type { ObjectLookup, VulkanObject } from "../vulkan/vulkan_object.js";
import type { NodeKind, RawAccess, RawResource, SyncPoint } from "../render_graph.js";
import type { ResourceSource } from "../frame_graph.js";
import type { ArgObject, ArgValue, CaptureCommand, CaptureDescriptorSets } from "../../shared/protocol.js";

const SUBRESOURCE_ALL = 0xffffffff;

/** The sets a draw or dispatch reads, per stream and bind point: stream -> bind point -> root parameter -> contents. */
type BoundSets = Map<string, Map<string, Map<number, CaptureDescriptorSets["sets"][number]>>>;

export class D3D12ResourceSource implements ResourceSource {
  private _db: ObjectLookup;
  private _bound: BoundSets = new Map();
  /** Vertex and index buffers bound per stream: stream -> "v<slot>" / "index" -> buffer id. */
  private _buffers = new Map<string, Map<string, number>>();
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
    if (!cmd.args) return;
    if (D3D12_SETS.BIND_VERTEX.has(cmd.method)) {
      for (const vb of D3D12_SETS.vertexBuffersOf(cmd)) {
        const id = refId(vb.buffer);
        if (id !== null) this._streamBuffers(stream).set(`v${vb.binding}`, id);
      }
    } else if (D3D12_SETS.BIND_INDEX.has(cmd.method)) {
      const id = refId(D3D12_SETS.indexBufferOf(cmd)?.buffer);
      if (id !== null) this._streamBuffers(stream).set("index", id);
    } else if (cmd.method === "SetGraphicsRootSignature" || cmd.method === "SetComputeRootSignature") {
      // A new root signature drops every root parameter bound under the old one.
      this._bound.get(stream)?.delete(cmd.method.startsWith("SetCompute") ? "compute" : "graphics");
    }
  }

  passAccesses(cmd: CaptureCommand, ordinal: number): { kind: NodeKind; label: string; accesses: RawAccess[] } | null {
    const a = cmd.args;
    if (!a) return null;
    const accesses: RawAccess[] = [];
    const targets: string[] = [];
    if (cmd.method === "BeginRenderPass") {
      const colors = Array.isArray(a.pRenderTargets) ? a.pRenderTargets.filter(isObject) : [];
      colors.forEach((rt) => this._renderPassTarget(rt, "color", accesses, targets));
      if (isObject(a.pDepthStencil)) this._renderPassTarget(a.pDepthStencil, "depth", accesses, targets);
    } else {
      // OMSetRenderTargets: no load or store operation, so the pass keeps what was there and
      // leaves its result in memory (discards: false, dropped: false).
      const colors = Array.isArray(a.pRenderTargetDescriptors) ? a.pRenderTargetDescriptors : [];
      for (const h of colors) this._target(h, "color attachment (load/store)", accesses, targets, false, false, "color");
      this._target(a.pDepthStencilDescriptor, "depth attachment (load/store)", accesses, targets, false, false, "depth");
    }
    return { kind: "render", label: `Pass ${ordinal}${targets.length ? `: ${targets.join(", ")}` : ""}`, accesses };
  }

  actionAccesses(cmd: CaptureCommand, stream: string): { accesses: RawAccess[]; unresolved: number } {
    const accesses: RawAccess[] = [];
    let unresolved = 0;
    const sets = this._bound.get(stream)?.get(D3D12_SETS.bindPointOf(cmd.method));
    if (sets) {
      for (const set of sets.values()) {
        // A table the library could not read: it binds something, and the graph cannot say what.
        if (!set.bindings.length && set.descriptorSet) unresolved++;
        for (const binding of set.bindings) {
          const usage = descriptorUsage(binding.type);
          if (!usage) continue;
          for (const d of binding.descriptors) {
            if (!d) continue;   // a slot of the range the application never wrote: nothing bound there
            const bufferId = refId(d.buffer);
            if (bufferId !== null) {
              const resource = this._bufferResource(bufferId);
              if (resource) accesses.push({ resource, mode: usage.write ? "readwrite" : "read", usage: usage.name });
              continue;
            }
            const textureId = refId(d.resource);
            if (textureId !== null) {
              const sub = d3d12ViewSubresource(d.view);
              const resource = this._imageResource(textureId, sub.mip, sub.slice);
              if (resource) accesses.push({ resource, mode: usage.write ? "readwrite" : "read", usage: usage.name });
            }
          }
        }
      }
    }
    if (!D3D12_SETS.DISPATCH.has(cmd.method) && !D3D12_SETS.TRACE.has(cmd.method)) {
      // Every vertex and index buffer bound in the stream, as the Vulkan source keeps them: which
      // slots the pipeline reads is not knowable here.
      for (const [binding, id] of this._streamBuffers(stream)) {
        const resource = this._bufferResource(id);
        if (resource) accesses.push({ resource, mode: "read", usage: binding === "index" ? "index buffer" : "vertex buffer" });
      }
    }
    if (cmd.method === "ExecuteIndirect") {
      for (const key of ["pArgumentBuffer", "pCountBuffer"]) {
        const resource = this._bufferResource(refId(cmd.args?.[key]));
        if (resource) accesses.push({ resource, mode: "read", usage: "indirect buffer" });
      }
    }
    return { accesses, unresolved };
  }

  transferAccesses(cmd: CaptureCommand): { label: string; accesses: RawAccess[] } | null {
    const a = cmd.args;
    if (!a) return null;
    const accesses: RawAccess[] = [];
    const names: string[] = [];
    const add = (id: number | null, mode: "read" | "write", verb: string, sub: { mip: number; slice: number } | null, discards = false, dropped = false): void => {
      if (id === null) return;
      const object = this._db.getObject(id);
      const resource = object && isD3D12Texture(object) ? this._imageResource(id, sub?.mip ?? 0, sub?.slice ?? 0) : this._bufferResource(id);
      if (!resource) return;
      accesses.push({ resource, mode, usage: `${verb} ${mode === "read" ? "src" : "dst"}`, discards: mode === "write" && discards, dropped });
      if (mode === "write") names.push(resource.label);
    };
    const full = (): boolean => num(a.NumRects) === 0 && !(Array.isArray(a.pRects) && a.pRects.length);
    let verb = "";
    switch (cmd.method) {
      case "CopyResource":
        verb = "copy";
        add(refId(a.pSrcResource), "read", verb, null);
        add(refId(a.pDstResource), "write", verb, null, true);   // replaces the whole resource
        break;
      case "CopyBufferRegion":
      case "AtomicCopyBufferUINT":
      case "AtomicCopyBufferUINT64":
        verb = "copy";
        add(refId(a.pSrcBuffer), "read", verb, null);
        add(refId(a.pDstBuffer), "write", verb, null);
        break;
      case "CopyTextureRegion": {
        verb = "copy";
        const src = isObject(a.pSrc) ? a.pSrc : null;
        const dst = isObject(a.pDst) ? a.pDst : null;
        if (src) add(refId(src.pResource), "read", verb, this._subresource(refId(src.pResource), src.SubresourceIndex));
        if (dst) add(refId(dst.pResource), "write", verb, this._subresource(refId(dst.pResource), dst.SubresourceIndex));
        break;
      }
      case "ResolveSubresource":
      case "ResolveSubresourceRegion":
        verb = "resolve copy";
        add(refId(a.pSrcResource), "read", verb, this._subresource(refId(a.pSrcResource), a.SrcSubresource));
        add(refId(a.pDstResource), "write", verb, this._subresource(refId(a.pDstResource), a.DstSubresource), cmd.method === "ResolveSubresource");
        break;
      case "ClearRenderTargetView":
      case "ClearDepthStencilView": {
        verb = "clear";
        const h = a[cmd.method === "ClearRenderTargetView" ? "RenderTargetView" : "DepthStencilView"];
        if (isObject(h)) add(refId(h.resource), "write", verb, d3d12ViewSubresource(h.view), full());
        break;
      }
      case "ClearUnorderedAccessViewUint":
      case "ClearUnorderedAccessViewFloat": {
        verb = "clear";
        const h = isObject(a.ViewCPUHandle) ? a.ViewCPUHandle : null;
        add(refId(a.pResource), "write", verb, h ? d3d12ViewSubresource(h.view) : null, full());
        break;
      }
      case "DiscardResource":
        // Nothing is written, but what was there is gone: a discarding write whose result is dropped.
        verb = "discard";
        add(refId(a.pResource), "write", verb, null, true, true);
        break;
      case "CopyTiles": {
        verb = "copy tiles";
        const toTiled = str(a.Flags).includes("LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE");
        add(refId(toTiled ? a.pBuffer : a.pTiledResource), "read", verb, null);
        add(refId(toTiled ? a.pTiledResource : a.pBuffer), "write", verb, null);
        break;
      }
      default:
        return null;
    }
    if (!accesses.length) return null;
    const label = verb.charAt(0).toUpperCase() + verb.slice(1);
    return { label: `${label} → ${names.join(", ") || cmd.method}`, accesses };
  }

  computePassLabel(ordinal: number): string {
    return `Compute ${ordinal}`;
  }

  /**
   * The subresources a ResourceBarrier or an enhanced Barrier names. An aliasing barrier is
   * structural (required by the API whatever the data does); a transition names the resource
   * whose state changes, a UAV barrier the resource it orders (or every UAV, when it names none).
   */
  syncPoint(cmd: CaptureCommand): Omit<SyncPoint, "after"> | null {
    const a = cmd.args;
    if (!a || (cmd.method !== "ResourceBarrier" && cmd.method !== "Barrier")) return null;
    const resources: string[] = [];
    let structural = false;
    if (cmd.method === "ResourceBarrier") {
      for (const b of arrayOf(a.pBarriers)) {
        const type = str(b.Type);
        if (type.endsWith("_ALIASING")) {
          structural = true;
          continue;
        }
        const detail = isObject(b.Transition) ? b.Transition : isObject(b.UAV) ? b.UAV : null;
        // A state transition is D3D12's layout change: required whether or not any data depends
        // on it, so only a UAV barrier (and a transition to the state already held) is questioned.
        if (isObject(b.Transition) && str(b.Transition.StateBefore) !== str(b.Transition.StateAfter)) structural = true;
        const id = refId(detail?.pResource);
        if (id === null) continue;
        const sub = this._subresource(id, detail?.Subresource);
        const object = this._db.getObject(id);
        const resource = object && isD3D12Texture(object) ? this._imageResource(id, sub.mip, sub.slice) : this._bufferResource(id);
        if (resource) resources.push(resource.key);
      }
    } else {
      for (const g of arrayOf(a.pBarrierGroups)) {
        for (const b of arrayOf(g.pTextureBarriers)) {
          if (str(b.LayoutBefore) !== str(b.LayoutAfter)) structural = true;
          const id = refId(b.pResource);
          const range = isObject(b.Subresources) ? b.Subresources : null;
          const resource = id === null ? null : this._imageResource(id, num(range?.IndexOrFirstMipLevel) === SUBRESOURCE_ALL ? 0 : num(range?.IndexOrFirstMipLevel), num(range?.FirstArraySlice));
          if (resource) resources.push(resource.key);
        }
        for (const b of arrayOf(g.pBufferBarriers)) {
          const resource = this._bufferResource(refId(b.pResource));
          if (resource) resources.push(resource.key);
        }
      }
    }
    return { commandIndex: cmd.index, method: cmd.method, resources, structural };
  }

  // ------------------------------------------------------------------------------- targets

  /** One BeginRenderPass target: its handle, beginning access and ending access. */
  private _renderPassTarget(rt: ArgObject, kind: "color" | "depth", accesses: RawAccess[], targets: string[]): void {
    const handle = isObject(rt.cpuDescriptor) ? rt.cpuDescriptor : rt;
    const beginning = kind === "color" ? rt.BeginningAccess : rt.DepthBeginningAccess;
    const ending = kind === "color" ? rt.EndingAccess : rt.DepthEndingAccess;
    const begin = accessType(beginning);
    const end = accessType(ending);
    // A depth target with a stencil aspect: the pass loads if either does, stores if either does.
    const stencilBegin = kind === "depth" ? accessType(rt.StencilBeginningAccess) : "";
    const stencilEnd = kind === "depth" ? accessType(rt.StencilEndingAccess) : "";
    const loads = begin === "PRESERVE" || stencilBegin === "PRESERVE";
    const stores = end === "PRESERVE" || stencilEnd === "PRESERVE";
    const resolved = end === "RESOLVE" || stencilEnd === "RESOLVE";
    const usage = `${kind} attachment (${loads ? "load" : begin === "CLEAR" ? "clear" : "discard"}/${stores ? "store" : resolved ? "resolve" : "discard"})`;
    this._target(handle, usage, accesses, targets, !loads, !stores && !resolved, kind, resolved);
    // The resolve destination is written as well as (or instead of) the target itself.
    const resolve = isObject(ending) && isObject(ending.Resolve) ? ending.Resolve : null;
    const resolveId = resolved && resolve ? refId(resolve.pDstResource) : null;
    if (resolveId !== null) {
      const params = Array.isArray(resolve!.pSubresourceParameters) && isObject(resolve!.pSubresourceParameters[0]) ? resolve!.pSubresourceParameters[0] : null;
      const sub = this._subresource(resolveId, params?.DstSubresource);
      const target = this._imageResource(resolveId, sub.mip, sub.slice);
      if (target) accesses.push({ resource: target, mode: "write", usage: "resolve target (discard/store)", discards: true });
    }
  }

  private _target(handle: ArgValue | undefined, usage: string, accesses: RawAccess[], targets: string[], discards: boolean, dropped: boolean, kind: string, resolved = false): void {
    if (!isObject(handle)) return;
    const id = refId(handle.resource);
    if (id === null) return;
    const sub = d3d12ViewSubresource(handle.view);
    const resource = this._imageResource(id, sub.mip, sub.slice);
    if (!resource) return;
    accesses.push({ resource, mode: "write", usage, discards, dropped, resolved });
    if (kind === "color") targets.push(resource.label);
  }

  // ------------------------------------------------------------------------------- resources

  private _streamBuffers(stream: string): Map<string, number> {
    let m = this._buffers.get(stream);
    if (!m) this._buffers.set(stream, (m = new Map()));
    return m;
  }

  /** A subresource index as a mip and slice (D3D12 numbers them mip-fastest: index = mip + slice * mips). */
  private _subresource(id: number | null, index: ArgValue | undefined): { mip: number; slice: number } {
    const i = num(index);
    if (id === null || i === SUBRESOURCE_ALL || i === 0) return { mip: 0, slice: 0 };
    const shape = d3d12TextureShape(this._db.getObject(id), this._db);
    const mips = Math.max(1, shape?.mips ?? 1);
    return { mip: i % mips, slice: Math.floor(i / mips) % Math.max(1, shape?.layers ?? 1) };
  }

  private _imageResource(imageId: number | null, mip: number, slice: number): RawResource | null {
    if (imageId === null) return null;
    const object = this._db.getObject(imageId);
    if (!object) return null;
    const key = `image:${imageId}:m${mip}:l${slice}`;
    const cached = this._resources.get(key);
    if (cached) return cached;
    const shape = d3d12TextureShape(object, this._db);
    const width = Math.max(1, (shape?.width ?? 0) >> mip);
    const height = Math.max(1, (shape?.height ?? 0) >> mip);
    const format = dxgiFormatShort(shape?.format);
    const sub = [(shape?.mips ?? 1) > 1 ? `mip ${mip}` : "", (shape?.layers ?? 1) > 1 ? `slice ${slice}` : ""].filter(Boolean).join(" ");
    const resource: RawResource = {
      key, objectId: imageId, type: "image",
      label: sub ? `${object.name} ${sub}` : object.name,
      detail: [shape ? `${width}x${height}` : "", format].filter(Boolean).join("  "),
      bytes: width * height * (dxgiFormatBytes(shape?.format) || 4),
      presented: isBackBuffer(object),
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
    const size = num(object.descriptor?.Width);
    const resource: RawResource = {
      key, objectId: bufferId, type: "buffer", label: object.name,
      detail: size ? formatBytes(size) : "", bytes: size, presented: false,
    };
    this._resources.set(key, resource);
    return resource;
  }
}

/** What a range or root parameter type means for the graph, or null for the ones that name no resource. */
function descriptorUsage(type: string): { name: string; write: boolean } | null {
  if (type.endsWith("_UAV")) return { name: "unordered access", write: true };
  if (type.endsWith("_SRV")) return { name: "shader resource", write: false };
  if (type.endsWith("_CBV")) return { name: "constant buffer", write: false };
  return null;   // samplers
}

/** "D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR" -> "CLEAR" (the Type of a beginning or ending access). */
function accessType(v: ArgValue | undefined): string {
  const type = isObject(v) ? str(v.Type) : str(v);
  return type.replace(/^D3D12_RENDER_PASS_(BEGINNING|ENDING)_ACCESS_TYPE_/, "");
}

function arrayOf(v: ArgValue | undefined): ArgObject[] {
  return Array.isArray(v) ? v.filter(isObject) : [];
}

/** A swap chain's back buffer: what the frame is for, so writing it is never pointless. */
function isBackBuffer(object: VulkanObject): boolean {
  return object.cmd === "GetBuffer";
}

function formatBytes(bytes: number): string {
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}
