// What an acceleration structure is made of and where its instances overlap
// (src/renderer/acceleration_tree.ts): the costs rolled up the tree, the search, and the pairs of
// instances whose boxes share space.
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
const dir = mkdtempSync(join(tmpdir(), "accel-tree-"));
const bundle = async (name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", "renderer", `${name}.ts`)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { structureTree, matchingKeys, instanceOverlaps, instanceBounds, groupStats, heatColor } = await bundle("acceleration_tree");
const { instanceScene } = await bundle("acceleration_structure");

/** A unit right triangle in the xy plane: area 0.5. */
const TRIANGLE = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
const translate = (x, y = 0, z = 0, s = 1) => [s, 0, 0, x, 0, s, 0, y, 0, 0, s, z];
const instance = (index, blas, transform) => ({ index, blas, transform, reference: "0", mask: 0xff, customIndex: 0, bindingTableOffset: 0, flagNames: [] });

/** A scene as structureDrawing would give it. */
function drawingOf(instances, partsOf) {
  const scene = instanceScene(instances, () => null, undefined, partsOf);
  return {
    positions: scene.mesh, kind: scene.kind, triangles: scene.triangles, lines: scene.lines, groups: scene.groups,
    shape: "triangles", instances, placed: scene.placed, note: "", fromCaptureStart: false,
  };
}

test("a scene keeps which geometry of which instance every run of vertices came from", () => {
  const parts = (blas) => (blas === 1 ? [{ geometry: 0, lines: false, positions: TRIANGLE }, { geometry: 1, lines: false, positions: TRIANGLE }] : null);
  const scene = instanceScene([instance(0, 1, translate(0)), instance(1, 2, translate(5))], () => null, undefined, parts);
  assert.deepEqual(scene.groups.map((g) => [g.instance, g.geometry, g.lines, g.first, g.count]), [
    [0, 0, false, 0, 3], [0, 1, false, 3, 3],
    // The second names a bottom level with nothing captured: a stand-in box, among the lines.
    [1, -1, true, 0, 24],
  ]);
  assert.equal(scene.triangles.length, 18);
  assert.equal(scene.lines.length, 72);
  assert.equal(scene.placed, 1);
});

test("the tree rolls primitives, world-space area and memory up to the top level", () => {
  const parts = (blas) => (blas === 1 ? [{ geometry: 0, lines: false, positions: TRIANGLE }] : null);
  // The second instance is scaled by 2, so its triangle has four times the area.
  const drawing = drawingOf([instance(0, 1, translate(0)), instance(1, 1, translate(5, 0, 0, 2))], parts);
  const facts = (id) => ({ 10: { name: "tlas", memory: 1000, primitives: 2 }, 1: { name: "rock", memory: 4096, primitives: 1 } })[id] ?? null;
  const tree = structureTree(drawing, 10, facts);
  assert.equal(tree.kind, "tlas");
  assert.equal(tree.primitives, 2);
  assert.ok(Math.abs(tree.area - 2.5) < 1e-6, `area ${tree.area}`);
  // A bottom level placed twice costs its memory once.
  assert.equal(tree.memory, 5096);
  const [first, second] = tree.children;
  assert.equal(first.kind, "instance");
  assert.equal(first.children[0].kind, "blas");
  assert.equal(first.children[0].label, "rock");
  assert.equal(first.children[0].children[0].kind, "geometry");
  assert.ok(Math.abs(second.area - 2) < 1e-6);
});

test("an instance whose bottom level was not captured takes its primitive count from the build", () => {
  const drawing = drawingOf([instance(0, 7, translate(0))], () => null);
  const tree = structureTree(drawing, 10, (id) => (id === 7 ? { name: "far", memory: null, primitives: 1234 } : null));
  const row = tree.children[0];
  assert.equal(row.primitives, 1234);
  assert.equal(row.area, null);
  assert.match(row.note, /not in this capture/);
  assert.equal(row.children[0].children.length, 0, "no geometry rows for geometry that is not here");
});

test("a search keeps the matching rows, their ancestors and what they hold", () => {
  const parts = (blas) => [{ geometry: 0, lines: false, positions: TRIANGLE }];
  const drawing = drawingOf([instance(0, 1, translate(0)), instance(1, 2, translate(5))], parts);
  const names = { 1: "rock", 2: "tree" };
  const tree = structureTree(drawing, 10, (id) => (names[id] ? { name: names[id], memory: null, primitives: 1 } : null));
  const keys = matchingKeys(tree, "TREE");
  assert.ok(keys.has("root") && keys.has("i1") && keys.has("i1b") && keys.has("i1g1"));
  assert.ok(!keys.has("i0"));
  assert.equal(matchingKeys(tree, "").size, 1 + 2 * 3);
});

test("overlapping instance boxes are found, the most overlapped first, and counted per instance", () => {
  const box = (x0, x1) => ({ min: [x0, 0, 0], max: [x1, 1, 1] });
  const report = instanceOverlaps([box(0, 2), box(1, 3), box(10, 11), box(1.5, 1.7), null]);
  assert.equal(report.total, 3);
  assert.deepEqual(report.pairs.map((p) => [p.a, p.b]), [[0, 3], [1, 3], [0, 1]]);
  assert.equal(report.pairs[0].fraction, 1, "a box wholly inside another overlaps it entirely");
  assert.ok(Math.abs(report.pairs[2].fraction - 0.5) < 1e-6);
  assert.deepEqual(report.counts, [2, 2, 0, 2, 0]);
  assert.equal(report.unknown, 1);
});

test("boxes that only touch do not overlap, but two flat boxes in one plane do", () => {
  const touching = instanceOverlaps([{ min: [0, 0, 0], max: [1, 1, 1] }, { min: [1, 0, 0], max: [2, 1, 1] }]);
  assert.equal(touching.total, 0);
  const planes = instanceOverlaps([{ min: [0, 0, 0], max: [2, 0, 2] }, { min: [1, 0, 1], max: [3, 0, 3] }]);
  assert.equal(planes.total, 1);
});

test("an instance's world box comes from its placed geometry, and a stand-in has none", () => {
  const parts = (blas) => (blas === 1 ? [{ geometry: 0, lines: false, positions: TRIANGLE }] : null);
  const drawing = drawingOf([instance(0, 1, translate(3)), instance(1, 9, translate(0))], parts);
  const bounds = instanceBounds(drawing);
  assert.deepEqual(bounds[0], { min: [3, 0, 0], max: [4, 1, 0] });
  assert.equal(bounds[1], null);
  const stats = groupStats(drawing, drawing.groups[0]);
  assert.equal(stats.primitives, 1);
});

test("the heat runs from blue to red", () => {
  const [r0, , b0] = heatColor(0);
  const [r1, , b1] = heatColor(1);
  assert.ok(b0 > r0 && r1 > b1);
});
