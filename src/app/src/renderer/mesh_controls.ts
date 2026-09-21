// The controls over a MeshPreview: how the eye moves and what the primitives are drawn as.
//
// One widget so the mesh view and the acceleration structure view read the same. Both show the same
// geometry in the same preview, and a control that means one thing in one of them and another in
// the other is worse than no control at all.
import { Div } from "./widget/div.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import { CAMERA_MODES, SHADE_MODES, type CameraMode, type MeshPreview, type ShadeMode } from "./mesh_preview.js";

/**
 * A row of camera and shading controls driving `preview`, which it follows: resetting the view or
 * loading a mesh with no faces in it changes what the controls may offer, and they update in place.
 */
export class MeshControls {
  readonly root: Div;
  private readonly _preview: MeshPreview;
  private readonly _camera: Select;
  private readonly _shade: Select;
  private readonly _hint: Span;

  constructor(parent: Widget, preview: MeshPreview) {
    this._preview = preview;
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

    this._hint = new Span(this.root, { class: "text-muted font-sm mesh-controls-hint" });
    preview.onModeChange = () => this.refresh();
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
    // geometry and every mesh drawn as lines: say so rather than offering a mode that does nothing.
    this._shade.select.element.disabled = !preview.canFill;
    this._shade.element.title = preview.canFill
      ? (SHADE_MODES.find((m) => m.value === preview.shadeMode)?.tooltip ?? "")
      : "These primitives have no faces to fill, so they are drawn as a wireframe whatever this says";
    this._camera.element.title = CAMERA_MODES.find((m) => m.value === preview.cameraMode)?.tooltip ?? "";
    this._hint.text = preview.cameraMode === "fly"
      ? "drag to look, WASD to walk, Q/E up and down, shift to hurry, wheel for speed"
      : "drag to turn, shift or middle drag to slide, wheel to zoom";
  }

  dispose(): void {
    if (this._preview.onModeChange === null) return;
    this._preview.onModeChange = null;
  }

  debugState(): Record<string, unknown> {
    return { camera: this._preview.cameraMode, shading: this._preview.shadeMode, canFill: this._preview.canFill };
  }
}

export type { CameraMode, ShadeMode };
