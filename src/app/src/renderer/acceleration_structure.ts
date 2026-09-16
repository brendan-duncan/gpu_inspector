// What a ray tracing acceleration structure was built from.
//
// An acceleration structure is opaque: the driver owns its layout and nothing can read it back. What
// *can* be known is what it was built out of — the geometry a bottom level was given, and the
// instances a top level was given — because those are ordinary buffers the build reads. The layer
// resolves the device addresses in a build to the buffers holding them and captures their contents
// (src/vulkan/src/acceleration_structure.h), and this turns those bytes into something to show.
//
// A top level's instances are the interesting half. Each one places a bottom level in the world with
// a transform, a visibility mask and flags, and names it by device address — so an instance is also
// the only link from a top level back to the structures under it. Resolving that address is what
// makes the bottom levels reachable at all: a capture holds the objects it references, and before
// this nothing referenced them.
import type { ArgObject, ArgValue } from "../shared/protocol.js";
import { isObject, num, str } from "./vulkan/vulkan_object.js";
import { vertexFormat } from "./vulkan/vk_format.js";

/** Bytes of one VkAccelerationStructureInstanceKHR. Fixed by the specification. */
export const INSTANCE_STRIDE = 64;

/** The flag bits of VkGeometryInstanceFlagBitsKHR, which the instance packs into a byte. */
export const INSTANCE_FLAGS: [number, string][] = [
  [0x1, "TRIANGLE_FACING_CULL_DISABLE"],
  [0x2, "TRIANGLE_FLIP_FACING"],
  [0x4, "FORCE_OPAQUE"],
  [0x8, "FORCE_NO_OPAQUE"],
];

export interface AccelerationInstance {
  index: number;
  /**
   * The 3x4 row-major transform placing the bottom level in the world, as 12 numbers. Vulkan stores
   * it row-major, unlike almost everything else in graphics, so it is kept that way rather than
   * transposed into a convention it does not use.
   */
  transform: number[];
  /** The value a shader reads as gl_InstanceCustomIndexEXT (24 bits). */
  customIndex: number;
  /** Rays whose mask ANDs to zero with this skip the instance entirely (8 bits). */
  mask: number;
  /** Which hit group the instance uses, as an offset into the binding table (24 bits). */
  bindingTableOffset: number;
  flags: number;
  flagNames: string[];
  /** The bottom level's device address, as a decimal string: it is 64-bit and past what a number holds. */
  reference: string;
  /** The object id of the bottom level that address resolved to, when the layer could resolve it. */
  blas?: number;
}

/** Translation of an instance's transform: the last column of the 3x4, which is where it sits. */
export function instancePosition(i: AccelerationInstance): [number, number, number] {
  return [i.transform[3] ?? 0, i.transform[7] ?? 0, i.transform[11] ?? 0];
}

/** Whether a transform is the identity, which most single-instance scenes use. */
export function isIdentity(transform: number[]): boolean {
  const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];
  return transform.length === 12 && identity.every((v, i) => Math.abs(transform[i] - v) < 1e-6);
}

function flagNamesOf(flags: number): string[] {
  return INSTANCE_FLAGS.filter(([bit]) => (flags & bit) !== 0).map(([, name]) => name);
}

/**
 * The instances a top level was built from, parsed out of the buffer the build read. Returns as many
 * whole instances as the bytes hold: a partial one at the end is a read-back cut short by the
 * capture's buffer limit, and half an instance is not worth guessing at.
 *
 * `references` maps a bottom level's device address (decimal string) to its object id, which is what
 * the layer resolved at build time.
 */
export function parseInstances(bytes: Uint8Array, references?: Map<string, number>): AccelerationInstance[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const out: AccelerationInstance[] = [];
  for (let i = 0; i + INSTANCE_STRIDE <= bytes.byteLength; i += INSTANCE_STRIDE) {
    const transform: number[] = [];
    for (let k = 0; k < 12; k++) transform.push(view.getFloat32(i + k * 4, true));
    // instanceCustomIndex is the low 24 bits and mask the high 8 of one word; the binding table
    // offset and flags are packed the same way in the next.
    const indexAndMask = view.getUint32(i + 48, true);
    const offsetAndFlags = view.getUint32(i + 52, true);
    const flags = offsetAndFlags >>> 24;
    const reference = view.getBigUint64(i + 56, true).toString();
    out.push({
      index: out.length,
      transform,
      customIndex: indexAndMask & 0xffffff,
      mask: indexAndMask >>> 24,
      bindingTableOffset: offsetAndFlags & 0xffffff,
      flags,
      flagNames: flagNamesOf(flags),
      reference,
      ...(references?.has(reference) ? { blas: references.get(reference) } : {}),
    });
  }
  return out;
}

/** One geometry of a bottom-level build, as the build described it. */
export interface AccelerationGeometry {
  index: number;
  /** "triangles", "aabbs" or "instances", from VkGeometryTypeKHR. */
  kind: string;
  flags: string;
  /** Triangles: the vertex format and stride the build read, and how many it was given. */
  vertexFormat?: string;
  vertexStride?: number;
  maxVertex?: number;
  indexType?: string;
  /** Primitives the build range asked for, which is what the structure actually holds. */
  primitiveCount: number;
  /** CaptureBuffers ids of the contents the layer captured, when it resolved the addresses. */
  vertexData?: number;
  indexData?: number;
  transformData?: number;
  instanceData?: number;
}

export interface AccelerationBuild {
  /** The structure being built, and whether it is a top or bottom level. */
  target: number;
  topLevel: boolean;
  /** "BUILD" or "UPDATE", from VkBuildAccelerationStructureModeKHR. */
  mode: string;
  flags: string;
  geometries: AccelerationGeometry[];
  /** Total primitives across the geometries: triangles for a bottom level, instances for a top. */
  primitives: number;
}

const SHORT = (s: string, prefix: RegExp): string => str(s).replace(prefix, "").replace(/_KHR$/, "");

/**
 * One `vkCmdBuildAccelerationStructuresKHR` info, from the recorded arguments. Null when the
 * arguments are not a build's, so a caller can map over commands without filtering first.
 */
export function parseBuild(info: ArgValue | undefined, range: ArgValue | undefined): AccelerationBuild | null {
  if (!isObject(info)) return null;
  const dst = isObject(info.dstAccelerationStructure) ? info.dstAccelerationStructure : null;
  const target = dst ? num(dst.__id) : 0;
  if (!target) return null;
  const type = str(info.type);
  const ranges = Array.isArray(range) ? range : range === undefined ? [] : [range];
  const raw = Array.isArray(info.pGeometries) ? info.pGeometries : [];

  const geometries: AccelerationGeometry[] = [];
  let primitives = 0;
  raw.forEach((g, index) => {
    if (!isObject(g)) return;
    const r = ranges[index];
    const count = isObject(r) ? num(r.primitiveCount) : 0;
    primitives += count;
    const kind = SHORT(str(g.geometryType), /^VK_GEOMETRY_TYPE_/).toLowerCase();
    const geometry = isObject(g.geometry) ? g.geometry : {};
    const triangles = isObject(geometry.triangles) ? geometry.triangles : null;
    geometries.push({
      index, kind, flags: str(g.flags), primitiveCount: count,
      ...(triangles ? {
        vertexFormat: SHORT(str(triangles.vertexFormat), /^VK_FORMAT_/),
        vertexStride: num(triangles.vertexStride),
        maxVertex: num(triangles.maxVertex),
        indexType: SHORT(str(triangles.indexType), /^VK_INDEX_TYPE_/),
      } : {}),
      ...capturedData(g),
    });
  });

  return {
    target,
    topLevel: type.includes("TOP_LEVEL"),
    mode: SHORT(str(info.mode), /^VK_BUILD_ACCELERATION_STRUCTURE_MODE_/),
    flags: str(info.flags),
    geometries,
    primitives,
  };
}

/**
 * The CaptureBuffers ids the layer attached to a geometry's addresses. Each address it could resolve
 * to a buffer gains a `capture` beside it; one it could not keeps only the raw address, which is a
 * build reading memory from a buffer the capture never saw.
 */
function capturedData(g: ArgObject): Partial<AccelerationGeometry> {
  const geometry = isObject(g.geometry) ? g.geometry : {};
  const pick = (holder: ArgValue | undefined, field: string): number | undefined => {
    if (!isObject(holder)) return undefined;
    const d = holder[field];
    if (!isObject(d)) return undefined;
    const id = num(d.capture);
    return id > 0 ? id : undefined;
  };
  const triangles = geometry.triangles;
  const instances = geometry.instances;
  return {
    vertexData: pick(triangles, "vertexData"),
    indexData: pick(triangles, "indexData"),
    transformData: pick(triangles, "transformData"),
    instanceData: pick(instances, "data"),
  };
}

// ---------------------------------------------------------------------------------------------
// Drawing what was built.
//
// The preview takes a flat list of positions, one per vertex, already expanded into primitives
// (renderer/mesh_preview.ts) — so everything here ends at a Float32Array and nothing here knows
// about WebGL.

/** A unit cube's 12 edges, as pairs of corners, for drawing an instance whose geometry is absent. */
const CUBE_EDGES: [number, number, number][] = (() => {
  const corner = (i: number): [number, number, number] =>
    [(i & 1) ? 0.5 : -0.5, (i & 2) ? 0.5 : -0.5, (i & 4) ? 0.5 : -0.5];
  const pairs: [number, number, number][] = [];
  for (let a = 0; a < 8; a++) {
    for (const bit of [1, 2, 4]) {
      const b = a ^ bit;
      if (b > a) pairs.push(corner(a), corner(b));
    }
  }
  return pairs;
})();

/** A point through an instance's row-major 3x4 transform. */
export function transformPoint(m: number[], x: number, y: number, z: number): [number, number, number] {
  return [
    m[0] * x + m[1] * y + m[2] * z + m[3],
    m[4] * x + m[5] * y + m[6] * z + m[7],
    m[8] * x + m[9] * y + m[10] * z + m[11],
  ];
}

/**
 * The triangles a bottom-level geometry was built from, as one position per vertex in triangle
 * order. Null when the build's vertices were not captured, or the format is not one that can be
 * read as positions.
 *
 * `maxVertex` is the highest index the build may read, so the vertex array holds one more than it.
 */
export function triangleMesh(g: AccelerationGeometry, vertices: Uint8Array | null,
                             indices: Uint8Array | null): Float32Array | null {
  if (g.kind !== "triangles" || !vertices || !g.vertexStride) return null;
  const format = vertexFormat(`VK_FORMAT_${g.vertexFormat ?? ""}`);
  if (!format) return null;
  const view = new DataView(vertices.buffer, vertices.byteOffset, vertices.byteLength);
  const count = Math.min((g.maxVertex ?? 0) + 1, Math.floor(vertices.byteLength / g.vertexStride));
  const positions: number[] = [];
  const at = (vertex: number): void => {
    const offset = vertex * g.vertexStride!;
    if (vertex >= count || offset + format.size > vertices.byteLength) {
      positions.push(0, 0, 0);
      return;
    }
    const v = format.read(view, offset);
    positions.push(v[0] ?? 0, v[1] ?? 0, v[2] ?? 0);
  };

  const wanted = g.primitiveCount * 3;
  if (g.indexType && g.indexType !== "NONE" && indices) {
    const size = g.indexType === "UINT16" ? 2 : 4;
    const iv = new DataView(indices.buffer, indices.byteOffset, indices.byteLength);
    const available = Math.floor(indices.byteLength / size);
    for (let i = 0; i < wanted && i < available; i++) {
      at(size === 2 ? iv.getUint16(i * size, true) : iv.getUint32(i * size, true));
    }
  } else {
    for (let i = 0; i < wanted; i++) at(i);
  }
  return positions.length ? new Float32Array(positions) : null;
}

/**
 * The scene a top level describes: each instance's bottom level placed by its transform, and a box
 * where the geometry of one is not in the capture. A bottom level is usually built once, before any
 * capture, so the boxes are the common case rather than the fallback — and they still say how many
 * instances there are and where they sit, which is what a top level is for.
 *
 * `meshOf` gives the triangles of a bottom level by object id, or null when they were not captured.
 */
export function instanceScene(instances: AccelerationInstance[],
                              meshOf: (blas: number) => Float32Array | null): { mesh: Float32Array; kind: "triangles" | "lines"; placed: number } {
  const triangles: number[] = [];
  const lines: number[] = [];
  let placed = 0;
  for (const i of instances) {
    const geometry = i.blas !== undefined ? meshOf(i.blas) : null;
    if (geometry) {
      placed++;
      for (let v = 0; v + 2 < geometry.length; v += 3) {
        const p = transformPoint(i.transform, geometry[v], geometry[v + 1], geometry[v + 2]);
        triangles.push(p[0], p[1], p[2]);
      }
    } else {
      for (const [x, y, z] of CUBE_EDGES) {
        const p = transformPoint(i.transform, x, y, z);
        lines.push(p[0], p[1], p[2]);
      }
    }
  }
  // Triangles win when any geometry was captured: a box drawn around known geometry says less than
  // the geometry does, and the preview draws one primitive kind at a time.
  return triangles.length
    ? { mesh: new Float32Array(triangles), kind: "triangles", placed }
    : { mesh: new Float32Array(lines), kind: "lines", placed };
}
