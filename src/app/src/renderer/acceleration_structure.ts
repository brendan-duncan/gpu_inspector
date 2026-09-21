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
  /** AABBs: the stride the build walks them by, which is not always the 24 bytes one takes. */
  aabbStride?: number;
  /** Primitives the build range asked for, which is what the structure actually holds. */
  primitiveCount: number;
  /** CaptureBuffers ids of the contents the layer captured, when it resolved the addresses. */
  vertexData?: number;
  indexData?: number;
  transformData?: number;
  instanceData?: number;
  aabbData?: number;
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
      ...(kind === "aabbs" ? { aabbStride: num(g.stride) } : {}),
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
  const aabbs = geometry.aabbs;
  return {
    vertexData: pick(triangles, "vertexData"),
    indexData: pick(triangles, "indexData"),
    transformData: pick(triangles, "transformData"),
    instanceData: pick(instances, "data"),
    aabbData: pick(aabbs, "data"),
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

/** Bytes of one AABB: six floats, the same in both APIs (VkAabbPositionsKHR, D3D12_RAYTRACING_AABB). */
export const AABB_SIZE = 24;

/**
 * The boxes a procedural bottom-level geometry was built from, as the endpoints of their edges: two
 * positions per edge, twelve edges per box. Null when the AABBs were not captured.
 *
 * A procedural geometry has no triangles at all — its shape is whatever its intersection shader
 * decides — so its boxes are the only thing there is to draw, and they are exactly what the
 * traversal tests against.
 */
export function aabbBoxes(g: AccelerationGeometry, bytes: Uint8Array | null): Float32Array | null {
  if (g.kind !== "aabbs" || !bytes) return null;
  const stride = g.aabbStride && g.aabbStride >= AABB_SIZE ? g.aabbStride : AABB_SIZE;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const count = Math.min(g.primitiveCount || Infinity, Math.floor(bytes.byteLength / stride));
  const lines: number[] = [];
  for (let i = 0; i < count; i++) {
    const at = i * stride;
    const min = [view.getFloat32(at, true), view.getFloat32(at + 4, true), view.getFloat32(at + 8, true)];
    const max = [view.getFloat32(at + 12, true), view.getFloat32(at + 16, true), view.getFloat32(at + 20, true)];
    // A degenerate or unwritten box says nothing and crowds the view.
    if (![0, 1, 2].every((k) => Number.isFinite(min[k]) && Number.isFinite(max[k]) && max[k] >= min[k])) continue;
    const corner = (c: number): [number, number, number] =>
      [(c & 1) ? max[0] : min[0], (c & 2) ? max[1] : min[1], (c & 4) ? max[2] : min[2]];
    for (let a = 0; a < 8; a++) {
      for (const bit of [1, 2, 4]) {
        const b = a ^ bit;
        if (b <= a) continue;
        lines.push(...corner(a), ...corner(b));
      }
    }
  }
  return lines.length ? new Float32Array(lines) : null;
}

/** One geometry of a bottom level, as a list of primitives in its own space. */
export interface GeometryPart {
  /** The geometry's index in its build. */
  geometry: number;
  /** Line pairs (a procedural geometry's boxes) rather than triangles. */
  lines: boolean;
  positions: Float32Array;
}

/**
 * A run of a scene's vertices that came from one geometry of one instance: what a tree row hides,
 * what a click in the preview names, and what a statistic is summed over. `first` and `count` are
 * in vertices of the scene's `triangles` or `lines`, as `lines` says.
 */
export interface SceneGroup {
  /** The instance's position in the list given, or -1 in a bottom level's own drawing. */
  instance: number;
  /** The geometry's index in its bottom level's build, or -1 for a stand-in box. */
  geometry: number;
  lines: boolean;
  first: number;
  count: number;
}

export interface InstanceScene {
  /** The triangles when there are any, the lines otherwise: what a one-kind preview draws. */
  mesh: Float32Array;
  kind: "triangles" | "lines";
  placed: number;
  drawn: SceneGeometry;
  /** Every placed triangle, and every line: procedural boxes and stand-ins for missing geometry. */
  triangles: Float32Array;
  lines: Float32Array;
  groups: SceneGroup[];
}

/**
 * The scene a top level describes: each instance's bottom level placed by its transform, and a box
 * where the geometry of one is not in the capture. A bottom level is usually built once, before any
 * capture, so the boxes are the common case rather than the fallback — and they still say how many
 * instances there are and where they sit, which is what a top level is for.
 *
 * `meshOf` gives the triangles of a bottom level by object id, or null when they were not captured;
 * `partsOf`, when given, every geometry of it apart, which is what lets the scene say which vertices
 * came from which geometry of which instance.
 */
export function instanceScene(instances: AccelerationInstance[],
                              meshOf: (blas: number) => Float32Array | null,
                              boxesOf?: (blas: number) => Float32Array | null,
                              partsOf?: (blas: number) => GeometryPart[] | null): InstanceScene {
  const triangles: number[] = [];
  const lines: number[] = [];
  const groups: SceneGroup[] = [];
  let placed = 0;
  let hasTriangles = false;
  let hasBoxes = false;
  const place = (instance: number, geometry: number, isLines: boolean, source: ArrayLike<number>, transform: number[]): void => {
    const out = isLines ? lines : triangles;
    const first = out.length / 3;
    for (let v = 0; v + 2 < source.length; v += 3) {
      const p = transformPoint(transform, source[v], source[v + 1], source[v + 2]);
      out.push(p[0], p[1], p[2]);
    }
    groups.push({ instance, geometry, lines: isLines, first, count: out.length / 3 - first });
  };
  instances.forEach((i, at) => {
    const parts = i.blas !== undefined && partsOf ? partsOf(i.blas) : null;
    if (parts && parts.length) {
      placed++;
      for (const part of parts) {
        place(at, part.geometry, part.lines, part.positions, i.transform);
        if (part.lines) hasBoxes = true;
        else hasTriangles = true;
      }
      return;
    }
    const geometry = !partsOf && i.blas !== undefined ? meshOf(i.blas) : null;
    if (geometry) {
      placed++;
      hasTriangles = true;
      place(at, 0, false, geometry, i.transform);
      return;
    }
    // A procedural bottom level: its own boxes, which is all its shape ever is outside its
    // intersection shader. Placed by the instance like any other geometry.
    const boxes = !partsOf && i.blas !== undefined && boxesOf ? boxesOf(i.blas) : null;
    if (boxes) {
      placed++;
      hasBoxes = true;
      place(at, 0, true, boxes, i.transform);
      return;
    }
    place(at, -1, true, CUBE_EDGES.flat(), i.transform);
  });
  const drawn: SceneGeometry = hasTriangles ? "triangles" : hasBoxes ? "aabbs" : "none";
  const tri = new Float32Array(triangles);
  const lin = new Float32Array(lines);
  // Triangles win when any geometry was captured, for a caller that draws one kind at a time: a
  // box drawn around known geometry says less than the geometry does.
  return tri.length
    ? { mesh: tri, kind: "triangles", placed, drawn, triangles: tri, lines: lin, groups }
    : { mesh: lin, kind: "lines", placed, drawn, triangles: tri, lines: lin, groups };
}

/**
 * What the scene ended up drawing, which decides what the caption can honestly claim: the bottom
 * levels' triangles, their own procedural boxes, or a stand-in cube per instance because neither
 * was in the capture.
 */
export type SceneGeometry = "triangles" | "aabbs" | "none";
