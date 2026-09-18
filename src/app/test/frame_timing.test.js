// Timing captures (src/renderer/frame_timing.ts): per-frame times over minutes, the hitches in
// them, and what the CPU was doing when they happened. The thing being tested is mostly judgement
// — what counts as a hitch, and what counts as its cause — so the cases are the ones where a
// simpler rule gets it wrong.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "frametiming-")), "frame_timing.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "frame_timing.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { summarizeTiming, hitchThresholdMs, rangeIndices, HITCH_FLOOR_MS } = await import(pathToFileURL(out).href);

const CATEGORIES = ["submit", "present", "waitFences", "acquire", "waitIdle", "pipeline"];
const at = (name) => CATEGORIES.indexOf(name);

/** `frames` as [durationMs, {category: ms}]. */
const capture = (frames) => ({
  categories: CATEGORIES,
  frames: frames.map(([durationMs, cats = {}], i) => {
    const categoryMs = CATEGORIES.map(() => 0);
    for (const [k, v] of Object.entries(cats)) categoryMs[at(k)] = v;
    return { frame: 100 + i, durationMs, categoryMs };
  }),
});

const steady = (n, ms = 16.7, cats = {}) => Array.from({ length: n }, () => [ms, cats]);

test("an empty range summarizes to nothing", () => {
  assert.equal(summarizeTiming(capture([])), null);
  assert.equal(summarizeTiming(capture(steady(5)), 3, 3), null);
});

test("a steady run has no hitches and says so", () => {
  const s = summarizeTiming(capture(steady(120)));
  assert.equal(s.frames, 120);
  assert.ok(Math.abs(s.medianMs - 16.7) < 0.001);
  assert.equal(s.hitches.length, 0);
  assert.match(s.verdict, /Nothing here hitched/);
  assert.match(s.verdict, /60 fps/);
});

test("a single slow frame is found, and reported with what it cost", () => {
  const frames = steady(60);
  frames[30] = [120, { pipeline: 100 }];
  const s = summarizeTiming(capture(frames));
  assert.equal(s.hitches.length, 1);
  assert.equal(s.hitches[0].frame, 130, "the application's own frame number, not the index");
  assert.equal(s.hitches[0].index, 30);
  assert.ok(s.hitches[0].times > 7);
  assert.equal(s.hitches[0].cause.category, "pipeline");
  assert.match(s.verdict, /Creating pipelines/i);
});

test("hitches are reported worst first", () => {
  const frames = steady(60);
  frames[10] = [50, {}];
  frames[20] = [200, {}];
  frames[30] = [90, {}];
  const s = summarizeTiming(capture(frames));
  assert.deepEqual(s.hitches.map((h) => h.durationMs), [200, 90, 50]);
});

test("a fast application's ordinary jitter is not called a hitch", () => {
  // At 300 fps twice the median is 6.6 ms, which nobody would feel. The floor is what stops the
  // multiple alone from calling every other frame a hitch on a very fast application.
  const frames = steady(60, 3.3);
  frames[10] = [6.8, {}];
  const s = summarizeTiming(capture(frames));
  assert.equal(s.hitches.length, 0, "over twice the median, but only 3.5 ms more than it");
  assert.equal(hitchThresholdMs(3.3), 3.3 + HITCH_FLOOR_MS);
});

test("the same frame time is a hitch on a slow application and not on a fast one", () => {
  const slow = summarizeTiming(capture([...steady(40, 33), [70, {}]]));
  const fast = summarizeTiming(capture([...steady(40, 100), [70, {}]]));
  assert.equal(slow.hitches.length, 1, "70 ms against a 33 ms median");
  assert.equal(fast.hitches.length, 0, "70 ms is faster than this application's ordinary frame");
});

test("a category that was going to run anyway is not blamed for the hitch", () => {
  // Submitting costs 8 ms in every frame including the slow one; what is different about the slow
  // frame is the compile. Taking the largest category outright would blame submission.
  const frames = steady(60, 16.7, { submit: 8 });
  frames[30] = [120, { submit: 8, pipeline: 95 }];
  const s = summarizeTiming(capture(frames));
  assert.equal(s.hitches[0].cause.category, "pipeline");
});

test("a hitch no timed call explains says so rather than blaming the largest", () => {
  // The application's own work between the calls: the finding is that nothing accounts for it.
  const frames = steady(60);
  frames[30] = [120, { submit: 2 }];
  const s = summarizeTiming(capture(frames));
  assert.equal(s.hitches[0].cause, null);
  assert.match(s.verdict, /application's own work/);
});

test("category totals are over the range, largest first, as a share of its wall time", () => {
  const s = summarizeTiming(capture(steady(10, 10, { submit: 2, waitFences: 5 })));
  assert.equal(s.categories[0].category, "waitFences");
  assert.equal(s.categories[0].ms, 50);
  assert.equal(s.categories[0].share, 0.5);
  assert.equal(s.categories[1].category, "submit");
  assert.ok(!s.categories.some((c) => c.ms === 0), "categories with nothing in them are left out");
});

test("percentiles describe the tail the median hides", () => {
  // Ninety ordinary frames and ten slow ones: the median says nothing is wrong, p99 does.
  const s = summarizeTiming(capture([...steady(90, 16), ...steady(10, 60)]));
  assert.equal(s.medianMs, 16);
  assert.ok(s.p95Ms >= 60, `p95 ${s.p95Ms}`);
  assert.equal(s.maxMs, 60);
});

test("a range can be summarized on its own", () => {
  // Asymmetric on purpose: an even split puts the whole range's median on the boundary, where it
  // is the slow value too and the comparison below proves nothing.
  const frames = [...steady(40, 16.7), ...steady(20, 50)];
  const whole = summarizeTiming(capture(frames));
  const tail = summarizeTiming(capture(frames), 40, 60);
  assert.equal(tail.frames, 20);
  assert.equal(tail.medianMs, 50);
  assert.equal(tail.hitches.length, 0, "steady at 50 ms is not hitching, it is just slow");
  assert.notEqual(whole.medianMs, tail.medianMs);
});

// A range dragged out on the graph (renderer/timing_view.ts). It is held by the application's own
// frame numbers, because both rings drop the oldest frames out of the front of the array and a
// range held by position would name different frames after every trim.

test("a range resolves to the frames it names, the last one included", () => {
  const c = capture(steady(10));            // frames 100 to 109
  assert.deepEqual(rangeIndices(c, { fromFrame: 102, toFrame: 105 }), { from: 2, to: 6 });
  assert.deepEqual(rangeIndices(c, { fromFrame: 100, toFrame: 109 }), { from: 0, to: 10 });
  assert.equal(summarizeTiming(c, 2, 6).frames, 4);
});

test("a range dragged right to left is the same range", () => {
  const c = capture(steady(10));
  assert.deepEqual(rangeIndices(c, { fromFrame: 105, toFrame: 102 }), { from: 2, to: 6 });
});

test("the same range names the same frames after the ring drops the oldest", () => {
  const range = { fromFrame: 104, toFrame: 106 };
  const c = capture(steady(10));
  const before = rangeIndices(c, range);
  assert.deepEqual(before, { from: 4, to: 7 });
  const kept = c.frames.slice(before.from, before.to).map((f) => f.frame);
  // Three frames trimmed off the front, as a long recording does every batch.
  c.frames.splice(0, 3);
  const after = rangeIndices(c, range);
  assert.deepEqual(after, { from: 1, to: 4 }, "the indices moved");
  assert.deepEqual(c.frames.slice(after.from, after.to).map((f) => f.frame), kept, "the frames did not");
});

test("a range half aged out keeps the frames it still has", () => {
  const c = capture(steady(10));
  c.frames.splice(0, 5);                    // frames 105 to 109 remain
  const r = rangeIndices(c, { fromFrame: 102, toFrame: 106 });
  assert.deepEqual(r, { from: 0, to: 2 }, "frames 105 and 106");
});

test("a range entirely aged out is nothing, so the caller can go back to the whole run", () => {
  const c = capture(steady(10));
  c.frames.splice(0, 5);
  assert.equal(rangeIndices(c, { fromFrame: 100, toFrame: 104 }), null);
  assert.equal(rangeIndices(capture([]), { fromFrame: 1, toFrame: 2 }), null);
});

test("hitches are counted in the range, not in the run", () => {
  // A calm stretch and then one with a spike every tenth frame, which is what makes whole-run
  // figures the wrong answer: the spikes are 10% of the second half and 5% of the run.
  const c = capture([...steady(60, 16.7), ...Array.from({ length: 20 }, (_, i) => [i % 10 ? 16.7 : 60])]);
  assert.ok(summarizeTiming(c).hitches.length > 0);
  const calm = rangeIndices(c, { fromFrame: 100, toFrame: 159 });
  const s = summarizeTiming(c, calm.from, calm.to);
  assert.equal(s.frames, 60);
  assert.equal(s.hitches.length, 0, "nothing in the calm stretch hitched");
  const rough = rangeIndices(c, { fromFrame: 160, toFrame: 179 });
  assert.equal(summarizeTiming(c, rough.from, rough.to).hitches.length, 2);
});

test("a range where the slow frames are the norm reports them as the norm", () => {
  // Half the range at 60 ms makes 60 ms the median, and a hitch is measured against the median of
  // the range being asked about. Selecting only the rough part therefore says the frames are slow
  // — in the figures — rather than that they are hitching, which is the honest answer: nothing
  // there is an outlier. The whole run, where those frames are outliers, still calls them hitches.
  const c = capture([...steady(60, 16.7), ...Array.from({ length: 20 }, (_, i) => [i % 2 ? 16.7 : 60])]);
  assert.ok(summarizeTiming(c).hitches.length >= 10);
  const rough = rangeIndices(c, { fromFrame: 160, toFrame: 179 });
  const s = summarizeTiming(c, rough.from, rough.to);
  assert.equal(s.hitches.length, 0);
  assert.equal(s.medianMs, 60);
  assert.match(s.verdict, /Nothing here hitched/);
});
