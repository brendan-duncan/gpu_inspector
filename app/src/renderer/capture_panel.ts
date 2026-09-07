// Frame capture panel: command list grouped by submit / command buffer / render pass on the
// left, selected command details and pass render targets on the right.
// Structure follows WebGPU Inspector's capture_panel.js and command_list_view.js (MIT).
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Split } from "./widget/split.js";
import { TabWidget } from "./widget/tab_widget.js";
import { TextInput } from "./widget/text_input.js";
import { Widget } from "./widget/widget.js";
import { objectLink, renderArgs } from "./args_view.js";
import { CaptureData, type CapturedTexture } from "./capture_data.js";
import { decodeImage } from "./vulkan/texture_decode.js";
import { fmt, isHandleRef, isObject, num, refId, str, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { SessionContext } from "./session_panel.js";
import type { ArgObject, ArgValue, CaptureCommand } from "../shared/protocol.js";

const DRAW_METHODS = new Set([
  "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect", "vkCmdDrawIndirectCount",
  "vkCmdDrawIndexedIndirectCount", "vkCmdDrawMeshTasksEXT", "vkCmdDrawMeshTasksIndirectEXT", "vkCmdDrawMeshTasksNV",
  "vkCmdDrawMultiEXT", "vkCmdDrawMultiIndexedEXT",
]);
const DISPATCH_METHODS = new Set(["vkCmdDispatch", "vkCmdDispatchIndirect", "vkCmdDispatchBase", "vkCmdTraceRaysKHR", "vkCmdTraceRaysIndirectKHR"]);
const PASS_BEGIN = new Set(["vkCmdBeginRenderPass", "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR", "vkCmdBeginRendering", "vkCmdBeginRenderingKHR"]);
const PASS_END = new Set(["vkCmdEndRenderPass", "vkCmdEndRenderPass2", "vkCmdEndRenderPass2KHR", "vkCmdEndRendering", "vkCmdEndRenderingKHR"]);
const LABEL_BEGIN = new Set(["vkCmdBeginDebugUtilsLabelEXT", "vkCmdDebugMarkerBeginEXT"]);
const LABEL_END = new Set(["vkCmdEndDebugUtilsLabelEXT", "vkCmdDebugMarkerEndEXT"]);
const SUBMIT_METHODS = new Set(["vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR", "vkQueuePresentKHR", "vkQueueBindSparse"]);

interface CommandRow extends Widget {
  command: CaptureCommand;
}

/** Bound state at a draw, reconstructed by walking back through the pass. */
export interface DrawState {
  pipeline: ArgObject | null;
  descriptorSets: Map<number, ArgObject>;   // set index -> vkCmdBindDescriptorSets args (last binding covering it)
  vertexBuffers: Map<number, { buffer: ArgValue; offset: ArgValue }>;
  indexBuffer: ArgObject | null;
  viewports: ArgValue | null;
  scissors: ArgValue | null;
  pushConstants: ArgObject[];
  passBegin: CaptureCommand | null;
  passIndex: number;
}

export class CapturePanel {
  readonly window: SessionContext;
  readonly parent: Widget;
  readonly data: CaptureData;

  private _statusLabel!: Span;
  private _frameCountInput!: TextInput;
  private _texturesCheck!: Checkbox;
  private _listPanel!: Div;
  private _infoPanel!: Div;
  private _selectedRow: CommandRow | null = null;
  private _drawCount = 0;
  private _commandBufferPassCounters = new Map<number, number>();

  constructor(win: SessionContext, parent: Widget) {
    this.window = win;
    this.parent = parent;
    this.data = new CaptureData();
    win.database.onOtherMessage.addListener((msg) => this.data.handleMessage(msg));
    this.data.onCaptureStatus.addListener((text) => { this._statusLabel.text = text; });
    this.data.onCommandsComplete.addListener(() => this._renderCommands());
    this.data.onTextureLoaded.addListener((tex) => this._textureLoaded(tex));
    this.data.onTexturesAnnounced.addListener(() => {
      this._updateStatus();
      if (this._selectedRow) this._showCommand(this._selectedRow.command);
    });
    this._build();
  }

  private _build(): void {
    const bar = new Div(this.parent, { class: "control-bar capture-bar" });
    const row = new Div(bar, { class: "launch-row" });
    new Button(row, { label: "Capture", class: "btn btn-success", callback: () => this.capture() });
    new Span(row, { text: "Frames", class: "launch-label" });
    this._frameCountInput = new TextInput(row, { value: "1", class: "launch-input launch-input-narrow" });
    this._texturesCheck = new Checkbox(row, { label: "Render targets", checked: true, tooltip: "Read back render pass attachments at the end of each pass" });
    this._statusLabel = new Span(row, { text: "", class: "launch-status" });

    const split = new Split(this.parent, { direction: Split.Horizontal, position: 520 });
    const pane1 = new Span(split);
    this._listPanel = new Div(pane1, { class: "capture-commands" });
    const pane2 = new Span(split, { style: "flex-grow: 1; overflow: hidden;" });
    this._infoPanel = new Div(pane2, { class: "capture-info" });
    new Div(this._listPanel, { text: "No capture yet. Launch an application and press Capture.", class: "text-muted", style: "padding: 12px;" });
  }

  capture(frames?: number): void {
    if (!this.window.connected) {
      this._statusLabel.text = "not connected";
      return;
    }
    if (frames && frames > 0) this._frameCountInput.value = String(frames);
    this.data.reset();
    this._listPanel.html = "";
    this._infoPanel.html = "";
    this._statusLabel.text = "capturing...";
    void this.window.send({
      action: "Capture",
      frameCount: Math.max(1, Number(this._frameCountInput.value) || 1),
      captureTextures: this._texturesCheck.checked,
    });
  }

  // ---------------------------------------------------------------------------------------
  // Command list

  private _renderCommands(): void {
    this._listPanel.html = "";
    this._infoPanel.html = "";
    this._selectedRow = null;
    this._drawCount = 0;
    const frames = this.data.frames;
    if (frames > 1) {
      // One tab per captured frame, like WebGPU Inspector.
      const tabs = new TabWidget(this._listPanel, { class: "capture-frame-tabs tabs-fill" });
      for (let f = 0; f < frames; f++) {
        const list = new Div(null, { class: "capture-frame-list" });
        tabs.addTab(`Frame ${this.data.frame + f}`, list);
        this._renderFrame(f, list);
      }
    } else {
      this._renderFrame(0, this._listPanel);
    }
    this._updateStatus();
    const first = this._listPanel.element.querySelector(".capture_drawcall") as HTMLElement | null;
    first?.click();
  }

  /** Builds the command tree of one captured frame into `container`. */
  private _renderFrame(frame: number, container: Widget): void {
    this._commandBufferPassCounters.clear();
    const commands = this.data.commandsForFrame(frame);
    const db = this.window.database;

    // Containers: submit -> command buffer -> render pass / debug label groups.
    let submitBody: Widget = container;
    let cbBody: Widget = submitBody;
    let currentCb = -1;
    const stack: Widget[] = [];     // open pass / label bodies within the command buffer
    let current: Widget = cbBody;
    let drawCount = 0;

    let currentSecondary = 0;      // secondary command buffer whose inlined commands are being listed
    let secondaryParent: Widget | null = null;

    const closeSecondary = (): void => {
      if (currentSecondary && secondaryParent) current = secondaryParent;
      currentSecondary = 0;
      secondaryParent = null;
    };
    const closeCommandBuffer = (): void => {
      closeSecondary();
      stack.length = 0;
      currentCb = -1;
      current = submitBody;
    };

    for (const cmd of commands) {
      const objId = cmd.object?.__id ?? 0;
      if ((cmd.secondary ?? 0) !== currentSecondary) {
        closeSecondary();
        if (cmd.secondary) {
          const sec = db.getObject(cmd.secondary);
          const block = new collapsible(current, { label: `Secondary: ${sec ? sec.name : `CommandBuffer ${cmd.secondary}`}`, collapsed: false, class: "capture-secondary" });
          secondaryParent = current;
          currentSecondary = cmd.secondary;
          current = block.body;
        }
      }
      if (SUBMIT_METHODS.has(cmd.method)) {
        closeCommandBuffer();
        const queue = db.getObject(objId);
        const block = new collapsible(container, { label: `${cmd.method}  ${queue ? queue.name : ""}`, collapsed: false, class: "capture-submit" });
        this._addRow(block.titleBar, cmd, true);
        submitBody = block.body;
        current = submitBody;
        continue;
      }
      if (objId !== currentCb) {
        // New command buffer within this submit.
        closeCommandBuffer();
        currentCb = objId;
        const cbObj = db.getObject(objId);
        const cbBlock = new collapsible(submitBody, { label: cbObj ? cbObj.name : `CommandBuffer ${objId}`, collapsed: false, class: "capture-cmdbuf" });
        cbBody = cbBlock.body;
        current = cbBody;
        if (cmd.method.startsWith("<")) {
          new Div(cbBody, { text: cmd.method.replace(/[<>]/g, ""), class: "text-muted capture-note" });
          continue;
        }
      }
      if (PASS_BEGIN.has(cmd.method)) {
        const passIndex = this._commandBufferPassCounters.get(objId) ?? 0;
        this._commandBufferPassCounters.set(objId, passIndex + 1);
        const label = this._passLabel(cmd, passIndex);
        const block = new collapsible(current, { label, collapsed: false, class: "capture_renderpass_block" });
        const row = this._addRow(block.titleBar, cmd, true);
        row.element.dataset.passIndex = String(passIndex);
        stack.push(current);
        current = block.body;
        continue;
      }
      if (PASS_END.has(cmd.method)) {
        closeSecondary();
        this._addRow(current, cmd);
        current = stack.pop() ?? cbBody;
        continue;
      }
      if (LABEL_BEGIN.has(cmd.method)) {
        const info = cmd.args && (isObject(cmd.args.pLabelInfo) ? cmd.args.pLabelInfo : isObject(cmd.args.pMarkerInfo) ? cmd.args.pMarkerInfo : null);
        const name = info ? str(info.pLabelName ?? info.pMarkerName) : cmd.method;
        const block = new collapsible(current, { label: name, collapsed: false, class: `capture_debugGroup capture_debugGroup${stack.length % 5}` });
        this._addRow(block.titleBar, cmd, true);
        stack.push(current);
        current = block.body;
        continue;
      }
      if (LABEL_END.has(cmd.method)) {
        this._addRow(current, cmd);
        current = stack.pop() ?? cbBody;
        continue;
      }
      const row = this._addRow(current, cmd);
      if (DRAW_METHODS.has(cmd.method) || DISPATCH_METHODS.has(cmd.method)) {
        row.classList.add("capture_drawcall");
        drawCount++;
      }
    }
    this._drawCount += drawCount;
  }

  private _updateStatus(): void {
    const d = this.data;
    const which = d.frames > 1 ? `frames ${d.frame}-${d.frame + d.frames - 1}` : `frame ${d.frame}`;
    this._statusLabel.text = `${which}: ${d.commands.length} commands, ${this._drawCount} draws/dispatches, ${d.textures.length} render targets`;
  }

  private _passLabel(cmd: CaptureCommand, passIndex: number): string {
    const db = this.window.database;
    const a = cmd.args;
    if (a && isObject(a.pRenderPassBegin)) {
      const rp = db.getObject(refId(a.pRenderPassBegin.renderPass));
      const fb = db.getObject(refId(a.pRenderPassBegin.framebuffer));
      return `Render Pass ${passIndex}: ${rp?.name ?? "?"}  (${fb?.name ?? "?"})`;
    }
    if (a && isObject(a.pRenderingInfo)) {
      const colors = Array.isArray(a.pRenderingInfo.pColorAttachments) ? a.pRenderingInfo.pColorAttachments.length : 0;
      return `Rendering ${passIndex}: ${colors} color attachment${colors === 1 ? "" : "s"}`;
    }
    return `Pass ${passIndex}`;
  }

  private _addRow(parent: Widget, cmd: CaptureCommand, inline = false): CommandRow {
    const row = new Div(parent, { class: inline ? "capture_command capture_command_inline" : "capture_command" }) as CommandRow;
    row.command = cmd;
    new Span(row, { text: `${cmd.index}`, class: "capture_callnum" });
    new Span(row, { text: cmd.method.replace(/^vk(Cmd)?/, ""), class: "capture_methodName" });
    new Span(row, { text: this._summarizeArgs(cmd), class: "capture_method_args" });
    row.element.onclick = (e: MouseEvent) => {
      e.stopPropagation();
      this._selectRow(row);
    };
    return row;
  }

  private _selectRow(row: CommandRow): void {
    if (this._selectedRow) this._selectedRow.classList.remove("capture_command_selected");
    this._selectedRow = row;
    row.classList.add("capture_command_selected");
    this._showCommand(row.command);
  }

  private _summarizeArgs(cmd: CaptureCommand): string {
    const a = cmd.args;
    if (!a) return "";
    const db = this.window.database;
    const name = (v: ArgValue | undefined): string => {
      const o = db.getObject(refId(v));
      return o ? o.name : "";
    };
    switch (cmd.method) {
      case "vkCmdDraw": return `${num(a.vertexCount)} verts x${num(a.instanceCount)}`;
      case "vkCmdDrawIndexed": return `${num(a.indexCount)} idx x${num(a.instanceCount)}`;
      case "vkCmdDispatch": return `${num(a.groupCountX)}x${num(a.groupCountY)}x${num(a.groupCountZ)}`;
      case "vkCmdBindPipeline": return `${fmt(a.pipelineBindPoint)} ${name(a.pipeline)}`;
      case "vkCmdBindDescriptorSets": return `set ${num(a.firstSet)} +${num(a.descriptorSetCount)}`;
      case "vkCmdBindVertexBuffers": return `binding ${num(a.firstBinding)} +${num(a.bindingCount)}`;
      case "vkCmdBindIndexBuffer": return `${name(a.buffer)} ${fmt(a.indexType)}`;
      case "vkCmdPushConstants": return `${fmt(a.stageFlags)} ${num(a.size)} bytes`;
      case "vkCmdPipelineBarrier": return `${num(a.memoryBarrierCount)}m ${num(a.bufferMemoryBarrierCount)}b ${num(a.imageMemoryBarrierCount)}i`;
      case "vkCmdCopyBufferToImage": return `${name(a.srcBuffer)} -> ${name(a.dstImage)}`;
      case "vkCmdCopyImage": return `${name(a.srcImage)} -> ${name(a.dstImage)}`;
      case "vkCmdCopyBuffer": return `${name(a.srcBuffer)} -> ${name(a.dstBuffer)}`;
      case "vkCmdSetViewport": {
        const v = Array.isArray(a.pViewports) && isObject(a.pViewports[0]) ? a.pViewports[0] : null;
        return v ? `${num(v.width)}x${num(v.height)}` : "";
      }
      case "vkCmdSetScissor": {
        const s = Array.isArray(a.pScissors) && isObject(a.pScissors[0]) && isObject(a.pScissors[0].extent) ? a.pScissors[0].extent : null;
        return s ? `${num(s.width)}x${num(s.height)}` : "";
      }
      case "vkQueueSubmit": return `${Array.isArray(a.pSubmits) ? a.pSubmits.length : 0} submit(s)`;
      case "vkQueuePresentKHR": return "";
      default: return "";
    }
  }

  // ---------------------------------------------------------------------------------------
  // Command details

  private _showCommand(cmd: CaptureCommand): void {
    this._infoPanel.html = "";
    const db = this.window.database;
    const onLink = (o: VulkanObject) => this.window.showObject(o.id);

    const box = new Div(this._infoPanel, { class: "info-box info-box-success" });
    new Div(box, { text: `${cmd.method}`, class: "font-lg" });
    const obj = db.getObject(cmd.object?.__id);
    if (obj) {
      const row = new Div(box, { class: "font-md text-muted" });
      new Span(row, { text: "Object:", style: "margin-right: 4px;" });
      objectLink(row, obj, onLink);
    }
    if (cmd.result) new Div(box, { text: `Result: ${cmd.result}`, class: "font-md text-muted" });

    if (DRAW_METHODS.has(cmd.method) || DISPATCH_METHODS.has(cmd.method)) {
      const state = this._drawState(cmd);
      this._renderDrawState(state, onLink);
    }
    if (PASS_BEGIN.has(cmd.method) || PASS_END.has(cmd.method) || DRAW_METHODS.has(cmd.method)) {
      const pass = this._findPass(cmd);
      if (pass) this._renderPassTargets(cmd.frame, pass.passBegin, pass.passIndex, cmd.object?.__id ?? 0);
    }

    const argsGrp = new collapsible(this._infoPanel, { label: "Arguments", collapsed: false });
    renderArgs(new Div(argsGrp.body, { class: "args-tree" }), cmd.args, db, onLink);

    if (cmd.secondary) {
      const sec = db.getObject(cmd.secondary);
      const row = new Div(box, { class: "font-md text-muted" });
      new Span(row, { text: "Recorded in secondary command buffer:", style: "margin-right: 4px;" });
      if (sec) objectLink(row, sec, onLink); else new Span(row, { text: String(cmd.secondary) });
    }
    if (cmd.children) {
      const grp = new collapsible(this._infoPanel, { label: `Secondary command buffers (${cmd.children.length})`, collapsed: true });
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

  /** Pass containing (or begun by) a command, found by walking back in the flat list. */
  private _findPass(cmd: CaptureCommand): { passBegin: CaptureCommand; passIndex: number } | null {
    const commands = this.data.commands;
    const cbId = cmd.object?.__id;
    let depth = 0;
    for (let i = cmd.index; i >= 0; i--) {
      const c = commands[i];
      if (!c || c.object?.__id !== cbId) break;
      if (i !== cmd.index && PASS_END.has(c.method)) depth++;
      if (PASS_BEGIN.has(c.method)) {
        if (depth === 0) {
          // Pass index = number of pass begins before this one in the same command buffer.
          let passIndex = 0;
          for (let j = i - 1; j >= 0; j--) {
            const p = commands[j];
            if (!p || p.object?.__id !== cbId) break;
            if (PASS_BEGIN.has(p.method)) passIndex++;
          }
          return { passBegin: c, passIndex };
        }
        depth--;
      }
    }
    return null;
  }

  private _drawState(cmd: CaptureCommand): DrawState {
    const commands = this.data.commands;
    const cbId = cmd.object?.__id;
    const state: DrawState = {
      pipeline: null, descriptorSets: new Map(), vertexBuffers: new Map(), indexBuffer: null,
      viewports: null, scissors: null, pushConstants: [], passBegin: null, passIndex: 0,
    };
    const bindPoint = DISPATCH_METHODS.has(cmd.method) ? "VK_PIPELINE_BIND_POINT_COMPUTE" : "VK_PIPELINE_BIND_POINT_GRAPHICS";
    for (let i = cmd.index - 1; i >= 0; i--) {
      const c = commands[i];
      if (!c || c.object?.__id !== cbId) break;
      const a = c.args;
      if (!a) continue;
      switch (c.method) {
        case "vkCmdBindPipeline":
          if (!state.pipeline && a.pipelineBindPoint === bindPoint) state.pipeline = a;
          break;
        case "vkCmdBindDescriptorSets":
          if (a.pipelineBindPoint === bindPoint && Array.isArray(a.pDescriptorSets)) {
            const first = num(a.firstSet);
            a.pDescriptorSets.forEach((set, k) => {
              const idx = first + k;
              if (!state.descriptorSets.has(idx) && isHandleRef(set)) state.descriptorSets.set(idx, { set, layout: a.layout, dynamicOffsets: a.pDynamicOffsets });
            });
          }
          break;
        case "vkCmdBindVertexBuffers":
        case "vkCmdBindVertexBuffers2":
        case "vkCmdBindVertexBuffers2EXT":
          if (Array.isArray(a.pBuffers)) {
            const first = num(a.firstBinding);
            a.pBuffers.forEach((buffer, k) => {
              const binding = first + k;
              if (!state.vertexBuffers.has(binding)) {
                state.vertexBuffers.set(binding, { buffer, offset: Array.isArray(a.pOffsets) ? a.pOffsets[k] : 0 });
              }
            });
          }
          break;
        case "vkCmdBindIndexBuffer":
        case "vkCmdBindIndexBuffer2":
        case "vkCmdBindIndexBuffer2KHR":
          if (!state.indexBuffer) state.indexBuffer = a;
          break;
        case "vkCmdSetViewport":
        case "vkCmdSetViewportWithCount":
          if (!state.viewports) state.viewports = a.pViewports ?? null;
          break;
        case "vkCmdSetScissor":
        case "vkCmdSetScissorWithCount":
          if (!state.scissors) state.scissors = a.pScissors ?? null;
          break;
        case "vkCmdPushConstants":
          state.pushConstants.unshift(a);
          break;
        default:
          break;
      }
      if (PASS_BEGIN.has(c.method) && !state.passBegin) {
        state.passBegin = c;
        break;  // state outside the pass still applies, but this is enough for now
      }
    }
    return state;
  }

  private _renderDrawState(state: DrawState, onLink: (o: VulkanObject) => void): void {
    const db = this.window.database;
    const grp = new collapsible(this._infoPanel, { label: "Pipeline State", collapsed: false });
    const body = new Div(grp.body, { class: "draw-state" });
    const line = (label: string): Div => {
      const row = new Div(body, { class: "draw-state-row" });
      new Span(row, { text: label, class: "draw-state-label" });
      return row;
    };

    const pipeline = db.getObject(refId(state.pipeline?.pipeline));
    const r = line("Pipeline");
    if (pipeline) objectLink(r, pipeline, onLink); else new Span(r, { text: "(none bound)", class: "text-muted" });

    if (pipeline?.descriptor) {
      const d = pipeline.descriptor;
      const stages = Array.isArray(d.pStages) ? d.pStages : (isObject(d.stage) ? [d.stage] : []);
      for (const s of stages) {
        if (!isObject(s)) continue;
        const module = db.getObject(refId(s.module));
        const row = line(`  ${fmt(s.stage)}`);
        if (module) objectLink(row, module, onLink);
        new Span(row, { text: ` ${str(s.pName)}`, class: "text-muted" });
      }
      const ia = isObject(d.pInputAssemblyState) ? d.pInputAssemblyState : null;
      if (ia) new Span(line("Topology"), { text: fmt(ia.topology) });
      const rs = isObject(d.pRasterizationState) ? d.pRasterizationState : null;
      if (rs) new Span(line("Raster"), { text: `${fmt(rs.polygonMode)} cull ${fmt(rs.cullMode)} ${fmt(rs.frontFace)}` });
      const ds = isObject(d.pDepthStencilState) ? d.pDepthStencilState : null;
      if (ds) new Span(line("Depth"), { text: `test ${ds.depthTestEnable ? "on" : "off"} write ${ds.depthWriteEnable ? "on" : "off"} ${fmt(ds.depthCompareOp)}` });
      const layout = db.getObject(refId(d.layout));
      if (layout) objectLink(line("Layout"), layout, onLink);
    }

    for (const [index, ds] of [...state.descriptorSets.entries()].sort((a, b) => a[0] - b[0])) {
      const set = db.getObject(refId(ds.set));
      const row = line(`Descriptor set ${index}`);
      if (set) objectLink(row, set, onLink); else new Span(row, { text: "(destroyed)" });
    }
    for (const [binding, vb] of [...state.vertexBuffers.entries()].sort((a, b) => a[0] - b[0])) {
      const buf = db.getObject(refId(vb.buffer));
      const row = line(`Vertex buffer ${binding}`);
      if (buf) objectLink(row, buf, onLink); else new Span(row, { text: "(none)" });
      new Span(row, { text: ` offset ${num(vb.offset)}`, class: "text-muted" });
    }
    if (state.indexBuffer) {
      const buf = db.getObject(refId(state.indexBuffer.buffer));
      const row = line("Index buffer");
      if (buf) objectLink(row, buf, onLink);
      new Span(row, { text: ` ${fmt(state.indexBuffer.indexType)} offset ${num(state.indexBuffer.offset)}`, class: "text-muted" });
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
    for (const pc of state.pushConstants) {
      new Span(line("Push constants"), { text: `${fmt(pc.stageFlags)} offset ${num(pc.offset)} size ${num(pc.size)}` });
    }
  }

  // ---------------------------------------------------------------------------------------
  // Render targets

  private _renderPassTargets(frame: number, passBegin: CaptureCommand, passIndex: number, commandBufferId: number): void {
    const textures = this.data.texturesForPass(frame, commandBufferId, passIndex);
    const grp = new collapsible(this._infoPanel, { label: `Render Targets (${textures.length})`, collapsed: false });
    if (!textures.length) {
      new Div(grp.body, { text: "No render target data for this pass (pre-recorded command buffer, or readback disabled).", class: "text-muted", style: "padding: 6px;" });
      return;
    }
    const strip = new Div(grp.body, { class: "capture_frameImages" });
    for (const tex of textures) this._renderTexture(strip, tex);
  }

  private _renderTexture(parent: Widget, tex: CapturedTexture): void {
    const db = this.window.database;
    const image = db.getObject(tex.info.id);
    const box = new Div(parent, { class: "capture_pass_texture" });
    const title = new Div(box, { class: "capture-texture-title" });
    new Span(title, { text: `${tex.info.attachment}: ` });
    if (image) objectLink(title, image, (o) => this.window.showObject(o.id)); else new Span(title, { text: `Image ${tex.info.id}` });
    new Div(box, { text: `${fmt(tex.info.format)} ${tex.info.width}x${tex.info.height}${tex.info.layers > 1 ? ` [${tex.info.layers}]` : ""} ${tex.info.aspect}`, class: "text-muted font-sm" });
    if (tex.info.error) {
      new Div(box, { text: tex.info.error, class: "text-muted font-sm" });
      return;
    }
    const canvas = document.createElement("canvas");
    canvas.className = "capture-texture-canvas";
    box.element.appendChild(canvas);
    if (tex.data) this._drawTexture(canvas, tex);
    else {
      canvas.width = 64;
      canvas.height = 64;
      tex.canvas = canvas;
    }
  }

  private _drawTexture(canvas: HTMLCanvasElement, tex: CapturedTexture): void {
    const decoded = decodeImage(tex.info, tex.data!);
    if (!decoded) {
      canvas.width = 64;
      canvas.height = 64;
      const ctx = canvas.getContext("2d")!;
      ctx.fillStyle = "#444";
      ctx.fillRect(0, 0, 64, 64);
      ctx.fillStyle = "#ccc";
      ctx.font = "10px sans-serif";
      ctx.fillText("unsupported", 2, 34);
      return;
    }
    canvas.width = tex.info.width;
    canvas.height = tex.info.height;
    const ctx = canvas.getContext("2d")!;
    ctx.putImageData(new ImageData(decoded, tex.info.width, tex.info.height), 0, 0);
    canvas.style.maxWidth = "100%";
  }

  private _textureLoaded(tex: CapturedTexture): void {
    if (tex.canvas && tex.data) this._drawTexture(tex.canvas, tex);
  }
}
