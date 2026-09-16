// Memory as a shape (src/renderer/memory_timeline.ts): the series the capture libraries send with
// each frame report, read as growing, sawtoothing, flat or shrinking — the distinction an instant
// cannot make.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "memtime-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { memoryTimeline, memoryVerdict, GROWTH_SHARE, SAWTOOTH_SHARE } =
  await load("renderer/memory_timeline.ts", "memory_timeline");

const MB = 1048576;

/** A series from per-frame totals in MB; `usage` adds the driver's resident figure. */
function series(valuesMB, { usage } = {}) {
  return valuesMB.map((v, i) => ({
    action: "MemorySample", frame: i * 10,
    heaps: [
      { allocated: v * MB, allocations: 10, ...(usage ? { usage: usage[i] * MB, budget: 8000 * MB } : {}) },
    ],
  }));
}

test("one sample has no direction, so there is no reading", () => {
  assert.equal(memoryTimeline([]), null);
  assert.equal(memoryTimeline(series([100])), null);
});

test("heaps are totalled into one point per sample", () => {
  const s = [{
    action: "MemorySample", frame: 0,
    heaps: [{ allocated: 100 * MB, allocations: 4 }, { allocated: 20 * MB, allocations: 2 }],
  }, {
    action: "MemorySample", frame: 10,
    heaps: [{ allocated: 100 * MB, allocations: 4 }, { allocated: 20 * MB, allocations: 2 }],
  }];
  const t = memoryTimeline(s);
  assert.equal(t.points[0].allocated, 120 * MB);
  assert.equal(t.points[0].allocations, 6);
  assert.equal(t.hasUsage, false, "no driver residency in this series");
});

test("memory climbing every frame and never falling is a leak", () => {
  const t = memoryTimeline(series([100, 120, 140, 160, 180, 200]));
  assert.equal(t.trend, "growing");
  assert.ok(t.bytesPerFrame > 0);
  assert.match(memoryVerdict(t), /leak/);
  assert.match(memoryVerdict(t), /never freed/);
});

test("a pool emptying and refilling is not a leak, however it ends", () => {
  // Ends well above where it started, but gives the ground back in between: the endpoints alone
  // would call this growth.
  const t = memoryTimeline(series([100, 400, 120, 420, 130, 440]));
  assert.equal(t.trend, "sawtooth");
  assert.match(memoryVerdict(t), /emptied and refilled/);
  // It may say "rather than a leak"; what it must not do is diagnose one.
  assert.doesNotMatch(memoryVerdict(t), /what a leak looks like/);
});

test("a steady renderer reads as flat", () => {
  const t = memoryTimeline(series([200, 201, 199, 200, 202, 200]));
  assert.equal(t.trend, "flat");
  assert.match(memoryVerdict(t), /steady/);
  assert.match(memoryVerdict(t), /Nothing is accumulating/);
});

test("an application releasing what it held reads as shrinking", () => {
  const t = memoryTimeline(series([400, 340, 280, 220, 160, 100]));
  assert.equal(t.trend, "shrinking");
  assert.match(memoryVerdict(t), /releasing/);
});

test("growth below the threshold is not called a leak", () => {
  // A few percent over the series is ordinary: caches warming, a level streaming in.
  const t = memoryTimeline(series([200, 202, 204, 206, 208, 210]));
  assert.ok((210 - 200) / 210 < GROWTH_SHARE);
  assert.equal(t.trend, "flat");
});

test("a single spike at the end does not decide the trend", () => {
  // Least squares over the whole series, not first-to-last: one sample must not outvote the rest.
  const t = memoryTimeline(series([200, 200, 200, 200, 200, 201]));
  assert.equal(t.trend, "flat");
});

test("the range and swing describe what has to fit", () => {
  const t = memoryTimeline(series([100, 500, 100, 500]));
  assert.equal(t.minBytes, 100 * MB);
  assert.equal(t.maxBytes, 500 * MB);
  assert.equal(t.swingBytes, 400 * MB);
  assert.ok(t.swingBytes / t.maxBytes >= SAWTOOTH_SHARE);
  // The peak is the number that matters for fitting, and the verdict says so.
  assert.match(memoryVerdict(t), /peak is what has to fit/);
});

test("the frames covered come from the samples' own frame numbers", () => {
  const t = memoryTimeline(series([100, 110, 120]));
  assert.equal(t.frames, 20, "frames 0, 10, 20");
});

test("the driver's residency is carried when it reported one", () => {
  const t = memoryTimeline(series([100, 100], { usage: [300, 310] }));
  assert.equal(t.hasUsage, true);
  assert.equal(t.points[0].usage, 300 * MB);
  // More resident than this application allocated: the driver's own overhead and other processes.
  assert.ok(t.points[0].usage > t.points[0].allocated);
});
