// Live object inspection: grouped object lists on the left, details of the selected object on
// the right. Structure follows WebGPU Inspector's devtools/inspect_panel.js (MIT, Brendan Duncan).
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Plot, type PlotData } from "./widget/plot.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import { Split } from "./widget/split.js";
import { TabWidget } from "./widget/tab_widget.js";
import { TextInput } from "./widget/text_input.js";
import { Widget } from "./widget/widget.js";
import { VulkanObject, fmt, fmtFlags, formatBytes, isHandleRef, isObject, num, refId, str } from "./vulkan/vulkan_object.js";
import { objectLink, renderArgs } from "./args_view.js";
import { CodeEditor, escapeHtml, highlight, highlightLines } from "./code_editor.js";
import { compilableSource, describeDebugInfo, disassemblyInstructions, hasEmbeddedSource, parseSpirvDebugInfo, sourceLanguageOf, sourceLineMap, type DebugLocation, type SpirvDebugInfo } from "./vulkan/spirv_debug.js";
import { ImageView } from "./image_view.js";
import { encodeBase64 } from "./utils/base64.js";
import { reflectSpirv, type ShaderStage } from "./vulkan/spirv_reflect.js";
import { stageLabel } from "./shader_cache.js";
import { renderReflection } from "./shader_reflection_view.js";
import { renderDeviceSections, renderInstanceSections, renderPhysicalDeviceSections } from "./device_info_view.js";
import type { SessionContext } from "./session_panel.js";
import type { ObjectDatabase, ValidationEntry } from "./vulkan/object_database.js";
import type { CaptureDescriptorBinding, HandleRef, LeakReportMessage, ShaderLanguage, ShaderReplacedMessage, ShaderTextMode } from "../shared/protocol.js";

// Preferred display order; any other type is appended alphabetically as it appears.
const TYPE_ORDER = [
  "VkInstance", "VkPhysicalDevice", "VkDevice", "VkQueue", "VkSurfaceKHR", "VkSwapchainKHR",
  "VkPipeline", "VkShaderModule", "VkPipelineLayout", "VkRenderPass", "VkFramebuffer",
  "VkImage", "VkImageView", "VkSampler", "VkBuffer", "VkBufferView", "VkDeviceMemory",
  "VkDescriptorSet", "VkDescriptorSetLayout", "VkDescriptorPool",
  "VkCommandBuffer", "VkCommandPool", "VkFence", "VkSemaphore", "VkEvent", "VkQueryPool",
  "VkPipelineCache",
];

const PLURALS: Record<string, string> = { VkDeviceMemory: "Device Memory", VkSurfaceKHR: "Surfaces", VkSwapchainKHR: "Swapchains" };

function typeLabel(type: string): string {
  if (PLURALS[type]) return PLURALS[type];
  const t = type.replace(/^Vk/, "").replace(/(KHR|EXT|NV|AMD|INTEL|ARM)$/, "");
  const words = t.replace(/([a-z])([A-Z])/g, "$1 $2");
  return words.endsWith("s") ? words + "es" : words + "s";
}

interface ObjectGroup extends collapsible {
  objectList: Widget;
  type: string;
  total: number;
  visibleCount: number;
}

interface ObjectItem extends Widget {
  group: ObjectGroup;
}

/** The views of a shader payload: the SDK conversions plus the source embedded in the SPIR-V. */
type ShaderViewMode = ShaderTextMode | "source";

interface ShaderView {
  index: number;
  blobName: string;
  pre: Widget;
  mode: ShaderViewMode;
  data: Uint8Array | null;
  text: string;                  // the converted text currently shown
  buttons: Partial<Record<ShaderViewMode, Button>>;
  editButton: Button;
  editor: Div | null;
  body: Widget;
  /** Debug information of the SPIR-V (embedded source, line mapping), parsed when the blob arrives. */
  debug: SpirvDebugInfo | null;
  sourceFile: number;            // file shown by the Source view
  disText: string;               // cached spirv-dis output
  summary: Div;
  fileBar: Div | null;
  /** The Reflection section of the payload, filled when its SPIR-V arrives. */
  reflection: collapsible;
  /** The payload's section (its label carries the "[edited]" mark). */
  group: collapsible;
}

/** An edit made in the shader editor, kept per shader payload so it survives re-inspection. */
interface ShaderEdit {
  language: ShaderLanguage;
  source: string;
  /** Pipelines the edit was applied to and the layer's answer for each. */
  results: Map<number, string>;
  applied: boolean;
}

/** Which pipelines an edit of a payload goes to, and the stage it replaces. */
interface EditTargets {
  pipelines: VulkanObject[];
  stage: ShaderStage;
  stageFlag: string;
  entryPoint: string;
  spirvVersion: string;
}

const STAGE_FLAG: Record<string, string> = {
  vertex: "VK_SHADER_STAGE_VERTEX_BIT", tess_control: "VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT",
  tess_eval: "VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT", geometry: "VK_SHADER_STAGE_GEOMETRY_BIT",
  fragment: "VK_SHADER_STAGE_FRAGMENT_BIT", compute: "VK_SHADER_STAGE_COMPUTE_BIT", task: "VK_SHADER_STAGE_TASK_BIT_EXT",
  mesh: "VK_SHADER_STAGE_MESH_BIT_EXT", raygen: "VK_SHADER_STAGE_RAYGEN_BIT_KHR", any_hit: "VK_SHADER_STAGE_ANY_HIT_BIT_KHR",
  closest_hit: "VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR", miss: "VK_SHADER_STAGE_MISS_BIT_KHR",
  intersection: "VK_SHADER_STAGE_INTERSECTION_BIT_KHR", callable: "VK_SHADER_STAGE_CALLABLE_BIT_KHR",
};

const LANGUAGE_OF_MODE: Record<ShaderViewMode, ShaderLanguage | null> = { dis: "spirv-asm", glsl: "glsl", hlsl: "hlsl", msl: null, source: null };
const LANGUAGE_LABEL: Record<ShaderLanguage, string> = { glsl: "GLSL (glslangValidator)", hlsl: "HLSL (dxc)", "spirv-asm": "SPIR-V assembly (spirv-as)" };

/** Object list filters, after WebGPU Inspector's inspect panel filter panel. */
interface Filters {
  search: string;
  onlyInLastCapture: boolean;
  image: { format: string; width: string; height: string; layers: string; usage: Set<string> };
  buffer: { size: string; usage: Set<string> };
  shader: Set<string>;                 // "VERTEX" | "FRAGMENT" | "COMPUTE"
  descriptorSet: { contains: string };
}

interface NumericFilter { op: string; value: number }

/** ">=256", "<1024", "4" -> comparison; null when empty or malformed. */
function parseNumeric(text: string): NumericFilter | null {
  const m = /^(>=|<=|>|<|=)?\s*(-?\d+(?:\.\d+)?)\s*$/.exec(text.trim());
  return m ? { op: m[1] || "=", value: Number(m[2]) } : null;
}

function numericMatches(f: NumericFilter | null, n: number): boolean {
  if (!f) return true;
  if (!Number.isFinite(n)) return false;
  switch (f.op) {
    case ">=": return n >= f.value;
    case "<=": return n <= f.value;
    case ">": return n > f.value;
    case "<": return n < f.value;
    default: return n === f.value;
  }
}

const IMAGE_USAGES: [string, string][] = [
  ["Sampled", "SAMPLED"], ["Storage", "STORAGE"], ["Color target", "COLOR_ATTACHMENT"],
  ["Depth target", "DEPTH_STENCIL_ATTACHMENT"], ["Transfer src", "TRANSFER_SRC"], ["Transfer dst", "TRANSFER_DST"],
];
const BUFFER_USAGES: [string, string][] = [
  ["Vertex", "VERTEX_BUFFER"], ["Index", "INDEX_BUFFER"], ["Uniform", "UNIFORM_BUFFER"], ["Storage", "STORAGE_BUFFER"],
  ["Indirect", "INDIRECT_BUFFER"], ["Transfer src", "TRANSFER_SRC"], ["Transfer dst", "TRANSFER_DST"],
];
const SHADER_STAGES: [string, string][] = [["Vertex", "VERTEX"], ["Fragment", "FRAGMENT"], ["Compute", "COMPUTE"]];

export class InspectPanel {
  readonly window: SessionContext;
  readonly database: ObjectDatabase;
  readonly parent: Widget;

  private _groups = new Map<string, ObjectGroup>();
  private _selectedGroup: ObjectGroup | null = null;
  private _selectedObject: VulkanObject | null = null;
  inspectedObject: VulkanObject | null = null;
  private _back: VulkanObject[] = [];
  private _forward: VulkanObject[] = [];
  private _filters: Filters = InspectPanel._emptyFilters();
  private _shaderViews = new Map<number, ShaderView>();
  /** Shader edits by "<object id>:<blob index>"; the layer holds the applied state. */
  private _shaderEdits = new Map<string, ShaderEdit>();
  /** The editor currently open, to route ShaderReplaced answers to its status line. */
  private _openEditor: { key: string; targets: EditTargets; status: Div; view: ShaderView } | null = null;
  private _imageView: ImageView | null = null;
  /** Descriptor sets whose contents have been requested from the layer (avoids re-asking on every re-render). */
  private _descriptorRequested = new Set<number>();

  private objectsPanel!: Div;
  private groupsContainer!: Div;
  private inspectPanel!: Div;
  // Validation messages (the layer's debug-utils messenger), listed above the object groups.
  private _validationGroup: collapsible | null = null;
  private _validationList: Widget | null = null;
  private _validationItems = new Map<number, Widget>();
  private _selectedValidation: ValidationEntry | null = null;
  private _leakGroup: collapsible | null = null;
  private _leakList: Widget | null = null;
  private _backButton!: Button;
  private _forwardButton!: Button;
  private _filterInput!: TextInput;

  // Meters (after WebGPU Inspector's inspect panel): frame time and object count plots, memory.
  private _frameTimeLabel!: Span;
  private _memoryLabel!: Span;
  private _frameTimePlot!: Plot;
  private _frameTimeData!: PlotData;
  private _frameTimeMaxData!: PlotData;
  private _submitData!: PlotData;
  private _objectCountPlot!: Plot;
  private _objectCountData!: PlotData;
  private _objectCountType = "";     // "" = all objects

  constructor(win: SessionContext, parent: Widget) {
    this.window = win;
    this.database = win.database;
    this.parent = parent;
    this._build();

    const db = this.database;
    db.onReset.addListener(() => this._reset());
    db.onAddObject.addListener((o) => this._addObject(o));
    db.onDeleteObject.addListener((id, o) => this._deleteObject(id, o));
    db.onObjectLabelChanged.addListener((id, o) => this._objectChanged(o));
    db.onObjectInvalidated.addListener((id, o) => this._objectChanged(o));
    db.onObjectUpdated.addListener((id, o) => this._objectUpdated(o));
    db.onObjectBlob.addListener((id, index, data) => this._objectBlob(id, index, data));
    db.onOtherMessage.addListener((msg) => {
      if (msg.action === "ImageData") this._imageView?.handleImageData(msg);
      else if (msg.action === "ShaderReplaced") this._shaderReplaced(msg);
    });
    db.onCapturedObjectsChanged.addListener(() => {
      if (this._filters.onlyInLastCapture) this._applyFilter();
    });
    db.onFrameStats.addListener((m) => this._updateMeters(m.frameTimeMs, m.maxMs ?? m.frameTimeMs, m.submitMs ?? 0));
    db.onValidationMessage.addListener((entry, isNew) => this._validationMessage(entry, isNew));
    db.onLeakReport.addListener((report) => this._leakReport(report));
  }

  // ---------------------------------------------------------------------------------------
  // Leak reports: objects still alive when their device or instance was destroyed, listed in
  // a group above the object list. The objects themselves are deleted right after the report,
  // so the entries keep what the report said about them.

  private _leakReport(report: LeakReportMessage): void {
    if (!this._leakGroup) {
      const g = new collapsible(null, { collapsed: false, label: "Leaked Objects 0", class: "leak-group" });
      g.body.style.maxHeight = "320px";
      g.body.style.overflow = "auto";
      this._leakList = new Widget("ol", g.body, { style: "margin-top: 6px; margin-bottom: 6px;" });
      const first = this.groupsContainer.children[0];
      if (first) this.groupsContainer.insertBefore(g, first);
      else this.groupsContainer.appendChild(g);
      this._leakGroup = g;
    }
    const owner = this.database.getObject(report.owner);
    const ownerName = owner ? owner.name : `${report.ownerClass} ${report.owner}`;
    const summary = Object.entries(report.byType).map(([t, n]) => `${n} ${t.replace(/^Vk/, "")}`).join(", ");
    new Widget("li", this._leakList!, { text: `${ownerName} destroyed with ${report.count} live object${report.count === 1 ? "" : "s"}: ${summary}`, class: "leak-owner text-muted font-sm" });
    for (const o of report.objects) {
      const item = new Widget("li", this._leakList!, { class: "object-item leak-item" });
      new Span(item, { text: "⚠ ", class: "validation-sev validation-sev-warning" });
      new Span(item, { text: o.name ? `${o.name}` : `${o.class.replace(/^Vk/, "")} ${o.id}`, class: "object-item-name" });
      new Span(item, { text: ` ${o.class} ${o.id}  ${o.cmd}`, class: "object-item-type" });
      item.tooltip = `${o.class} ${o.id} created by ${o.cmd}, still alive when ${ownerName} was destroyed`;
      item.element.onclick = () => {
        const obj = this.database.getObject(o.id);
        if (obj && !obj.isDeleted) {
          this.revealObject(obj);
          return;
        }
        this.inspectPanel.html = "";
        this.inspectedObject = null;
        const box = new Div(this.inspectPanel, { class: "info-box info-box-warning" });
        new Div(box, { text: `Leaked: ${o.name ? `${o.name}  ` : ""}${o.class} ${o.id}`, class: "font-lg" });
        new Div(box, { text: `Created by ${o.cmd}; still alive when ${ownerName} was destroyed, and destroyed with it.`, class: "font-md text-muted" });
        if (obj) {
          new Div(box, { text: "Its last known creation arguments:", class: "font-md text-muted" });
          renderArgs(new Div(box, { class: "args-tree" }), obj.args, this.database, (l) => this.revealObject(l));
        }
      };
    }
    if (report.count > report.objects.length) new Widget("li", this._leakList!, { text: `... ${report.count - report.objects.length} more`, class: "text-muted font-sm" });
    this._leakGroup.label.text = `Leaked Objects ${this.database.leakCount}`;
  }

  // ---------------------------------------------------------------------------------------
  // Validation messages: a group above the object list (WebGPU Inspector's "Validation Errors"),
  // an error mark on every object a message names, and the message as an inspectable item.

  private _ensureValidationGroup(): collapsible {
    if (this._validationGroup) return this._validationGroup;
    const g = new collapsible(null, { collapsed: true, label: "Validation Messages 0", class: "validation-group" });
    g.body.style.maxHeight = "320px";
    g.body.style.overflow = "auto";
    this._validationList = new Widget("ol", g.body, { style: "margin-top: 6px; margin-bottom: 6px;" });
    const first = this.groupsContainer.children[0];
    if (first) this.groupsContainer.insertBefore(g, first);
    else this.groupsContainer.appendChild(g);
    this._validationGroup = g;
    return g;
  }

  private _setValidationLabel(): void {
    if (!this._validationGroup) return;
    const [errors, warnings] = this.database.validationCounts;
    const n = this.database.validation.length;
    const dropped = this.database.validationDropped;
    this._validationGroup.label.text = `Validation Messages ${n}${dropped ? ` (+${dropped} dropped)` : ""}`;
    this._validationGroup.classList.toggle("has-errors", errors > 0);
    this._validationGroup.classList.toggle("has-warnings", errors === 0 && warnings > 0);
  }

  private _validationItemText(entry: ValidationEntry): string {
    // The validation layer's first line is "Validation Error: [ VUID ] Object 0: handle = ...;
    // | MessageID = ... | <what went wrong>": the last "|" segment is the readable part.
    let first = entry.message.split("\n")[0].replace(/^Validation (Error|Warning|Performance Warning): \[[^\]]*\]\s*/, "");
    const segments = first.split(" | ");
    if (segments.length > 1) first = segments[segments.length - 1].trim();
    const head = entry.idName ? `${entry.idName}: ` : "";
    const text = `${head}${first}`;
    return text.length > 140 ? `${text.slice(0, 140)}...` : text;
  }

  private _validationMessage(entry: ValidationEntry, isNew: boolean): void {
    const g = this._ensureValidationGroup();
    if (isNew) {
      const item = new Widget("li", this._validationList!, { class: "object-item validation-item" });
      new Span(item, { text: entry.severity === "error" ? "✖" : entry.severity === "warning" ? "⚠" : "ℹ", class: `validation-sev validation-sev-${entry.severity}` });
      new Span(item, { text: this._validationItemText(entry), class: "validation-text" });
      new Span(item, { text: entry.count > 1 ? `×${entry.count}` : "", class: "validation-count" });
      item.tooltip = entry.message;
      item.element.onclick = () => this._selectValidation(entry);
      this._validationItems.set(entry.key, item);
      // Objects the message names show the error in the list and in their details.
      for (const o of entry.objects ?? []) {
        if (!o.object || !isHandleRef(o.object)) continue;
        const obj = this.database.getObject(o.object.__id);
        if (!obj) continue;
        if (obj.widget) this._fillItem(obj, obj.widget as Widget);
        if (this.inspectedObject === obj) this._inspectObject(obj);
      }
    } else {
      const item = this._validationItems.get(entry.key);
      const count = item?.children[2] as Span | undefined;
      if (count) count.text = entry.count > 1 ? `×${entry.count}` : "";
    }
    this._setValidationLabel();
    if (this._selectedValidation === entry && !isNew) this._inspectValidation(entry);
    void g;
  }

  /** Expands the validation group (the session bar's counter). */
  showValidation(): void {
    const g = this._ensureValidationGroup();
    g.expand();
    g.element.scrollIntoView({ block: "nearest" });
  }

  private _selectValidation(entry: ValidationEntry): void {
    const prev = this._selectedObject?.widget as ObjectItem | null;
    if (prev) prev.element.classList.remove("selected");
    this._selectedObject = null;
    for (const [key, item] of this._validationItems) item.element.classList.toggle("selected", key === entry.key);
    this._selectedValidation = entry;
    this._inspectValidation(entry);
  }

  private _inspectValidation(entry: ValidationEntry): void {
    this.inspectPanel.html = "";
    this.inspectedObject = null;
    this.database.inspectedObject = null;
    const db = this.database;
    const onLink = (o: VulkanObject) => this.revealObject(o);
    const box = new Div(this.inspectPanel, { class: entry.severity === "error" ? "info-box info-box-error" : "info-box info-box-warning", style: "flex: 0 0 auto;" });
    new Div(box, { text: `Validation ${entry.severity}${entry.idName ? `: ${entry.idName}` : ""}`, class: "font-lg" });
    const meta: string[] = [];
    if (entry.types?.length) meta.push(entry.types.join(", "));
    meta.push(`frame ${entry.frame}`);
    meta.push(entry.count > 1 ? `reported ${entry.count} times` : "reported once");
    if (entry.idNumber) meta.push(`id ${entry.idNumber}`);
    new Div(box, { text: meta.join("  |  "), class: "font-md text-muted" });
    if (entry.objects?.length) {
      const grp = new collapsible(box, { label: `Objects (${entry.objects.length})`, collapsed: false });
      const ul = new Widget("ul", grp.body, { class: "dependency-list" });
      for (const o of entry.objects) {
        const li = new Widget("li", ul);
        const obj = o.object && isHandleRef(o.object) ? db.getObject(o.object.__id) : null;
        if (obj) objectLink(li, obj, onLink, true);
        else new Span(li, { text: `${o.class} ${o.handle}`, class: "text-muted" });
        if (o.name) new Span(li, { text: `  "${o.name}"`, class: "text-muted font-sm" });
      }
    }
    if (entry.cmdBufLabels?.length) new Div(box, { text: `Command buffer labels: ${entry.cmdBufLabels.join(" > ")}`, class: "font-md text-muted" });
    if (entry.queueLabels?.length) new Div(box, { text: `Queue labels: ${entry.queueLabels.join(" > ")}`, class: "font-md text-muted" });
    const grp = new collapsible(this.inspectPanel, { label: "Message", collapsed: false });
    new Widget("pre", grp.body, { text: entry.message, class: "validation-message" });
    if (entry.idName?.startsWith("VUID-")) {
      new Div(grp.body, { text: `Specification: search the Vulkan specification for ${entry.idName} (registry.khronos.org/vulkan/specs/latest/html/vkspec.html#${entry.idName}).`, class: "text-muted font-sm" });
    }
  }

  // ---------------------------------------------------------------------------------------
  // Meters

  private _buildMeters(parent: Widget): void {
    const bar = new Div(parent, { class: "inspect-meters" });
    const texts = new Div(bar, { class: "inspect-meter-texts" });
    this._frameTimeLabel = new Span(texts, { text: "Frame Time: --", class: "inspect-meter-stat" });
    this._memoryLabel = new Span(texts, { text: "", class: "inspect-meter-stat", tooltip: "Device memory: the application's VkDeviceMemory allocations. Images and buffers: estimated from their formats and sizes (they live inside those allocations)." });

    const plots = new Div(bar, { class: "inspect-meter-plots" });
    const frameLabel = new Span(plots, { class: "inspect-meter-legend" });
    new Span(frameLabel, { text: "Frame Time", class: "text-muted" });
    new Span(frameLabel, { text: "■ avg", style: "color: #cccccc;", class: "font-sm" });
    new Span(frameLabel, { text: "■ max", style: "color: #e0a060;", class: "font-sm" });
    new Span(frameLabel, { text: "■ submit", style: "color: #5fd08a;", class: "font-sm", tooltip: "CPU time per frame spent inside vkQueueSubmit" });
    this._frameTimePlot = new Plot(plots, { precision: 2, suffix: "ms", sharedScale: true, minValue: 0, class: "plot-container inspect-meter-plot" });
    this._frameTimeData = this._frameTimePlot.addData("Frame Time", "#cccccc");
    this._frameTimeMaxData = this._frameTimePlot.addData("Longest", "#e0a060");
    this._submitData = this._frameTimePlot.addData("Submit", "#5fd08a");
    this._frameTimePlot.tooltip = "Frame time per 100 ms reporting interval: average and longest frame, and the CPU time inside vkQueueSubmit";

    const options = ["All objects", ...TYPE_ORDER.map((t) => typeLabel(t))];
    new Select(plots, { options, index: 0, class: "inspect-meter-select", onChange: (_v: string, index: number) => {
      this._objectCountType = index > 0 ? TYPE_ORDER[index - 1] : "";
      this._objectCountPlot.reset();
      this._objectCountPlot.draw();
    } });
    this._objectCountPlot = new Plot(plots, { class: "plot-container inspect-meter-plot" });
    this._objectCountData = this._objectCountPlot.addData("Object Count", "#9ab8e0");
    this._objectCountPlot.tooltip = "Number of live objects of the selected type";
  }

  private _updateMeters(frameTimeMs: number, maxMs: number, submitMs: number): void {
    const db = this.database;
    this._frameTimeLabel.text = `Frame Time: ${frameTimeMs.toFixed(2)} ms  (${(1000 / Math.max(0.001, frameTimeMs)).toFixed(0)} fps)   Submit: ${submitMs.toFixed(2)} ms`;
    this._updateMemoryLabel();
    this._frameTimeData.add(frameTimeMs);
    this._frameTimeMaxData.add(maxMs);
    this._submitData.add(submitMs);
    this._frameTimePlot.draw();
    const count = this._objectCountType ? db.getObjectsOfType(this._objectCountType)?.size ?? 0 : db.allObjects.size;
    this._objectCountData.add(count);
    this._objectCountPlot.draw();
  }

  private _updateMemoryLabel(): void {
    const m = this.database.memory;
    this._memoryLabel.text = `Device Memory: ${formatBytes(m.device)} in ${m.allocations} allocation${m.allocations === 1 ? "" : "s"}   Images: ${formatBytes(m.images)}   Buffers: ${formatBytes(m.buffers)}   Objects: ${this.database.allObjects.size}`;
  }

  private static _emptyFilters(): Filters {
    return {
      search: "", onlyInLastCapture: false,
      image: { format: "", width: "", height: "", layers: "", usage: new Set() },
      buffer: { size: "", usage: new Set() },
      shader: new Set(),
      descriptorSet: { contains: "" },
    };
  }

  private _build(): void {
    this._buildMeters(this.parent);
    const split = new Split(this.parent, { direction: Split.Horizontal, position: 380 });
    const pane1 = new Span(split);
    const objectsTab = new TabWidget(pane1, { class: "tabs-fill" });
    this.objectsPanel = new Div(null, { class: "inspect-objects" });
    objectsTab.addTab("Objects", this.objectsPanel);

    this._backButton = new Button(objectsTab.headerElement, { label: "<", style: "font-weight: bold;", tooltip: "Back", disabled: true, callback: () => {
      const prev = this._back.pop();
      if (prev) {
        if (this.inspectedObject) this._forward.push(this.inspectedObject);
        this.inspectObject(prev, true);
      }
      this._updateHistoryButtons();
    }});
    this._forwardButton = new Button(objectsTab.headerElement, { label: ">", class: "font-bold", tooltip: "Forward", disabled: true, callback: () => {
      const next = this._forward.pop();
      if (next) {
        if (this.inspectedObject) this._back.push(this.inspectedObject);
        this.inspectObject(next, true);
      }
      this._updateHistoryButtons();
    }});

    const filterRow = new Div(this.objectsPanel, { class: "inspector-filter-row", style: "padding: 6px 6px 2px 6px;" });
    new Span(filterRow, { text: "Search", class: "inspector-filter-label" });
    this._filterInput = new TextInput(filterRow, { placeholder: "name, type, format, id...", class: "inspector-filter-input", style: "width: 240px;" });
    this._filterInput.element.oninput = () => {
      this._filters.search = this._filterInput.value.trim().toLowerCase();
      this._applyFilter();
    };
    this._buildFilterPanel(this.objectsPanel);

    this.groupsContainer = new Div(this.objectsPanel);

    const pane2 = new Span(split, { style: "flex-grow: 1; overflow: hidden;" });
    const inspectTab = new TabWidget(pane2, { class: "inspector-tabs tabs-fill" });
    this.inspectPanel = new Div(null, { class: "inspector_panel_content" });
    inspectTab.addTab("Inspect", this.inspectPanel);
  }

  private _reset(): void {
    this._groups.clear();
    this._validationGroup = null;
    this._validationList = null;
    this._validationItems.clear();
    this._selectedValidation = null;
    this._leakGroup = null;
    this._leakList = null;
    this.groupsContainer.html = "";
    this.inspectPanel.html = "";
    this._frameTimePlot.reset();
    this._objectCountPlot.reset();
    this._frameTimeLabel.text = "Frame Time: --";
    this._updateMemoryLabel();
    this._selectedGroup = null;
    this._selectedObject = null;
    this.inspectedObject = null;
    this._back = [];
    this._forward = [];
    this._descriptorRequested.clear();
    this._shaderEdits.clear();
    this._openEditor = null;
    this._updateHistoryButtons();
  }

  private _updateHistoryButtons(): void {
    this._backButton.disabled = this._back.length === 0;
    this._forwardButton.disabled = this._forward.length === 0;
  }

  // ---------------------------------------------------------------------------------------
  // Object list

  private _groupFor(type: string): ObjectGroup {
    let g = this._groups.get(type);
    if (g) return g;
    g = new collapsible(null, { collapsed: true, label: `${typeLabel(type)} 0` }) as ObjectGroup;
    g.body.style.maxHeight = "320px";
    g.body.style.overflow = "auto";
    g.objectList = new Widget("ol", g.body, { style: "margin-top: 6px; margin-bottom: 6px;" });
    g.type = type;
    g.total = 0;
    g.visibleCount = 0;
    const group = g;
    g.onExpanded.addListener(() => {
      if (this._selectedGroup && this._selectedGroup !== group) this._selectedGroup.collapsed = true;
      this._selectedGroup = group;
    });
    this._groups.set(type, g);

    // Insert in display order.
    const order = (t: string): number => { const i = TYPE_ORDER.indexOf(t); return i < 0 ? TYPE_ORDER.length : i; };
    let before: ObjectGroup | null = null;
    for (const s of this.groupsContainer.children as ObjectGroup[]) {
      if (order(s.type) > order(type) || (order(s.type) === order(type) && s.type > type)) { before = s; break; }
    }
    if (before) this.groupsContainer.insertBefore(g, before);
    else this.groupsContainer.appendChild(g);
    return g;
  }

  private _setGroupLabel(g: ObjectGroup): void {
    g.label.text = this._isFilterActive() ? `${typeLabel(g.type)} ${g.visibleCount}/${g.total}` : `${typeLabel(g.type)} ${g.total}`;
  }

  // ---------------------------------------------------------------------------------------
  // Filters

  /** The collapsible filter panel under the search box: per-type criteria, like WebGPU Inspector's. */
  private _buildFilterPanel(parent: Widget): void {
    const f = this._filters;
    const apply = (): void => this._applyFilter();
    const panel = new collapsible(parent, { collapsed: true, label: "Filters", class: "inspector-filter-panel" });

    const captureRow = new Div(panel.body, { class: "inspector-filter-row" });
    const captureCheck = new Checkbox(captureRow, { label: "Only objects used in the last capture", class: "inspector-filter-field" });
    captureCheck.input.onchange = () => {
      f.onlyInLastCapture = captureCheck.checked;
      apply();
    };

    const textField = (row: Widget, label: string, placeholder: string, set: (v: string) => void, small = true): void => {
      const field = new Div(row, { class: "inspector-filter-field" });
      new Span(field, { text: label, class: "inspector-filter-label-sm" });
      const input = new TextInput(field, { placeholder, class: small ? "inspector-filter-input-sm" : "inspector-filter-input" });
      input.element.oninput = () => {
        set(input.value.trim().toLowerCase());
        apply();
      };
    };
    const checks = (row: Widget, label: string, items: [string, string][], into: Set<string>): void => {
      new Span(row, { text: label, class: "inspector-filter-label-sm" });
      for (const [text, key] of items) {
        const c = new Checkbox(row, { label: text, class: "inspector-filter-field" });
        c.input.onchange = () => {
          if (c.checked) into.add(key); else into.delete(key);
          apply();
        };
      }
    };
    const group = (label: string): Div => {
      const g = new collapsible(panel.body, { collapsed: true, label, class: "inspector-filter-group" });
      return g.body;
    };

    const images = group("Images / Image Views");
    const imgRow = new Div(images, { class: "inspector-filter-row" });
    textField(imgRow, "Format:", "e.g. r8g8b8a8", (v) => { f.image.format = v; });
    textField(imgRow, "Width:", ">=256", (v) => { f.image.width = v; });
    textField(imgRow, "Height:", ">=256", (v) => { f.image.height = v; });
    textField(imgRow, "Layers:", ">1", (v) => { f.image.layers = v; });
    checks(new Div(images, { class: "inspector-filter-row" }), "Usage:", IMAGE_USAGES, f.image.usage);

    const buffers = group("Buffers");
    textField(new Div(buffers, { class: "inspector-filter-row" }), "Size:", ">=1024", (v) => { f.buffer.size = v; });
    checks(new Div(buffers, { class: "inspector-filter-row" }), "Usage:", BUFFER_USAGES, f.buffer.usage);

    const shaders = group("Shader Modules / Pipelines");
    checks(new Div(shaders, { class: "inspector-filter-row" }), "Stage:", SHADER_STAGES, f.shader);

    const sets = group("Descriptor Sets");
    textField(new Div(sets, { class: "inspector-filter-row" }), "Contains:", "name or id of a bound resource", (v) => { f.descriptorSet.contains = v; }, false);
    new Div(sets, { text: "Matches sets whose contents have been read in the Inspect panel.", class: "text-muted font-sm" });
  }

  private _isFilterActive(): boolean {
    const f = this._filters;
    return !!f.search || f.onlyInLastCapture
      || !!(f.image.format || f.image.width || f.image.height || f.image.layers || f.image.usage.size)
      || !!(f.buffer.size || f.buffer.usage.size)
      || f.shader.size > 0
      || !!f.descriptorSet.contains;
  }

  /** Format, size and usage of an image (swapchain images take theirs from the swapchain). */
  private _imageInfo(image: VulkanObject): { format: string; width: number; height: number; layers: number; usage: string } | null {
    const db = this.database;
    if (image.cmd === "vkGetSwapchainImagesKHR") {
      const sd = db.getObject(image.parentId)?.descriptor;
      if (!sd) return null;
      const e = isObject(sd.imageExtent) ? sd.imageExtent : {};
      return { format: fmt(sd.imageFormat), width: num(e.width), height: num(e.height), layers: num(sd.imageArrayLayers) || 1, usage: str(sd.imageUsage) };
    }
    const d = image.descriptor;
    if (!d) return null;
    const e = isObject(d.extent) ? d.extent : {};
    const layers = d.imageType === "VK_IMAGE_TYPE_3D" ? num(e.depth) : num(d.arrayLayers);
    return { format: fmt(d.format), width: num(e.width), height: num(e.height), layers: layers || 1, usage: str(d.usage) };
  }

  private _imageMatches(image: VulkanObject | null, viewFormat: string | null): boolean {
    const f = this._filters.image;
    const active = f.format || f.width || f.height || f.layers || f.usage.size;
    if (!active) return true;
    const info = image ? this._imageInfo(image) : null;
    if (!info) return false;
    if (f.format && !(viewFormat ?? info.format).toLowerCase().includes(f.format) && !info.format.toLowerCase().includes(f.format)) return false;
    if (!numericMatches(parseNumeric(f.width), info.width)) return false;
    if (!numericMatches(parseNumeric(f.height), info.height)) return false;
    if (!numericMatches(parseNumeric(f.layers), info.layers)) return false;
    for (const u of f.usage) if (!info.usage.includes(`VK_IMAGE_USAGE_${u}_BIT`)) return false;
    return true;
  }

  private _bufferMatches(buffer: VulkanObject): boolean {
    const f = this._filters.buffer;
    const d = buffer.descriptor;
    if (f.size && !numericMatches(parseNumeric(f.size), num(d?.size))) return false;
    const usage = str(d?.usage);
    for (const u of f.usage) if (!usage.includes(`VK_BUFFER_USAGE_${u}_BIT`)) return false;
    return true;
  }

  /** Stage bits of a pipeline, or of every pipeline that uses a shader module. */
  private _stagesOf(object: VulkanObject): Set<string> {
    const out = new Set<string>();
    const addPipeline = (p: VulkanObject, module: VulkanObject | null): void => {
      const d = p.descriptor;
      if (!d) return;
      const stages = Array.isArray(d.pStages) ? d.pStages : isObject(d.stage) ? [d.stage] : [];
      for (const s of stages) {
        if (!isObject(s)) continue;
        if (module && refId(s.module) !== module.id) continue;
        out.add(str(s.stage));
      }
    };
    if (object.type === "VkPipeline") addPipeline(object, null);
    else for (const dep of object.dependents) if (dep.type === "VkPipeline") addPipeline(dep, object);
    return out;
  }

  private _descriptorSetContains(set: VulkanObject, query: string): boolean {
    const bindings = set.updates.bindings;
    if (!Array.isArray(bindings)) return false;
    const ids = new Set<number>();
    this.database.collectReferences(bindings, ids);
    for (const id of ids) {
      const o = this.database.getObject(id);
      if (!o) continue;
      if (String(o.id) === query || o.name.toLowerCase().includes(query)) return true;
      // A view's image counts as contained too.
      if (o.type === "VkImageView") {
        const image = this.database.getObject(refId(o.descriptor?.image));
        if (image && (String(image.id) === query || image.name.toLowerCase().includes(query))) return true;
      }
    }
    return false;
  }

  private _matches(object: VulkanObject): boolean {
    const f = this._filters;
    if (f.search) {
      const q = f.search;
      if (String(object.id) !== q && !object.name.toLowerCase().includes(q) && !object.type.toLowerCase().includes(q)
          && !object.summary(this.database).toLowerCase().includes(q)) return false;
    }
    if (f.onlyInLastCapture && !this.database.capturedObjects.has(object.id)) return false;
    switch (object.type) {
      case "VkImage":
        return this._imageMatches(object, null);
      case "VkImageView":
        return this._imageMatches(this.database.getObject(refId(object.descriptor?.image)), fmt(object.descriptor?.format));
      case "VkBuffer":
        return this._bufferMatches(object);
      case "VkShaderModule":
      case "VkPipeline": {
        if (!f.shader.size) return true;
        const stages = this._stagesOf(object);
        for (const s of f.shader) if (stages.has(`VK_SHADER_STAGE_${s}_BIT`)) return true;
        return false;
      }
      case "VkDescriptorSet":
        return !f.descriptorSet.contains || this._descriptorSetContains(object, f.descriptorSet.contains);
      default:
        return true;
    }
  }

  private _addObject(object: VulkanObject): void {
    const g = this._groupFor(object.type);
    const item = new Widget("li", g.objectList, { class: "object-item" }) as ObjectItem;
    item.group = g;
    object.widget = item;
    this._fillItem(object, item);
    item.element.onclick = () => {
      const prev = this._selectedObject?.widget as ObjectItem | null;
      if (prev) prev.element.classList.remove("selected");
      if (this._selectedValidation) {
        this._validationItems.get(this._selectedValidation.key)?.element.classList.remove("selected");
        this._selectedValidation = null;
      }
      this._selectedObject = object;
      item.element.classList.add("selected");
      this.inspectObject(object);
    };
    g.total++;
    const visible = this._matches(object);
    item.element.style.display = visible ? "" : "none";
    if (visible) g.visibleCount++;
    this._setGroupLabel(g);
    if (object.type === "VkDeviceMemory" || object.type === "VkBuffer" || object.type === "VkImage") this._updateMemoryLabel();
  }

  private _fillItem(object: VulkanObject, item: Widget): void {
    item.html = "";
    const edited = object.edited || (object.type === "VkPipeline" && object.label.endsWith("(edited)"));
    if (edited) new Span(item, { text: "✎ ", class: "object-item-edited", tooltip: object.edited ? "A shader of this object has been edited" : "Replacement pipeline built from an edited shader" });
    new Span(item, { text: object.name, class: "object-item-name" });
    new Span(item, { text: ` ${object.summary(this.database)}`, class: "object-item-type" });
    item.element.title = `${object.type} ${object.id} (${object.handle})`;
    if (object.isInvalid) {
      item.element.classList.add("error");
      item.tooltip = object.invalidReason ?? "";
    }
    const validation = this.database.validationFor(object.id);
    if (validation.length) {
      item.element.classList.add("error");
      item.tooltip = validation.map((v) => this._validationItemText(v)).slice(0, 5).join("\n");
    }
  }

  private _deleteObject(id: number, object: VulkanObject): void {
    const g = this._groups.get(object.type);
    const item = object.widget as ObjectItem | null;
    if (item) {
      const wasVisible = item.element.style.display !== "none";
      item.remove();
      object.widget = null;
      if (g) {
        g.total--;
        if (wasVisible) g.visibleCount--;
        this._setGroupLabel(g);
      }
    }
    if (this.inspectedObject === object) {
      this.inspectedObject = null;
      this._showDeleted(object);
    }
  }

  private _objectChanged(object: VulkanObject): void {
    if (object.widget) this._fillItem(object, object.widget as Widget);
    if (this.inspectedObject === object) this._inspectObject(object);
  }

  private _objectUpdated(object: VulkanObject): void {
    if (this.inspectedObject === object) this._inspectObject(object);
  }

  private _applyFilter(): void {
    for (const g of this._groups.values()) {
      g.visibleCount = 0;
      const objects = this.database.getObjectsOfType(g.type);
      if (!objects) continue;
      for (const o of objects.values()) {
        const v = this._matches(o);
        const item = o.widget as ObjectItem | null;
        if (item) item.element.style.display = v ? "" : "none";
        if (v) g.visibleCount++;
      }
      this._setGroupLabel(g);
    }
  }

  // ---------------------------------------------------------------------------------------
  // Object details

  inspectObject(object: VulkanObject, skipHistory = false): void {
    if (!skipHistory && this.inspectedObject && this.inspectedObject !== object) {
      this._back.push(this.inspectedObject);
      this._forward = [];
    }
    this._updateHistoryButtons();
    this.inspectedObject = object;
    this._inspectObject(object);
  }

  revealObject(object: VulkanObject | null): void {
    const item = object?.widget as ObjectItem | null;
    if (!item) return;
    item.group.expand();
    item.element.scrollIntoView({ block: "nearest" });
    item.element.click();
  }

  private _showDeleted(object: VulkanObject): void {
    this.inspectPanel.html = "";
    const box = new Div(this.inspectPanel, { class: "info-box info-box-error" });
    new Div(box, { text: `${object.name} ID: ${object.id} <destroyed>` });
  }

  private _inspectObject(object: VulkanObject): void {
    this.inspectPanel.html = "";
    this.database.inspectedObject = object;
    const db = this.database;
    const onLink = (o: VulkanObject) => this.revealObject(o);

    const infoBox = new Div(this.inspectPanel, { class: object.isInvalid ? "info-box info-box-error" : "info-box info-box-success", style: "flex: 0 0 auto;" });
    new Div(infoBox, { text: `${object.name}`, class: "font-lg" });
    new Div(infoBox, { text: `${object.type}  ID: ${object.id}  handle: ${object.handle}`, class: "font-md text-muted" });
    const summary = object.summary(db);
    if (summary) new Div(infoBox, { text: summary, class: "font-md" });
    new Div(infoBox, { text: `Created by ${object.cmd}${object.index ? ` [${object.index}]` : ""}`, class: "font-md text-muted" });
    if (object.isInvalid) new Div(infoBox, { text: `Invalid: ${object.invalidReason}`, class: "inspect_info_error" });
    const validation = db.validationFor(object.id);
    if (validation.length) {
      infoBox.classList.remove("info-box-success");
      infoBox.classList.add("info-box-error");
      const grp = new collapsible(infoBox, { label: `Validation (${validation.length})`, collapsed: false });
      for (const v of validation) {
        const row = new Div(grp.body, { class: "validation-object-row" });
        new Span(row, { text: v.severity === "error" ? "✖ " : "⚠ ", class: `validation-sev validation-sev-${v.severity}` });
        new Span(row, { text: this._validationItemText(v), class: "validation-text dependency_link" });
        if (v.count > 1) new Span(row, { text: ` ×${v.count}`, class: "validation-count" });
        row.element.onclick = () => this._selectValidation(v);
        row.tooltip = v.message;
      }
    }

    const parent = db.getObject(object.parentId);
    if (parent) {
      const row = new Div(infoBox, { class: "font-md text-muted" });
      new Span(row, { text: "Owner:", style: "margin-right: 4px;" });
      objectLink(row, parent, onLink);
    }
    const memory = object.updates.memory;
    if (isHandleRef(memory)) {
      const row = new Div(infoBox, { class: "font-md text-muted" });
      new Span(row, { text: "Memory:", style: "margin-right: 4px;" });
      const mem = db.getObject(memory.__id);
      if (mem) objectLink(row, mem, onLink); else new Span(row, { text: "(destroyed)" });
      new Span(row, { text: `at offset ${String(object.updates.memoryOffset ?? 0)}`, style: "margin-left: 4px;" });
    }

    if (object.dependencies.size) {
      const grp = new collapsible(infoBox, { label: `Dependencies (${object.dependencies.size})`, collapsed: false });
      const ul = new Widget("ul", grp.body, { class: "dependency-list" });
      for (const dep of object.dependencies) objectLink(new Widget("li", ul), dep, onLink, true);
    }
    if (object.dependents.size) {
      const grp = new collapsible(infoBox, { label: `Used by (${object.dependents.size})`, collapsed: object.dependents.size > 8 });
      const ul = new Widget("ul", grp.body, { class: "dependency-list" });
      let n = 0;
      for (const dep of object.dependents) {
        if (n++ >= 200) { new Widget("li", ul, { text: `... ${object.dependents.size - 200} more` }); break; }
        objectLink(new Widget("li", ul), dep, onLink, true);
      }
    }

    if (object.type === "VkShaderModule" || object.type === "VkPipeline") this._buildShaderSection(object);
    if (object.type === "VkPhysicalDevice") renderPhysicalDeviceSections(this.inspectPanel, object);
    if (object.type === "VkDevice") renderDeviceSections(this.inspectPanel, object);
    if (object.type === "VkInstance") renderInstanceSections(this.inspectPanel, object);
    if (object.type === "VkDescriptorSet") this._buildDescriptorSetSection(object);
    this._imageView = null;
    if (object.type === "VkImage" || object.type === "VkImageView") {
      const grp = new collapsible(this.inspectPanel, { label: "Image", collapsed: false });
      this._imageView = new ImageView(grp.body, this.window, object);
    }

    const argsGrp = new collapsible(this.inspectPanel, { label: "Arguments", collapsed: false });
    const tree = new Div(argsGrp.body, { class: "args-tree" });
    renderArgs(tree, object.args, db, onLink);
  }

  // Descriptor set contents: asked from the layer (RequestDescriptorSet), which answers with an
  // ObjectUpdate carrying `bindings` in the shape of a capture's descriptor snapshot.
  private _buildDescriptorSetSection(object: VulkanObject): void {
    const db = this.database;
    const onLink = (o: VulkanObject) => this.revealObject(o);
    const grp = new collapsible(this.inspectPanel, { label: "Contents", collapsed: false });
    const bar = new Div(grp.body, { class: "shader-toolbar" });
    new Button(bar, { label: "Refresh", class: "btn btn-sm", tooltip: "Read the set's current contents from the application", callback: () => this._requestDescriptorSet(object) });
    const layout = db.getObject(refId(object.updates.layout));
    if (layout) {
      new Span(bar, { text: "Layout: ", class: "text-muted font-md" });
      objectLink(bar, layout, onLink);
    }

    const bindings = object.updates.bindings;
    if (!Array.isArray(bindings)) {
      new Div(grp.body, { text: this.window.connected ? "Reading contents..." : "Not connected: contents cannot be read.", class: "text-muted" });
      if (!this._descriptorRequested.has(object.id)) this._requestDescriptorSet(object);
      return;
    }
    if (object.updates.tracked === false) {
      new Div(grp.body, { text: "The layer has no record of this set's contents (freed, or its pool was reset).", class: "text-muted" });
      return;
    }
    if (!bindings.length) {
      new Div(grp.body, { text: "No bindings.", class: "text-muted" });
      return;
    }
    const list = new Widget("ul", grp.body, { class: "descriptor-contents" });
    for (const b of bindings as unknown as CaptureDescriptorBinding[]) {
      const count = b.descriptors.length;
      b.descriptors.forEach((d, k) => {
        const li = new Widget("li", list);
        new Span(li, { text: `Binding ${b.binding}${count > 1 ? `[${k}]` : ""}: `, class: "buffer-member" });
        new Span(li, { text: `${fmt(b.type)}  `, class: "args-enum" });
        if (!d) {
          new Span(li, { text: "(not written)", class: "text-muted" });
          return;
        }
        const link = (v: HandleRef | null | undefined, missing: string): void => {
          const o = db.getObject(refId(v));
          if (o) objectLink(li, o, onLink); else new Span(li, { text: missing, class: "text-muted" });
        };
        if (d.buffer !== undefined) {
          link(d.buffer, "(destroyed buffer)");
          new Span(li, { text: `  offset ${num(d.offset)}  range ${num(d.range)}`, class: "text-muted" });
        }
        if (d.imageView !== undefined) {
          link(d.imageView, "(destroyed view)");
          const view = db.getObject(refId(d.imageView));
          if (view) new Span(li, { text: `  ${view.summary(db)}`, class: "text-muted" });
          if (d.imageLayout) new Span(li, { text: `  ${fmt(d.imageLayout)}`, class: "text-muted" });
        }
        if (d.sampler !== undefined) {
          new Span(li, { text: d.imageView !== undefined ? "  sampler " : "", class: "text-muted" });
          link(d.sampler, "(destroyed sampler)");
          if (d.immutable) new Span(li, { text: " (immutable)", class: "text-muted" });
        }
        if (d.bufferView !== undefined) link(d.bufferView, "(destroyed buffer view)");
        if (b.stages) new Span(li, { text: `  ${fmtFlags(b.stages)}`, class: "text-muted font-sm" });
      });
    }
  }

  private _requestDescriptorSet(object: VulkanObject): void {
    if (!this.window.connected) return;
    this._descriptorRequested.add(object.id);
    void this.window.send({ action: "RequestDescriptorSet", id: object.id });
  }

  // Shader code: one sub-section per SPIR-V payload with disassembly / GLSL / HLSL views.
  private _buildShaderSection(object: VulkanObject): void {
    this._shaderViews = new Map();
    if (!object.blobs.length) {
      const grp = new collapsible(this.inspectPanel, { label: "Shader Code", collapsed: false });
      new Div(grp.body, { text: object.type === "VkShaderModule" ? "No shader code recorded." : "No shader stages recorded.", class: "text-muted" });
      return;
    }
    this._openEditor = null;
    object.blobs.forEach((blob, index) => {
      const key = `${object.id}:${index}`;
      const edit = this._shaderEdits.get(key);
      const grp = new collapsible(this.inspectPanel, { label: `Shader: ${blob.name} (${blob.size} bytes)${edit?.applied ? "  [edited]" : ""}`, collapsed: index > 0 });
      const bar = new Div(grp.body, { class: "shader-toolbar" });
      const view: ShaderView = {
        index, blobName: blob.name, pre: new Widget("pre"), mode: "dis", data: null, text: "", buttons: {},
        editButton: new Button(null), editor: null, body: grp.body, debug: null, sourceFile: 0, disText: "",
        summary: new Div(null), fileBar: null, reflection: new collapsible(null), group: grp,
      };
      // Reflection (entry points, interface, resources, push constants) from the SPIR-V itself,
      // so a module or pipeline explains what it expects without a capture.
      view.group = grp;
      view.reflection = new collapsible(grp.body, { label: "Reflection", collapsed: true, class: "shader-reflection" });
      new Div(view.reflection.body, { text: "Loading...", class: "text-muted font-sm" });
      // Source: the text the compiler embedded in the SPIR-V (shown once the payload says it has one).
      view.buttons.source = new Button(bar, { label: "Source", class: "btn btn-sm", tooltip: "The original source embedded in the SPIR-V by the compiler", callback: () => void this._showShader(index, "source") });
      view.buttons.source.style.display = "none";
      const modes: [ShaderTextMode, string][] = [["dis", "SPIR-V"], ["glsl", "GLSL"], ["hlsl", "HLSL"]];
      for (const [mode, label] of modes) {
        view.buttons[mode] = new Button(bar, { label, class: "btn btn-sm", callback: () => void this._showShader(index, mode) });
      }
      view.editButton = new Button(bar, { label: edit?.applied ? "Edit (edited)" : "Edit", class: "btn btn-sm shader-edit-button", disabled: true,
        tooltip: "Edit the shown text and compile it into the running application", callback: () => this._openShaderEditor(object, view) });
      view.summary = new Div(grp.body, { class: "shader-debug-summary text-muted font-sm" });
      view.pre = new Widget("pre", grp.body, { text: "Loading...", class: "shader-text" });
      this._shaderViews.set(index, view);
      void this.window.send({ action: "RequestBlob", id: object.id, index });
    });
  }

  private _fillReflection(view: ShaderView, data: Uint8Array): void {
    const body = view.reflection.body;
    body.html = "";
    let reflection = null;
    try {
      reflection = reflectSpirv(data);
    } catch (e) {
      new Div(body, { text: `Reflection failed: ${(e as Error).message}`, class: "text-muted font-sm" });
      return;
    }
    if (!reflection) {
      new Div(body, { text: "Not a SPIR-V module.", class: "text-muted font-sm" });
      return;
    }
    // Pipeline payloads are named "<stage>:<entry point>": show that entry point only.
    const sep = view.blobName.indexOf(":");
    const entryPoint = sep > 0 ? view.blobName.substring(sep + 1) : undefined;
    const info = new Div(body, { class: "shader-info" });
    renderReflection(info, reflection, { entryPoint, showStage: true });
    const entries = entryPoint ? 1 : reflection.entryPoints.length;
    view.reflection.label.text = `Reflection: ${entries} entry point${entries === 1 ? "" : "s"}, ${reflection.resources.length} resource${reflection.resources.length === 1 ? "" : "s"}${reflection.pushConstants.length ? ", push constants" : ""}`;
  }

  private _setShaderMode(view: ShaderView, mode: ShaderViewMode): void {
    view.mode = mode;
    for (const m of Object.keys(view.buttons) as ShaderViewMode[]) view.buttons[m]?.element.classList.toggle("active", m === mode);
    if (view.fileBar) view.fileBar.style.display = mode === "source" ? "" : "none";
  }

  private async _showShader(index: number, mode: ShaderViewMode): Promise<void> {
    const view = this._shaderViews.get(index);
    if (!view || !view.data) return;
    this._setShaderMode(view, mode);
    if (mode === "source") {
      this._renderSourceView(view, 0);
      return;
    }
    let r: { ok: boolean; text: string };
    if (mode === "dis" && view.disText) {
      r = { ok: true, text: view.disText };
    } else {
      view.pre.text = "Converting...";
      r = await window.inspector.shaderText(view.data, mode);
    }
    if (view.mode !== mode) return;
    view.text = r.text;
    const language = LANGUAGE_OF_MODE[mode];
    if (r.ok && mode === "dis") {
      view.disText = r.text;
      this._renderDisassembly(view, null);
    } else if (r.ok && language) {
      view.pre.html = highlight(r.text, language);
    } else {
      view.pre.text = r.text;
    }
    view.editButton.disabled = !r.ok || !this.window.connected;
  }

  private _objectBlob(id: number, index: number, data: Uint8Array | null): void {
    if (this.inspectedObject?.id !== id) return;
    const view = this._shaderViews.get(index);
    if (!view) return;
    if (!data) {
      view.pre.text = "No shader code available.";
      return;
    }
    view.data = data;
    view.disText = "";
    this._fillReflection(view, data);
    view.debug = parseSpirvDebugInfo(data);
    const info = view.debug;
    if (info) {
      view.summary.text = describeDebugInfo(info);
      if (!hasEmbeddedSource(info)) {
        view.summary.text += ". To embed the source, compile with -g (glslc, glslangValidator), -gVS (glslangValidator, NonSemantic form) or -fspv-debug=vulkan-with-source (dxc).";
      }
    }
    if (info && hasEmbeddedSource(info)) {
      view.buttons.source!.style.display = "";
      view.sourceFile = info.mainFile >= 0 ? info.mainFile : info.files.findIndex((f) => f.text !== null);
      const withText = info.files.filter((f) => f.text !== null);
      if (withText.length > 1) {
        // Several files (includes): one button each.
        const fileBar = new Div(null, { class: "shader-toolbar shader-file-bar" });
        view.body.insertBefore(fileBar, view.pre);
        info.files.forEach((f, i) => {
          if (f.text === null) return;
          const b = new Button(fileBar, { label: f.name, class: "btn btn-sm", tooltip: `${f.text.split("\n").length} lines`, callback: () => {
            view.sourceFile = i;
            this._renderSourceView(view, 0);
          } });
          b.element.dataset.file = String(i);
        });
        view.fileBar = fileBar;
      }
      void this._showShader(index, "source");
    } else {
      void this._showShader(index, view.mode === "source" ? "dis" : view.mode);
    }
  }

  /** The embedded source of `view.sourceFile` with line numbers; lines that instructions map to jump to them. */
  private _renderSourceView(view: ShaderView, activeLine: number): void {
    const info = view.debug;
    const file = info?.files[view.sourceFile];
    if (!info || !file || file.text === null) {
      view.pre.text = "No embedded source.";
      view.editButton.disabled = true;
      return;
    }
    if (view.fileBar) {
      for (const b of Array.from(view.fileBar.element.querySelectorAll("button"))) b.classList.toggle("active", b.dataset.file === String(view.sourceFile));
    }
    const language = sourceLanguageOf(info);
    view.text = file.text;
    const map = sourceLineMap(file.text);
    const lines = language ? highlightLines(file.text, language) : file.text.split("\n").map(escapeHtml);
    while (lines.length > map.lines.length) lines.pop();
    const mapped = new Set<number>();
    for (const loc of info.locations) if (loc && loc.file === view.sourceFile) mapped.add(loc.line);
    const width = String(Math.max(...map.lineOf, 1)).length;
    let html = "";
    for (let i = 0; i < lines.length; i++) {
      const n = map.lineOf[i];
      const isMapped = n > 0 && mapped.has(n);
      const cls = `code-line${isMapped ? " code-line-mapped" : ""}${n > 0 && n === activeLine ? " code-line-active" : ""}${n ? "" : " code-line-unnumbered"}`;
      const title = isMapped ? ' title="Show the SPIR-V instructions of this line"' : "";
      html += `<span class="${cls}" data-line="${n}"${title}><span class="code-lineno">${(n ? String(n) : "").padStart(width)}</span>${lines[i]}</span>\n`;
    }
    view.pre.html = html;
    view.pre.element.onclick = (e) => {
      const el = (e.target as HTMLElement).closest(".code-line-mapped") as HTMLElement | null;
      if (el) void this._jumpToDisassembly(view, view.sourceFile, Number(el.dataset.line));
    };
    if (activeLine) view.pre.element.querySelector(".code-line-active")?.scrollIntoView({ block: "center" });
    view.editButton.disabled = !language || !this.window.connected;
    view.editButton.tooltip = language
      ? "Edit the embedded source and compile it into the running application"
      : `The shader editor cannot compile ${info.language}; edit the GLSL, HLSL or SPIR-V view instead`;
  }

  /**
   * The spirv-dis output. With line information every instruction is wrapped in a span that
   * names its source line, annotated with the source text where the line changes, and clicking
   * it shows that line in the Source view.
   */
  private _renderDisassembly(view: ShaderView, active: { file: number; line: number } | null): void {
    const text = view.disText;
    const info = view.debug;
    const html = highlightLines(text, "spirv-asm");
    if (!info || info.form === "none") {
      view.pre.html = html.join("\n");
      view.pre.element.onclick = null;
      return;
    }
    const textLines = text.split("\n");
    const instructions = disassemblyInstructions(textLines);
    const prefix = new Array<string>(textLines.length).fill("");
    const suffix = new Array<string>(textLines.length).fill("");
    if (instructions.length === info.locations.length) {
      const fileMaps = info.files.map((f) => (f.text === null ? null : sourceLineMap(f.text)));
      let prev: DebugLocation | null = null;
      let firstActive = -1;
      instructions.forEach((ls, k) => {
        const loc = info.locations[k];
        if (!loc) {
          prev = null;
          return;
        }
        const isActive = !!active && active.file === loc.file && active.line === loc.line;
        if (isActive && firstActive < 0) firstActive = ls[0];
        const name = info.files[loc.file]?.name ?? "?";
        prefix[ls[0]] = `<span class="code-line code-line-mapped${isActive ? " code-line-active" : ""}" data-file="${loc.file}" data-line="${loc.line}" title="${escapeHtml(name)}:${loc.line}">`;
        let close = "</span>";
        if (!prev || prev.file !== loc.file || prev.line !== loc.line) {
          const fm = fileMaps[loc.file];
          const phys = fm?.physicalOf.get(loc.line);
          const src = fm && phys !== undefined ? fm.lines[phys].trim() : "";
          close = `  <span class="tok-srcmap">; ${escapeHtml(name)}:${loc.line}${src ? `  ${escapeHtml(src)}` : ""}</span>${close}`;
        }
        suffix[ls[ls.length - 1]] = close;
        prev = loc;
      });
    } else {
      console.warn(`spirv-dis printed ${instructions.length} instructions, the module has ${info.locations.length}; line mapping disabled`);
    }
    view.pre.html = html.map((h, i) => prefix[i] + h + suffix[i]).join("\n");
    view.pre.element.onclick = (e) => {
      const el = (e.target as HTMLElement).closest(".code-line-mapped") as HTMLElement | null;
      if (el && hasEmbeddedSource(info)) this._jumpToSource(view, Number(el.dataset.file), Number(el.dataset.line));
    };
    if (active) view.pre.element.querySelector(".code-line-active")?.scrollIntoView({ block: "center" });
  }

  private _jumpToSource(view: ShaderView, file: number, line: number): void {
    if (view.debug?.files[file]?.text === null) return;
    view.sourceFile = file;
    this._setShaderMode(view, "source");
    this._renderSourceView(view, line);
  }

  private async _jumpToDisassembly(view: ShaderView, file: number, line: number): Promise<void> {
    if (!view.data) return;
    this._setShaderMode(view, "dis");
    if (!view.disText) {
      view.pre.text = "Converting...";
      const r = await window.inspector.shaderText(view.data, "dis");
      if (view.mode !== "dis") return;
      if (!r.ok) {
        view.pre.text = r.text;
        return;
      }
      view.disText = r.text;
    }
    view.text = view.disText;
    this._renderDisassembly(view, { file, line });
    view.editButton.disabled = !this.window.connected;
  }

  // ---------------------------------------------------------------------------------------
  // Shader editor: edit the decompiled text, compile it with the SDK, have the layer rebuild
  // the pipeline(s) with the new code (see layer/src/shader_edit.h).

  /** The pipelines an edit of this payload applies to: the pipeline itself, or every pipeline using the module. */
  private _editTargets(object: VulkanObject, view: ShaderView): EditTargets | null {
    if (!view.data) return null;
    const reflection = reflectSpirv(view.data);
    const spirvVersion = reflection?.version ?? "1.5";
    if (object.type === "VkPipeline") {
      // Pipeline payloads are named "<stage>:<entry point>".
      const sep = view.blobName.indexOf(":");
      const stage = (sep > 0 ? view.blobName.substring(0, sep) : view.blobName) as ShaderStage;
      const entryPoint = sep > 0 ? view.blobName.substring(sep + 1) : "main";
      if (!STAGE_FLAG[stage]) return null;
      return { pipelines: [object], stage, stageFlag: STAGE_FLAG[stage], entryPoint, spirvVersion };
    }
    // A shader module: the stage comes from its entry point, the pipelines from its dependents.
    const entry = reflection?.entryPoints[0];
    if (!entry || !STAGE_FLAG[entry.stage]) return null;
    const pipelines: VulkanObject[] = [];
    for (const dep of object.dependents) {
      if (dep.type !== "VkPipeline" || dep.isDeleted) continue;
      const d = dep.descriptor;
      if (!d) continue;
      const stages = Array.isArray(d.pStages) ? d.pStages : isObject(d.stage) ? [d.stage] : [];
      if (stages.some((s) => isObject(s) && refId(s.module) === object.id)) pipelines.push(dep);
    }
    return { pipelines, stage: entry.stage, stageFlag: STAGE_FLAG[entry.stage], entryPoint: entry.name, spirvVersion };
  }

  private _openShaderEditor(object: VulkanObject, view: ShaderView): void {
    if (view.editor) {
      view.editor.remove();
      view.editor = null;
      view.pre.style.display = "";
      this._openEditor = null;
      return;
    }
    const key = `${object.id}:${view.index}`;
    const language = view.mode === "source" ? sourceLanguageOf(view.debug) : LANGUAGE_OF_MODE[view.mode];
    if (!language) return;
    const targets = this._editTargets(object, view);
    const existing = this._shaderEdits.get(key);
    const editor = new Div(null, { class: "shader-editor" });
    view.body.insertBefore(editor, view.pre);
    view.pre.style.display = "none";
    view.editor = editor;

    const head = new Div(editor, { class: "shader-editor-head" });
    const fromSource = view.mode === "source" && view.debug;
    new Span(head, { text: fromSource ? `Editing the embedded ${view.debug!.files[view.sourceFile]?.name ?? "source"} as ${LANGUAGE_LABEL[language]}` : `Editing as ${LANGUAGE_LABEL[language]}`, class: "font-md" });
    if (targets) {
      const where = object.type === "VkPipeline" ? "this pipeline" : `${targets.pipelines.length} pipeline${targets.pipelines.length === 1 ? "" : "s"} using this module`;
      new Span(head, { text: `  ${stageLabel(targets.stage)} stage, entry ${targets.entryPoint}, applies to ${where}`, class: "text-muted font-sm" });
    } else {
      new Span(head, { text: "  Cannot determine the stage of this code; edits cannot be applied.", class: "inspect_info_error" });
    }
    if (object.type === "VkPipeline" && targets && view.mode === "glsl") {
      new Div(editor, { text: "Tip: names the application stripped appear as _m0, _m1... in the decompiled source; that is fine, the layout is what matters.", class: "text-muted font-sm" });
    }
    if (fromSource && view.debug!.files.filter((f) => f.text !== null).length > 1) {
      new Div(editor, { text: "Note: the compiler is given only this file; #include directives cannot be resolved, so paste the included code in if the compile needs it.", class: "text-muted font-sm" });
    }

    // Embedded source carries the compiler's own prefix (comments, a #line directive before
    // #version) that a compiler will not take back; edit the clean text.
    const source = existing && existing.language === language ? existing.source : fromSource ? compilableSource(view.text) : view.text;
    const text = new CodeEditor(editor, { value: source, language, class: "shader-editor-text" });
    const buttons = new Div(editor, { class: "shader-toolbar" });
    const status = new Div(editor, { class: "shader-editor-status text-muted font-sm" });
    const log = new Widget("pre", editor, { class: "shader-editor-log", style: "display: none;" });
    this._openEditor = targets ? { key, targets, status, view } : null;
    if (existing?.results.size) status.text = [...existing.results.values()].join("\n");

    new Button(buttons, { label: "Compile & Apply", class: "btn btn-success btn-sm", disabled: !targets || !targets.pipelines.length, callback: () => {
      if (!targets) return;
      const edit: ShaderEdit = { language, source: text.value, results: new Map(), applied: false };
      this._shaderEdits.set(key, edit);
      status.text = `Compiling with ${LANGUAGE_LABEL[language]}...`;
      log.style.display = "none";
      void window.inspector.compileShader(text.value, language, targets.stage, targets.entryPoint, targets.spirvVersion).then((r) => {
        if (r.log) {
          log.text = r.log;
          log.style.display = "";
        }
        if (!r.ok || !r.spirv) {
          status.text = `${r.tool}: compilation failed.`;
          return;
        }
        const spirv = encodeBase64(r.spirv);
        status.text = `${r.tool}: ${r.spirv.byteLength} bytes of SPIR-V. Applying to ${targets.pipelines.length} pipeline${targets.pipelines.length === 1 ? "" : "s"}...`;
        for (const p of targets.pipelines) {
          edit.results.set(p.id, `${p.name}: applying...`);
          void this.window.send({ action: "ReplaceShader", pipeline: p.id, stage: targets.stageFlag, spirv });
        }
      });
    } });
    new Button(buttons, { label: "Restore Original", class: "btn btn-sm", disabled: !targets || !targets.pipelines.length,
      tooltip: "Bind the application's own pipeline again", callback: () => {
      if (!targets) return;
      const edit = this._shaderEdits.get(key) ?? { language, source: text.value, results: new Map<number, string>(), applied: false };
      edit.results.clear();
      edit.applied = false;
      this._shaderEdits.set(key, edit);
      status.text = "Restoring...";
      for (const p of targets.pipelines) void this.window.send({ action: "RestoreShader", pipeline: p.id, stage: targets.stageFlag });
    } });
    new Button(buttons, { label: "Close", class: "btn btn-sm", callback: () => {
      const edit = this._shaderEdits.get(key);
      if (edit) edit.source = text.value;
      else if (text.value !== view.text) this._shaderEdits.set(key, { language, source: text.value, results: new Map(), applied: false });
      this._openShaderEditor(object, view);
    } });
  }

  /** The layer's answer to a ReplaceShader / RestoreShader: shown in the open editor's status line. */
  private _shaderReplaced(msg: ShaderReplacedMessage): void {
    const open = this._openEditor;
    let edit: ShaderEdit | undefined;
    if (open) edit = this._shaderEdits.get(open.key);
    if (!open || !edit) return;
    if (!open.targets.pipelines.some((p) => p.id === msg.pipeline)) return;
    const pipeline = this.database.getObject(msg.pipeline);
    const name = pipeline?.name ?? `Pipeline ${msg.pipeline}`;
    const replacement = msg.replacement ? this.database.getObject(msg.replacement) : null;
    let line: string;
    if (msg.ok && msg.replacement) line = `${name}: edit applied${replacement ? ` as ${replacement.name}` : ""}${msg.note ? ` (${msg.note})` : ""}`;
    else if (msg.ok) line = `${name}: original restored`;
    else line = `${name}: failed: ${msg.error ?? "unknown error"}`;
    edit.results.set(msg.pipeline, line);
    edit.applied = [...edit.results.values()].some((l) => l.includes("edit applied"));
    open.status.text = [...edit.results.values()].join("\n");
    open.status.classList.toggle("inspect_info_error", !msg.ok);

    // Mark the pipeline (and the module the edit came from) in the object list and in the open
    // section, without re-rendering the panel: the editor stays open with its text.
    const inspected = this.inspectedObject;
    if (msg.ok && pipeline) {
      pipeline.edited = !!msg.replacement;
      if (pipeline.widget) this._fillItem(pipeline, pipeline.widget as Widget);
    }
    if (inspected && inspected.type === "VkShaderModule") {
      inspected.edited = open.targets.pipelines.some((p) => p.edited);
      if (inspected.widget) this._fillItem(inspected, inspected.widget as Widget);
    }
    const view = open.view;
    const blob = inspected?.blobs[view.index];
    view.group.label.text = `Shader: ${view.blobName}${blob ? ` (${blob.size} bytes)` : ""}${edit.applied ? "  [edited]" : ""}`;
    view.editButton.text = edit.applied ? "Edit (edited)" : "Edit";
  }
}
