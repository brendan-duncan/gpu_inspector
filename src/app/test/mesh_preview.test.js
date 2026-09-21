// The mesh preview's colorings and normals (src/renderer/mesh_preview.ts): what an attribute looks
// like as a color, and the smooth normals of geometry that has none of its own.
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
const out = join(mkdtempSync(join(tmpdir(), "preview-")), "mesh_preview.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "mesh_preview.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { attributeColors, smoothNormals, SHADE_MODES } = await import(pathToFileURL(out).href);

const attribute = (values, components, isColor = false) => ({
  name: "a", components, isColor, read: (v) => values.slice(v * components, v * components + components),
});

test("a color attribute is taken as it is", () => {
  const colors = attributeColors(attribute([1, 0, 0, 0, 0.5, 1], 3), 2);
  assert.deepEqual([...colors], [1, 0, 0, 0, 0.5, 1]);
});

test("anything outside 0 to 1 is stretched over its own range, per component, and a constant one sits in the middle", () => {
  const colors = attributeColors(attribute([-1, 10, 5, 1, 20, 5], 3), 2);
  assert.deepEqual([...colors].map((x) => Math.round(x * 100) / 100), [0, 0, 0.5, 1, 1, 0.5]);
});

test("a one-component attribute reads as gray", () => {
  const colors = attributeColors(attribute([0.25], 1), 1);
  assert.deepEqual([...colors], [0.25, 0.25, 0.25]);
});

test("smooth normals average the faces that share a position", () => {
  // Two triangles folded along the x axis: one in the xy plane facing +z, one in the xz plane facing -y.
  const drawn = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  const normals = smoothNormals(drawn, new Uint8Array(6).fill(1));
  const at = (v) => [...normals.subarray(v * 3, v * 3 + 3)].map((x) => Math.round(x * 1000) / 1000);
  // The fold's vertices are shared: their normal is halfway between the faces'.
  assert.deepEqual(at(0), [0, -0.707, 0.707]);
  assert.deepEqual(at(1), [0, -0.707, 0.707]);
  // The others belong to one face each.
  assert.deepEqual(at(2), [0, 0, 1]);
  assert.deepEqual(at(5), [0, -1, 0]);
});

test("every shading mode says whether it fills", () => {
  assert.deepEqual(SHADE_MODES.map((m) => `${m.value}:${m.fills}`),
    ["wireframe:false", "solid:true", "wire-solid:true", "flat:true", "smooth:true", "points:false"]);
});
