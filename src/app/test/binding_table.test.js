// Which shader group each binding table record holds (src/renderer/binding_table.ts): the records
// a trace reads, matched against the handles the driver gave the pipeline's groups.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "sbt-"));
const out = join(dir, "binding_table.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "binding_table.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { bindingTableRecords, unresolvedRecords } = await import(pathToFileURL(out).href);

const HANDLE = 32;

/** A distinct handle for group `g`, the shape a driver hands out. */
const handleFor = (g) => {
  const b = new Uint8Array(HANDLE);
  b.fill(0xa0 + g);
  b[0] = g + 1;
  return b;
};

/** The pipeline's handle blob: one handle per group, in group order. */
function handleBlob(groups) {
  const b = new Uint8Array(groups * HANDLE);
  for (let g = 0; g < groups; g++) b.set(handleFor(g), g * HANDLE);
  return b;
}

/** A region's contents: records of `stride`, each beginning with the named group's handle. */
function regionBytes(groupsInOrder, stride) {
  const b = new Uint8Array(groupsInOrder.length * stride);
  groupsInOrder.forEach((g, i) => b.set(handleFor(g), i * stride));
  return b;
}

test("nothing to walk gives no records", () => {
  assert.deepEqual(bindingTableRecords("raygen", null, 64, handleBlob(3), HANDLE), []);
  assert.deepEqual(bindingTableRecords("raygen", regionBytes([0], 64), 0, handleBlob(3), HANDLE), []);
  assert.deepEqual(bindingTableRecords("raygen", regionBytes([0], 64), 64, handleBlob(3), 0), []);
});

test("a record is matched to the group whose handle it holds", () => {
  const r = bindingTableRecords("raygen", regionBytes([0], 64), 64, handleBlob(3), HANDLE);
  assert.equal(r.length, 1);
  assert.equal(r[0].group, 0);
  assert.equal(r[0].region, "raygen");
  assert.equal(r[0].index, 0);
});

test("records are walked by the region's stride, not by the handle size", () => {
  // A 64-byte stride with 32-byte handles leaves 32 bytes of the application's own data per record.
  const r = bindingTableRecords("hit", regionBytes([2, 1, 0], 64), 64, handleBlob(3), HANDLE);
  assert.deepEqual(r.map((x) => x.group), [2, 1, 0], "in table order, not group order");
  assert.deepEqual(r.map((x) => x.index), [0, 1, 2]);
  assert.equal(r[0].dataBytes, 32, "the shader record data after the handle");
});

test("a tightly packed table has no record data", () => {
  const r = bindingTableRecords("miss", regionBytes([1], HANDLE), HANDLE, handleBlob(3), HANDLE);
  assert.equal(r[0].dataBytes, 0);
});

test("a handle matching no group is reported rather than dropped", () => {
  // The interesting failure: a table filled from the wrong pipeline, or from handles fetched before
  // it was rebuilt. Those rays run the wrong shader or none, and nothing else would show it.
  const contents = new Uint8Array(64);
  contents.fill(0x7f, 0, HANDLE);
  const r = bindingTableRecords("raygen", contents, 64, handleBlob(3), HANDLE);
  assert.equal(r.length, 1);
  assert.equal(r[0].group, null);
  assert.equal(r[0].handle, "7f".repeat(HANDLE), "the handle is shown when it names nothing");
  assert.deepEqual(unresolvedRecords(r).map((x) => x.index), [0]);
});

test("with no handles captured the records still list, unresolved", () => {
  // An application that never called vkGetRayTracingShaderGroupHandlesKHR while watched.
  const r = bindingTableRecords("raygen", regionBytes([0, 1], 64), 64, null, HANDLE);
  assert.equal(r.length, 2);
  assert.deepEqual(r.map((x) => x.group), [null, null]);
  assert.equal(unresolvedRecords(r).length, 2);
});

test("a read-back cut short stops at the last whole handle", () => {
  // The capture's buffer limit can cut the region; a partial handle is not matched against anything.
  const full = regionBytes([0, 1], 64);
  const cut = full.subarray(0, 64 + 10);
  const r = bindingTableRecords("hit", cut, 64, handleBlob(3), HANDLE);
  assert.equal(r.length, 1, "the second record's handle is not all there");
  assert.equal(r[0].group, 0);
});

test("a resolved table reports nothing unresolved", () => {
  const r = bindingTableRecords("hit", regionBytes([0, 1, 2], 64), 64, handleBlob(3), HANDLE);
  assert.deepEqual(unresolvedRecords(r), []);
});
