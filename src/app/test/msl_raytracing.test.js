// The CPU ray traversal the Metal shader debugger steps a ray query with
// (src/renderer/msl/raytracing.ts).
//
// Every one of these is arithmetic a screenshot cannot check and a wrong answer looks plausible
// for. The two that matter most:
//
//   * **The instance transform is applied to the ray, inverted.** A hit is found in the bottom
//     level's own space, so the ray goes through the transform's inverse — and if the inverse is
//     wrong, or applied to the direction *with* its translation, the geometry is still hit, just
//     somewhere else, and the distance comes back scaled. A test with a translated and rotated
//     instance is the only thing that tells those apart.
//   * **Barycentrics belong to the first vertex.** MSL reports (u, v) with the third weight implied
//     as 1 - u - v, and a hit at vertex 0 is (0, 0). Getting the pair the other way round shades
//     every triangle wrong in a way that looks like a lighting bug.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "mslrt-"));
const out = join(dir, "raytracing.js");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "msl", "raytracing.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const {
  buildRayScene, traceRay, invert3x4, intersectTriangle, intersectBox, boxHit,
  INTERSECTION_NONE, INTERSECTION_TRIANGLE, INTERSECTION_BOUNDING_BOX,
} = await import(pathToFileURL(out).href);

const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];

/** A triangle in the z = 0 plane: (0, 0), (1, 0), (0, 1). */
const UNIT_TRIANGLE = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);

function instance(over = {}) {
  return {
    index: 0, transform: IDENTITY.slice(), customIndex: 0, mask: 0xFF, bindingTableOffset: 0,
    flags: 0, flagNames: [], reference: "0", blas: 1, ...over,
  };
}

/** A scene of one bottom level (id 1) holding `geometries`, instanced by `instances`. */
function scene(instances, geometries) {
  return buildRayScene({
    instances,
    meshOf: () => null,
    traversalOf: (blas) => (blas === 1 ? geometries : null),
  });
}

function triangleGeometry(triangles, over = {}) {
  return [{ index: 0, triangles, extents: null, functionTableOffset: 0, opaque: false, ...over }];
}

function boxGeometry(extents, over = {}) {
  return [{ index: 0, triangles: null, extents, functionTableOffset: 0, opaque: false, ...over }];
}

const down = (x, y) => ({ origin: [x, y, 1], direction: [0, 0, -1], minDistance: 0, maxDistance: 10 });

// ------------------------------------------------------------------------------------ triangles

test("a ray down the z axis hits a triangle in the z = 0 plane at its own distance", () => {
  const s = scene([instance()], triangleGeometry(UNIT_TRIANGLE));
  const { hit } = traceRay(s, down(0.25, 0.25), 0xFF);
  assert.equal(hit.type, INTERSECTION_TRIANGLE);
  assert.ok(Math.abs(hit.distance - 1) < 1e-6, `distance ${hit.distance}`);
  assert.equal(hit.primitiveId, 0);
  assert.equal(hit.geometryId, 0);
  assert.equal(hit.instanceId, 0);
});

test("a ray beside the triangle misses", () => {
  const s = scene([instance()], triangleGeometry(UNIT_TRIANGLE));
  assert.equal(traceRay(s, down(0.9, 0.9), 0xFF).hit.type, INTERSECTION_NONE);
  assert.equal(traceRay(s, down(-0.1, 0.5), 0xFF).hit.type, INTERSECTION_NONE);
});

test("barycentrics are (u, v) against the first vertex, so a hit on vertex 0 is (0, 0)", () => {
  const s = scene([instance()], triangleGeometry(UNIT_TRIANGLE));
  const at = (x, y) => traceRay(s, down(x, y), 0xFF).hit.barycentric;
  const [u0, v0] = at(0.001, 0.001);
  assert.ok(u0 < 0.01 && v0 < 0.01, `near vertex 0: ${u0}, ${v0}`);
  // Vertex 1 is (1, 0, 0), which is the u axis; vertex 2 is (0, 1, 0), the v axis.
  const [u1, v1] = at(0.98, 0.001);
  assert.ok(u1 > 0.9 && v1 < 0.01, `near vertex 1: ${u1}, ${v1}`);
  const [u2, v2] = at(0.001, 0.98);
  assert.ok(u2 < 0.01 && v2 > 0.9, `near vertex 2: ${u2}, ${v2}`);
});

test("the nearest triangle wins, and accept_any_intersection takes the first found", () => {
  // Two triangles at z = 0 and z = 0.5: a ray from z = 1 downward meets the higher one first.
  const near = new Float32Array([0, 0, 0.5, 1, 0, 0.5, 0, 1, 0.5]);
  const s = scene([instance()], [
    { index: 0, triangles: UNIT_TRIANGLE, extents: null, functionTableOffset: 0, opaque: false },
    { index: 1, triangles: near, extents: null, functionTableOffset: 0, opaque: false },
  ]);
  const nearest = traceRay(s, down(0.25, 0.25), 0xFF).hit;
  assert.equal(nearest.geometryId, 1, "the nearer triangle should win");
  assert.ok(Math.abs(nearest.distance - 0.5) < 1e-6, `distance ${nearest.distance}`);
  // With early acceptance the traversal stops at whichever it reaches first, which is geometry 0.
  const any = traceRay(s, down(0.25, 0.25), 0xFF, { acceptAny: true }).hit;
  assert.equal(any.geometryId, 0, "accept_any_intersection should not keep looking for a nearer hit");
});

test("front facing follows the winding the triangle was built with", () => {
  const s = scene([instance()], triangleGeometry(UNIT_TRIANGLE));
  const front = traceRay(s, down(0.25, 0.25), 0xFF).hit.frontFacing;
  const flipped = new Float32Array([0, 0, 0, 0, 1, 0, 1, 0, 0]);
  const back = traceRay(scene([instance()], triangleGeometry(flipped)), down(0.25, 0.25), 0xFF).hit.frontFacing;
  assert.notEqual(front, back, "reversing the winding should flip front facing");
});

test("min_distance and max_distance bound the hit", () => {
  const s = scene([instance()], triangleGeometry(UNIT_TRIANGLE));
  const ray = down(0.25, 0.25);
  assert.equal(traceRay(s, { ...ray, minDistance: 2 }, 0xFF).hit.type, INTERSECTION_NONE);
  assert.equal(traceRay(s, { ...ray, maxDistance: 0.5 }, 0xFF).hit.type, INTERSECTION_NONE);
  assert.equal(traceRay(s, { ...ray, minDistance: 0.9, maxDistance: 1.1 }, 0xFF).hit.type, INTERSECTION_TRIANGLE);
});

// ------------------------------------------------------------------------------------ instances

test("an instance's transform places its geometry, and the distance stays in world space", () => {
  // Moved +2 in x: the triangle now covers x in [2, 3].
  const moved = instance({ transform: [1, 0, 0, 2, 0, 1, 0, 0, 0, 0, 1, 0] });
  const s = scene([moved], triangleGeometry(UNIT_TRIANGLE));
  assert.equal(traceRay(s, down(0.25, 0.25), 0xFF).hit.type, INTERSECTION_NONE,
               "the instance moved, so the ray at the origin should miss");
  const hit = traceRay(s, down(2.25, 0.25), 0xFF).hit;
  assert.equal(hit.type, INTERSECTION_TRIANGLE);
  assert.ok(Math.abs(hit.distance - 1) < 1e-6, `distance ${hit.distance}`);
});

test("a scaled instance reports the world-space distance, not the object-space one", () => {
  // Scaled by 2 about the origin: the plane stays at z = 0, so a ray from z = 1 still travels 1.
  // What changes is the extent — the triangle now covers x in [0, 2] — so a ray at x = 1.5 hits.
  const scaled = instance({ transform: [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0] });
  const s = scene([scaled], triangleGeometry(UNIT_TRIANGLE));
  const hit = traceRay(s, down(1.5, 0.25), 0xFF).hit;
  assert.equal(hit.type, INTERSECTION_TRIANGLE, "the scaled triangle should reach x = 1.5");
  assert.ok(Math.abs(hit.distance - 1) < 1e-6, `distance ${hit.distance}: the plane did not move`);
  // And a scale along z does move the plane, which the distance has to follow.
  const lifted = instance({ transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0.5] });
  const up = traceRay(scene([lifted], triangleGeometry(UNIT_TRIANGLE)), down(0.25, 0.25), 0xFF).hit;
  assert.ok(Math.abs(up.distance - 0.5) < 1e-6, `distance ${up.distance}`);
});

test("a rotated instance is hit where the rotation puts it", () => {
  // A quarter turn about z: (x, y) -> (-y, x). The triangle's (1, 0) corner goes to (0, 1).
  const turned = instance({ transform: [0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0] });
  const s = scene([turned], triangleGeometry(UNIT_TRIANGLE));
  assert.equal(traceRay(s, down(0.5, 0.25), 0xFF).hit.type, INTERSECTION_NONE,
               "the +x side is empty after the turn");
  assert.equal(traceRay(s, down(-0.25, 0.5), 0xFF).hit.type, INTERSECTION_TRIANGLE,
               "the -x side is where the turn put it");
});

test("the ray mask skips an instance it ANDs to zero with", () => {
  const s = scene([instance({ mask: 0x0F })], triangleGeometry(UNIT_TRIANGLE));
  assert.equal(traceRay(s, down(0.25, 0.25), 0x0F).hit.type, INTERSECTION_TRIANGLE);
  assert.equal(traceRay(s, down(0.25, 0.25), 0xF0).hit.type, INTERSECTION_NONE);
});

test("instance_id and user_instance_id are the instance's own, not its slot", () => {
  const s = scene([instance({ index: 3, customIndex: 77 })], triangleGeometry(UNIT_TRIANGLE));
  const hit = traceRay(s, down(0.25, 0.25), 0xFF).hit;
  assert.equal(hit.instanceId, 3);
  assert.equal(hit.userInstanceId, 77);
});

test("an instance whose bottom level was not captured is counted rather than silently empty", () => {
  const s = buildRayScene({ instances: [instance()], meshOf: () => null, traversalOf: () => null });
  assert.equal(s.missing, 1);
  assert.equal(s.triangles, 0);
  assert.equal(traceRay(s, down(0.25, 0.25), 0xFF).hit.type, INTERSECTION_NONE);
});

// ------------------------------------------------------------------------------- bounding boxes

test("a bounding box is returned as a candidate with where the ray entered and left it", () => {
  const box = new Float32Array([0, 0, -0.5, 1, 1, 0.5]);
  const s = scene([instance()], boxGeometry(box));
  const { hit, candidates } = traceRay(s, down(0.5, 0.5), 0xFF);
  assert.equal(hit.type, INTERSECTION_NONE, "a box is not a hit until the intersection function says so");
  assert.equal(candidates.length, 1);
  assert.equal(candidates[0].primitive, 0);
  assert.ok(Math.abs(candidates[0].tMin - 0.5) < 1e-6, `tMin ${candidates[0].tMin}`);
  assert.ok(Math.abs(candidates[0].tMax - 1.5) < 1e-6, `tMax ${candidates[0].tMax}`);
});

test("candidates come back in the order the ray entered them", () => {
  // Three boxes stacked along z; the ray goes down, so it enters the highest first.
  const boxes = new Float32Array([
    0, 0, -1, 1, 1, -0.9,
    0, 0, 0.4, 1, 1, 0.5,
    0, 0, -0.1, 1, 1, 0,
  ]);
  const s = scene([instance()], boxGeometry(boxes));
  const { candidates } = traceRay(s, down(0.5, 0.5), 0xFF);
  assert.deepEqual(candidates.map((c) => c.primitive), [1, 2, 0]);
});

test("a candidate carries the table entry to call: the instance's offset plus the geometry's", () => {
  const box = new Float32Array([0, 0, -0.5, 1, 1, 0.5]);
  const s = scene([instance({ bindingTableOffset: 4 })], boxGeometry(box, { functionTableOffset: 3 }));
  const { candidates } = traceRay(s, down(0.5, 0.5), 0xFF);
  assert.equal(candidates[0].functionTableOffset, 7);
});

test("an opaque geometry says so, since Metal calls no function for one", () => {
  const box = new Float32Array([0, 0, -0.5, 1, 1, 0.5]);
  const s = scene([instance()], boxGeometry(box, { opaque: true }));
  assert.equal(traceRay(s, down(0.5, 0.5), 0xFF).candidates[0].opaque, true);
});

test("a box the intersection function accepted becomes a bounding box hit at its distance", () => {
  const box = new Float32Array([0, 0, -0.5, 1, 1, 0.5]);
  const s = scene([instance({ index: 2, customIndex: 9 })], boxGeometry(box));
  const { candidates } = traceRay(s, down(0.5, 0.5), 0xFF);
  const hit = boxHit(s, candidates[0], 0.75);
  assert.equal(hit.type, INTERSECTION_BOUNDING_BOX);
  assert.equal(hit.distance, 0.75);
  assert.equal(hit.instanceId, 2);
  assert.equal(hit.userInstanceId, 9);
  assert.equal(hit.primitiveId, 0);
});

test("assume_geometry_type skips the kind it says the scene does not hold", () => {
  const box = new Float32Array([0, 0, -0.5, 1, 1, 0.5]);
  const s = scene([instance()], [
    { index: 0, triangles: UNIT_TRIANGLE, extents: null, functionTableOffset: 0, opaque: false },
    { index: 1, triangles: null, extents: box, functionTableOffset: 0, opaque: false },
  ]);
  const both = traceRay(s, down(0.25, 0.25), 0xFF);
  assert.equal(both.hit.type, INTERSECTION_TRIANGLE);
  assert.equal(both.candidates.length, 1);
  const onlyBoxes = traceRay(s, down(0.25, 0.25), 0xFF, { triangles: false });
  assert.equal(onlyBoxes.hit.type, INTERSECTION_NONE);
  assert.equal(onlyBoxes.candidates.length, 1);
  const onlyTriangles = traceRay(s, down(0.25, 0.25), 0xFF, { boundingBoxes: false });
  assert.equal(onlyTriangles.hit.type, INTERSECTION_TRIANGLE);
  assert.equal(onlyTriangles.candidates.length, 0);
});

// ------------------------------------------------------------------------------------ the maths

test("invert3x4 inverts rotation, scale and translation together", () => {
  const m = [0, -2, 0, 5, 2, 0, 0, -3, 0, 0, 2, 7];
  const inv = invert3x4(m);
  assert.ok(inv, "the transform is not singular");
  // Applying one then the other has to come back to the point it started at.
  const apply = (t, p) => [0, 1, 2].map((r) => t[r * 4] * p[0] + t[r * 4 + 1] * p[1] + t[r * 4 + 2] * p[2] + t[r * 4 + 3]);
  for (const p of [[1, 2, 3], [-4, 0.5, 9], [0, 0, 0]]) {
    const round = apply(inv, apply(m, p));
    for (let k = 0; k < 3; k++) {
      assert.ok(Math.abs(round[k] - p[k]) < 1e-9, `${p} came back as ${round}`);
    }
  }
});

test("invert3x4 refuses a singular transform rather than returning nonsense", () => {
  assert.equal(invert3x4([0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3]), null);
  // A scale of zero along one axis: a plane, with no inverse.
  assert.equal(invert3x4([1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0]), null);
});

test("intersectTriangle and intersectBox refuse a ray parallel to what they test", () => {
  // Along the triangle's own plane: the determinant is zero.
  assert.equal(intersectTriangle(UNIT_TRIANGLE, 0, [-1, 0.25, 0], [1, 0, 0], 0, 10), null);
  // Parallel to a box's x planes and outside them.
  const box = new Float32Array([0, 0, 0, 1, 1, 1]);
  assert.equal(intersectBox(box, 0, [2, 0.5, 0.5], [0, 0, 1], 0, 10), null);
  // Parallel and inside them: still a hit through the other axes.
  assert.ok(intersectBox(box, 0, [0.5, 0.5, -1], [0, 0, 1], 0, 10));
});
