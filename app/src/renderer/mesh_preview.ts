// The mesh view's 3D preview: a draw's primitives as a wireframe, turned with the mouse. VS In is drawn
// in the space its positions are in; VS Out in normalized device coordinates (clip space divided by
// w, y up the way the render target shows it) inside the outline of Vulkan's view volume, so what
// falls outside the volume is plain to see. WebGL2, so meshes of a million vertices still turn.
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

type Mat4 = Float32Array;

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

function lookAt(eye: number[], target: number[], up: number[]): Mat4 {
  const sub = (a: number[], b: number[]): number[] => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const norm = (v: number[]): number[] => { const l = Math.hypot(v[0], v[1], v[2]) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
  const cross = (a: number[], b: number[]): number[] => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const dot = (a: number[], b: number[]): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const z = norm(sub(eye, target));
  const x = norm(cross(up, z));
  const y = cross(z, x);
  return new Float32Array([x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1]);
}

/** The preview's vertical field of view. */
const FOV = Math.PI / 6;

const VERTEX = `#version 300 es
in vec3 position;
uniform mat4 transform;
uniform float pointSize;
void main() {
  gl_Position = transform * vec4(position, 1.0);
  gl_PointSize = pointSize;
}`;

const FRAGMENT = `#version 300 es
precision mediump float;
uniform vec4 color;
out vec4 outColor;
void main() { outColor = color; }`;

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
  private _volumeBuffer: WebGLBuffer | null = null;
  private _highlightBuffer: WebGLBuffer | null = null;
  private _edges = 0;
  private _points = 0;
  private _clip = false;
  private _kind: PrimitiveKind = "triangles";
  /** The drawn positions (x y z), for the highlighted vertex. */
  private _drawn = new Float32Array(0);
  private _highlight: number | null = null;
  private _center = [0, 0, 0];
  private _radius = 1;
  /** The largest half-extent of the framed box: what the view is fitted to (the radius is its corner). */
  private _extent = 1;
  private _yaw = 0;
  private _pitch = 0;
  private _distance = 3;
  private _frame = 0;
  private readonly _resize: ResizeObserver;

  constructor(parent: Widget) {
    this.root = new Div(parent, { class: "mesh-preview" });
    this._canvas = document.createElement("canvas");
    this._canvas.className = "mesh-preview-canvas";
    this._canvas.title = "Drag to turn, wheel to zoom, double-click to reset the view";
    this.root.element.appendChild(this._canvas);
    this._gl = this._canvas.getContext("webgl2", { antialias: true, alpha: true, preserveDrawingBuffer: true });
    if (!this._gl || !this._init(this._gl)) {
      new Div(this.root, { text: "This window has no WebGL2, which the mesh preview draws with.", class: "text-muted mesh-preview-note" });
    }
    this._bindMouse();
    this._resize = new ResizeObserver(() => this._schedule());
    this._resize.observe(this.root.element);
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

    // Edges of every primitive whose vertices are all drawable.
    const per = this._kind === "triangles" ? 3 : this._kind === "lines" ? 2 : 1;
    const edges: number[] = [];
    for (let i = 0; i + per <= count; i += per) {
      let ok = true;
      for (let k = 0; k < per; k++) ok = ok && valid[i + k] === 1;
      if (!ok) continue;
      if (per === 3) edges.push(i, i + 1, i + 1, i + 2, i + 2, i);
      else if (per === 2) edges.push(i, i + 1);
      else edges.push(i);
    }
    this._edges = per === 1 ? 0 : edges.length;
    this._points = per === 1 ? edges.length : 0;

    if (!keepView) {
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
      }
      this._center = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2];
      this._radius = Math.max(1e-6, Math.hypot(max[0] - min[0], max[1] - min[1], max[2] - min[2]) / 2);
      this._extent = Math.max(1e-6, (max[0] - min[0]) / 2, (max[1] - min[1]) / 2, (max[2] - min[2]) / 2);
      this.resetView();
    }
    if (gl && this._program) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this._vertexBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, drawn, gl.STATIC_DRAW);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._edgeBuffer);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint32Array(edges), gl.STATIC_DRAW);
    }
    this._schedule();
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
    this._schedule();
  }

  dispose(): void {
    this._resize.disconnect();
    if (this._frame) cancelAnimationFrame(this._frame);
  }

  debugState(): Record<string, unknown> {
    return { webgl: !!this._program, edges: this._edges / 2, points: this._points, clip: this._clip };
  }

  // ---------------------------------------------------------------------------------------

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
      let x = e.clientX;
      let y = e.clientY;
      const move = (m: MouseEvent): void => {
        this._yaw += (m.clientX - x) * 0.01;
        this._pitch = Math.max(-1.55, Math.min(1.55, this._pitch + (m.clientY - y) * 0.01));
        x = m.clientX;
        y = m.clientY;
        this._schedule();
      };
      const up = (): void => {
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", up);
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", up);
    };
    canvas.onwheel = (e) => {
      e.preventDefault();
      this._distance = Math.max(this._radius * 0.05, Math.min(this._radius * 50, this._distance * Math.exp(e.deltaY * 0.001)));
      this._schedule();
    };
    canvas.ondblclick = () => this.resetView();
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

    // The eye orbits the mesh's centre, from +z at rest.
    const c = this._center;
    const d = this._distance;
    const eye = [
      c[0] + d * Math.sin(this._yaw) * Math.cos(this._pitch),
      c[1] + d * Math.sin(this._pitch),
      c[2] + d * Math.cos(this._yaw) * Math.cos(this._pitch),
    ];
    const view = lookAt(eye, c, [0, 1, 0]);
    const projection = perspective(FOV, width / height, Math.max(d * 0.01, d - this._radius * 3), d + this._radius * 3);
    const transform = multiply(projection, view);
    gl.uniformMatrix4fv(gl.getUniformLocation(program, "transform"), false, transform);
    const color = gl.getUniformLocation(program, "color");
    const pointSize = gl.getUniformLocation(program, "pointSize");
    gl.enableVertexAttribArray(0);

    if (this._clip) {
      gl.bindBuffer(gl.ARRAY_BUFFER, this._volumeBuffer);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.uniform4f(color, 0.55, 0.55, 0.6, 1);
      gl.drawArrays(gl.LINES, 0, VOLUME.length / 3);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, this._vertexBuffer);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this._edgeBuffer);
    gl.uniform4f(color, 0.3, 0.65, 1, 1);
    gl.uniform1f(pointSize, 3 * scale);
    if (this._edges) gl.drawElements(gl.LINES, this._edges, gl.UNSIGNED_INT, 0);
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
