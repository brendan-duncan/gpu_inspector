// The controls over a MeshPreview: how the eye moves, what the primitives are drawn as and colored
// by, where their normals come from, and the camera's bookmarks.
//
// One widget so the mesh view and the acceleration structure view read the same. Both show the same
// geometry in the same preview, and a control that means one thing in one of them and another in
// the other is worse than no control at all.
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import { CAMERA_MODES, SHADE_MODES, type CameraMode, type CameraState, type MeshPreview, type ShadeMode } from "./mesh_preview.js";

/** Nine slots, as Ctrl+1 to Ctrl+9 set them. */
const SLOTS = 9;

/**
 * Bookmarks outlive the tab: reopening a structure, or stepping to another draw of the same pass,
 * finds the places kept for it. Keyed by what the caller says the view is of; not saved to disk.
 */
const BOOKMARKS = new Map<string, (CameraState | null)[]>();

export interface MeshControlsOptions {
  /** What the bookmarks belong to: the same key finds the same bookmarks. */
  bookmarkKey?: string;
}

/**
 * A row of controls driving `preview`, which it follows: resetting the view or loading a mesh with
 * no faces in it changes what the controls may offer, and they update in place.
 */
export class MeshControls {
  readonly root: Div;
  private readonly _preview: MeshPreview;
  private readonly _camera: Select;
  private readonly _shade: Select;
  private readonly _color: Select;
  private readonly _normals: Select;
  private readonly _showNormals: Checkbox;
  private readonly _zoom: Button;
  private readonly _bookmarks: Select;
  private readonly _hint: Span;
  private readonly _key: string;
  /** What the color and normal selects list, to rebuild them only when it changes. */
  private _listed = "";

  constructor(parent: Widget, preview: MeshPreview, options: MeshControlsOptions = {}) {
    this._preview = preview;
    this._key = options.bookmarkKey ?? "";
    this.root = new Div(parent, { class: "mesh-controls" });

    new Span(this.root, { text: "Camera", class: "text-muted font-sm" });
    this._camera = new Select(this.root, {
      options: CAMERA_MODES.map((m) => m.label),
      index: CAMERA_MODES.findIndex((m) => m.value === preview.cameraMode),
      onChange: (_v: string, index?: number) => {
        const mode = CAMERA_MODES[index ?? 0];
        if (mode) preview.setCameraMode(mode.value);
      },
    });

    new Span(this.root, { text: "Shading", class: "text-muted font-sm" });
    this._shade = new Select(this.root, {
      options: SHADE_MODES.map((m) => m.label),
      index: SHADE_MODES.findIndex((m) => m.value === preview.shadeMode),
      onChange: (_v: string, index?: number) => {
        const mode = SHADE_MODES[index ?? 0];
        if (mode) preview.setShadeMode(mode.value);
      },
    });

    new Span(this.root, { text: "Color", class: "text-muted font-sm" });
    this._color = new Select(this.root, {
      options: ["Default"],
      onChange: (_v: string, index?: number) => preview.setColorSource((index ?? 0) - 1),
    });
    this._color.tooltip = "Color the vertices by an attribute: a color as it is, anything else stretched over its own range";

    new Span(this.root, { text: "Normals", class: "text-muted font-sm" });
    this._normals = new Select(this.root, {
      options: ["Geometry"],
      onChange: (_v: string, index?: number) => preview.setNormalSource((index ?? 0) - 1),
    });
    this._normals.tooltip = "Where flat and smooth shading and the normals overlay take their normals: the geometry's own, or an attribute";
    this._showNormals = new Checkbox(this.root, {
      label: "Show", checked: preview.showNormals,
      tooltip: "Draw the normals as short lines: each vertex's with an attribute chosen, each face's without",
      onChange: (checked: boolean) => preview.setShowNormals(checked),
    });

    this._zoom = new Button(this.root, {
      label: "Zoom to Selected", class: "btn btn-sm", tooltip: "Frame what is selected (F)",
      callback: () => preview.zoomToSelection(),
    });

    this._bookmarks = new Select(this.root, {
      options: ["Bookmarks"],
      onChange: (_v: string, index?: number) => {
        const choice = index ?? 0;
        this._bookmarks.select.element.selectedIndex = 0;
        if (choice === 1) this._save(this._freeSlot());
        else if (choice > 1) this._recall(this._savedSlots()[choice - 2]);
      },
    });
    this._bookmarks.tooltip = "Keep the camera where it is, or go back to where it was kept (Ctrl+1-9 keeps, 1-9 goes back)";

    this._hint = new Span(this.root, { class: "text-muted font-sm mesh-controls-hint" });
    preview.onModeChange = () => this.refresh();
    preview.onBookmarkKey = (slot, save) => (save ? this._save(slot) : this._recall(slot));
    this.refresh();
  }

  /** Reads the preview's state back into the controls, and says what the mouse does now. */
  refresh(): void {
    const preview = this._preview;
    const camera = CAMERA_MODES.findIndex((m) => m.value === preview.cameraMode);
    if (camera >= 0) this._camera.select.element.selectedIndex = camera;
    const shade = SHADE_MODES.findIndex((m) => m.value === preview.shadeMode);
    if (shade >= 0) this._shade.select.element.selectedIndex = shade;
    // Nothing to fill on a line or point list, which is every acceleration structure of procedural
    // geometry and every mesh drawn as lines: say so rather than offering modes that do nothing.
    const options = this._shade.select.element.options;
    SHADE_MODES.forEach((m, i) => { if (options[i]) options[i].disabled = m.fills && !preview.canFill; });
    this._shade.element.title = !preview.canFill && SHADE_MODES[shade]?.fills
      ? "These primitives have no faces to fill, so they are drawn as a wireframe whatever this says"
      : (SHADE_MODES[shade]?.tooltip ?? "");
    this._camera.element.title = CAMERA_MODES.find((m) => m.value === preview.cameraMode)?.tooltip ?? "";

    const listed = JSON.stringify([preview.colorSources, preview.normalSources]);
    if (listed !== this._listed) {
      this._listed = listed;
      this._fill(this._color, ["Default", ...preview.colorSources]);
      this._fill(this._normals, ["Geometry", ...preview.normalSources]);
    }
    this._color.select.element.selectedIndex = preview.colorSource + 1;
    this._normals.select.element.selectedIndex = preview.normalSource + 1;
    this._color.select.element.disabled = !preview.colorSources.length;
    this._normals.select.element.disabled = !preview.normalSources.length;
    this._showNormals.checked = preview.showNormals;
    this._zoom.disabled = !preview.hasSelection;
    this._fillBookmarks();

    this._hint.text = preview.cameraMode === "fly"
      ? "drag to look, WASD to walk, Q/E up and down, shift to hurry, wheel for speed"
      : "drag to turn, shift or middle drag to slide, wheel to zoom";
  }

  dispose(): void {
    if (this._preview.onModeChange === null) return;
    this._preview.onModeChange = null;
    this._preview.onBookmarkKey = null;
  }

  debugState(): Record<string, unknown> {
    return {
      camera: this._preview.cameraMode, shading: this._preview.shadeMode, canFill: this._preview.canFill,
      colors: this._preview.colorSources, normals: this._preview.normalSources,
      bookmarks: this._slots().map((s, i) => (s ? i + 1 : 0)).filter((n) => n > 0),
    };
  }

  // ---------------------------------------------------------------------------------------

  private _fill(select: Select, labels: string[]): void {
    const element = select.select.element;
    element.innerHTML = "";
    for (const label of labels) {
      const option = document.createElement("option");
      option.textContent = label;
      element.appendChild(option);
    }
  }

  private _slots(): (CameraState | null)[] {
    let slots = BOOKMARKS.get(this._key);
    if (!slots) {
      slots = new Array<CameraState | null>(SLOTS).fill(null);
      BOOKMARKS.set(this._key, slots);
    }
    return slots;
  }

  private _savedSlots(): number[] {
    return this._slots().map((s, i) => (s ? i + 1 : 0)).filter((n) => n > 0);
  }

  private _freeSlot(): number {
    const free = this._slots().findIndex((s) => !s);
    return free >= 0 ? free + 1 : SLOTS;
  }

  private _save(slot: number): void {
    if (slot < 1 || slot > SLOTS) return;
    this._slots()[slot - 1] = this._preview.camera;
    this._fillBookmarks();
    this._hint.text = `Camera kept as bookmark ${slot}: press ${slot} in the view to come back`;
  }

  private _recall(slot: number | undefined): void {
    const state = slot ? this._slots()[slot - 1] : null;
    if (state) this._preview.camera = state;
  }

  private _fillBookmarks(): void {
    const saved = this._savedSlots();
    this._fill(this._bookmarks, ["Bookmarks", "Keep this view", ...saved.map((n) => `Go to bookmark ${n}`)]);
    this._bookmarks.select.element.selectedIndex = 0;
  }
}

export type { CameraMode, ShadeMode };
