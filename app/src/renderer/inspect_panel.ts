// Live object inspection: grouped object lists on the left, details of the selected object on
// the right. Structure follows WebGPU Inspector's devtools/inspect_panel.js (MIT, Brendan Duncan).
import { Button } from "./widget/button.js";
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Split } from "./widget/split.js";
import { TabWidget } from "./widget/tab_widget.js";
import { TextInput } from "./widget/text_input.js";
import { Widget } from "./widget/widget.js";
import { VulkanObject, isHandleRef } from "./vulkan/vulkan_object.js";
import { objectLink, renderArgs } from "./args_view.js";
import { ImageView } from "./image_view.js";
import type { SessionContext } from "./session_panel.js";
import type { ObjectDatabase } from "./vulkan/object_database.js";
import type { ShaderTextMode } from "../shared/protocol.js";

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

interface ShaderView {
  pre: Widget;
  mode: ShaderTextMode;
  data: Uint8Array | null;
  buttons: Partial<Record<ShaderTextMode, Button>>;
}

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
  private _filter = "";
  private _shaderViews = new Map<number, ShaderView>();
  private _imageView: ImageView | null = null;

  private objectsPanel!: Div;
  private groupsContainer!: Div;
  private inspectPanel!: Div;
  private _backButton!: Button;
  private _forwardButton!: Button;
  private _filterInput!: TextInput;

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
    });
  }

  private _build(): void {
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

    const filterRow = new Div(this.objectsPanel, { class: "inspector-filter-row", style: "padding: 6px;" });
    new Span(filterRow, { text: "Filter", class: "inspector-filter-label" });
    this._filterInput = new TextInput(filterRow, { placeholder: "name, type, format, id...", class: "inspector-filter-input", style: "width: 240px;" });
    this._filterInput.element.oninput = () => {
      this._filter = this._filterInput.value.trim().toLowerCase();
      this._applyFilter();
    };

    this.groupsContainer = new Div(this.objectsPanel);

    const pane2 = new Span(split, { style: "flex-grow: 1; overflow: hidden;" });
    const inspectTab = new TabWidget(pane2, { class: "inspector-tabs tabs-fill" });
    this.inspectPanel = new Div(null, { class: "inspector_panel_content" });
    inspectTab.addTab("Inspect", this.inspectPanel);
  }

  private _reset(): void {
    this._groups.clear();
    this.groupsContainer.html = "";
    this.inspectPanel.html = "";
    this._selectedGroup = null;
    this._selectedObject = null;
    this.inspectedObject = null;
    this._back = [];
    this._forward = [];
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
    g.label.text = this._filter ? `${typeLabel(g.type)} ${g.visibleCount}/${g.total}` : `${typeLabel(g.type)} ${g.total}`;
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
      this._selectedObject = object;
      item.element.classList.add("selected");
      this.inspectObject(object);
    };
    g.total++;
    const visible = this._matches(object);
    item.element.style.display = visible ? "" : "none";
    if (visible) g.visibleCount++;
    this._setGroupLabel(g);
  }

  private _fillItem(object: VulkanObject, item: Widget): void {
    item.html = "";
    new Span(item, { text: object.name, class: "object-item-name" });
    new Span(item, { text: ` ${object.summary(this.database)}`, class: "object-item-type" });
    item.element.title = `${object.type} ${object.id} (${object.handle})`;
    if (object.isInvalid) {
      item.element.classList.add("error");
      item.tooltip = object.invalidReason ?? "";
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

  private _matches(object: VulkanObject): boolean {
    if (!this._filter) return true;
    const f = this._filter;
    return String(object.id) === f
      || object.name.toLowerCase().includes(f)
      || object.type.toLowerCase().includes(f)
      || object.summary(this.database).toLowerCase().includes(f);
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
    this._imageView = null;
    if (object.type === "VkImage" || object.type === "VkImageView") {
      const grp = new collapsible(this.inspectPanel, { label: "Image", collapsed: false });
      this._imageView = new ImageView(grp.body, this.window, object);
    }

    const argsGrp = new collapsible(this.inspectPanel, { label: "Arguments", collapsed: false });
    const tree = new Div(argsGrp.body, { class: "args-tree" });
    renderArgs(tree, object.args, db, onLink);
  }

  // Shader code: one sub-section per SPIR-V payload with disassembly / GLSL / HLSL views.
  private _buildShaderSection(object: VulkanObject): void {
    this._shaderViews = new Map();
    if (!object.blobs.length) {
      const grp = new collapsible(this.inspectPanel, { label: "Shader Code", collapsed: false });
      new Div(grp.body, { text: object.type === "VkShaderModule" ? "No shader code recorded." : "No shader stages recorded.", class: "text-muted" });
      return;
    }
    object.blobs.forEach((blob, index) => {
      const grp = new collapsible(this.inspectPanel, { label: `Shader: ${blob.name} (${blob.size} bytes)`, collapsed: index > 0 });
      const bar = new Div(grp.body, { class: "shader-toolbar" });
      const view: ShaderView = { pre: new Widget("pre"), mode: "dis", data: null, buttons: {} };
      const modes: [ShaderTextMode, string][] = [["dis", "SPIR-V"], ["glsl", "GLSL"], ["hlsl", "HLSL"]];
      for (const [mode, label] of modes) {
        view.buttons[mode] = new Button(bar, { label, class: "btn btn-sm", callback: () => void this._showShader(index, mode) });
      }
      view.pre = new Widget("pre", grp.body, { text: "Loading...", class: "shader-text" });
      this._shaderViews.set(index, view);
      void this.window.send({ action: "RequestBlob", id: object.id, index });
    });
  }

  private async _showShader(index: number, mode: ShaderTextMode): Promise<void> {
    const view = this._shaderViews.get(index);
    if (!view || !view.data) return;
    view.mode = mode;
    for (const m of Object.keys(view.buttons) as ShaderTextMode[]) view.buttons[m]?.element.classList.toggle("active", m === mode);
    view.pre.text = "Converting...";
    const r = await window.inspector.shaderText(view.data, mode);
    if (view.mode === mode) view.pre.text = r.text;
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
    void this._showShader(index, view.mode);
  }
}
