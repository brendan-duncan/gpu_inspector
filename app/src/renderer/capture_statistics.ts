// Frame statistics of a capture, after WebGPU Inspector's capture_statistics.js: API activity,
// passes, pipelines, bindings, memory traffic and geometry, computed from the captured commands
// (with the bound pipeline's topology for triangle counts and captured indirect arguments for
// indirect draws).
import { Div } from "./widget/div.js";
import { Widget } from "./widget/widget.js";
import { DISPATCH_METHODS, DRAW_METHODS, LABEL_BEGIN, PASS_BEGIN, SUBMIT_METHODS, TRACE_METHODS } from "./vulkan/command_sets.js";
import { fmt, formatBytes, isObject, num, refId, str, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { CaptureData } from "./capture_data.js";
import type { ObjectDatabase } from "./vulkan/object_database.js";
import type { CaptureCommand } from "../shared/protocol.js";

const COPY_METHODS = new Set([
  "vkCmdCopyBuffer", "vkCmdCopyBuffer2", "vkCmdCopyBuffer2KHR", "vkCmdCopyImage", "vkCmdCopyImage2", "vkCmdCopyImage2KHR",
  "vkCmdCopyBufferToImage", "vkCmdCopyBufferToImage2", "vkCmdCopyBufferToImage2KHR", "vkCmdCopyImageToBuffer",
  "vkCmdCopyImageToBuffer2", "vkCmdCopyImageToBuffer2KHR", "vkCmdBlitImage", "vkCmdBlitImage2", "vkCmdBlitImage2KHR",
  "vkCmdResolveImage", "vkCmdResolveImage2", "vkCmdResolveImage2KHR", "vkCmdUpdateBuffer", "vkCmdFillBuffer",
  "vkCmdClearColorImage", "vkCmdClearDepthStencilImage", "vkCmdClearAttachments",
]);
const BARRIER_METHODS = new Set(["vkCmdPipelineBarrier", "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR", "vkCmdSetEvent", "vkCmdSetEvent2", "vkCmdWaitEvents", "vkCmdWaitEvents2"]);

export interface StatRow { label: string; value: number; bytes?: boolean }
export interface StatSection { title: string; rows: StatRow[] }

export class CaptureStatistics {
  frames = 0;
  apiCalls = 0;
  submits = 0;
  commandBuffers = 0;
  secondaryCommandBuffers = 0;
  draws = 0;
  indexedDraws = 0;
  indirectDraws = 0;
  meshDraws = 0;
  dispatches = 0;
  traceRays = 0;
  copyCommands = 0;
  barriers = 0;
  debugLabels = 0;

  renderPasses = 0;
  colorAttachments = 0;
  depthStencilAttachments = 0;
  renderTargetsCaptured = 0;

  bindPipeline = 0;
  graphicsPipelinesBound = 0;
  computePipelinesBound = 0;
  uniquePipelines = 0;
  vertexStages = 0;
  fragmentStages = 0;
  computeStages = 0;

  bindDescriptorSets = 0;
  descriptorSetsBound = 0;
  uniqueDescriptorSets = 0;
  uniformBuffers = 0;
  storageBuffers = 0;
  images = 0;
  samplers = 0;
  texelBuffers = 0;
  bindVertexBuffers = 0;
  bindIndexBuffer = 0;
  pushConstants = 0;
  pushConstantBytes = 0;

  updateBuffer = 0;
  updateBufferBytes = 0;
  fillBufferBytes = 0;
  bufferCopyBytes = 0;
  capturedBufferBytes = 0;
  capturedBuffers = 0;

  totalInstances = 0;
  totalVertices = 0;
  totalTriangles = 0;
  totalLines = 0;
  totalPoints = 0;
  totalPatches = 0;

  compute(data: CaptureData, db: ObjectDatabase): this {
    this.frames = data.frames;
    const pipelines = new Set<number>();
    const sets = new Set<number>();
    const commandBuffers = new Set<number>();
    const secondaries = new Set<number>();
    // Bound graphics pipeline per command stream (primary, or one inlined secondary).
    const boundPipeline = new Map<string, number>();

    for (const cmd of data.commands) {
      if (!cmd) continue;
      this.apiCalls++;
      const method = cmd.method;
      const a = cmd.args;
      const stream = `${cmd.object?.__id ?? 0}:${cmd.secondary ?? 0}`;
      if (SUBMIT_METHODS.has(method)) {
        this.submits++;
        continue;
      }
      if (cmd.object) commandBuffers.add(cmd.object.__id);
      if (cmd.secondary) secondaries.add(cmd.secondary);

      if (DRAW_METHODS.has(method)) {
        this.draws++;
        if (method.includes("Indexed")) this.indexedDraws++;
        if (method.includes("MeshTasks")) this.meshDraws++;
        const pipeline = db.getObject(boundPipeline.get(stream));
        this._geometry(cmd, data, pipeline);
      } else if (DISPATCH_METHODS.has(method)) {
        this.dispatches++;
      } else if (TRACE_METHODS.has(method)) {
        this.traceRays++;
      } else if (COPY_METHODS.has(method)) {
        this.copyCommands++;
        this._memory(cmd, db);
      } else if (BARRIER_METHODS.has(method)) {
        this.barriers++;
      } else if (LABEL_BEGIN.has(method)) {
        this.debugLabels++;
      } else if (PASS_BEGIN.has(method)) {
        this.renderPasses++;
        this._attachments(cmd, db);
      } else if (method === "vkCmdBindPipeline" && a) {
        this.bindPipeline++;
        const id = refId(a.pipeline);
        if (id !== null) pipelines.add(id);
        const bindPoint = str(a.pipelineBindPoint);
        if (bindPoint === "VK_PIPELINE_BIND_POINT_GRAPHICS") {
          this.graphicsPipelinesBound++;
          boundPipeline.set(stream, id ?? 0);
        } else if (bindPoint === "VK_PIPELINE_BIND_POINT_COMPUTE") {
          this.computePipelinesBound++;
        }
        const d = db.getObject(id)?.descriptor;
        const stages = d ? (Array.isArray(d.pStages) ? d.pStages : isObject(d.stage) ? [d.stage] : []) : [];
        for (const s of stages) {
          if (!isObject(s)) continue;
          const stage = str(s.stage);
          if (stage === "VK_SHADER_STAGE_VERTEX_BIT") this.vertexStages++;
          else if (stage === "VK_SHADER_STAGE_FRAGMENT_BIT") this.fragmentStages++;
          else if (stage === "VK_SHADER_STAGE_COMPUTE_BIT") this.computeStages++;
        }
      } else if (method === "vkCmdBindVertexBuffers" || method === "vkCmdBindVertexBuffers2" || method === "vkCmdBindVertexBuffers2EXT") {
        this.bindVertexBuffers++;
      } else if (method === "vkCmdBindIndexBuffer" || method === "vkCmdBindIndexBuffer2" || method === "vkCmdBindIndexBuffer2KHR") {
        this.bindIndexBuffer++;
      } else if (method === "vkCmdPushConstants" || method === "vkCmdPushConstants2" || method === "vkCmdPushConstants2KHR") {
        this.pushConstants++;
        const info = a && isObject(a.pPushConstantsInfo) ? a.pPushConstantsInfo : a;
        this.pushConstantBytes += num(info?.size);
      }

      if (cmd.descriptors) {
        this.bindDescriptorSets++;
        for (const set of cmd.descriptors.sets) {
          this.descriptorSetsBound++;
          const id = refId(set.descriptorSet);
          if (id !== null) sets.add(id);
          for (const b of set.bindings) {
            const written = b.descriptors.filter((d) => d).length;
            const t = b.type;
            if (t.includes("UNIFORM_BUFFER")) this.uniformBuffers += written;
            else if (t.includes("STORAGE_BUFFER")) this.storageBuffers += written;
            else if (t.includes("TEXEL_BUFFER")) this.texelBuffers += written;
            else if (t === "VK_DESCRIPTOR_TYPE_SAMPLER") this.samplers += written;
            else if (t === "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER") { this.images += written; this.samplers += written; }
            else if (t.includes("IMAGE") || t.includes("INPUT_ATTACHMENT")) this.images += written;
          }
        }
      }
    }
    this.commandBuffers = commandBuffers.size;
    this.secondaryCommandBuffers = secondaries.size;
    this.uniquePipelines = pipelines.size;
    this.uniqueDescriptorSets = sets.size;
    this.renderTargetsCaptured = data.textures.filter((t) => !t.info.error).length;
    for (const b of data.buffers.values()) {
      if (b.info.error) continue;
      this.capturedBuffers++;
      this.capturedBufferBytes += b.info.size;
    }
    return this;
  }

  private _geometry(cmd: CaptureCommand, data: CaptureData, pipeline: VulkanObject | null): void {
    const a = cmd.args;
    if (!a) return;
    let vertices = 0;
    let instances = 0;
    switch (cmd.method) {
      case "vkCmdDraw":
        vertices = num(a.vertexCount) * Math.max(1, num(a.instanceCount));
        instances = num(a.instanceCount);
        break;
      case "vkCmdDrawIndexed":
        vertices = num(a.indexCount) * Math.max(1, num(a.instanceCount));
        instances = num(a.instanceCount);
        break;
      case "vkCmdDrawMultiEXT":
      case "vkCmdDrawMultiIndexedEXT": {
        const infos = Array.isArray(a.pVertexInfo) ? a.pVertexInfo : Array.isArray(a.pIndexInfo) ? a.pIndexInfo : [];
        let count = 0;
        for (const i of infos) if (isObject(i)) count += num(i.vertexCount ?? i.indexCount);
        vertices = count * Math.max(1, num(a.instanceCount));
        instances = num(a.instanceCount) * infos.length;
        break;
      }
      case "vkCmdDrawIndirect":
      case "vkCmdDrawIndexedIndirect": {
        this.indirectDraws++;
        const buf = data.buffer(cmd.bufferData?.[0]);
        if (!buf?.data) return;
        const view = new DataView(buf.data.buffer, buf.data.byteOffset, buf.data.byteLength);
        const stride = Math.max(16, num(a.stride));
        const count = num(a.drawCount);
        for (let i = 0; i < count; i++) {
          const at = i * stride;
          if (at + 8 > view.byteLength) break;
          const n = view.getUint32(at, true);
          const inst = view.getUint32(at + 4, true);
          vertices += n * Math.max(1, inst);
          instances += inst;
        }
        break;
      }
      default:
        if (cmd.method.includes("Indirect")) this.indirectDraws++;
        return;
    }
    this.totalInstances += instances;
    this.totalVertices += vertices;
    const d = pipeline?.descriptor;
    const ia = d && isObject(d.pInputAssemblyState) ? d.pInputAssemblyState : null;
    const topology = ia ? str(ia.topology) : "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST";
    const perInstance = instances > 0 ? vertices / instances : vertices;
    const n = Math.max(0, perInstance);
    const scale = Math.max(1, instances);
    switch (topology) {
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST": this.totalTriangles += Math.floor(n / 3) * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP":
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN": this.totalTriangles += Math.max(0, n - 2) * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY": this.totalTriangles += Math.floor(n / 6) * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY": this.totalTriangles += Math.max(0, Math.floor((n - 4) / 2)) * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_LINE_LIST": this.totalLines += Math.floor(n / 2) * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP": this.totalLines += Math.max(0, n - 1) * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY": this.totalLines += Math.floor(n / 4) * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY": this.totalLines += Math.max(0, n - 3) * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_POINT_LIST": this.totalPoints += n * scale; break;
      case "VK_PRIMITIVE_TOPOLOGY_PATCH_LIST": {
        const ts = d && isObject(d.pTessellationState) ? d.pTessellationState : null;
        const points = Math.max(1, num(ts?.patchControlPoints ?? 3));
        this.totalPatches += Math.floor(n / points) * scale;
        break;
      }
      default: break;
    }
  }

  private _attachments(cmd: CaptureCommand, db: ObjectDatabase): void {
    const a = cmd.args;
    if (!a) return;
    if (isObject(a.pRenderingInfo)) {
      const r = a.pRenderingInfo;
      this.colorAttachments += Array.isArray(r.pColorAttachments) ? r.pColorAttachments.length : 0;
      if (isObject(r.pDepthAttachment) || isObject(r.pStencilAttachment)) this.depthStencilAttachments++;
      return;
    }
    if (isObject(a.pRenderPassBegin)) {
      const rp = db.getObject(refId(a.pRenderPassBegin.renderPass))?.descriptor;
      const atts = rp && Array.isArray(rp.pAttachments) ? rp.pAttachments : [];
      for (const att of atts) {
        if (!isObject(att)) continue;
        if (/_D\d+_|_D\d+$|_S8_UINT/.test(fmt(att.format))) this.depthStencilAttachments++;
        else this.colorAttachments++;
      }
    }
  }

  private _memory(cmd: CaptureCommand, db: ObjectDatabase): void {
    const a = cmd.args;
    if (!a) return;
    switch (cmd.method) {
      case "vkCmdUpdateBuffer":
        this.updateBuffer++;
        this.updateBufferBytes += num(a.dataSize);
        break;
      case "vkCmdFillBuffer": {
        let size = num(a.size);
        if (size === 0 || size > 1e15) {
          const buf = db.getObject(refId(a.dstBuffer))?.descriptor;
          size = Math.max(0, num(buf?.size) - num(a.offset));
        }
        this.fillBufferBytes += size;
        break;
      }
      case "vkCmdCopyBuffer":
      case "vkCmdCopyBuffer2":
      case "vkCmdCopyBuffer2KHR": {
        const info = isObject(a.pCopyBufferInfo) ? a.pCopyBufferInfo : a;
        const regions = Array.isArray(info.pRegions) ? info.pRegions : [];
        for (const r of regions) if (isObject(r)) this.bufferCopyBytes += num(r.size);
        break;
      }
      default:
        break;
    }
  }

  /** The non-empty sections, in WebGPU Inspector's order. */
  sections(): StatSection[] {
    const s = (title: string, rows: StatRow[]): StatSection | null => (rows.some((r) => r.value) ? { title, rows } : null);
    const out = [
      s("API Activity", [
        { label: "Frames", value: this.frames }, { label: "Commands", value: this.apiCalls }, { label: "Queue submits", value: this.submits },
        { label: "Command buffers", value: this.commandBuffers }, { label: "Secondary command buffers", value: this.secondaryCommandBuffers },
        { label: "Draws", value: this.draws }, { label: "Indexed draws", value: this.indexedDraws }, { label: "Indirect draws", value: this.indirectDraws },
        { label: "Mesh draws", value: this.meshDraws }, { label: "Dispatches", value: this.dispatches }, { label: "Ray tracing launches", value: this.traceRays },
        { label: "Copy / clear commands", value: this.copyCommands }, { label: "Barriers and events", value: this.barriers }, { label: "Debug labels", value: this.debugLabels },
      ]),
      s("Passes", [
        { label: "Render passes", value: this.renderPasses }, { label: "Color attachments", value: this.colorAttachments },
        { label: "Depth / stencil attachments", value: this.depthStencilAttachments }, { label: "Render targets read back", value: this.renderTargetsCaptured },
      ]),
      s("Pipeline", [
        { label: "Bind pipeline calls", value: this.bindPipeline }, { label: "Graphics pipelines bound", value: this.graphicsPipelinesBound },
        { label: "Compute pipelines bound", value: this.computePipelinesBound }, { label: "Distinct pipelines", value: this.uniquePipelines },
        { label: "Vertex stages", value: this.vertexStages }, { label: "Fragment stages", value: this.fragmentStages }, { label: "Compute stages", value: this.computeStages },
      ]),
      s("Bindings", [
        { label: "Bind descriptor set calls", value: this.bindDescriptorSets }, { label: "Descriptor sets bound", value: this.descriptorSetsBound },
        { label: "Distinct descriptor sets", value: this.uniqueDescriptorSets }, { label: "Uniform buffers", value: this.uniformBuffers },
        { label: "Storage buffers", value: this.storageBuffers }, { label: "Texel buffers", value: this.texelBuffers }, { label: "Images", value: this.images },
        { label: "Samplers", value: this.samplers }, { label: "Bind vertex buffers", value: this.bindVertexBuffers }, { label: "Bind index buffer", value: this.bindIndexBuffer },
        { label: "Push constant updates", value: this.pushConstants }, { label: "Push constant bytes", value: this.pushConstantBytes, bytes: true },
      ]),
      s("Memory", [
        { label: "Update buffer calls", value: this.updateBuffer }, { label: "Update buffer bytes", value: this.updateBufferBytes, bytes: true },
        { label: "Fill buffer bytes", value: this.fillBufferBytes, bytes: true }, { label: "Buffer copy bytes", value: this.bufferCopyBytes, bytes: true },
        { label: "Buffers read back", value: this.capturedBuffers }, { label: "Buffer bytes read back", value: this.capturedBufferBytes, bytes: true },
      ]),
      s("Geometry", [
        { label: "Instances", value: this.totalInstances }, { label: "Vertices", value: this.totalVertices }, { label: "Triangles", value: this.totalTriangles },
        { label: "Lines", value: this.totalLines }, { label: "Points", value: this.totalPoints }, { label: "Patches", value: this.totalPatches },
      ]),
    ];
    return out.filter((x): x is StatSection => x !== null);
  }
}

/** GPU pass timings of a capture (Profile passes) with the live frame and submit times for the Frame Bound card. */
export interface FrameTimingInfo {
  frameMs: number;       // live frame interval (the budget, as no refresh rate is known)
  submitMs: number;      // CPU time per frame inside vkQueueSubmit
  gpuSpanMs: number;     // first pass start to last pass end
  gpuTotalMs: number;    // sum of pass durations
  frames: number;
  passes: { label: string; durationMs: number; startMs: number; onJump: () => void }[];
}

/**
 * "Frame Bound" card: compares the GPU span of the captured passes and the CPU submit time
 * against the frame interval and names the likely bottleneck, like WebGPU Inspector's card.
 */
function renderFrameBound(root: Widget, t: FrameTimingInfo): void {
  const budget = t.frameMs > 0 ? t.frameMs : Math.max(t.gpuSpanMs, t.submitMs);
  if (!(budget > 0)) return;
  const gpu = t.frames > 1 ? t.gpuSpanMs / t.frames : t.gpuSpanMs;
  let verdict: string;
  let cls: string;
  if (gpu / budget > 0.8) {
    verdict = "GPU bound";
    cls = "frame-bound-gpu";
  } else if (t.submitMs / budget > 0.8) {
    verdict = "CPU bound (submit)";
    cls = "frame-bound-cpu";
  } else {
    verdict = "Present / CPU bound outside submit: the GPU has headroom";
    cls = "frame-bound-idle";
  }
  const card = new Div(root, { class: "frame-stats-section" });
  new Div(card, { text: "Frame Bound", class: "frame-stats-heading" });
  const body = new Div(card, { class: "frame-stats-list" });
  new Div(body, { text: verdict, class: `frame-bound-verdict ${cls}` });
  const bar = (label: string, ms: number, color: string): void => {
    const row = new Div(body, { class: "frame-bound-row" });
    new Div(row, { text: label, class: "frame-bound-label" });
    const track = new Div(row, { class: "frame-bound-track" });
    const fill = new Div(track, { class: "frame-bound-fill" });
    fill.style.width = `${Math.min(100, (ms / budget) * 100).toFixed(1)}%`;
    fill.style.background = color;
    new Div(row, { text: `${ms.toFixed(2)} ms  (${((ms / budget) * 100).toFixed(0)}% of ${budget.toFixed(2)} ms)`, class: "frame-bound-value" });
  };
  bar("GPU (pass span)", gpu, "#4a8db8");
  bar("CPU (submit)", t.submitMs, "#5fd08a");
  new Div(body, { text: "The budget is the live frame interval (no display refresh rate is known). GPU time is the span of this capture's timed passes; CPU is the time inside vkQueueSubmit, so work outside submission counts as headroom here.", class: "text-muted font-sm" });
}

function renderPassTimings(root: Widget, t: FrameTimingInfo): void {
  const card = new Div(root, { class: "frame-stats-section" });
  new Div(card, { text: `Pass Timings (${t.passes.length} passes, ${t.gpuTotalMs.toFixed(3)} ms GPU, ${t.gpuSpanMs.toFixed(3)} ms span)`, class: "frame-stats-heading" });
  const list = new Div(card, { class: "frame-stats-list" });
  const shown = t.passes.slice(0, 40);
  for (const p of shown) {
    const row = new Div(list, { class: "frame-stats-row frame-stats-pass" });
    new Div(row, { text: p.label, class: "frame-stats-label" });
    const pct = t.gpuTotalMs > 0 ? (p.durationMs / t.gpuTotalMs) * 100 : 0;
    new Div(row, { text: `${p.durationMs.toFixed(3)} ms  (${pct.toFixed(1)}%)`, class: "frame-stats-value" });
    row.element.onclick = p.onJump;
    row.element.title = "Jump to the pass in the command list";
  }
  if (t.passes.length > shown.length) new Div(list, { text: `... ${t.passes.length - shown.length} more`, class: "text-muted font-sm" });
}

/** Renders the statistics as WebGPU Inspector's Frame Stats view: one card per section. */
export function renderFrameStats(container: Widget, stats: CaptureStatistics, timing: FrameTimingInfo | null = null): void {
  const root = new Div(container, { class: "frame-stats" });
  new Div(root, { text: "Frame Statistics", class: "frame-stats-title" });
  if (stats.frames > 1) new Div(root, { text: `Totals over ${stats.frames} captured frames.`, class: "text-muted font-sm" });
  if (timing) {
    renderFrameBound(root, timing);
    renderPassTimings(root, timing);
  }
  for (const section of stats.sections()) {
    const card = new Div(root, { class: "frame-stats-section" });
    new Div(card, { text: section.title, class: "frame-stats-heading" });
    const list = new Div(card, { class: "frame-stats-list" });
    for (const row of section.rows) {
      if (!row.value && row.label !== "Frames") continue;
      const line = new Div(list, { class: "frame-stats-row" });
      new Div(line, { text: row.label, class: "frame-stats-label" });
      new Div(line, { text: row.bytes ? `${formatBytes(row.value)} (${row.value.toLocaleString()})` : row.value.toLocaleString(), class: "frame-stats-value" });
    }
  }
}
