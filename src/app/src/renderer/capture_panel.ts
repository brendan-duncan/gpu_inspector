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
import { renderTimingReport, sampledStretch, timingButtonLabel } from "./timing_view.js";
import { emptyTimingSamples, summarizeSamples, summaryAddresses } from "./timing_samples.js";
import { memoryButtonLabel, renderMemoryCaptureReport } from "./memory_capture_view.js";
import { encodeReplaceRequest, parseReplayedTargets, replayedTargetsSummary, type ReplayedTargets, type ShaderReplacement } from "./shader_replay.js";
import { renderShaderReplay } from "./shader_replay_view.js";
import { emptyMemoryCapture } from "./memory_capture.js";
import { memoryHeaps } from "./memory_heaps.js";
import type { FrameRange } from "./frame_timing.js";
import { CaptureData, isRenderTarget, parsePassKey, passKey, type CapturedOverdraw, type CapturedTexture } from "./capture_data.js";
import { capturedIds, fetchBlob, serializeCapture } from "./capture_file.js";
import { resolveSymbols } from "./stacktrace_view.js";
import { CAPTURE_FILE_FILTERS, captureFileName, parseCaptureFile, type LoadedCapture } from "./capture_format.js";
import { renderFrameReport, type FrameShaderReport } from "./shader_analysis_view.js";
import { renderFrameFlameGraph } from "./frame_flamegraph.js";
import type { StageModel } from "./frame_cost_tree.js";
import { analyzeSpirvCached } from "./vulkan/spirv_analysis.js";
import { pipelineUses, programStages, shaderProgram, stageLabel, stateStages } from "./shader_cache.js";
import { CommandInfoView, type CaptureHost } from "./capture_command_info.js";
import { CaptureStatistics } from "./capture_statistics.js";
import { renderFrameStats, SUBMIT_CALL, type FrameTimingInfo, type GpuTrackInput } from "./frame_stats_view.js";
import { buildTimelineTracks, defaultPassLabel, gpuSpan, submitToFirstPassMs, type LabelledPass } from "./timeline_tracks.js";
import { accelerationScene, structureDrawing, type StructureDrawing } from "./acceleration_scene.js";
import type { AccelerationScene } from "./ray_tracing_view.js";
import { analyzeFrame, type FrameFinding } from "./vulkan/frame_analysis.js";
import { frameRenderGraph } from "./frame_graph.js";
import { renderRenderGraph } from "./render_graph_view.js";
import { renderBottleneckReport } from "./bottleneck_report.js";
import { exportReportHtml } from "./report_export.js";
import { exportFolderName, exportSummaryText, exportsToCpp, parseExportSummary } from "./export_cpp.js";
import { collectPassMetrics, formatPercent, formatRatio, type PassMetrics } from "./pass_metrics.js";
import {
  isMeasured, measuresWhileCapturing, overdrawAverages, overdrawHistogramText, overdrawRgba, overdrawSummary,
  parseOverdrawFile, type OverdrawPassKey,
} from "./overdraw.js";
import { CaptureTextureView, type CaptureTarget, type CaptureTextureOptions } from "./capture_texture_view.js";
import { parseDrawOverlayFile, type DrawOverlay, type DrawOverlayKind } from "./draw_overlay.js";
import { drawState, findPass } from "./draw_state.js";
import { parseMeshFile, type MeshOutput } from "./mesh_output.js";
import { MeshView, type MeshViewOptions } from "./mesh_view.js";
import { AccelerationView, STRUCTURE_TYPES } from "./acceleration_view.js";
import { ShaderDebuggerView, type DebugRequest, type ShaderDebuggerOptions } from "./shader_debugger_view.js";
import { summarizeCpuTimeline } from "./cpu_timeline.js";
import { drawStatsSummary, parseDrawStats } from "./draw_stats.js";
import { hwCountersSummary, parseHwCounters } from "./hw_counters.js";
import type { ShaderMeasureTarget } from "./shader_ablation.js";
import { parsePixelHistory, type PixelHistory, type PixelRequest } from "./pixel_history.js";

/**
 * A tab a capture opens beside its own: a render target (its overlays and pixel history), a draw's
 * mesh, the shader debugger, or one of the whole-capture reports.
 */
interface CaptureSubTab {
  readonly root: Div;
  /** What the tab is called, before the capture's own label ("Overdraw", "Frame Stats"). */
  readonly label: string;
  dispose(): void;
  debugState(): Record<string, unknown>;
}
type SubTabKind = "texture" | "mesh" | "accel" | "debugger" | `report:${string}`;
import type { RenderGraph } from "./render_graph.js";
import { SEVERITY_RANK } from "./vulkan/spirv_analysis.js";
import { TimelineWidget, type TimelinePassCommand } from "./widget/timeline.js";
import { Signal } from "./utils/signal.js";
import { decodeImage } from "./vulkan/texture_decode.js";
import { ImageView } from "./image_view.js";
import { isAction, labelNameOf } from "./command_sets.js";
import { fmt, isObject, num, refId, type VulkanObject } from "./vulkan/vulkan_object.js";
import { d3d12AttributeNames, isD3D12Type } from "./d3d12/d3d12_object.js";
import type { SessionContext } from "./session_panel.js";
import { getHostPlatform } from "./launch_dialog.js";
import type { ArgValue, CaptureCommand, CaptureTextureInfo, LayerMessage, PassTiming } from "../shared/protocol.js";
import type { ValidationEntry } from "./vulkan/object_database.js";
import { severityMark, validationItemText, worstSeverity } from "./validation_text.js";

/**
 * Past this many commands in a frame, a render pass is listed collapsed and its rows are built the
 * first time it is opened. A row is several DOM elements and a formatted argument summary, and a
 * game engine's frame holds hundreds of thousands of commands -- more rows than a browser will
 * build in any useful time. The passes are the frame's structure, so they are what stays.
 */
const LAZY_PASS_COMMANDS = 20000;

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
// Export to C++: what it writes, spelled out. Wider than the other icons (btn-icon-wide), since
// braces with an arrow said "code" and not which, and five characters do not fit a square.
const ICON_EXPORT_CPP = '<svg viewBox="0 0 34 14" aria-label="Export to C++"><path d="M4.2 1.2c-1.5 0-1.8.7-1.8 1.8v2.2c0 .9-.4 1.4-1.2 1.8.8.4 1.2.9 1.2 1.8v2.2c0 1.1.3 1.8 1.8 1.8M29.8 1.2c1.5 0 1.8.7 1.8 1.8v2.2c0 .9.4 1.4 1.2 1.8-.8.4-1.2.9-1.2 1.8v2.2c0 1.1-.3 1.8-1.8 1.8" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/><path d="M12.9 4.5A3.7 3.7 0 1 0 12.9 9.5" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/><path d="M16.2 7h4.6M18.5 4.7v4.6M22.7 7h4.6M25 4.7v4.6" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>';
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

/** Draw overlays: a pass with this many draws or fewer has all of them drawn in the replay the first one needs. */
const OVERLAY_BATCH = 48;
/** The mesh tab: a pass with this many draws or fewer has all of them captured in the replay the first one needs. */
const MESH_BATCH = 16;
/** Stacked squares: fragments landing on the same pixels. */
const ICON_OVERDRAW = '<svg viewBox="0 0 16 16"><rect x="2" y="6.5" width="7.5" height="7.5" fill="none" stroke="currentColor" stroke-width="1.3"/><rect x="4.25" y="4.25" width="7.5" height="7.5" fill="none" stroke="currentColor" stroke-width="1.3"/><rect x="6.5" y="2" width="7.5" height="7.5" fill="currentColor" fill-opacity="0.35" stroke="currentColor" stroke-width="1.3"/></svg>';

/**
 * The Reports menu: the whole-capture views, each of which opens in a tab of its own beside the
 * capture's (see ReportView and CaptureView.openReport). `detail` is the one-line gloss under the
 * name in the menu; `tooltip` the full sentence on hover, so the menu stays scannable without
 * losing what each report actually contains.
 */
const REPORTS: { id: string; icon: string; label: string; detail: string; tooltip: string }[] = [
  { id: "stats", icon: ICON_STATS, label: "Frame Stats",
    detail: "Commands, passes, bindings, memory, geometry",
    tooltip: "Statistics of the captured frame: commands by kind, passes and attachments, pipelines and stages bound, descriptor sets, memory traffic and geometry" },
  { id: "shaders", icon: ICON_ANALYZE, label: "Analyze Shaders",
    detail: "Static analysis of every shader the frame used",
    tooltip: "Static performance analysis of every shader the frame's draws and dispatches used, worst first" },
  { id: "flame", icon: ICON_FLAME, label: "Shader Flame Graph",
    detail: "GPU time by pass, pipeline, stage and function",
    tooltip: "The frame's GPU work by pass, pipeline, shader stage and function: measured pass times with the cost model's split within each" },
  { id: "bottlenecks", icon: ICON_BOTTLENECK, label: "GPU Bottlenecks",
    detail: "What limits each pass, and what to do about it",
    tooltip: "Each pass measured in the terms a bottleneck is described in: which stage it waits on, how many times each pixel is shaded, how large its triangles are, and whether the depth test is rejecting work" },
  { id: "graph", icon: ICON_GRAPH, label: "Render Graph",
    detail: "Passes and the resources connecting them",
    tooltip: "Every pass and the resources it reads and writes: which pass produced each one, the frame's critical path, and what nothing reads" },
  { id: "overdraw", icon: ICON_OVERDRAW, label: "Overdraw",
    detail: "Fragments per pixel, over the pass's render target",
    tooltip: "The pass's render target with its overdraw over it: how many fragments landed on each pixel, with and without the depth test, the counts under the pointer, and the history of any pixel you click. A Metal or D3D12 capture carries what it was taken with; a Vulkan capture is replayed on this machine's GPU to measure it" },
];

/** Tabs of the same kind that are the result of something done rather than a report asked for, so the menu does not list them. */
const RESULT_LABELS: Record<string, string> = { "shader-edit": "Shader Edit" };

/** A report's name, for its tab and for the file it is exported to. */
const reportLabel = (id: string): string => REPORTS.find((r) => r.id === id)?.label ?? RESULT_LABELS[id] ?? id;

/** An arrow leaving a frame: the report in a window of its own. */
const ICON_NEW_WINDOW = '<svg viewBox="0 0 16 16" aria-label="Open in new window"><path d="M8.5 3H3.2v9.8H13V7.5" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/><path d="M9.8 2.6H13.4V6.2M13.4 2.6 8.4 7.6" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>';
/** A page with an arrow leaving it: the report written out as a file. */
const ICON_EXPORT = '<svg viewBox="0 0 16 16" aria-label="Export HTML"><path d="M9 2H4v12h8V5" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/><path d="M8.8 2v3.2H12" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/><path d="M8 7.4v4.4M6.2 10l1.8 1.9L9.8 10" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>';

/**
 * One of the whole-capture reports, in a tab beside the capture's: a header with the report's name
 * and the two things that can be done with a report as a whole, and the report itself below.
 *
 * Reports are tabs rather than the command details pane they used to replace, so that a report and
 * a command's details can be read side by side, a second report can be opened beside the first,
 * and a report can be taken out into a window of its own or written to a file.
 */
class ReportView implements CaptureSubTab {
  readonly root: Div;
  /** Where the report renders; the header is not part of what is exported. */
  readonly body: Div;

  constructor(readonly id: string, readonly label: string, o: { onExport: () => void; onNewWindow: () => void }) {
    this.root = new Div(null, { class: "report-tab" });
    const head = new Div(this.root, { class: "report-tab-head" });
    new Span(head, { text: label, class: "report-tab-title" });
    new Button(head, { html: ICON_NEW_WINDOW, class: "btn btn-sm btn-icon",
      tooltip: "Open in a new window: a copy of this capture in a window of its own, showing this report", callback: o.onNewWindow });
    new Button(head, { html: ICON_EXPORT, class: "btn btn-sm btn-icon",
      tooltip: "Export to HTML: write this report to a standalone file, what is on screen here with the application's styles in it, readable anywhere",
      callback: o.onExport });
    this.body = new Div(this.root, { class: "report-tab-body" });
  }

  dispose(): void {}

  debugState(): Record<string, unknown> {
    return { report: this.id, label: this.label };
  }
}

export class CapturePanel {
  readonly window: SessionContext;
  readonly parent: Widget;

  private _statusLabel!: Span;
  private _timingButton!: Button;
  private _timingPanel!: Div;
  private _timingRunning = false;
  private _sampleCheck: Checkbox | null = null;
  /** Addresses the timing report has asked the library to name, so each is asked for once. */
  private _timingSymbolsAsked = new Set<string>();
  private _memoryButton: Button | null = null;
  private _memoryPanel!: Div;
  private _memoryRunning = false;
  /**
   * The stretch of the timing run the report's figures are of, dragged out on its graph. Held by
   * frame number rather than by position because the ring drops the oldest frames out of the front
   * (see FrameRange), and held here rather than in the view because the report is rebuilt on every
   * batch of frames the layer sends.
   */
  private _timingRange: FrameRange | null = null;
  private _frameCountInput!: TextInput;
  private _atFrameInput!: TextInput;
  private _texturesCheck!: Checkbox;
  private _buffersCheck!: Checkbox;
  private _imagesCheck!: Checkbox;
  private _profileCheck!: Checkbox;
  private _stacksCheck!: Checkbox;
  /** Metal and D3D12: every render pass drawn again to measure its overdraw. */
  private _overdrawCheck: Checkbox | null = null;
  private _drawTimingsCheck: Checkbox | null = null;
  private _bufferSizeInput!: TextInput;
  private _saveButton!: Button;
  private _exportCppButton!: Button;
  /** The live-capture controls of the bar, hidden for capture files. */
  private _captureControls: Widget[] = [];
  private _tabs!: TabWidget;
  private _placeholder!: Div;
  private _views: CaptureView[] = [];
  private _handles = new Map<CaptureView, TabHandle>();
  /** The capture the layer is streaming to (the most recently requested one). */
  private _live: CaptureView | null = null;
  /** The live capture's stream has not ended yet, so the capture library would ignore another request. */
  private _liveStreaming = false;
  private _captureCount = 0;
  /** The tabs each capture opened beside its own, at most one of each kind. */
  private _subTabs = new Map<CaptureView, Map<SubTabKind, { tab: CaptureSubTab; handle: TabHandle }>>();
  /** Counts pixel history requests, so an older replay finishing late does not replace a newer one's result. */
  private _historyRuns = 0;

  constructor(win: SessionContext, parent: Widget) {
    this.window = win;
    this.parent = parent;
    win.database.onOtherMessage.addListener((msg) => this._handleMessage(msg));
    this._build();
  }

  /** The capture shown in the active tab. */
  /** The UI tests' view of every capture tab (tools/ui_tests.py). */
  debugState(): Record<string, unknown>[] {
    return this._views.map((v) => ({
      ...v.debugState(),
      textureTab: this._subTab(v, "texture")?.tab.debugState() ?? null,
      meshTab: this._subTab(v, "mesh")?.tab.debugState() ?? null,
      accelTab: this._subTab(v, "accel")?.tab.debugState() ?? null,
      debuggerTab: this._subTab(v, "debugger")?.tab.debugState() ?? null,
      // The reports open in tabs beside the capture's, in the order they were opened.
      reportTabs: [...(this._subTabs.get(v)?.keys() ?? [])].filter((k) => k.startsWith("report:")).map((k) => k.slice("report:".length)),
    }));
  }

  /** The UI tests' hands on the acceleration structure tabs: a row clicked, a box checked (AccelerationView.debugAction). */
  debugAccelAction(action: string, arg: string): boolean {
    let done = false;
    for (const v of this._views) {
      const tab = this._subTab(v, "accel")?.tab as { debugAction?: (a: string, b: string) => void } | undefined;
      if (tab?.debugAction) {
        tab.debugAction(action, arg);
        done = true;
      }
    }
    return done;
  }

  /** The capture shown in the active tab: its own tab, or a tab it opened beside it. */
  get activeView(): CaptureView | null {
    const index = this._tabs.activeTab;
    const handle = index >= 0 ? this._tabs.tabListElement.children[index] : null;
    if (!handle) return null;
    for (const [view, h] of this._handles) if (h === handle) return view;
    for (const [view, tabs] of this._subTabs) {
      for (const t of tabs.values()) if (t.handle === handle) return view;
    }
    return null;
  }

  /** The most recent capture's data. */
  get data(): CaptureData | null {
    return this._live?.data ?? this.activeView?.data ?? null;
  }

  /**
   * The scene a top level describes, from whichever open capture holds its build
   * (renderer/acceleration_structure.ts). A structure is opaque, so the instances its build read are
   * the only view of it there is.
   */
  accelerationScene(structureId: number): AccelerationScene | null {
    const views = this.activeView ? [this.activeView, ...this._views] : this._views;
    for (const v of views) {
      const scene = accelerationScene(v.data, this.window.database, structureId);
      if (scene) return scene;
    }
    return null;
  }

  /**
   * What an acceleration structure can be shown as, from whichever open capture can show the most
   * of it, and the capture that is: a structure built in one capture and only read in another is
   * worth opening from the first. Null when no capture is open.
   */
  structureDrawing(structureId: number): { drawing: StructureDrawing; view: CaptureView } | null {
    const views = this.activeView ? [this.activeView, ...this._views] : this._views;
    let fallback: { drawing: StructureDrawing; view: CaptureView } | null = null;
    for (const v of views) {
      const drawing = structureDrawing(v.data, this.window.database, structureId);
      if (drawing.positions.length) return { drawing, view: v };
      fallback ??= { drawing, view: v };
    }
    return fallback;
  }

  /** Opens the acceleration structure tab on a structure, in the capture that can show it. */
  openStructure(structureId: number): void {
    const found = this.structureDrawing(structureId);
    if (!found) return;
    this._showCaptureTab(found.view);
    found.view.openStructure(structureId);
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
    // The libraries that measure while capturing (src/metal/src/overdraw.h,
    // src/d3d12/src/overdraw.h) offer it here: a Metal target on macOS, a D3D12 one on Windows. A
    // Vulkan capture's overdraw comes from vkinsp_replay --overdraw afterwards instead, and the
    // Vulkan layer ignores the option, so a session that turns out to be Vulkan loses nothing.
    if (getHostPlatform() === "darwin" || getHostPlatform() === "win32") {
      this._overdrawCheck = new Checkbox(row, { label: "Overdraw", checked: false, tooltip: "Metal and D3D12: draw every render pass a second time with a counting fragment shader, and show how many fragments landed on each pixel (with the pass's depth and stencil tests, and without). Costs GPU and CPU time in the captured frame." });
      c.push(this._overdrawCheck);
    }
    // D3D12 measures its draws while capturing, with queries around each one
    // (src/d3d12/src/capture.cpp). A Vulkan capture is replayed for the same numbers afterwards
    // (Measure draws, vkinsp_replay --draws), so the option is only offered where it is the only
    // way to get them; the Vulkan layer ignores it.
    if (getHostPlatform() === "win32") {
      this._drawTimingsCheck = new Checkbox(row, { label: "Measure draws", checked: false, tooltip: "D3D12: put a timestamp pair, a pipeline statistics query and an occlusion query around every draw and dispatch, so the Shader Flame Graph can split a pass's time between its draws. Costs GPU and CPU time in the captured frame, and a command list recorded before the capture began carries no queries." });
      c.push(this._drawTimingsCheck);
    }
    // macOS: the next frame as an Xcode GPU trace document, for the shader debugger and profiler
    // this tool does not have (src/metal/src/gpu_trace.mm). Written beside the Desktop; the Log
    // tab says where.
    if (getHostPlatform() === "darwin") {
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
    this._exportCppButton = new Button(row, { html: ICON_EXPORT_CPP, class: "btn btn-icon btn-icon-wide", disabled: true,
      tooltip: "Export to C++: write the capture in the active tab as a standalone C++ project that re-creates its objects and runs its frame again, for reproducing a problem outside the application (a driver bug report). Vulkan, Direct3D 12 and Metal captures; the frame is replayed on this machine's GPU to write it.",
      callback: () => void this.exportCppActive() });
    // A timing capture is a different question from a frame capture — minutes of frame times
    // rather than every call of one frame — so it is its own control and its own report.
    this._timingButton = new Button(row, { label: timingButtonLabel(false), class: "btn",
      tooltip: "Record every frame's time and where its CPU went, for as long as it runs. A frame report averages five or six frames together and a hitch is one frame, so this is what finds one.",
      callback: () => this.toggleTiming() });
    c.push(this._timingButton);
    // Call stacks sampled with it (src/vulkan/src/cpu_sampler.h), where the capture library can: the
    // Windows ones. On by default, since the hitch nothing timed explains is the common one; off
    // for a run whose frame times must not be touched at all (a sample stops a thread for microseconds).
    if (getHostPlatform() === "win32") {
      this._sampleCheck = new Checkbox(row, { label: "Sample stacks", checked: true,
        tooltip: "Timing Capture: also sample every thread's call stack 250 times a second, and whether it was running or blocked there. The report then says what each thread was doing in the worst hitch, or in the stretch you drag out. Each sample stops a thread for a few microseconds." });
      c.push(this._sampleCheck);
    }
    // The same shape of question about memory: not what is held now (Inspect's memory view) but
    // what was allocated and freed over a stretch of the run. The Metal library does not record
    // one, so a Mac is not offered it.
    if (getHostPlatform() !== "darwin") {
      this._memoryButton = new Button(row, { label: memoryButtonLabel(false), class: "btn",
        tooltip: "Vulkan and D3D12: record every allocation and free for as long as it runs, and report what made here is still held (a leak, by name), what was made and freed again within a few frames (churn a pool would remove), and which frames allocated most.",
        callback: () => this.toggleMemoryCapture() });
      c.push(this._memoryButton);
    }
    this._statusLabel = new Span(row, { text: "", class: "launch-status" });
    this._timingPanel = new Div(this.parent, { class: "timing-panel" });
    this._timingPanel.element.hidden = true;
    this.window.database.onTimingFrames.addListener(() => this._refreshTiming());
    this._memoryPanel = new Div(this.parent, { class: "timing-panel" });
    this._memoryPanel.element.hidden = true;
    this.window.database.onMemoryEvents.addListener(() => this._refreshMemoryCapture());

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
  capture(frames?: number, atFrame?: number, stacks?: boolean,
          pixelHistory?: { texture: number; x: number; y: number; mip?: number; layer?: number },
          drawOverlay?: { passIndex: number; drawIndex: number },
          meshOutput?: { passIndex: number; drawIndex: number }): CaptureView | null {
    if (!this.window.connected) {
      this._statusLabel.text = "not connected";
      return null;
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
    this._liveStreaming = true;
    view.onCaptureComplete.addListener(() => { if (this._live === view) this._liveStreaming = false; });
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
      ...(this._drawTimingsCheck?.checked ? { drawTimings: true } : {}),
      ...(pixelHistory ? { pixelHistory } : {}),
      ...(drawOverlay ? { drawOverlay } : {}),
      ...(meshOutput ? { meshOutput } : {}),
    });
    return view;
  }

  /** The application went away: a capture that was streaming in will never finish. */
  connectionLost(): void {
    this._liveStreaming = false;
    // Whatever was recording went with the process; what it recorded stays on screen.
    if (this._timingRunning) {
      this._timingRunning = false;
      this._timingButton.text = timingButtonLabel(false);
    }
    if (this._memoryRunning) {
      this._memoryRunning = false;
      if (this._memoryButton) this._memoryButton.text = memoryButtonLabel(false);
      this._refreshMemoryCapture();
    }
  }

  /**
   * Turns capture options off by name ("textures", "buffers", "images", "profile"), for
   * --debug-capture-without: what each read-back costs is measured by leaving it out.
   */
  setCaptureOptions(without: string[]): void {
    for (const name of without) {
      if (name === "textures") this._texturesCheck.checked = false;
      else if (name === "buffers") this._buffersCheck.checked = false;
      else if (name === "images") this._imagesCheck.checked = false;
      else if (name === "profile") this._profileCheck.checked = false;
    }
  }

  /**
   * Turns on the options that are off by default, for --debug-capture-with: "overdraw", "draws"
   * and "stacks". Only the ones the host offers — asking for overdraw on a platform whose capture
   * bar has no such checkbox does nothing, the way ticking it by hand could not.
   */
  setExtraCaptureOptions(on: string[]): void {
    for (const name of on) {
      if (name === "overdraw" && this._overdrawCheck) this._overdrawCheck.checked = true;
      else if (name === "draws" && this._drawTimingsCheck) this._drawTimingsCheck.checked = true;
      else if (name === "stacks") this._stacksCheck.checked = true;
    }
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
    view.onOpenTexture.addListener((target, options) => this._openTexture(view, target, options));
    view.onOpenMesh.addListener((draw, options) => this._openMesh(view, draw, options));
    view.onOpenStructure.addListener((id) => this._openStructure(view, id));
    // Which acceleration structures can be viewed depends on the captures open, and a live
    // capture's builds arrive after its tab does.
    view.data.onCommandsComplete.addListener(() => this.window.structuresChanged());
    this.window.structuresChanged();
    view.onDebugShader.addListener((request, options) => this._openDebugger(view, request, options));
    view.onOpenReport.addListener((report) => this._openReport(view, report));
    view.onOpenReportWindow.addListener((id) => void this._openInNewWindow(view, id));
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
  /**
   * A copy of the capture in a window of its own (serialized, handed to the main process as a
   * temporary file). `report` names one of the reports for the new window to open on it, which is
   * what a report tab's "Open in New Window" asks for: the report there is live, not a snapshot,
   * so the capture goes with it.
   */
  private async _openInNewWindow(view: CaptureView, report?: string): Promise<void> {
    const bytes = await this._serialize(view);
    if (!bytes) return;
    const ok = await window.inspector.openCaptureWindow({
      data: bytes, name: captureFileName(this.window.name, view.data.frame, view.data.frames), ...(report ? { view: report } : {}),
    });
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
    if (msg.action === "AppCaptureRequest") {
      // The application called gpu_inspector_capture (include/gpu_inspector.h): the same capture
      // the button takes, with the bar's options. One already streaming in would make the capture
      // library ignore the request, and this would leave an empty tab behind.
      if (this._liveStreaming) this._statusLabel.text = "the application asked for a capture while one was being taken";
      else this.capture(Math.max(1, Math.floor(msg.frameCount) || 1));
      return;
    }
    this._live?.handleMessage(msg);
  }

  /**
   * Shows one of the capture's render targets in a tab beside the capture's: the image, the pass's
   * overdraw over it when asked, and the history of the pixel clicked (capture_texture_view.ts).
   * One such tab per capture, pointed at whichever target is opened next.
   */
  private _openTexture(view: CaptureView, target: CaptureTarget, options: CaptureTextureOptions = {}): void {
    const existing = this._subTab<CaptureTextureView>(view, "texture");
    if (existing) {
      existing.tab.show(target, options);
      existing.handle.textElement.text = `${existing.tab.label}: ${view.label}`;
      this._tabs.setHandleActive(existing.handle);
      return;
    }
    const db = view.window.database;
    // The view's host needs the tab to hand a history to, which only exists once it is built.
    let tab: CaptureTextureView | null = null;
    tab = new CaptureTextureView({
      data: view.data,
      session: view.window,
      passLabelOf: (k) => view.passLabelOf(k),
      selectPass: (k) => {
        this._showCaptureTab(view);
        view.selectPass(k);
      },
      selectCommand: (index) => {
        this._showCaptureTab(view);
        view.selectCommand(index);
      },
      objectName: (id) => db.getObject(id)?.name ?? `object ${id}`,
      imageObject: (id) => db.getObject(id) ?? null,
      showObject: (id) => view.window.showObject(id),
      followPixel: (request) => { if (tab) this._followPixel(view, tab, request); },
      storedHistory: (request) => view.hasPixelHistory(request),
      captureHistory: (request) => this._captureWithPixelHistory(request),
      measureOverdraw: () => view.measureOverdraw(),
      drawsOfPass: (k) => view.drawsOfPass(k),
      drawOverlay: (command, passDraws) => view.drawOverlay(command, passDraws),
      captureDrawOverlay: (command, kind) => this._captureWithDrawOverlay(view, command, kind),
      debugPixel: (command, x, y) => view.debugShader({ stage: "fragment", command, x, y }),
      // The Overdraw report opens this tab, so it exports the way the reports in tabs of their own do.
      exportHtml: () => { if (tab) void view.exportTabHtml(tab.label, tab.root.element); },
    }, target, options);
    this._addSubTab(view, "texture", tab, `${tab.label}: ${view.label}`);
  }

  /** Shows a draw's mesh in a tab beside the capture's (mesh_view.ts); one such tab per capture, pointed at the next draw opened. */
  private _openMesh(view: CaptureView, draw: CaptureCommand, options: MeshViewOptions = {}): void {
    const existing = this._subTab<MeshView>(view, "mesh");
    if (existing) {
      existing.tab.show(draw, options);
      existing.handle.textElement.text = `${existing.tab.label}: ${view.label}`;
      this._tabs.setHandleActive(existing.handle);
      return;
    }
    const tab = new MeshView({
      data: view.data,
      db: view.window.database,
      passLabelOf: (k) => view.passLabelOf(k),
      passOfDraw: (cmd) => view.passOfDraw(cmd),
      drawsOfPass: (k) => view.drawsOfPass(k),
      selectCommand: (index) => {
        this._showCaptureTab(view);
        view.selectCommand(index);
      },
      meshOutput: (command, passDraws) => view.meshOutput(command, passDraws),
      captureMeshOutput: (command) => this._captureWithMeshOutput(view, command),
      inputNames: (cmd) => view.vertexInputNames(cmd),
      debugVertex: (command, row, stage) => view.debugShader(stage === "in" ? { stage: "vertex", command, vertex: row, instance: 0 } : { stage: "vertex", command, record: row }),
    }, draw, options);
    const entry = this._addSubTab(view, "mesh", tab, `${tab.label}: ${view.label}`);
    // The label follows the draw the tab is stepped to.
    const relabel = new MutationObserver(() => { entry.handle.textElement.text = `${tab.label}: ${view.label}`; });
    relabel.observe(tab.root.element, { childList: true });
  }

  /**
   * Shows an acceleration structure in a tab beside the capture's (acceleration_view.ts); one such
   * tab per capture, pointed at the next structure opened.
   */
  private _openStructure(view: CaptureView, structureId: number): void {
    const existing = this._subTab<AccelerationView>(view, "accel");
    if (existing) {
      existing.tab.show(structureId);
      existing.handle.textElement.text = `${existing.tab.label}: ${view.label}`;
      this._tabs.setHandleActive(existing.handle);
      return;
    }
    const tab = new AccelerationView({
      data: view.data,
      db: view.window.database,
      structures: () => view.structures(),
      showObject: (id) => {
        view.window.showObject(id);
      },
      selectCommand: (index) => {
        this._showCaptureTab(view);
        view.selectCommand(index);
      },
      buildOf: (id) => view.buildCommandOf(id),
    }, structureId);
    const entry = this._addSubTab(view, "accel", tab, `${tab.label}: ${view.label}`);
    // The label follows the structure the tab is pointed at.
    const relabel = new MutationObserver(() => { entry.handle.textElement.text = `${tab.label}: ${view.label}`; });
    relabel.observe(tab.root.element, { childList: true });
  }

  /** Debugs a shader invocation in a tab beside the capture's (shader_debugger_view.ts); one such tab per capture. */
  private _openDebugger(view: CaptureView, request: DebugRequest, options: ShaderDebuggerOptions = {}): void {
    const existing = this._subTab<ShaderDebuggerView>(view, "debugger");
    if (existing) {
      existing.tab.show(request, options);
      existing.handle.textElement.text = `${existing.tab.label}: ${view.label}`;
      this._tabs.setHandleActive(existing.handle);
      return;
    }
    const tab = new ShaderDebuggerView({
      data: view.data,
      db: view.window.database,
      session: view.window,
      passLabelOf: (k) => view.passLabelOf(k),
      passOfDraw: (cmd) => view.passOfDraw(cmd),
      selectCommand: (index) => {
        this._showCaptureTab(view);
        view.selectCommand(index);
      },
      meshOutput: (command) => view.meshOutput(command),
      inputNames: (cmd) => view.vertexInputNames(cmd),
      disassemble: (spirv) => window.inspector.shaderText(spirv, "dis"),
      decompile: (spirv, stage, entryPoint) => window.inspector.decompileForDebugging(spirv, stage, entryPoint),
      // D3D12: the stage's HLSL, from the container or a PDB under the session's symbol directories, compiled to SPIR-V.
      compileHlsl: (bytecode, stage, entryPoint, target) => window.inspector.compileHlslForDebugging(bytecode, stage, entryPoint, target, view.window.symbolDirs),
      fetchBlob: (objectId, index) => view.fetchShaderBlob(objectId, index),
    }, request, options);
    const entry = this._addSubTab(view, "debugger", tab, `${tab.label}: ${view.label}`);
    const relabel = new MutationObserver(() => { entry.handle.textElement.text = `${tab.label}: ${view.label}`; });
    relabel.observe(tab.root.element, { childList: true });
  }

  /**
   * Shows one of the capture's reports in a tab beside the capture's; one tab per report, so a
   * report reopened from the menu comes forward rather than being opened twice. The view has
   * already rendered into it — this only places it and gives its handle the report's menu.
   */
  private _openReport(view: CaptureView, report: ReportView): void {
    const kind: SubTabKind = `report:${report.id}`;
    const existing = this._subTab<ReportView>(view, kind);
    if (existing) {
      existing.handle.textElement.text = `${report.label}: ${view.label}`;
      this._tabs.setHandleActive(existing.handle);
      return;
    }
    const entry = this._addSubTab(view, kind, report, `${report.label}: ${view.label}`);
    entry.handle.element.oncontextmenu = (e: MouseEvent) => {
      e.preventDefault();
      this._tabs.setHandleActive(entry.handle);
      showContextMenu(e.clientX, e.clientY, [
        { label: "Open in New Window", callback: () => void this._openInNewWindow(view, report.id) },
        { label: "Export to HTML...", callback: () => void view.exportReport(report.id) },
        { separator: true },
        { label: "Close", callback: () => this._tabs.closeTabHandle(entry.handle) },
      ]);
    };
  }

  /** Runs a pixel's history for the capture's render target tab (a Vulkan replay, or Metal's own). */
  private _followPixel(view: CaptureView, tab: CaptureTextureView, request: PixelRequest): void {
    const run = ++this._historyRuns;
    tab.setHistoryRunning(request);
    view.pixelHistory(request).then(
      (h: PixelHistory) => { if (run === this._historyRuns) tab.setHistoryResult(h); },
      (e: unknown) => { if (run === this._historyRuns) tab.setHistoryError(e instanceof Error ? e.message : String(e)); },
    );
  }

  /**
   * Metal and D3D12: captures the application's next frame with the library following the pixel
   * (src/metal/src/pixel_history.mm, src/d3d12/src/pixel_history.cpp), and shows the history in the
   * new capture's tab when it arrives. A pixel of a Metal drawable, or of a D3D12 swap chain's back
   * buffer, follows whichever one that frame renders into.
   */
  /**
   * Testing aid (--debug-view=pixel-history on Metal or D3D12): ask for the second capture the
   * history needs. On those backends the library follows the pixel while it captures, so the first
   * capture only names one — the history comes from capturing the next frame, which is what the
   * "Capture Next Frame" button in the render target tab does. The pixel is the center of the
   * first color target, the same one `showView("pixel-history")` opened the tab on.
   */
  debugCaptureHistory(): void {
    const view = this.activeView;
    if (!view || !measuresWhileCapturing(view.data.api) || view.data.pixelHistory) return;
    const t = view.data.textures.find((x) => isRenderTarget(x.info) && x.info.aspect === "color" && !x.info.error);
    if (!t) return;
    this._captureWithPixelHistory({ image: t.info.id, x: t.info.width >> 1, y: t.info.height >> 1, mip: t.info.mip, layer: 0 });
  }

  /**
   * D3D12: the overlay of one draw, measured while the application's next frame records
   * (src/d3d12/src/draw_overlay.cpp). The draw is named by its pass and its ordinal within it,
   * since the frame captured now numbers its commands from the start; the new capture opens on its
   * own render target tab with the overlay on, the way a Metal pixel history does.
   */
  /**
   * D3D12: one draw's vertex shader outputs, streamed out while the application's next frame
   * records (src/d3d12/src/mesh_output.cpp). As with a draw overlay, the draw is named by its pass
   * and its ordinal within it, and the new capture opens its own mesh tab on the result.
   */
  private _captureWithMeshOutput(from: CaptureView, command: number): void {
    const draw = from.data.commands[command];
    const pass = draw ? from.passOfDraw(draw) : null;
    if (!pass) {
      this._statusLabel.text = "that draw is not in a render pass";
      return;
    }
    const drawIndex = from.drawsOfPass(pass).findIndex((c) => c.index === command);
    if (drawIndex < 0) {
      this._statusLabel.text = "that draw is not one of its pass's";
      return;
    }
    const live = this.capture(undefined, undefined, undefined, undefined, undefined,
                              { passIndex: pass.passIndex, drawIndex });
    if (!live) {
      this._statusLabel.text = "not connected: a D3D12 mesh output is streamed out while the application's next frame is captured";
      return;
    }
    this._statusLabel.text = `capturing the next frame, streaming draw ${drawIndex} of pass ${pass.passIndex} out...`;
    let done = false;
    const finish = (): void => {
      if (done) return;
      const measured = [...live.data.meshOutputs.values()][0];
      if (!measured) return;
      done = true;
      const cmd = live.data.commands[measured.command];
      if (cmd) this._openMesh(live, cmd);
      else this._statusLabel.text = "the new capture does not hold the draw that was streamed out";
    };
    live.data.onMeshOutputs.addListener(finish);
    live.onCaptureComplete.addListener(finish);
  }

  private _captureWithDrawOverlay(from: CaptureView, command: number, kind: DrawOverlayKind): void {
    const draw = from.data.commands[command];
    const pass = draw ? from.passOfDraw(draw) : null;
    if (!pass) {
      this._statusLabel.text = "that draw is not in a render pass";
      return;
    }
    const draws = from.drawsOfPass(pass);
    const drawIndex = draws.findIndex((c) => c.index === command);
    if (drawIndex < 0) {
      this._statusLabel.text = "that draw is not one of its pass's";
      return;
    }
    const attachment = from.data.textures.findIndex((t) => t.info.frame === pass.frame && t.info.commandBuffer === pass.commandBuffer
      && t.info.passIndex === pass.passIndex && t.info.aspect === "color");
    const live = this.capture(undefined, undefined, undefined, undefined, { passIndex: pass.passIndex, drawIndex });
    if (!live) {
      this._statusLabel.text = "not connected: a D3D12 draw overlay is measured while the application's next frame is captured";
      return;
    }
    this._statusLabel.text = `capturing the next frame, measuring draw ${drawIndex} of pass ${pass.passIndex}...`;
    let done = false;
    const finish = (): void => {
      if (done) return;
      const measured = [...live.data.drawOverlays.values()][0];
      if (!measured) return;
      done = true;
      // The overlay belongs to the new capture's own draw, so its render target tab is the one to
      // open: the image beside it is the frame the measurement was taken in. It has to be that
      // draw's own pass's color attachment -- a capture's textures hold the images its shaders
      // sampled as well, and one of those would draw the overlay over the wrong picture entirely.
      const drawn = live.data.commands[measured.command];
      const target = drawn ? live.targetOfDraw(drawn) : null;
      if (target) live.onOpenTexture.emit(target, { overlay: kind, draw: measured.command });
      else this._statusLabel.text = "the new capture has no color render target for that draw";
    };
    live.data.onDrawOverlays.addListener(finish);
    live.onCaptureComplete.addListener(finish);
  }

  private _captureWithPixelHistory(request: PixelRequest): void {
    const live = this.capture(undefined, undefined, undefined,
      { texture: request.image, x: request.x, y: request.y, mip: request.mip ?? 0, layer: request.layer ?? 0 });
    if (!live) {
      this._statusLabel.text = "not connected: a Metal pixel history captures the application's next frame";
      return;
    }
    this._statusLabel.text = `capturing the next frame, following pixel (${request.x}, ${request.y})...`;
    let done = false;
    // The history arrives at the end of the capture's stream: then the new capture's own render
    // target tab opens on the pixel, the way clicking a pixel of a Vulkan capture does.
    const finish = (): void => {
      if (done) return;
      done = true;
      if (!live.data.pixelHistory) {
        this._statusLabel.text = "the capture ended without a pixel history (a capture library built before pixel history, or not a Metal application)";
        return;
      }
      let pixel = request;
      try {
        const h = parsePixelHistory(live.data.pixelHistory);
        pixel = { image: h.image, x: h.x, y: h.y, mip: h.mip, layer: h.layer };
      } catch {
        // the request's own pixel, and the view will say what went wrong
      }
      live.openTextureForPixel(pixel);
    };
    live.data.onPixelHistory.addListener(finish);
    live.onCaptureComplete.addListener(finish);
  }

  /**
   * Starts or stops a timing capture. Starting clears what the last one recorded: two runs of an
   * application are two questions, and a graph spanning both would answer neither.
   */
  /** Starts or stops a timing capture (the button, and --debug-timing). */
  toggleTiming(): void {
    if (!this.window.connected) {
      this._statusLabel.text = "not connected";
      return;
    }
    this._timingRunning = !this._timingRunning;
    if (this._timingRunning) {
      this.window.database.timing.frames.length = 0;
      this.window.database.timingSamples = emptyTimingSamples();
      this._timingSymbolsAsked.clear();
      // Two runs are two questions, and a range dragged out of the last one names frames this one
      // will number again from somewhere else.
      this._timingRange = null;
      this._timingPanel.element.hidden = false;
    }
    this._timingButton.text = timingButtonLabel(this._timingRunning);
    this._statusLabel.text = this._timingRunning ? "recording frame times..." : "";
    void this.window.send({ action: "TimingCapture", start: this._timingRunning, ...(this._timingRunning && this._sampleCheck?.checked ? { sampleHz: 250 } : {}) });
    this._refreshTiming();
  }

  /**
   * Starts or stops a memory capture (the button, and --debug-memory). Starting clears what the
   * last one recorded, as a timing capture does.
   */
  toggleMemoryCapture(): void {
    if (!this.window.connected) {
      this._statusLabel.text = "not connected";
      return;
    }
    this._memoryRunning = !this._memoryRunning;
    if (this._memoryRunning) {
      this.window.database.memoryCapture = emptyMemoryCapture();
      this._memoryPanel.element.hidden = false;
    }
    if (this._memoryButton) this._memoryButton.text = memoryButtonLabel(this._memoryRunning);
    this._statusLabel.text = this._memoryRunning ? "recording allocations..." : "";
    void this.window.send({ action: "MemoryCapture", start: this._memoryRunning });
    this._refreshMemoryCapture();
  }

  private _refreshMemoryCapture(): void {
    if (this._memoryPanel.element.hidden) return;
    const db = this.window.database;
    const heaps = memoryHeaps(db);
    renderMemoryCaptureReport(this._memoryPanel, db.memoryCapture, this._memoryRunning, {
      getObject: (id) => db.getObject(id),
      onInspect: (id) => this.window.showObject(id),
      ...(heaps ? { heapNames: heaps.heaps.map((h) => `Heap ${h.index}${h.deviceLocal ? " (device local)" : ""}`) } : {}),
    });
  }

  private _refreshTiming(): void {
    if (this._timingPanel.element.hidden) return;
    const db = this.window.database;
    // The stacks the report is about to show, named: asked of the library once each, and the
    // report drawn again when the names arrive.
    const stretch = db.timingSamples.samples.length ? sampledStretch(db.timing, this._timingRange) : null;
    const shown = stretch ? summarizeSamples(db.timingSamples, stretch.fromFrame, stretch.toFrame) : null;
    const unnamed = shown ? summaryAddresses(shown).filter((a) => !db.symbols.has(a) && !this._timingSymbolsAsked.has(a)) : [];
    if (unnamed.length && this.window.connected) {
      for (const a of unnamed) this._timingSymbolsAsked.add(a);
      void resolveSymbols(this.window, unnamed).then(() => this._refreshTiming());
    }
    renderTimingReport(this._timingPanel, this.window.database.timing, {
      samples: db.timingSamples,
      symbolOf: (a) => db.symbols.get(a),
      range: this._timingRange,
      onRange: (range) => {
        this._timingRange = range;
        this._refreshTiming();
      },
    });
  }

  private _showCaptureTab(view: CaptureView): void {
    const own = this._handles.get(view);
    if (own) this._tabs.setHandleActive(own);
  }

  private _subTab<T extends CaptureSubTab>(view: CaptureView, kind: SubTabKind): { tab: T; handle: TabHandle } | null {
    return (this._subTabs.get(view)?.get(kind) as { tab: T; handle: TabHandle } | undefined) ?? null;
  }

  private _addSubTab<T extends CaptureSubTab>(view: CaptureView, kind: SubTabKind, tab: T, label: string): { tab: T; handle: TabHandle } {
    const handle = this._tabs.addTab(label, tab.root);
    let tabs = this._subTabs.get(view);
    if (!tabs) {
      tabs = new Map();
      this._subTabs.set(view, tabs);
    }
    const entry = { tab, handle };
    tabs.set(kind, entry);
    this._tabs.setHandleActive(handle);
    return entry;
  }

  /**
   * Writes the active tab to a standalone HTML file: a report's tab, or the render target tab the
   * Overdraw report opens (--debug-export, tools/ui_tests.py). Null when the active tab is not one
   * that can be exported, the capture's own tab among them.
   */
  async exportActive(path?: string): Promise<string | null> {
    const index = this._tabs.activeTab;
    const handle = index >= 0 ? this._tabs.tabListElement.children[index] : null;
    for (const [view, tabs] of this._subTabs) {
      for (const t of tabs.values()) {
        if (t.handle !== handle) continue;
        // A report's header is its tab's, not part of the report; everything else exports whole.
        const element = t.tab instanceof ReportView ? t.tab.body.element : t.tab.root.element;
        return view.exportTabHtml(t.tab.label, element, path);
      }
    }
    return null;
  }

  private _tabMenu(view: CaptureView): ContextMenuItem[] {
    const empty = !view.data.commands.length;
    return [
      { label: "Save Capture...", disabled: empty, callback: () => void this.saveActive() },
      { label: "Export to C++...", disabled: empty || !exportsToCpp(view.data.api), callback: () => void this.exportCppActive() },
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
    for (const [view, tabs] of this._subTabs) {
      for (const [kind, t] of tabs) {
        if (t.tab.root !== panel) continue;
        t.tab.dispose();
        tabs.delete(kind);
        // The capture keeps the report tabs it has open, for the Reports menu's marks.
        if (kind.startsWith("report:")) view.reportClosed(kind.slice("report:".length));
        this._updateStatus();
        return;
      }
    }
    const view = this._views.find((v) => v.root === panel);
    if (!view) return;
    this._views = this._views.filter((v) => v !== view);
    this.window.structuresChanged();
    this._handles.delete(view);
    view.releaseReplay();
    if (this._live === view) this._live = null;
    // The tabs the capture opened go with it.
    for (const t of [...(this._subTabs.get(view)?.values() ?? [])]) this._tabs.closeTabHandle(t.handle);
    this._subTabs.delete(view);
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
    this._exportCppButton.disabled = !view || !view.data.commands.length || !exportsToCpp(view.data.api);
  }

  /**
   * Export to C++: the capture in the active tab written as a standalone C++ project (CaptureView.exportCpp).
   * The project goes into a folder of its own, named after the capture, inside the directory chosen;
   * `parent` skips the dialog.
   */
  async exportCppActive(parent?: string): Promise<string | null> {
    const view = this.activeView;
    if (!view || !view.data.commands.length) {
      this._statusLabel.text = "nothing to export";
      return null;
    }
    if (!exportsToCpp(view.data.api)) {
      this._statusLabel.text = `Export to C++ replays the capture to write it, and there is no replay for a ${view.data.api ?? "capture"} capture`;
      return null;
    }
    // The dialog opens where the last export went: bug reports tend to collect in one place.
    const chosen = parent ?? await window.inspector.chooseFile({ title: "Export to C++: choose where the project's folder goes", directory: true, remember: "exportCpp" });
    if (!chosen) return null;
    // A capture opened from a file is named after the file, which says its frame already.
    const source = this.window.name;
    const name = exportFolderName(/\.gpucap$/i.test(source) ? source : captureFileName(source, view.data.frame, view.data.frames));
    const written = await view.exportCpp(`${chosen.replace(/[\\/]+$/, "")}/${name}`);
    // The project is what the user came for, so it is shown; not when a directory was given
    // (--debug-export-cpp, a test), which has nobody to show it to.
    if (written && !parent) void window.inspector.showFolder(written);
    return written;
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
    if (m.depthRejectRate !== null) {
      lines.push(`depth and stencil rejected: ${formatPercent(m.depthRejectRate)} of shaded fragments`
               + `${m.depthRejectSource === "replay" ? " (measured per draw by the replay)" : ""}`);
    }
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
  /**
   * A render target asked to open in a tab of its own: the image, the pass's overdraw over it, and
   * the history of the pixel clicked (the panel opens it, capture_texture_view.ts).
   */
  readonly onOpenTexture = new Signal<(target: CaptureTarget, options: CaptureTextureOptions) => void>();
  readonly onOpenMesh = new Signal<(draw: CaptureCommand, options: MeshViewOptions) => void>();
  /** Asks the panel for the acceleration structure tab, on this structure. */
  readonly onOpenStructure = new Signal<(structureId: number) => void>();
  /** The shader debugger asked for, on an invocation of a draw or dispatch (shader_debugger_view.ts). */
  readonly onDebugShader = new Signal<(request: DebugRequest, options: ShaderDebuggerOptions) => void>();
  /** A whole-capture report asked to be shown in a tab beside the capture's (the panel places it). */
  readonly onOpenReport = new Signal<(report: ReportView) => void>();
  /** A report tab asked for a window of its own: a copy of the capture there, opened on that report. */
  readonly onOpenReportWindow = new Signal<(reportId: string) => void>();
  /** The capture library marked the end of the capture's stream (CaptureComplete). */
  readonly onCaptureComplete = new Signal<() => void>();
  /** The capture serialized for vkinsp_replay, kept for the next replay of the same capture. */
  private _replayFile: Promise<Uint8Array> | null = null;
  /** The key the main process keeps this capture's replay under; null until a replay is asked for. */
  private _replayKey: string | null = null;
  /** An export to C++ is running: a second one would write into the same folder. */
  private _exportRunning = false;
  /** Vulkan: the replay measuring overdraw, while it runs or after it failed. */
  private _overdrawRun: { running: boolean; error?: string } | null = null;
  /** Vulkan: the replay measuring the frame's draws, while it runs or after it failed. */
  private _drawRun: { running: boolean; error?: string } | null = null;
  /** Vulkan: the replay reading the GPU's hardware counters, while it runs or after it failed. */
  private _hwCounterRun: { running: boolean; error?: string } | null = null;
  /** Replays under way for draw overlays, by each draw they will answer for. */
  private _overlayRuns = new Map<number, Promise<void>>();
  /** Vertex shader outputs replayed so far, by draw, and the replays under way. */
  private _meshes = new Map<number, MeshOutput>();
  private _meshRuns = new Map<number, Promise<void>>();

  private _listPanel: Div;
  private _infoPanel: Div;
  private _filterInput: TextInput;
  private _filter = "";
  private _rows: CommandRow[] = [];
  /**
   * Command buffers and passes listed collapsed (LAZY_PASS_COMMANDS) and the commands each holds,
   * so a jump to a command can open what holds it. A command buffer's entry comes first and its
   * passes add theirs when it is opened, so a command inside both takes two rounds.
   */
  private _lazyBodies: { from: number; to: number; opened: boolean; open: () => void }[] = [];
  /** How long the command list took to build, said in the status when it is worth knowing. */
  private _listMs = 0;
  /** The frame analysis of the current commands (Frame Issues and the row markers). */
  _analysis: { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } | null = null;
  /** The capture's render graph, built on demand (see renderGraph()). */
  private _renderGraph: RenderGraph | null = null;
  /** Entries of the Reports menu by id, for marking the ones whose tab is open. */
  private _reportItems = new Map<string, Div>();
  /** The report tabs this capture has open, by report id (the panel owns the tab, this owns its contents). */
  private _reportTabs = new Map<string, ReportView>();
  /**
   * Counts openings of each report, so a report that gathers its data asynchronously drops its
   * result when the report was closed or opened again while it was working.
   */
  private _reportRuns = new Map<string, number>();
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
    // Depth rejection for a pass the layer could not measure comes from the per-draw measurements.
    this.data.onDrawStats.addListener(() => {
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
    return { frameMs: db.frameTimeMs, refreshMs: db.refreshMs, refreshSource: db.refreshSource, submitMs: db.submitMs, gpuSpanMs: maxEnd - minStart, gpuTotalMs: total, frames: this.data.frames, passes, submitCall: SUBMIT_CALL[this.data.api] };
  }

  /**
   * The GPU half of the Timeline card: every timed pass with its label, and the device tick their
   * starts are measured from. Unlike timingSummary() these keep their recorded order and are not
   * sorted by cost, since the card draws them against a clock.
   */
  gpuTrack(): GpuTrackInput {
    // Every timed pass, named from its command-list block where there is one. Driven by the timings
    // rather than the blocks so that a pass with no block — a command buffer submitted again in a
    // multi-frame capture — is still drawn (see defaultPassLabel).
    const passes: LabelledPass[] = [...this.data.passTimings.entries()].map(([key, timing]) => {
      const block = this._passBlocks.get(key);
      return {
        timing, label: block?.label ?? defaultPassLabel(timing),
        // Clicking the pass's span goes to it in the command list, the way Pass Timings does. A
        // pass with no block has nowhere to go, so its span is left unclickable rather than inert.
        select: block ? () => {
          block.row.element.scrollIntoView({ block: "center" });
          if ("command" in block.row) block.row.element.click();   // a command row: select it (a label only scrolls)
        } : undefined,
      };
    });
    return { passes, originTicks: this.data.passTimingOrigin };
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
    if (msg.action === "CaptureComplete") {
      // What the frame made and released is gone from the application by now: the capture keeps it.
      this.window.database.pinCaptured(capturedIds(this.window.database, this.data));
      this.onCaptureComplete.emit();
    }
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
    const began = performance.now();
    this._listPanel.html = "";
    this._infoPanel.html = "";
    // A report describes the capture that was listed when it was opened. A new one in this tab
    // leaves the open reports stale rather than describing a frame that is no longer here.
    for (const [id, tab] of this._reportTabs) this._staleReport(id, tab);
    this._selectedRow = null;
    this._rows = [];
    this._lazyBodies = [];
    this._renderGraph = null;
    this._replayFile = null;
    this.releaseReplay();
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
    // A frame listed collapsed (LAZY_PASS_COMMANDS) has no draw row to select until something is
    // opened, so the first command buffer and then the first pass in it are opened: the view opens
    // on a draw either way.
    for (let i = 0; i < 2 && !this._listPanel.element.querySelector(".capture_drawcall"); i++) {
      const next = this._lazyBodies.find((b) => !b.opened);
      if (!next) break;
      next.opened = true;
      next.open();
    }
    const first = this._listPanel.element.querySelector(".capture_drawcall") as HTMLElement | null;
    first?.click();
    // A frame of this size takes visible time to list, and the status is where the user is looking.
    this._listMs = performance.now() - began;
    if (this._listMs > 1000) this._updateStatus();
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
    const lazy = commands.length > LAZY_PASS_COMMANDS;

    // Containers: submit -> command buffer -> render pass / debug label groups. The first two are
    // built here; what is inside a command buffer is _fillCommandBuffer's, at once or when the
    // buffer is opened (LAZY_PASS_COMMANDS).
    let submitBody: Widget = container;
    for (let index = 0; index < commands.length; index++) {
      const cmd = commands[index];
      const objId = cmd.object?.__id ?? 0;
      if (sets.SUBMIT.has(cmd.method)) {
        const queue = db.getObject(objId);
        const block = new collapsible(container, { label: `${cmd.method}  ${queue ? queue.name : ""}`, collapsed: false, class: "capture-submit" });
        this._addRow(block.titleBar, cmd, true);
        submitBody = block.body;
        continue;
      }
      // One command buffer: its commands run to the next submit or the next buffer.
      let end = index + 1;
      while (end < commands.length && !sets.SUBMIT.has(commands[end].method) && (commands[end].object?.__id ?? 0) === objId) end++;
      const body = commands.slice(index, end);
      index = end - 1;
      const cbObj = db.getObject(objId);
      const cbBlock = new collapsible(submitBody, { label: cbObj ? cbObj.name : `CommandBuffer ${objId}`, collapsed: false, class: "capture-cmdbuf" });
      if (!lazy) {
        this._drawCount += this._fillCommandBuffer(cbBlock.body, body, frame, objId, false);
        continue;
      }
      for (const c of body) if (isAction(sets, c.method)) this._drawCount++;
      cbBlock.collapsed = true;
      let filled = false;
      const fill = (): void => {
        if (filled) return;
        filled = true;
        this._fillCommandBuffer(cbBlock.body, body, frame, objId, true);
        this._applyCommandFilter();
      };
      cbBlock.onExpanded.addListener(fill);
      this._lazyBodies.push({ from: body[0].index, to: body[body.length - 1].index, opened: false,
        open: (): void => { cbBlock.collapsed = false; fill(); } });
    }
  }

  /**
   * The contents of one command buffer: its render and compute passes, its debug label groups, the
   * commands of a secondary it executes, and everything else as rows. Returns the draws and
   * dispatches in it. `lazy` lists each render pass collapsed and fills it when it is opened.
   */
  private _fillCommandBuffer(container: Widget, commands: CaptureCommand[], frame: number, cbKey: number, lazy: boolean): number {
    const sets = this.data.sets;
    const db = this.window.database;
    const stack: Widget[] = [];     // open pass / label bodies within the command buffer
    let current: Widget = container;
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
    const openCompute = (key: number): void => {
      const index = this._computePassCounters.get(key) ?? 0;
      this._computePassCounters.set(key, index + 1);
      const label = `Compute ${index}`;
      const block = new collapsible(current, { label, collapsed: false, class: "capture_computepass_block" });
      // No command begins a compute pass; the block's label stands in for the header row.
      this._passBlocks.set(passKey(frame, key, index, true), { block, row: block.label, label, frame });
      compute = { block, parent: current, dispatches: 0, label };
      current = block.body;
    };
    const closeSecondary = (): void => {
      closeCompute();
      if (currentSecondary && secondaryParent) current = secondaryParent;
      currentSecondary = 0;
      secondaryParent = null;
    };

    for (let index = 0; index < commands.length; index++) {
      const cmd = commands[index];
      const objId = cmd.object?.__id ?? 0;
      // A list the capture holds no commands of stands for the whole buffer.
      if (index === 0 && cmd.method.startsWith("<")) {
        new Div(container, { text: cmd.method.replace(/[<>]/g, ""), class: "text-muted capture-note" });
        continue;
      }
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
      if (sets.PASS_BEGIN.has(cmd.method)) {
        closeCompute();
        inRenderPass = true;
        const passIndex = this._commandBufferPassCounters.get(cbKey) ?? 0;
        this._commandBufferPassCounters.set(cbKey, passIndex + 1);
        const label = this._passLabel(cmd, passIndex);
        const block = new collapsible(current, { label, collapsed: false, class: "capture_renderpass_block" });
        const row = this._addRow(block.titleBar, cmd, true);
        row.element.dataset.passIndex = String(passIndex);
        // A compute encoder is timed under its own key, so its block has to be filed there too.
        this._passBlocks.set(passKey(frame, cbKey, passIndex, sets.passIsCompute?.(cmd.method) ?? false), { block, row, label, frame });
        stack.push(current);
        current = block.body;
        if (lazy) {
          // The pass's own commands, up to the one that ends it, are counted now and listed when
          // the pass is opened (LAZY_PASS_COMMANDS).
          let end = index + 1;
          while (end < commands.length && !sets.PASS_END.has(commands[end].method)) {
            if (isAction(sets, commands[end].method)) drawCount++;
            end++;
          }
          const body = commands.slice(index + 1, end);
          block.collapsed = true;
          let filled = false;
          const fill = (): void => {
            if (filled) return;
            filled = true;
            this._fillPassBody(block.body, body);
            this._applyCommandFilter();
          };
          block.onExpanded.addListener(fill);
          if (body.length) {
            this._lazyBodies.push({ from: body[0].index, to: body[body.length - 1].index, opened: false,
              open: (): void => { block.collapsed = false; fill(); } });
          }
          index = end - 1;   // the loop takes the command that ends the pass next
        }
        continue;
      }
      if (sets.PASS_END.has(cmd.method)) {
        closeSecondary();
        inRenderPass = false;
        this._addRow(current, cmd);
        current = stack.pop() ?? container;
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
        current = stack.pop() ?? container;
        continue;
      }
      const row = this._addRow(current, cmd);
      if (isAction(sets, cmd.method)) {
        row.classList.add("capture_drawcall");
        drawCount++;
      }
    }
    closeSecondary();
    return drawCount;
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
    const targets = d.textures.filter((t) => isRenderTarget(t.info)).length;
    const imageText = images || failedImages ? `, ${images} image${images === 1 ? "" : "s"}${failedImages ? ` (${failedImages} failed)` : ""}` : "";
    const listed = this._listMs > 1000 ? `, listed in ${(this._listMs / 1000).toFixed(1)}s` : "";
    this._setStatus(`${which}: ${d.commands.length} commands, ${this._drawCount} draws/dispatches, ${targets} render targets${imageText}${buffers}${listed}`);
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
    // Metal: a render pass descriptor names its attachments directly, and the first color
    // attachment's texture is the best short name for the pass.
    if (a && Array.isArray(a.colorAttachments)) {
      const first = a.colorAttachments.find((c) => isObject(c) && c.texture !== null);
      const target = isObject(first) ? db.getObject(refId(first.texture)) : null;
      const colors = a.colorAttachments.length;
      const depth = isObject(a.depthAttachment) ? " + depth" : "";
      return `Render Pass ${passIndex}: ${target?.name ?? `${colors} color attachment${colors === 1 ? "" : "s"}`}${depth}`;
    }
    // D3D12: OMSetRenderTargets / BeginRenderPass carry each target handle resolved to its resource.
    if (a && (cmd.method === "OMSetRenderTargets" || cmd.method === "BeginRenderPass")) {
      const list = cmd.method === "OMSetRenderTargets" ? a.pRenderTargetDescriptors : a.pRenderTargets;
      const targets = Array.isArray(list) ? list.filter(isObject) : [];
      const first = targets.map((t) => (isObject(t.cpuDescriptor) ? t.cpuDescriptor : t)).find((t) => isObject(t.resource));
      const target = first ? db.getObject(refId(first.resource)) : null;
      const depth = isObject(cmd.method === "OMSetRenderTargets" ? a.pDepthStencilDescriptor : a.pDepthStencil) ? " + depth" : "";
      const colors = targets.length;
      return `Render Pass ${passIndex}: ${target?.name ?? `${colors} render target${colors === 1 ? "" : "s"}`}${depth}`;
    }
    if (cmd.method.startsWith("computeCommandEncoder")) return `Compute Pass ${passIndex}`;
    if (cmd.method.startsWith("blitCommandEncoder")) return `Blit Pass ${passIndex}`;
    if (cmd.method.startsWith("resourceStateCommandEncoder")) return `Resource State Pass ${passIndex}`;
    if (cmd.method.startsWith("accelerationStructureCommandEncoder")) return `Acceleration Structure Pass ${passIndex}`;
    return `Pass ${passIndex}`;
  }

  /**
   * The rows of one render pass, built when the pass is first opened (see LAZY_PASS_COMMANDS).
   * Only what can appear inside a render pass is handled here: debug label groups, the commands of
   * a secondary command buffer, and the commands themselves. A dispatch inside a render pass stays
   * in it, so there is no compute block to open, and passes do not nest.
   */
  private _fillPassBody(body: Widget, commands: CaptureCommand[]): void {
    const sets = this.data.sets;
    const db = this.window.database;
    let current: Widget = body;
    const stack: Widget[] = [];
    let currentSecondary = 0;
    let secondaryParent: Widget | null = null;
    for (const cmd of commands) {
      if ((cmd.secondary ?? 0) !== currentSecondary) {
        if (currentSecondary && secondaryParent) current = secondaryParent;
        currentSecondary = 0;
        secondaryParent = null;
        if (cmd.secondary) {
          const sec = db.getObject(cmd.secondary);
          const block = new collapsible(current, { label: `Secondary: ${sec ? sec.name : `CommandBuffer ${cmd.secondary}`}`, collapsed: false, class: "capture-secondary" });
          secondaryParent = current;
          currentSecondary = cmd.secondary;
          current = block.body;
        }
      }
      if (sets.LABEL_BEGIN.has(cmd.method)) {
        const block = new collapsible(current, { label: labelNameOf(cmd), collapsed: false, class: `capture_debugGroup capture_debugGroup${stack.length % 5}` });
        this._addRow(block.titleBar, cmd, true);
        stack.push(current);
        current = block.body;
        continue;
      }
      if (sets.LABEL_END.has(cmd.method)) {
        this._addRow(current, cmd);
        current = stack.pop() ?? body;
        continue;
      }
      const row = this._addRow(current, cmd);
      if (isAction(sets, cmd.method)) row.classList.add("capture_drawcall");
    }
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
      // A shader edited and run in the capture (Compile & Replay): what it did to the render targets.
      shaderReplay: this.shaderReplay ? {
        targets: this.shaderReplay.targets.length, compared: this.shaderReplay.targets.filter((t) => t.compared).length,
        changed: this.shaderReplay.targets.filter((t) => t.differingTexels > 0).map((t) => ({ image: t.image, aspect: t.aspect, differingTexels: t.differingTexels, pixels: t.pixels?.byteLength ?? 0 })),
        problems: this.shaderReplay.problems.length,
      } : null,
      // Shader stages measured by ablation (Measure shader), with what each part saved.
      ablations: d.ablations.map((a) => ({
        pipeline: a.pipeline, stage: a.stage, command: a.command, baselineMs: a.baselineMs, stageMs: a.stageMs,
        parts: a.parts.map((p) => ({ kind: p.kind, name: p.name, functionId: p.functionId ?? null, savedMs: p.savedMs })),
        skipped: a.skipped.length,
      })),
      // Draws measured one by one: the D3D12 capture's own queries, or a replay's (Measure draws).
      drawStats: d.drawStats?.length ?? 0,
      drawStatsTimed: d.drawStats?.filter((s) => s.timed).length ?? 0,
      drawStatsCounted: d.drawStats?.filter((s) => s.counted).length ?? 0,
      // Measurements whose command really is a draw or a dispatch: a library names a command by
      // the slot it took in its list, which is not its index here, and a measurement keyed by the
      // wrong one would be attributed to whatever command happens to sit at that index.
      drawStatsOnDraws: d.drawStats?.filter((s) => {
        const c = d.commands[s.command];
        return !!c && (sets.DRAW.has(c.method) || sets.DISPATCH.has(c.method));
      }).length ?? 0,
      // Passes whose GPU counters arrived: what the GPU Bottlenecks report is built from.
      passCounters: [...d.passTimings.values()].filter((t) => t.counters && Object.keys(t.counters).length).length,
      passDepthRejection: [...d.passTimings.values()].filter((t) => typeof t.counters?.fragmentsPassed === "number").length,
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
      // "Where the CPU went" (frame_stats_view.ts) in numbers: which categories the layer timed
      // and how much went to each. What a case checks is usually that a category is there at all
      // — a frame that compiles has a `pipeline` total, a vsynced one has `acquire` — which no
      // screenshot can assert.
      cpuTimeline: (() => {
        const s = summarizeCpuTimeline(d.cpuTimeline);
        return s ? {
          spanMs: s.spanMs, frames: s.frames, threads: s.threads, dropped: s.dropped, calibrated: s.calibrated,
          categories: s.totals.map((t) => ({ category: t.category, kind: t.kind, calls: t.calls, ms: t.ms })),
        } : null;
      })(),
      // The Timeline card's lanes. `submitToFirstPass` is where a clock relation that is wrong
      // shows itself: the GPU lane is drawn on the CPU's axis, so a bad calibration puts the
      // passes before the submit that issued them or a whole frame away from it, which no total
      // would reveal.
      timelineTracks: (() => {
        const gpu = this.gpuTrack();
        const t = buildTimelineTracks({ timeline: d.cpuTimeline, passes: gpu.passes, originTicks: gpu.originTicks });
        if (!t) return null;
        const span = gpuSpan(t);
        return {
          spanMs: t.spanMs, hasGpu: t.hasGpu, gpuNote: t.gpuNote,
          tracks: t.tracks.map((k) => ({ kind: k.kind, spans: k.spans.length, busyMs: k.busyMs })),
          gpuStartMs: span?.startMs ?? null, gpuEndMs: span?.endMs ?? null,
          submitToFirstPassMs: submitToFirstPassMs(t),
        };
      })(),
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
    this._showCommand(row.command);
  }

  selectCommand(index: number): void {
    let row = this._rows.find((r) => r.command.index === index);
    // The command may be in a command buffer or a pass that has not been listed yet
    // (LAZY_PASS_COMMANDS): open what holds it, the buffer before the pass inside it.
    while (!row) {
      const holder = this._lazyBodies.find((b) => !b.opened && index >= b.from && index <= b.to);
      if (!holder) break;
      holder.opened = true;
      holder.open();
      row = this._rows.find((r) => r.command.index === index);
    }
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
    // The command's details first, then the reports' tabs, which have sections of their own.
    const panels = [this._infoPanel, ...[...this._reportTabs.values()].map((t) => t.body)];
    for (const panel of panels) {
      for (const bar of panel.element.querySelectorAll(".title_bar")) {
        if (!(bar.textContent ?? "").toLowerCase().includes(needle)) continue;
        const body = bar.parentElement?.querySelector(".collapsible_body");
        if (body?.classList.contains("collapsed")) (bar as HTMLElement).click();
        return true;
      }
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

  // ---------------------------------------------------------------------------------------
  // Reports over the whole capture, each in a tab beside this one (see ReportView)

  /**
   * Opens one of the capture's reports: the Reports menu, --debug-view, a report tab's refresh,
   * and a window opened on a report all come through here.
   */
  openReport(id: string): void {
    switch (id) {
      case "stats": this._showStats(); break;
      case "shaders": void this._analyzeShaders(); break;
      case "flame": void this._showFlameGraph(); break;
      case "bottlenecks": this._showBottlenecks(); break;
      case "graph": this._showRenderGraph(); break;
      // Overdraw is the pass's render target with the heat over it, so it opens the target's tab.
      case "overdraw": void this.openOverdraw(); break;
      default: break;
    }
  }

  /**
   * Opens this capture's tab for a report, and returns its body emptied and ready to render into.
   * `status` is shown in it meanwhile by a report that has to fetch shaders first. Null when there
   * is nothing to report on yet, a note having been shown in the tab instead.
   */
  private _reportBody(id: string, status?: string): Div | null {
    let tab = this._reportTabs.get(id);
    if (!tab) {
      tab = new ReportView(id, reportLabel(id), {
        onExport: () => void this.exportReport(id),
        onNewWindow: () => { this.onOpenReportWindow.emit(id); },
      });
      this._reportTabs.set(id, tab);
    }
    tab.body.html = "";
    this._reportRuns.set(id, (this._reportRuns.get(id) ?? 0) + 1);
    this.onOpenReport.emit(tab);
    this._markReports();
    if (!this.data.commands.length) {
      new Div(tab.body, { text: "No commands captured yet.", class: "text-muted", style: "padding: 12px;" });
      return null;
    }
    if (status !== undefined) new Div(tab.body, { text: status, class: "text-muted", style: "padding: 12px;" });
    return tab.body;
  }

  /**
   * True if the report is still the one that opening `run` started: it was not closed, and it was
   * not opened again while this one was fetching what it needed.
   */
  private _reportCurrent(id: string, run: number | undefined): boolean {
    return this._reportTabs.has(id) && this._reportRuns.get(id) === run;
  }

  /** An open report after a new capture arrived in this tab: what it says is of the previous one. */
  private _staleReport(id: string, tab: ReportView): void {
    tab.body.html = "";
    // The run counter moves on, so anything still gathering data for the old capture drops it.
    this._reportRuns.set(id, (this._reportRuns.get(id) ?? 0) + 1);
    const note = new Div(tab.body, { class: "text-muted", style: "padding: 12px;" });
    new Div(note, { text: `This ${tab.label} report was of the previous capture in this tab.` });
    new Button(note, { label: "Rebuild for this capture", class: "btn btn-sm", style: "margin-top: 8px;",
      callback: () => this.openReport(id) });
  }

  /** The panel closed a report's tab: the capture keeps no contents for it any more. */
  reportClosed(id: string): void {
    this._reportTabs.delete(id);
    this._markReports();
  }

  /** Writes a report's tab to a standalone HTML file (report_export.ts); `path` skips the dialog. */
  async exportReport(id: string, path?: string): Promise<string | null> {
    const tab = this._reportTabs.get(id);
    if (!tab) return null;
    return this.exportTabHtml(tab.label, tab.body.element, path);
  }

  /** Writes a tab's contents to a standalone HTML file: a report's, or the render target tab's. */
  async exportTabHtml(label: string, element: HTMLElement, path?: string): Promise<string | null> {
    try {
      const saved = await exportReportHtml({
        title: label, subtitle: `${this.window.name} · ${this.label}`, element,
        fileName: `${label} ${this.label}`, ...(path ? { path } : {}),
      });
      if (saved) this._setStatus(`exported ${saved}`);
      return saved;
    } catch (e) {
      this._setStatus(`export failed: ${(e as Error).message}`);
      return null;
    }
  }

  /**
   * "Analyze Shaders": the pipelines the frame's draws and dispatches used (bound pipeline per
   * command stream and bind point), each stage's SPIR-V analyzed statically (see
   * renderer/vulkan/spirv_analysis.ts), reported worst first.
   */
  private async _analyzeShaders(): Promise<void> {
    const body = this._reportBody("shaders", "Analyzing shaders...");
    if (!body) return;
    const run = this._reportRuns.get("shaders");
    const db = this.window.database;
    const reports: FrameShaderReport[] = [];
    for (const [key, count] of pipelineUses(this.data)) {
      const program = shaderProgram(this.data, db, key);
      if (!program) continue;
      for (const source of programStages(program, db)) {
        const data = await fetchBlob(this.window, source.object, source.blobIndex);
        reports.push({
          label: `${program.name}: ${stageLabel(source.stage)} ${source.entryPoint}`, objectId: source.object.id, stage: source.stage, uses: count,
          analysis: data ? analyzeSpirvCached(data) : null,
        });
      }
    }
    if (!this._reportCurrent("shaders", run)) return;   // closed or reopened while shaders were fetched
    body.html = "";
    if (!reports.length) {
      new Div(body, { text: "No pipelines or shader objects were bound by the frame's draws or dispatches.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    renderFrameReport(body, reports, (id) => this.window.showObject(id));
  }

  /**
   * "Flame Graph": the frame's GPU work by pass, pipeline, shader stage and function, from the
   * pass timings and the static cost model of every shader the frame used (frame_cost_tree.ts).
   */
  private async _showFlameGraph(): Promise<void> {
    const body = this._reportBody("flame", "Analyzing shaders...");
    if (!body) return;
    const run = this._reportRuns.get("flame");
    const db = this.window.database;
    const models = new Map<number, StageModel[]>();
    for (const pipelineId of pipelineUses(this.data).keys()) {
      const program = shaderProgram(this.data, db, pipelineId);
      if (!program) continue;
      const stages: StageModel[] = [];
      for (const source of programStages(program, db)) {
        const data = await fetchBlob(this.window, source.object, source.blobIndex);
        // Compute stages need the workgroup size (invocations = groups x size), from reflection.
        const reflection = source.stage === "compute" ? await this.window.shaders.get(source.object, source.blobIndex) : null;
        const entry = reflection?.entryPoints.find((e) => e.name === source.entryPoint) ?? reflection?.entryPoints[0] ?? null;
        // The cost model reads SPIR-V. A D3D12 stage's is what its HLSL compiles to, which is what
        // the shader debugger steps as well (d3d12/shader_debug.ts): a stage with no source anywhere
        // stays unanalyzed. What is measured by ablation is the DXIL itself.
        const dxil = this.data.api === "d3d12" ? data : null;
        const spirv = dxil ? await this._hlslAsSpirv(source.object.id, source.blobIndex, dxil, source.stage, source.entryPoint) : data;
        stages.push({
          stage: source.stage, entryPoint: source.entryPoint, objectId: source.object.id,
          analysis: spirv ? analyzeSpirvCached(spirv) : null, workgroupSize: entry?.workgroupSize ?? null, spirv,
          ...(dxil ? { dxil } : {}),
        });
      }
      models.set(pipelineId, stages);
    }
    if (!this._reportCurrent("flame", run)) return;
    body.html = "";
    if (!models.size) {
      new Div(body, { text: "No pipelines or shader objects were bound by the frame's draws or dispatches.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    renderFrameFlameGraph(body, {
      data: this.data, db, models,
      onSelectCommand: (index) => this.selectCommand(index),
      onInspect: (id) => this.window.showObject(id),
      // Per-draw and per-shader measurements replay the capture, which a Metal capture cannot be.
      ...(this.data.api === "vulkan" || this.data.api === "d3d12"
        ? { measureDraws: () => this.measureDraws(), measureShader: (t) => this.measureShader(t) } : {}),
    });
  }

  /** A D3D12 stage's HLSL compiled to SPIR-V for the cost model, once per stage; null when it has no source or does not compile. */
  private _hlslSpirv = new Map<string, Promise<Uint8Array | null>>();
  private _hlslAsSpirv(objectId: number, blobIndex: number, dxil: Uint8Array, stage: string, entryPoint: string): Promise<Uint8Array | null> {
    const key = `${objectId}:${blobIndex}:${entryPoint}`;
    let pending = this._hlslSpirv.get(key);
    if (!pending) {
      pending = window.inspector.compileHlslForDebugging(dxil, stage, entryPoint, undefined, this.window.symbolDirs)
        .then((r) => (r.ok && r.spirv ? new Uint8Array(r.spirv) : null), () => null);
      this._hlslSpirv.set(key, pending);
    }
    return pending;
  }

  /** "Frame Stats": the capture in numbers (WebGPU Inspector's Frame Stats). */
  private _showStats(): void {
    const body = this._reportBody("stats");
    if (!body) return;
    const db = this.window.database;
    if (!this._analysis) this._analysis = analyzeFrame(this.data, db, this.renderGraph());
    renderFrameStats(body, new CaptureStatistics().compute(this.data, db), this.timingSummary(),
      { findings: this._analysis.findings, onJump: (index) => this.selectCommand(index) }, this.data.cpuTimeline,
      this.gpuTrack());
  }

  /**
   * The Reports menu (REPORTS): one menu rather than one button each, so the filter row holds them
   * however many there come to be. Each opens in a tab beside the capture's, and the entries whose
   * tab is open are marked.
   */
  private _buildReportsMenu(row: Widget): void {
    const container = new Div(row, { class: "menu-container" });
    const button = new Button(container, {
      html: `${ICON_REPORTS}<span>Reports</span><span class="menu-caret">▾</span>`,
      class: "btn btn-sm btn-menu", tooltip: "Reports over the whole capture, instead of one command. Each opens in a tab of its own.",
    });
    const menu = new Div(container, { class: "menu-dropdown reports-menu" });
    button.callback = () => menu.classList.toggle("open");
    for (const report of REPORTS) {
      const item = new Div(menu, { class: "menu-item reports-menu-item" });
      new Span(item, { html: report.icon, class: "reports-menu-icon" });
      const text = new Div(item, { class: "reports-menu-text" });
      new Div(text, { text: report.label });
      new Div(text, { text: report.detail, class: "reports-menu-detail" });
      item.tooltip = report.tooltip;
      item.element.onclick = () => {
        menu.classList.remove("open");
        this.openReport(report.id);
      };
      this._reportItems.set(report.id, item);
    }
    // Clicking anywhere else closes the menu, the way the theme picker's does.
    document.addEventListener("mousedown", (e) => {
      if (!container.element.contains(e.target as Node)) menu.classList.remove("open");
    });
  }

  /** Marks the menu entries whose report has a tab open, so the menu says what is already there. */
  private _markReports(): void {
    for (const [key, item] of this._reportItems) item.classList.toggle("active", this._reportTabs.has(key));
  }

  /** "GPU Bottlenecks": what limits each pass, measured (metal/bottleneck_report.ts). */
  private _showBottlenecks(): void {
    const body = this._reportBody("bottlenecks");
    if (!body) return;
    renderBottleneckReport(body, this.data, this.window.database, (index) => this.selectCommand(index),
      this.data.api === "vulkan" || this.data.api === "d3d12"
        ? () => this.measureHwCounters().then((ok) => { if (ok) this._showBottlenecks(); return ok; }) : undefined);
  }

  /** "Render Graph": the frame's passes and the resources that connect them (render_graph_view.ts). */
  private _showRenderGraph(): void {
    const body = this._reportBody("graph");
    if (!body) return;
    renderRenderGraph(body, this.renderGraph(), {
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
    else if (name === "shaders") void this._analyzeShaders();
    else if (name === "stats" || name.startsWith("stats:")) {
      // Testing aid (--debug-view=stats[:<card>]): Frame Stats, scrolled to the card whose heading
      // contains the text, since a screenshot otherwise only ever shows the top of the report.
      this._showStats();
      const heading = name.split(":")[1];
      const body = this._reportTabs.get("stats")?.body;
      if (heading && body) {
        const needle = heading.toLowerCase();
        for (const h of body.element.querySelectorAll(".frame-stats-heading")) {
          if (!(h.textContent ?? "").toLowerCase().includes(needle)) continue;
          h.scrollIntoView({ block: "start" });
          break;
        }
      }
    }
    else if (name === "flame" || name === "flamegraph") void this._showFlameGraph();
    else if (name === "shader-edit") void this._debugShaderEdit();
    else if (name.startsWith("flame:")) {
      // Testing aid (--debug-view=flame:draws|shader): the flame graph, then its Measure draws or
      // Measure shader button pressed, which is the whole of what a person does to measure.
      const wanted = name.split(":")[1] === "draws" ? "Measure draws" : "Measure ";
      void this._showFlameGraph().then(() => setTimeout(() => {
        const body = this._reportTabs.get("flame")?.body;
        const buttons = [...(body?.element.querySelectorAll("button") ?? [])] as HTMLButtonElement[];
        const button = buttons.find((b) => (b.textContent ?? "").startsWith(wanted) && (wanted !== "Measure " || !(b.textContent ?? "").startsWith("Measure draws")));
        if (button && !button.disabled) button.click();
        else this._setStatus(`the flame graph offers no "${wanted.trim()}" to press`);
      }, 500));
    }
    else if (name === "overdraw") void this.openOverdraw();
    else if (name.startsWith("accel")) {
      // Testing aid (--debug-view=accel[:<object id>|<name>]): the acceleration structure tab on the
      // capture's first structure that can be drawn, or the one named.
      const [, which] = name.split(":");
      const all = this.structures();
      const named = which === undefined ? null
        : all.find((o) => String(o.id) === which) ?? all.find((o) => o.name.includes(which)) ?? null;
      const drawable = all.find((o) => structureDrawing(this.data, this.window.database, o.id).positions.length > 0);
      const target = named ?? drawable ?? all[0];
      if (target) this.onOpenStructure.emit(target.id);
      else this._setStatus("this capture has no acceleration structures");
    }
    else if (name.startsWith("mesh")) {
      // Testing aid (--debug-view=mesh[:in|out[:<command>|last]]): the mesh tab on the first draw, or the one named.
      const [, stage = "out", at] = name.split(":");
      const draws = this.data.commands.filter((c) => this.data.sets.DRAW.has(c.method));
      const draw = at === "last" ? draws[draws.length - 1] : at !== undefined ? this.data.commands[Number(at)] : draws[0];
      if (draw) this.onOpenMesh.emit(draw, { stage: stage === "in" ? "in" : "out" });
      else this._setStatus("this capture has no draws");
    }
    else if (name.startsWith("debugger")) {
      // Testing aid (--debug-view=debugger[:vertex|pixel|compute[:<command>|last[:<lines>|end[:decompiled]]]]): the shader
      // debugger on the first draw (or dispatch), or the one named, stepped over that many lines or run to the end, on the
      // SPIR-V or on GLSL decompiled from it.
      const [, kind = "pixel", at, steps, code] = name.split(":");
      const compute = kind === "compute";
      const commands = this.data.commands.filter((c) => (compute ? this.data.sets.DISPATCH : this.data.sets.DRAW).has(c.method));
      const cmd = at === "last" ? commands[commands.length - 1] : at !== undefined && at !== "" ? this.data.commands[Number(at)] : commands[0];
      const options: ShaderDebuggerOptions = steps === "end" ? { steps: -1 } : steps !== undefined && steps !== "" ? { steps: Number(steps) } : {};
      if (code === "decompiled") options.decompiled = true;
      if (!cmd) this._setStatus(`this capture has no ${compute ? "dispatches" : "draws"}`);
      else this.debugShader(compute ? { stage: "compute", command: cmd.index } : kind === "vertex" ? { stage: "vertex", command: cmd.index } : { stage: "fragment", command: cmd.index }, options);
    }
    else if (name.startsWith("overlay")) {
      // Testing aid (--debug-view=overlay[:<kind>[:<command>|last]]): a draw overlay, on the first draw of a
      // pass with a render target unless a command (or the last such draw) is named.
      const [, kind = "highlight", at] = name.split(":");
      const kinds: DrawOverlayKind[] = ["highlight", "depth", "stencil", "backface", "wireframe"];
      const overlay = kinds.includes(kind as DrawOverlayKind) ? (kind as DrawOverlayKind) : "highlight";
      const drawn = this.data.commands.filter((c) => this.data.sets.DRAW.has(c.method) && this.targetOfDraw(c));
      const draw = at === "last" ? drawn[drawn.length - 1] : at !== undefined ? this.data.commands[Number(at)] : drawn[0];
      if (draw) this.openDrawOverlay(draw, overlay);
      else this._setStatus("no draw of this capture is in a pass with a render target");
    }
    else if (name.startsWith("target")) {
      // Testing aid (--debug-view=target[:color|depth|<image id>]): a render target in its own tab,
      // which is otherwise only reachable through overdraw or pixel history.
      const [, which = "color"] = name.split(":");
      const id = Number(which);
      const t = this.data.textures.find((x) => isRenderTarget(x.info) && !x.info.error
        && (Number.isFinite(id) ? x.info.id === id : x.info.aspect === which));
      if (t) this.openTextureForPixel({ image: t.info.id, x: t.info.width >> 1, y: t.info.height >> 1, mip: t.info.mip, layer: 0 });
      else this._setStatus(`this capture has no ${which} render target`);
    }
    else if (name === "pixel-history") {
      // Testing aid (--debug-view=pixel-history): the pixel a Metal capture followed, else the
      // center of the first color render target.
      if (this.data.pixelHistory) {
        try {
          const h = parsePixelHistory(this.data.pixelHistory);
          this.openTextureForPixel({ image: h.image, x: h.x, y: h.y, mip: h.mip, layer: h.layer });
          return;
        } catch {
          // shown as the center of a render target instead
        }
      }
      const t = this.data.textures.find((x) => isRenderTarget(x.info) && x.info.aspect === "color" && !x.info.error);
      if (t) this.openTextureForPixel({ image: t.info.id, x: t.info.width >> 1, y: t.info.height >> 1, mip: t.info.mip, layer: 0 });
    }
  }

  /** The captured render target an image belongs to, with the pass that rendered it. */
  private _targetOf(imageId: number, mip?: number): CaptureTarget | null {
    const textures = this.data.textures.filter((t) => isRenderTarget(t.info) && t.info.id === imageId);
    const tex = textures.find((t) => mip === undefined || t.info.mip === mip) ?? textures[0];
    if (!tex) return null;
    return { key: { frame: tex.info.frame, commandBuffer: tex.info.commandBuffer, passIndex: tex.info.passIndex }, texture: tex };
  }

  /** The pass's first color target (what its overdraw heat is drawn over). */
  private _targetOfPass(key: OverdrawPassKey): CaptureTarget | null {
    const textures = this.data.texturesForPass(key.frame, key.commandBuffer, key.passIndex);
    const tex = textures.find((t) => t.info.aspect === "color" && !t.info.resolve && !t.info.error) ?? textures[0];
    return tex ? { key, texture: tex } : null;
  }

  /** The render target a draw's overlay is drawn over: its pass's first color target. */
  targetOfDraw(cmd: CaptureCommand): CaptureTarget | null {
    const pass = findPass(this.data, cmd);
    if (!pass) return null;
    return this._targetOfPass({ frame: cmd.frame ?? 0, commandBuffer: pass.passBegin.object?.__id ?? 0, passIndex: pass.passIndex });
  }

  /** Opens the render target tab with a draw overlay on a draw (Highlight Draw in a draw's render targets). */
  openDrawOverlay(cmd: CaptureCommand, overlay: DrawOverlayKind, target?: CaptureTarget): void {
    const t = target ?? this.targetOfDraw(cmd);
    if (!t) {
      this._setStatus("the draw's pass has no render target read back, so there is nothing to draw the overlay over");
      return;
    }
    this.onOpenTexture.emit(t, { overlay, draw: cmd.index });
  }

  /** Opens the mesh tab on a draw (View Mesh in a draw's details). */
  openMesh(cmd: CaptureCommand): void {
    this.onOpenMesh.emit(cmd, {});
  }

  /** Opens the acceleration structure tab (View Structure, in the Inspect panel or on a build). */
  openStructure(structureId: number): void {
    this.onOpenStructure.emit(structureId);
  }

  /** Every acceleration structure the capture holds, in id order, whichever API took it. */
  structures(): VulkanObject[] {
    const out: VulkanObject[] = [];
    for (const type of STRUCTURE_TYPES) {
      for (const o of this.window.database.getObjectsOfType(type)?.values() ?? []) out.push(o);
    }
    return out.sort((a, b) => a.id - b.id);
  }

  /**
   * The command that last built a structure, or null. A build names its destination by address
   * rather than by handle, so this is what the capture library resolved it to: `destStructure` on
   * D3D12, and the build info's own reference on Vulkan.
   */
  buildCommandOf(structureId: number): number | null {
    let found: number | null = null;
    for (const c of this.data.commands) {
      if (c.method === "BuildRaytracingAccelerationStructure") {
        if (num((c as { destStructure?: ArgValue }).destStructure) === structureId) found = c.index;
        continue;
      }
      if (!c.method.includes("BuildAccelerationStructures")) continue;
      const infos = isObject(c.args) && Array.isArray(c.args.pInfos) ? c.args.pInfos : [];
      for (const info of infos) {
        if (isObject(info) && refId(info.dstAccelerationStructure) === structureId) found = c.index;
      }
    }
    return found;
  }

  /** Opens the shader debugger on an invocation (Debug Vertex / Pixel / Invocation). */
  debugShader(request: DebugRequest, options: ShaderDebuggerOptions = {}): void {
    this.onDebugShader.emit(request, options);
  }

  /** The render pass a draw is in, keyed the way passes are everywhere else (its primary command buffer). */
  passOfDraw(cmd: CaptureCommand): OverdrawPassKey | null {
    const pass = findPass(this.data, cmd);
    return pass ? { frame: cmd.frame ?? 0, commandBuffer: pass.passBegin.object?.__id ?? 0, passIndex: pass.passIndex } : null;
  }

  /** The vertex shader's input names by location, from its reflection. */
  /**
   * An object's payload, from the database's cache or from the layer: a Metal library's source,
   * which nothing asks for until the debugger is opened on one of its functions.
   */
  async fetchShaderBlob(objectId: number, index: number): Promise<Uint8Array | null> {
    const object = this.window.database.getObject(objectId);
    if (!object) return null;
    return fetchBlob(this.window, object, index).catch(() => null);
  }

  async vertexInputNames(cmd: CaptureCommand): Promise<Map<number, string>> {
    const names = new Map<number, string>();
    const state = drawState(this.data, this.window.database, cmd);
    if ((!state.pipeline && !state.shaders.length) || state.pipeline?.type.startsWith("MTL")) return names;
    // A D3D12 input layout names its attributes itself ("POSITION0", "TEXCOORD0").
    if (state.pipeline && isD3D12Type(state.pipeline.type)) return d3d12AttributeNames(state.pipeline);
    const stages = await this.window.shaders.stagesOf(state);
    const vs = stages.find((s) => s.source.stage === "vertex");
    for (const input of vs?.reflection?.entryPoint(vs.source.entryPoint)?.inputs ?? []) {
      if (input.location !== undefined && input.name) names.set(input.location, input.name);
    }
    return names;
  }

  /**
   * Vulkan: replays the capture with the draw's vertex shader writing transform feedback
   * (src/replay/src/mesh.cpp) for the mesh tab's VS Out. A pass with few draws has them all captured
   * in the same replay.
   */
  async meshOutput(command: number, passDraws: CaptureCommand[] = []): Promise<MeshOutput> {
    if (!this._meshes.has(command)) {
      let run = this._meshRuns.get(command);
      if (!run) {
        const batch = passDraws.length <= MESH_BATCH ? passDraws.map((c) => c.index) : [];
        const commands = [...new Set([command, ...batch])].filter((c) => !this._meshes.has(c) && !this._meshRuns.has(c));
        run = this._runMeshOutputs(commands);
        for (const c of commands) this._meshRuns.set(c, run);
      }
      await run;
    }
    const m = this._meshes.get(command);
    if (!m) throw new Error(`the replay did not answer for draw #${command}`);
    return m;
  }

  private async _runMeshOutputs(commands: number[]): Promise<void> {
    this._setStatus(`mesh output: replaying the capture for ${commands.length === 1 ? `draw #${commands[0]}` : `${commands.length} draws`}...`);
    try {
      const result = await this._replay((r) => window.inspector.meshOutput({ ...r, commands }));
      if (!result.data) throw new Error(result.error ?? "the replay wrote no vertex outputs");
      for (const m of parseMeshFile(result.data).draws) this._meshes.set(m.command, m);
    } finally {
      for (const c of commands) this._meshRuns.delete(c);
      this._updateStatus();
    }
  }

  /** A pass's draws, in command order: those of its secondary command buffers included. */
  drawsOfPass(key: OverdrawPassKey): CaptureCommand[] {
    const pass = collectPassMetrics(this.data, this.window.database).passes
      .find((p) => !p.compute && p.frame === key.frame && p.commandBuffer === key.commandBuffer && p.passIndex === key.passIndex);
    if (!pass) return [];
    const sets = this.data.sets;
    return this.data.commands.slice(pass.commandIndex, pass.endIndex + 1).filter((c) => sets.DRAW.has(c.method));
  }

  /**
   * Vulkan: replays the capture drawing one draw on its own (src/replay/src/overlay.cpp) for the render
   * target tab's draw overlays. A pass with few draws has them all drawn in the same replay, so
   * stepping through them afterwards needs no more.
   */
  async drawOverlay(command: number, passDraws: CaptureCommand[] = []): Promise<DrawOverlay> {
    const have = (): DrawOverlay | undefined => this.data.drawOverlays.get(command);
    if (!have()) {
      let run = this._overlayRuns.get(command);
      if (!run) {
        const batch = passDraws.length <= OVERLAY_BATCH ? passDraws.map((c) => c.index) : [];
        const commands = [...new Set([command, ...batch])].filter((c) => !this.data.drawOverlays.has(c) && !this._overlayRuns.has(c));
        run = this._runDrawOverlays(commands);
        for (const c of commands) this._overlayRuns.set(c, run);
      }
      await run;
    }
    const o = have();
    if (!o) throw new Error(`the replay did not answer for draw #${command}`);
    return o;
  }

  private async _runDrawOverlays(commands: number[]): Promise<void> {
    this._setStatus(`draw overlay: replaying the capture for ${commands.length === 1 ? `draw #${commands[0]}` : `${commands.length} draws`}...`);
    try {
      const result = await this._replay((r) => window.inspector.drawOverlay({ ...r, commands }));
      if (!result.data) throw new Error(result.error ?? "the replay wrote no overlay");
      for (const d of parseDrawOverlayFile(result.data).draws) this.data.drawOverlays.set(d.command, d);
      this.data.onDrawOverlays.emit();
    } finally {
      for (const c of commands) this._overlayRuns.delete(c);
      this._updateStatus();
    }
  }

  /** Opens the render target tab following one pixel (a click in an image viewer, or Metal's own history). */
  openTextureForPixel(request: PixelRequest): void {
    const target = this._targetOf(request.image, request.mip);
    if (!target) {
      this._setStatus("that image is not one of this capture's render targets");
      return;
    }
    this.onOpenTexture.emit(target, { pixel: request });
  }

  /** Opens the render target tab on a pass, with its overdraw over the image. */
  openPassOverdraw(key: OverdrawPassKey, depthTested: boolean): void {
    const target = this._targetOfPass(key);
    if (!target) {
      this._setStatus("this pass has no render target read back, so there is nothing to draw the overdraw over");
      return;
    }
    this.onOpenTexture.emit(target, { overdraw: true, depthTested });
  }

  /** The capture as a file for vkinsp_replay: serialized once, and again only after the capture changed. */
  private _replayBytes(): Promise<Uint8Array> {
    if (!this._replayFile) {
      const file = serializeCapture(this.window, this.data, { forReplay: true });
      file.catch(() => { if (this._replayFile === file) this._replayFile = null; });
      this._replayFile = file;
    }
    return this._replayFile;
  }

  /**
   * Runs a replay request under the capture's key: the main process keeps a replay alive per key
   * (main/replay.ts), so the capture is serialized and sent only when it asks for it.
   */
  private async _replay<T extends { needData?: boolean }>(call: (request: { key: string; name: string; data?: Uint8Array }) => Promise<T>): Promise<T> {
    const key = (this._replayKey ??= `${this.label}:${Date.now()}:${Math.random().toString(36).slice(2)}`);
    const result = await call({ key, name: this.label });
    if (!result.needData) return result;
    return call({ key, name: this.label, data: await this._replayBytes() });
  }

  /** Stops the capture's replay (the tab closed, or the capture was rebuilt). */
  releaseReplay(): void {
    if (this._replayKey) void window.inspector.releaseReplay(this._replayKey);
    this._replayKey = null;
  }

  /** Metal and D3D12: whether the capture followed this pixel while it was taken (a capture with pixelHistory). */
  hasPixelHistory(request: PixelRequest): boolean {
    const h = this.data.pixelHistory;
    if (!h) return false;
    const n = (v: unknown): number => (typeof v === "number" ? v : 0);
    return (request.image === n(h.image) || request.image === n(h.requestedImage)) && request.x === n(h.x) && request.y === n(h.y)
      && (request.mip ?? 0) === n(h.mip) && (request.layer ?? 0) === n(h.layer);
  }

  /**
   * Vulkan: replays the capture following one pixel of an image through the frame (vkinsp_replay --pixel).
   * Metal: the pixel the capture followed while it was taken.
   */
  async pixelHistory(request: PixelRequest): Promise<PixelHistory> {
    if (measuresWhileCapturing(this.data.api)) {
      if (!this.data.pixelHistory || !this.hasPixelHistory(request)) {
        throw new Error("this capture did not follow that pixel: the library follows one while it captures, so another pixel means capturing the application's next frame");
      }
      return parsePixelHistory(this.data.pixelHistory);
    }
    if (this.data.api !== "vulkan") throw new Error("a pixel history needs either a replay or a capture library that follows the pixel while it captures");
    this._setStatus(`pixel history: replaying the capture for pixel (${request.x}, ${request.y})...`);
    try {
      const result = await this._replay((r) => window.inspector.pixelHistory({ ...r, pixel: request }));
      if (!result.data) throw new Error(result.error ?? "the replay wrote no pixel history");
      return parsePixelHistory(result.data);
    } finally {
      this._updateStatus();
    }
  }

  /** A pass's label as the command tree shows it, with its frame when the capture has several. */
  passLabelOf(key: OverdrawPassKey): string {
    // A pixel history event from outside any render pass (a clear, a copy, a dispatch) carries no
    // pass index: UINT32_MAX, which names no pass and would read as "Pass 4294967295".
    const label = key.passIndex === 0xffffffff
      ? `Outside a render pass (command buffer ${key.commandBuffer})`
      : this._passBlocks.get(passKey(key.frame, key.commandBuffer, key.passIndex))?.label
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

  /** Opens the first measured pass's render target with its overdraw over it; a Vulkan capture is measured first. */
  async openOverdraw(): Promise<void> {
    if (!this.data.overdraw.length) {
      if (measuresWhileCapturing(this.data.api)) {
        this._setStatus("this capture did not measure overdraw: capture again with Overdraw ticked");
        return;
      }
      if (this.data.api !== "vulkan") {
        this._setStatus("overdraw is measured by replaying the capture, which this capture's API has no replay for");
        return;
      }
      if (!(await this.measureOverdraw())) return;
    }
    // The pass with the most overdraw to show, rather than the first measured one: a real frame
    // opens with a depth prepass or a shadow pass that drew nothing into the target being viewed,
    // whose heatmap is an empty image, and the report is meant to open on the problem.
    const measured = this.data.overdraw.filter((o) => o.info.measured !== false && o.info.coveredPixels);
    const best = measured.reduce<CapturedOverdraw | null>((worst, o) => {
      const perPixel = o.info.fragments / o.info.coveredPixels;
      const worstPerPixel = worst ? worst.info.fragments / worst.info.coveredPixels : -1;
      return perPixel > worstPerPixel ? o : worst;
    }, null);
    const open = (best ?? this.data.overdraw.find((o) => o.info.measured !== false) ?? this.data.overdraw[0])?.info;
    if (open) this.openPassOverdraw({ frame: open.frame, commandBuffer: open.commandBuffer, passIndex: open.passIndex }, true);
    else this._setStatus("the replay measured no pass");
  }

  /**
   * Vulkan: replays the capture on this machine's GPU with vkinsp_replay, which draws every pass
   * again with a counting fragment shader (docs/REPLAY.md), and takes its measurements. `open`
   * opens that pass's render target with the overdraw over it afterwards.
   */
  async measureOverdraw(open?: OverdrawPassKey): Promise<boolean> {
    if (this._overdrawRun?.running) return false;
    if (this.data.api !== "vulkan") {
      this._setStatus(`overdraw is measured by replaying the capture, and ${this.data.api === "metal" ? "Metal" : "D3D12"} captures do not replay`);
      return false;
    }
    this._overdrawRun = { running: true };
    this._refreshSelection();
    this._setStatus("measuring overdraw: replaying the capture on this machine's GPU...");
    try {
      const result = await this._replay((r) => window.inspector.measureOverdraw(r));
      if (!result.data) throw new Error(result.error ?? "the replay wrote no overdraw data");
      const file = parseOverdrawFile(result.data);
      this._overdrawRun = null;
      this.data.overdraw = file.measurements;
      this.data.onOverdraw.emit();
      this._updateStatus();
      if (open) this.openPassOverdraw(open, true);
      return true;
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      this._overdrawRun = { running: false, error: message };
      this._refreshSelection();
      this._setStatus(`overdraw not measured: ${message.split("\n")[0]}`);
      return false;
    }
  }

  /**
   * Vulkan and Direct3D 12: replays the capture and writes it, as it replays, as a standalone C++ project in `dir`
   * (src/replay/src/exporter.h, src/d3d12/replay/src/dx_exporter.h): every object, what the frame's images and buffers held, and every
   * command, with a program that compares its render targets with the capture's. The directory, or null.
   */
  async exportCpp(dir: string): Promise<string | null> {
    if (this._exportRunning) return null;
    this._exportRunning = true;
    this._setStatus("exporting to C++: replaying the capture on this machine's GPU...");
    try {
      const result = await this._replay((r) => window.inspector.exportCpp({ ...r, dir, api: this.data.api }));
      if (!result.data) throw new Error(result.error ?? "the replay wrote no project");
      const summary = parseExportSummary(result.data);
      this._setStatus(exportSummaryText(summary));
      return summary.ok ? summary.directory : null;
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      // The first line says what went wrong here; the last is the replay tool's own word on why.
      const lines = message.split("\n").map((l) => l.trim()).filter(Boolean);
      this._setStatus(`export to C++ failed: ${lines[0] ?? message}${lines.length > 1 ? ` (${lines[lines.length - 1]})` : ""}`);
      return null;
    } finally {
      this._exportRunning = false;
    }
  }

  /**
   * Testing aid (--debug-view=shader-edit): the first draw's pixel shader edited to write magenta,
   * compiled, and the capture replayed with it — what Edit and Compile & Replay do in the Inspect
   * panel, with the edit made here instead of typed.
   */
  private async _debugShaderEdit(): Promise<void> {
    const db = this.window.database;
    const draw = this.data.commands.find((c) => this.data.sets.DRAW.has(c.method));
    const state = draw ? drawState(this.data, db, draw) : null;
    const source = state ? stateStages(state, db).find((s) => s.stage === "fragment") : undefined;
    const bytes = source ? await fetchBlob(this.window, source.object, source.blobIndex) : null;
    if (!state?.pipeline || !source || !bytes) {
      this._setStatus("shader edit: the first draw has no pixel shader the capture holds");
      return;
    }
    const d3d12 = this.data.api === "d3d12";
    const text = await window.inspector.shaderText(bytes, d3d12 ? "hlsl" : "glsl", this.window.symbolDirs);
    if (!text.ok) {
      this._setStatus(`shader edit: no source to edit: ${text.text.split("\n")[0]}`);
      return;
    }
    // The last thing the entry point writes: HLSL returns its color, GLSL assigns its output.
    const edited = d3d12
      ? text.text.replace(/return\s+float4\s*\([^;]*\)\s*;(?![\s\S]*return\s+float4)/, "return float4(1.0, 0.0, 1.0, 1.0);")
      : text.text.replace(/(\b\w+)\s*=\s*vec4\s*\([^;]*\)\s*;(?![\s\S]*=\s*vec4\s*\()/, "$1 = vec4(1.0, 0.0, 1.0, 1.0);");
    if (edited === text.text) {
      this._setStatus("shader edit: the source has no color written the way this testing aid edits one");
      return;
    }
    const compiled = d3d12
      ? await window.inspector.compileDxil(edited, "fragment", source.entryPoint, "6_0")
      : await window.inspector.compileShader(edited, "glsl", "fragment", source.entryPoint, "1.3");
    if (!compiled.ok || !compiled.spirv) {
      this._setStatus(`shader edit: the edit did not compile: ${compiled.log.split("\n")[0]}`);
      return;
    }
    await this.replayWithShaders([{ pipeline: state.pipeline.id, stage: "fragment", code: new Uint8Array(compiled.spirv) }]);
  }

  /** Whether this capture can be run again: a Vulkan or a D3D12 one, once its commands are here. */
  get canReplay(): boolean {
    return (this.data.api === "vulkan" || this.data.api === "d3d12") && this.data.commands.length > 0;
  }

  /** The last shader edit's effect on the frame, for the debug dump. */
  shaderReplay: ReplayedTargets | null = null;

  /**
   * A shader edited and run in the capture (renderer/shader_replay.ts): the frame replayed with
   * other code for some pipelines' stages, and its render targets against what the capture read
   * back, in a tab beside this one. Resolves to a line saying what happened.
   */
  async replayWithShaders(replacements: ShaderReplacement[]): Promise<string> {
    if (!this.canReplay) return "this capture cannot be replayed";
    const db = this.window.database;
    const edited = replacements.map((r) => `${r.stage} stage of ${db.getObject(r.pipeline)?.name ?? `pipeline ${r.pipeline}`}`);
    const body = this._reportBody("shader-edit", "Replaying the frame with the edited shader...");
    this._setStatus(`replaying the frame with the edited ${edited.join(", ")}...`);
    try {
      const request = encodeReplaceRequest(replacements);
      const run = await this._replay((r) => window.inspector.replayEdited({ ...r, api: this.data.api, request }));
      if (!run.data) throw new Error(run.error ?? "the replay wrote no render targets");
      const result = parseReplayedTargets(run.data);
      this.shaderReplay = result;
      const pipelines = replacements.map((r) => r.pipeline);
      if (body) {
        renderShaderReplay(body, result, {
          edited, pipelines,
          captured: (t) => this.data.textures.find((c) => c.info.id === t.image && c.info.commandBuffer === t.commandBuffer && c.info.frame === t.frame
            && c.info.passIndex === t.passIndex && c.info.attachment === t.attachment && c.info.aspect === t.aspect && (c.info.kind ?? "attachment") === "attachment") ?? null,
          imageName: (id) => db.getObject(id)?.name ?? `Image ${id}`,
          onInspect: (id) => this.window.showObject(id),
        });
      }
      const summary = replayedTargetsSummary(result, pipelines);
      this._setStatus(summary);
      return summary;
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      if (body) {
        body.html = "";
        new Div(body, { text: `The frame could not be replayed with the edit: ${message}`, class: "text-muted", style: "padding: 12px; white-space: pre-wrap;" });
      }
      this._setStatus(`not replayed: ${message.split("\n")[0]}`);
      return `not replayed: ${message.split("\n")[0]}`;
    }
  }

  /**
   * Replays the capture with a timestamp pair and a pipeline statistics query around every draw
   * and dispatch (src/replay/src/draw_stats.cpp, src/d3d12/replay/src/dx_measure.cpp), for the
   * Shader Flame Graph's per-draw weights. A D3D12 capture can also measure them while it is
   * taken (Measure draws in the capture bar); this measures one that did not, or a file.
   */
  async measureDraws(): Promise<boolean> {
    if (this._drawRun?.running) return false;
    if (this.data.api !== "vulkan" && this.data.api !== "d3d12") {
      this._setStatus("per-draw measurements need the capture replayed, and Metal captures do not replay yet");
      return false;
    }
    this._drawRun = { running: true };
    this._setStatus("measuring draws: replaying the capture on this machine's GPU...");
    try {
      const result = await this._replay((r) => window.inspector.measureDraws({ ...r, api: this.data.api }));
      if (!result.data) throw new Error(result.error ?? "the replay wrote no draw measurements");
      const file = parseDrawStats(result.data);
      this._drawRun = null;
      this.data.drawStats = file.draws;
      this.data.onDrawStats.emit();
      this._setStatus(drawStatsSummary(file));
      return true;
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      this._drawRun = { running: false, error: message };
      this._setStatus(`draws not measured: ${message.split("\n")[0]}`);
      return false;
    }
  }

  /**
   * Vulkan: reads the GPU's own hardware counters around each render pass by replaying the capture
   * (src/replay/src/hw_counters.cpp), for the GPU Bottlenecks report's limiters. The frame is
   * replayed once per collection pass the counters need, so this takes a while on a large capture.
   */
  async measureHwCounters(perDraw = false): Promise<boolean> {
    if (this._hwCounterRun?.running) return false;
    // Vulkan replays with vkinsp_replay; D3D12 with dxinsp_replay (dx_counters.cpp). Metal has no
    // replay, so its captures have no counters.
    if (this.data.api !== "vulkan" && this.data.api !== "d3d12") {
      this._setStatus("hardware counters need the capture replayed, and Metal captures do not replay yet");
      return false;
    }
    this._hwCounterRun = { running: true };
    this._setStatus("reading hardware counters: replaying the capture once per collection pass...");
    try {
      const result = await this._replay((r) => window.inspector.measureHwCounters({ ...r, perDraw, api: this.data.api }));
      if (!result.data) throw new Error(result.error ?? "the replay read no hardware counters");
      const file = parseHwCounters(result.data);
      this._hwCounterRun = null;
      this.data.hwCounters = file;
      this.data.onHwCounters.emit();
      this._setStatus(hwCountersSummary(file));
      return file.passes.length > 0 || file.draws.length > 0;
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      this._hwCounterRun = { running: false, error: message };
      this._setStatus(`hardware counters not read: ${message.split("\n")[0]}`);
      return false;
    }
  }

  /**
   * Vulkan: measures what a shader stage's functions, lines and textures cost at one draw, by
   * replaying the draw with variants of the stage that leave each out (vkinsp_replay --ablate).
   */
  async measureShader(target: ShaderMeasureTarget): Promise<boolean> {
    if (this.data.api !== "vulkan" && this.data.api !== "d3d12") return false;
    const drawMs = this.data.drawStats?.find((d) => d.command === target.command)?.ms ?? null;
    this._setStatus(`measuring the ${target.stage} shader at draw #${target.command}: replaying its variants...`);
    try {
      const result = await this._replay((r) => window.inspector.measureShader({ ...r, api: this.data.api, stage: { ...target, drawMs } }));
      if (!result.ablation) throw new Error(result.error ?? "the replay did not measure the shader");
      this.data.addAblation(result.ablation);
      const a = result.ablation;
      this._setStatus(`${target.stage} shader measured at draw #${target.command}: ${a.parts.length} part${a.parts.length === 1 ? "" : "s"}, `
        + `stage ${a.stageMs === null ? "not timed" : `${a.stageMs.toFixed(4)} ms`} of ${a.baselineMs.toFixed(4)} ms per draw`);
      return true;
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      this._setStatus(`shader not measured: ${message.split("\n")[0]}`);
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

  renderPassTargets(container: Widget, frame: number, passBegin: CaptureCommand, passIndex: number, commandBufferId: number, command?: CaptureCommand): void {
    const textures = this.data.texturesForPass(frame, commandBufferId, passIndex);
    const grp = new collapsible(container, { label: `Render Targets (${textures.length})`, collapsed: false });
    if (!textures.length) {
      new Div(grp.body, { text: "No render target data for this pass (pre-recorded command buffer, or readback disabled).", class: "text-muted", style: "padding: 6px;" });
    } else {
      const strip = new Div(grp.body, { class: "capture_frameImages" });
      // A Vulkan draw's targets can show where it landed (replayed); Metal and D3D12 captures have no replay to draw it with.
      const draw = command && this.data.api === "vulkan" && this.data.sets.DRAW.has(command.method) ? command : undefined;
      for (const tex of textures) this._renderTexture(strip, tex, draw);
    }
    this._renderPassOverdraw(container, frame, commandBufferId, passIndex);
  }

  /**
   * The pass's overdraw heatmaps, when the capture has them: a Metal capture measured while it was
   * taken (src/metal/src/overdraw.h), a Vulkan one replayed (measureOverdraw). A heatmap opens over the
   * pass's render target in the capture's render target tab (capture_texture_view.ts).
   */
  private _renderPassOverdraw(container: Widget, frame: number, commandBufferId: number, passIndex: number): void {
    const key: OverdrawPassKey = { frame, commandBuffer: commandBufferId, passIndex };
    const measurements = this.data.overdrawForPass(frame, commandBufferId, passIndex);
    if (!measurements.length) {
      if (this.data.api === "vulkan") this._renderOverdrawReplay(container, key);
      return;
    }
    const grp = new collapsible(container, { label: "Overdraw", collapsed: false });
    new Button(grp.body, { label: "Open in Tab", class: "btn btn-sm", tooltip: "The pass's render target in a tab of its own, with the overdraw over it: zoom, the counts under the pointer, and the history of any pixel you click",
      callback: () => this.openPassOverdraw(key, true) });
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
      canvas.title = "Click to open the render target with this overdraw over it: zoom, the counts under the pointer, and the history of any pixel you click";
      canvas.onclick = () => this.openPassOverdraw(key, o.info.depthTested);
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

  private _renderTexture(parent: Widget, tex: CapturedTexture, draw?: CaptureCommand): void {
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
    if (isRenderTarget(tex.info)) {
      new Button(box, { label: "Open in Tab", class: "btn btn-sm",
        tooltip: "The render target in a tab of its own: zoom, the pass's overdraw over it, and the history of any pixel you click",
        callback: () => this.onOpenTexture.emit({ key: { frame: tex.info.frame, commandBuffer: tex.info.commandBuffer, passIndex: tex.info.passIndex }, texture: tex }, {}) });
      if (draw) {
        new Button(box, { label: "Highlight Draw", class: "btn btn-sm",
          tooltip: "The render target in its tab with this draw's pixels highlighted (replays the capture); Depth Test and Wireframe are in the tab's Overlay list",
          callback: () => this.openDrawOverlay(draw, "highlight",
            { key: { frame: tex.info.frame, commandBuffer: tex.info.commandBuffer, passIndex: tex.info.passIndex }, texture: tex }) });
      }
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
      // A render target's pixel can be followed through the frame: replayed (Vulkan), or the next frame captured following it (Metal).
      const history = isRenderTarget(tex.info)
        ? { pixelHistory: (x: number, y: number, mip: number, layer: number) => this.openTextureForPixel({ image: tex.info.id, x, y, mip, layer }) }
        : {};
      new ImageView(viewer, this.window, image, { info: tex.info, data: tex.data }, history);
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
