// Frame capture panel: a capture bar and one tab per capture, like WebGPU Inspector. Each tab
// (CaptureView) shows its capture's command list grouped by submit / command buffer / render
// pass on the left and the selected command's details (see capture_command_info.ts) with the
// pass's render targets on the right. Structure follows WebGPU Inspector's capture_panel.js and
// command_list_view.js (MIT).
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { collapsible } from "./widget/collapsible.js";
import { showContextMenu, type ContextMenuItem } from "./widget/context_menu.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Split } from "./widget/split.js";
import { TabHandle } from "./widget/tab_handle.js";
import { TabWidget } from "./widget/tab_widget.js";
import { TextInput } from "./widget/text_input.js";
import { Widget } from "./widget/widget.js";
import { objectLink } from "./args_view.js";
import { CaptureData, type CapturedTexture } from "./capture_data.js";
import { CommandInfoView, type CaptureHost } from "./capture_command_info.js";
import { Signal } from "./utils/signal.js";
import { decodeImage } from "./vulkan/texture_decode.js";
import { LABEL_BEGIN, LABEL_END, PASS_BEGIN, PASS_END, SUBMIT_METHODS, isAction } from "./vulkan/command_sets.js";
import { fmt, isObject, num, refId, str } from "./vulkan/vulkan_object.js";
import type { SessionContext } from "./session_panel.js";
import type { ArgValue, CaptureCommand, LayerMessage } from "../shared/protocol.js";

interface CommandRow extends Widget {
  command: CaptureCommand;
}

export class CapturePanel {
  readonly window: SessionContext;
  readonly parent: Widget;

  private _statusLabel!: Span;
  private _frameCountInput!: TextInput;
  private _texturesCheck!: Checkbox;
  private _buffersCheck!: Checkbox;
  private _bufferSizeInput!: TextInput;
  private _tabs!: TabWidget;
  private _placeholder!: Div;
  private _views: CaptureView[] = [];
  private _handles = new Map<CaptureView, TabHandle>();
  /** The capture the layer is streaming to (the most recently requested one). */
  private _live: CaptureView | null = null;
  private _captureCount = 0;

  constructor(win: SessionContext, parent: Widget) {
    this.window = win;
    this.parent = parent;
    win.database.onOtherMessage.addListener((msg) => this._handleMessage(msg));
    this._build();
  }

  /** The capture shown in the active tab. */
  get activeView(): CaptureView | null {
    const index = this._tabs.activeTab;
    return index >= 0 ? this._views[index] ?? null : null;
  }

  /** The most recent capture's data. */
  get data(): CaptureData | null {
    return this._live?.data ?? this.activeView?.data ?? null;
  }

  private _build(): void {
    const bar = new Div(this.parent, { class: "control-bar capture-bar" });
    const row = new Div(bar, { class: "launch-row" });
    new Button(row, { label: "Capture", class: "btn btn-success", callback: () => this.capture() });
    new Span(row, { text: "Frames", class: "launch-label" });
    this._frameCountInput = new TextInput(row, { value: "1", class: "launch-input launch-input-narrow" });
    this._texturesCheck = new Checkbox(row, { label: "Render targets", checked: true, tooltip: "Read back render pass attachments at the end of each pass" });
    this._buffersCheck = new Checkbox(row, { label: "Buffers", checked: true, tooltip: "Read back the buffers bound by descriptor sets, vertex and index bindings and indirect draws" });
    new Span(row, { text: "Max KB", class: "launch-label", tooltip: "Bytes captured per bound buffer range; longer ranges are truncated" });
    this._bufferSizeInput = new TextInput(row, { value: "128", class: "launch-input launch-input-narrow" });
    this._statusLabel = new Span(row, { text: "", class: "launch-status" });

    this._tabs = new TabWidget(this.parent, { class: "capture-tabs tabs-fill", displayCloseButton: true });
    this._tabs.onTabClosed.addListener((panel) => this._tabClosed(panel));
    this._tabs.onActiveTabChanged.addListener(() => this._updateStatus());
    this._placeholder = new Div(this.parent, { class: "main-placeholder" });
    new Div(this._placeholder, { text: "No capture yet. Launch an application and press Capture. Each capture opens in its own tab.", class: "text-muted" });
    this._updatePlaceholder();
  }

  capture(frames?: number): void {
    if (!this.window.connected) {
      this._statusLabel.text = "not connected";
      return;
    }
    if (frames && frames > 0) this._frameCountInput.value = String(frames);
    const view = new CaptureView(this.window, ++this._captureCount);
    this._views.push(view);
    const handle = this._tabs.addTab(view.label, view.root);
    this._handles.set(view, handle);
    view.onLabelChanged.addListener(() => { handle.textElement.text = view.label; });
    view.onStatus.addListener(() => { if (this.activeView === view) this._updateStatus(); });
    handle.element.oncontextmenu = (e: MouseEvent) => {
      e.preventDefault();
      this._tabs.setHandleActive(handle);
      showContextMenu(e.clientX, e.clientY, this._tabMenu(view));
    };
    this._tabs.setHandleActive(handle);
    this._live = view;
    this._updatePlaceholder();
    this._statusLabel.text = "capturing...";
    const maxKb = Math.max(1, Number(this._bufferSizeInput.value) || 128);
    void this.window.send({
      action: "Capture",
      frameCount: Math.max(1, Number(this._frameCountInput.value) || 1),
      captureTextures: this._texturesCheck.checked,
      captureBuffers: this._buffersCheck.checked,
      maxBufferSize: maxKb * 1024,
    });
  }

  private _handleMessage(msg: LayerMessage): void {
    if (msg.action === "ImageData") {
      // Live image readbacks (descriptor set thumbnails) may be waited for by any capture.
      for (const v of this._views) v.info.handleImageData(msg);
      return;
    }
    this._live?.handleMessage(msg);
  }

  private _tabMenu(view: CaptureView): ContextMenuItem[] {
    return [
      { label: "Close", callback: () => this._close(view) },
      { label: "Close Others", disabled: this._views.length < 2, callback: () => { for (const v of [...this._views]) if (v !== view) this._close(v); } },
      { label: "Close All", callback: () => { for (const v of [...this._views]) this._close(v); } },
    ];
  }

  private _close(view: CaptureView): void {
    const handle = this._handles.get(view);
    if (handle) this._tabs.closeTabHandle(handle);
  }

  private _tabClosed(panel: Widget): void {
    const view = this._views.find((v) => v.root === panel);
    if (!view) return;
    this._views = this._views.filter((v) => v !== view);
    this._handles.delete(view);
    if (this._live === view) this._live = null;
    this._updatePlaceholder();
    this._updateStatus();
  }

  private _updatePlaceholder(): void {
    const empty = this._views.length === 0;
    this._placeholder.style.display = empty ? "" : "none";
    this._tabs.style.display = empty ? "none" : "";
  }

  private _updateStatus(): void {
    this._statusLabel.text = this.activeView?.status ?? "";
  }
}

// ---------------------------------------------------------------------------------------------

/** One capture: its data, command list and command details. `root` is the capture tab's contents. */
export class CaptureView implements CaptureHost {
  readonly window: SessionContext;
  readonly data = new CaptureData();
  readonly info: CommandInfoView;
  readonly captureIndex: number;
  readonly root: Div;
  status = "capturing...";

  readonly onStatus = new Signal<() => void>();
  readonly onLabelChanged = new Signal<() => void>();

  private _listPanel: Div;
  private _infoPanel: Div;
  private _selectedRow: CommandRow | null = null;
  private _drawCount = 0;
  private _commandBufferPassCounters = new Map<number, number>();
  private _refreshTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(win: SessionContext, captureIndex: number) {
    this.root = new Div(null, { class: "capture-view" });
    this.window = win;
    this.captureIndex = captureIndex;
    this.info = new CommandInfoView(this);

    const split = new Split(this.root, { direction: Split.Horizontal, position: 520 });
    const pane1 = new Span(split);
    this._listPanel = new Div(pane1, { class: "capture-commands" });
    const pane2 = new Span(split, { style: "flex-grow: 1; overflow: hidden;" });
    this._infoPanel = new Div(pane2, { class: "capture-info" });
    new Div(this._listPanel, { text: "Capturing...", class: "text-muted", style: "padding: 12px;" });

    this.data.onCaptureStatus.addListener((text) => this._setStatus(text));
    this.data.onCommandsComplete.addListener(() => this._renderCommands());
    this.data.onTextureLoaded.addListener((tex) => this._textureLoaded(tex));
    this.data.onTexturesAnnounced.addListener(() => {
      this._updateStatus();
      this._refreshSelection();
    });
    this.data.onBuffersAnnounced.addListener(() => this._updateStatus());
    // Buffer contents stream in after the commands; show them once they are all here (or a
    // little later while they are still arriving, so a slow transfer still updates the view).
    this.data.onBufferLoaded.addListener(() => this._scheduleRefresh());
    this.data.onBuffersComplete.addListener(() => {
      this._updateStatus();
      this._refreshSelection();
    });
  }

  /** Tab label: the captured frame number(s) once known. */
  get label(): string {
    const d = this.data;
    if (!d.commands.length && !d.frame) return `Capture ${this.captureIndex}`;
    return d.frames > 1 ? `Frames ${d.frame}-${d.frame + d.frames - 1}` : `Frame ${d.frame}`;
  }

  handleMessage(msg: LayerMessage): void {
    this.data.handleMessage(msg);
    if (msg.action === "CaptureFrameResults") this.onLabelChanged.emit();
  }

  private _setStatus(text: string): void {
    this.status = text;
    this.onStatus.emit();
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
      // One sub-tab per captured frame.
      const tabs = new TabWidget(this._listPanel, { class: "capture-frame-tabs tabs-fill" });
      for (let f = 0; f < frames; f++) {
        const list = new Div(null, { class: "capture-frame-list" });
        tabs.addTab(`Frame ${this.data.frame + f}`, list);
        this._renderFrame(f, list);
      }
    } else {
      this._renderFrame(0, this._listPanel);
    }
    this.onLabelChanged.emit();
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
      if (isAction(cmd.method)) {
        row.classList.add("capture_drawcall");
        drawCount++;
      }
    }
    this._drawCount += drawCount;
  }

  private _updateStatus(): void {
    const d = this.data;
    const which = d.frames > 1 ? `frames ${d.frame}-${d.frame + d.frames - 1}` : `frame ${d.frame}`;
    let buffers = "";
    if (d.buffers.size) {
      let failed = 0;
      for (const b of d.buffers.values()) if (b.info.error) failed++;
      buffers = `, ${d.buffers.size} buffers${failed ? ` (${failed} failed)` : ""}${d.buffersLoading ? " loading..." : ""}`;
    }
    this._setStatus(`${which}: ${d.commands.length} commands, ${this._drawCount} draws/dispatches, ${d.textures.length} render targets${buffers}`);
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
      case "vkCmdDrawIndirect":
      case "vkCmdDrawIndexedIndirect": return `${name(a.buffer)} x${num(a.drawCount)}`;
      case "vkCmdDispatch": return `${num(a.groupCountX)}x${num(a.groupCountY)}x${num(a.groupCountZ)}`;
      case "vkCmdBindPipeline": return `${fmt(a.pipelineBindPoint)} ${name(a.pipeline)}`;
      case "vkCmdBindDescriptorSets": return `set ${num(a.firstSet)} +${num(a.descriptorSetCount)}`;
      case "vkCmdPushDescriptorSet":
      case "vkCmdPushDescriptorSetKHR": return `set ${num(a.set)}: ${num(a.descriptorWriteCount)} writes`;
      case "vkCmdBindVertexBuffers":
      case "vkCmdBindVertexBuffers2":
      case "vkCmdBindVertexBuffers2EXT": return `binding ${num(a.firstBinding)} +${num(a.bindingCount)}`;
      case "vkCmdBindIndexBuffer":
      case "vkCmdBindIndexBuffer2":
      case "vkCmdBindIndexBuffer2KHR": return `${name(a.buffer)} ${fmt(a.indexType)}`;
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
    this.info.show(this._infoPanel, cmd);
  }

  /** Re-renders the selected command (new texture or buffer data arrived), keeping the scroll position. */
  private _refreshSelection(): void {
    if (this._refreshTimer) {
      clearTimeout(this._refreshTimer);
      this._refreshTimer = null;
    }
    if (!this._selectedRow) return;
    const scroll = this._infoPanel.element.scrollTop;
    this._showCommand(this._selectedRow.command);
    this._infoPanel.element.scrollTop = scroll;
  }

  private _scheduleRefresh(): void {
    if (this._refreshTimer || !this._selectedRow) return;
    this._refreshTimer = setTimeout(() => {
      this._refreshTimer = null;
      this._refreshSelection();
    }, 400);
  }

  // ---------------------------------------------------------------------------------------
  // Render targets

  renderPassTargets(container: Widget, frame: number, passBegin: CaptureCommand, passIndex: number, commandBufferId: number): void {
    const textures = this.data.texturesForPass(frame, commandBufferId, passIndex);
    const grp = new collapsible(container, { label: `Render Targets (${textures.length})`, collapsed: false });
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
