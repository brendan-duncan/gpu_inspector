// Frame-level performance analysis of a capture: rules over the captured commands and the
// objects they use (render passes, framebuffers, images), the counterpart of the per-shader
// rules in spirv_analysis.ts. The rules lean towards tiled / mobile GPUs and XR, where the
// cost of loading and storing attachments and of extra passes dominates:
//
//   clear-outside-pass      vkCmdClear*Image on an image the frame then renders to with loadOp LOAD
//   color-load              a color attachment loaded on its first use in the frame
//   depth-store             a depth/stencil attachment stored although nothing reads it
//   depth-transient         a depth attachment neither loaded nor stored that is not transient
//   msaa-store              a multisampled attachment stored although it is resolved
//   stereo-without-multiview  two passes with the same draws to same-sized targets (one per eye)
//   redundant-pipeline-bind  binding the pipeline that is already bound
//   redundant-descriptor-bind  binding descriptor sets that are already bound (same offsets)
//   redundant-buffer-bind   binding the vertex or index buffers that are already bound
//   push-constants-unchanged  pushing the bytes that are already in the range
//   barrier-adjacent        a barrier right after another, nothing between them
//   barrier-in-render-pass  a pipeline barrier inside a render pass (breaks the tile pass)
//   full-pipeline-barrier   a barrier from ALL_COMMANDS to ALL_COMMANDS (drains the whole GPU)
//   msaa-sampled            a multisampled attachment stored without a resolve, for sampling
//   single-workgroup-dispatch  a dispatch of one workgroup
//   tiny-draws              many draws of a handful of vertices
//   oversized-attachment    an attachment larger than every render area the frame draws into it
//   redundant-transition    a layout transition nothing uses before the next one, or a barrier
//                           that changes nothing
//
// Every finding names the command it is about so the UI can jump to it (findings per command
// through byCommand()).
//
// analyzeFrame() below also folds in the rules over the capture's render graph
// (../render_graph_analysis.ts) when the caller has built one. Those answer exactly what a few of
// the rules here can only approximate from usage flags, and replace them (SUPERSEDED_RULES).
import { DISPATCH_METHODS, DRAW_METHODS, PASS_BEGIN, PASS_END, TRACE_METHODS, bindPointOf } from "./command_sets.js";
import { decodePass, imageOfView, pNextChain, type AttachmentUse, type PassAttachments } from "./pass_info.js";
import { isHandleRef, isObject, num, refId, str, type VulkanObject } from "./vulkan_object.js";
import { SEVERITY_RANK, type Confidence, type Severity } from "./spirv_analysis.js";
import { SUPERSEDED_RULES, analyzeRenderGraph } from "../render_graph_analysis.js";
import type { RenderGraph } from "../render_graph.js";
import type { CaptureData } from "../capture_data.js";
import type { ObjectLookup } from "./vulkan_object.js";
import { analyzeMetalFrame } from "../metal/frame_analysis.js";
import { analyzeD3D12Frame } from "../d3d12/frame_analysis.js";
import { analyzeCounters } from "../counter_rules.js";
import { analyzeSampling } from "../sampling_rules.js";
import { textureReads, type TextureReads } from "./spirv_ablate.js";
import { pipelineStages } from "../shader_cache.js";
import type { ArgObject, ArgValue, CaptureCommand, CaptureDescriptorSet, CaptureDescriptorSets } from "../../shared/protocol.js";

export interface FrameFinding {
  rule: string;
  severity: Severity;
  confidence: Confidence;
  message: string;
  /** The command the finding is about (a pass begin, a clear, a draw), for jumping to it. */
  commandIndex?: number;
  /** How many commands raised it (folded into one finding). */
  count: number;
}

/**
 * What the analysis needs of the object database: VulkanObject.allObjects for the device's memory
 * types, and the payloads (SPIR-V) it has, for the rules that look at how a shader reads a texture.
 */
export interface FrameAnalysisDatabase extends ObjectLookup {
  allObjects: Map<number, VulkanObject>;
  blobData?: Map<string, Uint8Array>;
}

interface PassInfo extends PassAttachments {
  command: CaptureCommand;
  ordinal: number;
  draws: number;
  /** One entry per draw: the bound pipeline and the vertex count, the pass's "shape". */
  drawSignature: string[];
  /** The earlier clear command of an attachment image loaded by this pass, if any. */
  clearedBefore: Map<number, CaptureCommand>;
  /** The render area's offset (its size is width and height). */
  x: number;
  y: number;
  /** The pipelines and descriptors its draws ran with, each combination once. */
  drawStates: Map<string, { pipeline: number; descriptors: CaptureDescriptorSets }>;
}

const TINY_DRAW_VERTICES = 12;
const TINY_DRAW_COUNT = 32;
/** An attachment is oversized when the frame's render areas in it cover at most this much of it. */
const OVERSIZED_COVERAGE = 0.75;

const RULE_ORDER = ["stereo-without-multiview", "clear-outside-pass", "depth-store", "msaa-store", "msaa-sampled", "barrier-in-render-pass", "oversized-attachment", "color-load", "tiny-draws",
  "full-pipeline-barrier", "redundant-transition", "redundant-pipeline-bind", "redundant-descriptor-bind", "redundant-buffer-bind", "push-constants-unchanged", "barrier-adjacent", "single-workgroup-dispatch", "depth-transient"];

/** The part of an image a barrier names. */
interface BarrierRange { aspects: string[]; mip: number; mips: number; layer: number; layers: number }

function barrierRange(b: ArgObject): BarrierRange {
  const r = isObject(b.subresourceRange) ? b.subresourceRange : null;
  const count = (v: ArgValue | undefined): number => (num(v) === 0xffffffff ? Infinity : num(v));
  return { aspects: str(r?.aspectMask).split("|").map((s) => s.trim()).filter(Boolean), mip: num(r?.baseMipLevel), mips: count(r?.levelCount),
           layer: num(r?.baseArrayLayer), layers: count(r?.layerCount) };
}

function rangesOverlap(a: BarrierRange, b: BarrierRange): boolean {
  const aspects = !a.aspects.length || !b.aspects.length || a.aspects.some((x) => b.aspects.includes(x));
  return aspects && a.mip < b.mip + b.mips && b.mip < a.mip + a.mips && a.layer < b.layer + b.layers && b.layer < a.layer + a.layers;
}

/** Every VkImage a value names, directly or through a VkImageView. */
function collectImages(v: ArgValue | undefined, db: ObjectLookup, out: Set<number>): void {
  if (Array.isArray(v)) {
    for (const e of v) collectImages(e, db, out);
  } else if (isHandleRef(v)) {
    if (v.__class === "VkImage") out.add(v.__id);
    else if (v.__class === "VkImageView") {
      const image = imageOfView(db, v.__id);
      if (image !== null) out.add(image);
    }
  } else if (isObject(v)) {
    for (const e of Object.values(v)) collectImages(e, db, out);
  }
}

/** Every stage mask a barrier command carries (the command's own, or its VkDependencyInfo's barriers). */
function barrierStageMasks(a: ArgObject): { src: string; dst: string }[] {
  if (isObject(a.pDependencyInfo)) {
    const out: { src: string; dst: string }[] = [];
    for (const key of ["pMemoryBarriers", "pBufferMemoryBarriers", "pImageMemoryBarriers"]) {
      const list = a.pDependencyInfo[key];
      if (Array.isArray(list)) for (const b of list) if (isObject(b)) out.push({ src: str(b.srcStageMask), dst: str(b.dstStageMask) });
    }
    return out;
  }
  return [{ src: str(a.srcStageMask), dst: str(a.dstStageMask) }];
}
const BARRIER_METHODS = new Set(["vkCmdPipelineBarrier", "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR"]);

/** A stable text for an argument value (handles by id), to compare successive binds. */
function argKey(v: ArgValue | undefined): string {
  if (isHandleRef(v)) return `#${v.__id}`;
  if (Array.isArray(v)) return `[${v.map(argKey).join(",")}]`;
  if (isObject(v)) return `{${Object.entries(v).map(([k, e]) => `${k}:${argKey(e)}`).join(",")}}`;
  return str(v);
}

/** One finding per rule with the commands it applies to folded in: the first command is named, the rest counted. */
class Folded {
  first: CaptureCommand | null = null;
  count = 0;
  commands: CaptureCommand[] = [];
  add(cmd: CaptureCommand): void {
    if (!this.first) this.first = cmd;
    this.count++;
    if (this.commands.length < 64) this.commands.push(cmd);
  }
}

/** Every VkImageView referenced anywhere in a value (descriptor snapshots, attachment lists). */
function collectImageViews(v: ArgValue | undefined, out: Set<number>): void {
  if (Array.isArray(v)) {
    for (const e of v) collectImageViews(e, out);
  } else if (isHandleRef(v)) {
    if (v.__class === "VkImageView") out.add(v.__id);
  } else if (isObject(v)) {
    for (const e of Object.values(v)) collectImageViews(e, out);
  }
}

export class FrameAnalysis {
  findings: FrameFinding[] = [];
  private _db: FrameAnalysisDatabase;
  private _passes: PassInfo[] = [];
  private _lazyMemory = false;
  /** Every command a finding applies to (folded findings list all of theirs), by command index. */
  private _byCommand = new Map<number, FrameFinding[]>();

  /** The findings that apply to a command (a folded finding counts for each of its commands). */
  byCommand(): Map<number, FrameFinding[]> {
    return this._byCommand;
  }

  constructor(db: FrameAnalysisDatabase) {
    this._db = db;
    for (const o of db.allObjects.values()) {
      if (o.type !== "VkPhysicalDevice") continue;
      const mem = o.updates.memoryProperties;
      const types = isObject(mem) && Array.isArray(mem.memoryTypes) ? mem.memoryTypes : [];
      if (types.some((t) => isObject(t) && str(t.propertyFlags).includes("LAZILY_ALLOCATED"))) this._lazyMemory = true;
    }
  }

  analyze(data: CaptureData): FrameFinding[] {
    this.findings = [];
    this._passes = [];
    this._byCommand = new Map();
    this._walk(data.commands);
    this._passRules();
    this._stereoRule();
    this.findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity] || RULE_ORDER.indexOf(a.rule) - RULE_ORDER.indexOf(b.rule));
    return this.findings;
  }

  // ---------------------------------------------------------------------------- the walk

  private _walk(commands: CaptureCommand[]): void {
    const db = this._db;
    const open = new Map<number, PassInfo>();           // command buffer id -> the pass being recorded
    const boundPipeline = new Map<string, number>();     // "cb:bindPoint" -> pipeline id
    const clears = new Map<number, CaptureCommand>();    // image id -> its last clear command
    const readImages = new Set<number>();                // images something in the frame reads
    const loadedImages = new Set<number>();              // images a pass loaded (read as attachments)
    const lastWrite = new Map<number, number>();         // image id -> index of the last pass that rendered to it
    let lastPass: PassInfo | null = null;
    const redundantBinds = new Folded();
    const redundantSets = new Folded();
    const redundantBuffers = new Folded();
    const unchangedPush = new Folded();
    const adjacentBarriers = new Folded();
    const passBarriers = new Folded();
    const fullBarriers = new Folded();
    const singleDispatches = new Folded();
    const tinyDraws = new Folded();
    let draws = 0;
    const boundSets = new Map<string, string>();         // "cb:bindPoint:set" -> set id + dynamic offsets
    const boundBuffers = new Map<string, string>();      // "cb:v<binding>" / "cb:index" -> buffer, offset (and size, stride, type)
    const pushed = new Map<string, string>();            // "cb:offset:size" -> the bytes last pushed there
    const previous = new Map<number, string>();          // cb -> the method recorded just before
    // Layout transitions nothing has used yet, per image: a later transition of the same
    // subresources makes them wasted.
    const unusedTransitions = new Map<number, { cmd: CaptureCommand; range: BarrierRange }[]>();
    const wastedTransitions = new Folded();
    const noopBarriers = new Folded();

    // The descriptor sets bound per stream and bind point, as the capture snapshots them at each bind.
    const boundDescriptors = new Map<string, Map<number, CaptureDescriptorSet>>();
    const actionDescriptors = (cmd: CaptureCommand, stream: string): CaptureDescriptorSets | null => {
      const bindPoint = bindPointOf(cmd.method);
      const sets = boundDescriptors.get(`${stream}|${bindPoint}`);
      return sets?.size ? { bindPoint, sets: [...sets.values()] } : null;
    };

    for (const cmd of commands) {
      const method = cmd.method;
      const a = cmd.args;
      const cb = cmd.object?.__id ?? 0;
      const stream = `${cb}:${cmd.secondary ?? 0}`;
      if (cmd.descriptors) {
        const key = `${stream}|${cmd.descriptors.bindPoint}`;
        let sets = boundDescriptors.get(key);
        if (!sets) boundDescriptors.set(key, (sets = new Map()));
        for (const s of cmd.descriptors.sets) sets.set(s.set, s);
      }
      if (unusedTransitions.size && !BARRIER_METHODS.has(method)) {
        // Anything else naming an image uses it in the layout it is in: its arguments, the
        // descriptors a draw or dispatch reads, the attachments a pass begins with.
        const used = new Set<number>();
        collectImages(a, db, used);
        if (DRAW_METHODS.has(method) || DISPATCH_METHODS.has(method) || TRACE_METHODS.has(method)) {
          collectImages(actionDescriptors(cmd, stream) as unknown as ArgValue, db, used);
        }
        if (PASS_BEGIN.has(method)) {
          const attachments = decodePass(cmd, db)?.attachments ?? [];
          // A pass whose targets the capture cannot name could be using any of them.
          if (!attachments.length || attachments.some((att) => att.imageId === null)) unusedTransitions.clear();
          for (const att of attachments) if (att.imageId !== null) used.add(att.imageId);
        }
        for (const image of used) unusedTransitions.delete(image);
      }
      if (cmd.descriptors) {
        const views = new Set<number>();
        collectImageViews(cmd.descriptors as unknown as ArgValue, views);
        for (const v of views) {
          const img = this._imageOfView(v);
          if (img !== null) readImages.add(img);
        }
      }
      if (PASS_BEGIN.has(method)) {
        const pass = this._passInfo(cmd, this._passes.length);
        if (!pass) continue;
        for (const att of pass.attachments) {
          if (att.imageId === null) continue;
          const loads = att.loadOp === "LOAD" || (att.kind === "depth" && att.stencilLoadOp === "LOAD");
          if (loads) {
            loadedImages.add(att.imageId);
            // A clear after the image's last pass (or before any) is what this pass loads.
            const clear = clears.get(att.imageId);
            const writtenAt = lastWrite.get(att.imageId) ?? -1;
            if (clear && clear.index > writtenAt) pass.clearedBefore.set(att.imageId, clear);
            else if (!clear && writtenAt < 0 && att.kind === "color") {
              // Loaded before anything in this frame wrote it: the previous frame's contents.
              this._add("color-load", "low", "medium", `${this._passName(pass)} loads ${this._imageName(att.imageId)} (loadOp LOAD) before anything in the frame rendered to it, so it reads the previous frame's contents into tile memory. CLEAR or DONT_CARE skips that read when the pass covers the whole target.`, cmd);
            }
          }
          lastWrite.set(att.imageId, cmd.index);
        }
        open.set(cb, pass);
        lastPass = pass;
        this._passes.push(pass);
      } else if (PASS_END.has(method)) {
        open.delete(cb);
      } else if (method === "vkCmdClearColorImage" || method === "vkCmdClearDepthStencilImage") {
        const id = refId(a?.image);
        if (id !== null) clears.set(id, cmd);
      } else if (method === "vkCmdBindPipeline" && a) {
        const key = `${cb}:${str(a.pipelineBindPoint)}`;
        const id = refId(a.pipeline) ?? 0;
        if (boundPipeline.get(key) === id) redundantBinds.add(cmd);
        boundPipeline.set(key, id);
      } else if (method === "vkCmdBindShadersEXT" && a) {
        // Shader objects replace the pipeline's stages. The fragment shader object stands for the
        // draws' "pipeline" here: its reads are what the subpass rule looks at.
        const stages = Array.isArray(a.pStages) ? a.pStages.map(str) : [];
        const shaders = Array.isArray(a.pShaders) ? a.pShaders : [];
        if (stages.some((st) => st !== "VK_SHADER_STAGE_COMPUTE_BIT")) {
          const fragment = stages.indexOf("VK_SHADER_STAGE_FRAGMENT_BIT");
          const key = `${cb}:VK_PIPELINE_BIND_POINT_GRAPHICS`;
          if (fragment >= 0) boundPipeline.set(key, refId(shaders[fragment]) ?? 0);
          else if (!this._db.getObject(boundPipeline.get(key))?.type.startsWith("VkShaderEXT")) boundPipeline.set(key, 0);
        }
      } else if ((method === "vkCmdBindDescriptorSets" || method === "vkCmdBindDescriptorSets2" || method === "vkCmdBindDescriptorSets2KHR") && a) {
        const info = isObject(a.pBindDescriptorSetsInfo) ? a.pBindDescriptorSetsInfo : a;
        const sets = Array.isArray(info.pDescriptorSets) ? info.pDescriptorSets : [];
        const offsets = Array.isArray(info.pDynamicOffsets) ? info.pDynamicOffsets.map(str) : [];
        const first = num(info.firstSet);
        const bindPoint = str(info.pipelineBindPoint ?? info.stageFlags);
        let same = sets.length > 0;
        let offsetAt = 0;
        sets.forEach((set, i) => {
          // The dynamic offsets belong to the sets in order; without the layouts the split is
          // unknown, so the whole list is attached to the last set and compared as one.
          const own = i === sets.length - 1 ? offsets.slice(offsetAt).join(",") : "";
          const value = `${argKey(set)}|${own}`;
          const key = `${cb}:${bindPoint}:${first + i}`;
          if (boundSets.get(key) !== value) same = false;
          boundSets.set(key, value);
        });
        offsetAt = offsets.length;
        if (same) redundantSets.add(cmd);
      } else if ((method === "vkCmdBindVertexBuffers" || method === "vkCmdBindVertexBuffers2" || method === "vkCmdBindVertexBuffers2EXT") && a) {
        const buffers = Array.isArray(a.pBuffers) ? a.pBuffers : [];
        const field = (name: string, i: number): string => (Array.isArray(a[name]) ? str(a[name][i]) : "");
        let same = buffers.length > 0;
        buffers.forEach((b, i) => {
          const key = `${cb}:v${num(a.firstBinding) + i}`;
          const value = `${argKey(b)}|${field("pOffsets", i)}|${field("pSizes", i)}|${field("pStrides", i)}`;
          if (boundBuffers.get(key) !== value) same = false;
          boundBuffers.set(key, value);
        });
        if (same) redundantBuffers.add(cmd);
      } else if ((method === "vkCmdBindIndexBuffer" || method === "vkCmdBindIndexBuffer2" || method === "vkCmdBindIndexBuffer2KHR") && a) {
        const key = `${cb}:index`;
        const value = `${argKey(a.buffer)}|${str(a.offset)}|${str(a.size)}|${str(a.indexType)}`;
        if (boundBuffers.get(key) === value) redundantBuffers.add(cmd);
        boundBuffers.set(key, value);
      } else if ((method === "vkCmdPushConstants" || method === "vkCmdPushConstants2" || method === "vkCmdPushConstants2KHR") && a) {
        const info = isObject(a.pPushConstantsInfo) ? a.pPushConstantsInfo : a;
        const bytes = isObject(info.pValues) ? str(info.pValues.base64) : "";
        if (bytes) {
          const key = `${cb}:${argKey(info.layout)}:${str(info.stageFlags)}:${num(info.offset)}:${num(info.size)}`;
          if (pushed.get(key) === bytes) unchangedPush.add(cmd);
          pushed.set(key, bytes);
        }
      } else if (BARRIER_METHODS.has(method)) {
        if (BARRIER_METHODS.has(previous.get(cb) ?? "")) adjacentBarriers.add(cmd);
        if (open.has(cb)) passBarriers.add(cmd);
        if (a && barrierStageMasks(a).some((m) => m.src.includes("ALL_COMMANDS") && m.dst.includes("ALL_COMMANDS"))) fullBarriers.add(cmd);
        if (a) this._transitions(cmd, a, unusedTransitions, wastedTransitions, noopBarriers);
      } else if ((method === "vkCmdDispatch" || method === "vkCmdDispatchBase" || method === "vkCmdDispatchBaseKHR") && a) {
        if (num(a.groupCountX) * num(a.groupCountY) * num(a.groupCountZ) === 1) singleDispatches.add(cmd);
      } else if (DRAW_METHODS.has(method)) {
        draws++;
        const pass = open.get(cb) ?? lastPass;
        const pipeline = boundPipeline.get(`${cb}:VK_PIPELINE_BIND_POINT_GRAPHICS`) ?? 0;
        const vertices = method.includes("Indirect") || method.includes("MeshTasks") ? -1
          : num(a?.indexCount || a?.vertexCount) * Math.max(1, num(a?.instanceCount));
        if (pass) {
          pass.draws++;
          pass.drawSignature.push(`${pipeline}:${vertices}`);
          const descriptors = pass.drawStates.size < 256 ? actionDescriptors(cmd, stream) : null;
          if (descriptors) {
            const key = `${pipeline}|${argKey(descriptors as unknown as ArgValue)}`;
            if (!pass.drawStates.has(key)) pass.drawStates.set(key, { pipeline, descriptors });
          }
        }
        if (vertices >= 0 && vertices <= TINY_DRAW_VERTICES) tinyDraws.add(cmd);
      } else if (a) {
        // Copies, blits and resolves read their source image.
        for (const key of ["srcImage", "pCopyImageInfo", "pBlitImageInfo", "pResolveImageInfo", "pCopyImageToBufferInfo"]) {
          const v = a[key];
          const id = refId(isObject(v) ? v.srcImage : v);
          if (id !== null) readImages.add(id);
        }
      }
      // Commands that record nothing (labels) do not separate two barriers.
      if (!method.includes("DebugUtilsLabel") && !method.includes("DebugMarker")) previous.set(cb, method);
    }

    for (const pass of this._passes) {
      for (const att of pass.attachments) {
        if (att.imageId === null) continue;
        att.usage = this._imageUsage(att.imageId);
        (att as AttachmentUse & { read?: boolean }).read = readImages.has(att.imageId) || loadedImages.has(att.imageId);
      }
    }
    const times = (f: Folded): string => (f.count === 1 ? "once" : `${f.count} times`);
    if (redundantBinds.count) this._addFolded("redundant-pipeline-bind", "low", "high", `vkCmdBindPipeline binds the pipeline that is already bound ${times(redundantBinds)}. Drivers do not always skip the redundant bind; binding once per pipeline change is free.`, redundantBinds);
    if (redundantSets.count) this._addFolded("redundant-descriptor-bind", "low", "high", `vkCmdBindDescriptorSets binds the descriptor sets already bound at those set numbers, with the same dynamic offsets, ${times(redundantSets)}. Binding once per change saves the command and the driver's descriptor work.`, redundantSets);
    if (redundantBuffers.count) this._addFolded("redundant-buffer-bind", "low", "high", `The vertex or index buffers already bound (same buffers, offsets and sizes) are bound again ${times(redundantBuffers)}.`, redundantBuffers);
    if (unchangedPush.count) this._addFolded("push-constants-unchanged", "low", "high", `vkCmdPushConstants pushes the bytes that range already holds ${times(unchangedPush)}. Pushing only what changed saves the command and the constant update.`, unchangedPush);
    if (adjacentBarriers.count) this._addFolded("barrier-adjacent", "low", "medium", `A pipeline barrier directly follows another ${times(adjacentBarriers)}: nothing is recorded between them, so one barrier carrying both sets of transitions and stage masks would do, and each barrier can drain the pipeline.`, adjacentBarriers);
    if (fullBarriers.count) this._addFolded("full-pipeline-barrier", "low", "medium", `A barrier waits for every stage and blocks every stage (ALL_COMMANDS to ALL_COMMANDS) ${times(fullBarriers)}: the GPU drains completely before it continues. Naming the stages that produce and consume the data lets the rest overlap.`, fullBarriers);
    if (passBarriers.count) this._addFolded("barrier-in-render-pass", "medium", "medium", `A pipeline barrier is recorded inside a render pass ${times(passBarriers)}. On a tiled GPU a barrier inside a pass forces the tiles to be flushed and reloaded; move the dependency to a subpass dependency or before the pass.`, passBarriers);
    if (singleDispatches.count) this._addFolded("single-workgroup-dispatch", "low", "medium", `vkCmdDispatch launches a single workgroup ${times(singleDispatches)}: most of the GPU idles during it. Larger dispatches, or a dispatch that folds the work of several small ones, use the machine.`, singleDispatches);
    if (wastedTransitions.count) this._addFolded("redundant-transition", "low", "medium", `A barrier transitions an image to a layout that nothing uses before a later barrier transitions the same subresources again, ${times(wastedTransitions)}. One transition straight to the layout the image is used in does the same work once. Only this capture's commands are seen, so a use on another queue or in a command buffer recorded before the capture looks like none.`, wastedTransitions);
    if (noopBarriers.count) this._addFolded("redundant-transition", "low", "medium", `A barrier leaves an image in the layout it was in, with no queue family change and no write access to wait for, ${times(noopBarriers)}: it synchronizes nothing, and each barrier still costs the driver a pipeline stall.`, noopBarriers);
    if (tinyDraws.count >= TINY_DRAW_COUNT) this._addFolded("tiny-draws", "medium", "medium", `${tinyDraws.count} of ${draws} draws render at most ${TINY_DRAW_VERTICES} vertices each. Per-draw overhead (command processing, state changes) outweighs such draws; instancing or merged geometry renders them in one draw.`, tinyDraws);
  }

  /** A barrier command's image transitions, against those nothing has used yet (redundant-transition). */
  private _transitions(cmd: CaptureCommand, a: ArgObject, unused: Map<number, { cmd: CaptureCommand; range: BarrierRange }[]>,
                       wasted: Folded, noop: Folded): void {
    const groups = isObject(a.pDependencyInfo) ? [a.pDependencyInfo] : [a];
    let changesNothing = true;
    let images = 0;
    for (const g of groups) {
      if ((Array.isArray(g.pBufferMemoryBarriers) && g.pBufferMemoryBarriers.length) || (Array.isArray(g.pMemoryBarriers) && g.pMemoryBarriers.length)) changesNothing = false;
      for (const b of Array.isArray(g.pImageMemoryBarriers) ? g.pImageMemoryBarriers.filter(isObject) : []) {
        const image = refId(b.image);
        if (image === null) continue;
        images++;
        const src = str(b.srcQueueFamilyIndex);
        const dst = str(b.dstQueueFamilyIndex);
        const queueChange = src !== dst && src !== "" && dst !== "";
        const oldLayout = str(b.oldLayout);
        const newLayout = str(b.newLayout);
        if (oldLayout !== newLayout || queueChange || /WRITE|MEMORY_WRITE/.test(str(b.srcAccessMask))) changesNothing = false;
        if (oldLayout === newLayout || newLayout.includes("UNDEFINED")) continue;
        const range = barrierRange(b);
        const list = unused.get(image) ?? [];
        // An earlier transition of these subresources that nothing used: this one replaces it.
        const kept: { cmd: CaptureCommand; range: BarrierRange }[] = [];
        for (const t of list) {
          if (t.cmd !== cmd && rangesOverlap(t.range, range)) {
            if (!wasted.commands.includes(t.cmd)) wasted.add(t.cmd);
          } else {
            kept.push(t);
          }
        }
        kept.push({ cmd, range });
        unused.set(image, kept);
      }
    }
    if (images && changesNothing) noop.add(cmd);
  }

  // ---------------------------------------------------------------------------- per-pass rules

  /**
   * Attachments larger than every render area the frame draws into them: dynamic resolution
   * rendering into a full-size target, or a target allocated for another size. The memory, and on
   * a tiled GPU the load and store of the whole attachment, cost the full size. An atlas drawn a
   * region at a time is covered by the union of its passes' areas and is not reported.
   */
  private _oversizedAttachments(): void {
    const byTarget = new Map<string, { imageId: number; width: number; height: number; x0: number; y0: number; x1: number; y1: number; passes: PassInfo[] }>();
    for (const pass of this._passes) {
      if (!pass.width || !pass.height) continue;
      for (const att of pass.attachments) {
        if (att.imageId === null) continue;
        const image = this._db.getObject(att.imageId);
        const extent = isObject(image?.descriptor?.extent) ? image.descriptor.extent : null;
        if (!extent) continue;       // swapchain images: the swapchain's size is the window's
        const width = Math.max(1, num(extent.width) >> att.mipLevel);
        const height = Math.max(1, num(extent.height) >> att.mipLevel);
        const key = `${att.imageId}:${att.mipLevel}`;
        let t = byTarget.get(key);
        if (!t) byTarget.set(key, (t = { imageId: att.imageId, width, height, x0: Infinity, y0: Infinity, x1: 0, y1: 0, passes: [] }));
        t.x0 = Math.min(t.x0, pass.x);
        t.y0 = Math.min(t.y0, pass.y);
        t.x1 = Math.max(t.x1, Math.min(width, pass.x + pass.width));
        t.y1 = Math.max(t.y1, Math.min(height, pass.y + pass.height));
        if (!t.passes.includes(pass)) t.passes.push(pass);
      }
    }
    const folded = new Folded();
    const cases: string[] = [];
    let targets = 0;
    for (const t of byTarget.values()) {
      const covered = Math.max(0, t.x1 - t.x0) * Math.max(0, t.y1 - t.y0);
      if (covered <= 0 || covered > OVERSIZED_COVERAGE * t.width * t.height) continue;
      targets++;
      for (const p of t.passes) if (!folded.commands.includes(p.command)) folded.add(p.command);
      if (cases.length < 3) cases.push(`${this._imageName(t.imageId)} is ${t.width}x${t.height} but drawn only in ${t.x1 - t.x0}x${t.y1 - t.y0}`);
    }
    if (!folded.count) return;
    const more = targets > cases.length ? ` (and ${targets - cases.length} more attachments)` : "";
    this._addFolded("oversized-attachment", "low", "medium", `${cases.join("; ")}${more}. The frame never renders outside that area, but the image's memory, and on a tiled GPU each load and store, cost the whole attachment. Sized to the area (or, for dynamic resolution, recreated when the scale settles), it costs what is drawn. Passes whose render area moves between frames look the same here.`, folded);
  }

  private _passRules(): void {
    this._oversizedAttachments();
    for (const pass of this._passes) {
      const cmd = pass.command;
      for (const [imageId, clear] of pass.clearedBefore) {
        this._add("clear-outside-pass", "high", "high", `${this._imageName(imageId)} is cleared with ${clear.method} and then loaded by ${this._passName(pass)} (loadOp LOAD). On a tiled GPU the clear is a separate pass over the whole image and the load reads it back into tile memory; loadOp CLEAR clears it for free when the pass begins.`, clear);
      }
      for (const att of pass.attachments) {
        const read = (att as AttachmentUse & { read?: boolean }).read ?? false;
        // TRANSFER_SRC is not a sign the application reads the image: captures from before the
        // record kept the application's arguments carry the layer's own addition of it for the
        // read-back. Copies in the frame are tracked separately (readImages).
        const canBeRead = /SAMPLED|INPUT_ATTACHMENT|STORAGE/.test(att.usage);
        if (att.kind === "depth") {
          const stored = att.storeOp === "STORE" || (att.format.includes("S8") && att.stencilStoreOp === "STORE");
          if (stored && !read && att.usage !== "") {
            if (canBeRead) {
              this._add("depth-store", "low", "low", `${this._passName(pass)} stores its depth attachment ${this._imageName(att.imageId)} (storeOp STORE). Nothing in the captured frame reads it, but the image can be sampled or copied; if no later frame reads it either, storeOp DONT_CARE saves writing it back from tile memory.`, cmd);
            } else {
              this._add("depth-store", "medium", "high", `${this._passName(pass)} stores its depth attachment ${this._imageName(att.imageId)} (storeOp STORE) although the image is only ever a depth attachment: nothing can read it back. storeOp DONT_CARE keeps a tiled GPU from writing the depth tile to memory at the end of the pass.`, cmd);
            }
          } else if (!stored && att.loadOp !== "LOAD" && att.usage !== "" && !att.usage.includes("TRANSIENT_ATTACHMENT") && !canBeRead) {
            this._add("depth-transient", this._lazyMemory ? "low" : "info", this._lazyMemory ? "high" : "medium", `The depth attachment ${this._imageName(att.imageId)} of ${this._passName(pass)} is neither loaded nor stored, so it never needs memory outside the tile: VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT with LAZILY_ALLOCATED memory lets a tiled GPU skip allocating it${this._lazyMemory ? " (this device has a lazily allocated memory type)" : ""}.`, cmd);
          }
        } else if (att.kind === "color" && att.samples > 1 && att.resolved && att.storeOp === "STORE") {
          this._add("msaa-store", "medium", "high", `${this._passName(pass)} stores the ${att.samples}x multisampled attachment ${this._imageName(att.imageId)} (storeOp STORE) although the pass resolves it: the resolve target holds the result, so DONT_CARE saves writing ${att.samples} samples per pixel.`, cmd);
        } else if (att.kind === "color" && att.samples > 1 && !att.resolved && att.storeOp === "STORE" && att.usage.includes("SAMPLED")) {
          this._add("msaa-sampled", "low", "medium", `${this._passName(pass)} stores the ${att.samples}x multisampled attachment ${this._imageName(att.imageId)} without resolving it, and the image can be sampled: sampling a multisampled image costs ${att.samples} fetches per texel, while a resolve attachment on the pass produces the single-sampled result in tile memory.`, cmd);
        }
      }
    }
  }

  /** Passes without multiview that render the same draws to same-sized targets: one per eye. */
  private _stereoRule(): void {
    const groups = new Map<string, PassInfo[]>();
    for (const pass of this._passes) {
      if (pass.draws === 0 || pass.viewMask > 1) continue;
      const formats = pass.attachments.map((a) => `${a.kind}:${a.format}:${a.samples}`).join(",");
      const key = `${pass.width}x${pass.height}|${formats}|${pass.drawSignature.join(",")}`;
      const g = groups.get(key);
      if (g) g.push(pass); else groups.set(key, [pass]);
    }
    for (const g of groups.values()) {
      if (g.length < 2) continue;
      // Different targets (another image, or another layer of the same image) rule out a plain
      // repeat of the same pass.
      const targets = new Set(g.map((p) => p.attachments.filter((a) => a.kind === "color").map((a) => `${a.imageId}/${a.baseLayer}`).join(",")));
      if (targets.size < 2) continue;
      const first = g[0];
      const pairs = g.length === 2 ? `${this._passName(g[0])} and ${this._passName(g[1])}` : `${g.length} passes starting with ${this._passName(first)}`;
      const f = this._add("stereo-without-multiview", "medium", "medium", `${pairs} record the same ${first.draws} draw${first.draws === 1 ? "" : "s"} with the same pipelines into different ${first.width}x${first.height} targets, which looks like one pass per eye. With multiview (VK_KHR_multiview: a view mask on the render pass or on vkCmdBeginRendering, gl_ViewIndex in the shaders) both eyes render in one pass: half the commands, and the GPU can share the vertex work between the views.`, first.command, g.length);
      for (const p of g.slice(1)) this._attach(p.command.index, f);
    }
  }

  /**
   * Whether a pass's fragment shaders filter an image they read: some draw binds a view of it where
   * the shader reads that binding more than once per invocation, or in a loop, as a blur or an
   * ambient occlusion pass does. False when every read is a single one; null when no shader could
   * be looked at (no SPIR-V in the capture, or no draw names the image).
   */
  filtersInput(passCommand: number, imageId: number): boolean | null {
    const pass = this._passes.find((p) => p.command.index === passCommand);
    if (!pass) return null;
    let known = false;
    for (const { pipeline, descriptors } of pass.drawStates.values()) {
      const bindings: { set: number; binding: number }[] = [];
      for (const s of descriptors.sets) {
        for (const b of s.bindings) {
          if (b.descriptors.some((d) => d?.imageView && imageOfView(this._db, refId(d.imageView)) === imageId)) bindings.push({ set: s.set, binding: b.binding });
        }
      }
      if (!bindings.length) continue;
      const reads = this._fragmentReads(pipeline);
      if (!reads) continue;
      known = true;
      for (const { set, binding } of bindings) {
        const r = reads.find((t) => t.set === set && t.binding === binding);
        if (r && (r.reads > 1 || r.inLoop)) return true;
      }
    }
    return known ? false : null;
  }

  private _readsCache = new Map<number, TextureReads[] | null>();

  private _fragmentReads(pipelineId: number): TextureReads[] | null {
    if (this._readsCache.has(pipelineId)) return this._readsCache.get(pipelineId)!;
    let result: TextureReads[] | null = null;
    const pipeline = this._db.getObject(pipelineId);
    const source = pipeline ? pipelineStages(pipeline, this._db).find((s) => s.stage === "fragment") : undefined;
    const spirv = source ? this._db.blobData?.get(`${source.object.id}:${source.blobIndex}`) : undefined;
    if (source && spirv) result = textureReads(spirv, source.entryPoint);
    this._readsCache.set(pipelineId, result);
    return result;
  }

  // ---------------------------------------------------------------------------- pass decoding

  private _passInfo(cmd: CaptureCommand, ordinal: number): PassInfo | null {
    const decoded = decodePass(cmd, this._db);
    if (!decoded) return null;
    const a = cmd.args;
    const info = isObject(a?.pRenderingInfo) ? a.pRenderingInfo : isObject(a?.pRenderPassBegin) ? a.pRenderPassBegin : null;
    const offset = isObject(info?.renderArea) && isObject(info.renderArea.offset) ? info.renderArea.offset : null;
    return { command: cmd, ordinal, ...decoded, draws: 0, drawSignature: [], clearedBefore: new Map(), x: Math.max(0, num(offset?.x)), y: Math.max(0, num(offset?.y)), drawStates: new Map() };
  }

  private _imageOfView(viewId: number | null): number | null {
    return imageOfView(this._db, viewId);
  }

  private _imageUsage(imageId: number): string {
    const image = this._db.getObject(imageId);
    return str(image?.descriptor?.usage);
  }

  private _imageName(imageId: number | null): string {
    const o = this._db.getObject(imageId);
    return o ? o.name : "the image";
  }

  private _passName(pass: PassInfo): string {
    const begin = isObject(pass.command.args?.pRenderPassBegin) ? pass.command.args.pRenderPassBegin : null;
    const rp = this._db.getObject(refId(begin?.renderPass));
    return `pass ${pass.ordinal}${rp?.label ? ` (${rp.label})` : ""}`;
  }

  private _add(rule: string, severity: Severity, confidence: Confidence, message: string, cmd: CaptureCommand | null, count = 1): FrameFinding {
    const f: FrameFinding = { rule, severity, confidence, message, commandIndex: cmd?.index, count };
    this.findings.push(f);
    if (cmd) this._attach(cmd.index, f);
    return f;
  }

  private _addFolded(rule: string, severity: Severity, confidence: Confidence, message: string, folded: Folded): void {
    const f = this._add(rule, severity, confidence, message, folded.first, folded.count);
    for (const cmd of folded.commands) if (cmd !== folded.first) this._attach(cmd.index, f);
  }

  private _attach(index: number, f: FrameFinding): void {
    const list = this._byCommand.get(index);
    if (list) list.push(f); else this._byCommand.set(index, [f]);
  }
}

/**
 * The findings of a capture, and which commands each applies to.
 *
 * Two analyses contribute. These per-command rules (or metal/frame_analysis.ts for a Metal
 * capture, d3d12/frame_analysis.ts for a D3D12 one) read each API's own command stream; and when
 * the caller has built the capture's render graph, the rules over that graph
 * (render_graph_analysis.ts) read the frame's dependencies. The graph answers exactly what a few
 * of the per-command rules can only approximate, so those are dropped in its favor rather than
 * reported twice in two wordings.
 */
export function analyzeFrame(data: CaptureData, db: FrameAnalysisDatabase, graph?: RenderGraph | null): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } {
  const vulkan = data.api === "metal" || data.api === "d3d12" ? null : new FrameAnalysis(db);
  const base = vulkan ? { findings: vulkan.analyze(data), byCommand: vulkan.byCommand() }
    : data.api === "d3d12" ? analyzeD3D12Frame(data, db) : analyzeMetalFrame(data, db);
  // The rules over the GPU counters read the same measurements for either API (counter_rules.ts),
  // and say nothing when the capture carries none; the sampling rules read descriptors both APIs
  // record (sampling_rules.ts).
  const sources = [base, analyzeCounters(data, db), analyzeSampling(data, db)];
  if (graph) sources.push(analyzeRenderGraph(graph, vulkan ? { filtersInput: (node, imageId) => vulkan.filtersInput(node.commandIndex, imageId) } : {}));

  const findings: FrameFinding[] = [];
  const byCommand = new Map<number, FrameFinding[]>();
  for (const source of sources) {
    // The graph answers exactly what a few per-command rules can only approximate, so those are
    // dropped in its favor rather than reported twice in two wordings.
    const drop = graph && source === base ? SUPERSEDED_RULES : null;
    for (const f of source.findings) {
      if (!drop || !drop.has(f.rule)) findings.push(f);
    }
    for (const [index, list] of source.byCommand) {
      const kept = drop ? list.filter((f) => !drop.has(f.rule)) : list;
      if (!kept.length) continue;
      const existing = byCommand.get(index);
      if (existing) existing.push(...kept); else byCommand.set(index, [...kept]);
    }
  }
  findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity]);
  return { findings, byCommand };
}
