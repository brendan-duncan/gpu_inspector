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
//   tiny-draws              many draws of a handful of vertices
//
// Every finding names the command it is about so the UI can jump to it.
import { DRAW_METHODS, PASS_BEGIN, PASS_END } from "./command_sets.js";
import { isHandleRef, isObject, num, refId, str, type VulkanObject } from "./vulkan_object.js";
import { SEVERITY_RANK, type Confidence, type Severity } from "./spirv_analysis.js";
import type { CaptureData } from "../capture_data.js";
import type { ObjectLookup } from "./vulkan_object.js";
import type { ArgObject, ArgValue, CaptureCommand } from "../../shared/protocol.js";

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

/** What the analysis needs of the object database (VulkanObject.allObjects for the device's memory types). */
export interface FrameAnalysisDatabase extends ObjectLookup {
  allObjects: Map<number, VulkanObject>;
}

type AttachmentKind = "color" | "depth" | "resolve";

interface AttachmentUse {
  kind: AttachmentKind;
  imageId: number | null;
  viewId: number | null;
  baseLayer: number;
  format: string;
  samples: number;
  loadOp: string;
  storeOp: string;
  stencilLoadOp: string;
  stencilStoreOp: string;
  /** VkImageUsageFlags of the image ("" when unknown: a swapchain image without a create info). */
  usage: string;
  /** A color attachment that has a resolve target in the same subpass. */
  resolved: boolean;
}

interface PassInfo {
  command: CaptureCommand;
  ordinal: number;
  width: number;
  height: number;
  viewMask: number;
  attachments: AttachmentUse[];
  draws: number;
  /** One entry per draw: the bound pipeline and the vertex count, the pass's "shape". */
  drawSignature: string[];
  /** The earlier clear command of an attachment image loaded by this pass, if any. */
  clearedBefore: Map<number, CaptureCommand>;
}

const TINY_DRAW_VERTICES = 12;
const TINY_DRAW_COUNT = 32;

const RULE_ORDER = ["stereo-without-multiview", "clear-outside-pass", "depth-store", "msaa-store", "color-load", "tiny-draws", "redundant-pipeline-bind", "depth-transient"];

function op(v: ArgValue | undefined): string {
  return str(v).replace("VK_ATTACHMENT_LOAD_OP_", "").replace("VK_ATTACHMENT_STORE_OP_", "");
}

function sampleCount(v: ArgValue | undefined): number {
  const m = /VK_SAMPLE_COUNT_(\d+)_BIT/.exec(str(v));
  return m ? Number(m[1]) : 1;
}

function pNextChain(o: ArgObject | null | undefined): ArgObject[] {
  const chain = o?.pNext;
  return Array.isArray(chain) ? chain.filter(isObject) : [];
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
    let redundantBinds = 0;
    let firstRedundantBind: CaptureCommand | null = null;
    let tinyDraws = 0;
    let firstTinyDraw: CaptureCommand | null = null;
    let draws = 0;

    for (const cmd of commands) {
      const method = cmd.method;
      const a = cmd.args;
      const cb = cmd.object?.__id ?? 0;
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
        if (boundPipeline.get(key) === id) {
          redundantBinds++;
          if (!firstRedundantBind) firstRedundantBind = cmd;
        }
        boundPipeline.set(key, id);
      } else if (DRAW_METHODS.has(method)) {
        draws++;
        const pass = open.get(cb) ?? lastPass;
        const pipeline = boundPipeline.get(`${cb}:VK_PIPELINE_BIND_POINT_GRAPHICS`) ?? 0;
        const vertices = method.includes("Indirect") || method.includes("MeshTasks") ? -1
          : num(a?.indexCount || a?.vertexCount) * Math.max(1, num(a?.instanceCount));
        if (pass) {
          pass.draws++;
          pass.drawSignature.push(`${pipeline}:${vertices}`);
        }
        if (vertices >= 0 && vertices <= TINY_DRAW_VERTICES) {
          tinyDraws++;
          if (!firstTinyDraw) firstTinyDraw = cmd;
        }
      } else if (a) {
        // Copies, blits and resolves read their source image.
        for (const key of ["srcImage", "pCopyImageInfo", "pBlitImageInfo", "pResolveImageInfo", "pCopyImageToBufferInfo"]) {
          const v = a[key];
          const id = refId(isObject(v) ? v.srcImage : v);
          if (id !== null) readImages.add(id);
        }
      }
    }

    for (const pass of this._passes) {
      for (const att of pass.attachments) {
        if (att.imageId === null) continue;
        att.usage = this._imageUsage(att.imageId);
        (att as AttachmentUse & { read?: boolean }).read = readImages.has(att.imageId) || loadedImages.has(att.imageId);
      }
    }
    if (redundantBinds && firstRedundantBind) {
      this._add("redundant-pipeline-bind", "low", "high", `vkCmdBindPipeline binds the pipeline that is already bound ${redundantBinds === 1 ? "once" : `${redundantBinds} times`}. Drivers do not always skip the redundant bind; binding once per pipeline change is free.`, firstRedundantBind, redundantBinds);
    }
    if (tinyDraws >= TINY_DRAW_COUNT && firstTinyDraw) {
      this._add("tiny-draws", "medium", "medium", `${tinyDraws} of ${draws} draws render at most ${TINY_DRAW_VERTICES} vertices each. Per-draw overhead (command processing, state changes) outweighs such draws; instancing or merged geometry renders them in one draw.`, firstTinyDraw, tinyDraws);
    }
  }

  // ---------------------------------------------------------------------------- per-pass rules

  private _passRules(): void {
    for (const pass of this._passes) {
      const cmd = pass.command;
      for (const [imageId, clear] of pass.clearedBefore) {
        this._add("clear-outside-pass", "high", "high", `${this._imageName(imageId)} is cleared with ${clear.method} and then loaded by ${this._passName(pass)} (loadOp LOAD). On a tiled GPU the clear is a separate pass over the whole image and the load reads it back into tile memory; loadOp CLEAR clears it for free when the pass begins.`, clear);
      }
      for (const att of pass.attachments) {
        const read = (att as AttachmentUse & { read?: boolean }).read ?? false;
        // TRANSFER_SRC is not a sign the application reads the image: the layer adds it to every
        // image for its own read-back. Copies in the frame are tracked separately (readImages).
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
      this._add("stereo-without-multiview", "medium", "medium", `${pairs} record the same ${first.draws} draw${first.draws === 1 ? "" : "s"} with the same pipelines into different ${first.width}x${first.height} targets, which looks like one pass per eye. With multiview (VK_KHR_multiview: a view mask on the render pass or on vkCmdBeginRendering, gl_ViewIndex in the shaders) both eyes render in one pass: half the commands, and the GPU can share the vertex work between the views.`, first.command, g.length);
    }
  }

  // ---------------------------------------------------------------------------- pass decoding

  private _passInfo(cmd: CaptureCommand, ordinal: number): PassInfo | null {
    const a = cmd.args;
    if (!a) return null;
    const pass: PassInfo = { command: cmd, ordinal, width: 0, height: 0, viewMask: 0, attachments: [], draws: 0, drawSignature: [], clearedBefore: new Map() };
    if (cmd.method.startsWith("vkCmdBeginRendering")) {
      const info = isObject(a.pRenderingInfo) ? a.pRenderingInfo : null;
      if (!info) return pass;
      const extent = isObject(info.renderArea) && isObject(info.renderArea.extent) ? info.renderArea.extent : null;
      pass.width = num(extent?.width);
      pass.height = num(extent?.height);
      pass.viewMask = num(info.viewMask);
      const colors = Array.isArray(info.pColorAttachments) ? info.pColorAttachments.filter(isObject) : [];
      for (const c of colors) pass.attachments.push(this._dynamicAttachment(c, "color"));
      for (const key of ["pDepthAttachment", "pStencilAttachment"]) {
        const d = info[key];
        if (isObject(d) && refId(d.imageView) !== null) pass.attachments.push(this._dynamicAttachment(d, "depth"));
      }
      return pass;
    }
    const begin = isObject(a.pRenderPassBegin) ? a.pRenderPassBegin : null;
    if (!begin) return pass;
    const rp = this._db.getObject(refId(begin.renderPass))?.descriptor ?? null;
    const fb = this._db.getObject(refId(begin.framebuffer))?.descriptor ?? null;
    const extent = isObject(begin.renderArea) && isObject(begin.renderArea.extent) ? begin.renderArea.extent : null;
    pass.width = num(extent?.width) || num(fb?.width);
    pass.height = num(extent?.height) || num(fb?.height);
    if (!rp) return pass;
    // The view masks: VkRenderPassMultiviewCreateInfo (create info 1) or the subpasses' own (2).
    const subpasses = Array.isArray(rp.pSubpasses) ? rp.pSubpasses.filter(isObject) : [];
    const mv = pNextChain(rp).find((s) => str(s.sType) === "VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO");
    const masks = mv && Array.isArray(mv.pViewMasks) ? mv.pViewMasks.map(num) : subpasses.map((s) => num(s.viewMask));
    pass.viewMask = masks.reduce((m, v) => m | v, 0);
    // Attachment views: the framebuffer's, or VkRenderPassAttachmentBeginInfo for an imageless one.
    let views = Array.isArray(fb?.pAttachments) ? fb.pAttachments : [];
    const imageless = pNextChain(begin).find((s) => str(s.sType) === "VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO");
    if (imageless && Array.isArray(imageless.pAttachments)) views = imageless.pAttachments;
    const descs = Array.isArray(rp.pAttachments) ? rp.pAttachments : [];
    const kinds = new Map<number, AttachmentKind>();
    const resolvedColors = new Set<number>();
    for (const s of subpasses) {
      const refs = (key: string): number[] => (Array.isArray(s[key]) ? s[key] : []).filter(isObject).map((r) => num(r.attachment)).filter((i) => i < 0xffffffff);
      const colors = refs("pColorAttachments");
      const resolves = refs("pResolveAttachments");
      colors.forEach((c, i) => { kinds.set(c, "color"); if (resolves[i] !== undefined) resolvedColors.add(c); });
      for (const r of resolves) kinds.set(r, "resolve");
      const ds = isObject(s.pDepthStencilAttachment) ? num(s.pDepthStencilAttachment.attachment) : 0xffffffff;
      if (ds < 0xffffffff) kinds.set(ds, "depth");
    }
    descs.forEach((d, i) => {
      if (!isObject(d)) return;
      const kind = kinds.get(i);
      if (!kind) return;
      const viewId = refId(views[i]);
      const view = this._db.getObject(viewId)?.descriptor ?? null;
      const range = isObject(view?.subresourceRange) ? view.subresourceRange : null;
      pass.attachments.push({
        kind, viewId, imageId: this._imageOfView(viewId), baseLayer: num(range?.baseArrayLayer),
        format: str(d.format), samples: sampleCount(d.samples),
        loadOp: op(d.loadOp), storeOp: op(d.storeOp), stencilLoadOp: op(d.stencilLoadOp), stencilStoreOp: op(d.stencilStoreOp),
        usage: "", resolved: resolvedColors.has(i),
      });
    });
    return pass;
  }

  private _dynamicAttachment(att: ArgObject, kind: AttachmentKind): AttachmentUse {
    const viewId = refId(att.imageView);
    const view = this._db.getObject(viewId)?.descriptor ?? null;
    const imageId = this._imageOfView(viewId);
    const image = this._db.getObject(imageId)?.descriptor ?? null;
    const range = isObject(view?.subresourceRange) ? view.subresourceRange : null;
    return {
      kind, viewId, imageId, baseLayer: num(range?.baseArrayLayer),
      format: str(view?.format ?? image?.format), samples: sampleCount(image?.samples),
      loadOp: op(att.loadOp), storeOp: op(att.storeOp), stencilLoadOp: op(att.loadOp), stencilStoreOp: op(att.storeOp),
      usage: "", resolved: refId(att.resolveImageView) !== null,
    };
  }

  private _imageOfView(viewId: number | null): number | null {
    const view = this._db.getObject(viewId);
    return view ? refId(view.descriptor?.image) : null;
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

  private _add(rule: string, severity: Severity, confidence: Confidence, message: string, cmd: CaptureCommand | null, count = 1): void {
    this.findings.push({ rule, severity, confidence, message, commandIndex: cmd?.index, count });
  }
}

/** Shorthand: the findings of a capture. */
export function analyzeFrame(data: CaptureData, db: FrameAnalysisDatabase): FrameFinding[] {
  return new FrameAnalysis(db).analyze(data);
}
