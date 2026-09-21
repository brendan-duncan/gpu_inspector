// Ray queries for the Metal Shading Language debugger: what `intersector::intersect` finds, worked
// out on the CPU over the geometry the capture read back.
//
// Metal traverses a scene inside the shader — there are no hit shaders and no binding table, a
// kernel holds an `intersector` and calls `intersect` — so a debugger that cannot follow that call
// stops at the one line the whole kernel is about. This is the traversal, and it is brute force:
// every instance, every primitive, nearest hit wins. That is correct, and for the one invocation
// being stepped it is fast enough — a driver's BVH exists to make millions of rays a frame
// affordable, and this traces one.
//
// What makes it possible at all is that the capture already holds the scene. The ray tracing work
// reads a build's vertices, indices and bounding boxes back and resolves a top level's instances to
// their bottom levels (acceleration_scene.ts), which is exactly the input a traversal needs. So
// this module is the arithmetic and nothing else.
//
// Two things are worth knowing before reading the intersection code:
//
//   * **A hit is found in the instance's object space.** An instance places its bottom level with a
//     3x4 transform, so the ray is transformed by that transform's *inverse* and tested against the
//     geometry as it was built. The distance comes back in world space because the inverse is
//     applied to the direction without normalizing it — the parameter `t` along a transformed ray is
//     the same `t` as along the original.
//   * **A bounding box is only a candidate.** Its geometry is procedural: what is actually in the
//     box is whatever the application's intersection function says, and that function is in the
//     shader. So the traversal returns the boxes a ray entered, in the order it entered them, and
//     the caller runs the intersection function for each (interpreter.ts). Nothing here decides
//     whether a box was hit.
import { transformPoint, type AccelerationInstance } from "../acceleration_structure.js";
import type { AccelerationScene } from "../ray_tracing_view.js";

/** What `intersection_type` names, with the values MSL gives them. */
export const INTERSECTION_NONE = 0;
export const INTERSECTION_TRIANGLE = 1;
export const INTERSECTION_BOUNDING_BOX = 2;

/** A ray as MSL's `ray` struct holds one. */
export interface DebugRay {
  origin: [number, number, number];
  direction: [number, number, number];
  minDistance: number;
  maxDistance: number;
}

/** One geometry of a bottom level, in the form the traversal walks. */
export interface RayGeometry {
  /** The geometry's index within its bottom level, which is what `geometry_id` reports. */
  index: number;
  /** Triangles as 9 floats each (three positions), already in the bottom level's own space. */
  triangles: Float32Array | null;
  /** Boxes as 6 floats each (min then max), for a procedural geometry. */
  boxes: Float32Array | null;
  /** The entry of a bound intersection function table this geometry's boxes call. */
  functionTableOffset: number;
  /** Whether the build marked the geometry opaque, so an intersection function is not called. */
  opaque: boolean;
}

/** One instance of the top level, with everything a traversal needs to place and filter it. */
export interface RayInstance {
  index: number;
  /** The 3x4 row-major transform placing the bottom level in the world. */
  transform: number[];
  /** Its inverse, for taking a ray into the bottom level's space. Null when it is singular. */
  inverse: number[] | null;
  mask: number;
  userId: number;
  /** The instance's own offset into the intersection function table, added to the geometry's. */
  functionTableOffset: number;
  geometries: RayGeometry[];
  /** Set when the instance names a bottom level whose build the capture does not hold. */
  missing: boolean;
}

export interface RayScene {
  instances: RayInstance[];
  /** Instances naming a bottom level with no geometry in the capture: what a ray cannot be told about. */
  missing: number;
  /** Triangles and boxes across the whole scene, for the note the debugger shows. */
  triangles: number;
  boxes: number;
}

/** A box a ray entered: what the intersection function has to be asked about. */
export interface BoxCandidate {
  instance: number;
  geometry: number;
  primitive: number;
  /** Where the ray enters and leaves the box, in world-space `t`. */
  tMin: number;
  tMax: number;
  /** The table entry to call: the instance's offset plus the geometry's. */
  functionTableOffset: number;
  /** The geometry was built opaque, so Metal would not call a function for it. */
  opaque: boolean;
}

/** What a traversal found, in the shape `intersection_result` reports. */
export interface RayHit {
  type: number;
  distance: number;
  primitiveId: number;
  geometryId: number;
  instanceId: number;
  userInstanceId: number;
  /** Barycentric (u, v) of a triangle hit, as MSL reports them. */
  barycentric: [number, number];
  frontFacing: boolean;
  /** The instance's transforms, for `world_space_data`. 3x4 row-major, as the instance holds them. */
  objectToWorld: number[];
  worldToObject: number[];
}

export function noHit(): RayHit {
  return {
    type: INTERSECTION_NONE, distance: 0, primitiveId: 0, geometryId: 0, instanceId: 0, userInstanceId: 0,
    barycentric: [0, 0], frontFacing: false, objectToWorld: IDENTITY_3X4.slice(), worldToObject: IDENTITY_3X4.slice(),
  };
}

const IDENTITY_3X4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];

/**
 * The inverse of a 3x4 row-major affine transform, or null when its rotation part is singular.
 *
 * Written out rather than looped: a 3x3 inverse by cofactors is short, exact for the transforms an
 * instance actually carries (rotations, scales and translations), and has no pivoting to get wrong.
 */
export function invert3x4(m: number[]): number[] | null {
  const [a, b, c, tx, d, e, f, ty, g, h, i, tz] = m;
  const A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  const det = a * A + b * B + c * C;
  if (!Number.isFinite(det) || Math.abs(det) < 1e-20) return null;
  const s = 1 / det;
  const r = [
    A * s, (c * h - b * i) * s, (b * f - c * e) * s, 0,
    B * s, (a * i - c * g) * s, (c * d - a * f) * s, 0,
    C * s, (b * g - a * h) * s, (a * e - b * d) * s, 0,
  ];
  // The translation of the inverse is -R⁻¹·t.
  r[3] = -(r[0] * tx + r[1] * ty + r[2] * tz);
  r[7] = -(r[4] * tx + r[5] * ty + r[6] * tz);
  r[11] = -(r[8] * tx + r[9] * ty + r[10] * tz);
  return r;
}

/** A direction through a 3x4 transform: the rotation part only, with no translation and no normalizing. */
function transformDirection(m: number[], x: number, y: number, z: number): [number, number, number] {
  return [
    m[0] * x + m[1] * y + m[2] * z,
    m[4] * x + m[5] * y + m[6] * z,
    m[8] * x + m[9] * y + m[10] * z,
  ];
}

/**
 * The geometries of a bottom level, in traversal shape.
 *
 * `traversalOf` is the accessor that gives box *extents* and each geometry's own table offset
 * (acceleration_scene.ts); `meshOf` is the fallback, and it concatenates every geometry of the
 * bottom level into one mesh, so a hit in it reports `geometry_id` 0 whichever geometry it was in.
 * That is the honest reading of what a scene built that way can say, and it is only reached for a
 * scene object that predates `traversalOf`.
 */
function geometriesOf(scene: AccelerationScene, blas: number): RayGeometry[] {
  const parts = scene.traversalOf?.(blas) ?? null;
  if (parts && parts.length) {
    return parts.map((part) => ({
      index: part.index,
      triangles: part.triangles,
      boxes: part.extents,
      functionTableOffset: part.functionTableOffset,
      opaque: part.opaque,
    }));
  }
  const triangles = scene.meshOf(blas);
  if (!triangles) return [];
  return [{ index: 0, triangles, boxes: null, functionTableOffset: 0, opaque: false }];
}

/** The scene a `[[buffer(n)]]` acceleration structure binds, flattened for traversal. */
export function buildRayScene(scene: AccelerationScene): RayScene {
  const instances: RayInstance[] = [];
  let missing = 0;
  let triangles = 0;
  let boxes = 0;
  const byBlas = new Map<number, RayGeometry[]>();
  for (const i of scene.instances as AccelerationInstance[]) {
    const blas = i.blas ?? 0;
    let geometries = blas ? byBlas.get(blas) : undefined;
    if (geometries === undefined) {
      geometries = blas ? geometriesOf(scene, blas) : [];
      if (blas) byBlas.set(blas, geometries);
    }
    const absent = geometries.length === 0;
    if (absent) missing++;
    for (const g of geometries) {
      triangles += g.triangles ? Math.floor(g.triangles.length / 9) : 0;
      boxes += g.boxes ? Math.floor(g.boxes.length / 6) : 0;
    }
    instances.push({
      index: i.index,
      transform: i.transform,
      inverse: invert3x4(i.transform),
      mask: i.mask,
      userId: i.customIndex,
      functionTableOffset: i.bindingTableOffset,
      geometries,
      missing: absent,
    });
  }
  return { instances, missing, triangles, boxes };
}

/** How a traversal is to behave, from the intersector's tags and its setters. */
export interface RayOptions {
  /** `accept_any_intersection(true)`: stop at the first hit rather than the nearest. */
  acceptAny?: boolean;
  /** `assume_geometry_type`: skip the kind the intersector says the scene does not hold. */
  triangles?: boolean;
  boundingBoxes?: boolean;
  /**
   * `force_opacity(forced_opacity::opaque)`: a box is taken at the point the ray enters it, with no
   * intersection function called. What Metal does with an opaque geometry, said for the whole query.
   */
  forceOpaque?: boolean;
}

/**
 * Traces one ray. Returns the nearest triangle hit and the boxes the ray entered.
 *
 * The boxes are sorted by where the ray enters them, because that is the order an intersection
 * function has to be asked in for "nearest hit" to come out right with early acceptance: a function
 * that reports a hit at the far side of a near box must not beat one at the near side of a far box,
 * and asking in entry order with the current nearest as the limit is what keeps that straight.
 */
export function traceRay(scene: RayScene, ray: DebugRay, mask: number,
                         options: RayOptions = {}): { hit: RayHit; candidates: BoxCandidate[] } {
  const wantTriangles = options.triangles !== false;
  const wantBoxes = options.boundingBoxes !== false;
  const hit = noHit();
  let best = ray.maxDistance;
  const candidates: BoxCandidate[] = [];

  for (const instance of scene.instances) {
    // Metal's rule: a ray whose mask ANDs to zero with the instance's skips it entirely.
    if ((mask & instance.mask) === 0) continue;
    const inverse = instance.inverse;
    if (!inverse) continue;
    const origin = transformPoint(inverse, ray.origin[0], ray.origin[1], ray.origin[2]);
    const direction = transformDirection(inverse, ray.direction[0], ray.direction[1], ray.direction[2]);

    for (const geometry of instance.geometries) {
      if (wantTriangles && geometry.triangles) {
        const count = Math.floor(geometry.triangles.length / 9);
        for (let p = 0; p < count; p++) {
          const t = intersectTriangle(geometry.triangles, p * 9, origin, direction, ray.minDistance, best);
          if (!t) continue;
          best = t.distance;
          hit.type = INTERSECTION_TRIANGLE;
          hit.distance = t.distance;
          hit.primitiveId = p;
          hit.geometryId = geometry.index;
          hit.instanceId = instance.index;
          hit.userInstanceId = instance.userId;
          hit.barycentric = [t.u, t.v];
          hit.frontFacing = t.frontFacing;
          hit.objectToWorld = instance.transform;
          hit.worldToObject = inverse;
          if (options.acceptAny) return { hit, candidates };
        }
      }
      if (wantBoxes && geometry.boxes) {
        const count = Math.floor(geometry.boxes.length / 6);
        for (let p = 0; p < count; p++) {
          const range = intersectBox(geometry.boxes, p * 6, origin, direction, ray.minDistance, ray.maxDistance);
          if (!range) continue;
          candidates.push({
            instance: instance.index, geometry: geometry.index, primitive: p,
            tMin: range.tMin, tMax: range.tMax,
            functionTableOffset: instance.functionTableOffset + geometry.functionTableOffset,
            opaque: geometry.opaque,
          });
        }
      }
    }
  }
  candidates.sort((a, b) => a.tMin - b.tMin);
  return { hit, candidates };
}

/**
 * Möller–Trumbore, with the barycentrics Metal reports.
 *
 * MSL's `triangle_barycentric_coord` is (u, v) with the third weight implied as 1 - u - v, and the
 * vertex it belongs to is the *first*: a hit at vertex 0 reports (0, 0). That is the same
 * convention Vulkan and DXR use, so the shared views read all three the same way.
 */
export function intersectTriangle(triangles: Float32Array, at: number,
                                  origin: [number, number, number], direction: [number, number, number],
                                  tMin: number, tMax: number):
    { distance: number; u: number; v: number; frontFacing: boolean } | null {
  const ax = triangles[at], ay = triangles[at + 1], az = triangles[at + 2];
  const bx = triangles[at + 3], by = triangles[at + 4], bz = triangles[at + 5];
  const cx = triangles[at + 6], cy = triangles[at + 7], cz = triangles[at + 8];
  const e1x = bx - ax, e1y = by - ay, e1z = bz - az;
  const e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
  // p = direction x e2
  const px = direction[1] * e2z - direction[2] * e2y;
  const py = direction[2] * e2x - direction[0] * e2z;
  const pz = direction[0] * e2y - direction[1] * e2x;
  const det = e1x * px + e1y * py + e1z * pz;
  if (!Number.isFinite(det) || Math.abs(det) < 1e-20) return null;
  const inv = 1 / det;
  const sx = origin[0] - ax, sy = origin[1] - ay, sz = origin[2] - az;
  const u = (sx * px + sy * py + sz * pz) * inv;
  if (u < 0 || u > 1) return null;
  // q = s x e1
  const qx = sy * e1z - sz * e1y;
  const qy = sz * e1x - sx * e1z;
  const qz = sx * e1y - sy * e1x;
  const v = (direction[0] * qx + direction[1] * qy + direction[2] * qz) * inv;
  if (v < 0 || u + v > 1) return null;
  const distance = (e2x * qx + e2y * qy + e2z * qz) * inv;
  if (!(distance >= tMin) || !(distance <= tMax)) return null;
  return { distance, u, v, frontFacing: det > 0 };
}

/** The slab test: where a ray enters and leaves an axis-aligned box, or null when it misses. */
export function intersectBox(boxes: Float32Array, at: number,
                             origin: [number, number, number], direction: [number, number, number],
                             tMin: number, tMax: number): { tMin: number; tMax: number } | null {
  let near = tMin;
  let far = tMax;
  for (let axis = 0; axis < 3; axis++) {
    const lo = boxes[at + axis], hi = boxes[at + 3 + axis];
    const d = direction[axis];
    if (Math.abs(d) < 1e-20) {
      // Parallel to this pair of planes: inside them or nowhere.
      if (origin[axis] < lo || origin[axis] > hi) return null;
      continue;
    }
    const inv = 1 / d;
    let t0 = (lo - origin[axis]) * inv;
    let t1 = (hi - origin[axis]) * inv;
    if (t0 > t1) { const swap = t0; t0 = t1; t1 = swap; }
    near = Math.max(near, t0);
    far = Math.min(far, t1);
    if (near > far) return null;
  }
  return { tMin: near, tMax: far };
}

/**
 * An intersection function table as the debugger reads one: which function each entry runs, and the
 * buffers the table binds for them.
 *
 * Both come from the capture. An `MTLFunctionHandle` carries its function's *name*, so the table's
 * entries are recorded by name rather than as opaque identifiers (src/metal/src/raytracing.mm), and
 * a name is exactly what the debugger needs to find the function in the shader it is stepping.
 */
export interface RayFunctionTable {
  /** The function each entry runs, by index; null for an entry the application never set. */
  entries: (string | null)[];
  /** The bytes the table bound at `[[buffer(n)]]` for its functions. */
  buffer(index: number): Uint8Array | null;
}

// The handles a register holds, carried in an OpaqueValue's `handle` (debug/values.ts): the shared
// value type has a case for a texture and a sampler because both interpreters have those, and none
// for these because only MSL does.

/** An acceleration structure bound at a buffer index. */
export interface SceneHandle {
  scene: RayScene | null;
  binding: string;
}

/** An intersection function table bound at a buffer index. */
export interface TableHandle {
  table: RayFunctionTable | null;
  binding: string;
}

/**
 * An `intersector<...>`: what its setters have changed, carried to the next `intersect`.
 *
 * Mutable, and deliberately shared rather than copied: `isect.accept_any_intersection(true)`
 * changes the intersector the shader declared, and the same one has to reach the `intersect` that
 * follows it. The interpreter's `cloneValue` leaves an OpaqueValue alone, which is what makes that
 * work.
 */
export interface IntersectorHandle {
  options: RayOptions;
  /** The tags the type was written with, for the note the debugger shows. */
  tags: string[];
}

export const SCENE_KIND = "acceleration_structure";
export const TABLE_KIND = "intersection_function_table";
export const INTERSECTOR_KIND = "intersector";

/** Whether an opaque type's name is an acceleration structure. */
export function isAccelerationStructure(name: string): boolean {
  return name === "instance_acceleration_structure" || name === "primitive_acceleration_structure"
    || name === "acceleration_structure";
}

/** Whether an opaque type's name is a function table. */
export function isFunctionTable(name: string): boolean {
  return name === "intersection_function_table" || name === "visible_function_table";
}

/** The hit a resolved bounding box makes, given what the intersection function reported. */
export function boxHit(scene: RayScene, candidate: BoxCandidate, distance: number): RayHit {
  const instance = scene.instances.find((i) => i.index === candidate.instance);
  const hit = noHit();
  hit.type = INTERSECTION_BOUNDING_BOX;
  hit.distance = distance;
  hit.primitiveId = candidate.primitive;
  hit.geometryId = candidate.geometry;
  hit.instanceId = candidate.instance;
  hit.userInstanceId = instance?.userId ?? 0;
  hit.objectToWorld = instance?.transform ?? IDENTITY_3X4.slice();
  hit.worldToObject = instance?.inverse ?? IDENTITY_3X4.slice();
  return hit;
}
