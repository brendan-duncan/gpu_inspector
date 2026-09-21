// An acceleration structure in a tab of its own: what it was built from, drawn in the same preview
// the mesh view uses and with the same camera and shading controls (mesh_controls.ts).
//
// The Inspect panel shows a structure in a strip beside everything else about the object, which is
// the right size for "what is this" and the wrong size for looking at a scene. A top level of a
// real frame holds thousands of instances spread over a level; that wants the window, a fly camera
// and a list you can pick through — which is what this is.
//
// A top level is drawn as its instances placed in the world, a bottom level as its own geometry.
// Either may have nothing to draw, which is the usual state of a bottom level built before the
// capture began; the view says which and why rather than showing an empty box
// (acceleration_scene.ts, structureDrawing).
import { Button } from "./widget/button.js";
import { Div } from "./widget/div.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import { MeshControls } from "./mesh_controls.js";
import { MeshPreview } from "./mesh_preview.js";
import { instancePosition, isIdentity, type AccelerationInstance } from "./acceleration_structure.js";
import { structureDrawing, type StructureDrawing } from "./acceleration_scene.js";
export { STRUCTURE_TYPES, structuresOfCommand, type StructureReference } from "./acceleration_scene.js";
import type { CaptureData } from "./capture_data.js";
import type { StructureDatabase } from "./acceleration_scene.js";
import type { ObjectLookup, VulkanObject } from "./vulkan/vulkan_object.js";


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

const ROWS = 200;

function plural(n: number, one: string, many: string): string {
  return `${n.toLocaleString()} ${n === 1 ? one : many}`;
}

export class AccelerationView {
  readonly host: AccelerationViewHost;
  readonly root: Div;
  private _id: number;
  private _drawing: StructureDrawing | null = null;
  private _preview: MeshPreview | null = null;
  private _controls: MeshControls | null = null;
  private _status: Span | null = null;
  private _note: Div | null = null;
  private _table: Div | null = null;
  private _selected: number | null = null;

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
    this._selected = null;
    this._build();
  }

  dispose(): void {
    this._controls?.dispose();
    this._preview?.dispose();
  }

  debugState(): Record<string, unknown> {
    return {
      structure: this._id,
      shape: this._drawing?.shape ?? null,
      instances: this._drawing?.instances.length ?? 0,
      placed: this._drawing?.placed ?? 0,
      note: this._drawing?.note ?? "",
      preview: this._preview?.debugState() ?? null,
      controls: this._controls?.debugState() ?? null,
    };
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
    this._controls = new MeshControls(cameraBar, this._preview);
    const bottom = new Div(body, { class: "accel-view-bottom" });
    this._table = new Div(bottom, { class: "accel-view-table" });

    const drawing = structureDrawing(this.host.data, this.host.db, this._id);
    this._drawing = drawing;
    this._preview.setMesh(drawing.positions.length ? { positions: drawing.positions, kind: drawing.kind, clip: false } : null);
    // A scene of a whole level is nothing to orbit, so it opens the way you would walk it.
    if (drawing.instances.length > 8) this._preview.setCameraMode("fly");

    this._status.text = this._summary(object, drawing);
    this._note.text = drawing.note || this._caption(drawing);
    this._renderTable(drawing);
    // A bottom level has no instances to list; the preview has the room instead.
    bottom.element.style.display = drawing.instances.length ? "" : "none";
  }

  private _summary(object: VulkanObject | null, drawing: StructureDrawing): string {
    const kind = drawing.instances.length ? "top level" : "bottom level";
    const parts = [object ? `${object.type.replace(/^ID3D12Raytracing|^Vk/, "")} ${object.id}` : `structure ${this._id}`, kind];
    if (drawing.instances.length) parts.push(`${drawing.instances.length} instance${drawing.instances.length === 1 ? "" : "s"}`);
    else if (drawing.shape === "triangles") parts.push(plural(Math.floor(drawing.positions.length / 9), "triangle", "triangles"));
    // A box is its twelve edges, two endpoints each, three floats an endpoint.
    else if (drawing.shape === "aabbs") parts.push(plural(Math.floor(drawing.positions.length / 72), "box", "boxes"));
    return parts.join(" · ");
  }

  private _caption(drawing: StructureDrawing): string {
    if (drawing.instances.length) {
      return drawing.shape === "triangles"
        ? `${drawing.placed} of ${drawing.instances.length} instances drawn with the geometry their bottom level was built from.`
        : drawing.shape === "aabbs"
        ? `${drawing.placed} of ${drawing.instances.length} instances drawn with the bounding boxes their bottom level was built from: `
          + "a procedural bottom level has no triangles, and its boxes are what the traversal tests against."
        : "The bottom levels' geometry is not in this capture, so each instance is drawn as a box where its transform puts it.";
    }
    return drawing.shape === "aabbs"
      ? "The bounding boxes this procedural bottom level was built from, in its own space. Its real shape is whatever its intersection shader decides."
      : "The triangles this bottom level was built from, in its own space.";
  }

  /** A top level's instances, or a bottom level's geometries. */
  private _renderTable(drawing: StructureDrawing): void {
    const table = this._table;
    if (!table) return;
    table.removeAllChildren();
    if (!drawing.instances.length) return;
    const header = new Div(table, { class: "accel-view-row accel-view-header" });
    for (const text of ["#", "Bottom level", "Position", "Mask", "Custom index", "Hit group", "Flags"]) {
      new Span(header, { text, class: "accel-view-cell" });
    }
    const shown = drawing.instances.slice(0, ROWS);
    shown.forEach((i) => {
      const row = new Div(table, { class: `accel-view-row${this._selected === i.index ? " selected" : ""}` });
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
      row.element.onclick = () => {
        this._selected = i.index;
        this._renderTable(drawing);
        // Following an instance to the level it names is the one link a scene cannot draw.
        if (blas) this.show(blas.id);
      };
    });
    if (drawing.instances.length > shown.length) {
      new Div(table, { text: `... ${drawing.instances.length - shown.length} more instances`, class: "text-muted accel-view-row" });
    }
  }
}

/** Which structure an instance names, for a caller wanting to open it. */
export function instanceTarget(instance: AccelerationInstance): number | null {
  return instance.blas ?? null;
}

