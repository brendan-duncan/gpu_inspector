// The capture file container (src/renderer/capture_format.ts): the bytes it writes, that it reads
// its own files back, and that the batched path a very large capture takes is indistinguishable
// from the single-call path everything else takes.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "captureformat-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { encodeCaptureFile, parseCaptureFile, CAPTURE_FORMAT, CAPTURE_VERSION } = await load("renderer/capture_format.ts", "capture_format");

const MAGIC = "GPUCAP 1\n";
/** Forces every array through the batched writer and every member through the batched parser. */
const BATCHED_WRITE = { maxDirectElements: 0, batchElements: 2 };
const BATCHED_READ = { threshold: 0, batchBytes: 1 };

const pixels = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
const spirv = new Uint8Array([0x03, 0x02, 0x23, 0x07, 0, 0, 0, 0]);
const vertices = new Uint8Array(64).fill(9);
/** In the order the manifest's offsets count them: pixels at 0, spirv at 8, vertices at 16. */
const PAYLOADS = [pixels, spirv, vertices];

function manifest(commandCount = 6) {
  return {
    format: CAPTURE_FORMAT, version: CAPTURE_VERSION, api: "vulkan", application: "GPU Inspector",
    savedAt: "2026-09-18T00:00:00.000Z", source: { name: "test — ünïcode 😀" },
    frame: 3, frames: 2, frameTimeMs: 16.7, submitMs: 3.25, passTimingOrigin: 1234,
    objects: [
      { id: 1, parent: 0, type: "VkShaderModule", cmd: "vkCreateShaderModule", index: 0, handle: "0x1",
        label: 'a "quoted" \\ name', args: { codeSize: 8 }, blobs: [{ name: "spirv", payload: [8, 8] }], updates: {}, deleted: false },
      { id: 2, parent: 0, type: "VkImage", cmd: "vkCreateImage", index: 1, handle: "0x2",
        label: null, args: { extent: { width: 4, height: 2 } }, blobs: [], updates: { layout: "GENERAL" }, deleted: true },
    ],
    commands: Array.from({ length: commandCount }, (_, i) => ({
      index: i, frame: 0, method: i % 2 ? "vkCmdDraw" : "vkCmdBindVertexBuffers",
      object: { __id: 10, __class: "VkCommandBuffer" },
      args: { firstBinding: i, pOffsets: ["0", "18446744073709551615"], note: `cmd "${i}" \\ x` },
      slot: i,
    })),
    textures: [{ info: { id: 2, frame: 0, commandBuffer: 10, passIndex: 0, attachment: 0, width: 2, height: 1, format: "R8G8B8A8_UNORM" }, payload: [0, 8] }],
    buffers: [{ info: { id: 5, size: 64, offset: 0 }, payload: [16, 64] }],
    passTimings: [{ frame: 0, commandBuffer: 10, passIndex: 0, kind: "render", startTicks: "9007199254740993", durationMs: 1.5 }],
    validation: [{ severity: "error", message: "something \"went\" wrong", objects: [2] }],
  };
}

/** The container as the format describes it, built without going through the writer. */
function reference(m, payloads) {
  const json = new TextEncoder().encode(JSON.stringify(m));
  const magic = new TextEncoder().encode(MAGIC);
  const size = payloads.reduce((n, p) => n + p.byteLength, 0);
  const out = new Uint8Array(magic.byteLength + 4 + json.byteLength + size);
  let pos = 0;
  out.set(magic, pos); pos += magic.byteLength;
  new DataView(out.buffer).setUint32(pos, json.byteLength, true); pos += 4;
  out.set(json, pos); pos += json.byteLength;
  for (const p of payloads) { out.set(p, pos); pos += p.byteLength; }
  return out;
}

test("the bytes are the header, the manifest and the payloads", () => {
  const m = manifest();
  const payloads = PAYLOADS;
  assert.deepEqual(encodeCaptureFile(m, payloads), reference(m, payloads));
});

test("the batched writer produces the same bytes as the single-call one", () => {
  const m = manifest(20);
  const payloads = PAYLOADS;
  assert.deepEqual(encodeCaptureFile(m, payloads, BATCHED_WRITE), encodeCaptureFile(m, payloads));
});

test("a capture file reads back what was written, on both paths", () => {
  const m = manifest();
  const payloads = PAYLOADS;
  for (const [label, write, read] of [["direct", {}, {}], ["batched", BATCHED_WRITE, BATCHED_READ]]) {
    const loaded = parseCaptureFile(encodeCaptureFile(manifest(), payloads, write), read);
    assert.deepEqual(loaded.manifest, m, label);
    assert.equal(loaded.api, "vulkan", label);
    assert.equal(loaded.commands.length, 6, label);
    assert.deepEqual(loaded.objects.map((o) => o.id), [1, 2], label);
    assert.deepEqual(loaded.validation, m.validation, label);
    assert.equal(loaded.passTimingOrigin, 1234, label);
    // Payloads are views into the file at the offsets the manifest names.
    assert.deepEqual(loaded.textures[0].data, pixels, label);
    assert.deepEqual(loaded.blobs.get("1:0"), spirv, label);
    assert.deepEqual(loaded.buffers.get(5).data, vertices, label);
    assert.equal(loaded.passTimings.size, 1, label);
  }
});

test("a measured draw overlay survives the file, mask and all", () => {
  // Metal and D3D12 measure a draw overlay while the frame is captured, so the capture *is* the
  // only copy: a file that dropped it would lose the answer the second capture was taken for.
  // Vulkan's come from replaying the file, so its captures carry none and the field is absent.
  const mask = new Uint8Array([0, 1, 3, 11, 15, 4, 0, 0]);
  const m = manifest();
  m.drawOverlays = [{
    info: {
      command: 17, method: "drawIndexedPrimitives:", frame: 0, commandBuffer: 752, passIndex: 1,
      measured: true, width: 4, height: 2, fragments: 5, pixelsCovered: 5, pixelsPassed: 0,
      pixelsRejected: 5, pixelsStencilRejected: 0, pixelsBackFacing: 0,
      depthTested: true, wireframe: true, stencilTested: false, backFaceTested: true,
    },
    // After pixels (0, 8), spirv (8, 8) and vertices (16, 64).
    payload: [80, 8],
  }];
  for (const [label, write, read] of [["direct", {}, {}], ["batched", BATCHED_WRITE, BATCHED_READ]]) {
    const loaded = parseCaptureFile(encodeCaptureFile(m, [...PAYLOADS, mask], write), read);
    assert.equal(loaded.drawOverlays.size, 1, label);
    const o = loaded.drawOverlays.get(17);
    assert.equal(o.pixelsRejected, 5, label);
    assert.equal(o.depthTested, true, label);
    assert.deepEqual(o.mask, mask, label);
  }
  // A capture with none has no field to read, and the map is empty rather than absent.
  assert.equal(parseCaptureFile(encodeCaptureFile(manifest(), PAYLOADS)).drawOverlays.size, 0);
});

test("commands are renumbered in place, without copying the list", () => {
  const m = manifest(5);
  m.commands.forEach((c) => { c.index = 999; });
  const loaded = parseCaptureFile(encodeCaptureFile(m, PAYLOADS));
  assert.deepEqual(loaded.commands.map((c) => c.index), [0, 1, 2, 3, 4]);
  // The same array the manifest holds, not a copy of it: a large capture must not hold two.
  assert.equal(loaded.commands, loaded.manifest.commands);
});

test("the element hook drops a field without changing anything else", () => {
  const m = manifest(4);
  m.commands.forEach((c, i) => { c.children = [{ commandBuffer: 20, commands: [{ method: "vkCmdDraw", args: { n: i } }] }]; });
  const element = { commands: (c) => { const { children: _children, ...rest } = c; return rest; } };
  const expected = { ...m, commands: m.commands.map(element.commands) };
  for (const [label, opts] of [["direct", { element }], ["batched", { element, ...BATCHED_WRITE }]]) {
    const loaded = parseCaptureFile(encodeCaptureFile(m, PAYLOADS, opts));
    assert.deepEqual(loaded.manifest, expected, label);
    assert.equal("children" in loaded.commands[0], false, label);
  }
});

test("a file with no optional members still loads", () => {
  const bare = { format: CAPTURE_FORMAT, version: CAPTURE_VERSION, frame: 0, frames: 1 };
  for (const [label, read] of [["direct", {}], ["batched", BATCHED_READ]]) {
    const loaded = parseCaptureFile(encodeCaptureFile(bare, []), read);
    assert.deepEqual(loaded.commands, [], label);
    assert.deepEqual(loaded.objects, [], label);
    assert.equal(loaded.api, "vulkan", label);          // files written before the field existed
    assert.equal(loaded.passTimingOrigin, null, label);
    assert.equal(loaded.blobs.size, 0, label);
  }
});

test("a file that is not one is rejected with a readable message", () => {
  assert.throws(() => parseCaptureFile(new Uint8Array(4)), /too short/);
  assert.throws(() => parseCaptureFile(new Uint8Array(32)), /bad header/);
  const good = encodeCaptureFile(manifest(), []);
  assert.throws(() => parseCaptureFile(good.subarray(0, good.byteLength - 8)), /truncated/);
  const badJson = encodeCaptureFile(manifest(), []);
  badJson[MAGIC.length + 4] = 0x7c;                      // '|' where '{' should be
  assert.throws(() => parseCaptureFile(badJson), /not valid JSON/);
});

test("a newer format version is refused", () => {
  const m = { ...manifest(), version: CAPTURE_VERSION + 1 };
  assert.throws(() => parseCaptureFile(encodeCaptureFile(m, [])), /newer GPU Inspector/);
});

test("a capture with many commands round-trips identically on both paths", () => {
  const m = manifest(5000);
  const direct = encodeCaptureFile(m, PAYLOADS);
  assert.deepEqual(encodeCaptureFile(m, PAYLOADS, { maxDirectElements: 0, batchElements: 64 }), direct);
  assert.deepEqual(parseCaptureFile(direct, { threshold: 0, batchBytes: 4096 }).manifest, parseCaptureFile(direct).manifest);
});
