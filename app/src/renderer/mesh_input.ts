// A draw's mesh before its vertex shader (RenderDoc's VS In): the vertices it reads, decoded from the
// captured vertex and index buffers through the pipeline's vertex layout (draw_state.ts), in the order
// the draw reads them. The first instance only; a per-instance attribute takes its first element.
import type { CaptureData } from "./capture_data.js";
import { drawState, vertexLayout, type VertexLayout } from "./draw_state.js";
import { primitiveKind, verticesPerPrimitive } from "./mesh_output.js";
import { vertexFormat, type VertexFormat } from "./vulkan/vk_format.js";
import { isObject, num, str, type ObjectLookup } from "./vulkan/vulkan_object.js";
import type { CaptureCommand } from "../shared/protocol.js";

export interface MeshInputAttribute {
  name: string;
  location: number;
  binding: number;
  format: string;
  components: number;
  perInstance: boolean;
}

export interface MeshInput {
  topology: string;
  attributes: MeshInputAttribute[];
  /** The vertex each position in draw order reads (the index buffer applied, with its base vertex). */
  ids: number[];
  /** The index buffer's values in draw order, for an indexed draw. */
  indices: number[] | null;
  /** The attribute the preview draws as the position, or -1 when none looks like one. */
  position: number;
  /** Why something is missing: buffers not captured, an indirect draw's counts, a truncated range. */
  notes: string[];
  /** An attribute's values at a position in draw order; null where the capture does not reach. */
  values(order: number, attribute: number): number[] | null;
}

/** Vertices read at most: a draw with more is decoded up to here. */
export const MESH_INPUT_LIMIT = 1_000_000;

const INDIRECT_FIELDS: Record<string, string[]> = {
  vkCmdDrawIndirect: ["vertexCount", "instanceCount", "firstVertex", "firstInstance"],
  vkCmdDrawIndexedIndirect: ["indexCount", "instanceCount", "firstIndex", "vertexOffset", "firstInstance"],
};

function indexBytes(indexType: string): number {
  if (/UINT8/i.test(indexType)) return 1;
  if (/16/.test(indexType)) return 2;
  if (/32/.test(indexType)) return 4;
  return 0;
}

/** The draw's arguments, from its own arguments or the first entry of its captured indirect buffer. */
function drawArgs(data: CaptureData, cmd: CaptureCommand, notes: string[]): Record<string, number> {
  const a = cmd.args ?? {};
  const fields = INDIRECT_FIELDS[cmd.method];
  if (!fields) {
    return { vertexCount: num(a.vertexCount), indexCount: num(a.indexCount), firstVertex: num(a.firstVertex), firstIndex: num(a.firstIndex), vertexOffset: num(a.vertexOffset) };
  }
  const b = data.buffer(cmd.bufferData?.[0]);
  if (!b?.data || b.data.byteLength < fields.length * 4) {
    notes.push("An indirect draw's counts are in a buffer the capture did not read back.");
    return {};
  }
  const view = new DataView(b.data.buffer, b.data.byteOffset, b.data.byteLength);
  const out: Record<string, number> = {};
  fields.forEach((f, k) => { out[f] = f === "vertexOffset" ? view.getInt32(k * 4, true) : view.getUint32(k * 4, true); });
  if (num(a.drawCount) > 1) notes.push(`The first of the indirect draw's ${num(a.drawCount)} draws.`);
  return out;
}

export function meshInput(data: CaptureData, db: ObjectLookup, cmd: CaptureCommand, names: Map<number, string> = new Map()): MeshInput {
  const state = drawState(data, db, cmd);
  const notes: string[] = [];
  const d = state.pipeline?.descriptor;
  const assembly = d && isObject(d.pInputAssemblyState) ? d.pInputAssemblyState : null;
  const topology = assembly ? str(assembly.topology) : "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST";
  const args = drawArgs(data, cmd, notes);

  // Draw order: the index buffer's values, or the run of vertices from firstVertex.
  let ids: number[] = [];
  let indices: number[] | null = null;
  const indexed = /Indexed/.test(cmd.method);
  if (indexed) {
    const ib = state.indexBuffer;
    const captured = ib ? data.buffer(ib.dataId) : null;
    const size = ib ? indexBytes(ib.indexType) : 0;
    if (!captured?.data || !size) {
      notes.push("The index buffer was not captured.");
    } else {
      const view = new DataView(captured.data.buffer, captured.data.byteOffset, captured.data.byteLength);
      const count = Math.min(args.indexCount ?? 0, MESH_INPUT_LIMIT);
      const first = args.firstIndex ?? 0;
      indices = [];
      for (let i = first; i < first + count && (i + 1) * size <= captured.data.byteLength; i++) {
        indices.push(size === 1 ? view.getUint8(i * size) : size === 2 ? view.getUint16(i * size, true) : view.getUint32(i * size, true));
      }
      if (indices.length < count) notes.push(`The captured index buffer holds ${indices.length.toLocaleString()} of the draw's ${count.toLocaleString()} indices.`);
      ids = indices.map((v) => v + (args.vertexOffset ?? 0));
    }
  } else {
    const count = Math.min(args.vertexCount ?? 0, MESH_INPUT_LIMIT);
    ids = Array.from({ length: count }, (_, i) => (args.firstVertex ?? 0) + i);
  }

  // Every attribute of every bound binding, named from the vertex shader's inputs.
  const attributes: MeshInputAttribute[] = [];
  const readers: { layout: VertexLayout; format: VertexFormat; offset: number; view: DataView | null; available: number }[] = [];
  for (const vb of [...state.vertexBuffers.values()].sort((x, y) => x.binding - y.binding)) {
    const layout = vertexLayout(state, vb.binding, vb);
    if (!layout?.stride) continue;
    const captured = data.buffer(vb.dataId);
    if (!captured?.data) notes.push(`Vertex buffer binding ${vb.binding} was not captured.`);
    const view = captured?.data ? new DataView(captured.data.buffer, captured.data.byteOffset, captured.data.byteLength) : null;
    for (const a of layout.attributes) {
      const format = vertexFormat(a.format);
      if (!format) continue;
      const perInstance = layout.rate.includes("INSTANCE");
      attributes.push({ name: names.get(a.location) || `location${a.location}`, location: a.location, binding: vb.binding, format: a.format, components: format.channels.length, perInstance });
      readers.push({ layout, format, offset: a.offset, view, available: view ? Math.floor(view.byteLength / layout.stride) : 0 });
    }
  }
  if (!state.vertexBuffers.size) notes.push("No vertex buffers are bound: the vertex shader makes its vertices (from gl_VertexIndex, or a storage buffer).");

  // The position: an input named like one, else location 0 when it has three or four components.
  let position = attributes.findIndex((a) => /pos/i.test(a.name) && a.components >= 2 && !vertexFormat(a.format)?.integer);
  if (position < 0) position = attributes.findIndex((a) => a.location === 0 && a.components >= 3 && !vertexFormat(a.format)?.integer);

  return {
    topology, attributes, ids, indices, position, notes,
    values: (order, attribute) => {
      const r = readers[attribute];
      if (!r?.view) return null;
      const id = attributes[attribute].perInstance ? 0 : ids[order];
      if (id === undefined || id >= r.available) return null;
      return r.format.read(r.view, id * r.layout.stride + r.offset);
    },
  };
}

/**
 * The positions of a mesh as a list of primitives (strips and fans unrolled, like transform feedback
 * writes them): x y z per vertex, with the draw-order position each came from.
 */
export function listPositions(input: MeshInput): { positions: Float32Array; order: Uint32Array } {
  const n = input.ids.length;
  if (input.position < 0 || !n) return { positions: new Float32Array(0), order: new Uint32Array(0) };
  const kind = primitiveKind(input.topology);
  const list: number[] = [];
  if (/STRIP/.test(input.topology) && kind === "triangles") {
    for (let i = 0; i + 2 < n; i++) list.push(...(i % 2 ? [i + 1, i, i + 2] : [i, i + 1, i + 2]));
  } else if (/FAN/.test(input.topology)) {
    for (let i = 1; i + 1 < n; i++) list.push(0, i, i + 1);
  } else if (/STRIP/.test(input.topology) && kind === "lines") {
    for (let i = 0; i + 1 < n; i++) list.push(i, i + 1);
  } else {
    const per = verticesPerPrimitive(kind);
    for (let i = 0; i < n - (n % per); i++) list.push(i);
  }
  const positions = new Float32Array(list.length * 3);
  const order = new Uint32Array(list);
  list.forEach((o, i) => {
    const v = input.values(o, input.position);
    positions[i * 3] = v?.[0] ?? NaN;
    positions[i * 3 + 1] = v?.[1] ?? NaN;
    positions[i * 3 + 2] = v?.[2] ?? 0;
  });
  return { positions, order };
}
