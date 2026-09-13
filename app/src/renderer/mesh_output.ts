// A draw's mesh after its vertex shader (RenderDoc's VS Out): what `vkinsp_replay --mesh-data` writes
// (replay/src/mesh.cpp), and what can be said about it at a glance. The replay captures every vertex
// the draw assembled with transform feedback: an indexed draw's vertices in index order, strips and
// fans as lists, every instance after the first.

/** One output the vertex shader wrote, at `offset` in each vertex's record. */
export interface MeshOutputVariable {
  name: string;
  offset: number;
  /** Scalars in it, four bytes each. */
  components: number;
  base: "float" | "int" | "uint";
  /** "Position" for gl_Position. */
  builtin?: string;
  location?: number;
}

export interface MeshOutput {
  command: number;
  method: string;
  frame: number;
  commandBuffer: number;
  passIndex: number;
  /** The replay captured it; false with a note saying why not. */
  measured: boolean;
  /** The pipeline's topology (VK_PRIMITIVE_TOPOLOGY_*), " (dynamic)" when the draw may have set another. */
  topology: string;
  stride: number;
  vertices: number;
  /** The draw wrote more vertices than were captured. */
  truncated: boolean;
  outputs: MeshOutputVariable[];
  note?: string;
  /** `vertices` records of `stride` bytes. */
  data: Uint8Array | null;
}

export interface MeshFile {
  device: string;
  draws: MeshOutput[];
  problems: string[];
}

export const MESH_MAGIC = "MESH 1\n";

/** Parses `vkinsp_replay --mesh-data` (WriteMeshData in replay/src/main.cpp). */
export function parseMeshFile(bytes: Uint8Array): MeshFile {
  const magic = new TextEncoder().encode(MESH_MAGIC);
  if (bytes.byteLength < magic.byteLength + 4 || magic.some((b, i) => bytes[i] !== b)) throw new Error("Not a mesh output file from vkinsp_replay.");
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(magic.byteLength, true);
  const start = magic.byteLength + 4;
  const base = start + length;
  if (base > bytes.byteLength) throw new Error("The mesh output file is truncated.");
  const manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(start, base))) as {
    device?: string;
    draws?: (Omit<MeshOutput, "data"> & { payload?: [number, number] })[];
    problems?: string[];
  };
  const draws = (manifest.draws ?? []).map(({ payload, ...info }): MeshOutput => {
    let data: Uint8Array | null = null;
    if (payload) {
      const [offset, size] = payload;
      if (base + offset + size > bytes.byteLength) throw new Error("The mesh output file is truncated (vertices out of range).");
      data = bytes.slice(base + offset, base + offset + size);
    }
    return { ...info, outputs: info.outputs ?? [], data };
  });
  return { device: manifest.device ?? "", draws, problems: manifest.problems ?? [] };
}

/** What a topology assembles, as the preview draws it. */
export type PrimitiveKind = "triangles" | "lines" | "points";

export function primitiveKind(topology: string): PrimitiveKind {
  if (/POINT/.test(topology)) return "points";
  if (/LINE/.test(topology)) return "lines";
  return "triangles";
}

export function verticesPerPrimitive(kind: PrimitiveKind): number {
  return kind === "triangles" ? 3 : kind === "lines" ? 2 : 1;
}

/** The output holding the clip-space position, if the shader writes one the replay could capture. */
export function positionOutput(m: MeshOutput): MeshOutputVariable | null {
  return m.outputs.find((o) => o.builtin === "Position" && o.base === "float" && o.components === 4) ?? null;
}

/** One output's scalars at a vertex. */
export function outputValues(m: MeshOutput, output: MeshOutputVariable, vertex: number): number[] {
  if (!m.data) return [];
  const view = new DataView(m.data.buffer, m.data.byteOffset, m.data.byteLength);
  const at = vertex * m.stride + output.offset;
  if (at + output.components * 4 > m.data.byteLength) return [];
  const out: number[] = [];
  for (let k = 0; k < output.components; k++) {
    const o = at + k * 4;
    out.push(output.base === "float" ? view.getFloat32(o, true) : output.base === "int" ? view.getInt32(o, true) : view.getUint32(o, true));
  }
  return out;
}

/** Every vertex's clip-space position, x y z w, or null without a position output. */
export function clipPositions(m: MeshOutput): Float32Array | null {
  const p = positionOutput(m);
  if (!p || !m.data) return null;
  const out = new Float32Array(m.vertices * 4);
  const view = new DataView(m.data.buffer, m.data.byteOffset, m.data.byteLength);
  for (let v = 0; v < m.vertices; v++) {
    const at = v * m.stride + p.offset;
    if (at + 16 > m.data.byteLength) break;
    for (let k = 0; k < 4; k++) out[v * 4 + k] = view.getFloat32(at + k * 4, true);
  }
  return out;
}

/** What the draw's geometry looks like after the vertex shader, in the terms of "why can I not see it". */
export interface ClipStats {
  vertices: number;
  primitives: number;
  /** Vertices behind the eye (w <= 0), which the rasterizer clips. */
  behind: number;
  /** Vertices with a NaN or infinite position. */
  invalid: number;
  /** Primitives entirely outside one plane of the view volume: nothing of them can be seen. */
  outside: number;
  /** Triangles in front of the eye with no area on screen. */
  degenerate: number;
  /** The normalized device coordinates the vertices in front of the eye span; null when none are. */
  ndc: { min: [number, number, number]; max: [number, number, number] } | null;
}

export function clipStats(m: MeshOutput): ClipStats | null {
  const clip = clipPositions(m);
  if (!clip) return null;
  const kind = primitiveKind(m.topology);
  const per = verticesPerPrimitive(kind);
  const stats: ClipStats = { vertices: m.vertices, primitives: Math.floor(m.vertices / per), behind: 0, invalid: 0, outside: 0, degenerate: 0, ndc: null };
  const min: [number, number, number] = [Infinity, Infinity, Infinity];
  const max: [number, number, number] = [-Infinity, -Infinity, -Infinity];
  for (let v = 0; v < m.vertices; v++) {
    const [x, y, z, w] = clip.subarray(v * 4, v * 4 + 4);
    if (![x, y, z, w].every(Number.isFinite)) {
      stats.invalid++;
      continue;
    }
    if (w <= 0) {
      stats.behind++;
      continue;
    }
    const ndc = [x / w, y / w, z / w];
    for (let k = 0; k < 3; k++) {
      min[k] = Math.min(min[k], ndc[k]);
      max[k] = Math.max(max[k], ndc[k]);
    }
  }
  if (min[0] <= max[0]) stats.ndc = { min, max };
  // Vulkan's view volume: -w <= x, y <= w and 0 <= z <= w.
  const planes: ((x: number, y: number, z: number, w: number) => boolean)[] = [
    (x, _y, _z, w) => x < -w, (x, _y, _z, w) => x > w, (_x, y, _z, w) => y < -w, (_x, y, _z, w) => y > w,
    (_x, _y, z) => z < 0, (_x, _y, z, w) => z > w,
  ];
  for (let p = 0; p < stats.primitives; p++) {
    const at = p * per;
    const vertex = (i: number): number[] => Array.from(clip.subarray((at + i) * 4, (at + i) * 4 + 4));
    const vs = Array.from({ length: per }, (_, i) => vertex(i));
    if (planes.some((out) => vs.every(([x, y, z, w]) => out(x, y, z, w)))) {
      stats.outside++;
      continue;
    }
    if (kind === "triangles" && vs.every(([, , , w]) => w > 0)) {
      const s = vs.map(([x, y, , w]) => [x / w, y / w]);
      const area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (s[1][1] - s[0][1]);
      if (Math.abs(area) < 1e-12) stats.degenerate++;
    }
  }
  return stats;
}

/** One line for the view's status: the counts that say whether the geometry can be seen. */
export function meshSummary(m: MeshOutput): string {
  if (!m.measured) return `Not captured: ${m.note ?? "the replay could not capture it"}`;
  const kind = primitiveKind(m.topology);
  const stats = clipStats(m);
  const parts = [`${m.vertices.toLocaleString()} vertices, ${Math.floor(m.vertices / verticesPerPrimitive(kind)).toLocaleString()} ${kind}`];
  if (m.truncated) parts.push("truncated");
  if (!stats) {
    parts.push("no gl_Position the replay could capture");
  } else {
    if (stats.outside) parts.push(`${stats.outside.toLocaleString()} outside the view`);
    if (stats.behind) parts.push(`${stats.behind.toLocaleString()} vertices behind the eye`);
    if (stats.degenerate) parts.push(`${stats.degenerate.toLocaleString()} with no area`);
    if (stats.invalid) parts.push(`${stats.invalid.toLocaleString()} NaN or infinite positions`);
  }
  return parts.join(", ");
}
