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
// And three shading modes. A wireframe says where every primitive is and hides nothing behind
// another; a solid one says what the surface is; a flat-shaded one says which way each face points,
// which is what you look at when the normals or the winding are suspect. The flat normal comes from
// the derivatives of the view-space position rather than from a normal buffer, so it costs the
// caller nothing and is the face's own normal by construction.
import { Div } from "./widget/div.js";
import type { Widget } from "./widget/widget.js";
import type { PrimitiveKind } from "./mesh_output.js";

export interface PreviewMesh {
  /** One position per vertex, as a list of primitives: x y z, or x y z w with `clip`. */
  positions: Float32Array;
  kind: PrimitiveKind;
  /** Clip-space positions, drawn divided by w with the view volume outlined. */
  clip: boolean;
}

/** How the eye moves. */
export type CameraMode = "arcball" | "fly";

/** What the primitives are drawn as. Only triangles can be filled; anything else stays a wireframe. */
export type ShadeMode = "wireframe" | "solid" | "flat";

export const SHADE_MODES: { value: ShadeMode; label: string; tooltip: string }[] = [
  { value: "wireframe", label: "Wireframe", tooltip: "Every primitive's edges, with nothing hidden behind anything else" },
  { value: "solid", label: "Solid", tooltip: "Filled triangles in one colour, with their edges over them" },
  { value: "flat", label: "Flat", tooltip: "Filled triangles lit by the face's own normal, which is what shows a wrong winding or a fold" },
];

export const CAMERA_MODES: { value: CameraMode; label: string; tooltip: string }[] = [
  { value: "arcball", label: "Arcball", tooltip: "Drag to turn around the model, middle or shift drag to slide it, wheel to come closer" },
  { value: "fly", label: "Fly", tooltip: "Drag to look, WASD to walk, Q and E to rise and fall, shift to hurry, wheel to change speed" },
];

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

/** The preview's vertical field of view. */
const FOV = Math.PI / 6;
/** How far the pitch may go before the up vector and the view line up. */
const PITCH_LIMIT = 1.55;

const VERTEX = `#version 300 es
in vec3 position;
uniform mat4 transform;
uniform mat4 view;
uniform float pointSize;
out vec3 viewPosition;
void main() {
  viewPosition = (view * vec4(position, 1.0)).xyz;
  gl_Position = transform * vec4(position, 1.0);
  gl_PointSize = pointSize;
}`;

// `lit` shades by the face's own normal, which the derivatives of the view-space position give
// exactly: a triangle is planar, so its screen-space gradient is constant across it.
const FRAGMENT = `#version 300 es
precision highp float;
uniform vec4 color;
uniform bool lit;
in vec3 viewPosition;
out vec4 outColor;
void main() {
  if (!lit) {
    outColor = color;
    return;
  }
  vec3 normal = normalize(cross(dFdx(viewPosition), dFdy(viewPosition)));
  // A headlight at the eye, and both sides lit: a back face is worth seeing, not worth hiding.
  float facing = abs(normal.z);
  outColor = vec4(color.rgb * (0.25 + 0.75 * facing), color.a);
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

export class MeshPreview {
  readonly root: Div;
  private readonly _canvas: HTMLCanvasElement;
  private readonly _gl: WebGL2RenderingContext | null;
  private _program: WebGLProgram | null = null;
  private _vertexBuffer: WebGLBuffer | null = null;
  private _edgeBuffer: WebGLBuffer | null = null;
  private _triangleBuffer: WebGLBuffer | null = null;
  private _volumeBuffer: WebGLBuffer | null = null;
  private _highlightBuffer: WebGLBuffer | null = null;
  private _edges = 0;
  private _points = 0;
  private _filled = 0;
  private _clip = false;
  private _kind: PrimitiveKind = "triangles";
  /** The drawn positions (x y z), for the highlighted vertex. */
  private _drawn = new Float32Array(0);
  private _highlight: number | null = null;
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
  private readonly _resize: ResizeObserver;
  /** Told when the camera or a mode changes, so a toolbar can follow. */
  onModeChange: (() => void) | null = null;

  constructor(parent: Widget) {
    this.root = new Div(parent, { class: "mesh-preview" });
    this._canvas = document.createElement("canvas");
    this._canvas.className = "mesh-preview-canvas";
    // A canvas only takes key events when it can hold focus.
    this._canvas.tabIndex = 0;
    this.root.element.appendChild(this._canvas);
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
    this._schedule();
    this.onModeChange?.();
  }

  /** Shows a mesh; `keepView` keeps the camera (stepping between draws of one pass). */
  setMesh(mesh: PreviewMesh | null, keepView = false): void {
    const gl = this._gl;
    this._highlight = null;
    this._clip = mesh?.clip ?? false;
    this._kind = mesh?.kind ?? "triangles";
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
    this._drawn = drawn;

    // Edges of every primitive whose vertices are all drawable, and the triangles themselves for
    // the filled modes.
    const per = this._kind === "triangles" ? 3 : this._kind === "lines" ? 2 : 1;
    const edges: number[] = [];
    const triangles: number[] = [];
    for (let i = 0; i + per <= count; i += per) {
      let ok = true;
      for (let k = 0; k < per; k++) ok = ok && valid[i + k] === 1;
      if (!ok) continue;
      if (per === 3) {
        edges.push(i, i + 1, i + 1, i + 2, i + 2, i);
        triangles.push(i, i + 1, i + 2);
      } else if (per === 2) edges.push(i, i + 1);
      else edges.push(i);
    }
    this._edges = per === 1 ? 0 : edges.length;
    this._points = per === 1 ? edges.length : 0;
    this._filled = triangles.length;

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
      this.resetView();
    }
    if (gl && this._program) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this._vertexBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, drawn, gl.STATIC_DRAW);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._edgeBuffer);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(edges), gl.STATIC_DRAW);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._triangleBuffer);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(triangles), gl.STATIC_DRAW);
    }
    this._schedule();
    this.onModeChange?.();
  }

  /** Marks one vertex (an index into the list the mesh was given), or none. */
  highlight(vertex: number | null): void {
    this._highlight = vertex !== null && vertex < this._drawn.length / 3 ? vertex : null;
    this._schedule();
  }

  resetView(): void {
    // VS Out faces the render target; VS In is turned a little so its depth shows.
    this._yaw = this._clip ? 0 : 0.6;
    this._pitch = this._clip ? 0 : 0.35;
    // Far enough for the mesh to fill most of a 30 degree view: flatter than 45, so the volume reads as a box.
    this._distance = this._extent * 1.25 / Math.sin(FOV / 2);
    this._eye = this._orbitEye();
    // A walk crosses the scene in a few seconds, whatever scale it is in.
    this._speed = Math.max(1e-6, this._radius);
    this._schedule();
    this.onModeChange?.();
  }

  dispose(): void {
    this._resize.disconnect();
    if (this._frame) cancelAnimationFrame(this._frame);
    if (this._walking) cancelAnimationFrame(this._walking);
    this._held.clear();
  }

  debugState(): Record<string, unknown> {
    return {
      webgl: !!this._program, edges: this._edges / 2, points: this._points, triangles: this._filled / 3,
      clip: this._clip, camera: this._mode, shading: this._shade, canFill: this.canFill,
    };
  }

  // ---------------------------------------------------------------------------------------

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
      ? "Drag to look, WASD to walk, Q and E to rise and fall, shift to hurry, wheel to change speed, double-click to reset"
      : "Drag to turn, middle or shift drag to slide, wheel to zoom, double-click to reset the view";
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
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) return false;
    this._program = program;
    this._vertexBuffer = gl.createBuffer();
    this._edgeBuffer = gl.createBuffer();
    this._triangleBuffer = gl.createBuffer();
    this._volumeBuffer = gl.createBuffer();
    this._highlightBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this._volumeBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, VOLUME, gl.STATIC_DRAW);
    return true;
  }

  private _bindMouse(): void {
    const canvas = this._canvas;
    canvas.onmousedown = (e) => {
      e.preventDefault();
      canvas.focus();
      let x = e.clientX;
      let y = e.clientY;
      // Arcball: the middle button, the right button or shift slides the model instead of turning it.
      const sliding = this._mode === "arcball" && (e.button === 1 || e.button === 2 || e.shiftKey);
      const move = (m: MouseEvent): void => {
        const dx = m.clientX - x;
        const dy = m.clientY - y;
        x = m.clientX;
        y = m.clientY;
        if (sliding) this._slide(dx, dy);
        else this._look(dx, dy);
        this._schedule();
      };
      const up = (): void => {
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", up);
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", up);
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
      this._distance = Math.max(this._radius * 0.05, Math.min(Math.max(this._radius * 50, this._fullRadius * 4), this._distance * Math.exp(e.deltaY * 0.001)));
      this._schedule();
    };
    canvas.ondblclick = () => this.resetView();
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

  private _draw(): void {
    const gl = this._gl;
    const program = this._program;
    if (!gl || !program) return;
    const scale = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(this.root.element.clientWidth * scale));
    const height = Math.max(1, Math.round(this.root.element.clientHeight * scale));
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
    const near = Math.max(span * 1e-4, this._mode === "fly" ? this._radius * 1e-3 : this._distance - this._radius * 3);
    const projection = perspective(FOV, width / height, Math.max(1e-6, near), Math.max(near * 1.001, span));
    const transform = multiply(projection, view);
    gl.uniformMatrix4fv(gl.getUniformLocation(program, "transform"), false, transform);
    gl.uniformMatrix4fv(gl.getUniformLocation(program, "view"), false, view);
    const color = gl.getUniformLocation(program, "color");
    const lit = gl.getUniformLocation(program, "lit");
    const pointSize = gl.getUniformLocation(program, "pointSize");
    gl.uniform1i(lit, 0);
    gl.enableVertexAttribArray(0);

    if (this._clip) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this._volumeBuffer);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.uniform4f(color, 0.55, 0.55, 0.6, 1);
      gl.drawArrays(gl.LINES, 0, VOLUME.length / 3);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, this._vertexBuffer);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);

    // The filled pass, pushed back so the wireframe over it does not fight with it for the depth test.
    const fill = this._shade !== "wireframe" && this.canFill;
    if (fill) {
      gl.enable(gl.POLYGON_OFFSET_FILL);
      gl.polygonOffset(1, 1);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._triangleBuffer);
      gl.uniform1i(lit, this._shade === "flat" ? 1 : 0);
      if (this._shade === "flat") gl.uniform4f(color, 0.62, 0.72, 0.85, 1);
      else gl.uniform4f(color, 0.18, 0.34, 0.5, 1);
      gl.drawElements(gl.TRIANGLES, this._filled, gl.UNSIGNED_INT, 0);
      gl.uniform1i(lit, 0);
      gl.disable(gl.POLYGON_OFFSET_FILL);
    }

    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._edgeBuffer);
    // Solid keeps its edges, which is what says where one triangle ends and the next begins; flat
    // shading says that with its own shading and reads better without them.
    const wireframe = this._shade !== "flat" || !fill;
    gl.uniform4f(color, 0.3, 0.65, 1, 1);
    gl.uniform1f(pointSize, 3 * scale);
    if (wireframe && this._edges) gl.drawElements(gl.LINES, this._edges, gl.UNSIGNED_INT, 0);
    if (this._points) gl.drawElements(gl.POINTS, this._points, gl.UNSIGNED_INT, 0);

    if (this._highlight !== null) {
      const v = this._highlight;
      gl.disable(gl.DEPTH_TEST);
      gl.bindBuffer(gl.ARRAY_BUFFER, this._highlightBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, this._drawn.subarray(v * 3, v * 3 + 3), gl.DYNAMIC_DRAW);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.uniform4f(color, 1, 0.6, 0.1, 1);
      gl.uniform1f(pointSize, 9 * scale);
      gl.drawArrays(gl.POINTS, 0, 1);
    }
  }
}
