// Memory captures (src/renderer/memory_capture.ts): every allocation and free over a stretch of a
// run, and the three things a total cannot say about them — what made here is still held, what was
// made and thrown away again, and which frames allocated. The cases are the ones where the obvious
// reading is wrong: an allocation too young to be called held, a handle-less event, a free of
// something older than the capture.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "memorycapture-")), "memory_capture.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "memory_capture.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { summarizeMemoryCapture, appendMemoryEvents, emptyMemoryCapture, TRANSIENT_FRAMES } = await import(pathToFileURL(out).href);

const MB = 1 << 20;
const alloc = (frame, id, bytes, heap = 0) => ({ frame, ms: frame * 16, id, bytes, heap });
const free = (frame, id, bytes, heap = 0) => ({ frame, ms: frame * 16, id, bytes, heap, free: true });
const capture = (events, baseline = [{ allocated: 100 * MB, allocations: 10 }]) => ({ baseline, events, dropped: 0 });

test("nothing recorded is no summary, not a summary of nothing", () => {
  assert.equal(summarizeMemoryCapture(emptyMemoryCapture()), null);
});

test("the first message's baseline makes the totals absolute", () => {
  const c = emptyMemoryCapture();
  appendMemoryEvents(c, { action: "MemoryEvents", baseline: [{ allocated: 8 * MB, allocations: 2 }, { allocated: MB, allocations: 1 }], events: [alloc(10, 1, MB)] });
  appendMemoryEvents(c, { action: "MemoryEvents", events: [alloc(20, 2, MB)] });
  const s = summarizeMemoryCapture(c);
  assert.equal(s.startBytes, 9 * MB);
  assert.equal(s.endBytes, 11 * MB);
  assert.equal(s.peakBytes, 11 * MB);
  assert.equal(s.allocations, 2);
});

test("an allocation made here and never freed is a survivor, largest first", () => {
  const s = summarizeMemoryCapture(capture([
    alloc(10, 1, MB), alloc(11, 2, 4 * MB), alloc(12, 3, 2 * MB), free(13, 3, 2 * MB), alloc(100, 9, 16),
  ]));
  assert.deepEqual(s.survivors.map((v) => v.id), [2, 1]);
  assert.equal(s.survivorBytes, 5 * MB);
  assert.match(s.verdict, /still held/);
});

test("an allocation in the capture's last few frames is too young to call held", () => {
  // Every frame makes one and frees the one from two frames before: at any moment two are live,
  // and a report that named them as leaks would do so for every churning application there is.
  const events = [];
  for (let f = 0; f < 60; f++) {
    events.push(alloc(f, 100 + f, 65536, 1));
    if (f >= 2) events.push(free(f, 100 + f - 2, 65536, 1));
  }
  const s = summarizeMemoryCapture(capture(events));
  assert.equal(s.survivors.length, 0);
  assert.equal(s.transient.count, 58);
  assert.ok(s.transient.perFrame > 0.9);
  assert.match(s.verdict, /ring buffer or a pool/);
  assert.doesNotMatch(s.verdict, /still held/);
});

test("transient means freed within TRANSIENT_FRAMES, not merely freed", () => {
  const s = summarizeMemoryCapture(capture([
    alloc(10, 1, MB), free(10 + TRANSIENT_FRAMES, 1, MB),
    alloc(10, 2, MB), free(10 + TRANSIENT_FRAMES + 1, 2, MB),
    alloc(200, 3, 16),
  ]));
  assert.equal(s.transient.count, 1);
  assert.equal(s.netBytes, 16);
});

test("a free of something older than the capture is counted apart", () => {
  const s = summarizeMemoryCapture(capture([free(5, 77, 10 * MB), alloc(6, 1, MB), alloc(50, 2, 16)]));
  assert.deepEqual(s.freedOlder, { count: 1, bytes: 10 * MB });
  assert.equal(s.endBytes, 100 * MB - 10 * MB + MB + 16);
  // The total fell, but what was made here is still held, and that is what the list is of.
  assert.deepEqual(s.survivors.map((v) => v.id), [1]);
});

test("an allocation with no id is counted and left out of the lists", () => {
  const s = summarizeMemoryCapture(capture([alloc(1, 0, MB), alloc(2, 0, MB), alloc(50, 5, 16)]));
  assert.equal(s.unnamed, 2);
  assert.equal(s.allocatedBytes, 2 * MB + 16);
  assert.equal(s.survivors.length, 0);
});

test("the frames that allocated most, and the change per heap", () => {
  const s = summarizeMemoryCapture(capture([
    alloc(10, 1, MB, 0), alloc(10, 2, MB, 0), alloc(11, 3, 8 * MB, 1), free(30, 3, 8 * MB, 1), alloc(40, 4, 16, 0),
  ]));
  assert.deepEqual(s.busiest.slice(0, 2).map((f) => [f.frame, f.allocations]), [[11, 1], [10, 2]]);
  assert.deepEqual(s.heaps.map((h) => [h.heap, h.netBytes]), [[0, 2 * MB + 16], [1, 0]]);
  assert.deepEqual(s.series.map((p) => p.frame), [10, 11, 30, 40]);
});

test("steady state reads as steady", () => {
  const s = summarizeMemoryCapture(capture([alloc(10, 1, MB), free(100, 1, MB)]));
  assert.equal(s.netBytes, 0);
  assert.match(s.verdict, /nothing is growing/);
});
