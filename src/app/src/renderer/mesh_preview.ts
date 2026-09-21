// The 3D preview the mesh view and the acceleration structure view draw with: a set of primitives,
// turned with the mouse. VS In is drawn in the space its positions are in; VS Out in normalized
// device coordinates (clip space divided by w, y up the way the render target shows it) inside the
// outline of Vulkan's view volume, so what falls outside the volume is plain to see. WebGL2, so
// meshes of a million vertices still turn.
//
// Two camera modes, after RenderDoc's mesh viewer:
//
//   Arcball  the eye orbits a point: drag to turn, middle or right drag (or shift-drag) to slide
//            that point across the screen, wheel to come closer. What you want for one object.
//   Fly      the eye is where you are: drag to look, WASD to walk, Q and E to rise and fall, shift
//            to hurry, wheel to change how fast. What you want inside a scene — a top level
//            acceleration structure of a whole level is nothing to orbit.
//
// Ctrl+1 to Ctrl+9 keep the camera as a bookmark and 1 to 9 go back to it (mesh_controls.ts keeps them).
//
// The shading modes, after Nsight Graphics' geometry viewer. A wireframe says where every primitive
// is and hides nothing behind another; solid says what the surface is; flat says which way each face
// points, which is what you look at when the normals or the winding are suspect; smooth says what
// the lighting will see. Flat and smooth take their normals from an attribute when one is chosen,
// and otherwise from the geometry itself: flat from the derivatives of the view-space position (the
// face's own normal by construction), smooth from the faces around each position, averaged.
//
// Any attribute can colour the vertices instead of the one flat colour, and a caller can add its own
// colourings (the acceleration structure view's overlap heat). Clicking a primitive selects it and
// says which it is; hovering names it without selecting. A caller that groups its vertices (an
// acceleration structure's instances and geometries) can hide groups and select them whole.
import { Div } from "./widget/div.js";
import type { Widget } from "./widget/widget.js";
import type { PrimitiveKind } from "./mesh_output.js";

/** One attribute of every vertex of a mesh, read on demand so a mesh with many costs nothing until one is shown. */
export interface PreviewAttribute {
  name: string;
  /** How many components each vertex has (1 to 4). */
  components: number;
  /** The attribute of one vertex of the list, or null when it has none. */
  read: (vertex: number) => ArrayLike<number> | null;
  /** The values already are colours, 0 to 1 (a heat), rather than something to map into them. */
  isColor?: boolean;
  /** The positions themselves: a colouring, never a normal. */
  isPosition?: boolean;
}

/** A run of the list's vertices that belong together: what can be hidden, and selected, as one. */
export interface PreviewGroup {
  id: number;
  first: number;
  count: number;
}

/** Lines drawn over the mesh with the same camera: boxes around things, stand-ins, anything else. */
export interface PreviewOverlay {
  /** Line pairs, x y z each. */
  positions: Float32Array;
  color: [number, number, number];
  /** A colour per vertex instead of `color`. */
  colors?: Float32Array;
  /** Groups of the overlay's vertices, hidden and selected with the mesh's groups of the same id. */
  groups?: PreviewGroup[];
  /** Clicks and hovers find these lines too. */
  pickable?: boolean;
}

export interface PreviewMesh {
  /** One position per vertex, as a list of primitives: x y z, or x y z w with `clip`. */
  positions: Float32Array;
  kind: PrimitiveKind;
  /** Clip-space positions, drawn divided by w with the view volume outlined. */
  clip: boolean;
  /** What the vertices can be coloured by, and what their normals can come from. */
  attributes?: PreviewAttribute[];
  groups?: PreviewGroup[];
  overlays?: PreviewOverlay[];
}

/** What a click or a hover landed on. */
export interface PreviewHit {
  /** An overlay's line (its index in the mesh's overlays), or -1 for the mesh's own primitives. */
  overlay: number;
  /** The primitive's index in its list: triangles, lines or points. */
  primitive: number;
  /** Its first vertex, an index into the list the mesh (or overlay) was given. */
  vertex: number;
  /** The group it belongs to, or null. */
  group: number | null;
  point: [number, number, number];
}

/** How the eye moves. */
export type CameraMode = "arcball" | "fly";

/** What the primitives are drawn as. Only triangles can be filled; lines and points stay what they are. */
export type ShadeMode = "wireframe" | "solid" | "wire-solid" | "flat" | "smooth" | "points";

export const SHADE_MODES: { value: ShadeMode; label: string; tooltip: string; fills: boolean }[] = [
  { value: "wireframe", label: "Wireframe", fills: false, tooltip: "Every primitive's edges, with nothing hidden behind anything else" },
  { value: "solid", label: "Solid", fills: true, tooltip: "Filled triangles in one colour (or the chosen attribute's)" },
  { value: "wire-solid", label: "Wireframe + Solid", fills: true, tooltip: "Filled triangles with their edges over them" },
  { value: "flat", label: "Flat", fills: true, tooltip: "Lit by each face's normal — the geometry's own, or the chosen normal attribute's at the face's last vertex — which is what shows a wrong winding or a fold" },
  { value: "smooth", label: "Smooth", fills: true, tooltip: "Lit by normals interpolated across each face — the chosen attribute's, or the faces' around each position averaged" },
  { value: "points", label: "Points", fills: false, tooltip: "Only the vertices" },
];

export const CAMERA_MODES: { value: CameraMode; label: string; tooltip: string }[] = [
  { value: "arcball", label: "Arcball", tooltip: "Drag to turn around the model, middle or shift drag to slide it, wheel to come closer" },
  { value: "fly", label: "Fly", tooltip: "Drag to look, WASD to walk, Q and E to rise and fall, shift to hurry, wheel to change speed" },
];

/** Where the camera is, for a bookmark. */
export interface CameraState {
  mode: CameraMode;
  yaw: number;
  pitch: number;
  distance: number;
  center: [number, number, number];
  eye: [number, number, number];
  speed: number;
}

type Mat4 = Float32Array;
type Vec3 = [number, number, number];

function multiply(a: Mat4, b: Mat4): Mat4 {
  const out = new Float32Array(16);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = s;
    }
  }
  return out;
}

function perspective(fovY: number, aspect: number, near: number, far: number): Mat4 {
  const f = 1 / Math.tan(fovY / 2);
  const m = new Float32Array(16);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (far + near) / (near - far);
  m[11] = -1;
  m[14] = (2 * far * near) / (near - far);
  return m;
}

const sub = (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a: Vec3, b: Vec3): Vec3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scale = (a: Vec3, s: number): Vec3 => [a[0] * s, a[1] * s, a[2] * s];
const norm = (v: Vec3): Vec3 => { const l = Math.hypot(v[0], v[1], v[2]) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
const cross = (a: Vec3, b: Vec3): Vec3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const dot = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

function lookAt(eye: Vec3, target: Vec3, up: Vec3): Mat4 {
  const z = norm(sub(eye, target));
  const x = norm(cross(up, z));
  const y = cross(z, x);
  return new Float32Array([x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1]);
}

/**
 * The box the bulk of the vertices lie in: each axis from its 2nd to its 98th percentile. What a view
 * should be framed on when a few primitives are far larger than the rest. Sampled on a big mesh,
 * which is plenty for a percentile. Null with too few vertices for a percentile to mean anything.
 */
function robustBounds(drawn: Float32Array, valid: Uint8Array): { min: number[]; max: number[] } | null {
  const count = valid.length;
  const step = Math.max(1, Math.floor(count / 200000));
  const axes: number[][] = [[], [], []];
  for (let v = 0; v < count; v += step) {
    if (!valid[v]) continue;
    for (let k = 0; k < 3; k++) axes[k].push(drawn[v * 3 + k]);
  }
  if (axes[0].length < 16) return null;
  const min: number[] = [];
  const max: number[] = [];
  for (const values of axes) {
    values.sort((a, b) => a - b);
    min.push(values[Math.floor(values.length * 0.02)]);
    max.push(values[Math.min(values.length - 1, Math.ceil(values.length * 0.98))]);
  }
  return { min, max };
}

/**
 * An attribute as a colour per vertex. Values already between 0 and 1 are taken as they are (a
 * colour); anything else is stretched per component over its own range, so a position or a normal
 * shows as a gradient across the mesh.
 */
export function attributeColors(attribute: PreviewAttribute, count: number): Float32Array {
  const out = new Float32Array(count * 3);
  const n = Math.min(3, attribute.components);
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  const values: (ArrayLike<number> | null)[] = new Array(count);
  for (let v = 0; v < count; v++) {
    const a = attribute.read(v);
    values[v] = a;
    if (!a) continue;
    for (let k = 0; k < n; k++) {
      const x = a[k];
      if (!Number.isFinite(x)) continue;
      if (x < min[k]) min[k] = x;
      if (x > max[k]) max[k] = x;
    }
  }
  const unit = attribute.isColor || [0, 1, 2].slice(0, n).every((k) => min[k] >= 0 && max[k] <= 1);
  for (let v = 0; v < count; v++) {
    const a = values[v];
    for (let k = 0; k < 3; k++) {
      // A one- or two-component attribute repeats its last component, so it reads as grey or a ramp.
      const x = a ? a[Math.min(k, n - 1)] : 0;
      const kk = Math.min(k, n - 1);
      out[v * 3 + k] = !Number.isFinite(x) ? 0 : unit ? x : max[kk] > min[kk] ? (x - min[kk]) / (max[kk] - min[kk]) : 0.5;
    }
  }
  return out;
}

/** Normals from an attribute's first three components, per vertex. */
function attributeNormals(attribute: PreviewAttribute, count: number): Float32Array {
  const out = new Float32Array(count * 3);
  for (let v = 0; v < count; v++) {
    const a = attribute.read(v);
    if (!a || attribute.components < 3) continue;
    out[v * 3] = a[0];
    out[v * 3 + 1] = a[1];
    out[v * 3 + 2] = a[2];
  }
  return out;
}

/**
 * Smooth normals from the geometry alone: each triangle's normal, weighted by its area, summed at
 * every vertex sharing a position with its corners. A triangle list repeats a shared vertex once per
 * triangle, so sharing is by position rather than by index.
 */
export function smoothNormals(drawn: Float32Array, valid: Uint8Array): Float32Array {
  const count = valid.length;
  const slot = new Int32Array(count);
  const bySpot = new Map<string, number>();
  for (let v = 0; v < count; v++) {
    const key = `${drawn[v * 3]},${drawn[v * 3 + 1]},${drawn[v * 3 + 2]}`;
    let s = bySpot.get(key);
    if (s === undefined) {
      s = bySpot.size;
      bySpot.set(key, s);
    }
    slot[v] = s;
  }
  const sum = new Float32Array(bySpot.size * 3);
  for (let v = 0; v + 2 < count; v += 3) {
    if (!valid[v] || !valid[v + 1] || !valid[v + 2]) continue;
    const a: Vec3 = [drawn[v * 3], drawn[v * 3 + 1], drawn[v * 3 + 2]];
    const u = sub([drawn[v * 3 + 3], drawn[v * 3 + 4], drawn[v * 3 + 5]], a);
    const w = sub([drawn[v * 3 + 6], drawn[v * 3 + 7], drawn[v * 3 + 8]], a);
    const n = cross(u, w);
    for (let k = 0; k < 3; k++) {
      const s = slot[v + k] * 3;
      sum[s] += n[0];
      sum[s + 1] += n[1];
      sum[s + 2] += n[2];
    }
  }
  const out = new Float32Array(count * 3);
  for (let v = 0; v < count; v++) {
    const s = slot[v] * 3;
    const n = norm([sum[s], sum[s + 1], sum[s + 2]]);
    out[v * 3] = n[0];
    out[v * 3 + 1] = n[1];
    out[v * 3 + 2] = n[2];
  }
  return out;
}

/** The preview's vertical field of view. */
const FOV = Math.PI / 6;
/** How far the pitch may go before the up vector and the view line up. */
const PITCH_LIMIT = 1.55;
/** A mesh with more triangles than this is only picked on a click, not on every hover. */
const HOVER_LIMIT = 300000;
/** The most normals the overlay draws; a bigger mesh shows every nth. */
const NORMAL_LIMIT = 100000;

const VERTEX = `#version 300 es
in vec3 position;
in vec3 color;
in vec3 normal;
uniform mat4 transform;
uniform mat4 view;
uniform float pointSize;
out vec3 viewPosition;
out vec3 vertexColor;
out vec3 smoothNormal;
flat out vec3 flatNormal;
void main() {
  viewPosition = (view * vec4(position, 1.0)).xyz;
  vertexColor = color;
  vec3 n = mat3(view) * normal;
  smoothNormal = n;
  flatNormal = n;
  gl_Position = transform * vec4(position, 1.0);
  gl_PointSize = pointSize;
}`;

// `lighting`: 0 none, 1 the face's own normal (the derivatives of the view-space position give it
// exactly: a triangle is planar, so its screen-space gradient is constant across it), 2 the normal
// attribute of the face's last vertex, 3 the normal interpolated across the face. A headlight at
// the eye, and both sides lit: a back face is worth seeing, not worth hiding.
const FRAGMENT = `#version 300 es
precision highp float;
uniform vec4 color;
uniform int lighting;
uniform bool useVertexColor;
in vec3 viewPosition;
in vec3 vertexColor;
in vec3 smoothNormal;
flat in vec3 flatNormal;
out vec4 outColor;
void main() {
  vec3 base = useVertexColor ? vertexColor : color.rgb;
  if (lighting == 0) {
    outColor = vec4(base, color.a);
    return;
  }
  vec3 normal = lighting == 1 ? cross(dFdx(viewPosition), dFdy(viewPosition))
              : lighting == 2 ? flatNormal : smoothNormal;
  float facing = abs(dot(normalize(normal), normalize(-viewPosition)));
  outColor = vec4(base * (0.25 + 0.75 * facing), color.a);
}`;

/**
 * The outline of the view volume as line pairs. Normalized device coordinates are drawn with y and z
 * negated, so the render target's right, up and into the screen are the preview's x, y and -z.
 */
const VOLUME = (() => {
  const c = [[-1, -1], [1, -1], [1, 1], [-1, 1]];
  const out: number[] = [];
  for (let i = 0; i < 4; i++) {
    const [x0, y0] = c[i];
    const [x1, y1] = c[(i + 1) % 4];
    out.push(x0, y0, 0, x1, y1, 0, x0, y0, -1, x1, y1, -1, x0, y0, 0, x0, y0, -1);
  }
  return new Float32Array(out);
})();

/** An overlay as uploaded. */
interface OverlayState {
  source: PreviewOverlay;
  vertices: WebGLBuffer | null;
  colors: WebGLBuffer | null;
  indices: WebGLBuffer | null;
  count: number;
  /** Per vertex: 1 when its group is hidden. */
  hidden: Uint8Array;
}

export class MeshPreview {
  readonly root: Div;
  private readonly _canvas: HTMLCanvasElement;
  private readonly _hover: HTMLDivElement;
  private readonly _gl: WebGL2RenderingContext | null;
  private _program: WebGLProgram | null = null;
  private _uniforms: Record<string, WebGLUniformLocation | null> = {};
  private _vertexBuffer: WebGLBuffer | null = null;
  private _colorBuffer: WebGLBuffer | null = null;
  private _normalBuffer: WebGLBuffer | null = null;
  private _edgeBuffer: WebGLBuffer | null = null;
  private _triangleBuffer: WebGLBuffer | null = null;
  private _pointBuffer: WebGLBuffer | null = null;
  private _volumeBuffer: WebGLBuffer | null = null;
  private _scratchBuffer: WebGLBuffer | null = null;
  private _selectionBuffer: WebGLBuffer | null = null;
  private _normalLinesBuffer: WebGLBuffer | null = null;
  private _edges = 0;
  private _points = 0;
  private _filled = 0;
  private _selectionEdges = 0;
  private _normalLines = 0;
  private _clip = false;
  private _kind: PrimitiveKind = "triangles";
  private _mesh: PreviewMesh | null = null;
  /** The drawn positions (x y z), for picking, framing and the highlighted vertex. */
  private _drawn = new Float32Array(0);
  private _valid = new Uint8Array(0);
  /** Per vertex: 1 when its group is hidden. */
  private _hiddenVertex = new Uint8Array(0);
  private _hidden = new Set<number>();
  private _overlays: OverlayState[] = [];
  private _highlight: number | null = null;
  private _selection: { hit: PreviewHit | null; groups: number[] } = { hit: null, groups: [] };
  private _colorSource = -1;
  private _normalSource = -1;
  private _showNormals = false;
  /** Normals as uploaded, per vertex, or null when none are (face lighting needs none). */
  private _normals: Float32Array | null = null;
  private _smooth: Float32Array | null = null;
  private _center: Vec3 = [0, 0, 0];
  private _radius = 1;
  /**
   * The radius of everything, where `_radius` is of what the view is framed on. They differ when a
   * few primitives dwarf the rest: the view frames the rest, and the zoom limit and the depth range
   * still reach the few.
   */
  private _fullRadius = 1;
  /** The largest half-extent of the framed box: what the view is fitted to (the radius is its corner). */
  private _extent = 1;
  /** How close the arcball may come, which framing something small lowers. */
  private _closest = 0.05;

  // The camera. Arcball orbits `_center`; fly walks `_eye`. Both share the yaw and pitch, so
  // switching mode keeps you looking the way you were.
  private _mode: CameraMode = "arcball";
  private _shade: ShadeMode = "wireframe";
  private _yaw = 0;
  private _pitch = 0;
  private _distance = 3;
  private _eye: Vec3 = [0, 0, 3];
  /** Units a second at a walk, scaled to the scene when a mesh is set. */
  private _speed = 1;
  private _held = new Set<string>();
  private _lastStep = 0;
  private _frame = 0;
  private _walking = 0;
  private _hoverTimer = 0;
  /** The last transform drawn with, for picking. */
  private _transform: Mat4 = new Float32Array(16);
  private readonly _resize: ResizeObserver;
  /** Told when the camera or a mode changes, so a toolbar can follow. */
  onModeChange: (() => void) | null = null;
  /** Told when a click selects something (or nothing). */
  onPick: ((hit: PreviewHit | null) => void) | null = null;
  /** Names what the pointer is over; the preview's own words ("triangle 12") otherwise. */
  describe: ((hit: PreviewHit) => string) | null = null;
  /** Ctrl+digit keeps a bookmark in `slot`, a digit goes back to it. */
  onBookmarkKey: ((slot: number, save: boolean) => void) | null = null;

  constructor(parent: Widget) {
    this.root = new Div(parent, { class: "mesh-preview" });
    this._canvas = document.createElement("canvas");
    this._canvas.className = "mesh-preview-canvas";
    // A canvas only takes key events when it can hold focus.
    this._canvas.tabIndex = 0;
    this.root.element.appendChild(this._canvas);
    this._hover = document.createElement("div");
    this._hover.className = "mesh-preview-hover";
    this.root.element.appendChild(this._hover);
    this._updateHint();
    this._gl = this._canvas.getContext("webgl2", { antialias: true, alpha: true, preserveDrawingBuffer: true });
    if (!this._gl || !this._init(this._gl)) {
      new Div(this.root, { text: "This window has no WebGL2, which the mesh preview draws with.", class: "text-muted mesh-preview-note" });
    }
    this._bindMouse();
    this._bindKeys();
    this._resize = new ResizeObserver(() => this._schedule());
    this._resize.observe(this.root.element);
  }

  get cameraMode(): CameraMode { return this._mode; }
  get shadeMode(): ShadeMode { return this._shade; }
  /** Whether filling means anything for what is loaded: only a triangle list has faces. */
  get canFill(): boolean { return this._kind === "triangles" && this._filled > 0; }
  /** What the vertices can be coloured by: the mesh's attributes, by name. */
  get colorSources(): string[] { return (this._mesh?.attributes ?? []).map((a) => a.name); }
  /** What the normals can come from: the attributes with three components or more. */
  get normalSources(): string[] { return (this._mesh?.attributes ?? []).filter((a) => a.components >= 3 && !a.isColor && !a.isPosition).map((a) => a.name); }
  get colorSource(): number { return this._colorSource; }
  get normalSource(): number { return this._normalSource; }
  get showNormals(): boolean { return this._showNormals; }
  get hasSelection(): boolean { return !!this._selection.hit || this._selection.groups.length > 0 || this._highlight !== null; }

  setCameraMode(mode: CameraMode): void {
    if (this._mode === mode) return;
    this._mode = mode;
    // Entering fly, stand where the arcball's eye was, so the view does not jump.
    if (mode === "fly") this._eye = this._orbitEye();
    else this._distance = Math.max(this._radius * 0.05, Math.hypot(...sub(this._eye, this._center)));
    this._updateHint();
    this._schedule();
    this.onModeChange?.();
  }

  setShadeMode(mode: ShadeMode): void {
    if (this._shade === mode) return;
    this._shade = mode;
    this._uploadNormals();
    this._schedule();
    this.onModeChange?.();
  }

  /** Colours the vertices by the attribute at `index` of `colorSources`, or -1 for the one colour. */
  setColorSource(index: number): void {
    this._colorSource = index >= 0 && index < this.colorSources.length ? index : -1;
    this._uploadColors();
    this._schedule();
    this.onModeChange?.();
  }

  /** Takes normals from the attribute at `index` of `normalSources`, or -1 for the geometry's own. */
  setNormalSource(index: number): void {
    this._normalSource = index >= 0 && index < this.normalSources.length ? index : -1;
    this._uploadNormals();
    this._schedule();
    this.onModeChange?.();
  }

  /** Draws each vertex's normal (or each face's, with no normal attribute chosen) as a short line. */
  setShowNormals(show: boolean): void {
    this._showNormals = show;
    this._uploadNormals();
    this._schedule();
    this.onModeChange?.();
  }

  /** Shows a mesh; `keepView` keeps the camera (stepping between draws of one pass). */
  setMesh(mesh: PreviewMesh | null, keepView = false): void {
    const gl = this._gl;
    this._mesh = mesh;
    this._highlight = null;
    this._selection = { hit: null, groups: [] };
    this._smooth = null;
    this._clip = mesh?.clip ?? false;
    this._kind = mesh?.kind ?? "triangles";
    if (this._colorSource >= this.colorSources.length) this._colorSource = -1;
    if (this._normalSource >= this.normalSources.length) this._normalSource = -1;
    const stride = this._clip ? 4 : 3;
    const count = mesh ? Math.floor(mesh.positions.length / stride) : 0;
    const drawn = new Float32Array(count * 3);
    const valid = new Uint8Array(count);
    const min = [Infinity, Infinity, Infinity];
    const max = [-Infinity, -Infinity, -Infinity];
    for (let v = 0; v < count && mesh; v++) {
      const p = mesh.positions;
      let x = p[v * stride], y = p[v * stride + 1], z = p[v * stride + 2];
      if (this._clip) {
        const w = p[v * 4 + 3];
        if (!(w > 0)) continue;
        x /= w;
        y = -y / w;   // Vulkan's y points down the render target
        z = -z / w;   // and its z into it
      }
      if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) continue;
      drawn[v * 3] = x;
      drawn[v * 3 + 1] = y;
      drawn[v * 3 + 2] = z;
      valid[v] = 1;
      min[0] = Math.min(min[0], x); min[1] = Math.min(min[1], y); min[2] = Math.min(min[2], z);
      max[0] = Math.max(max[0], x); max[1] = Math.max(max[1], y); max[2] = Math.max(max[2], z);
    }
    // Overlays count toward the framing: a scene of stand-in boxes has nothing else.
    for (const o of mesh?.overlays ?? []) {
      for (let v = 0; v + 2 < o.positions.length; v += 3) {
        for (let k = 0; k < 3; k++) {
          const x = o.positions[v + k];
          if (!Number.isFinite(x)) continue;
          min[k] = Math.min(min[k], x);
          max[k] = Math.max(max[k], x);
        }
      }
    }
    this._drawn = drawn;
    this._valid = valid;

    if (!keepView) {
      this._fullRadius = 0;
      if (this._clip) {
        // Framed on the view volume, and on the geometry when it strays outside, up to four times the
        // volume: vertices near w = 0 divide out to enormous coordinates that would shrink it to a dot.
        for (let k = 0; k < 3; k++) {
          min[k] = Math.max(Math.min(min[k], -1), -4);
          max[k] = Math.min(Math.max(max[k], k === 2 ? 0 : 1), 4);
        }
      }
      if (min[0] > max[0]) {
        min.fill(-1);
        max.fill(1);
      } else if (!this._clip) {
        this._fullRadius = Math.max(1e-6, Math.hypot(max[0] - min[0], max[1] - min[1], max[2] - min[2]) / 2);
        // Framed on where the geometry is, not on its farthest vertex. One enormous primitive —
        // a ground plane, a skybox, a sphere of radius 1000 under a scene of small ones — would
        // otherwise shrink everything else to a speck; the whole of it is still there, a wheel away.
        const robust = robustBounds(drawn, valid);
        if (robust) {
          const full = Math.max(max[0] - min[0], max[1] - min[1], max[2] - min[2]);
          const core = Math.max(robust.max[0] - robust.min[0], robust.max[1] - robust.min[1], robust.max[2] - robust.min[2]);
          if (core > 0 && full > core * 4) {
            for (let k = 0; k < 3; k++) {
              min[k] = robust.min[k];
              max[k] = robust.max[k];
            }
          }
        }
      }
      this._center = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2];
      this._radius = Math.max(1e-6, Math.hypot(max[0] - min[0], max[1] - min[1], max[2] - min[2]) / 2);
      this._fullRadius = Math.max(this._fullRadius, this._radius);
      this._extent = Math.max(1e-6, (max[0] - min[0]) / 2, (max[1] - min[1]) / 2, (max[2] - min[2]) / 2);
      this._closest = this._radius * 0.05;
      this.resetView();
    }
    if (gl && this._program) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this._vertexBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, drawn, gl.STATIC_DRAW);
    }
    this._uploadOverlays();
    this._applyHidden();
    this._uploadColors();
    this._uploadNormals();
    this._uploadSelection();
    this._schedule();
    this.onModeChange?.();
  }

  /** Replaces the overlays, keeping the camera and everything else. */
  setOverlays(overlays: PreviewOverlay[]): void {
    if (!this._mesh) return;
    this._mesh = { ...this._mesh, overlays };
    this._uploadOverlays();
    this._applyHidden();
    this._schedule();
  }

  /** Replaces the attributes (a caller's colouring changed), keeping the chosen sources where they still exist. */
  setAttributes(attributes: PreviewAttribute[]): void {
    if (!this._mesh) return;
    const colorName = this._colorSource >= 0 ? this.colorSources[this._colorSource] : null;
    const normalName = this._normalSource >= 0 ? this.normalSources[this._normalSource] : null;
    this._mesh = { ...this._mesh, attributes };
    this._colorSource = colorName ? this.colorSources.indexOf(colorName) : -1;
    this._normalSource = normalName ? this.normalSources.indexOf(normalName) : -1;
    this._uploadColors();
    this._uploadNormals();
    this._schedule();
    this.onModeChange?.();
  }

  /** Hides the groups with these ids, in the mesh and its overlays alike. */
  setHidden(ids: Iterable<number>): void {
    this._hidden = new Set(ids);
    this._applyHidden();
    this._uploadNormals();
    this._schedule();
  }

  /** Marks one vertex (an index into the list the mesh was given), or none. */
  highlight(vertex: number | null): void {
    this._highlight = vertex !== null && vertex < this._drawn.length / 3 ? vertex : null;
    this._schedule();
    this.onModeChange?.();
  }

  /** Selects a primitive, or whole groups, drawn over everything else. Null clears it. */
  select(hit: PreviewHit | null, groups: number[] = []): void {
    this._selection = { hit, groups };
    this._uploadSelection();
    this._schedule();
    this.onModeChange?.();
  }

  /** Frames what is selected (or the highlighted vertex); false when nothing is. */
  zoomToSelection(): boolean {
    let box: { min: Vec3; max: Vec3 } | null = null;
    const take = (p: ArrayLike<number>, v: number): void => {
      const x = p[v * 3], y = p[v * 3 + 1], z = p[v * 3 + 2];
      if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return;
      if (!box) box = { min: [x, y, z], max: [x, y, z] };
      else {
        box.min = [Math.min(box.min[0], x), Math.min(box.min[1], y), Math.min(box.min[2], z)];
        box.max = [Math.max(box.max[0], x), Math.max(box.max[1], y), Math.max(box.max[2], z)];
      }
    };
    const { hit, groups } = this._selection;
    for (const id of groups) {
      for (const g of this._mesh?.groups ?? []) if (g.id === id) for (let v = g.first; v < g.first + g.count; v++) if (this._valid[v]) take(this._drawn, v);
      for (const o of this._overlays) for (const g of o.source.groups ?? []) if (g.id === id) for (let v = g.first; v < g.first + g.count; v++) take(o.source.positions, v);
    }
    if (hit) {
      const per = hit.overlay >= 0 ? 2 : this._perPrimitive();
      const positions = hit.overlay >= 0 ? this._overlays[hit.overlay]?.source.positions : this._drawn;
      if (positions) for (let k = 0; k < per; k++) take(positions, hit.vertex + k);
    }
    if (!box && this._highlight !== null) take(this._drawn, this._highlight);
    if (!box) return false;
    this.frameBox((box as { min: Vec3; max: Vec3 }).min, (box as { min: Vec3; max: Vec3 }).max);
    return true;
  }

  /** Points the camera at a box, from the direction it looks now. */
  frameBox(min: Vec3, max: Vec3): void {
    const center: Vec3 = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2];
    // A point or a sliver still gets a view of some size: a hundredth of the scene.
    const extent = Math.max(this._radius * 0.01, (max[0] - min[0]) / 2, (max[1] - min[1]) / 2, (max[2] - min[2]) / 2);
    const distance = extent * 1.6 / Math.sin(FOV / 2);
    this._closest = Math.min(this._radius * 0.05, distance * 0.2);
    this._center = center;
    this._distance = distance;
    if (this._mode === "fly") {
      this._eye = sub(center, scale(this._forward(), distance));
      this._speed = Math.max(1e-6, extent * 2);
    }
    this._schedule();
    this.onModeChange?.();
  }

  resetView(): void {
    // VS Out faces the render target; VS In is turned a little so its depth shows.
    this._yaw = this._clip ? 0 : 0.6;
    this._pitch = this._clip ? 0 : 0.35;
    // Far enough for the mesh to fill most of a 30 degree view: flatter than 45, so the volume reads as a box.
    this._distance = this._extent * 1.25 / Math.sin(FOV / 2);
    this._closest = this._radius * 0.05;
    this._eye = this._orbitEye();
    // A walk crosses the scene in a few seconds, whatever scale it is in.
    this._speed = Math.max(1e-6, this._radius);
    this._schedule();
    this.onModeChange?.();
  }

  get camera(): CameraState {
    return {
      mode: this._mode, yaw: this._yaw, pitch: this._pitch, distance: this._distance,
      center: [...this._center], eye: [...this._eye], speed: this._speed,
    };
  }

  set camera(state: CameraState) {
    this._mode = state.mode;
    this._yaw = state.yaw;
    this._pitch = state.pitch;
    this._distance = state.distance;
    this._center = [...state.center];
    this._eye = [...state.eye];
    this._speed = state.speed;
    this._closest = Math.min(this._radius * 0.05, state.distance * 0.2);
    this._updateHint();
    this._schedule();
    this.onModeChange?.();
  }

  dispose(): void {
    this._resize.disconnect();
    if (this._frame) cancelAnimationFrame(this._frame);
    if (this._walking) cancelAnimationFrame(this._walking);
    if (this._hoverTimer) clearTimeout(this._hoverTimer);
    this._held.clear();
  }

  debugState(): Record<string, unknown> {
    return {
      webgl: !!this._program, edges: this._edges / 2, points: this._points, triangles: this._filled / 3,
      clip: this._clip, camera: this._mode, shading: this._shade, canFill: this.canFill,
      color: this._colorSource >= 0 ? this.colorSources[this._colorSource] : null,
      normals: this._normalSource >= 0 ? this.normalSources[this._normalSource] : null,
      showNormals: this._showNormals, normalLines: this._normalLines / 2,
      hidden: this._hidden.size, overlays: this._overlays.map((o) => o.count / 2),
      selection: this._selection.hit ? { ...this._selection.hit } : null, selectedGroups: this._selection.groups,
      center: [...this._center], distance: this._distance,
    };
  }

  /** What the preview would pick at a point of the canvas, in CSS pixels (the UI tests' way in). */
  pickAt(x: number, y: number): PreviewHit | null {
    return this._pick(x, y);
  }

  // ---------------------------------------------------------------------------------------

  private _perPrimitive(): number {
    return this._kind === "triangles" ? 3 : this._kind === "lines" ? 2 : 1;
  }

  /** Rebuilds the index lists without the hidden groups' vertices. */
  private _applyHidden(): void {
    const count = this._valid.length;
    this._hiddenVertex = new Uint8Array(count);
    for (const g of this._mesh?.groups ?? []) {
      if (!this._hidden.has(g.id)) continue;
      this._hiddenVertex.fill(1, g.first, Math.min(count, g.first + g.count));
    }
    const per = this._perPrimitive();
    const edges: number[] = [];
    const triangles: number[] = [];
    const points: number[] = [];
    for (let i = 0; i + per <= count; i += per) {
      let ok = true;
      for (let k = 0; k < per; k++) ok = ok && this._valid[i + k] === 1 && !this._hiddenVertex[i + k];
      if (!ok) continue;
      for (let k = 0; k < per; k++) points.push(i + k);
      if (per === 3) {
        edges.push(i, i + 1, i + 1, i + 2, i + 2, i);
        triangles.push(i, i + 1, i + 2);
      } else if (per === 2) edges.push(i, i + 1);
    }
    this._edges = edges.length;
    this._filled = triangles.length;
    this._points = points.length;
    const gl = this._gl;
    if (gl && this._program) {
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._edgeBuffer);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(edges), gl.STATIC_DRAW);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._triangleBuffer);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(triangles), gl.STATIC_DRAW);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._pointBuffer);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(points), gl.STATIC_DRAW);
    }
    for (const o of this._overlays) {
      const n = o.source.positions.length / 3;
      o.hidden = new Uint8Array(n);
      for (const g of o.source.groups ?? []) if (this._hidden.has(g.id)) o.hidden.fill(1, g.first, Math.min(n, g.first + g.count));
      const list: number[] = [];
      for (let v = 0; v + 1 < n; v += 2) if (!o.hidden[v] && !o.hidden[v + 1]) list.push(v, v + 1);
      o.count = list.length;
      if (gl && o.indices) {
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, o.indices);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(list), gl.STATIC_DRAW);
      }
    }
  }

  private _uploadOverlays(): void {
    const gl = this._gl;
    for (const o of this._overlays) {
      if (!gl) break;
      gl.deleteBuffer(o.vertices);
      gl.deleteBuffer(o.colors);
      gl.deleteBuffer(o.indices);
    }
    this._overlays = (this._mesh?.overlays ?? []).map((source) => {
      const state: OverlayState = { source, vertices: null, colors: null, indices: null, count: 0, hidden: new Uint8Array(0) };
      if (gl && this._program) {
        state.vertices = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, state.vertices);
        gl.bufferData(gl.ARRAY_BUFFER, source.positions, gl.STATIC_DRAW);
        if (source.colors) {
          state.colors = gl.createBuffer();
          gl.bindBuffer(gl.ARRAY_BUFFER, state.colors);
          gl.bufferData(gl.ARRAY_BUFFER, source.colors, gl.STATIC_DRAW);
        }
        state.indices = gl.createBuffer();
      }
      return state;
    });
  }

  private _uploadColors(): void {
    const gl = this._gl;
    const attribute = this._colorSource >= 0 ? this._mesh?.attributes?.[this._colorSource] : null;
    if (!gl || !this._program || !attribute) return;
    gl.bindBuffer(gl.ARRAY_BUFFER, this._colorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, attributeColors(attribute, this._valid.length), gl.STATIC_DRAW);
  }

  /** The attribute the normals come from, or null for the geometry's own. */
  private _normalAttribute(): PreviewAttribute | null {
    if (this._normalSource < 0) return null;
    const name = this.normalSources[this._normalSource];
    return this._mesh?.attributes?.find((a) => a.name === name) ?? null;
  }

  /** The normals the shading and the overlay use, uploaded when either needs them. */
  private _uploadNormals(): void {
    const gl = this._gl;
    const attribute = this._normalAttribute();
    const needed = this._shade === "smooth" || (this._shade === "flat" && attribute) || this._showNormals;
    this._normals = null;
    if (needed && this._kind === "triangles") {
      if (attribute) this._normals = attributeNormals(attribute, this._valid.length);
      else if (this._shade === "smooth" || this._showNormals) {
        this._smooth ??= smoothNormals(this._drawn, this._valid);
        this._normals = this._smooth;
      }
    } else if (needed && attribute) {
      this._normals = attributeNormals(attribute, this._valid.length);
    }
    if (gl && this._program && this._normals) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this._normalBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, this._normals, gl.STATIC_DRAW);
    }
    this._uploadNormalLines(attribute !== null);
  }

  /** Short lines along the normals: each vertex's with an attribute chosen, each face's without. */
  private _uploadNormalLines(fromAttribute: boolean): void {
    const gl = this._gl;
    this._normalLines = 0;
    if (!this._showNormals) return;
    const drawn = this._drawn;
    const length = this._extent * 0.04;
    const out: number[] = [];
    const count = this._valid.length;
    if (fromAttribute && this._normals) {
      const step = Math.max(1, Math.ceil(count / NORMAL_LIMIT));
      for (let v = 0; v < count; v += step) {
        if (!this._valid[v] || this._hiddenVertex[v]) continue;
        const n = norm([this._normals[v * 3], this._normals[v * 3 + 1], this._normals[v * 3 + 2]]);
        out.push(drawn[v * 3], drawn[v * 3 + 1], drawn[v * 3 + 2],
                 drawn[v * 3] + n[0] * length, drawn[v * 3 + 1] + n[1] * length, drawn[v * 3 + 2] + n[2] * length);
      }
    } else if (this._kind === "triangles") {
      const faces = Math.floor(count / 3);
      const step = Math.max(1, Math.ceil(faces / NORMAL_LIMIT));
      for (let f = 0; f < faces; f += step) {
        const v = f * 3;
        if (!this._valid[v] || !this._valid[v + 1] || !this._valid[v + 2] || this._hiddenVertex[v]) continue;
        const a: Vec3 = [drawn[v * 3], drawn[v * 3 + 1], drawn[v * 3 + 2]];
        const b: Vec3 = [drawn[v * 3 + 3], drawn[v * 3 + 4], drawn[v * 3 + 5]];
        const c: Vec3 = [drawn[v * 3 + 6], drawn[v * 3 + 7], drawn[v * 3 + 8]];
        const n = norm(cross(sub(b, a), sub(c, a)));
        const m = scale(add(add(a, b), c), 1 / 3);
        out.push(m[0], m[1], m[2], m[0] + n[0] * length, m[1] + n[1] * length, m[2] + n[2] * length);
      }
    }
    this._normalLines = out.length / 3;
    if (gl && this._program) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this._normalLinesBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(out), gl.STATIC_DRAW);
    }
  }

  /** The edges of what is selected, as line pairs of positions. */
  private _uploadSelection(): void {
    const gl = this._gl;
    const out: number[] = [];
    const edgesOf = (p: ArrayLike<number>, first: number, count: number, per: number): void => {
      for (let v = first; v + per <= first + count; v += per) {
        const corners = per === 3 ? [[0, 1], [1, 2], [2, 0]] : per === 2 ? [[0, 1]] : [[0, 0]];
        for (const [a, b] of corners) {
          out.push(p[(v + a) * 3], p[(v + a) * 3 + 1], p[(v + a) * 3 + 2], p[(v + b) * 3], p[(v + b) * 3 + 1], p[(v + b) * 3 + 2]);
        }
      }
    };
    const per = this._perPrimitive();
    for (const id of this._selection.groups) {
      for (const g of this._mesh?.groups ?? []) if (g.id === id) edgesOf(this._drawn, g.first, g.count, per);
      for (const o of this._overlays) for (const g of o.source.groups ?? []) if (g.id === id) edgesOf(o.source.positions, g.first, g.count, 2);
    }
    const hit = this._selection.hit;
    if (hit) {
      if (hit.overlay >= 0) {
        const o = this._overlays[hit.overlay];
        if (o) edgesOf(o.source.positions, hit.vertex, 2, 2);
      } else {
        edgesOf(this._drawn, hit.vertex, per, per);
      }
    }
    this._selectionEdges = out.length / 3;
    if (gl && this._program) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this._selectionBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(out), gl.STATIC_DRAW);
    }
  }

  /** Where the arcball's eye sits: on a sphere around the center, at the current yaw and pitch. */
  private _orbitEye(): Vec3 {
    const d = this._distance;
    return [
      this._center[0] + d * Math.sin(this._yaw) * Math.cos(this._pitch),
      this._center[1] + d * Math.sin(this._pitch),
      this._center[2] + d * Math.cos(this._yaw) * Math.cos(this._pitch),
    ];
  }

  /** Which way the eye looks, from the yaw and pitch. The arcball looks back at its center. */
  private _forward(): Vec3 {
    return norm([-Math.sin(this._yaw) * Math.cos(this._pitch), -Math.sin(this._pitch), -Math.cos(this._yaw) * Math.cos(this._pitch)]);
  }

  private _updateHint(): void {
    this._canvas.title = this._mode === "fly"
      ? "Drag to look, WASD to walk, Q and E to rise and fall, shift to hurry, wheel to change speed. Click to select, double-click to reset. Ctrl+1-9 bookmarks the camera, 1-9 goes back"
      : "Drag to turn, middle or shift drag to slide, wheel to zoom. Click to select, double-click to reset. Ctrl+1-9 bookmarks the camera, 1-9 goes back";
  }

  private _init(gl: WebGL2RenderingContext): boolean {
    const compile = (type: number, source: string): WebGLShader | null => {
      const s = gl.createShader(type);
      if (!s) return null;
      gl.shaderSource(s, source);
      gl.compileShader(s);
      return gl.getShaderParameter(s, gl.COMPILE_STATUS) ? s : null;
    };
    const vs = compile(gl.VERTEX_SHADER, VERTEX);
    const fs = compile(gl.FRAGMENT_SHADER, FRAGMENT);
    const program = gl.createProgram();
    if (!vs || !fs || !program) return false;
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.bindAttribLocation(program, 0, "position");
    gl.bindAttribLocation(program, 1, "color");
    gl.bindAttribLocation(program, 2, "normal");
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) return false;
    this._program = program;
    for (const name of ["transform", "view", "color", "lighting", "useVertexColor", "pointSize"]) {
      this._uniforms[name] = gl.getUniformLocation(program, name);
    }
    this._vertexBuffer = gl.createBuffer();
    this._colorBuffer = gl.createBuffer();
    this._normalBuffer = gl.createBuffer();
    this._edgeBuffer = gl.createBuffer();
    this._triangleBuffer = gl.createBuffer();
    this._pointBuffer = gl.createBuffer();
    this._volumeBuffer = gl.createBuffer();
    this._scratchBuffer = gl.createBuffer();
    this._selectionBuffer = gl.createBuffer();
    this._normalLinesBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this._volumeBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, VOLUME, gl.STATIC_DRAW);
    return true;
  }

  private _bindMouse(): void {
    const canvas = this._canvas;
    canvas.onmousedown = (e) => {
      e.preventDefault();
      canvas.focus();
      const startX = e.clientX;
      const startY = e.clientY;
      let x = e.clientX;
      let y = e.clientY;
      let moved = false;
      // Arcball: the middle button, the right button or shift slides the model instead of turning it.
      const sliding = this._mode === "arcball" && (e.button === 1 || e.button === 2 || e.shiftKey);
      const move = (m: MouseEvent): void => {
        const dx = m.clientX - x;
        const dy = m.clientY - y;
        x = m.clientX;
        y = m.clientY;
        if (Math.abs(m.clientX - startX) + Math.abs(m.clientY - startY) > 3) moved = true;
        if (!moved) return;
        if (sliding) this._slide(dx, dy);
        else this._look(dx, dy);
        this._schedule();
      };
      const up = (u: MouseEvent): void => {
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", up);
        // A click rather than a drag selects what is under it.
        if (!moved && e.button === 0) {
          const rect = canvas.getBoundingClientRect();
          const hit = this._pick(u.clientX - rect.left, u.clientY - rect.top);
          this.select(hit);
          this.onPick?.(hit);
        }
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", up);
    };
    canvas.onmousemove = (e) => {
      if (e.buttons) return;
      if (this._hoverTimer) clearTimeout(this._hoverTimer);
      // Only once the pointer rests, and only on a mesh small enough to test on every rest.
      this._hoverTimer = window.setTimeout(() => {
        this._hoverTimer = 0;
        if (this._filled / 3 > HOVER_LIMIT) return;
        const rect = canvas.getBoundingClientRect();
        this._showHover(this._pick(e.clientX - rect.left, e.clientY - rect.top));
      }, 60);
    };
    canvas.onmouseleave = () => {
      if (this._hoverTimer) clearTimeout(this._hoverTimer);
      this._showHover(null);
    };
    // The right button slides rather than opening a menu over the view.
    canvas.oncontextmenu = (e) => e.preventDefault();
    canvas.onwheel = (e) => {
      e.preventDefault();
      if (this._mode === "fly") {
        // Not a zoom: a field of view does not move you, so the wheel sets the walking pace.
        this._speed = Math.max(this._radius * 1e-3, Math.min(this._radius * 100, this._speed * Math.exp(-e.deltaY * 0.001)));
        this.onModeChange?.();
        return;
      }
      this._distance = Math.max(this._closest, Math.min(Math.max(this._radius * 50, this._fullRadius * 4), this._distance * Math.exp(e.deltaY * 0.001)));
      this._schedule();
    };
    canvas.ondblclick = () => this.resetView();
  }

  private _describe(hit: PreviewHit): string {
    if (this.describe) return this.describe(hit);
    const kind = hit.overlay >= 0 ? "line" : this._kind === "triangles" ? "triangle" : this._kind === "lines" ? "line" : "point";
    return `${kind} ${hit.primitive} (vertex ${hit.vertex})`;
  }

  private _showHover(hit: PreviewHit | null): void {
    this._hover.textContent = hit ? this._describe(hit) : "";
    this._hover.style.display = hit ? "block" : "none";
  }

  /**
   * What is under a point of the canvas: the nearest triangle whose projection holds it, or the
   * nearest line or point within a few pixels, hidden groups left out. On the CPU, against the
   * transform the last frame was drawn with.
   */
  private _pick(px: number, py: number): PreviewHit | null {
    const width = this.root.element.clientWidth;
    const height = this.root.element.clientHeight;
    if (!width || !height) return null;
    const m = this._transform;
    // A vertex on the screen: x and y in CSS pixels, and the depth to compare hits by.
    const project = (p: ArrayLike<number>, v: number): [number, number, number] | null => {
      const x = p[v * 3], y = p[v * 3 + 1], z = p[v * 3 + 2];
      const cw = m[3] * x + m[7] * y + m[11] * z + m[15];
      if (!(cw > 1e-9)) return null;
      const cx = (m[0] * x + m[4] * y + m[8] * z + m[12]) / cw;
      const cy = (m[1] * x + m[5] * y + m[9] * z + m[13]) / cw;
      const cz = (m[2] * x + m[6] * y + m[10] * z + m[14]) / cw;
      return [(cx * 0.5 + 0.5) * width, (1 - (cy * 0.5 + 0.5)) * height, cz];
    };
    let best: PreviewHit | null = null;
    let bestDepth = Infinity;
    const groupOf = (groups: PreviewGroup[] | undefined, v: number): number | null => {
      for (const g of groups ?? []) if (v >= g.first && v < g.first + g.count) return g.id;
      return null;
    };
    const point = (p: ArrayLike<number>, v: number): [number, number, number] => [p[v * 3], p[v * 3 + 1], p[v * 3 + 2]];
    const count = this._valid.length;
    const TOLERANCE = 5;

    const segment = (p: ArrayLike<number>, a: number, b: number): number | null => {
      const pa = project(p, a);
      const pb = project(p, b);
      if (!pa || !pb) return null;
      const dx = pb[0] - pa[0], dy = pb[1] - pa[1];
      const length2 = dx * dx + dy * dy;
      const t = length2 > 0 ? Math.max(0, Math.min(1, ((px - pa[0]) * dx + (py - pa[1]) * dy) / length2)) : 0;
      const qx = pa[0] + dx * t, qy = pa[1] + dy * t;
      if (Math.hypot(px - qx, py - qy) > TOLERANCE) return null;
      return pa[2] + (pb[2] - pa[2]) * t;
    };

    if (this._shade === "points" || this._kind === "points") {
      for (let v = 0; v < count; v++) {
        if (!this._valid[v] || this._hiddenVertex[v]) continue;
        const s = project(this._drawn, v);
        if (!s || Math.hypot(s[0] - px, s[1] - py) > TOLERANCE + 2) continue;
        if (s[2] < bestDepth) {
          bestDepth = s[2];
          best = { overlay: -1, primitive: Math.floor(v / this._perPrimitive()), vertex: v - (v % this._perPrimitive()), group: groupOf(this._mesh?.groups, v), point: point(this._drawn, v) };
        }
      }
    } else if (this._kind === "triangles") {
      for (let v = 0; v + 2 < count; v += 3) {
        if (!this._valid[v] || !this._valid[v + 1] || !this._valid[v + 2] || this._hiddenVertex[v]) continue;
        const a = project(this._drawn, v), b = project(this._drawn, v + 1), c = project(this._drawn, v + 2);
        if (!a || !b || !c) continue;
        const area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        if (Math.abs(area) < 1e-12) continue;
        const w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / area;
        const w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / area;
        const w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const depth = w0 * a[2] + w1 * b[2] + w2 * c[2];
        if (depth < bestDepth && depth >= -1) {
          bestDepth = depth;
          const p = this._drawn;
          best = {
            overlay: -1, primitive: v / 3, vertex: v, group: groupOf(this._mesh?.groups, v),
            point: [0, 1, 2].map((k) => w0 * p[v * 3 + k] + w1 * p[(v + 1) * 3 + k] + w2 * p[(v + 2) * 3 + k]) as [number, number, number],
          };
        }
      }
    } else {
      for (let v = 0; v + 1 < count; v += 2) {
        if (!this._valid[v] || !this._valid[v + 1] || this._hiddenVertex[v]) continue;
        const depth = segment(this._drawn, v, v + 1);
        if (depth !== null && depth < bestDepth) {
          bestDepth = depth;
          best = { overlay: -1, primitive: v / 2, vertex: v, group: groupOf(this._mesh?.groups, v), point: point(this._drawn, v) };
        }
      }
    }
    this._overlays.forEach((o, index) => {
      if (!o.source.pickable) return;
      const p = o.source.positions;
      for (let v = 0; v + 1 < p.length / 3; v += 2) {
        if (o.hidden[v]) continue;
        const depth = segment(p, v, v + 1);
        if (depth !== null && depth < bestDepth) {
          bestDepth = depth;
          best = { overlay: index, primitive: v / 2, vertex: v, group: groupOf(o.source.groups, v), point: point(p, v) };
        }
      }
    });
    return best;
  }

  /** Turns the view. The arcball swings the eye around the model; the fly camera turns on the spot. */
  private _look(dx: number, dy: number): void {
    this._yaw += dx * 0.01 * (this._mode === "fly" ? -1 : 1);
    this._pitch = Math.max(-PITCH_LIMIT, Math.min(PITCH_LIMIT, this._pitch + dy * 0.01 * (this._mode === "fly" ? -1 : 1)));
  }

  /** Arcball: slides the point the eye orbits across the screen, at the distance it is watching from. */
  private _slide(dx: number, dy: number): void {
    const forward = this._forward();
    const right = norm(cross(forward, [0, 1, 0]));
    const up = cross(right, forward);
    // A pixel is worth this much of the world at the distance being watched from.
    const perPixel = (2 * this._distance * Math.tan(FOV / 2)) / Math.max(1, this.root.element.clientHeight);
    this._center = add(this._center, add(scale(right, -dx * perPixel), scale(up, dy * perPixel)));
  }

  private _bindKeys(): void {
    const canvas = this._canvas;
    canvas.onkeydown = (e) => {
      // Camera bookmarks, in either mode.
      if (/^[1-9]$/.test(e.key) && !e.altKey && !e.metaKey) {
        e.preventDefault();
        this.onBookmarkKey?.(Number(e.key), e.ctrlKey);
        return;
      }
      if (e.key === "f" && !e.ctrlKey && this._mode !== "fly") {
        // F frames the selection, as in most 3D tools (fly mode keeps its keys for walking).
        e.preventDefault();
        this.zoomToSelection();
        return;
      }
      if (this._mode !== "fly") return;
      const key = e.key.toLowerCase();
      if (!"wasdqe".includes(key) && key !== "shift") return;
      e.preventDefault();
      this._held.add(key);
      this._startWalking();
    };
    canvas.onkeyup = (e) => {
      this._held.delete(e.key.toLowerCase());
    };
    // Keys held when the view loses focus would otherwise walk for ever.
    canvas.onblur = () => this._held.clear();
  }

  private _startWalking(): void {
    if (this._walking) return;
    this._lastStep = performance.now();
    const step = (now: number): void => {
      const seconds = Math.min(0.1, (now - this._lastStep) / 1000);
      this._lastStep = now;
      if (this._mode !== "fly" || !this._held.size || (this._held.size === 1 && this._held.has("shift"))) {
        this._walking = 0;
        return;
      }
      const forward = this._forward();
      const right = norm(cross(forward, [0, 1, 0]));
      const pace = this._speed * seconds * (this._held.has("shift") ? 5 : 1);
      let move: Vec3 = [0, 0, 0];
      if (this._held.has("w")) move = add(move, forward);
      if (this._held.has("s")) move = sub(move, forward);
      if (this._held.has("d")) move = add(move, right);
      if (this._held.has("a")) move = sub(move, right);
      if (this._held.has("e")) move = add(move, [0, 1, 0]);
      if (this._held.has("q")) move = sub(move, [0, 1, 0]);
      const length = Math.hypot(move[0], move[1], move[2]);
      if (length > 0) this._eye = add(this._eye, scale(scale(move, 1 / length), pace));
      this._schedule();
      this._walking = requestAnimationFrame(step);
    };
    this._walking = requestAnimationFrame(step);
  }

  private _schedule(): void {
    if (this._frame) return;
    this._frame = requestAnimationFrame(() => {
      this._frame = 0;
      this._draw();
    });
  }

  /** Points attribute 1 or 2 at a buffer, or holds it at one value for every vertex. */
  private _attribute(location: number, buffer: WebGLBuffer | null, constant: Vec3): void {
    const gl = this._gl!;
    if (buffer) {
      gl.enableVertexAttribArray(location);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.vertexAttribPointer(location, 3, gl.FLOAT, false, 0, 0);
    } else {
      gl.disableVertexAttribArray(location);
      gl.vertexAttrib3f(location, constant[0], constant[1], constant[2]);
    }
  }

  private _draw(): void {
    const gl = this._gl;
    const program = this._program;
    if (!gl || !program) return;
    const u = this._uniforms;
    const pixel = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.root.element.clientWidth * pixel));
    const height = Math.max(1, Math.round(this.root.element.clientHeight * pixel));
    if (this._canvas.width !== width || this._canvas.height !== height) {
      this._canvas.width = width;
      this._canvas.height = height;
    }
    gl.viewport(0, 0, width, height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    gl.useProgram(program);

    const eye = this._mode === "fly" ? this._eye : this._orbitEye();
    const target = this._mode === "fly" ? add(eye, this._forward()) : this._center;
    const view = lookAt(eye, target, [0, 1, 0]);
    // Fly walks anywhere, so the depth range follows the eye rather than the model's radius.
    const span = this._mode === "fly" ? Math.max(this._fullRadius * 4, Math.hypot(...sub(eye, this._center)) + this._fullRadius * 2)
                                      : this._distance + this._fullRadius * 3;
    const near = Math.max(span * 1e-5, this._mode === "fly" ? this._radius * 1e-3 : Math.min(this._distance * 0.1, this._distance - this._radius * 3));
    const projection = perspective(FOV, width / height, Math.max(1e-6, near), Math.max(near * 1.001, span));
    const transform = multiply(projection, view);
    this._transform = transform;
    gl.uniformMatrix4fv(u.transform, false, transform);
    gl.uniformMatrix4fv(u.view, false, view);
    gl.uniform1i(u.lighting, 0);
    gl.uniform1i(u.useVertexColor, 0);
    gl.enableVertexAttribArray(0);
    this._attribute(1, null, [1, 1, 1]);
    this._attribute(2, null, [0, 0, 1]);

    const lines = (buffer: WebGLBuffer | null, count: number, rgb: [number, number, number]): void => {
      if (!count) return;
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.uniform4f(u.color, rgb[0], rgb[1], rgb[2], 1);
      gl.drawArrays(gl.LINES, 0, count);
    };

    if (this._clip) lines(this._volumeBuffer, VOLUME.length / 3, [0.55, 0.55, 0.6]);

    gl.bindBuffer(gl.ARRAY_BUFFER, this._vertexBuffer);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    const colored = this._colorSource >= 0;
    if (colored) this._attribute(1, this._colorBuffer, [1, 1, 1]);

    // The filled pass, pushed back so a wireframe over it does not fight with it for the depth test.
    const mode = SHADE_MODES.find((m) => m.value === this._shade);
    const fill = !!mode?.fills && this.canFill;
    if (fill) {
      const attribute = this._normalAttribute();
      const lighting = this._shade === "flat" ? (attribute && this._normals ? 2 : 1)
                     : this._shade === "smooth" ? (this._normals ? 3 : 1) : 0;
      if (lighting >= 2) this._attribute(2, this._normalBuffer, [0, 0, 1]);
      gl.enable(gl.POLYGON_OFFSET_FILL);
      gl.polygonOffset(1, 1);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._triangleBuffer);
      gl.uniform1i(u.lighting, lighting);
      gl.uniform1i(u.useVertexColor, colored ? 1 : 0);
      if (lighting) gl.uniform4f(u.color, 0.62, 0.72, 0.85, 1);
      else gl.uniform4f(u.color, 0.18, 0.34, 0.5, 1);
      gl.drawElements(gl.TRIANGLES, this._filled, gl.UNSIGNED_INT, 0);
      gl.uniform1i(u.lighting, 0);
      gl.disable(gl.POLYGON_OFFSET_FILL);
      this._attribute(2, null, [0, 0, 1]);
    }

    gl.uniform1f(u.pointSize, 3 * pixel);
    const pointsOnly = this._shade === "points" || this._kind === "points";
    // Edges over a fill take the one colour, so they read against whatever colours the faces;
    // on their own they take the vertices' colours when there are any.
    gl.uniform1i(u.useVertexColor, colored && !fill ? 1 : 0);
    gl.uniform4f(u.color, 0.3, 0.65, 1, 1);
    if (pointsOnly) {
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._pointBuffer);
      if (this._points) gl.drawElements(gl.POINTS, this._points, gl.UNSIGNED_INT, 0);
    } else if (!fill || this._shade === "wire-solid") {
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._edgeBuffer);
      if (this._edges) gl.drawElements(gl.LINES, this._edges, gl.UNSIGNED_INT, 0);
    }
    gl.uniform1i(u.useVertexColor, 0);
    this._attribute(1, null, [1, 1, 1]);

    for (const o of this._overlays) {
      if (!o.count || !o.indices) continue;
      gl.bindBuffer(gl.ARRAY_BUFFER, o.vertices);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      if (o.colors) {
        this._attribute(1, o.colors, [1, 1, 1]);
        gl.uniform1i(u.useVertexColor, 1);
      }
      gl.uniform4f(u.color, o.source.color[0], o.source.color[1], o.source.color[2], 1);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, o.indices);
      gl.drawElements(gl.LINES, o.count, gl.UNSIGNED_INT, 0);
      gl.uniform1i(u.useVertexColor, 0);
      this._attribute(1, null, [1, 1, 1]);
    }

    if (this._showNormals) lines(this._normalLinesBuffer, this._normalLines, [0.95, 0.8, 0.25]);

    // What is selected, over everything: it has to be findable wherever it is.
    if (this._selectionEdges) {
      gl.disable(gl.DEPTH_TEST);
      lines(this._selectionBuffer, this._selectionEdges, [1, 0.6, 0.1]);
      gl.enable(gl.DEPTH_TEST);
    }

    if (this._highlight !== null) {
      const v = this._highlight;
      gl.disable(gl.DEPTH_TEST);
      gl.bindBuffer(gl.ARRAY_BUFFER, this._scratchBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, this._drawn.subarray(v * 3, v * 3 + 3), gl.DYNAMIC_DRAW);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.uniform4f(u.color, 1, 0.6, 0.1, 1);
      gl.uniform1f(u.pointSize, 9 * pixel);
      gl.drawArrays(gl.POINTS, 0, 1);
    }
  }
}
