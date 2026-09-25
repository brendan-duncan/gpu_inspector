// The heaps a D3D12 draw's shaders index directly, as of its submission
// (src/renderer/d3d12/indexed_heap.ts).
//
// The capture library sends a directly indexed heap's slots on each ExecuteCommandLists that uses
// it, only those written since it last sent them, so a draw's view of the heap is every earlier
// submission's entries applied in order. Getting that fold wrong shows a draw the contents of a
// later frame's descriptors, or none at all.
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
const out = join(mkdtempSync(join(tmpdir(), "heap-")), "indexed_heap.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "d3d12", "indexed_heap.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { indexedHeapsAt } = await import(pathToFileURL(out).href);

const LIST = 11, QUEUE = 3, HEAP = 15, SAMPLER_HEAP = 16, BINDLESS_ROOT = 29, PLAIN_ROOT = 30;
const ref = (id, cls) => ({ __id: id, __class: cls });
const objects = {
  [BINDLESS_ROOT]: { args: { pDesc: { Version: "D3D_ROOT_SIGNATURE_VERSION_1_1", Desc_1_1: { Flags: "D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED" } } } },
  [PLAIN_ROOT]: { args: { pDesc: { Version: "D3D_ROOT_SIGNATURE_VERSION_1_1", Desc_1_1: { Flags: "D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT" } } } },
  [HEAP]: { args: { pDesc: { Type: "D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV" } } },
  [SAMPLER_HEAP]: { args: { pDesc: { Type: "D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER" } } },
};
const objectOf = (id) => objects[id] ?? null;
const texture = (id) => ({ resource: ref(id, "ID3D12Resource"), view: { ViewDimension: "D3D12_SRV_DIMENSION_TEXTURE2D" } });

/** A frame of submissions, each of one list recording [Reset, SetDescriptorHeaps, SetGraphicsRootSignature, ...body, Close]. */
function frame(submissions) {
  const commands = [];
  const push = (c) => commands.push({ index: commands.length, frame: 0, args: null, ...c });
  for (const s of submissions) {
    push({ method: "ExecuteCommandLists", object: ref(QUEUE, "ID3D12CommandQueue"), heapDescriptors: s.heapDescriptors });
    const list = ref(LIST, "ID3D12GraphicsCommandList");
    push({ method: "Reset", object: list });
    push({ method: "SetDescriptorHeaps", object: list, args: { ppDescriptorHeaps: (s.heaps ?? [HEAP]).map((h) => ref(h, "ID3D12DescriptorHeap")) } });
    push({ method: "SetGraphicsRootSignature", object: list, args: { pRootSignature: ref(s.root ?? BINDLESS_ROOT, "ID3D12RootSignature") } });
    for (const b of s.body ?? [{ method: "DrawIndexedInstanced" }]) push({ object: list, ...b });
    push({ method: "Close", object: list });
  }
  return commands;
}
const draws = (commands, method = "DrawIndexedInstanced") => commands.filter((c) => c.method === method);

test("each draw sees the heap as of its own submission, later rewrites of a slot included only after", () => {
  const commands = frame([
    { heapDescriptors: [{ heap: ref(HEAP, "ID3D12DescriptorHeap"), slots: [{ slot: 3, type: 0, descriptor: texture(40) }, { slot: 1, type: 0, descriptor: texture(41) }] }] },
    // The second submission rewrote slot 3 only; slot 1 is still the first one's.
    { heapDescriptors: [{ heap: ref(HEAP, "ID3D12DescriptorHeap"), slots: [{ slot: 3, type: 0, descriptor: texture(42) }] }] },
  ]);
  const [first, second] = draws(commands);
  const [a] = indexedHeapsAt(commands, first, objectOf, false);
  assert.equal(a.heap, HEAP);
  assert.equal(a.captured, true);
  assert.deepEqual(a.slots.map((s) => [s.slot, s.descriptor.resource.__id]), [[1, 41], [3, 40]], "in slot order, the first submission's contents");
  assert.equal(a.slots[0].type, "D3D12_DESCRIPTOR_RANGE_TYPE_SRV");
  const [b] = indexedHeapsAt(commands, second, objectOf, false);
  assert.deepEqual(b.slots.map((s) => [s.slot, s.descriptor.resource.__id, s.sentBy]), [[1, 41, 0], [3, 42, b.submission]]);
});

test("a root signature without the flag indexes nothing", () => {
  const commands = frame([{ root: PLAIN_ROOT, heapDescriptors: [{ heap: ref(HEAP, "ID3D12DescriptorHeap"), slots: [{ slot: 0, type: 2, descriptor: { buffer: ref(50, "ID3D12Resource"), offset: 0, range: 256 } }] }] }]);
  assert.deepEqual(indexedHeapsAt(commands, draws(commands)[0], objectOf, false), []);
});

test("only the heap kinds the flags name: the resource flag leaves a bound sampler heap out", () => {
  const commands = frame([{ heaps: [HEAP, SAMPLER_HEAP] }]);
  const heaps = indexedHeapsAt(commands, draws(commands)[0], objectOf, false);
  assert.deepEqual(heaps.map((h) => [h.heap, h.samplers]), [[HEAP, false]]);
  assert.equal(heaps[0].captured, false, "a capture without heapDescriptors says it has none, rather than an empty heap");
});

test("a dispatch reads the compute root signature, which a graphics one does not stand in for", () => {
  const commands = frame([{ heapDescriptors: [], body: [{ method: "Dispatch" }] }]);
  assert.deepEqual(indexedHeapsAt(commands, draws(commands, "Dispatch")[0], objectOf, true), []);
});

test("a bundle's draw inherits the executing list's root signature and heaps", () => {
  const commands = frame([{
    heapDescriptors: [{ heap: ref(HEAP, "ID3D12DescriptorHeap"), slots: [{ slot: 7, type: 0, descriptor: texture(44) }] }],
    body: [{ method: "ExecuteBundle" }, { method: "DrawIndexedInstanced", secondary: 90 }],
  }]);
  const [h] = indexedHeapsAt(commands, draws(commands)[0], objectOf, false);
  assert.deepEqual(h.slots.map((s) => s.slot), [7]);
});
