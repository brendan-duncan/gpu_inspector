// How a captured frame would fare on a tile-based GPU (the mobile GPUs: Arm Mali, Qualcomm Adreno,
// Apple, Imagination PowerVR), from its render graph (render_graph.ts), for every API the graph is
// built for.
//
// A tiled GPU renders a pass one screen tile at a time in a small on-chip memory, so what a pass
// costs beyond its shading is what crosses between that tile memory and DRAM: an attachment loaded
// into the tile when the pass starts, stored out of it when the pass ends, and whatever its shaders
// read or write in memory meanwhile. The same frame on a desktop GPU pays little for any of it. So
// the report here is mostly bytes: what each render pass loads and stores, which of that the frame
// could avoid, and what forces work out of the tile altogether.
//
//   Attachment traffic   per render pass, the bytes each attachment loads (its contents are kept,
//                        not cleared or discarded) and stores (not thrown away), at the size the
//                        graph gives the resource. An estimate: a GPU that compresses framebuffers
//                        (AFBC, UBWC) moves less, and one that skips unchanged tiles less again.
//   Avoidable            of that, what the frame's own structure does not need: a store nothing
//                        reads; a target stored and loaded straight back by the next pass to the
//                        same targets (one pass would keep it in the tile); an image only the next
//                        pass reads (a subpass, framebuffer fetch or a transient target would).
//   Post-processing      render passes that read, as a texture, what an earlier pass rendered at
//                        their own size: whether that read is at the pixel's own position (it
//                        could come from tile memory) or filters neighbours (it has to be memory).
//   Out of the tile      a draw writing a buffer or image through a shader (storage, UAV), a pass
//                        sampling the attachment it renders to, a compute pass splitting two
//                        render passes over the same data, and transfers between passes: each one
//                        makes the GPU flush the tile or go around it.
//   Subpasses            Vulkan: render passes recorded with more than one subpass, and how many
//                        attachments they pass on as input attachments.
import { usageClass } from "./render_graph.js";
import { FrameAnalysis, type FrameAnalysisDatabase } from "./vulkan/frame_analysis.js";
import { isObject, num, refId } from "./vulkan/vulkan_object.js";
import type { GraphNode, GraphUse, RenderGraph } from "./render_graph.js";
import type { CaptureData } from "./capture_data.js";

/** What a render pass's API records about subpasses, where it has them (Vulkan). */
export interface SubpassInfo {
  subpasses: number;
  /** Input attachment references across the pass's subpasses. */
  inputAttachments: number;
}

export interface TileOptions {
  /** Whether a pass's shaders filter an image they read (several reads or a loop); null when unknown. */
  filtersInput?: (node: GraphNode, imageId: number) => boolean | null;
  /** The render pass's subpasses, for an API that has them; null for a pass without that information. */
  subpasses?: (node: GraphNode) => SubpassInfo | null;
}

export interface TileAttachment {
  label: string;
  objectId: number;
  bytes: number;
  loads: boolean;
  stores: boolean;
  /** Why the load or the store is avoidable, when it is. */
  avoidable: string | null;
  avoidableBytes: number;
  /**
   * A color result stored that nothing in the capture reads: the frame's output on its way to
   * something the capture does not see (an XR compositor, the next frame, the host), or a store to
   * drop. Not counted as avoidable, since the report cannot tell which.
   */
  unreadBytes: number;
  /** Something worth knowing about the attachment that is not counted as avoidable (why not, or what it might be). */
  note: string | null;
}

export interface TilePass {
  node: GraphNode;
  attachments: TileAttachment[];
  loadBytes: number;
  storeBytes: number;
  avoidableBytes: number;
  unreadBytes: number;
  subpasses: SubpassInfo | null;
}

export type PostKind = "same-pixel" | "filters" | "unknown";

export interface PostProcessStep {
  node: GraphNode;
  /** The image it reads that an earlier pass rendered at its size. */
  input: string;
  producer: GraphNode;
  /** The producer is the pass immediately before it: the case a subpass or one pass could take. */
  adjacent: boolean;
  kind: PostKind;
}

export interface OutOfTile {
  node: GraphNode;
  kind: "shader-write" | "feedback" | "compute-split" | "transfer";
  message: string;
}

export interface TileReport {
  passes: TilePass[];
  loadBytes: number;
  storeBytes: number;
  avoidableBytes: number;
  /** Color results stored that nothing in the capture reads (see TileAttachment.unreadBytes). */
  unreadBytes: number;
  post: PostProcessStep[];
  outOfTile: OutOfTile[];
  /** Render passes recorded with subpasses (Vulkan), and the input attachments they use. */
  subpassPasses: TilePass[];
  renderPasses: number;
}

const sizeOf = (detail: string): string | null => /^(\d+x\d+)/.exec(detail)?.[1] ?? null;

/** The render targets a render pass writes, by resource key. */
function attachmentWrites(node: GraphNode): GraphUse[] {
  return node.writes.filter((w) => usageClass(w.usage) === "attachment");
}

export function analyzeTiling(graph: RenderGraph, options: TileOptions = {}): TileReport {
  const nodes = graph.nodes;
  const render = nodes.filter((n) => n.kind === "render");
  const passes: TilePass[] = [];
  for (const node of render) {
    const next = nodes[node.ordinal + 1] ?? null;
    const previous = node.ordinal > 0 ? nodes[node.ordinal - 1] : null;
    const attachments: TileAttachment[] = [];
    for (const w of attachmentWrites(node)) {
      const bytes = w.resource.bytes || 0;
      // A part of a render pass suspended across command lists (Direct3D 12) carries its
      // attachments to the next part on chip: "(local/..." and ".../local)" move nothing.
      const loadsLocal = /\(local\//.test(w.usage);
      const storesLocal = /\/local\)/.test(w.usage);
      const loads = !w.discards && !loadsLocal;
      const stores = !w.dropped && !storesLocal;
      let avoidable: string | null = null;
      let avoidableBytes = 0;
      let unreadBytes = 0;
      let note: string | null = null;
      const readers = w.version.readers;
      if (stores && !w.resource.presented && !readers.length && !w.resolved) {
        // Replaced later in the capture before anything read it, or depth nothing reads: the store
        // is wasted for certain. A color result nothing in the capture reads may be the frame's
        // output to something the capture does not see, so it is only reported.
        const replaced = w.resource.versions.some((v) => v.index > w.version.index && v.producer &&
          v.producer.writes.some((pw) => pw.version === v && pw.discards));
        const depth = /^(depth|stencil)/.test(w.usage);
        if (replaced || depth) {
          avoidable = replaced ? "stored, and replaced before anything reads it" : "depth stored, and nothing later in the capture reads it";
          avoidableBytes += bytes;
        } else {
          unreadBytes = bytes;
        }
      } else if (stores && !w.resource.presented && readers.length && readers.every((r) => r === next) && next?.kind === "render") {
        const carried = next.writes.some((nw) => nw.resource.key === w.resource.key && !nw.discards);
        // Read by the next pass as a texture: that stays in the tile only if the shader reads each
        // pixel once at its own position. A filter (a blur, a downsample) needs it in memory.
        const filters = options.filtersInput?.(next, w.resource.objectId) ?? null;
        if (carried) {
          avoidable = "stored, and loaded straight back by the next pass: one pass would keep it in the tile";
          avoidableBytes += bytes;
        } else if (filters === false) {
          avoidable = "stored for the next pass alone, which reads it once per pixel: a subpass, framebuffer fetch or a transient target would keep it in the tile";
          avoidableBytes += bytes;
        } else if (filters === true) {
          note = "read by the next pass alone, which filters it: that needs it in memory";
        } else {
          note = "read by the next pass alone: if that reads each pixel once, a subpass or framebuffer fetch would keep it in the tile";
        }
      }
      if (loads && previous?.kind === "render" && w.version.index > 1 && previous.writes.some((pw) => pw.version.index === w.version.index - 1 && pw.resource.key === w.resource.key && !pw.dropped && !/\/local\)/.test(pw.usage))) {
        // The load of what the previous pass stored: the other half of the same round trip.
        avoidable = avoidable ?? "loaded from the pass before, which stored it: one pass would keep it in the tile";
        avoidableBytes += bytes;
      }
      attachments.push({ label: w.resource.label, objectId: w.resource.objectId, bytes, loads, stores, avoidable, avoidableBytes, unreadBytes, note });
    }
    const loadBytes = attachments.reduce((s, a) => s + (a.loads ? a.bytes : 0), 0);
    const storeBytes = attachments.reduce((s, a) => s + (a.stores ? a.bytes : 0), 0);
    const avoidableBytes = attachments.reduce((s, a) => s + a.avoidableBytes, 0);
    const unreadBytes = attachments.reduce((s, a) => s + a.unreadBytes, 0);
    passes.push({ node, attachments, loadBytes, storeBytes, avoidableBytes, unreadBytes, subpasses: options.subpasses?.(node) ?? null });
  }

  // Post-processing: a render pass sampling what an earlier render pass rendered at its size.
  const post: PostProcessStep[] = [];
  for (const node of render) {
    const targets = attachmentWrites(node);
    const size = targets.length ? sizeOf(targets[0].resource.detail) : null;
    if (!size) continue;
    for (const r of node.reads) {
      if (r.resource.type !== "image" || usageClass(r.usage) !== "sampled") continue;
      const producer = r.version.producer;
      if (!producer || producer.kind !== "render" || producer.frame !== node.frame) continue;
      if (sizeOf(r.resource.detail) !== size) continue;
      const filters = options.filtersInput?.(node, r.resource.objectId) ?? null;
      post.push({
        node, input: r.resource.label, producer,
        adjacent: producer.ordinal === node.ordinal - 1,
        kind: filters === true ? "filters" : filters === false ? "same-pixel" : "unknown",
      });
    }
  }

  // What leaves the tile.
  const outOfTile: OutOfTile[] = [];
  for (const node of render) {
    const shaderWrites = node.writes.filter((w) => usageClass(w.usage) === "storage");
    if (shaderWrites.length) {
      const names = [...new Set(shaderWrites.map((w) => w.resource.label))];
      outOfTile.push({ node, kind: "shader-write", message: `its shaders write ${names.slice(0, 3).join(", ")}${names.length > 3 ? ` and ${names.length - 3} more` : ""} (storage / UAV): memory traffic from inside the pass, which a tiled GPU cannot keep in the tile` });
    }
    // An attachment the pass also samples: the graph folds a pass's accesses to one resource into
    // one, its usages joined ("color attachment (load/store), sampled").
    const feedback = attachmentWrites(node).filter((w) => w.usage.split(", ").some((u) => usageClass(u) === "sampled") ||
      node.reads.some((r) => r.resource.key === w.resource.key && usageClass(r.usage) === "sampled"));
    if (feedback.length) {
      outOfTile.push({ node, kind: "feedback", message: `samples ${[...new Set(feedback.map((w) => w.resource.label))].join(", ")}, which it also renders to: the texture read goes to memory, which does not hold what the tile has; an input attachment or framebuffer fetch reads the tile` });
    }
  }
  for (let i = 1; i + 1 < nodes.length; i++) {
    const node = nodes[i];
    if (node.kind !== "compute") continue;
    const before = nodes[i - 1];
    const after = nodes[i + 1];
    if (before.kind !== "render" || after.kind !== "render") continue;
    const fromBefore = node.reads.filter((r) => r.version.producer === before);
    if (!fromBefore.length) continue;
    outOfTile.push({ node, kind: "compute-split", message: `a compute pass between two render passes reads what the first rendered (${[...new Set(fromBefore.map((r) => r.resource.label))].join(", ")}): the first has to be stored in full before it runs; a fragment shader in the second pass reading it as an input attachment would not need that` });
  }
  for (let i = 1; i + 1 < nodes.length; i++) {
    const node = nodes[i];
    if (node.kind !== "transfer") continue;
    const touches = [...node.reads, ...node.writes].filter((u) => nodes.some((n) => n.kind === "render" && n.writes.some((w) => w.resource.key === u.resource.key && usageClass(w.usage) === "attachment")));
    if (!touches.length) continue;
    outOfTile.push({ node, kind: "transfer", message: `${node.label} copies or clears ${[...new Set(touches.map((u) => u.resource.label))].join(", ")}, a render target of the frame, outside a pass: the target goes through memory for it, where a clear load op or a resolve in the pass would not` });
  }

  const renderPasses = passes.length;
  return {
    passes,
    loadBytes: passes.reduce((s, p) => s + p.loadBytes, 0),
    storeBytes: passes.reduce((s, p) => s + p.storeBytes, 0),
    avoidableBytes: passes.reduce((s, p) => s + p.avoidableBytes, 0),
    unreadBytes: passes.reduce((s, p) => s + p.unreadBytes, 0),
    post,
    outOfTile,
    subpassPasses: passes.filter((p) => (p.subpasses?.subpasses ?? 1) > 1),
    renderPasses,
  };
}

/** Vulkan: the subpasses and input attachments each render pass was recorded with. */
function vulkanSubpasses(data: CaptureData, db: FrameAnalysisDatabase): (node: GraphNode) => SubpassInfo | null {
  return (node) => {
    const cmd = data.commands[node.commandIndex];
    if (!cmd) return null;
    if (cmd.method.startsWith("vkCmdBeginRendering")) return { subpasses: 1, inputAttachments: 0 };
    const begin = isObject(cmd.args?.pRenderPassBegin) ? cmd.args!.pRenderPassBegin : null;
    const rp = begin ? db.getObject(refId(begin.renderPass))?.descriptor ?? null : null;
    if (!rp) return null;
    const subpasses = Array.isArray(rp.pSubpasses) ? rp.pSubpasses.filter(isObject) : [];
    return {
      subpasses: Math.max(1, subpasses.length),
      inputAttachments: subpasses.reduce((sum, s) => sum + num(s.inputAttachmentCount), 0),
    };
  };
}

/** The tile analysis of a capture, with what each API can tell it beyond the graph. */
export function tileReport(data: CaptureData, db: FrameAnalysisDatabase, graph: RenderGraph): TileReport {
  const options: TileOptions = {};
  if (data.api === "vulkan") {
    // The Vulkan analysis reads fragment shaders' SPIR-V for how they read an image, which is what
    // tells a same-pixel post-processing step from a filter.
    const vulkan = new FrameAnalysis(db);
    vulkan.analyze(data);
    options.filtersInput = (node, imageId) => vulkan.filtersInput(node.commandIndex, imageId);
    options.subpasses = vulkanSubpasses(data, db);
  }
  return analyzeTiling(graph, options);
}
