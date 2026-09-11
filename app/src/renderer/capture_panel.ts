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
import { CaptureData, parsePassKey, passKey, type CapturedTexture } from "./capture_data.js";
import { fetchBlob, serializeCapture } from "./capture_file.js";
import { resolveSymbols } from "./stacktrace_view.js";
import { CAPTURE_FILE_FILTERS, captureFileName, parseCaptureFile, type LoadedCapture } from "./capture_format.js";
import { renderFrameReport, type FrameShaderReport } from "./shader_analysis_view.js";
import { renderFrameFlameGraph } from "./frame_flamegraph.js";
import type { StageModel } from "./frame_cost_tree.js";
import { analyzeSpirvCached } from "./vulkan/spirv_analysis.js";
import { pipelineStages, pipelineUses, stageLabel } from "./shader_cache.js";
import { CommandInfoView, type CaptureHost } from "./capture_command_info.js";
import { CaptureStatistics } from "./capture_statistics.js";
import { renderFrameStats, type FrameTimingInfo } from "./frame_stats_view.js";
import { analyzeFrame, type FrameFinding } from "./vulkan/frame_analysis.js";
import { frameRenderGraph } from "./frame_graph.js";
import { renderRenderGraph } from "./render_graph_view.js";
import { renderBottleneckReport } from "./bottleneck_report.js";
import { collectPassMetrics, formatPercent, formatRatio, type PassMetrics } from "./pass_metrics.js";
import { isMeasured, overdrawAverages, overdrawHistogramText, overdrawRgba, overdrawSummary, parseOverdrawFile } from "./overdraw.js";
import { OverdrawView, type OverdrawPassKey } from "./overdraw_view.js";
import type { RenderGraph } from "./render_graph.js";
import { SEVERITY_RANK } from "./vulkan/spirv_analysis.js";
import { TimelineWidget, type TimelinePassCommand } from "./widget/timeline.js";
import { Signal } from "./utils/signal.js";
import { decodeImage } from "./vulkan/texture_decode.js";
import { ImageView } from "./image_view.js";
import { isAction, labelNameOf } from "./command_sets.js";
import { fmt, isObject, refId } from "./vulkan/vulkan_object.js";
import type { SessionContext } from "./session_panel.js";
import { getHostPlatform } from "./launch_dialog.js";
import type { ArgValue, CaptureCommand, CaptureTextureInfo, LayerMessage, PassTiming } from "../shared/protocol.js";
import type { ValidationEntry } from "./vulkan/object_database.js";
import { severityMark, validationItemText, worstSeverity } from "./validation_text.js";

interface CommandRow extends Widget {
  command: CaptureCommand;
  /** The validation marker, once the command has messages. */
  validationMark?: Span;
  /** The frame analysis marker, when a finding applies to the command. */
  findingMark?: Span;
  /** Lower-case text the command list filter matches against (method and argument summary). */
  filterText: string;
}

// Capture bar icon (inline SVG in the button's text color): a floppy disk for Save.
const ICON_SAVE = '<svg viewBox="0 0 16 16" aria-label="Save"><path d="M2.5 2.5h8.6l2.4 2.4v8.6h-11z" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"/><path d="M5 2.5v3.5h5v-3.5" fill="none" stroke="currentColor" stroke-width="1.4"/><rect x="4.5" y="9" width="7" height="4.5" fill="none" stroke="currentColor" stroke-width="1.4"/></svg>';

// The Reports menu and its entries. Four reports as four buttons filled the filter row and
// wrapped it; one menu holds them, and the next report to be added as well.
const ICON_REPORTS = '<svg viewBox="0 0 16 16" aria-label="Reports"><path d="M2.5 4h11M2.5 8h11M2.5 12h11" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/></svg>';
/** Bar chart: counts of things in the frame. */
const ICON_STATS = '<svg viewBox="0 0 16 16"><path d="M3 13.2V8.5M8 13.2V3.2M13 13.2V6.2" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/></svg>';
/** Magnifier over a document: static analysis of the shaders. */
const ICON_ANALYZE = '<svg viewBox="0 0 16 16"><circle cx="7" cy="7" r="4.2" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M10.2 10.2 14 14" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/><path d="M5 7h4" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/></svg>';
/** Stacked frames narrowing upwards: a flame graph. */
const ICON_FLAME = '<svg viewBox="0 0 16 16"><rect x="2" y="10.5" width="12" height="3" fill="none" stroke="currentColor" stroke-width="1.3"/><rect x="2" y="6.5" width="7.5" height="3" fill="none" stroke="currentColor" stroke-width="1.3"/><rect x="2" y="2.5" width="4" height="3" fill="none" stroke="currentColor" stroke-width="1.3"/></svg>';
/** A gauge needle: what limits each pass. */
const ICON_BOTTLENECK = '<svg viewBox="0 0 16 16"><path d="M2.2 12a6.4 6.4 0 0 1 11.6 0" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M8 12 11 6.6" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/><circle cx="8" cy="12" r="1.1" fill="currentColor"/></svg>';
/** Nodes joined by edges: the pass dependency graph. */
const ICON_GRAPH = '<svg viewBox="0 0 16 16"><circle cx="3.5" cy="8" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="12.5" cy="3.8" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="12.5" cy="12.2" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M5.4 7.2 10.6 4.6M5.4 8.8l5.2 2.6" fill="none" stroke="currentColor" stroke-width="1.3"/></svg>';

/** Stacked squares: fragments landing on the same pixels. */
const ICON_OVERDRAW = '<svg viewBox="0 0 16 16"><rect x="2" y="6.5" width="7.5" height="7.5" fill="none" stroke="currentColor" stroke-width="1.3"/><rect x="4.25" y="4.25" width="7.5" height="7.5" fill="none" stroke="currentColor" stroke-width="1.3"/><rect x="6.5" y="2" width="7.5" height="7.5" fill="currentColor" fill-opacity="0.35" stroke="currentColor" stroke-width="1.3"/></svg>';

export class CapturePanel {
  readonly window: SessionContext;
  readonly parent: Widget;

  private _statusLabel!: Span;
  private _frameCountInput!: TextInput;
  private _atFrameInput!: TextInput;
  private _texturesCheck!: Checkbox;
  private _buffersCheck!: Checkbox;
  private _imagesCheck!: Checkbox;
  private _profileCheck!: Checkbox;
  private _stacksCheck!: Checkbox;
  /** Metal only: every render pass drawn again to measure its overdraw. */
  private _overdrawCheck: Checkbox | null = null;
  private _bufferSizeInput!: TextInput;
  private _saveButton!: Button;
  /** The live-capture controls of the bar, hidden for capture files. */
  private _captureControls: Widget[] = [];
  private _tabs!: TabWidget;
  private _placeholder!: Div;
  private _views: CaptureView[] = [];
  private _handles = new Map<CaptureView, TabHandle>();
  /** The capture the layer is streaming to (the most recently requested one). */
  private _live: CaptureView | null = null;
  private _captureCount = 0;
  /** Each capture's overdraw tab, once one of its passes was opened in it. */
  private _overdrawTabs = new Map<CaptureView, { view: OverdrawView; handle: TabHandle }>();

  constructor(win: SessionContext, parent: Widget) {
    this.window = win;
    this.parent = parent;
    win.database.onOtherMessage.addListener((msg) => this._handleMessage(msg));
    this._build();
  }

  /** The capture shown in the active tab. */
  /** The UI tests' view of every capture tab (tools/ui_tests.py). */
  debugState(): Record<string, unknown>[] {
    return this._views.map((v) => ({ ...v.debugState(), overdrawTab: this._overdrawTabs.get(v)?.view.debugState() ?? null }));
  }

  /** The capture shown in the active tab: its own tab, or the overdraw tab it opened. */
  get activeView(): CaptureView | null {
    const index = this._tabs.activeTab;
    const handle = index >= 0 ? this._tabs.tabListElement.children[index] : null;
    if (!handle) return null;
    for (const [view, h] of this._handles) if (h === handle) return view;
    for (const [view, t] of this._overdrawTabs) if (t.handle === handle) return view;
    return null;
  }

  /** The most recent capture's data. */
  get data(): CaptureData | null {
    return this._live?.data ?? this.activeView?.data ?? null;
  }

  /** Captured contents of an image from the active tab, else the most recent capture that has it. */
  capturedImage(imageId: number): CapturedTexture | null {
    const active = this.activeView?.data.imageContents(imageId);
    if (active) return active;
    for (let i = this._views.length - 1; i >= 0; i--) {
      const t = this._views[i].data.imageContents(imageId);
      if (t) return t;
    }
    return null;
  }

  /** A line for the bar's status, from outside the panel (the Xcode trace result). */
  setStatus(text: string): void {
    this._statusLabel.text = text;
  }

  /** A session showing a capture file: nothing to capture, only save and inspect. */
  setFileMode(): void {
    for (const w of this._captureControls) w.style.display = "none";
  }

  private _build(): void {
    const bar = new Div(this.parent, { class: "control-bar capture-bar" });
    const row = new Div(bar, { class: "launch-row" });
    const c = this._captureControls;
    c.push(new Button(row, { label: "Capture", class: "btn btn-success", callback: () => this.capture() }));
    c.push(new Span(row, { text: "Frames", class: "launch-label" }));
    this._frameCountInput = new TextInput(row, { value: "1", class: "launch-input launch-input-narrow" });
    c.push(new Span(row, { text: "At frame", class: "launch-label", tooltip: "Capture that frame of the application (the layer's present counter, 0 = the first frame; a frame already passed captures the next one). Empty: the next frame." }));
    this._atFrameInput = new TextInput(row, { value: "", placeholder: "next", class: "launch-input launch-input-narrow" });
    c.push(this._atFrameInput);
    this._texturesCheck = new Checkbox(row, { label: "Render targets", checked: true, tooltip: "Read back render pass attachments at the end of each pass" });
    this._buffersCheck = new Checkbox(row, { label: "Buffers", checked: true, tooltip: "Read back the buffers bound by descriptor sets, vertex and index bindings and indirect draws" });
    this._imagesCheck = new Checkbox(row, { label: "Images", checked: true, tooltip: "Read back the images bound by descriptor sets (sampled and storage images, once per image view, up to 256 MB per capture), so the capture shows what the shaders sampled" });
    this._profileCheck = new Checkbox(row, { label: "Profile passes", checked: true, tooltip: "Write GPU timestamps around every render pass: pass durations, the pass timeline and the Frame Bound card in Frame Stats" });
    this._stacksCheck = new Checkbox(row, { label: "Stack traces", checked: false, tooltip: "Record the call stack of every command of the captured frame (a Stack trace section in the command's details). Costs CPU time in the target while capturing." });
    c.push(this._frameCountInput, this._texturesCheck, this._buffersCheck, this._imagesCheck, this._profileCheck, this._stacksCheck);
    // macOS: the next frame as an Xcode GPU trace document, for the shader debugger and profiler
    // this tool does not have (metal/src/gpu_trace.mm). Written beside the Desktop; the Log
    // tab says where.
    if (getHostPlatform() === "darwin") {
      // metal/src/overdraw.h. A Vulkan capture's overdraw comes from vkinsp_replay --overdraw instead.
      this._overdrawCheck = new Checkbox(row, { label: "Overdraw", checked: false, tooltip: "Metal: draw every render pass a second time with a counting fragment shader, and show how many fragments landed on each pixel (with the pass's depth and stencil tests, and without). Costs GPU and CPU time in the captured frame." });
      c.push(this._overdrawCheck);
      c.push(new Button(row, { label: "Xcode Trace", class: "btn", tooltip: "Write the next frame as a .gputrace document, to open in Xcode's Metal debugger (shader debugging and per-line profiling of the same frame). The path is in the Log tab.", callback: () => {
        if (!this.window.connected) {
          this._statusLabel.text = "not connected";
          return;
        }
        this._statusLabel.text = "writing Xcode trace of the next frame...";
        void this.window.send({ action: "SaveGpuTrace" });
      } }));
    }
    c.push(new Span(row, { text: "Max KB", class: "launch-label", tooltip: "Bytes captured per bound buffer range; longer ranges are truncated" }));
    this._bufferSizeInput = new TextInput(row, { value: "128", class: "launch-input launch-input-narrow" });
    c.push(this._bufferSizeInput);
    this._saveButton = new Button(row, { html: ICON_SAVE, class: "btn btn-icon", tooltip: "Save the capture in the active tab to a file (.gpucap)", disabled: true, callback: () => void this.saveActive() });
    this._statusLabel = new Span(row, { text: "", class: "launch-status" });

    this._tabs = new TabWidget(this.parent, { class: "capture-tabs tabs-fill", displayCloseButton: true });
    this._tabs.onTabClosed.addListener((panel) => this._tabClosed(panel));
    this._tabs.onActiveTabChanged.addListener(() => this._updateStatus());
    this._placeholder = new Div(this.parent, { class: "main-placeholder" });
    new Div(this._placeholder, { text: "No capture yet. Launch an application and press Capture. Each capture opens in its own tab.", class: "text-muted" });
    this._updatePlaceholder();
  }

  /**
   * Requests a capture in a new tab. `atFrame` captures that frame of the application (0 = the
   * first frame; a frame already passed captures the next one) instead of the next frame.
   */
  capture(frames?: number, atFrame?: number, stacks?: boolean): void {
    if (!this.window.connected) {
      this._statusLabel.text = "not connected";
      return;
    }
    if (frames && frames > 0) this._frameCountInput.value = String(frames);
    if (stacks !== undefined) this._stacksCheck.checked = stacks;
    if (atFrame === undefined) {
      // The bar's "At frame" field, when filled in; a queued capture passes its own.
      const at = this._atFrameInput.value.trim();
      if (at !== "" && Number.isFinite(Number(at))) atFrame = Math.max(0, Math.floor(Number(at)));
    }
    const view = new CaptureView(this.window, ++this._captureCount, this._profileCheck.checked);
    if (atFrame !== undefined) view.status = `waiting for frame ${atFrame}...`;
    this._addView(view);
    this._live = view;
    this._statusLabel.text = view.status;
    const maxKb = Math.max(1, Number(this._bufferSizeInput.value) || 128);
    void this.window.send({
      action: "Capture",
      frameCount: Math.max(1, Number(this._frameCountInput.value) || 1),
      ...(atFrame !== undefined ? { atFrame: Math.max(0, Math.floor(atFrame)) } : {}),
      captureTextures: this._texturesCheck.checked,
      captureBuffers: this._buffersCheck.checked,
      captureImages: this._imagesCheck.checked,
      profilePasses: this._profileCheck.checked,
      stacktraces: this._stacksCheck.checked,
      maxBufferSize: maxKb * 1024,
      ...(this._overdrawCheck?.checked ? { overdraw: true } : {}),
    });
  }

  /** Opens a loaded capture (a file, or a copy of another tab) in a new tab. */
  openLoaded(capture: LoadedCapture, label?: string): CaptureView {
    const view = new CaptureView(this.window, ++this._captureCount, capture.passTimings.size > 0);
    if (label) view.customLabel = label;
    this._addView(view);
    view.loadCapture(capture);
    this._updateStatus();
    return view;
  }

  private _addView(view: CaptureView): void {
    this._views.push(view);
    const handle = this._tabs.addTab(view.label, view.root);
    this._handles.set(view, handle);
    view.onLabelChanged.addListener(() => { handle.textElement.text = view.label; });
    view.onStatus.addListener(() => { if (this.activeView === view) this._updateStatus(); });
    view.onOpenOverdraw.addListener((key, depthTested) => this._openOverdraw(view, key, depthTested));
    handle.element.oncontextmenu = (e: MouseEvent) => {
      e.preventDefault();
      this._tabs.setHandleActive(handle);
      showContextMenu(e.clientX, e.clientY, this._tabMenu(view));
    };
    this._tabs.setHandleActive(handle);
    this._updatePlaceholder();
  }

  /** Saves the active tab's capture; `path` skips the dialog (testing aid). */
  async saveActive(path?: string): Promise<string | null> {
    const view = this.activeView;
    if (!view || !view.data.commands.length) {
      this._statusLabel.text = "nothing to save";
      return null;
    }
    const bytes = await this._serialize(view);
    if (!bytes) return null;
    const defaultPath = captureFileName(this.window.name, view.data.frame, view.data.frames);
    const saved = await window.inspector.saveFile({ title: "Save capture", defaultPath, filters: CAPTURE_FILE_FILTERS, ...(path ? { path } : {}) }, bytes);
    this._statusLabel.text = saved ? `saved ${saved} (${(bytes.byteLength / (1024 * 1024)).toFixed(1)} MB)` : view.status;
    if (saved) void window.inspector.addRecentCapture(saved);
    return saved;
  }

  private async _serialize(view: CaptureView): Promise<Uint8Array | null> {
    try {
      return await serializeCapture(this.window, view.data, {
        onProgress: (text) => { this._statusLabel.text = text; },
        resolveSymbols: (addresses) => resolveSymbols(this.window, addresses),
      });
    } catch (e) {
      this._statusLabel.text = `save failed: ${(e as Error).message}`;
      return null;
    }
  }

  /** Copies a tab through the file format into a new, independent tab. */
  /** A copy of the capture in a window of its own (serialized, handed to the main process as a temporary file). */
  private async _openInNewWindow(view: CaptureView): Promise<void> {
    const bytes = await this._serialize(view);
    if (!bytes) return;
    const ok = await window.inspector.openCaptureWindow({ data: bytes, name: captureFileName(this.window.name, view.data.frame, view.data.frames) });
    this._statusLabel.text = ok ? view.status : "could not open a window for the capture";
  }

  private async _openInNewTab(view: CaptureView): Promise<void> {
    const bytes = await this._serialize(view);
    if (!bytes) return;
    this.openLoaded(parseCaptureFile(bytes), `${view.label} (copy)`);
  }

  private _handleMessage(msg: LayerMessage): void {
    if (msg.action === "ImageData") {
      // Live image readbacks (descriptor set thumbnails) may be waited for by any capture.
      for (const v of this._views) v.info.handleImageData(msg);
      return;
    }
    this._live?.handleMessage(msg);
  }

  /** Points the capture's overdraw tab at a pass, opening the tab the first time. */
  private _openOverdraw(view: CaptureView, key: OverdrawPassKey, depthTested: boolean): void {
    const existing = this._overdrawTabs.get(view);
    if (existing) {
      existing.view.show(key, depthTested);
      this._tabs.setHandleActive(existing.handle);
      return;
    }
    const overdraw = new OverdrawView({
      data: view.data,
      passLabelOf: (k) => view.passLabelOf(k),
      selectPass: (k) => {
        const own = this._handles.get(view);
        if (own) this._tabs.setHandleActive(own);
        view.selectPass(k);
      },
    }, key, depthTested);
    const handle = this._tabs.addTab(`Overdraw: ${view.label}`, overdraw.root);
    this._overdrawTabs.set(view, { view: overdraw, handle });
    this._tabs.setHandleActive(handle);
  }

  private _tabMenu(view: CaptureView): ContextMenuItem[] {
    const empty = !view.data.commands.length;
    return [
      { label: "Save Capture...", disabled: empty, callback: () => void this.saveActive() },
      { label: "Open in New Tab", disabled: empty, callback: () => void this._openInNewTab(view) },
      { label: "Open in New Window", disabled: empty, callback: () => void this._openInNewWindow(view) },
      { separator: true },
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
    for (const [owner, t] of this._overdrawTabs) {
      if (t.view.root !== panel) continue;
      t.view.dispose();
      this._overdrawTabs.delete(owner);
      this._updateStatus();
      return;
    }
    const view = this._views.find((v) => v.root === panel);
    if (!view) return;
    this._views = this._views.filter((v) => v !== view);
    this._handles.delete(view);
    if (this._live === view) this._live = null;
    // The capture's overdraw tab goes with it.
    const overdraw = this._overdrawTabs.get(view);
    if (overdraw) this._tabs.closeTabHandle(overdraw.handle);
    this._updatePlaceholder();
    this._updateStatus();
  }

  private _updatePlaceholder(): void {
    const empty = this._views.length === 0;
    this._placeholder.style.display = empty ? "" : "none";
    this._tabs.style.display = empty ? "none" : "";
  }

  private _updateStatus(): void {
    const view = this.activeView;
    this._statusLabel.text = view?.status ?? "";
    this._saveButton.disabled = !view || !view.data.commands.length;
  }
}

// ---------------------------------------------------------------------------------------------

/** One capture: its data, command list and command details. `root` is the capture tab's contents. */
/** The GPU counters of a pass as tooltip lines: invocations, then cycles per stage as shares of the total. */
function passCountersText(t: PassTiming, m?: PassMetrics): string {
  const lines: string[] = [];
  // The derived figures first: they are what a bottleneck is described in, and the GPU Bottlenecks
  // report shows the same ones (metal/pass_metrics.ts).
  if (m) {
    if (m.bound) lines.push(`${m.bound === "target" ? "Target write" : m.bound[0].toUpperCase() + m.bound.slice(1)} bound: ${m.boundReason}`);
    if (m.overdraw !== null) lines.push(`overdraw: ${formatRatio(m.overdraw)} ${m.overdrawSource === "measured" ? "fragments passing depth" : "shader runs"} per pixel`);
    const measured = m.measuredOverdraw;
    if (measured?.depthTested && measured.rasterized) {
      lines.push(`measured: ${formatRatio(overdrawAverages(measured.depthTested).perPixel)} passing depth, ${formatRatio(overdrawAverages(measured.rasterized).perPixel)} rasterized per pixel`);
    }
    if (m.fragmentsPerPrimitive !== null) lines.push(`fragments per primitive: ${formatRatio(m.fragmentsPerPrimitive, 1)}`);
    if (m.depthRejectRate !== null) lines.push(`depth and stencil rejected: ${formatPercent(m.depthRejectRate)} of shaded fragments`);
    if (lines.length) lines.push("");
  }
  const c = t.counters ?? {};
  const names: [string, string][] = [["vertexInvocations", "vertex invocations"], ["clipperPrimitivesOut", "primitives out of the clipper"],
    ["fragmentInvocations", "fragment invocations"], ["fragmentsPassed", "fragments passed"], ["computeKernelInvocations", "kernel invocations"]];
  for (const [key, label] of names) if (c[key] !== undefined) lines.push(`${label}: ${c[key].toLocaleString()}`);
  const u = t.utilization ?? {};
  const total = u.totalCycles ?? 0;
  if (total > 0) {
    const share = (key: string, label: string): void => {
      if (u[key] !== undefined) lines.push(`${label}: ${(100 * u[key] / total).toFixed(0)}% of ${total.toLocaleString()} cycles`);
    };
    share("vertexCycles", "vertex");
    share("fragmentCycles", "fragment");
    share("renderTargetCycles", "render target");
  }
  return lines.join("\n");
}

export class CaptureView implements CaptureHost {
  readonly window: SessionContext;
  readonly data = new CaptureData();
  readonly info: CommandInfoView;
  readonly captureIndex: number;
  readonly root: Div;
  status = "capturing...";
  /** Tab label override (a copied tab); the frame label otherwise. */
  customLabel: string | null = null;
  /** Filled from a capture file or a copied tab rather than streamed by the layer. */
  loaded = false;

  readonly onStatus = new Signal<() => void>();
  readonly onLabelChanged = new Signal<() => void>();
  /** A pass's overdraw asked to open in a tab of its own (the panel opens it). */
  readonly onOpenOverdraw = new Signal<(key: OverdrawPassKey, depthTested: boolean) => void>();
  /** Vulkan: the replay measuring overdraw, while it runs or after it failed. */
  private _overdrawRun: { running: boolean; error?: string } | null = null;

  private _listPanel: Div;
  private _infoPanel: Div;
  private _filterInput: TextInput;
  private _filter = "";
  private _rows: CommandRow[] = [];
  /** The frame analysis of the current commands (Frame Issues and the row markers). */
  _analysis: { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } | null = null;
  /** The capture's render graph, built on demand (see renderGraph()). */
  private _renderGraph: RenderGraph | null = null;
  /** Entries of the Reports menu by id, for marking the one being shown. */
  private _reportItems = new Map<string, Div>();
  private _validationListener: (entry: ValidationEntry) => void;
  private _timeline: TimelineWidget;
  private _profile: boolean;
  private _thumbStrip: Div;
  /** Canvases showing a captured texture (thumbnail strip, render target sections), drawn when its data arrives. */
  private _textureCanvases = new Map<CapturedTexture, HTMLCanvasElement[]>();
  /**
   * Pass blocks of the command tree, keyed by passKey(), for durations and the timeline. `row`
   * is the header the timeline scrolls to: the begin command's row for render passes, the block
   * label for compute passes (no command begins one).
   */
  private _passBlocks = new Map<string, { block: collapsible; row: Widget; label: string; frame: number }>();
  private _selectedRow: CommandRow | null = null;
  private _drawCount = 0;
  private _commandBufferPassCounters = new Map<number, number>();
  private _computePassCounters = new Map<number, number>();
  private _refreshTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(win: SessionContext, captureIndex: number, profile = true) {
    this.root = new Div(null, { class: "capture-view" });
    this.window = win;
    this.captureIndex = captureIndex;
    this._profile = profile;
    this.info = new CommandInfoView(this);
    // Messages can arrive after the list is built (the layer resends a message whose command
    // reference moved to the captured recording at the next frame tick).
    this._validationListener = (entry: ValidationEntry) => {
      if (!entry.command) return;
      for (const row of this._rows) {
        const c = row.command;
        if ((c.secondary ?? c.object?.__id) === entry.command.commandBuffer && c.slot === entry.command.slot) this._markValidation(row);
      }
    };
    win.database.onValidationMessage.addListener(this._validationListener);

    const split = new Split(this.root, { direction: Split.Horizontal, position: 700 });
    const pane1 = new Span(split);
    const leftRow = new Div(pane1, { class: "capture-left-row" });
    // Render pass thumbnails to the left of the command list (WebGPU Inspector's frame images strip).
    this._thumbStrip = new Div(leftRow, { class: "capture-thumb-strip" });
    this._thumbStrip.style.display = "none";
    const left = new Div(leftRow, { class: "capture-left" });
    const filterRow = new Div(left, { class: "capture-filter-row" });
    new Span(filterRow, { text: "Filter", class: "inspector-filter-label-sm" });
    this._filterInput = new TextInput(filterRow, { placeholder: "filter...", class: "inspector-filter-input-sm capture-filter-input" });
    this._filterInput.element.oninput = () => {
      this._filter = this._filterInput.value.trim().toLowerCase();
      this._applyCommandFilter();
    };
    this._buildReportsMenu(filterRow);
    // GPU pass timeline (Profile passes): stays at 0 height until timestamp data arrives.
    this._timeline = new TimelineWidget(left);
    this._listPanel = new Div(left, { class: "capture-commands" });
    const pane2 = new Span(split, { style: "flex-grow: 1; overflow: hidden;" });
    this._infoPanel = new Div(pane2, { class: "capture-info" });
    new Div(this._listPanel, { text: "Capturing...", class: "text-muted", style: "padding: 12px;" });

    this.data.onCaptureStatus.addListener((text) => this._setStatus(text));
    this.data.onCommandsComplete.addListener(() => this._renderCommands());
    this.data.onTextureLoaded.addListener((tex) => this._textureLoaded(tex));
    this.data.onTexturesAnnounced.addListener(() => {
      this._updateStatus();
      this._refreshSelection();
      this._buildThumbnails();
    });
    this.data.onBuffersAnnounced.addListener(() => this._updateStatus());
    // Buffer contents stream in after the commands; show them once they are all here (or a
    // little later while they are still arriving, so a slow transfer still updates the view).
    this.data.onBufferLoaded.addListener(() => this._scheduleRefresh());
    this.data.onBuffersComplete.addListener(() => {
      this._updateStatus();
      this._refreshSelection();
    });
    this.data.onPassTimings.addListener(() => this._applyPassTimings());
    // The selected pass's heatmaps, and the measured figures in the pass header tooltips.
    this.data.onOverdraw.addListener(() => {
      this._scheduleRefresh();
      if (this.data.passTimings.size) this._applyPassTimings();
    });
  }

  /** Pass durations into the pass headers and the timeline (Profile passes). */
  private _applyPassTimings(): void {
    const timed: TimelinePassCommand[] = [];
    // The derived per-pass figures, so a pass header says the same as the GPU Bottlenecks report.
    const metrics = new Map<string, PassMetrics>();
    for (const m of collectPassMetrics(this.data, this.window.database).passes) {
      metrics.set(passKey(m.frame, m.commandBuffer, m.passIndex, m.compute), m);
    }
    for (const [key, p] of this._passBlocks) {
      const k = parsePassKey(key);
      const t = this.data.passTiming(k.frame, k.commandBuffer, k.passIndex, k.compute);
      if (!t) {
        p.block.label.text = p.label;
        continue;
      }
      const split = t.vertexMs !== undefined && t.fragmentMs !== undefined ? `  (vertex ${t.vertexMs.toFixed(3)} / fragment ${t.fragmentMs.toFixed(3)})` : "";
      p.block.label.text = `${p.label}  ${t.durationMs.toFixed(3)} ms${split}`;
      p.row.tooltip = passCountersText(t, metrics.get(key));
      timed.push({
        method: k.compute ? "beginComputePass" : "beginRenderPass", startTime: t.startMs, endTime: t.startMs + t.durationMs, duration: t.durationMs,
        args: [{ label: p.label.replace(/^(Render Pass|Rendering|Pass|Compute) \d+: ?/, "") || p.label }], _passIndex: k.passIndex, header: p.row,
      });
    }
    if (!timed.length) {
      if (this._profile && this.data.commands.length) this._timeline.showPlaceholder("Profile passes: no GPU timestamps were received (timestamps unsupported, or the passes' command buffers were not submitted)");
      else this._timeline.clear();
      return;
    }
    timed.sort((a, b) => a.startTime - b.startTime);
    const db = this.window.database;
    this._timeline.setData({ commands: timed, firstTime: timed[0].startTime, budgetMs: db.refreshMs > 0 ? db.refreshMs : db.frameTimeMs });
  }

  /** GPU timing summary of the capture for Frame Stats: span, sum, and the passes sorted by cost. */
  timingSummary(): FrameTimingInfo | null {
    const passes: FrameTimingInfo["passes"] = [];
    let minStart = Infinity;
    let maxEnd = -Infinity;
    let total = 0;
    for (const [key, p] of this._passBlocks) {
      const k = parsePassKey(key);
      const t = this.data.passTiming(k.frame, k.commandBuffer, k.passIndex, k.compute);
      if (!t) continue;
      minStart = Math.min(minStart, t.startMs);
      maxEnd = Math.max(maxEnd, t.startMs + t.durationMs);
      total += t.durationMs;
      const row = p.row;
      passes.push({ label: p.label, durationMs: t.durationMs, startMs: t.startMs, onJump: () => {
        row.element.scrollIntoView({ block: "center" });
        if ("command" in row) row.element.click();   // a command row: select it (a label only scrolls)
      } });
    }
    if (!passes.length) return null;
    passes.sort((a, b) => b.durationMs - a.durationMs);
    const db = this.window.database;
    return { frameMs: db.frameTimeMs, refreshMs: db.refreshMs, refreshSource: db.refreshSource, submitMs: db.submitMs, gpuSpanMs: maxEnd - minStart, gpuTotalMs: total, frames: this.data.frames, passes };
  }

  /** Tab label: the captured frame number(s) once known. */
  get label(): string {
    if (this.customLabel) return this.customLabel;
    const d = this.data;
    if (!d.commands.length && !d.frame) return `Capture ${this.captureIndex}`;
    return d.frames > 1 ? `Frames ${d.frame}-${d.frame + d.frames - 1}` : `Frame ${d.frame}`;
  }

  handleMessage(msg: LayerMessage): void {
    if (this.loaded) return;
    this.data.handleMessage(msg);
    if (msg.action === "CaptureFrameResults") this.onLabelChanged.emit();
  }

  /** Shows a capture from a file or a copied tab (see capture_file.ts). */
  loadCapture(capture: LoadedCapture): void {
    this.loaded = true;
    this.data.load(capture);
    this.onLabelChanged.emit();
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
    this._rows = [];
    this._renderGraph = null;
    // The graph is built here rather than lazily: its rules contribute to Frame Issues and to the
    // finding flags on the command rows, which are put on as the rows are built just below.
    this._analysis = analyzeFrame(this.data, this.window.database, this.renderGraph());
    this._passBlocks.clear();
    this._textureCanvases.clear();
    this._drawCount = 0;
    // Objects this capture references, for the Inspect panel's "used in last capture" filter.
    const db = this.window.database;
    const referenced = new Set<number>();
    for (const c of this.data.commands) {
      if (c.object) referenced.add(c.object.__id);
      if (c.secondary) referenced.add(c.secondary);
      db.collectReferences(c.args, referenced);
      db.collectReferences(c.descriptors, referenced);
    }
    db.setCapturedObjects(referenced);
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
    this._applyCommandFilter();
    this._buildThumbnails();
    if (this.data.passTimings.size) this._applyPassTimings();
    else if (this._profile) this._timeline.showPlaceholder("Profile passes: waiting for GPU timestamps...");
    const first = this._listPanel.element.querySelector(".capture_drawcall") as HTMLElement | null;
    first?.click();
  }

  /** Hides the command rows that do not match the filter text (containers stay, like WebGPU Inspector). */
  private _applyCommandFilter(): void {
    const f = this._filter;
    for (const row of this._rows) {
      const show = !f || row.filterText.includes(f) || String(row.command.index) === f;
      row.element.style.display = show ? "" : "none";
    }
  }

  /** Builds the command tree of one captured frame into `container`. */
  private _renderFrame(frame: number, container: Widget): void {
    const sets = this.data.sets;
    this._commandBufferPassCounters.clear();
    this._computePassCounters.clear();
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
    let inRenderPass = false;

    // Compute passes: a run of dispatches outside a render pass, grouped the way the layer times
    // them (see COMPUTE_PASS_END in ../command_sets.ts; render pass begins, labels, secondaries and the end of the
    // buffer close one too). Each command buffer, secondaries included, counts its own.
    let compute: { block: collapsible; parent: Widget; dispatches: number; label: string } | null = null;
    const closeCompute = (): void => {
      if (!compute) return;
      compute.block.label.text = `${compute.label}  (${compute.dispatches} dispatch${compute.dispatches === 1 ? "" : "es"})`;
      const key = [...this._passBlocks.entries()].find(([, v]) => v.block === compute!.block)?.[0];
      if (key) this._passBlocks.get(key)!.label = compute.block.label.text;
      current = compute.parent;
      compute = null;
    };
    const openCompute = (cbKey: number): void => {
      const index = this._computePassCounters.get(cbKey) ?? 0;
      this._computePassCounters.set(cbKey, index + 1);
      const label = `Compute ${index}`;
      const block = new collapsible(current, { label, collapsed: false, class: "capture_computepass_block" });
      // No command begins a compute pass; the block's label stands in for the header row.
      this._passBlocks.set(passKey(frame, cbKey, index, true), { block, row: block.label, label, frame });
      compute = { block, parent: current, dispatches: 0, label };
      current = block.body;
    };

    const closeSecondary = (): void => {
      closeCompute();
      if (currentSecondary && secondaryParent) current = secondaryParent;
      currentSecondary = 0;
      secondaryParent = null;
    };
    const closeCommandBuffer = (): void => {
      closeSecondary();
      stack.length = 0;
      currentCb = -1;
      inRenderPass = false;
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
      if (sets.SUBMIT.has(cmd.method)) {
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
      if (sets.PASS_BEGIN.has(cmd.method)) {
        closeCompute();
        inRenderPass = true;
        const passIndex = this._commandBufferPassCounters.get(objId) ?? 0;
        this._commandBufferPassCounters.set(objId, passIndex + 1);
        const label = this._passLabel(cmd, passIndex);
        const block = new collapsible(current, { label, collapsed: false, class: "capture_renderpass_block" });
        const row = this._addRow(block.titleBar, cmd, true);
        row.element.dataset.passIndex = String(passIndex);
        // A compute encoder is timed under its own key, so its block has to be filed there too.
        this._passBlocks.set(passKey(frame, objId, passIndex, sets.passIsCompute?.(cmd.method) ?? false), { block, row, label, frame });
        stack.push(current);
        current = block.body;
        continue;
      }
      if (sets.PASS_END.has(cmd.method)) {
        closeSecondary();
        inRenderPass = false;
        this._addRow(current, cmd);
        current = stack.pop() ?? cbBody;
        continue;
      }
      if (sets.COMPUTE_PASS_END.has(cmd.method) || cmd.method === "vkEndCommandBuffer" || sets.LABEL_BEGIN.has(cmd.method) || sets.LABEL_END.has(cmd.method)) closeCompute();
      if (sets.DISPATCH.has(cmd.method) && !inRenderPass) {
        if (!compute) openCompute(cmd.secondary || objId);
        compute!.dispatches++;
      }
      if (sets.LABEL_BEGIN.has(cmd.method)) {
        const name = labelNameOf(cmd);
        const block = new collapsible(current, { label: name, collapsed: false, class: `capture_debugGroup capture_debugGroup${stack.length % 5}` });
        this._addRow(block.titleBar, cmd, true);
        stack.push(current);
        current = block.body;
        continue;
      }
      if (sets.LABEL_END.has(cmd.method)) {
        this._addRow(current, cmd);
        current = stack.pop() ?? cbBody;
        continue;
      }
      const row = this._addRow(current, cmd);
      if (isAction(sets, cmd.method)) {
        row.classList.add("capture_drawcall");
        drawCount++;
      }
    }
    closeCommandBuffer();
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
    const [images, failedImages] = d.sampledImageCounts;
    const targets = d.textures.length - images - failedImages;
    const imageText = images || failedImages ? `, ${images} image${images === 1 ? "" : "s"}${failedImages ? ` (${failedImages} failed)` : ""}` : "";
    this._setStatus(`${which}: ${d.commands.length} commands, ${this._drawCount} draws/dispatches, ${targets} render targets${imageText}${buffers}`);
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
    // Metal: a render pass descriptor names its attachments directly, and the first colour
    // attachment's texture is the best short name for the pass.
    if (a && Array.isArray(a.colorAttachments)) {
      const first = a.colorAttachments.find((c) => isObject(c) && c.texture !== null);
      const target = isObject(first) ? db.getObject(refId(first.texture)) : null;
      const colors = a.colorAttachments.length;
      const depth = isObject(a.depthAttachment) ? " + depth" : "";
      return `Render Pass ${passIndex}: ${target?.name ?? `${colors} color attachment${colors === 1 ? "" : "s"}`}${depth}`;
    }
    if (cmd.method.startsWith("computeCommandEncoder")) return `Compute Pass ${passIndex}`;
    if (cmd.method.startsWith("blitCommandEncoder")) return `Blit Pass ${passIndex}`;
    if (cmd.method.startsWith("resourceStateCommandEncoder")) return `Resource State Pass ${passIndex}`;
    if (cmd.method.startsWith("accelerationStructureCommandEncoder")) return `Acceleration Structure Pass ${passIndex}`;
    return `Pass ${passIndex}`;
  }

  private _addRow(parent: Widget, cmd: CaptureCommand, inline = false): CommandRow {
    const row = new Div(parent, { class: inline ? "capture_command capture_command_inline" : "capture_command" }) as CommandRow;
    row.command = cmd;
    const summary = this._summarizeArgs(cmd);
    row.filterText = `${cmd.method} ${summary}`.toLowerCase();
    new Span(row, { text: `${cmd.index}`, class: "capture_callnum" });
    this._markValidation(row);
    this._markFindings(row);
    new Span(row, { text: cmd.method.replace(/^vk(Cmd)?/, ""), class: "capture_methodName" });
    new Span(row, { text: summary, class: "capture_method_args" });
    row.element.onclick = (e: MouseEvent) => {
      e.stopPropagation();
      this._selectRow(row);
    };
    this._rows.push(row);
    return row;
  }

  /** What the UI tests read through --debug-dump (tools/ui_tests.py): the capture in numbers. */
  debugState(): Record<string, unknown> {
    const sets = this.data.sets;
    const db = this.window.database;
    const d = this.data;
    const draws = d.commands.filter((c) => sets.DRAW.has(c.method) || sets.DISPATCH.has(c.method)).length;
    const passes = d.commands.filter((c) => sets.PASS_BEGIN.has(c.method)).length;
    if (!this._analysis && d.commands.length) this._analysis = analyzeFrame(d, db, this.renderGraph());
    return {
      status: this.status, frame: d.frame, frames: d.frames, commands: d.commands.length, draws, passes,
      textures: d.textures.length, textureErrors: d.textures.filter((t) => !!t.info.error).length,
      texturesLoaded: d.textures.filter((t) => !!t.data).length,
      buffers: d.buffers.size, passTimings: d.passTimings.size,
      overdraw: d.overdraw.length, overdrawCounts: d.overdraw.filter((o) => !!o.data).length,
      // Passes whose GPU counters arrived: what the GPU Bottlenecks report is built from.
      passCounters: [...d.passTimings.values()].filter((t) => t.counters && Object.keys(t.counters).length).length,
      findings: (this._analysis?.findings ?? []).map((f) => ({ rule: f.rule, severity: f.severity, count: f.count, command: f.commandIndex ?? null })),
      renderGraph: d.commands.length ? (() => {
        const g = this.renderGraph();
        return {
          nodes: g.nodes.length, resources: g.resources.length, edges: g.edges.length,
          externalInputs: g.externalInputs.length, unreadNodes: g.unreadNodes.length,
          criticalPath: g.criticalPath.length, warnings: g.warnings.length,
          // Nodes whose pass key does not resolve to a timing when the frame was profiled: the
          // graph's pass grouping drifting from the command tree's shows up here first.
          untimedNodes: d.passTimings.size ? g.nodes.filter((n) => n.passKey && !d.passTimings.has(n.passKey)).length : null,
        };
      })() : null,
      commandsWithStacks: d.commands.filter((c) => c.stack && c.stack.length).length,
      commandsWithValidation: d.commands.filter((c) => db.validationForCommand(c.secondary ?? c.object?.__id, c.slot).length).length,
    };
  }

  /** The frame analysis findings that apply to a command (see vulkan/frame_analysis.ts). */
  frameFindings(cmd: CaptureCommand): FrameFinding[] {
    return this._analysis?.byCommand.get(cmd.index) ?? [];
  }

  /** Marks a row whose command a frame analysis finding applies to (a flag after the call number). */
  private _markFindings(row: CommandRow): void {
    const findings = this.frameFindings(row.command);
    if (!findings.length || row.findingMark) return;
    const worst = findings.reduce((w, f) => (SEVERITY_RANK[f.severity] > SEVERITY_RANK[w] ? f.severity : w), findings[0].severity);
    const mark = new Span(null, { text: "\u2691", class: `capture_finding_mark perf-mark perf-mark-${worst}` });
    mark.tooltip = findings.map((f) => `${f.severity}: ${f.rule}`).join("\n");
    row.element.insertBefore(mark.element, row.element.children[1] ?? null);
    row.findingMark = mark;
  }

  /** Marks a row whose command raised validation messages (the marker sits after the call number). */
  private _markValidation(row: CommandRow): void {
    const cmd = row.command;
    const msgs = this.window.database.validationForCommand(cmd.secondary ?? cmd.object?.__id, cmd.slot);
    if (!msgs.length || row.validationMark) return;
    const sev = worstSeverity(msgs);
    const mark = new Span(null, { text: severityMark(sev), class: `capture_validation_mark validation-sev validation-sev-${sev}` });
    mark.tooltip = msgs.map((m) => validationItemText(m)).join("\n");
    row.element.insertBefore(mark.element, row.element.children[1] ?? null);
    row.validationMark = mark;
    row.classList.add("capture_command_validation");
  }

  private _selectRow(row: CommandRow): void {
    if (this._selectedRow) this._selectedRow.classList.remove("capture_command_selected");
    this._selectedRow = row;
    row.classList.add("capture_command_selected");
    this._markReport(null);
    this._showCommand(row.command);
  }

  selectCommand(index: number): void {
    const row = this._rows.find((r) => r.command.index === index);
    if (!row) return;
    row.element.scrollIntoView({ block: "center" });
    this._selectRow(row);
  }

  /**
   * Opens the selected command's collapsible section whose title contains `text` (--debug-expand,
   * tools/ui_tests.py). The UI tests used to reach these by clicking a screen coordinate, which
   * any change to the capture bar's height silently broke.
   */
  expandSection(text: string): boolean {
    const needle = text.toLowerCase();
    for (const bar of this._infoPanel.element.querySelectorAll(".title_bar")) {
      if (!(bar.textContent ?? "").toLowerCase().includes(needle)) continue;
      const body = bar.parentElement?.querySelector(".collapsible_body");
      if (body?.classList.contains("collapsed")) (bar as HTMLElement).click();
      return true;
    }
    return false;
  }

  private _summarizeArgs(cmd: CaptureCommand): string {
    if (!cmd.args) return "";
    const db = this.window.database;
    const name = (v: ArgValue | undefined): string => {
      const o = db.getObject(refId(v));
      return o ? o.name : "";
    };
    return this.data.sets.summarize?.(cmd, name) ?? "";
  }

  // ---------------------------------------------------------------------------------------
  // Command details

  private _showCommand(cmd: CaptureCommand): void {
    this.info.show(this._infoPanel, cmd);
  }

  /** Replaces the command details with the capture's statistics (WebGPU Inspector's Frame Stats). */
  /**
   * "Analyze Shaders": the pipelines the frame's draws and dispatches used (bound pipeline per
   * command stream and bind point), each stage's SPIR-V analyzed statically (see
   * renderer/vulkan/spirv_analysis.ts), reported worst first in the details panel.
   */
  private async _analyzeShaders(): Promise<void> {
    const status = this._startReport("Analyzing shaders...", "shaders");
    if (!status) return;
    const db = this.window.database;
    const reports: FrameShaderReport[] = [];
    for (const [pipelineId, count] of pipelineUses(this.data)) {
      const pipeline = db.getObject(pipelineId);
      if (!pipeline) continue;
      for (const source of pipelineStages(pipeline, db)) {
        const data = await fetchBlob(this.window, source.object, source.blobIndex);
        reports.push({
          label: `${pipeline.name}: ${stageLabel(source.stage)} ${source.entryPoint}`, objectId: source.object.id, stage: source.stage, uses: count,
          analysis: data ? analyzeSpirvCached(data) : null,
        });
      }
    }
    status.remove();
    if (this._selectedRow) return;   // the user moved on while shaders were fetched
    if (!reports.length) {
      new Div(this._infoPanel, { text: "No pipelines were bound by the frame's draws or dispatches.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    renderFrameReport(this._infoPanel, reports, (id) => this.window.showObject(id));
  }

  /**
   * "Flame Graph": the frame's GPU work by pass, pipeline, shader stage and function, from the
   * pass timings and the static cost model of every shader the frame used (frame_cost_tree.ts).
   */
  private async _showFlameGraph(): Promise<void> {
    const status = this._startReport("Analyzing shaders...", "flame");
    if (!status) return;
    const db = this.window.database;
    const models = new Map<number, StageModel[]>();
    for (const pipelineId of pipelineUses(this.data).keys()) {
      const pipeline = db.getObject(pipelineId);
      if (!pipeline) continue;
      const stages: StageModel[] = [];
      for (const source of pipelineStages(pipeline, db)) {
        const data = await fetchBlob(this.window, source.object, source.blobIndex);
        // Compute stages need the workgroup size (invocations = groups x size), from reflection.
        const reflection = source.stage === "compute" ? await this.window.shaders.get(source.object, source.blobIndex) : null;
        const entry = reflection?.entryPoints.find((e) => e.name === source.entryPoint) ?? reflection?.entryPoints[0] ?? null;
        stages.push({
          stage: source.stage, entryPoint: source.entryPoint, objectId: source.object.id,
          analysis: data ? analyzeSpirvCached(data) : null, workgroupSize: entry?.workgroupSize ?? null,
        });
      }
      models.set(pipelineId, stages);
    }
    status.remove();
    if (this._selectedRow) return;
    if (!models.size) {
      new Div(this._infoPanel, { text: "No pipelines were bound by the frame's draws or dispatches.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    renderFrameFlameGraph(this._infoPanel, {
      data: this.data, db, models,
      onSelectCommand: (index) => this.selectCommand(index),
      onInspect: (id) => this.window.showObject(id),
    });
  }

  /** Clears the details panel for a frame-wide report; null (a note shown instead) without a capture. */
  private _startReport(text: string, report: string): Div | null {
    if (this._selectedRow) this._selectedRow.classList.remove("capture_command_selected");
    this._selectedRow = null;
    this._markReport(report);
    this._infoPanel.html = "";
    if (!this.data.commands.length) {
      new Div(this._infoPanel, { text: "No commands captured yet.", class: "text-muted", style: "padding: 12px;" });
      return null;
    }
    return new Div(this._infoPanel, { text, class: "text-muted", style: "padding: 12px;" });
  }

  private _showStats(): void {
    if (this._selectedRow) this._selectedRow.classList.remove("capture_command_selected");
    this._selectedRow = null;
    this._markReport("stats");
    this._infoPanel.html = "";
    if (!this.data.commands.length) {
      new Div(this._infoPanel, { text: "No commands captured yet.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    const db = this.window.database;
    if (!this._analysis) this._analysis = analyzeFrame(this.data, db, this.renderGraph());
    renderFrameStats(this._infoPanel, new CaptureStatistics().compute(this.data, db), this.timingSummary(),
      { findings: this._analysis.findings, onJump: (index) => this.selectCommand(index) });
  }

  /**
   * The Reports menu: the whole-capture views, which all replace the command details. One menu
   * rather than one button each, so the filter row holds them however many there come to be. The
   * entry whose report is showing is marked, and the mark clears as soon as a command is selected
   * and the details pane is that command's again.
   */
  private _buildReportsMenu(row: Widget): void {
    const container = new Div(row, { class: "menu-container" });
    const button = new Button(container, {
      html: `${ICON_REPORTS}<span>Reports</span><span class="menu-caret">▾</span>`,
      class: "btn btn-sm btn-menu", tooltip: "Reports over the whole capture, instead of one command",
    });
    const menu = new Div(container, { class: "menu-dropdown reports-menu" });
    button.callback = () => menu.classList.toggle("open");
    // `detail` is the one-line gloss under the name; `tooltip` the full sentence on hover, so
    // the menu stays scannable without losing what each report actually contains.
    const reports: { id: string; icon: string; label: string; detail: string; tooltip: string; open: () => void }[] = [
      { id: "stats", icon: ICON_STATS, label: "Frame Stats", open: () => this._showStats(),
        detail: "Commands, passes, bindings, memory, geometry",
        tooltip: "Statistics of the captured frame: commands by kind, passes and attachments, pipelines and stages bound, descriptor sets, memory traffic and geometry" },
      { id: "shaders", icon: ICON_ANALYZE, label: "Analyze Shaders", open: () => void this._analyzeShaders(),
        detail: "Static analysis of every shader the frame used",
        tooltip: "Static performance analysis of every shader the frame's draws and dispatches used, worst first" },
      { id: "flame", icon: ICON_FLAME, label: "Shader Flame Graph", open: () => void this._showFlameGraph(),
        detail: "GPU time by pass, pipeline, stage and function",
        tooltip: "The frame's GPU work by pass, pipeline, shader stage and function: measured pass times with the cost model's split within each" },
      { id: "bottlenecks", icon: ICON_BOTTLENECK, label: "GPU Bottlenecks", open: () => this._showBottlenecks(),
        detail: "What limits each pass, and what to do about it",
        tooltip: "Each pass measured in the terms a bottleneck is described in: which stage it waits on, how many times each pixel is shaded, how large its triangles are, and whether the depth test is rejecting work" },
      { id: "graph", icon: ICON_GRAPH, label: "Render Graph", open: () => this._showRenderGraph(),
        detail: "Passes and the resources connecting them",
        tooltip: "Every pass and the resources it reads and writes: which pass produced each one, the frame's critical path, and what nothing reads" },
      { id: "overdraw", icon: ICON_OVERDRAW, label: "Overdraw", open: () => void this.openOverdraw(),
        detail: "Fragments per pixel of every pass, as heatmaps",
        tooltip: "Every pass's overdraw in a tab of its own: how many fragments landed on each pixel, with and without the depth test, and the counts under the pointer. A Vulkan capture is replayed on this machine's GPU to measure it" },
    ];
    for (const report of reports) {
      const item = new Div(menu, { class: "menu-item reports-menu-item" });
      new Span(item, { html: report.icon, class: "reports-menu-icon" });
      const text = new Div(item, { class: "reports-menu-text" });
      new Div(text, { text: report.label });
      new Div(text, { text: report.detail, class: "reports-menu-detail" });
      item.tooltip = report.tooltip;
      item.element.onclick = () => {
        menu.classList.remove("open");
        report.open();
      };
      this._reportItems.set(report.id, item);
    }
    // Clicking anywhere else closes the menu, the way the theme picker's does.
    document.addEventListener("mousedown", (e) => {
      if (!container.element.contains(e.target as Node)) menu.classList.remove("open");
    });
  }

  /** Marks the menu entry whose report the details pane is showing, or none. */
  private _markReport(id: string | null): void {
    for (const [key, item] of this._reportItems) item.classList.toggle("active", key === id);
  }

  /** "GPU Bottlenecks": what limits each pass, measured (metal/bottleneck_report.ts). */
  private _showBottlenecks(): void {
    if (this._selectedRow) this._selectedRow.classList.remove("capture_command_selected");
    this._selectedRow = null;
    this._markReport("bottlenecks");
    this._infoPanel.html = "";
    if (!this.data.commands.length) {
      new Div(this._infoPanel, { text: "No commands captured yet.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    renderBottleneckReport(this._infoPanel, this.data, this.window.database, (index) => this.selectCommand(index));
  }

  /** "Render Graph": the frame's passes and the resources that connect them (render_graph_view.ts). */
  private _showRenderGraph(): void {
    if (this._selectedRow) this._selectedRow.classList.remove("capture_command_selected");
    this._selectedRow = null;
    this._markReport("graph");
    this._infoPanel.html = "";
    if (!this.data.commands.length) {
      new Div(this._infoPanel, { text: "No commands captured yet.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    renderRenderGraph(this._infoPanel, this.renderGraph(), {
      onSelectCommand: (index) => this.selectCommand(index),
      onInspect: (id) => this.window.showObject(id),
      onShowFrameStats: () => this._showStats(),
    });
  }

  /** The capture's render graph, built once and kept for the Render Graph view and the UI tests. */
  renderGraph(): RenderGraph {
    if (!this._renderGraph) this._renderGraph = frameRenderGraph(this.data, this.window.database);
    return this._renderGraph;
  }

  /** Shows the Frame Stats view (the Frame Issues card) in the details pane. */
  showFrameStats(): void {
    this._showStats();
  }

  /** Opens one of the capture's reports by name (--debug-view, tools/ui_tests.py). */
  showView(name: string): void {
    if (name === "graph" || name === "render-graph") this._showRenderGraph();
    else if (name === "bottlenecks") this._showBottlenecks();
    else if (name === "stats") this._showStats();
    else if (name === "overdraw") void this.openOverdraw();
  }

  /** A pass's label as the command tree shows it, with its frame when the capture has several. */
  passLabelOf(key: OverdrawPassKey): string {
    const label = this._passBlocks.get(passKey(key.frame, key.commandBuffer, key.passIndex))?.label
      ?? `Pass ${key.passIndex} (command buffer ${key.commandBuffer})`;
    return this.data.frames > 1 ? `Frame ${this.data.frame + key.frame}: ${label}` : label;
  }

  /** Selects a pass's begin command. */
  selectPass(key: OverdrawPassKey): void {
    const row = this._passBlocks.get(passKey(key.frame, key.commandBuffer, key.passIndex))?.row;
    if (!row) return;
    row.element.scrollIntoView({ block: "center" });
    if ("command" in row) this._selectRow(row as CommandRow);
  }

  /** Opens the overdraw tab on the first measured pass; a Vulkan capture is measured first. */
  async openOverdraw(): Promise<void> {
    if (!this.data.overdraw.length) {
      if (this.data.api === "metal") {
        this._setStatus("this capture did not measure overdraw: capture again with Overdraw ticked");
        return;
      }
      if (!(await this.measureOverdraw())) return;
    }
    const first = (this.data.overdraw.find((o) => o.info.measured !== false) ?? this.data.overdraw[0])?.info;
    if (first) this.onOpenOverdraw.emit({ frame: first.frame, commandBuffer: first.commandBuffer, passIndex: first.passIndex }, true);
    else this._setStatus("the replay measured no pass");
  }

  /**
   * Vulkan: replays the capture on this machine's GPU with vkinsp_replay, which draws every pass
   * again with a counting fragment shader (docs/REPLAY.md), and takes its measurements. `open`
   * opens that pass's overdraw tab afterwards.
   */
  async measureOverdraw(open?: OverdrawPassKey): Promise<boolean> {
    if (this._overdrawRun?.running) return false;
    this._overdrawRun = { running: true };
    this._refreshSelection();
    this._setStatus("measuring overdraw: replaying the capture on this machine's GPU...");
    try {
      const bytes = await serializeCapture(this.window, this.data, { forReplay: true });
      const result = await window.inspector.measureOverdraw({ data: bytes, name: this.label });
      if (!result.data) throw new Error(result.error ?? "the replay wrote no overdraw data");
      const file = parseOverdrawFile(result.data);
      this._overdrawRun = null;
      this.data.overdraw = file.measurements;
      this.data.onOverdraw.emit();
      this._updateStatus();
      if (open) this.onOpenOverdraw.emit(open, true);
      return true;
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      this._overdrawRun = { running: false, error: message };
      this._refreshSelection();
      this._setStatus(`overdraw not measured: ${message.split("\n")[0]}`);
      return false;
    }
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
    } else {
      const strip = new Div(grp.body, { class: "capture_frameImages" });
      for (const tex of textures) this._renderTexture(strip, tex);
    }
    this._renderPassOverdraw(container, frame, commandBufferId, passIndex);
  }

  /**
   * The pass's overdraw heatmaps, when the capture has them: a Metal capture measured while it was
   * taken (metal/src/overdraw.h), a Vulkan one replayed (measureOverdraw). A heatmap opens in the
   * capture's overdraw tab.
   */
  private _renderPassOverdraw(container: Widget, frame: number, commandBufferId: number, passIndex: number): void {
    const key: OverdrawPassKey = { frame, commandBuffer: commandBufferId, passIndex };
    const measurements = this.data.overdrawForPass(frame, commandBufferId, passIndex);
    if (!measurements.length) {
      if (this.data.api !== "metal") this._renderOverdrawReplay(container, key);
      return;
    }
    const grp = new collapsible(container, { label: "Overdraw", collapsed: false });
    new Button(grp.body, { label: "Open in Tab", class: "btn btn-sm", tooltip: "The pass's overdraw in a tab of its own: zoom, the render target underneath, and the counts under the pointer",
      callback: () => this.onOpenOverdraw.emit(key, true) });
    const strip = new Div(grp.body, { class: "capture_frameImages" });
    for (const o of measurements) {
      const box = new Div(strip, { class: "capture_pass_texture" });
      new Div(box, { class: "capture-texture-title", text: o.info.depthTested ? "Fragments passing depth and stencil" : "Every rasterized fragment" });
      new Div(box, { text: overdrawSummary(o.info), class: "text-muted font-sm" });
      if (!isMeasured(o.info)) continue;
      const histogram = overdrawHistogramText(o.info);
      if (histogram) new Div(box, { text: `Pixels by count: ${histogram}`, class: "text-muted font-sm" });
      if (o.info.note) new Div(box, { text: o.info.note, class: "text-muted font-sm" });
      const rgba = overdrawRgba(o);
      if (!rgba) {
        if (o.info.size) new Div(box, { text: "Waiting for the per-pixel counts...", class: "text-muted font-sm" });
        continue;
      }
      const canvas = document.createElement("canvas");
      canvas.className = "capture-texture-canvas";
      canvas.width = o.info.width;
      canvas.height = o.info.height;
      canvas.getContext("2d")!.putImageData(new ImageData(rgba, o.info.width, o.info.height), 0, 0);
      canvas.style.maxWidth = "100%";
      canvas.title = "Click to open in the overdraw tab: zoom, the render target underneath, and the counts under the pointer";
      canvas.onclick = () => this.onOpenOverdraw.emit(key, o.info.depthTested);
      box.element.appendChild(canvas);
    }
    new Div(grp.body, {
      text: "Fragments per pixel: black none, dark blue 1, blue 2, teal 3, green 4, yellow 5-6, orange 7-10, red 11-16, magenta 17-32, white 33 and more. Discarded fragments count, since the counting shader does not discard.",
      class: "text-muted font-sm", style: "padding: 4px 6px;",
    });
  }

  /** A Vulkan pass without measurements: the replay that measures them, or where it stands. */
  private _renderOverdrawReplay(container: Widget, key: OverdrawPassKey): void {
    const grp = new collapsible(container, { label: "Overdraw", collapsed: false });
    const note = (text: string): void => { new Div(grp.body, { text, class: "text-muted font-sm", style: "padding: 2px 0; white-space: pre-wrap;" }); };
    if (this.data.overdraw.length) {
      note("The replay did not measure this pass: it could not rebuild it (the capture's other passes have their overdraw).");
      return;
    }
    if (this._overdrawRun?.running) {
      note("Replaying the capture on this machine's GPU...");
      return;
    }
    note("A Vulkan capture's overdraw is measured by replaying it on this machine's GPU (vkinsp_replay): every pass is drawn again with a counting fragment shader, with and without its depth test.");
    if (this._overdrawRun?.error) note(this._overdrawRun.error);
    new Button(grp.body, { label: "Measure Overdraw", class: "btn btn-sm", tooltip: "Replay the capture and open this pass's overdraw in a tab",
      callback: () => void this.measureOverdraw(key) });
  }

  private _renderTexture(parent: Widget, tex: CapturedTexture): void {
    const db = this.window.database;
    const image = db.getObject(tex.info.id);
    const box = new Div(parent, { class: "capture_pass_texture" });
    const title = new Div(box, { class: "capture-texture-title" });
    new Span(title, { text: `${tex.info.attachment}${tex.info.resolve ? " (resolve)" : ""}: ` });
    if (image) objectLink(title, image, (o) => this.window.showObject(o.id)); else new Span(title, { text: `Image ${tex.info.id}` });
    new Div(box, { text: `${fmt(tex.info.format)} ${tex.info.width}x${tex.info.height}${tex.info.layers > 1 ? ` [${tex.info.layers}]` : ""} ${tex.info.aspect}${msaaNote(tex.info)}`, class: "text-muted font-sm" });
    if (tex.info.error) {
      new Div(box, { text: tex.info.error, class: "text-muted font-sm" });
      return;
    }
    const canvas = this._textureCanvas(tex, "capture-texture-canvas");
    canvas.title = "Click to open in the image viewer (zoom, channels, exposure, texel values)";
    box.element.appendChild(canvas);
    // Clicking the thumbnail opens the full image viewer on the captured pixels, in place.
    let viewer: Div | null = null;
    const toggle = (): void => {
      if (!tex.data) return;
      if (viewer) {
        viewer.remove();
        viewer = null;
        canvas.style.display = "";
        box.classList.remove("open");
        return;
      }
      viewer = new Div(box, { class: "capture-texture-viewer" });
      new Button(viewer, { label: "Close viewer", class: "btn btn-sm", callback: toggle });
      new ImageView(viewer, this.window, image, { info: tex.info, data: tex.data });
      canvas.style.display = "none";
      box.classList.add("open");
    };
    canvas.onclick = toggle;
  }

  /** A canvas showing a captured texture: drawn now when its data is here, else when it arrives. */
  textureCanvas(tex: CapturedTexture, className: string): HTMLCanvasElement {
    return this._textureCanvas(tex, className);
  }

  private _textureCanvas(tex: CapturedTexture, className: string): HTMLCanvasElement {
    const canvas = document.createElement("canvas");
    canvas.className = className;
    if (tex.data) this._drawTexture(canvas, tex);
    else {
      canvas.width = 64;
      canvas.height = 64;
      const list = this._textureCanvases.get(tex) ?? [];
      list.push(canvas);
      this._textureCanvases.set(tex, list);
    }
    return canvas;
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
    if (!tex.data) return;
    for (const canvas of this._textureCanvases.get(tex) ?? []) this._drawTexture(canvas, tex);
    this._textureCanvases.delete(tex);
  }

  // ---------------------------------------------------------------------------------------
  // Thumbnail strip: every render pass's attachments beside the command list; clicking one
  // selects the pass's begin command (WebGPU Inspector's frame images).

  private _buildThumbnails(): void {
    const strip = this._thumbStrip;
    strip.html = "";
    let count = 0;
    for (const [key, block] of this._passBlocks) {
      const k = parsePassKey(key);
      if (k.compute) continue;
      const textures = this.data.texturesForPass(k.frame, k.commandBuffer, k.passIndex);
      if (!textures.length) continue;
      const tile = new Div(strip, { class: "capture-thumb-tile" });
      const frames = this.data.frames > 1 ? `Frame ${this.data.frame + k.frame}  ` : "";
      new Div(tile, { text: `${frames}${block.label.replace(/^(Render Pass|Rendering) /, "Pass ")}`, class: "capture-thumb-label", tooltip: block.label });
      for (const tex of textures) {
        new Div(tile, { text: `${tex.info.attachment}${tex.info.resolve ? " (resolve)" : ""}: ${fmt(tex.info.format).replace(/^VK_FORMAT_/, "")} ${tex.info.width}x${tex.info.height}${msaaNote(tex.info)}`, class: "text-muted font-sm capture-thumb-info" });
        if (tex.info.error) {
          new Div(tile, { text: tex.info.error, class: "text-muted font-sm" });
          continue;
        }
        tile.element.appendChild(this._textureCanvas(tex, "capture-thumb-canvas"));
        count++;
      }
      const row = block.row;
      tile.element.onclick = () => {
        row.element.scrollIntoView({ block: "center" });
        if ("command" in row) row.element.click();
      };
    }
    strip.style.display = count ? "" : "none";
  }
}

/** " 4x MSAA" for multisampled captures (the pixels shown are the resolve of the samples). */
export function msaaNote(info: CaptureTextureInfo): string {
  return info.samples && info.samples > 1 ? ` ${info.samples}x MSAA` : "";
}
