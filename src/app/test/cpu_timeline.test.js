// The CPU side of a captured frame (src/renderer/cpu_timeline.ts): what the layer timed on the
// host (src/vulkan/src/cpu_timeline.h), and the verdict it turns into.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "cputimeline-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { summarizeCpuTimeline, cpuVerdict, cpuKindOf, gpuTicksToCpuMs, eventsOfThread } =
  await load("renderer/cpu_timeline.ts", "cpu_timeline");

/** Events over a 10 ms span, given as [category, startMs, durationMs]. */
const timeline = (events, extra = {}) => ({
  action: "CaptureCpuTimeline",
  threads: [1000],
  events: events.map(([category, startMs, durationMs]) => ({ thread: 0, category, frame: 1, startMs, durationMs })),
  ...extra,
});

test("a capture with no timeline, or no events, summarizes to nothing", () => {
  assert.equal(summarizeCpuTimeline(null), null, "a capture from a layer before the timeline");
  assert.equal(summarizeCpuTimeline(timeline([])), null);
});

test("time is split by what the call was doing, largest first", () => {
  const s = summarizeCpuTimeline(timeline([["submit", 0, 1], ["waitFences", 1, 6], ["present", 7, 2]]));
  assert.equal(s.totals[0].category, "waitFences");
  assert.equal(s.gpuWaitMs, 6);
  assert.equal(s.displayWaitMs, 2);
  assert.equal(s.submitMs, 1);
  assert.equal(s.spanMs, 9, "from the first start to the last end");
});

test("waiting on the GPU and waiting on the display are not the same kind", () => {
  // The distinction the verdict rests on: one means the GPU is the limit, the other that neither is.
  assert.equal(cpuKindOf("waitFences"), "gpuWait");
  assert.equal(cpuKindOf("waitIdle"), "gpuWait");
  assert.equal(cpuKindOf("present"), "displayWait");
  assert.equal(cpuKindOf("acquire"), "displayWait");
  assert.equal(cpuKindOf("submit"), "work");
});

test("a frame mostly blocked on fences says the GPU sets the frame time", () => {
  const s = summarizeCpuTimeline(timeline([["waitFences", 0, 8], ["submit", 8, 0.5], ["present", 8.5, 0.5]]));
  assert.match(cpuVerdict(s), /waiting on fences/);
  assert.match(cpuVerdict(s), /GPU is what sets the frame time/);
});

test("a frame mostly inside submission says submission is the cost", () => {
  const s = summarizeCpuTimeline(timeline([["submit", 0, 7], ["waitFences", 7, 1], ["present", 8, 1]]));
  assert.match(cpuVerdict(s), /inside submission/);
});

test("a frame waiting in present is paced by the display, not limited by either processor", () => {
  // The case that used to be misread as expensive submission: vkQueuePresentKHR blocks on vsync.
  const s = summarizeCpuTimeline(timeline([["present", 0, 8], ["waitFences", 8, 0.5], ["submit", 8.5, 0.2]]));
  const v = cpuVerdict(s);
  assert.match(v, /paced by the display/);
  assert.doesNotMatch(v, /submission is a real cost/);
});

test("a frame spending its time outside timed calls says so", () => {
  const s = summarizeCpuTimeline(timeline([["submit", 0, 0.1], ["waitFences", 9.8, 0.1]]));
  assert.match(cpuVerdict(s), /application's own work/);
});

test("a GPU timestamp maps onto the CPU axis only with a calibration", () => {
  const withCal = timeline([["submit", 0, 1]], { calibration: { deviceTicks: 1000, hostMs: 5, timestampPeriod: 1 } });
  // One million ticks of 1 ns past the calibration instant is one millisecond after it.
  assert.equal(gpuTicksToCpuMs(withCal, 1000 + 1e6), 6);
  assert.equal(gpuTicksToCpuMs(withCal, 1000), 5);
  assert.equal(gpuTicksToCpuMs(timeline([["submit", 0, 1]]), 1000), null, "no calibrated-timestamps extension");
  assert.equal(gpuTicksToCpuMs(null, 0), null);
});

test("events can be taken per thread, for one track of a drawing", () => {
  const t = timeline([["submit", 0, 1], ["present", 1, 1]]);
  t.events[1].thread = 1;
  t.threads = [1000, 1001];
  assert.equal(eventsOfThread(t, 0).length, 1);
  assert.equal(eventsOfThread(t, 1)[0].category, "present");
});

test("the summary carries what the capture could not keep", () => {
  const s = summarizeCpuTimeline(timeline([["submit", 0, 1]], { dropped: 12 }));
  assert.equal(s.dropped, 12);
  assert.equal(s.calibrated, false);
});

test("pipeline creation is its own kind, not submission work", () => {
  // Both are the application's thread doing something rather than waiting, but the fixes differ:
  // submission wants fewer and larger submits, a compile wants to happen before the frame.
  assert.equal(cpuKindOf("pipeline"), "compile");
  const s = summarizeCpuTimeline(timeline([["submit", 0, 1], ["pipeline", 1, 4]]));
  assert.equal(s.compileMs, 4);
  assert.equal(s.submitMs, 1, "a compile is not counted as submission");
  assert.equal(s.totals.find((t) => t.category === "pipeline").label, "Creating pipelines");
});

test("a frame that stopped to build a pipeline is told so first", () => {
  // Even beside a long fence wait: the wait is a symptom of the GPU being behind, the compile is a
  // stall with a different cause and a different fix, and nothing else in the frame explains it.
  const s = summarizeCpuTimeline(timeline([["waitFences", 0, 5], ["pipeline", 5, 4], ["submit", 9, 1]]));
  const verdict = cpuVerdict(s);
  assert.match(verdict, /creating pipelines/i);
  assert.match(verdict, /1 call/, "the count is named, since one slow compile reads differently from many");
});

test("a frame that built nothing says nothing about compiling", () => {
  const s = summarizeCpuTimeline(timeline([["waitFences", 0, 8], ["submit", 8, 2]]));
  assert.equal(s.compileMs, 0);
  assert.doesNotMatch(cpuVerdict(s), /pipeline/i);
});

test("a compile too small to have caused the frame's trouble is not the verdict", () => {
  const s = summarizeCpuTimeline(timeline([["waitFences", 0, 9], ["pipeline", 9, 0.2], ["submit", 9.2, 0.8]]));
  assert.doesNotMatch(cpuVerdict(s), /creating pipelines/i);
  assert.equal(s.compileMs, 0.2, "still counted, just not the headline");
});
