// Metal's memory breakdown (src/renderer/metal/metal_memory.ts). Metal has no heap table, so the
// breakdown is by object kind, totaled from each resource's allocatedSize. The trap the tests are
// really about is double counting: a resource made from a heap is suballocated out of memory the
// heap already reserved, so adding both would count those bytes twice.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "metalmem-")), "metal_memory.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "metal", "metal_memory.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { metalMemory, heapOccupancy, HEAP_OCCUPANCY_LOW } = await import(pathToFileURL(out).href);

const ref = (id) => ({ __id: id, __class: "MTLHeap" });
/** One tracked object; `args` is what the library recorded at creation. */
const obj = (id, type, args, isDeleted = false, updates = {}) => [id, { id, type, args, isDeleted, updates }];
const db = (...objects) => ({ allObjects: new Map(objects) });

test("a session with no Metal resources has no breakdown", () => {
  assert.equal(metalMemory(db()), null);
  assert.equal(metalMemory(db(obj(1, "VkImage", { allocatedSize: 100 }))), null, "not a Metal object");
});

test("standalone buffers and textures are grouped by kind, largest group first", () => {
  const m = metalMemory(db(
    obj(1, "MTLBuffer", { allocatedSize: 1000 }),
    obj(2, "MTLBuffer", { allocatedSize: 3000 }),
    obj(3, "MTLTexture", { allocatedSize: 500 }),
  ));
  assert.equal(m.totalBytes, 4500);
  assert.equal(m.groups[0].label, "Buffers");
  assert.equal(m.groups[0].count, 2);
  assert.equal(m.groups[0].bytes, 4000);
  assert.equal(m.groups[0].largestBytes, 3000);
  assert.equal(m.groups[1].label, "Textures");
});

test("a resource inside a heap is not counted twice", () => {
  // The heap reserved 8000; the texture is 2000 of that, not 2000 more.
  const m = metalMemory(db(
    obj(10, "MTLHeap", { allocatedSize: 8000, usedSize: 2000 }),
    obj(11, "MTLTexture", { allocatedSize: 2000, heap: ref(10) }),
  ));
  assert.equal(m.totalBytes, 8000, "the heap's reservation, not the heap plus its contents");
  assert.equal(m.groups.length, 1);
  assert.equal(m.groups[0].label, "Heaps");
  assert.equal(m.inHeaps.count, 1);
  assert.equal(m.inHeaps.bytes, 2000, "reported, but apart from the total");
});

test("heaps and standalone resources add up together", () => {
  const m = metalMemory(db(
    obj(10, "MTLHeap", { allocatedSize: 8000, usedSize: 6000 }),
    obj(11, "MTLTexture", { allocatedSize: 6000, heap: ref(10) }),
    obj(12, "MTLBuffer", { allocatedSize: 1500 }),
  ));
  assert.equal(m.totalBytes, 9500, "8000 reserved plus the 1500 that is not in it");
  assert.equal(heapOccupancy(m), 0.75);
});

test("destroyed objects are not counted", () => {
  const m = metalMemory(db(
    obj(1, "MTLBuffer", { allocatedSize: 1000 }),
    obj(2, "MTLBuffer", { allocatedSize: 9000 }, true),
  ));
  assert.equal(m.totalBytes, 1000);
  assert.equal(m.groups[0].count, 1);
});

test("a resource the library recorded no size for counts as an object but no bytes", () => {
  // Older libraries, and any resource that does not answer allocatedSize.
  const m = metalMemory(db(obj(1, "MTLBuffer", {})));
  assert.equal(m.totalBytes, 0);
  assert.equal(m.groups[0].count, 1);
});

test("occupancy is null when nothing reserved, and low occupancy is what gets reported", () => {
  assert.equal(heapOccupancy(metalMemory(db(obj(1, "MTLBuffer", { allocatedSize: 10 })))), null);
  const mostlyEmpty = metalMemory(db(obj(10, "MTLHeap", { allocatedSize: 10000, usedSize: 1000 })));
  assert.ok(heapOccupancy(mostlyEmpty) < HEAP_OCCUPANCY_LOW, "a tenth used is worth saying");
  const full = metalMemory(db(obj(10, "MTLHeap", { allocatedSize: 10000, usedSize: 9000 })));
  assert.ok(heapOccupancy(full) > HEAP_OCCUPANCY_LOW);
});


test("a heap's usage comes from its updates, not from what it was created with", () => {
  // The library refreshes usedSize every time a resource is made from the heap. A heap has used
  // none of itself at the moment it is created, so reading the creation arguments would report
  // every heap in every session as entirely empty.
  //
  // The updates are flat, which is the shape the wire has: the library groups the two fields under
  // a key ("usage") for its own bookkeeping, but an ObjectUpdate sends them beside the id and the
  // database merges them into `updates` individually. Written nested, these cases passed against a
  // reader that could never see a real heap's usage at all.
  const m = metalMemory(db(obj(10, "MTLHeap", { allocatedSize: 8000, usedSize: 0 }, false,
                              { usedSize: 7000, currentAllocatedSize: 8000 })));
  assert.equal(m.heapUsedBytes, 7000);
  assert.equal(heapOccupancy(m), 0.875);
});

test("a heap that grew is counted at its current size", () => {
  const m = metalMemory(db(obj(10, "MTLHeap", { allocatedSize: 4000 }, false,
                              { usedSize: 5000, currentAllocatedSize: 9000 })));
  assert.equal(m.totalBytes, 9000, "what it holds now, not what it was asked for");
});

test("a heap that has reported nothing since it was created falls back to its arguments", () => {
  // No update yet: the creation figures are all there is, and a heap created full (one made to
  // hold a single resource placed at once) should not read as empty.
  const m = metalMemory(db(obj(10, "MTLHeap", { allocatedSize: 8000, usedSize: 0 })));
  assert.equal(m.heapReservedBytes, 8000);
  assert.equal(m.heapUsedBytes, 0);
});
