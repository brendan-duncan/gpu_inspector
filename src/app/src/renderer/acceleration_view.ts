// An acceleration structure in a tab of its own: what it was built from, drawn in the same preview
// the mesh view uses and with the same controls (mesh_controls.ts).
//
// The Inspect panel shows a structure in a strip beside everything else about the object, which is
// the right size for "what is this" and the wrong size for looking at a scene. A top level of a
// real frame holds thousands of instances spread over a level; that wants the window, a fly camera
// and a way through the list — which is what this is.
//
// Under the preview, three tabs:
//
//   Tree       top level → instances → bottom level → geometries, with primitives, surface area and
//              memory rolled up (acceleration_tree.ts). A checkbox hides a row's geometry, a search
//              narrows the rows, and Boxes draws every instance's bounding box.
//   Instances  what each instance says: its bottom level, place, mask, custom index, hit group, flags.
//   Overlaps   the instances whose bounding boxes overlap, most first, and a heat colouring of the
//              scene by how many others each instance's box shares its space with.
//
// A top level is drawn as its instances placed in the world, a bottom level as its own geometry.
// Either may have nothing to draw, which is the usual state of a bottom level built before the
// capture began; the view says which and why rather than showing an empty box
// (acceleration_scene.ts, structureDrawing).
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import { TextInput } from "./widget/text_input.js";
import { MeshControls } from "./mesh_controls.js";
import { MeshPreview, type PreviewAttribute, type PreviewGroup, type PreviewHit, type PreviewOverlay } from "./mesh_preview.js";
import { instancePosition, isIdentity, type AccelerationInstance } from "./acceleration_structure.js";
import { structureDrawing, type StructureDrawing } from "./acceleration_scene.js";
import {
  boundsLines, heatColor, heatColors, instanceBounds, instanceOverlaps, matchingKeys, structureTree, walkTree,
  type Bounds, type OverlapReport, type StructureFacts, type TreeNode,
} from "./acceleration_tree.js";
export { STRUCTURE_TYPES, structuresOfCommand, type StructureReference } from "./acceleration_scene.js";
import type { CaptureData } from "./capture_data.js";
import type { StructureDatabase } from "./acceleration_scene.js";
import { isObject, num, type ObjectLookup, type VulkanObject } from "./vulkan/vulkan_object.js";


/** What the view needs of the capture tab it belongs to. */
export interface AccelerationViewHost {
  readonly data: CaptureData;
  /** The lookup an instance's reference is resolved through, which needs every structure by type. */
  readonly db: ObjectLookup & StructureDatabase;
  /** Every acceleration structure of the capture, in id order. */
  structures(): VulkanObject[];
  /** Shows the structure in the Inspect panel. */
  showObject(id: number): void;
  /** Selects a command in the capture's tab, for the build that made this structure. */
  selectCommand(index: number): void;
  /** The command that last built this structure, or null. */
  buildOf(structureId: number): number | null;
}

type BottomTab = "tree" | "instances" | "overlaps";

/** Rows of the tree or a table drawn at most; a search narrows the rest. */
const ROWS = 500;

function plural(n: number, one: string, many: string): string {
  return `${n.toLocaleString()} ${n === 1 ? one : many}`;
}

export function formatBytes(bytes: number | null): string {
  if (bytes === null) return "—";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

export function formatArea(area: number | null): string {
  if (area === null) return "—";
  if (area === 0) return "0";
  return Math.abs(area) >= 1e5 || Math.abs(area) < 1e-2 ? area.toExponential(2) : area.toPrecision(4).replace(/\.?0+$/, "");
}

/**
 * What the tree needs of a structure object: its name, what it costs in memory (the driver's size
 * for its last build, else what its Vulkan create info reserved) and how many primitives its build
 * holds.
 */
export function structureFacts(o: VulkanObject | null): StructureFacts | null {
  if (!o) return null;
  const build = isObject(o.updates.build) ? o.updates.build : null;
  const createInfo = o.args && isObject(o.args.pCreateInfo) ? o.args.pCreateInfo : null;
  const memory = build && num(build.resultSize) > 0 ? num(build.resultSize)
               : createInfo && num(createInfo.size) > 0 ? num(createInfo.size) : null;
  return { name: o.name, memory, primitives: build ? num(build.primitiveCount) : null };
}

export class AccelerationView {
  readonly host: AccelerationViewHost;
  readonly root: Div;
  private _id: number;
  private _drawing: StructureDrawing | null = null;
  private _tree: TreeNode | null = null;
  private _overlap: OverlapReport | null = null;
  private _bounds: (Bounds | null)[] = [];
  private _preview: MeshPreview | null = null;
  private _controls: MeshControls | null = null;
  private _status: Span | null = null;
  private _note: Div | null = null;
  private _tabs: Div | null = null;
  private _panel: Div | null = null;
  private _tab: BottomTab = "tree";
  /** The tree row (key) or the instance selected. */
  private _selectedKey: string | null = null;
  private _hidden = new Set<string>();
  private _expanded = new Set<string>(["root"]);
  private _search = "";
  private _showBoxes = false;
  private _heat = false;
  /** Which preview groups are the main mesh's and which the stand-in overlay's, as drawing group indexes. */
  private _mainLines = false;

  constructor(host: AccelerationViewHost, structureId: number) {
    this.host = host;
    this._id = structureId;
    this.root = new Div(null, { class: "accel-view" });
    this._build();
  }

  get label(): string {
    const o = this.host.db.getObject(this._id);
    return `Structure ${o?.name ?? this._id}`;
  }

  /** Points the tab at another structure. */
  show(structureId: number): void {
    this._id = structureId;
    this._selectedKey = null;
    this._hidden.clear();
    this._expanded = new Set(["root"]);
    this._search = "";
    this._build();
  }

  dispose(): void {
    this._controls?.dispose();
    this._preview?.dispose();
  }

  debugState(): Record<string, unknown> {
    const tree = this._tree;
    return {
      structure: this._id,
      shape: this._drawing?.shape ?? null,
      instances: this._drawing?.instances.length ?? 0,
      placed: this._drawing?.placed ?? 0,
      note: this._drawing?.note ?? "",
      tab: this._tab,
      tree: tree ? { primitives: tree.primitives, area: tree.area, memory: tree.memory, children: tree.children.length } : null,
      overlaps: this._overlap ? { total: this._overlap.total, unknown: this._overlap.unknown, top: this._overlap.pairs.slice(0, 5) } : null,
      hidden: [...this._hidden], selected: this._selectedKey, boxes: this._showBoxes, heat: this._heat,
      preview: this._preview?.debugState() ?? null,
      controls: this._controls?.debugState() ?? null,
    };
  }

  /** The UI tests' hands: what a click on a row or a checkbox would do. */
  debugAction(action: string, arg = ""): void {
    if (action === "tab") this._showTab(arg as BottomTab);
    else if (action === "hide") this._setHidden(arg, true);
    else if (action === "show") this._setHidden(arg, false);
    else if (action === "select") this._selectRow(arg);
    else if (action === "boxes") this._setBoxes(arg !== "off");
    else if (action === "heat") this._setHeat(arg !== "off");
    else if (action === "search") { this._search = arg; this._renderPanel(); }
    else if (action === "zoom") this._preview?.zoomToSelection();
  }

  // ---------------------------------------------------------------------------------------

  private _build(): void {
    this._controls?.dispose();
    this._preview?.dispose();
    this.root.removeAllChildren();

    const object = this.host.db.getObject(this._id);
    const structures = this.host.structures();
    const at = Math.max(0, structures.findIndex((o) => o.id === this._id));

    const bar = new Div(this.root, { class: "accel-view-bar" });
    if (structures.length > 1) {
      const pick = new Select(bar, {
        options: structures.map((o) => o.name),
        index: at,
        onChange: (_v: string, index?: number) => {
          const next = structures[index ?? 0];
          if (next && next.id !== this._id) this.show(next.id);
        },
      });
      pick.tooltip = `Structure ${at + 1} of the capture's ${structures.length}`;
    }
    new Button(bar, { label: "Show in Inspect", class: "btn btn-sm", tooltip: "Select the structure in the Inspect panel",
      callback: () => this.host.showObject(this._id) });
    const build = this.host.buildOf(this._id);
    new Button(bar, { label: "Go to Build", class: "btn btn-sm", disabled: build === null,
      tooltip: build === null ? "This capture holds no build of this structure" : "Select the build that made it, in the capture's tab",
      callback: () => { if (build !== null) this.host.selectCommand(build); } });
    new Button(bar, { label: "Reset View", class: "btn btn-sm", tooltip: "Frame it again (or double-click the preview)",
      callback: () => this._preview?.resetView() });
    const cameraBar = new Div(bar, { class: "accel-view-camera" });
    this._status = new Span(bar, { class: "text-muted" });

    this._note = new Div(this.root, { class: "accel-view-note text-muted" });

    const body = new Div(this.root, { class: "accel-view-body" });
    this._preview = new MeshPreview(body);
    this._controls = new MeshControls(cameraBar, this._preview, { bookmarkKey: `accel:${this._id}` });
    // The overlays follow the colouring: a heat chosen in the controls colours the boxes too.
    const follow = this._preview.onModeChange;
    this._preview.onModeChange = () => {
      follow?.();
      const heat = this._heatSelected();
      if (heat !== this._heat) {
        this._heat = heat;
        this._updateOverlays();
        if (this._tab === "overlaps") this._renderPanel();
      }
    };
    this._preview.describe = (hit) => this._describeHit(hit);
    this._preview.onPick = (hit) => this._picked(hit);
    const bottom = new Div(body, { class: "accel-view-bottom" });
    this._tabs = new Div(bottom, { class: "accel-view-tabs" });
    this._panel = new Div(bottom, { class: "accel-view-panel" });

    const drawing = structureDrawing(this.host.data, this.host.db, this._id);
    this._drawing = drawing;
    const facts = (id: number): StructureFacts | null => structureFacts(this.host.db.getObject(id));
    this._tree = drawing.groups.length || drawing.instances.length ? structureTree(drawing, this._id, facts) : null;
    this._bounds = drawing.instances.length ? instanceBounds(drawing) : [];
    this._overlap = drawing.instances.length ? instanceOverlaps(this._bounds) : null;
    this._heat = false;
    if (!drawing.instances.length && this._tab !== "tree") this._tab = "tree";

    this._setMesh(drawing);
    // A scene of a whole level is nothing to orbit, so it opens the way you would walk it.
    if (drawing.instances.length > 8) this._preview.setCameraMode("fly");

    this._status.text = this._summary(object, drawing);
    this._note.text = drawing.note || this._caption(drawing)
      + (drawing.fromCaptureStart
        ? " Built before the capture began: what it was built from was read back as the capture started, which is "
          + "what the build read for geometry that does not change, and not for a buffer the application has rewritten since."
        : "");
    this._renderTabs();
    this._renderPanel();
    bottom.element.style.display = this._tree ? "" : "none";
  }

  /** The drawing as the preview takes it: its triangles (or lines), groups, attributes and overlays. */
  private _setMesh(drawing: StructureDrawing): void {
    const preview = this._preview;
    if (!preview) return;
    const mainLines = !drawing.triangles.length;
    this._mainLines = mainLines;
    const positions = mainLines ? drawing.lines : drawing.triangles;
    if (!positions.length) {
      preview.setMesh(null);
      return;
    }
    const groups: PreviewGroup[] = [];
    drawing.groups.forEach((g, i) => { if (g.lines === mainLines) groups.push({ id: i, first: g.first, count: g.count }); });
    preview.setMesh({
      positions, kind: mainLines ? "lines" : "triangles", clip: false, groups,
      attributes: this._attributes(drawing), overlays: this._overlays(),
    });
  }

  /** Position, and for a top level the overlap heat, as the preview's colourings. */
  private _attributes(drawing: StructureDrawing): PreviewAttribute[] {
    const positions = this._mainLines ? drawing.lines : drawing.triangles;
    const out: PreviewAttribute[] = [{ name: "Position", components: 3, isPosition: true, read: (v) => positions.subarray(v * 3, v * 3 + 3) }];
    if (this._overlap) {
      const heat = heatColors(drawing, this._overlap.counts);
      const colors = this._mainLines ? heat.lines : heat.triangles;
      out.push({ name: "Overlap heat", components: 3, isColor: true, read: (v) => colors.subarray(v * 3, v * 3 + 3) });
    }
    return out;
  }

  private _heatSelected(): boolean {
    const preview = this._preview;
    return !!preview && preview.colorSource >= 0 && preview.colorSources[preview.colorSource] === "Overlap heat";
  }

  /** The stand-ins and procedural boxes beside triangles, and the instance boxes when asked for. */
  private _overlays(): PreviewOverlay[] {
    const drawing = this._drawing;
    if (!drawing) return [];
    const out: PreviewOverlay[] = [];
    const heat = this._heat && this._overlap ? heatColors(drawing, this._overlap.counts) : null;
    if (!this._mainLines && drawing.lines.length) {
      const groups: PreviewGroup[] = [];
      drawing.groups.forEach((g, i) => { if (g.lines) groups.push({ id: i, first: g.first, count: g.count }); });
      out.push({ positions: drawing.lines, color: [0.55, 0.6, 0.7], colors: heat?.lines, groups, pickable: true });
    }
    if (this._showBoxes && this._bounds.length) {
      const hiddenGroups = this._hiddenGroups();
      const shown = this._bounds.map((b, i) => {
        const groups = drawing.groups.map((g, k) => (g.instance === i ? k : -1)).filter((k) => k >= 0);
        return groups.length && groups.every((k) => hiddenGroups.has(k)) ? null : b;
      });
      const boxes = boundsLines(shown);
      let colors: Float32Array | undefined;
      if (heat && this._overlap) {
        const most = Math.max(1, ...this._overlap.counts);
        colors = new Float32Array(boxes.positions.length);
        boxes.owners.forEach((instance, n) => {
          const c = heatColor((this._overlap!.counts[instance] ?? 0) / most);
          for (let v = n * 24; v < (n + 1) * 24; v++) colors!.set(c, v * 3);
        });
      }
      out.push({ positions: boxes.positions, color: [0.95, 0.8, 0.3], colors });
    }
    return out;
  }

  private _updateOverlays(): void {
    this._preview?.setOverlays(this._overlays());
  }

  // ---------------------------------------------------------------------------------------
  // Visibility and selection

  /** The drawing's groups hidden by the tree: every group of every row unchecked, or under one. */
  private _hiddenGroups(): Set<number> {
    const out = new Set<number>();
    if (!this._tree) return out;
    walkTree(this._tree, (n) => { if (this._hidden.has(n.key)) for (const g of n.groups) out.add(g); });
    return out;
  }

  private _setHidden(key: string, hidden: boolean): void {
    if (hidden) this._hidden.add(key);
    else this._hidden.delete(key);
    this._preview?.setHidden(this._hiddenGroups());
    if (this._showBoxes) this._updateOverlays();
    this._renderPanel();
  }

  private _setBoxes(show: boolean): void {
    this._showBoxes = show;
    this._updateOverlays();
    this._renderPanel();
  }

  private _setHeat(on: boolean): void {
    const preview = this._preview;
    if (!preview) return;
    const index = preview.colorSources.indexOf("Overlap heat");
    preview.setColorSource(on ? index : -1);
    this._heat = on && index >= 0;
    this._updateOverlays();
    this._renderPanel();
  }

  private _node(key: string): TreeNode | null {
    let found: TreeNode | null = null;
    if (this._tree) walkTree(this._tree, (n) => { if (n.key === key) found = n; });
    return found;
  }

  /** Selects a tree row: its geometry lit up in the preview, ready for Zoom to Selected. */
  private _selectRow(key: string): void {
    const node = this._node(key);
    this._selectedKey = node ? key : null;
    // Every ancestor opens, so a row selected from the preview can be seen.
    if (node) for (const k of this._ancestors(key)) this._expanded.add(k);
    this._preview?.select(null, node?.groups ?? []);
    this._renderPanel();
  }

  private _ancestors(key: string): string[] {
    const out: string[] = [];
    const visit = (n: TreeNode, path: string[]): boolean => {
      if (n.key === key) {
        out.push(...path);
        return true;
      }
      return n.children.some((c) => visit(c, [...path, n.key]));
    };
    if (this._tree) visit(this._tree, []);
    return out;
  }

  /** The drawing group a preview hit is in: its group id is the group's index. */
  private _groupOf(hit: PreviewHit): number | null {
    return hit.group;
  }

  private _describeHit(hit: PreviewHit): string {
    const drawing = this._drawing;
    const index = this._groupOf(hit);
    const g = drawing && index !== null ? drawing.groups[index] : null;
    if (!drawing || !g) return `primitive ${hit.primitive}`;
    const within = Math.floor((hit.vertex - g.first) / (g.lines ? 24 : 3));
    const primitive = g.geometry < 0 ? "stand-in box" : g.lines ? `box ${within}` : `triangle ${within}`;
    if (g.instance < 0) return `Geometry ${g.geometry} · ${primitive}`;
    const instance = drawing.instances[g.instance];
    const blas = instance?.blas !== undefined ? this.host.db.getObject(instance.blas) : null;
    return [`Instance ${instance?.index ?? g.instance}`, blas?.name ?? "", g.geometry >= 0 ? `geometry ${g.geometry}` : "", primitive]
      .filter(Boolean).join(" · ");
  }

  /** A click in the preview: the instance (or geometry) it hit, selected in the tree and lit whole. */
  private _picked(hit: PreviewHit | null): void {
    const drawing = this._drawing;
    const index = hit ? this._groupOf(hit) : null;
    const g = drawing && index !== null ? drawing.groups[index] : null;
    if (!drawing || !g || !this._tree) {
      this._selectedKey = null;
      this._renderPanel();
      return;
    }
    const key = g.instance >= 0 ? `i${g.instance}` : `g${index}`;
    const node = this._node(key);
    this._selectedKey = node ? key : null;
    if (node) for (const k of this._ancestors(key)) this._expanded.add(k);
    this._preview?.select(hit, node?.groups ?? []);
    if (this._tab !== "tree" && this._tab !== "instances") this._tab = "tree";
    this._renderTabs();
    this._renderPanel();
    this._panel?.element.querySelector(".selected")?.scrollIntoView({ block: "nearest" });
  }

  // ---------------------------------------------------------------------------------------
  // The tabs under the preview

  private _renderTabs(): void {
    const tabs = this._tabs;
    const drawing = this._drawing;
    if (!tabs || !drawing) return;
    tabs.removeAllChildren();
    const list: { tab: BottomTab; label: string }[] = [{ tab: "tree", label: "Tree" }];
    if (drawing.instances.length) {
      list.push({ tab: "instances", label: `Instances (${drawing.instances.length.toLocaleString()})` });
      list.push({ tab: "overlaps", label: `Overlaps (${(this._overlap?.total ?? 0).toLocaleString()})` });
    }
    for (const t of list) {
      const b = new Button(tabs, { label: t.label, class: `btn btn-sm accel-view-tab${this._tab === t.tab ? " active" : ""}`,
        callback: () => this._showTab(t.tab) });
      b.element.dataset.tab = t.tab;
    }
  }

  private _showTab(tab: BottomTab): void {
    this._tab = tab;
    this._renderTabs();
    this._renderPanel();
  }

  private _renderPanel(): void {
    const panel = this._panel;
    const drawing = this._drawing;
    if (!panel || !drawing) return;
    panel.removeAllChildren();
    if (this._tab === "instances") this._renderInstances(panel, drawing);
    else if (this._tab === "overlaps") this._renderOverlaps(panel, drawing);
    else this._renderTree(panel);
  }

  private _renderTree(panel: Div): void {
    const tree = this._tree;
    if (!tree) return;
    const tools = new Div(panel, { class: "accel-view-tools" });
    const search = new TextInput(tools, { value: this._search, placeholder: "Search by name", class: "accel-view-search" });
    search.element.oninput = () => {
      this._search = (search.element as HTMLInputElement).value;
      this._renderTreeRows(rows);
    };
    if (tree.kind === "tlas") {
      new Checkbox(tools, { label: "Boxes", checked: this._showBoxes, tooltip: "Draw every instance's world-space bounding box",
        onChange: (checked: boolean) => this._setBoxes(checked) });
    }
    new Button(tools, { label: "Show All", class: "btn btn-sm", disabled: !this._hidden.size, tooltip: "Check every row again",
      callback: () => { this._hidden.clear(); this._preview?.setHidden([]); if (this._showBoxes) this._updateOverlays(); this._renderPanel(); } });
    new Button(tools, { label: "Zoom to Selected", class: "btn btn-sm", disabled: !this._selectedKey, tooltip: "Frame the selected row's geometry (F in the view)",
      callback: () => this._preview?.zoomToSelection() });
    const header = new Div(panel, { class: "accel-tree-row accel-view-header" });
    for (const text of ["Name", "Primitives", "Surface area", "Memory"]) new Span(header, { text, class: "accel-view-cell" });
    const rows = new Div(panel, { class: "accel-tree-rows" });
    this._renderTreeRows(rows);
  }

  private _renderTreeRows(rows: Div): void {
    const tree = this._tree;
    if (!tree) return;
    rows.removeAllChildren();
    const keep = matchingKeys(tree, this._search);
    const searching = !!this._search.trim();
    let drawn = 0;
    let skipped = 0;
    const visit = (node: TreeNode, depth: number, hiddenAbove: boolean): void => {
      if (!keep.has(node.key)) return;
      if (drawn >= ROWS) {
        skipped++;
        return;
      }
      drawn++;
      const hidden = this._hidden.has(node.key);
      const open = searching || this._expanded.has(node.key);
      const row = new Div(rows, { class: `accel-tree-row${this._selectedKey === node.key ? " selected" : ""}${hidden || hiddenAbove ? " hidden" : ""}` });
      row.element.dataset.key = node.key;
      const name = new Span(row, { class: "accel-view-cell accel-tree-name" });
      name.element.style.paddingLeft = `${depth * 14}px`;
      const twist = document.createElement("span");
      twist.className = "accel-tree-twist";
      twist.textContent = node.children.length ? (open ? "▾" : "▸") : "";
      twist.onclick = (e) => {
        e.stopPropagation();
        if (this._expanded.has(node.key)) this._expanded.delete(node.key);
        else this._expanded.add(node.key);
        this._renderTreeRows(rows);
      };
      name.element.appendChild(twist);
      const box = document.createElement("input");
      box.type = "checkbox";
      box.checked = !hidden;
      box.title = hidden ? "Hidden: check to draw it again" : "Uncheck to hide it in the view";
      box.onclick = (e) => e.stopPropagation();
      box.onchange = () => this._setHidden(node.key, !box.checked);
      name.element.appendChild(box);
      const label = document.createElement("span");
      label.textContent = node.label;
      if (node.note) label.title = node.note;
      name.element.appendChild(label);
      // A bottom level placed by an instance opens in this tab, the way following a reference should.
      if (node.kind === "blas" && node.objectId !== undefined && node.objectId !== this._id) {
        const go = document.createElement("a");
        go.className = "accel-tree-open";
        go.textContent = "open";
        go.title = "Show this bottom level on its own";
        go.onclick = (e) => { e.stopPropagation(); this.show(node.objectId!); };
        name.element.appendChild(go);
      }
      new Span(row, { text: node.primitives ? node.primitives.toLocaleString() : "—", class: "accel-view-cell" });
      new Span(row, { text: formatArea(node.area), class: "accel-view-cell" });
      new Span(row, { text: formatBytes(node.memory), class: "accel-view-cell" });
      row.element.onclick = () => this._selectRow(node.key);
      row.element.ondblclick = () => { this._selectRow(node.key); this._preview?.zoomToSelection(); };
      if (open) for (const c of node.children) visit(c, depth + 1, hiddenAbove || hidden);
    };
    visit(tree, 0, false);
    if (skipped) new Div(rows, { text: `... ${skipped.toLocaleString()} more rows: search to narrow them`, class: "text-muted accel-tree-row" });
    if (searching && !drawn) new Div(rows, { text: "Nothing matches", class: "text-muted accel-tree-row" });
  }

  /** A top level's instances as the build gave them. */
  private _renderInstances(panel: Div, drawing: StructureDrawing): void {
    const table = new Div(panel, { class: "accel-view-table" });
    const header = new Div(table, { class: "accel-view-row accel-view-header" });
    for (const text of ["#", "Bottom level", "Position", "Mask", "Custom index", "Hit group", "Flags"]) {
      new Span(header, { text, class: "accel-view-cell" });
    }
    const shown = drawing.instances.slice(0, ROWS);
    shown.forEach((i, at) => {
      const key = `i${at}`;
      const row = new Div(table, { class: `accel-view-row${this._selectedKey === key ? " selected" : ""}` });
      const blas = i.blas !== undefined ? this.host.db.getObject(i.blas) : null;
      const cells = [
        String(i.index),
        blas ? blas.name : `at ${i.reference}`,
        isIdentity(i.transform) ? "origin" : instancePosition(i).map((v) => v.toFixed(2)).join(", "),
        `0x${i.mask.toString(16).toUpperCase()}`,
        String(i.customIndex),
        `+${i.bindingTableOffset}`,
        i.flagNames.join(" | ") || "—",
      ];
      for (const text of cells) new Span(row, { text, class: "accel-view-cell" });
      row.element.onclick = () => this._selectRow(key);
      // Following an instance to the level it names is the one link a scene cannot draw.
      row.element.ondblclick = () => { if (blas) this.show(blas.id); };
      row.element.title = blas ? "Click to select it in the view, double-click to open its bottom level" : "";
    });
    if (drawing.instances.length > shown.length) {
      new Div(table, { text: `... ${drawing.instances.length - shown.length} more instances`, class: "text-muted accel-view-row" });
    }
  }

  /** The overlapping instance pairs, the most overlapped first, and the heat colouring's switch. */
  private _renderOverlaps(panel: Div, drawing: StructureDrawing): void {
    const report = this._overlap;
    if (!report) return;
    const tools = new Div(panel, { class: "accel-view-tools" });
    new Checkbox(tools, {
      label: "Heatmap", checked: this._heat,
      tooltip: "Colour each instance by how many other instances' bounding boxes overlap its own: blue alone, red the most crowded",
      onChange: (checked: boolean) => this._setHeat(checked),
    });
    const known = drawing.instances.length - report.unknown;
    const parts = [`${plural(report.total, "overlapping pair", "overlapping pairs")} among ${plural(known, "instance", "instances")}`];
    if (report.unknown) parts.push(`${plural(report.unknown, "instance", "instances")} left out: their bottom level's geometry is not in the capture`);
    if (report.total > report.pairs.length) parts.push(`the ${report.pairs.length.toLocaleString()} most overlapped shown`);
    new Span(tools, { text: parts.join("; "), class: "text-muted" });
    new Div(panel, {
      class: "text-muted accel-view-explain",
      text: "A ray through a region where several instances' boxes overlap descends into every one of them. "
        + "An instance buried in another, or many instances stacked in one place, is where traversal gets expensive.",
    });
    const table = new Div(panel, { class: "accel-view-table" });
    const header = new Div(table, { class: "accel-overlap-row accel-view-header" });
    for (const text of ["Instance", "Instance", "Overlap", "Shared volume"]) new Span(header, { text, class: "accel-view-cell" });
    const name = (at: number): string => {
      const i = drawing.instances[at];
      const blas = i?.blas !== undefined ? this.host.db.getObject(i.blas) : null;
      return `${i?.index ?? at}${blas ? ` · ${blas.name}` : ""}`;
    };
    report.pairs.slice(0, ROWS).forEach((p) => {
      const key = `o${p.a}:${p.b}`;
      const row = new Div(table, { class: `accel-overlap-row${this._selectedKey === key ? " selected" : ""}` });
      new Span(row, { text: name(p.a), class: "accel-view-cell" });
      new Span(row, { text: name(p.b), class: "accel-view-cell" });
      new Span(row, { text: `${(p.fraction * 100).toFixed(p.fraction >= 0.995 ? 0 : 1)}% of the smaller`, class: "accel-view-cell" });
      new Span(row, { text: formatArea(p.volume), class: "accel-view-cell" });
      row.element.title = "Click to select both, double-click to frame them";
      const select = (): void => {
        this._selectedKey = key;
        const groups = [...(this._node(`i${p.a}`)?.groups ?? []), ...(this._node(`i${p.b}`)?.groups ?? [])];
        this._preview?.select(null, groups);
        this._renderPanel();
      };
      row.element.onclick = select;
      row.element.ondblclick = () => { select(); this._preview?.zoomToSelection(); };
    });
  }

  // ---------------------------------------------------------------------------------------

  private _summary(object: VulkanObject | null, drawing: StructureDrawing): string {
    const kind = drawing.instances.length ? "top level" : "bottom level";
    const parts = [object ? `${object.type.replace(/^ID3D12Raytracing|^Vk/, "")} ${object.id}` : `structure ${this._id}`, kind];
    if (drawing.instances.length) parts.push(plural(drawing.instances.length, "instance", "instances"));
    else if (drawing.shape === "triangles") parts.push(plural(Math.floor(drawing.triangles.length / 9), "triangle", "triangles"));
    // A box is its twelve edges, two endpoints each, three floats an endpoint.
    else if (drawing.shape === "aabbs") parts.push(plural(Math.floor(drawing.lines.length / 72), "box", "boxes"));
    if (this._tree?.memory) parts.push(formatBytes(this._tree.memory));
    return parts.join(" · ");
  }

  private _caption(drawing: StructureDrawing): string {
    if (drawing.instances.length) {
      return drawing.shape === "triangles"
        ? `${drawing.placed} of ${drawing.instances.length} instances drawn with the geometry their bottom level was built from`
          + (drawing.placed < drawing.instances.length ? "; the rest as boxes where their transforms put them." : ".")
        : drawing.shape === "aabbs"
        ? `${drawing.placed} of ${drawing.instances.length} instances drawn with the bounding boxes their bottom level was built from: `
          + "a procedural bottom level has no triangles, and its boxes are what the traversal tests against."
        : "The bottom levels' geometry is not in this capture, so each instance is drawn as a box where its transform puts it.";
    }
    return drawing.shape === "aabbs"
      ? "The bounding boxes this procedural bottom level was built from, in its own space. Its real shape is whatever its intersection shader decides."
      : "The triangles this bottom level was built from, in its own space.";
  }
}

/** Which structure an instance names, for a caller wanting to open it. */
export function instanceTarget(instance: AccelerationInstance): number | null {
  return instance.blas ?? null;
}
