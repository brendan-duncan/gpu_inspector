// Details of the command selected in the capture panel: for a draw or dispatch, the bound
// pipeline and its shaders, every descriptor set with its resources and the parsed contents of
// its uniform / storage buffers, the vertex and index buffers decoded by the vertex layout, the
// push constants, and the pass's render targets. Binding commands show the same for what they
// bind. This is the counterpart of WebGPU Inspector's _showCaptureCommandInfo_* family
// (capture_panel.js, MIT).
import { Button } from "./widget/button.js";
import { collapsible } from "./widget/collapsible.js";
import { Dialog } from "./widget/dialog.js";
import { Div } from "./widget/div.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import { TextArea } from "./widget/text_area.js";
import { Widget } from "./widget/widget.js";
import { objectLink, renderArgs } from "./args_view.js";
import { renderIndexData, renderTypedData, type Radix } from "./buffer_data_view.js";
import { decodeBase64 } from "./utils/base64.js";
import { layoutText, parseLayout, type LayoutRules } from "./vulkan/buffer_layout.js";
import { isAction, type CommandSets } from "./command_sets.js";
import { decodeImage } from "./vulkan/texture_decode.js";
import {
  typeName, type ReflType, type ShaderReflection, type ShaderResource, type ShaderStage, type StructMember, type StructType,
} from "./vulkan/spirv_reflect.js";
import { vertexFormat } from "./vulkan/vk_format.js";
import { fmt, fmtFlags, formatBytes, isObject, num, refId, str, type VulkanObject } from "./vulkan/vulkan_object.js";
import { stageLabel, type StageSource } from "./shader_cache.js";
import { kindLabel, renderReflection } from "./shader_reflection_view.js";
import { severityMark, validationItemText, worstSeverity } from "./validation_text.js";
import type { FrameFinding } from "./vulkan/frame_analysis.js";
import { renderCommandStack } from "./stacktrace_view.js";
import { renderEmbeddedSource } from "./shader_source_view.js";
import { renderAnalysisSection, renderCostSection } from "./shader_analysis_view.js";
import { analyzeSpirvCached } from "./vulkan/spirv_analysis.js";
import { fetchBlob } from "./capture_file.js";
import type { CaptureData, CapturedBuffer, CapturedTexture } from "./capture_data.js";
import { ImageView } from "./image_view.js";
import type { SessionContext } from "./session_panel.js";
import type { ObjectDatabase } from "./vulkan/object_database.js";
import type {
  ArgObject, ArgValue, CaptureCommand, CaptureDescriptor, CaptureDescriptorBinding, CaptureDescriptorSet, ImageDataMessage,
} from "../shared/protocol.js";

// ---------------------------------------------------------------------------------------------
// Bound state

export interface BoundSet { cmd: CaptureCommand; set: CaptureDescriptorSet }

export interface BoundVertexBuffer {
  cmd: CaptureCommand;
  binding: number;
  buffer: ArgValue;
  offset: number;
  size: number | null;     // vkCmdBindVertexBuffers2 pSizes
  stride: number | null;   // vkCmdBindVertexBuffers2 pStrides
  dataId: number;
}

export interface BoundIndexBuffer {
  cmd: CaptureCommand;
  buffer: ArgValue;
  offset: number;
  indexType: string;
  dataId: number;
}

export interface PushConstantUpdate {
  cmd: CaptureCommand;
  stageFlags: string;
  offset: number;
  size: number;
  data: Uint8Array | null;
}

/** State in effect at a command, reconstructed by walking back through its command buffer. */
export interface DrawState {
  bindPoint: string;
  pipelineCmd: CaptureCommand | null;
  pipeline: VulkanObject | null;
  sets: Map<number, BoundSet>;
  vertexBuffers: Map<number, BoundVertexBuffer>;
  indexBuffer: BoundIndexBuffer | null;
  /** vkCmdSetVertexInputEXT arguments when the vertex layout is dynamic. */
  vertexInput: ArgObject | null;
  viewports: ArgValue | null;
  scissors: ArgValue | null;
  pushConstants: PushConstantUpdate[];   // in recording order
}

/** What the details view needs from the capture it belongs to (a CaptureView). */
/** Testing aid (--debug-expand-stacks): open Stack trace sections as soon as a command is shown. */
export let debugExpandStacks = false;
export function setDebugExpandStacks(on: boolean): void {
  debugExpandStacks = on;
}

export interface CaptureHost {
  readonly window: SessionContext;
  readonly data: CaptureData;
  renderPassTargets(container: Widget, frame: number, passBegin: CaptureCommand, passIndex: number, commandBufferId: number): void;
  /** A canvas showing a captured texture, drawn when its data is (or becomes) available. */
  textureCanvas(tex: CapturedTexture, className: string): HTMLCanvasElement;
  /** Selects a command of the list by its index (scrolls to it and shows its details). */
  selectCommand(index: number): void;
  /** The frame analysis findings that apply to a command. */
  frameFindings(cmd: CaptureCommand): FrameFinding[];
  /** Shows Frame Stats (the Frame Issues card) in the details pane. */
  showFrameStats(): void;
}

/** Commands that write a buffer through a transfer, and the argument naming the destination. */
const BUFFER_WRITE_METHODS: Record<string, (a: ArgObject) => ArgValue | undefined> = {
  vkCmdCopyBuffer: (a) => a.dstBuffer,
  vkCmdCopyBuffer2: (a) => (isObject(a.pCopyBufferInfo) ? a.pCopyBufferInfo.dstBuffer : undefined),
  vkCmdCopyBuffer2KHR: (a) => (isObject(a.pCopyBufferInfo) ? a.pCopyBufferInfo.dstBuffer : undefined),
  vkCmdCopyImageToBuffer: (a) => a.dstBuffer,
  vkCmdCopyImageToBuffer2: (a) => (isObject(a.pCopyImageToBufferInfo) ? a.pCopyImageToBufferInfo.dstBuffer : undefined),
  vkCmdCopyImageToBuffer2KHR: (a) => (isObject(a.pCopyImageToBufferInfo) ? a.pCopyImageToBufferInfo.dstBuffer : undefined),
  vkCmdUpdateBuffer: (a) => a.dstBuffer,
  vkCmdFillBuffer: (a) => a.dstBuffer,
  vkCmdCopyQueryPoolResults: (a) => a.dstBuffer,
};

interface StageReflection { source: StageSource; reflection: ShaderReflection | null }

interface FormatOverride { type: StructType; radix: Radix; rules: LayoutRules }

const INDIRECT_TYPES: Record<string, StructType> = {
  vkCmdDrawIndirect: struct("VkDrawIndirectCommand", ["vertexCount", "instanceCount", "firstVertex", "firstInstance"]),
  vkCmdDrawIndexedIndirect: struct("VkDrawIndexedIndirectCommand", ["indexCount", "instanceCount", "firstIndex", "vertexOffset", "firstInstance"]),
  vkCmdDispatchIndirect: struct("VkDispatchIndirectCommand", ["x", "y", "z"]),
};

function struct(name: string, fields: string[]): StructType {
  return {
    kind: "struct", name, size: fields.length * 4,
    members: fields.map((f, i): StructMember => ({ name: f, offset: i * 4, type: { kind: "scalar", base: f === "vertexOffset" ? "int" : "uint", width: 32, size: 4 } })),
  };
}

function pushConstantBytes(a: ArgObject | null): Uint8Array | null {
  const v = a && isObject(a.pValues) ? a.pValues : null;
  if (!v || typeof v.base64 !== "string") return null;
  try {
    return decodeBase64(v.base64);
  } catch {
    return null;
  }
}

function sameStream(cmdSets: CommandSets, cmd: CaptureCommand, c: CaptureCommand): boolean {
  if (c.object?.__id !== cmd.object?.__id) return false;
  if (cmdSets.SUBMIT.has(c.method)) return false;
  return true;
}

export class CommandInfoView {
  readonly panel: CaptureHost;
  private _token = 0;
  private _formats = new Map<string, FormatOverride>();
  private _thumbs = new Map<number, HTMLCanvasElement[]>();
  private _thumbRequested = new Set<number>();
  private _radix: Radix = 10;
  /** The command being shown (for "Affected by": what wrote a buffer before it). */
  private _current: CaptureCommand | null = null;

  constructor(panel: CaptureHost) {
    this.panel = panel;
  }

  private get db(): ObjectDatabase {
    return this.panel.window.database;
  }

  private _link = (o: VulkanObject): void => this.panel.window.showObject(o.id);

  // ---------------------------------------------------------------------------------------
  // Entry point

  show(container: Div, cmd: CaptureCommand): void {
    const cmdSets = this.panel.data.sets;
    container.html = "";
    this._thumbs.clear();
    this._thumbRequested.clear();
    this._current = cmd;
    const token = ++this._token;
    const db = this.db;
    const method = cmd.method;

    const box = new Div(container, { class: "info-box info-box-success" });
    new Div(box, { text: method, class: "font-lg" });
    const obj = db.getObject(cmd.object?.__id);
    if (obj) {
      const row = new Div(box, { class: "font-md text-muted" });
      new Span(row, { text: "Object:", style: "margin-right: 4px;" });
      objectLink(row, obj, this._link);
    }
    if (cmd.result) new Div(box, { text: `Result: ${cmd.result}`, class: "font-md text-muted" });
    if (cmd.secondary) {
      const sec = db.getObject(cmd.secondary);
      const row = new Div(box, { class: "font-md text-muted" });
      new Span(row, { text: "Recorded in secondary command buffer:", style: "margin-right: 4px;" });
      if (sec) objectLink(row, sec, this._link); else new Span(row, { text: String(cmd.secondary) });
    }
    this._renderValidation(box, cmd);
    this._renderFindings(box, cmd);
    if (cmd.stack && cmd.stack.length) {
      const stackGrp = new collapsible(box, { label: "Stack trace", collapsed: !debugExpandStacks, class: "stack-group" });
      let loaded = false;
      if (debugExpandStacks) setTimeout(() => stackGrp.onExpanded.emit(), 0);
      stackGrp.onExpanded.addListener(() => {
        if (loaded) return;
        loaded = true;
        void renderCommandStack(stackGrp.body, this.panel.window, cmd.stack ?? []);
      });
    }

    if (isAction(cmdSets, method)) {
      const state = this.drawState(cmd);
      const graphics = state.bindPoint === "VK_PIPELINE_BIND_POINT_GRAPHICS";
      this._renderPipelineState(container, state);
      this._renderShaders(container, state.pipeline, token);
      this._renderDescriptorSets(container, state, [...state.sets.values()].sort((a, b) => a.set.set - b.set.set), token);
      if (graphics) {
        this._renderVertexBuffers(container, state, [...state.vertexBuffers.values()].sort((a, b) => a.binding - b.binding), token);
        if (state.indexBuffer) this._renderIndexBuffer(container, state.indexBuffer, cmd);
      }
      if (cmdSets.INDIRECT.has(method)) this._renderIndirect(container, cmd);
      this._renderPushConstants(container, state, state.pushConstants, token);
      if (cmdSets.DRAW.has(method)) this._renderTargets(container, cmd);
    } else if (cmdSets.BIND_PIPELINE.has(method)) {
      const pipeline = db.getObject(refId(cmd.args?.pipeline));
      const state = this._emptyState(cmdSets.pipelineBindPointOf(method, cmd.args));
      state.pipelineCmd = cmd;
      state.pipeline = pipeline;
      this._renderPipelineState(container, state);
      this._renderShaders(container, pipeline, token);
    } else if (cmdSets.BIND_DESCRIPTOR.has(method) && cmd.descriptors) {
      const state = this._stateFor(cmd, cmd.descriptors.bindPoint);
      this._renderDescriptorSets(container, state, cmd.descriptors.sets.map((set) => ({ cmd, set })), token);
    } else if (cmdSets.BIND_VERTEX.has(method)) {
      const state = this._stateFor(cmd, "VK_PIPELINE_BIND_POINT_GRAPHICS");
      this._renderVertexBuffers(container, state, this._vertexBuffersOf(cmd), token);
    } else if (cmdSets.BIND_INDEX.has(method)) {
      const ib = this._indexBufferOf(cmd);
      if (ib) this._renderIndexBuffer(container, ib, null);
    } else if (cmdSets.PUSH_CONSTANT.has(method)) {
      const pc = this._pushConstantOf(cmd);
      const state = this._stateFor(cmd, pc && pc.stageFlags.includes("COMPUTE") ? "VK_PIPELINE_BIND_POINT_COMPUTE" : "VK_PIPELINE_BIND_POINT_GRAPHICS");
      if (pc) this._renderPushConstants(container, state, [pc], token);
    } else if (cmdSets.PASS_BEGIN.has(method) || cmdSets.PASS_END.has(method)) {
      this._renderTargets(container, cmd);
    }

    const argsGrp = new collapsible(container, { label: "Arguments", collapsed: false });
    renderArgs(new Div(argsGrp.body, { class: "args-tree" }), cmd.args, db, this._link);

    if (cmd.children) {
      const grp = new collapsible(container, { label: `Secondary command buffers (${cmd.children.length})`, collapsed: true });
      for (const child of cmd.children) {
        const cbObj = db.getObject(child.commandBuffer);
        const sub = new collapsible(grp.body, { label: `${cbObj?.name ?? child.commandBuffer}: ${child.commands.length} commands`, collapsed: child.commands.length > 50 });
        for (const c of child.commands) {
          const row = new Div(sub.body, { class: "capture_command" });
          new Span(row, { text: c.method.replace(/^vk(Cmd)?/, ""), class: "capture_methodName" });
        }
      }
    }
  }

  /** Live image data for the thumbnails of images bound in descriptor sets. */
  handleImageData(msg: ImageDataMessage): void {
    const canvases = this._thumbs.get(msg.id);
    if (!canvases) return;
    for (const canvas of canvases) {
      if (msg.error || !msg.__binary) {
        canvas.title = msg.error ?? "no data";
        continue;
      }
      const rgba = decodeImage(msg, msg.__binary);
      if (!rgba) {
        canvas.title = `${msg.format}: display of this format is not supported yet`;
        continue;
      }
      canvas.width = msg.width;
      canvas.height = msg.height;
      canvas.getContext("2d")!.putImageData(new ImageData(rgba, msg.width, msg.height), 0, 0);
      canvas.classList.add("loaded");
    }
  }

  // ---------------------------------------------------------------------------------------
  // State reconstruction

  private _emptyState(bindPoint: string): DrawState {
    return {
      bindPoint, pipelineCmd: null, pipeline: null, sets: new Map(), vertexBuffers: new Map(), indexBuffer: null,
      vertexInput: null, viewports: null, scissors: null, pushConstants: [],
    };
  }

  /**
   * Walks back from a command through its command buffer, collecting the state bound before it.
   * Commands inlined from a secondary command buffer see only that buffer's own commands (a
   * secondary starts with no state); commands of a primary skip the inlined ones.
   */
  drawState(cmd: CaptureCommand, bindPoint = this.panel.data.sets.bindPointOf(cmd.method)): DrawState {
    const cmdSets = this.panel.data.sets;
    const commands = this.panel.data.commands;
    const state = this._emptyState(bindPoint);
    for (let i = cmd.index - 1; i >= 0; i--) {
      const c = commands[i];
      if (!c || !sameStream(cmdSets, cmd, c)) break;
      if (cmd.secondary) {
        if (c.secondary !== cmd.secondary) break;
      } else if (c.secondary) {
        continue;
      }
      const a = c.args;
      if (!a) continue;
      if (cmdSets.BIND_PIPELINE.has(c.method)) {
        if (!state.pipelineCmd && cmdSets.pipelineBindPointOf(c.method, a) === bindPoint) {
          state.pipelineCmd = c;
          state.pipeline = this.db.getObject(refId(a.pipeline));
        }
        continue;
      }
      switch (c.method) {
        case "vkCmdBindVertexBuffers":
        case "vkCmdBindVertexBuffers2":
        case "vkCmdBindVertexBuffers2EXT":
          for (const vb of this._vertexBuffersOf(c)) {
            if (!state.vertexBuffers.has(vb.binding)) state.vertexBuffers.set(vb.binding, vb);
          }
          break;
        case "vkCmdBindIndexBuffer":
        case "vkCmdBindIndexBuffer2":
        case "vkCmdBindIndexBuffer2KHR":
          if (!state.indexBuffer) state.indexBuffer = this._indexBufferOf(c);
          break;
        case "vkCmdSetVertexInputEXT":
          if (!state.vertexInput) state.vertexInput = a;
          break;
        case "vkCmdSetViewport":
        case "vkCmdSetViewportWithCount":
        case "vkCmdSetViewportWithCountEXT":
          if (!state.viewports) state.viewports = a.pViewports ?? null;
          break;
        case "vkCmdSetScissor":
        case "vkCmdSetScissorWithCount":
        case "vkCmdSetScissorWithCountEXT":
          if (!state.scissors) state.scissors = a.pScissors ?? null;
          break;
        case "vkCmdPushConstants":
        case "vkCmdPushConstants2":
        case "vkCmdPushConstants2KHR": {
          const pc = this._pushConstantOf(c);
          if (pc) state.pushConstants.unshift(pc);
          break;
        }
        default:
          break;
      }
      if (c.descriptors && c.descriptors.bindPoint === bindPoint) {
        for (const set of c.descriptors.sets) {
          if (!state.sets.has(set.set)) state.sets.set(set.set, { cmd: c, set });
        }
      }
    }
    return state;
  }

  /** State for a binding command: what is bound before it, plus the pipeline bound next if none was bound before. */
  private _stateFor(cmd: CaptureCommand, bindPoint: string): DrawState {
    const cmdSets = this.panel.data.sets;
    const state = this.drawState(cmd, bindPoint);
    if (state.pipeline) return state;
    const commands = this.panel.data.commands;
    for (let i = cmd.index + 1; i < commands.length; i++) {
      const c = commands[i];
      if (!c || !sameStream(cmdSets, cmd, c)) break;
      if (cmd.secondary ? c.secondary !== cmd.secondary : c.secondary) {
        if (cmd.secondary) break;
        continue;
      }
      if (cmdSets.BIND_PIPELINE.has(c.method) &&
          cmdSets.pipelineBindPointOf(c.method, c.args) === bindPoint) {
        state.pipelineCmd = c;
        state.pipeline = this.db.getObject(refId(c.args?.pipeline));
        break;
      }
    }
    return state;
  }

  private _vertexBuffersOf(c: CaptureCommand): BoundVertexBuffer[] {
    const a = c.args;
    if (!a || !Array.isArray(a.pBuffers)) return [];
    const first = num(a.firstBinding);
    return a.pBuffers.map((buffer, k) => ({
      cmd: c,
      binding: first + k,
      buffer,
      offset: Array.isArray(a.pOffsets) ? num(a.pOffsets[k]) : 0,
      size: Array.isArray(a.pSizes) && a.pSizes[k] ? num(a.pSizes[k]) : null,
      stride: Array.isArray(a.pStrides) ? num(a.pStrides[k]) : null,
      dataId: c.bufferData?.[k] ?? 0,
    }));
  }

  private _indexBufferOf(c: CaptureCommand): BoundIndexBuffer | null {
    const a = c.args;
    if (!a) return null;
    return { cmd: c, buffer: a.buffer, offset: num(a.offset), indexType: str(a.indexType), dataId: c.bufferData?.[0] ?? 0 };
  }

  private _pushConstantOf(c: CaptureCommand): PushConstantUpdate | null {
    let a = c.args;
    if (!a) return null;
    if (isObject(a.pPushConstantsInfo)) a = a.pPushConstantsInfo;
    return { cmd: c, stageFlags: str(a.stageFlags), offset: num(a.offset), size: num(a.size), data: pushConstantBytes(a) };
  }

  /** Pass containing (or begun / ended by) a command. */
  findPass(cmd: CaptureCommand): { passBegin: CaptureCommand; passIndex: number } | null {
    const cmdSets = this.panel.data.sets;
    const commands = this.panel.data.commands;
    let depth = 0;
    for (let i = cmd.index; i >= 0; i--) {
      const c = commands[i];
      if (!c || !sameStream(cmdSets, cmd, c)) break;
      if (c.secondary) continue;   // passes are begun and ended by the primary
      if (i !== cmd.index && cmdSets.PASS_END.has(c.method)) depth++;
      if (cmdSets.PASS_BEGIN.has(c.method)) {
        if (depth === 0) {
          let passIndex = 0;
          for (let j = i - 1; j >= 0; j--) {
            const p = commands[j];
            if (!p || !sameStream(cmdSets, cmd, p)) break;
            if (cmdSets.PASS_BEGIN.has(p.method)) passIndex++;
          }
          return { passBegin: c, passIndex };
        }
        depth--;
      }
    }
    return null;
  }

  // ---------------------------------------------------------------------------------------
  // Pipeline state

  private _renderPipelineState(container: Widget, state: DrawState): void {
    const db = this.db;
    const grp = new collapsible(container, { label: "Pipeline State", collapsed: false });
    const body = new Div(grp.body, { class: "draw-state" });
    const line = (label: string): Div => {
      const row = new Div(body, { class: "draw-state-row" });
      new Span(row, { text: label, class: "draw-state-label" });
      return row;
    };

    const pipeline = state.pipeline;
    const r = line("Pipeline");
    if (pipeline) objectLink(r, pipeline, this._link); else new Span(r, { text: "(none bound)", class: "text-muted" });
    new Span(r, { text: `  ${fmt(state.bindPoint)}`, class: "text-muted" });

    const d = pipeline?.descriptor;
    if (d) {
      const stages = Array.isArray(d.pStages) ? d.pStages : (isObject(d.stage) ? [d.stage] : []);
      for (const s of stages) {
        if (!isObject(s)) continue;
        const module = db.getObject(refId(s.module));
        const row = line(`  ${fmt(s.stage)}`);
        if (module) objectLink(row, module, this._link); else new Span(row, { text: "(inline code)", class: "text-muted" });
        new Span(row, { text: ` ${str(s.pName)}`, class: "text-muted" });
      }
      const layout = db.getObject(refId(d.layout));
      if (layout) objectLink(line("Layout"), layout, this._link);
      const ia = isObject(d.pInputAssemblyState) ? d.pInputAssemblyState : null;
      if (ia) new Span(line("Topology"), { text: `${fmt(ia.topology)}${ia.primitiveRestartEnable ? " (primitive restart)" : ""}` });
      const rs = isObject(d.pRasterizationState) ? d.pRasterizationState : null;
      if (rs) new Span(line("Raster"), { text: `${fmt(rs.polygonMode)} cull ${fmt(rs.cullMode)} ${fmt(rs.frontFace)}${rs.rasterizerDiscardEnable ? " discard" : ""}${rs.depthBiasEnable ? " depth bias" : ""}` });
      const ms = isObject(d.pMultisampleState) ? d.pMultisampleState : null;
      if (ms && ms.rasterizationSamples !== "VK_SAMPLE_COUNT_1_BIT") new Span(line("Samples"), { text: fmt(ms.rasterizationSamples) });
      const ds = isObject(d.pDepthStencilState) ? d.pDepthStencilState : null;
      if (ds) {
        new Span(line("Depth"), { text: `test ${ds.depthTestEnable ? "on" : "off"} write ${ds.depthWriteEnable ? "on" : "off"} ${fmt(ds.depthCompareOp)}${ds.depthBoundsTestEnable ? " bounds" : ""}` });
        if (ds.stencilTestEnable) {
          const f = isObject(ds.front) ? ds.front : {};
          new Span(line("Stencil"), { text: `on  front ${fmt(f.compareOp)} pass ${fmt(f.passOp)} fail ${fmt(f.failOp)} ref ${num(f.reference)} mask ${num(f.compareMask)}/${num(f.writeMask)}` });
        }
      }
      const cb = isObject(d.pColorBlendState) ? d.pColorBlendState : null;
      if (cb && Array.isArray(cb.pAttachments)) {
        cb.pAttachments.forEach((att, i) => {
          if (!isObject(att)) return;
          const text = att.blendEnable
            ? `blend ${fmt(att.srcColorBlendFactor)} ${fmt(att.colorBlendOp)} ${fmt(att.dstColorBlendFactor)}, alpha ${fmt(att.srcAlphaBlendFactor)} ${fmt(att.alphaBlendOp)} ${fmt(att.dstAlphaBlendFactor)}`
            : "no blend";
          new Span(line(`Color ${i}`), { text: `${text}  write ${fmtFlags(att.colorWriteMask) || "0"}` });
        });
      }
      const dyn = isObject(d.pDynamicState) && Array.isArray(d.pDynamicState.pDynamicStates) ? d.pDynamicState.pDynamicStates : null;
      if (dyn && dyn.length) new Span(line("Dynamic"), { text: dyn.map((s) => fmt(s)).join(", "), class: "text-muted" });
    }

    if (Array.isArray(state.viewports) && isObject(state.viewports[0])) {
      const v = state.viewports[0];
      new Span(line("Viewport"), { text: `${num(v.x)},${num(v.y)} ${num(v.width)}x${num(v.height)} depth ${num(v.minDepth)}..${num(v.maxDepth)}` });
    }
    if (Array.isArray(state.scissors) && isObject(state.scissors[0])) {
      const s = state.scissors[0];
      const o = isObject(s.offset) ? s.offset : {};
      const e = isObject(s.extent) ? s.extent : {};
      new Span(line("Scissor"), { text: `${num(o.x)},${num(o.y)} ${num(e.width)}x${num(e.height)}` });
    }
  }

  // ---------------------------------------------------------------------------------------
  // Shaders

  private _renderShaders(container: Widget, pipeline: VulkanObject | null, token: number): void {
    if (!pipeline) return;
    const host = new Div(container);
    void this.panel.window.shaders.stages(pipeline).then((stages) => {
      if (token !== this._token) return;
      for (const { source, reflection } of stages) this._renderShader(host, source, reflection);
    });
  }

  private _renderShader(container: Widget, source: StageSource, reflection: ShaderReflection | null): void {
    const target = source.module ?? source.object;
    const grp = new collapsible(container, { label: `${stageLabel(source.stage)} Shader: ${target.name}  ${source.entryPoint}`, collapsed: true });
    const body = new Div(grp.body, { class: "shader-info" });
    const row = new Div(body);
    new Span(row, { text: source.module ? "Module: " : "Code in: ", class: "text-muted" });
    objectLink(row, target, this._link);
    // The embedded source, the modeled cost and the findings come from the SPIR-V payload,
    // fetched when the group is first opened (from the layer, or the capture file).
    const details = new Div(body);
    let loaded = false;
    const load = async (): Promise<void> => {
      if (loaded) return;
      loaded = true;
      const status = new Div(details, { text: "Loading shader code...", class: "text-muted font-sm" });
      const data = await fetchBlob(this.panel.window, source.object, source.blobIndex);
      status.remove();
      if (!data) {
        new Div(details, { text: "Shader code not available.", class: "text-muted font-sm" });
        return;
      }
      const sourceGrp = new collapsible(details, { label: "Source", collapsed: false, class: "shader-source-section" });
      const view = renderEmbeddedSource(sourceGrp.body, data, this.panel.window);
      const analysis = analyzeSpirvCached(data);
      if (analysis) {
        const jump = (file: string | undefined, line: number): void => {
          const files = view.info?.files ?? [];
          let index = files.findIndex((f) => f.text !== null && f.name.replace(/^.*[\/]/, "") === file);
          if (index < 0) index = files.findIndex((f) => f.text !== null);
          if (index < 0) return;
          sourceGrp.expand();
          view.show(index, line);
        };
        renderCostSection(details, analysis, source.entryPoint, jump);
        renderAnalysisSection(details, analysis, jump);
      }
    };
    if (!grp.collapsed) void load(); else grp.onExpanded.addListener(() => void load());
    if (!reflection) {
      new Div(body, { text: "Shader code not available for reflection.", class: "text-muted" });
      return;
    }
    renderReflection(body, reflection, { entryPoint: source.entryPoint });
  }

  // ---------------------------------------------------------------------------------------
  // Descriptor sets

  private _renderDescriptorSets(container: Widget, state: DrawState, sets: BoundSet[], token: number): void {
    if (!sets.length) {
      new Div(container, { text: "No descriptor sets bound.", class: "text-muted capture-note" });
      return;
    }
    const host = new Div(container);
    const build = (stages: StageReflection[]): void => {
      for (const bound of sets) this._renderDescriptorSet(host, state, bound, stages);
    };
    if (state.pipeline) {
      void this.panel.window.shaders.stages(state.pipeline).then((stages) => {
        if (token === this._token) build(stages);
      });
    } else {
      build([]);
    }
  }

  private _renderDescriptorSet(container: Widget, state: DrawState, bound: BoundSet, stages: StageReflection[]): void {
    const db = this.db;
    const set = bound.set;
    const setObj = db.getObject(refId(set.descriptorSet));
    const pushed = !set.descriptorSet;
    const grp = new collapsible(container, { label: `Descriptor Set ${set.set}: ${pushed ? "push descriptors" : setObj?.name ?? "(destroyed)"}  (${set.bindings.length} bindings)`, collapsed: false });
    const head = new Div(grp.body, { class: "font-md text-muted descriptor-set-head" });
    if (setObj) {
      new Span(head, { text: "Set: " });
      objectLink(head, setObj, this._link);
    }
    const layout = db.getObject(refId(set.layout));
    if (layout) {
      new Span(head, { text: "  Layout: " });
      objectLink(head, layout, this._link);
    }
    if (bound.cmd !== state.pipelineCmd) {
      new Span(head, { text: `  bound by #${bound.cmd.index} ${bound.cmd.method.replace(/^vkCmd/, "")}`, class: "text-muted" });
    }
    if (!set.bindings.length) new Div(grp.body, { text: "Contents unknown (the set was not tracked).", class: "text-muted" });
    for (const binding of set.bindings) this._renderBinding(grp.body, state, set, binding, stages);
  }

  private _renderBinding(container: Widget, state: DrawState, set: CaptureDescriptorSet, binding: CaptureDescriptorBinding, stages: StageReflection[]): void {
    const db = this.db;
    const found = findResource(stages, set.set, binding.binding);
    const res = found?.resource ?? null;
    const shaderText = res ? `  ${res.name}: ${res.typeName}` : "";
    const count = binding.descriptors.length;
    const shown = Math.min(count, 32);
    for (let k = 0; k < shown; k++) {
      const d = binding.descriptors[k];
      const index = count > 1 ? `[${k}]` : "";
      let resourceText = "(not written)";
      let sizeText = "";
      if (d) {
        if (d.buffer !== undefined) {
          const buf = db.getObject(refId(d.buffer));
          resourceText = buf ? buf.name : "(destroyed buffer)";
          sizeText = `  ${formatBytes(num(d.range))}`;
        } else if (d.imageView !== undefined) {
          const view = db.getObject(refId(d.imageView));
          const image = db.getObject(refId(view?.descriptor?.image));
          resourceText = image ? image.name : view ? view.name : "(destroyed view)";
          if (d.sampler !== undefined) {
            const sampler = db.getObject(refId(d.sampler));
            resourceText += ` + ${sampler ? sampler.name : "(no sampler)"}`;
          }
        } else if (d.sampler !== undefined) {
          const sampler = db.getObject(refId(d.sampler));
          resourceText = sampler ? sampler.name : "(destroyed sampler)";
        } else if (d.bufferView !== undefined) {
          const bv = db.getObject(refId(d.bufferView));
          resourceText = bv ? bv.name : "(destroyed buffer view)";
        }
      }
      const isBuffer = !!d && d.buffer !== undefined;
      const grp = new collapsible(container, {
        label: `Binding ${binding.binding}${index}: ${fmt(binding.type)}  ${resourceText}${sizeText}${shaderText}`,
        collapsed: !isBuffer,
        class: "descriptor-binding",
      });
      if (!d) {
        new Div(grp.body, { text: "This descriptor was never written.", class: "text-muted" });
        continue;
      }
      if (binding.stages) new Div(grp.body, { text: `Stages: ${fmtFlags(binding.stages)}`, class: "text-muted font-sm" });
      if (isBuffer) this._renderBufferBinding(grp.body, state, set, binding, d, res);
      else this._renderImageBinding(grp, d);
    }
    if (shown < count) new Div(container, { text: `... ${count - shown} more descriptors in binding ${binding.binding}`, class: "text-muted capture-note" });
  }

  private _renderBufferBinding(body: Widget, state: DrawState, set: CaptureDescriptorSet, binding: CaptureDescriptorBinding, d: CaptureDescriptor, res: ShaderResource | null): void {
    const db = this.db;
    const buf = db.getObject(refId(d.buffer));
    const row = new Div(body, { class: "font-md" });
    new Span(row, { text: "Buffer: ", class: "text-muted" });
    if (buf) objectLink(row, buf, this._link); else new Span(row, { text: "(destroyed)" });
    const dyn = d.dynamicOffset !== undefined ? `  dynamic offset ${d.dynamicOffset}  (effective ${num(d.offset) + num(d.dynamicOffset)})` : "";
    new Span(row, { text: `  offset ${num(d.offset)}  range ${num(d.range)}${dyn}`, class: "text-muted" });
    if (buf?.descriptor) new Div(body, { text: `${formatBytes(num(buf.descriptor.size))}  ${fmtFlags(buf.descriptor.usage)}`, class: "text-muted font-sm" });
    if (buf) this._renderAffectedBy(body, buf.id);

    const captured = this.panel.data.buffer(d.data);
    const key = `${state.pipeline?.id ?? 0}:${set.set}:${binding.binding}`;
    const kind = binding.type.includes("STORAGE") ? "storage" : "uniform";
    if (captured && !captured.info.error && captured.info.size < num(d.range)) {
      const bufSize = num(buf?.descriptor?.size);
      new Div(body, { class: "inspect_info_error", text:
        `Capture is shorter than the bound range: captured ${captured.info.size} bytes at buffer offset ${captured.info.offset}, ` +
        `descriptor offset ${num(d.offset)} + dynamic offset ${num(d.dynamicOffset)}, range ${num(d.range)}, buffer size ${bufSize}, capture id ${captured.info.id}` });
    }
    this._renderBufferContents(body, key, kind, res, captured);
  }

  /**
   * "Affected by": the earlier commands of the frame that wrote the buffer (WebGPU Inspector's
   * list under a bind group's buffer): transfers naming it as their destination, and draws,
   * dispatches and ray tracing launches whose bound descriptor sets hold it as a storage buffer.
   */
  private _affectedBy(bufferId: number): CaptureCommand[] {
    const cmdSets = this.panel.data.sets;
    const current = this._current;
    if (!current) return [];
    const out: CaptureCommand[] = [];
    // Storage buffers bound per stream and bind point: stream -> bind point -> set index -> buffers.
    const bound = new Map<string, Map<string, Map<number, Set<number>>>>();
    for (const c of this.panel.data.commands) {
      if (!c || c.index >= current.index) break;
      if (c.frame !== current.frame || cmdSets.SUBMIT.has(c.method)) continue;
      const a = c.args;
      const writer = BUFFER_WRITE_METHODS[c.method];
      if (writer && a && refId(writer(a)) === bufferId) {
        out.push(c);
        continue;
      }
      const stream = `${c.object?.__id ?? 0}:${c.secondary ?? 0}`;
      if (c.descriptors) {
        let byPoint = bound.get(stream);
        if (!byPoint) bound.set(stream, (byPoint = new Map()));
        let sets = byPoint.get(c.descriptors.bindPoint);
        if (!sets) byPoint.set(c.descriptors.bindPoint, (sets = new Map()));
        for (const s of c.descriptors.sets) {
          const buffers = new Set<number>();
          for (const b of s.bindings) {
            if (!b.type.includes("STORAGE_BUFFER")) continue;
            for (const d of b.descriptors) {
              const id = refId(d?.buffer);
              if (id !== null) buffers.add(id);
            }
          }
          sets.set(s.set, buffers);
        }
        continue;
      }
      if (isAction(cmdSets, c.method)) {
        const sets = bound.get(stream)?.get(cmdSets.bindPointOf(c.method));
        if (!sets) continue;
        for (const buffers of sets.values()) {
          if (buffers.has(bufferId)) {
            out.push(c);
            break;
          }
        }
      }
    }
    return out;
  }

  /** The frame analysis findings that apply to the command (Frame Issues in Frame Stats). */
  private _renderFindings(box: Div, cmd: CaptureCommand): void {
    const findings = this.panel.frameFindings(cmd);
    if (!findings.length) return;
    const grp = new collapsible(box, { label: `Performance (${findings.length})`, collapsed: false });
    for (const f of findings) {
      const row = new Div(grp.body, { class: `perf-finding perf-row-${f.severity}` });
      const head = new Div(row, { class: "perf-finding-head" });
      new Span(head, { text: f.severity.toUpperCase(), class: `perf-badge perf-${f.severity}` });
      new Span(head, { text: f.rule, class: "perf-rule" });
      if (f.count > 1) new Span(head, { text: `\u00d7${f.count} in the frame`, class: "perf-count text-muted" });
      new Div(row, { text: f.message, class: "perf-msg" });
    }
    const link = new Div(grp.body, { text: "All frame issues in Frame Stats", class: "dependency_link font-sm" });
    link.element.onclick = () => this.panel.showFrameStats();
  }

  /** The validation messages that fired while the command was recorded; clicking one shows it in the Inspect tab. */
  private _renderValidation(box: Div, cmd: CaptureCommand): void {
    const msgs = this.db.validationForCommand(cmd.secondary ?? cmd.object?.__id, cmd.slot);
    if (!msgs.length) return;
    const sev = worstSeverity(msgs);
    box.classList.remove("info-box-success");
    box.classList.add(sev === "error" ? "info-box-error" : "info-box-warning");
    const grp = new collapsible(box, { label: `Validation (${msgs.length})`, collapsed: false });
    for (const v of msgs) {
      const row = new Div(grp.body, { class: "validation-object-row" });
      new Span(row, { text: `${severityMark(v.severity)} `, class: `validation-sev validation-sev-${v.severity}` });
      new Span(row, { text: validationItemText(v), class: "validation-text dependency_link" });
      if (v.count > 1) new Span(row, { text: ` \u00d7${v.count}`, class: "validation-count" });
      row.tooltip = v.message;
      row.element.onclick = () => this.panel.window.showValidation(v);
    }
  }

  private _renderAffectedBy(body: Widget, bufferId: number): void {
    const commands = this._affectedBy(bufferId);
    if (!commands.length) return;
    const grp = new collapsible(body, { label: `Affected by (${commands.length})`, collapsed: commands.length > 6, class: "affected-by" });
    const ul = new Widget("ul", grp.body, { class: "dependency-list affected-by-list" });
    for (const c of commands) {
      const li = new Widget("li", ul);
      const link = new Span(li, { text: `#${c.index} ${c.method.replace(/^vkCmd/, "")}`, class: "dependency_link" });
      const how = BUFFER_WRITE_METHODS[c.method] ? "transfer destination" : "bound as a storage buffer";
      new Span(li, { text: `  ${how}`, class: "text-muted font-sm" });
      link.element.onclick = () => this.panel.selectCommand(c.index);
    }
  }

  /** Header line, Format button and typed values of a captured buffer range. */
  private _renderBufferContents(body: Widget, key: string, kind: "uniform" | "storage", res: ShaderResource | null, captured: CapturedBuffer | null): void {
    if (!captured) {
      new Div(body, { text: this.panel.data.buffers.size ? "Contents were not captured." : "Contents not available (buffer capture disabled or not received).", class: "text-muted" });
      return;
    }
    if (captured.info.error) {
      new Div(body, { text: `Contents not captured: ${captured.info.error}`, class: "text-muted" });
      return;
    }
    if (!captured.data) {
      new Div(body, { text: "Loading contents...", class: "text-muted" });
      return;
    }
    const data = captured.data;
    const override = this._formats.get(key);
    const type: ReflType | null = override?.type ?? res?.type ?? null;
    const radix: Radix = override?.radix ?? this._radix;

    const head = new Div(body, { class: "buffer-head" });
    const label = res
      ? `${kindLabel(res.kind)}${res.kind === "storage" ? (res.readOnly ? " (read-only)" : "") : ""}: ${res.name || "(unnamed)"}: ${override ? override.type.name : res.typeName || "block"}`
      : `${kind === "storage" ? "STORAGE" : "UNIFORM"}: ${override ? override.type.name : "(no shader type: raw view)"}`;
    new Span(head, { text: label, class: "buffer-label" });
    const blockSize = type && type.kind !== "opaque" ? type.size : 0;
    new Span(head, { text: `  ${data.byteLength} bytes captured${captured.info.originalSize ? ` of ${captured.info.originalSize} (truncated to the capture limit)` : ""}${blockSize ? `, block is ${blockSize} bytes` : ""}`, class: "text-muted font-sm" });
    if (blockSize && data.byteLength < blockSize && !captured.info.originalSize) {
      new Div(body, { text: `The bound range (${captured.info.size} bytes at offset ${captured.info.offset}) covers only part of the declared ${blockSize}-byte block: the shader reads just the members inside it. The rest are listed at the end.`, class: "text-muted font-sm" });
    }
    const dataUi = new Div(body, { class: "buffer-data" });
    const bufferKind = kind;
    new Button(head, { label: "Format", class: "btn btn-sm buffer-format-button", tooltip: "Change how the bytes are interpreted", callback: () => {
      this._editFormat(key, type, bufferKind, (t, r) => {
        dataUi.html = "";
        this._renderData(dataUi, t, data, r);
      });
    } });
    this._renderData(dataUi, type, data, radix);
  }

  private _renderData(ui: Widget, type: ReflType | null, data: Uint8Array, radix: Radix): void {
    const list = new Widget("ul", ui, { class: "buffer-root" });
    if (type) {
      renderTypedData(list, type, data, 0, radix);
      return;
    }
    // No type: the words as floats and as unsigned integers.
    const words = Math.floor(data.byteLength / 4);
    const asFloat = new collapsible(ui, { label: `as float[${words}]`, collapsed: false });
    renderTypedData(new Widget("ul", asFloat.body, { class: "buffer-root" }), { kind: "array", element: { kind: "scalar", base: "float", width: 32, size: 4 }, count: words, stride: 4, size: words * 4 }, data, 0, radix);
    const asUint = new collapsible(ui, { label: `as uint[${words}]`, collapsed: true });
    renderTypedData(new Widget("ul", asUint.body, { class: "buffer-root" }), { kind: "array", element: { kind: "scalar", base: "uint", width: 32, size: 4 }, count: words, stride: 4, size: words * 4 }, data, 0, radix);
  }

  /** The buffer layout editor: GLSL struct declarations, layout rules, radix. */
  private _editFormat(key: string, current: ReflType | null, kind: "uniform" | "storage", apply: (type: ReflType | null, radix: Radix) => void): void {
    const existing = this._formats.get(key);
    const dialog = new Dialog({ title: "Buffer Format", width: 640, windowClass: "dialog format-dialog" });
    const body = dialog.body;
    body.classList.add("format-dialog-body");
    new Div(body, { text: "Declare the buffer's layout as GLSL structs; the last struct is the buffer's type. Offsets follow the selected layout rules.", class: "text-muted font-sm" });
    const text = new TextArea(body, { value: current ? layoutText(current) : "struct Buffer {\n    vec4 value[];\n};", class: "format-dialog-text" });
    const row = new Div(body, { class: "format-dialog-row" });
    new Span(row, { text: "Layout:" });
    const rulesOptions: LayoutRules[] = ["std140", "std430"];
    let rules: LayoutRules = existing?.rules ?? (kind === "uniform" ? "std140" : "std430");
    new Select(row, { options: rulesOptions, index: rulesOptions.indexOf(rules), onChange: (_v: string, i: number) => { rules = rulesOptions[i] ?? rules; } });
    new Span(row, { text: "Radix:" });
    const radixOptions: Radix[] = [10, 16, 8, 2];
    let radix: Radix = existing?.radix ?? this._radix;
    new Select(row, { options: ["Decimal", "Hexadecimal", "Octal", "Binary"], index: Math.max(0, radixOptions.indexOf(radix)), onChange: (_v: string, i: number) => { radix = radixOptions[i] ?? 10; } });
    const error = new Div(body, { class: "inspect_info_error", style: "display: none;" });
    const footer = new Div(body, { class: "dialog-footer format-dialog-footer" });
    new Button(footer, { label: "Apply", class: "btn", callback: () => {
      try {
        const type = parseLayout(text.value, rules, current?.kind === "struct" ? current.name : undefined);
        this._formats.set(key, { type, radix, rules });
        dialog.close();
        apply(type, radix);
      } catch (e) {
        error.text = e instanceof Error ? e.message : String(e);
        error.style.display = "";
      }
    } });
    new Button(footer, { label: "Revert", class: "btn", tooltip: "Use the shader's declared type again", callback: () => {
      this._formats.delete(key);
      dialog.close();
      apply(current, this._radix);
    } });
    new Button(footer, { label: "Cancel", class: "btn", callback: () => dialog.close() });
  }

  private _renderImageBinding(grp: collapsible, d: CaptureDescriptor): void {
    const db = this.db;
    const body = grp.body;
    if (d.imageView !== undefined) {
      const view = db.getObject(refId(d.imageView));
      const image = db.getObject(refId(view?.descriptor?.image));
      const row = new Div(body, { class: "font-md" });
      new Span(row, { text: "Image view: ", class: "text-muted" });
      if (view) objectLink(row, view, this._link); else new Span(row, { text: "(destroyed)" });
      if (view) new Span(row, { text: `  ${view.summary(db)}`, class: "text-muted" });
      if (image) {
        const row2 = new Div(body, { class: "font-md" });
        new Span(row2, { text: "Image: ", class: "text-muted" });
        objectLink(row2, image, this._link);
        new Span(row2, { text: `  ${image.summary(db)}`, class: "text-muted" });
      }
      if (d.imageLayout) new Div(body, { text: `Layout: ${fmt(d.imageLayout)}`, class: "text-muted font-sm" });
      const captured = this.panel.data.capturedImage(d.data);
      if (captured && !captured.info.error) this._capturedThumbnail(grp, captured, image);
      else if (view && image) {
        if (captured?.info.error) new Div(body, { text: `Not read back by the capture: ${captured.info.error}`, class: "text-muted font-sm" });
        this._thumbnail(grp, view, image);
      }
    }
    if (d.sampler !== undefined) {
      const sampler = db.getObject(refId(d.sampler));
      const row = new Div(body, { class: "font-md" });
      new Span(row, { text: `Sampler${d.immutable ? " (immutable)" : ""}: `, class: "text-muted" });
      if (sampler) {
        objectLink(row, sampler, this._link);
        new Span(row, { text: `  ${sampler.summary(db)}`, class: "text-muted" });
      } else {
        new Span(row, { text: "(destroyed)" });
      }
    }
    if (d.bufferView !== undefined) {
      const bv = db.getObject(refId(d.bufferView));
      const row = new Div(body, { class: "font-md" });
      new Span(row, { text: "Buffer view: ", class: "text-muted" });
      if (bv) objectLink(row, bv, this._link); else new Span(row, { text: "(destroyed)" });
      const buffer = db.getObject(refId(bv?.descriptor?.buffer));
      if (bv?.descriptor && buffer) {
        const row2 = new Div(body, { class: "font-md" });
        new Span(row2, { text: `${fmt(bv.descriptor.format)} offset ${num(bv.descriptor.offset)} range ${num(bv.descriptor.range)} of `, class: "text-muted" });
        objectLink(row2, buffer, this._link);
      }
    }
  }

  /** Contents the capture read back when the binding pass ended; clicking opens the image viewer in place. */
  private _capturedThumbnail(grp: collapsible, tex: CapturedTexture, image: VulkanObject | null): void {
    const box = new Div(grp.body, { class: "capture-image-box" });
    const canvas = this.panel.textureCanvas(tex, "capture-thumb loaded capture-texture-canvas");
    canvas.title = "Contents captured when the pass ended. Click to open in the image viewer";
    box.element.appendChild(canvas);
    new Div(box, { text: `Captured: ${fmt(tex.info.format).replace(/^VK_FORMAT_/, "")} ${tex.info.width}x${tex.info.height}${tex.info.layers > 1 ? ` [${tex.info.layers} layers]` : ""} ${tex.info.mips && tex.info.mips > 1 ? `mips ${tex.info.mip}-${tex.info.mip + tex.info.mips - 1}` : `mip ${tex.info.mip}`}${tex.info.samples && tex.info.samples > 1 ? ` (${tex.info.samples}x MSAA, resolved)` : ""}`, class: "text-muted font-sm" });
    let viewer: Div | null = null;
    const toggle = (): void => {
      if (!tex.data) return;
      if (viewer) {
        viewer.remove();
        viewer = null;
        canvas.style.display = "";
        return;
      }
      viewer = new Div(box, { class: "capture-texture-viewer" });
      new Button(viewer, { label: "Close viewer", class: "btn btn-sm", callback: toggle });
      new ImageView(viewer, this.panel.window, image, { info: tex.info, data: tex.data });
      canvas.style.display = "none";
    };
    canvas.onclick = toggle;
  }

  /** Current contents of the image (read from the running application when the binding is expanded). */
  private _thumbnail(grp: collapsible, view: VulkanObject, image: VulkanObject): void {
    const canvas = document.createElement("canvas");
    canvas.className = "capture-thumb";
    canvas.title = "Current contents of the image (read from the application, not from the capture)";
    grp.body.element.appendChild(canvas);
    const list = this._thumbs.get(image.id) ?? [];
    list.push(canvas);
    this._thumbs.set(image.id, list);
    const range = isObject(view.descriptor?.subresourceRange) ? view.descriptor.subresourceRange : null;
    const request = (): void => {
      if (this._thumbRequested.has(image.id)) return;
      this._thumbRequested.add(image.id);
      if (!this.panel.window.connected) {
        canvas.title = "Not connected: the image cannot be read";
        return;
      }
      void this.panel.window.send({ action: "RequestImage", id: image.id, mip: num(range?.baseMipLevel), layer: num(range?.baseArrayLayer) });
    };
    if (grp.collapsed) grp.onExpanded.addListener(request);
    else request();
  }

  // ---------------------------------------------------------------------------------------
  // Vertex and index buffers

  private _renderVertexBuffers(container: Widget, state: DrawState, buffers: BoundVertexBuffer[], token: number): void {
    if (!buffers.length) return;
    const host = new Div(container);
    const build = (vertexReflection: ShaderReflection | null): void => {
      for (const vb of buffers) this._renderVertexBuffer(host, state, vb, vertexReflection);
    };
    if (state.pipeline) {
      void this.panel.window.shaders.stages(state.pipeline).then((stages) => {
        if (token !== this._token) return;
        build(stages.find((s) => s.source.stage === "vertex")?.reflection ?? null);
      });
    } else {
      build(null);
    }
  }

  /** The vertex layout of one binding: stride, input rate and its attributes, from the pipeline or dynamic state. */
  private _vertexLayout(state: DrawState, binding: number, vb: BoundVertexBuffer): { stride: number; rate: string; attributes: { location: number; format: string; offset: number }[] } | null {
    const vi = state.vertexInput ?? (isObject(state.pipeline?.descriptor?.pVertexInputState) ? state.pipeline!.descriptor!.pVertexInputState : null);
    if (!isObject(vi)) return null;
    const bindings = Array.isArray(vi.pVertexBindingDescriptions) ? vi.pVertexBindingDescriptions : [];
    const attrs = Array.isArray(vi.pVertexAttributeDescriptions) ? vi.pVertexAttributeDescriptions : [];
    const b = bindings.find((x) => isObject(x) && num(x.binding) === binding);
    if (!isObject(b)) return null;
    const attributes = attrs.filter((a): a is ArgObject => isObject(a) && num(a.binding) === binding)
      .map((a) => ({ location: num(a.location), format: str(a.format), offset: num(a.offset) }))
      .sort((x, y) => x.offset - y.offset);
    return { stride: vb.stride ?? num(b.stride), rate: str(b.inputRate), attributes };
  }

  private _renderVertexBuffer(container: Widget, state: DrawState, vb: BoundVertexBuffer, vertexReflection: ShaderReflection | null): void {
    const db = this.db;
    const buf = db.getObject(refId(vb.buffer));
    const layout = this._vertexLayout(state, vb.binding, vb);
    const grp = new collapsible(container, {
      label: `Vertex Buffer ${vb.binding}: ${buf ? buf.name : "(none)"}  offset ${vb.offset}${layout ? `  stride ${layout.stride}${layout.rate.includes("INSTANCE") ? "  per instance" : ""}` : ""}`,
      collapsed: true,
    });
    const body = grp.body;
    const row = new Div(body, { class: "font-md" });
    new Span(row, { text: "Buffer: ", class: "text-muted" });
    if (buf) objectLink(row, buf, this._link); else new Span(row, { text: "(none)" });
    if (buf?.descriptor) new Span(row, { text: `  ${formatBytes(num(buf.descriptor.size))}  ${fmtFlags(buf.descriptor.usage)}`, class: "text-muted" });
    if (vb.cmd !== state.pipelineCmd) new Div(body, { text: `bound by #${vb.cmd.index} ${vb.cmd.method.replace(/^vkCmd/, "")}`, class: "text-muted font-sm" });
    if (buf) this._renderAffectedBy(body, buf.id);

    const inputs = vertexReflection?.entryPoint()?.inputs ?? [];
    const nameOf = (location: number): string => inputs.find((i) => i.location === location)?.name || `location${location}`;
    if (!layout) {
      new Div(body, { text: "No vertex input layout for this binding in the bound pipeline.", class: "text-muted" });
    } else {
      const table = new Widget("ul", body, { class: "vertex-attributes" });
      for (const a of layout.attributes) {
        new Widget("li", table, { text: `location ${a.location}  ${nameOf(a.location)}  ${fmt(a.format)}  offset ${a.offset}` });
      }
      if (!layout.attributes.length) new Widget("li", table, { text: "(no attributes read from this binding)", class: "text-muted" });
    }

    const captured = this.panel.data.buffer(vb.dataId);
    if (!captured) {
      new Div(body, { text: "Contents were not captured.", class: "text-muted" });
      return;
    }
    if (captured.info.error) {
      new Div(body, { text: `Contents not captured: ${captured.info.error}`, class: "text-muted" });
      return;
    }
    const data = captured.data;
    if (!data) {
      new Div(body, { text: "Loading contents...", class: "text-muted" });
      return;
    }
    const info = new Div(body, { class: "text-muted font-sm" });
    info.text = `${data.byteLength} bytes captured${captured.info.originalSize ? ` of ${captured.info.originalSize} (truncated)` : ""}`;
    if (!layout || !layout.stride) return;
    const stride = layout.stride;
    const count = Math.floor(data.byteLength / stride);
    info.text += `  ->  ${count} vertices`;
    const dataUi = new Div(body, { class: "buffer-data" });
    const button = new Button(null, { label: "Show Data", class: "btn btn-sm", callback: () => {
      if (dataUi.element.childElementCount) {
        dataUi.html = "";
        button.text = "Show Data";
        return;
      }
      button.text = "Hide Data";
      const element: StructType = {
        kind: "struct", name: "Vertex", size: stride,
        members: layout.attributes.map((a): StructMember => ({ name: nameOf(a.location), offset: a.offset, type: { kind: "format", format: a.format, size: vertexFormat(a.format)?.size ?? 0 } })),
      };
      const type: ReflType = { kind: "array", element, count, stride, size: count * stride };
      renderTypedData(new Widget("ul", dataUi, { class: "buffer-root" }), type, data, 0, this._radix);
    } });
    body.insertBefore(button, dataUi);
  }

  private _renderIndexBuffer(container: Widget, ib: BoundIndexBuffer, draw: CaptureCommand | null): void {
    const db = this.db;
    const buf = db.getObject(refId(ib.buffer));
    const grp = new collapsible(container, { label: `Index Buffer: ${buf ? buf.name : "(none)"}  ${fmt(ib.indexType)}  offset ${ib.offset}`, collapsed: true });
    const body = grp.body;
    const row = new Div(body, { class: "font-md" });
    new Span(row, { text: "Buffer: ", class: "text-muted" });
    if (buf) objectLink(row, buf, this._link); else new Span(row, { text: "(none)" });
    if (buf?.descriptor) new Span(row, { text: `  ${formatBytes(num(buf.descriptor.size))}  ${fmtFlags(buf.descriptor.usage)}`, class: "text-muted" });
    if (buf) this._renderAffectedBy(body, buf.id);

    const captured = this.panel.data.buffer(ib.dataId);
    if (!captured) {
      new Div(body, { text: "Contents were not captured.", class: "text-muted" });
      return;
    }
    if (captured.info.error) {
      new Div(body, { text: `Contents not captured: ${captured.info.error}`, class: "text-muted" });
      return;
    }
    const data = captured.data;
    if (!data) {
      new Div(body, { text: "Loading contents...", class: "text-muted" });
      return;
    }
    new Div(body, { text: `${data.byteLength} bytes captured${captured.info.originalSize ? ` of ${captured.info.originalSize} (truncated)` : ""}`, class: "text-muted font-sm" });
    const firstIndex = draw && draw.method === "vkCmdDrawIndexed" ? num(draw.args?.firstIndex) : 0;
    const indexCount = draw && draw.method === "vkCmdDrawIndexed" ? num(draw.args?.indexCount) : 0;
    const dataUi = new Div(body, { class: "buffer-data" });
    const button = new Button(null, { label: "Show Data", class: "btn btn-sm", callback: () => {
      if (dataUi.element.childElementCount) {
        dataUi.html = "";
        button.text = "Show Data";
        return;
      }
      button.text = "Hide Data";
      renderIndexData(dataUi, new DataView(data.buffer, data.byteOffset, data.byteLength), ib.indexType, firstIndex, indexCount);
    } });
    body.insertBefore(button, dataUi);
  }

  private _renderIndirect(container: Widget, cmd: CaptureCommand): void {
    const db = this.db;
    const a = cmd.args;
    if (!a) return;
    const buf = db.getObject(refId(a.buffer));
    const grp = new collapsible(container, { label: `Indirect Arguments: ${buf ? buf.name : "(none)"}  offset ${num(a.offset)}`, collapsed: false });
    const body = grp.body;
    const row = new Div(body, { class: "font-md" });
    new Span(row, { text: "Buffer: ", class: "text-muted" });
    if (buf) objectLink(row, buf, this._link); else new Span(row, { text: "(none)" });
    if (buf) this._renderAffectedBy(body, buf.id);
    const captured = this.panel.data.buffer(cmd.bufferData?.[0]);
    if (!captured || captured.info.error) {
      new Div(body, { text: captured?.info.error ? `Contents not captured: ${captured.info.error}` : "Contents were not captured.", class: "text-muted" });
      return;
    }
    if (!captured.data) {
      new Div(body, { text: "Loading contents...", class: "text-muted" });
      return;
    }
    const element = INDIRECT_TYPES[cmd.method];
    if (!element) return;
    const stride = Math.max(element.size, num(a.stride));
    const count = cmd.method === "vkCmdDispatchIndirect" ? 1 : Math.max(0, num(a.drawCount));
    renderTypedData(new Widget("ul", new Div(body, { class: "buffer-data" }), { class: "buffer-root" }),
      { kind: "array", element, count, stride, size: count * stride }, captured.data, 0, 10);
  }

  // ---------------------------------------------------------------------------------------
  // Push constants

  private _renderPushConstants(container: Widget, state: DrawState, updates: PushConstantUpdate[], token: number): void {
    if (!updates.length) return;
    // Merge the updates into one block (later updates overwrite earlier ones).
    let end = 0;
    for (const u of updates) end = Math.max(end, u.offset + u.size);
    const merged = new Uint8Array(end);
    const stages = new Set<string>();
    for (const u of updates) {
      if (u.data) merged.set(u.data.subarray(0, Math.min(u.data.byteLength, u.size)), u.offset);
      for (const s of u.stageFlags.split(" | ")) stages.add(s);
    }
    const grp = new collapsible(container, { label: `Push Constants  ${end} bytes  ${[...stages].map((s) => fmt(s)).join(" | ")}`, collapsed: false });
    const body = grp.body;
    if (updates.length > 1) {
      new Div(body, { text: updates.map((u) => `#${u.cmd.index}: ${fmt(u.stageFlags)} offset ${u.offset} size ${u.size}`).join("; "), class: "text-muted font-sm" });
    }
    if (updates.some((u) => !u.data)) new Div(body, { text: "Some values were not recorded (too large to inline).", class: "text-muted font-sm" });
    const host = new Div(body);
    const key = `${state.pipeline?.id ?? 0}:pc`;
    const captured: CapturedBuffer = { info: { id: 0, buffer: 0, frame: 0, commandBuffer: 0, offset: 0, size: end }, data: merged };
    const render = (blocks: { res: ShaderResource; stage: ShaderStage }[]): void => {
      if (!blocks.length) {
        this._renderBufferContents(host, key, "uniform", null, captured);
        return;
      }
      // Stages usually share one block declaration; show each distinct one once.
      const seen = new Set<string>();
      for (const b of blocks) {
        const sig = `${b.res.typeName}:${b.res.type.kind === "struct" ? b.res.type.size : 0}`;
        if (seen.has(sig)) continue;
        seen.add(sig);
        if (blocks.length > 1) new Div(host, { text: `${stageLabel(b.stage)} stage:`, class: "text-muted font-sm" });
        this._renderBufferContents(host, `${key}:${sig}`, "uniform", b.res, captured);
      }
    };
    if (state.pipeline) {
      void this.panel.window.shaders.stages(state.pipeline).then((refl) => {
        if (token !== this._token) return;
        const blocks: { res: ShaderResource; stage: ShaderStage }[] = [];
        for (const r of refl) for (const pc of r.reflection?.pushConstants ?? []) blocks.push({ res: pc, stage: r.source.stage });
        render(blocks);
      });
    } else {
      render([]);
    }
  }

  // ---------------------------------------------------------------------------------------
  // Render targets

  private _renderTargets(container: Widget, cmd: CaptureCommand): void {
    const pass = this.findPass(cmd);
    if (pass) this.panel.renderPassTargets(container, cmd.frame, pass.passBegin, pass.passIndex, cmd.object?.__id ?? 0);
  }
}

// ---------------------------------------------------------------------------------------------

function findResource(stages: StageReflection[], set: number, binding: number): { resource: ShaderResource; source: StageSource } | null {
  for (const s of stages) {
    const r = s.reflection?.findResource(set, binding);
    if (r) return { resource: r, source: s.source };
  }
  return null;
}

