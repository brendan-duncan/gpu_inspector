// Per-heap memory use (src/renderer/memory_heaps.ts): derived from the object graph's allocations
// and the device's heaps, with the driver's residency where it reported one (VK_EXT_memory_budget,
// attached by src/vulkan/src/cpu_timeline.cpp).
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "memheaps-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { memoryHeaps, usedHeaps, heapPressure, HEAP_PRESSURE_SHARE } = await load("renderer/memory_heaps.ts", "memory_heaps");

const MB = 1048576;

/** A database of a physical device plus allocations, as [sizeBytes, memoryTypeIndex] pairs. */
function db(allocations, { heaps, types, budget } = {}) {
  const objects = new Map();
  objects.set(1, {
    id: 1, type: "VkPhysicalDevice", isDeleted: false, args: {},
    updates: {
      memoryProperties: {
        memoryHeapCount: (heaps ?? []).length, memoryTypeCount: (types ?? []).length,
        // Vulkan's arrays are fixed length; entries past the count are padding and must be ignored.
        memoryHeaps: [...(heaps ?? []), { size: 0, flags: "0" }, { size: 0, flags: "0" }],
        memoryTypes: [...(types ?? []), { heapIndex: 0, propertyFlags: "0" }],
      },
      ...(budget ? { memoryBudget: budget } : {}),
    },
  });
  allocations.forEach(([allocationSize, memoryTypeIndex], i) => {
    objects.set(100 + i, {
      id: 100 + i, type: "VkDeviceMemory", isDeleted: false, updates: {},
      args: { pAllocateInfo: { allocationSize, memoryTypeIndex } },
    });
  });
  return { allObjects: objects };
}

const DEVICE_HEAPS = [{ size: 1000 * MB, flags: "VK_MEMORY_HEAP_DEVICE_LOCAL_BIT" }, { size: 500 * MB, flags: "0" }];
const DEVICE_TYPES = [
  { heapIndex: 0, propertyFlags: "VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT" },
  { heapIndex: 1, propertyFlags: "VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT" },
];

test("a device with no reported memory properties has no heap view", () => {
  assert.equal(memoryHeaps({ allObjects: new Map() }), null);
});

test("allocations are totalled against the heap their memory type draws from", () => {
  const m = memoryHeaps(db([[100 * MB, 0], [50 * MB, 0], [8 * MB, 1]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES }));
  assert.equal(m.allocations, 3);
  assert.equal(m.totalBytes, 158 * MB);
  assert.equal(m.heaps[0].bytes, 150 * MB);
  assert.equal(m.heaps[0].allocations, 2);
  assert.equal(m.heaps[0].largestBytes, 100 * MB);
  assert.equal(m.heaps[1].bytes, 8 * MB);
  assert.equal(m.heaps[0].deviceLocal, true);
  assert.equal(m.heaps[1].deviceLocal, false);
});

test("the share is of the heap's own size, so the same bytes read differently per heap", () => {
  // 150 MB is nothing in a 1 GB heap and a third of a 500 MB one: a single total cannot say that.
  const m = memoryHeaps(db([[150 * MB, 0], [150 * MB, 1]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES }));
  assert.ok(Math.abs(m.heaps[0].share - 0.15) < 1e-9);
  assert.ok(Math.abs(m.heaps[1].share - 0.3) < 1e-9);
});

test("padding entries past the reported counts are ignored", () => {
  const m = memoryHeaps(db([[1 * MB, 0]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES }));
  assert.equal(m.heaps.length, 2, "not the padded array length");
});

test("a destroyed allocation no longer counts", () => {
  const d = db([[100 * MB, 0], [100 * MB, 0]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES });
  d.allObjects.get(101).isDeleted = true;
  const m = memoryHeaps(d);
  assert.equal(m.allocations, 1);
  assert.equal(m.totalBytes, 100 * MB);
});

test("types are listed under their heap, largest first", () => {
  const types = [
    { heapIndex: 0, propertyFlags: "DEVICE_LOCAL" },
    { heapIndex: 0, propertyFlags: "DEVICE_LOCAL | HOST_VISIBLE" },
  ];
  const m = memoryHeaps(db([[10 * MB, 0], [90 * MB, 1]], { heaps: DEVICE_HEAPS, types }));
  assert.equal(m.heaps[0].types.length, 2);
  assert.equal(m.heaps[0].types[0].index, 1, "the larger type first");
  assert.equal(m.heaps[0].types[0].bytes, 90 * MB);
});

test("the driver's residency is carried when it reported one", () => {
  const budget = { heapBudget: [800 * MB, 400 * MB], heapUsage: [300 * MB, 20 * MB] };
  const m = memoryHeaps(db([[100 * MB, 0]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES, budget }));
  assert.equal(m.hasBudget, true);
  assert.equal(m.heaps[0].budgetBytes, 800 * MB);
  // More resident than this application allocated: the driver's own overhead and other processes.
  assert.equal(m.heaps[0].usageBytes, 300 * MB);
});

test("without the budget extension there is still a heap view, just no residency", () => {
  const m = memoryHeaps(db([[100 * MB, 0]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES }));
  assert.equal(m.hasBudget, false);
  assert.equal(m.heaps[0].budgetBytes, undefined);
});

test("a heap the application is filling is flagged", () => {
  const m = memoryHeaps(db([[900 * MB, 0]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES }));
  const pressure = heapPressure(m);
  assert.equal(pressure.length, 1);
  assert.equal(pressure[0].index, 0);
  assert.ok(pressure[0].share >= HEAP_PRESSURE_SHARE);
});

test("a heap the driver says is nearly spent is flagged even when we hold little of it", () => {
  // Another process filling the GPU is still a problem for this one, and only the driver can see it.
  const budget = { heapBudget: [1000 * MB, 400 * MB], heapUsage: [950 * MB, 10 * MB] };
  const m = memoryHeaps(db([[10 * MB, 0]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES, budget }));
  const pressure = heapPressure(m);
  assert.equal(pressure.length, 1);
  assert.ok(pressure[0].share < HEAP_PRESSURE_SHARE, "ours is small; the driver's view is what flagged it");
});

test("only heaps drawn from are listed, largest first", () => {
  const m = memoryHeaps(db([[8 * MB, 1], [100 * MB, 0]], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES }));
  const used = usedHeaps(m);
  assert.equal(used.length, 2);
  assert.equal(used[0].index, 0);
  const none = memoryHeaps(db([], { heaps: DEVICE_HEAPS, types: DEVICE_TYPES }));
  assert.equal(usedHeaps(none).length, 0);
});
