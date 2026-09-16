// What an acceleration structure was built from (src/renderer/acceleration_structure.ts): the
// instance array a top level reads, and the geometry a bottom level was given.
//
// The instance layout is fixed by the Vulkan specification (VkAccelerationStructureInstanceKHR, 64
// bytes) with two bit-packed words, so it is worth pinning against bytes rather than a builder.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "accel-"));
const out = join(dir, "acceleration_structure.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "acceleration_structure.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { parseInstances, parseBuild, instancePosition, isIdentity, INSTANCE_STRIDE } =
  await import(pathToFileURL(out).href);

/** One VkAccelerationStructureInstanceKHR as the 64 bytes a build would read. */
function instanceBytes({ transform, customIndex = 0, mask = 0xff, offset = 0, flags = 0, reference = 0n }) {
  const b = new Uint8Array(INSTANCE_STRIDE);
  const v = new DataView(b.buffer);
  const t = transform ?? [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];
  t.forEach((n, i) => v.setFloat32(i * 4, n, true));
  v.setUint32(48, (customIndex & 0xffffff) | ((mask & 0xff) << 24), true);
  v.setUint32(52, (offset & 0xffffff) | ((flags & 0xff) << 24), true);
  v.setBigUint64(56, reference, true);
  return b;
}

const concat = (...arrays) => {
  const total = arrays.reduce((n, a) => n + a.byteLength, 0);
  const b = new Uint8Array(total);
  let at = 0;
  for (const a of arrays) { b.set(a, at); at += a.byteLength; }
  return b;
};

test("an empty buffer holds no instances", () => {
  assert.deepEqual(parseInstances(new Uint8Array(0)), []);
});

test("the two bit-packed words come apart the way the specification packs them", () => {
  // customIndex is the low 24 bits and mask the high 8 of one word; the binding table offset and
  // flags are packed the same way in the next.
  const b = instanceBytes({ customIndex: 0x123456, mask: 0xab, offset: 0x654321, flags: 0x05 });
  const [i] = parseInstances(b);
  assert.equal(i.customIndex, 0x123456);
  assert.equal(i.mask, 0xab);
  assert.equal(i.bindingTableOffset, 0x654321);
  assert.equal(i.flags, 0x05);
  assert.deepEqual(i.flagNames, ["TRIANGLE_FACING_CULL_DISABLE", "FORCE_OPAQUE"]);
});

test("the transform is kept row-major, as Vulkan stores it", () => {
  // A translation to (5, 6, 7): Vulkan's 3x4 is row-major, so the offsets are the last of each row.
  const t = [1, 0, 0, 5, 0, 1, 0, 6, 0, 0, 1, 7];
  const [i] = parseInstances(instanceBytes({ transform: t }));
  assert.deepEqual(i.transform, t);
  assert.deepEqual(instancePosition(i), [5, 6, 7]);
  assert.equal(isIdentity(i.transform), false);
  assert.equal(isIdentity([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]), true);
});

test("a 64-bit reference survives as a string, since a number would not hold it", () => {
  const big = 0x0000123456789abcn;
  const [i] = parseInstances(instanceBytes({ reference: big }));
  assert.equal(i.reference, big.toString());
  assert.equal(Number(i.reference) > Number.MAX_SAFE_INTEGER, false, "this one fits; the point is it is exact");
  const huge = 0x7fffffffffffffffn;
  const [j] = parseInstances(instanceBytes({ reference: huge }));
  assert.equal(j.reference, huge.toString(), "exact past 2^53, which a float would round");
});

test("a reference the layer resolved names the bottom level it points at", () => {
  const refs = new Map([["4096", 77]]);
  const [i] = parseInstances(instanceBytes({ reference: 4096n }), refs);
  assert.equal(i.blas, 77);
  const [j] = parseInstances(instanceBytes({ reference: 9999n }), refs);
  assert.equal(j.blas, undefined, "an address the layer never saw is left unresolved");
});

test("several instances come out in order", () => {
  const b = concat(instanceBytes({ customIndex: 1 }), instanceBytes({ customIndex: 2 }), instanceBytes({ customIndex: 3 }));
  const all = parseInstances(b);
  assert.equal(all.length, 3);
  assert.deepEqual(all.map((i) => i.customIndex), [1, 2, 3]);
  assert.deepEqual(all.map((i) => i.index), [0, 1, 2]);
});

test("a read-back cut short by the buffer limit yields the whole instances it holds", () => {
  // Half an instance is not worth guessing at; two whole ones are still worth showing.
  const b = concat(instanceBytes({ customIndex: 1 }), instanceBytes({ customIndex: 2 }), new Uint8Array(20));
  const all = parseInstances(b);
  assert.equal(all.length, 2);
});

// ---------------------------------------------------------------------------------------------
// The build arguments, as the layer records them.

const TRIANGLE_GEOMETRY = {
  sType: "VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR",
  geometryType: "VK_GEOMETRY_TYPE_TRIANGLES_KHR",
  flags: "VK_GEOMETRY_OPAQUE_BIT_KHR",
  geometry: {
    triangles: {
      vertexFormat: "VK_FORMAT_R32G32B32_SFLOAT", vertexStride: 12, maxVertex: 2,
      indexType: "VK_INDEX_TYPE_NONE_KHR",
      vertexData: { deviceAddress: 1234, capture: 9 },
    },
  },
};

test("a bottom-level build reports its geometry and the buffers the layer captured", () => {
  const b = parseBuild({
    type: "VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR",
    mode: "VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR",
    flags: "0",
    dstAccelerationStructure: { __id: 65, __class: "VkAccelerationStructureKHR" },
    pGeometries: [TRIANGLE_GEOMETRY],
  }, [{ primitiveCount: 1 }]);
  assert.equal(b.target, 65);
  assert.equal(b.topLevel, false);
  assert.equal(b.mode, "BUILD");
  assert.equal(b.primitives, 1);
  assert.equal(b.geometries[0].kind, "triangles");
  assert.equal(b.geometries[0].vertexFormat, "R32G32B32_SFLOAT");
  assert.equal(b.geometries[0].vertexData, 9, "the captured contents of the vertices it read");
  assert.equal(b.geometries[0].indexData, undefined, "this build is not indexed");
});

test("a top-level build is marked as one and counts its instances", () => {
  const b = parseBuild({
    type: "VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR",
    mode: "VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR",
    flags: "0",
    dstAccelerationStructure: { __id: 65, __class: "VkAccelerationStructureKHR" },
    pGeometries: [{
      geometryType: "VK_GEOMETRY_TYPE_INSTANCES_KHR", flags: "VK_GEOMETRY_OPAQUE_BIT_KHR",
      geometry: { instances: { arrayOfPointers: false, data: { deviceAddress: 99, capture: 4 } } },
    }],
  }, [{ primitiveCount: 3 }]);
  assert.equal(b.topLevel, true);
  assert.equal(b.mode, "UPDATE");
  assert.equal(b.geometries[0].kind, "instances");
  assert.equal(b.geometries[0].instanceData, 4);
  assert.equal(b.primitives, 3, "a top level's primitives are its instances");
});

test("an address the layer could not resolve leaves no capture id", () => {
  const b = parseBuild({
    type: "VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR",
    mode: "VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR", flags: "0",
    dstAccelerationStructure: { __id: 1, __class: "VkAccelerationStructureKHR" },
    pGeometries: [{
      geometryType: "VK_GEOMETRY_TYPE_TRIANGLES_KHR", flags: "0",
      geometry: { triangles: { vertexData: { deviceAddress: 1234 } } },
    }],
  }, [{ primitiveCount: 1 }]);
  assert.equal(b.geometries[0].vertexData, undefined);
});

test("arguments that are not a build's give nothing rather than throwing", () => {
  assert.equal(parseBuild(null, null), null);
  assert.equal(parseBuild({}, null), null, "no destination structure");
});

// ---------------------------------------------------------------------------------------------
// Drawing what was built.

const { triangleMesh, instanceScene, transformPoint } = await import(pathToFileURL(out).href);

/** Tightly packed R32G32B32_SFLOAT vertices. */
function vertexBytes(...xyz) {
  const f = new Float32Array(xyz);
  return new Uint8Array(f.buffer.slice(0));
}

const TRI_GEOMETRY = {
  index: 0, kind: "triangles", flags: "0", primitiveCount: 1,
  vertexFormat: "R32G32B32_SFLOAT", vertexStride: 12, maxVertex: 2, indexType: "NONE",
};

test("a bottom level's triangles come out as one position per vertex", () => {
  const v = vertexBytes(-0.5, -0.5, 0, 0.5, -0.5, 0, 0, 0.5, 0);
  const m = triangleMesh(TRI_GEOMETRY, v, null);
  assert.equal(m.length, 9, "three vertices of three floats");
  assert.deepEqual([...m], [-0.5, -0.5, 0, 0.5, -0.5, 0, 0, 0.5, 0]);
});

test("an indexed build is expanded through its indices", () => {
  const v = vertexBytes(0, 0, 0, 1, 0, 0, 0, 1, 0);
  const idx = new Uint8Array(new Uint16Array([2, 1, 0]).buffer.slice(0));
  const m = triangleMesh({ ...TRI_GEOMETRY, indexType: "UINT16" }, v, idx);
  assert.deepEqual([...m], [0, 1, 0, 1, 0, 0, 0, 0, 0], "vertices in index order");
});

test("a build whose vertices were not captured draws nothing rather than guessing", () => {
  assert.equal(triangleMesh(TRI_GEOMETRY, null, null), null);
  assert.equal(triangleMesh({ ...TRI_GEOMETRY, kind: "instances" }, vertexBytes(0, 0, 0), null), null);
  assert.equal(triangleMesh({ ...TRI_GEOMETRY, vertexFormat: "NOT_A_FORMAT" }, vertexBytes(0, 0, 0), null), null);
});

test("a read-back cut short does not read past what it holds", () => {
  // One vertex captured of the three the build declares: the rest are origin rather than garbage.
  const m = triangleMesh(TRI_GEOMETRY, vertexBytes(1, 2, 3), null);
  assert.deepEqual([...m.slice(0, 3)], [1, 2, 3]);
  assert.deepEqual([...m.slice(3)], [0, 0, 0, 0, 0, 0]);
});

test("a point goes through the row-major 3x4 the way Vulkan means it", () => {
  // Translation by (10, 20, 30) with an identity rotation.
  const t = [1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30];
  assert.deepEqual(transformPoint(t, 1, 2, 3), [11, 22, 33]);
  // A 90-degree rotation about z: x becomes y.
  const r = [0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0];
  assert.deepEqual(transformPoint(r, 1, 0, 0), [0, 1, 0]);
});

test("an instance whose bottom level was captured is placed by its transform", () => {
  const geometry = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const instances = parseInstances(instanceBytes({ transform: [1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0], reference: 7n }),
                                   new Map([["7", 42]]));
  const scene = instanceScene(instances, (id) => (id === 42 ? geometry : null));
  assert.equal(scene.kind, "triangles");
  assert.equal(scene.placed, 1);
  assert.deepEqual([...scene.mesh.slice(0, 3)], [5, 0, 0], "moved along x by the transform");
});

test("an instance whose bottom level was not captured is drawn as a box where it sits", () => {
  // The common case: a bottom level is built once, before any capture, so its geometry is absent —
  // but where the instances are and how many there are is still worth showing.
  const instances = parseInstances(instanceBytes({ transform: [1, 0, 0, 3, 0, 1, 0, 0, 0, 0, 1, 0] }));
  const scene = instanceScene(instances, () => null);
  assert.equal(scene.kind, "lines");
  assert.equal(scene.placed, 0);
  assert.equal(scene.mesh.length, 12 * 2 * 3, "twelve edges of a cube, two ends, three floats");
  // Centred on the instance's position rather than the origin.
  const xs = [...scene.mesh].filter((_, i) => i % 3 === 0);
  assert.ok(Math.min(...xs) > 2 && Math.max(...xs) < 4);
});

test("geometry wins over boxes when any instance has it", () => {
  const geometry = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const two = concat(instanceBytes({ reference: 7n }), instanceBytes({ reference: 8n }));
  const scene = instanceScene(parseInstances(two, new Map([["7", 42]])), (id) => (id === 42 ? geometry : null));
  assert.equal(scene.kind, "triangles");
  assert.equal(scene.placed, 1, "one of the two had geometry");
});
