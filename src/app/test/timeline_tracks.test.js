// CPU and GPU activity on one axis (src/renderer/timeline_tracks.ts): the lanes, the shared origin
// that the calibration provides, and the idle GPU stretches the drawing exists to make visible.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "tracks-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { buildTimelineTracks, gpuGaps, gpuSpan, attributeGaps, submitToFirstPassMs, tracksVerdict,
  fullView, clampView, zoomView, panView, visibleBoxes, axisTicks, MIN_VIEW_MS, commandBufferQueues } =
  await load("renderer/timeline_tracks.ts", "timeline_tracks");

/** The layer's timeline message: events as [thread, category, startMs, durationMs]. */
function timeline(events, { threads = [4812], calibration } = {}) {
  return {
    action: "CaptureCpuTimeline", threads, events: events.map(([thread, category, startMs, durationMs]) =>
      ({ thread, category, frame: 1, startMs, durationMs })),
    ...(calibration ? { calibration } : {}),
  };
}

/** A pass as the layer reports it, with the label the UI shows. */
function pass(label, startMs, durationMs) {
  return { label, timing: { frame: 1, commandBuffer: 1, passIndex: 0, startMs, durationMs } };
}

// deviceTicks 0 at hostMs 10, 1 tick = 1 ns, so ticks map to host ms as 10 + ticks/1e6.
const CALIBRATION = { deviceTicks: 0, hostMs: 10, timestampPeriod: 1 };

test("a capture with no CPU events has nothing to draw", () => {
  assert.equal(buildTimelineTracks({ timeline: null, passes: [], originTicks: null }), null);
  assert.equal(buildTimelineTracks({ timeline: timeline([]), passes: [], originTicks: null }), null);
});

test("each thread becomes a lane, and the first is named as the main one", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 0, 1], [1, "submit", 2, 1]], { threads: [100, 200] }),
    passes: [], originTicks: null,
  });
  assert.equal(t.tracks.length, 2);
  assert.equal(t.tracks[0].label, "Thread 100 (main)");
  assert.equal(t.tracks[1].label, "Thread 200");
});

test("a single-threaded capture does not claim a main thread", () => {
  const t = buildTimelineTracks({ timeline: timeline([[0, "submit", 0, 1]]), passes: [], originTicks: null });
  assert.equal(t.tracks[0].label, "Thread 4812");
});

test("spans are rebased onto the drawn range and sorted by start", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 12, 1], [0, "waitFences", 10, 0.5]]), passes: [], originTicks: null,
  });
  assert.equal(t.spanMs, 3, "10.0 to 13.0");
  assert.equal(t.tracks[0].spans[0].startMs, 0, "the earliest event is the origin");
  assert.equal(t.tracks[0].spans[0].label, "Waiting on fences");
  assert.equal(t.tracks[0].spans[1].startMs, 2);
});

test("a category's kind is carried through, so waiting and working can read differently", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "waitFences", 0, 1], [0, "present", 2, 1], [0, "submit", 4, 1]]),
    passes: [], originTicks: null,
  });
  assert.deepEqual(t.tracks[0].spans.map((s) => s.kind), ["gpuWait", "displayWait", "work"]);
});

test("the GPU lane is placed with the calibration, not with its own origin", () => {
  // The pass starts 1 ms after originTicks (1e6 ticks at 1 ns), and originTicks 2e6 is host 12 ms.
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 1]], { calibration: CALIBRATION }),
    passes: [pass("Shadows", 1, 2)], originTicks: 2e6,
  });
  assert.equal(t.hasGpu, true);
  const gpu = t.tracks.find((x) => x.kind === "gpu");
  // Pass sits at host 13 ms; the range starts at the submit's 10 ms, so 3 ms in.
  assert.equal(gpu.spans[0].startMs, 3);
  assert.equal(gpu.spans[0].durationMs, 2);
  assert.equal(gpu.spans[0].label, "Shadows");
});

test("the range grows to cover a pass that runs past the last CPU call", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 1]], { calibration: CALIBRATION }),
    passes: [pass("Main", 0, 5)], originTicks: 1e6,   // host 11 ms, ends at 16
  });
  assert.equal(t.spanMs, 6, "10 to 16");
});

test("without a calibration the CPU lanes still draw and the GPU lane is left out with a reason", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 0, 1]]), passes: [pass("Main", 0, 1)], originTicks: 1e6,
  });
  assert.equal(t.hasGpu, false);
  assert.equal(t.tracks.length, 1);
  assert.match(t.gpuNote, /calibrated-timestamps/);
});

test("an older capture with a calibration but no origin says to capture again", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 0, 1]], { calibration: CALIBRATION }),
    passes: [pass("Main", 0, 1)], originTicks: null,
  });
  assert.equal(t.hasGpu, false);
  assert.match(t.gpuNote, /Capture again/);
});

test("a capture that timed no passes says so rather than blaming the clock", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 0, 1]], { calibration: CALIBRATION }), passes: [], originTicks: 1e6,
  });
  assert.equal(t.hasGpu, false);
  assert.match(t.gpuNote, /No passes were timed/);
});

test("an event on a thread the layer did not list is kept rather than dropped", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 0, 1], [3, "submit", 1, 1]], { threads: [100] }),
    passes: [], originTicks: null,
  });
  assert.equal(t.tracks.length, 2);
  assert.equal(t.tracks[1].label, "Thread #3");
});

test("busy time is the time inside spans, not the range", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 0, 1], [0, "submit", 5, 1]]), passes: [], originTicks: null,
  });
  assert.equal(t.spanMs, 6);
  assert.equal(t.tracks[0].busyMs, 2);
});

test("every span is kept, however many a frame has", () => {
  const events = [];
  for (let i = 0; i < 20000; i++) events.push([0, "submit", i, 0.1]);
  const t = buildTimelineTracks({ timeline: timeline(events), passes: [], originTicks: null });
  // A view draws what its width allows (visibleBoxes); the model drops nothing, so zooming in
  // can reach the end of a frame with thousands of draws.
  assert.equal(t.tracks[0].spans.length, 20000);
  assert.ok(Math.abs(t.tracks[0].busyMs - 20000 * 0.1) < 1e-6);
  assert.equal(t.tracks[0].maxDurationMs, 0.1);
});

test("idle GPU between passes is found, longest first", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    // Passes at host 10-11, 13-14, 20-21 with the CPU call at 10: gaps of 2 and 6 ms.
    passes: [pass("A", 0, 1), pass("B", 3, 1), pass("C", 10, 1)], originTicks: 0,
  });
  const gaps = gpuGaps(t);
  assert.equal(gaps.length, 2);
  assert.equal(gaps[0].durationMs, 6);
  assert.equal(gaps[0].startMs, 4, "after B ends, 4 ms into the range");
  assert.equal(gaps[1].durationMs, 2);
});

test("time before the first pass and after the last is not counted as idle", () => {
  // The axis runs 0-20 ms (the CPU call at 0, the pass at 10-11). Outside the timed passes the GPU
  // may have been running the previous frame, which this capture never timed: counting those ends
  // as idle would report a stall for every capture.
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1], [0, "present", 29, 1]], { calibration: CALIBRATION }),
    passes: [pass("A", 10, 1)], originTicks: 0,
  });
  assert.equal(gpuGaps(t).length, 0);
  const span = gpuSpan(t);
  assert.equal(span.endMs - span.startMs, 1, "the GPU's own span is the pass, not the axis");
  assert.match(tracksVerdict(t), /back to back/);
});

test("the GPU's busy share is of its own span, not of the whole axis", () => {
  // One 1 ms pass on a 20 ms axis is 100% busy over its own span, not 5%: the rest of the axis is
  // not time the GPU was measured to be idle.
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 0, 0.1], [0, "present", 19, 1]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 1)], originTicks: 0,
  });
  assert.match(tracksVerdict(t), /busy for 100%/);
});

test("an idle GPU while the CPU is in present is the display pacing, not a stall", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "present", 11, 8]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 1), pass("B", 10, 1)], originTicks: 0,   // 8 ms gap, CPU in present across it
  });
  const gaps = gpuGaps(t);
  assert.equal(gaps.length, 1);
  const a = attributeGaps(t, gaps);
  assert.ok(a.displayWaitMs >= 7.9, `display wait covers the gap, got ${a.displayWaitMs}`);
  assert.match(tracksVerdict(t), /paced by the display/);
  assert.doesNotMatch(tracksVerdict(t), /waiting on work/);
});

test("an idle GPU while the CPU is submitting blames submission", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 11, 8]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 1), pass("B", 10, 1)], originTicks: 0,
  });
  const v = tracksVerdict(t);
  assert.match(v, /waiting on work/);
  assert.match(v, /fewer, larger submissions/);
});

test("an idle GPU with the CPU in none of the timed calls names the application's own work", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.05]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 1), pass("B", 10, 1)], originTicks: 0,
  });
  const a = attributeGaps(t, gpuGaps(t));
  assert.ok(a.untimedMs > 7, `most of the gap is untimed, got ${a.untimedMs}`);
  assert.match(tracksVerdict(t), /outside the calls the layer times/);
});

test("a tick past 2^53 arrives as a string and is still placed correctly", () => {
  // What a real device sends: the layer quotes an integer a JSON number could not hold (protocol.ts).
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 1]],
      { calibration: { deviceTicks: "1789568468675152992", hostMs: 10, timestampPeriod: 1 } }),
    passes: [pass("A", 0, 2)], originTicks: "1789568468675152992",
  });
  assert.equal(t.hasGpu, true);
  const gpu = t.tracks.find((x) => x.kind === "gpu");
  // Same tick as the calibration, so the pass sits at hostMs 10 — exactly where the submit is.
  assert.ok(Math.abs(gpu.spans[0].startMs - 0) < 0.001, `expected ~0, got ${gpu.spans[0].startMs}`);
});

test("overlapping passes on two queues are not read as a gap", () => {
  // B starts inside A and ends before it. A naive "last end" frontier would report a gap after B.
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 10), pass("B", 1, 1)], originTicks: 0,
  });
  assert.deepEqual(gpuGaps(t), []);
});

test("gaps shorter than the threshold are ordinary pipeline overhead, not stalls", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 1), pass("B", 1.1, 1)], originTicks: 0,
  });
  assert.deepEqual(gpuGaps(t), [], "0.1 ms between passes is not a stall");
});

test("the verdict names an idle GPU, and says it ran back to back when it did", () => {
  const stalled = buildTimelineTracks({
    timeline: timeline([[0, "waitFences", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 1), pass("B", 9, 1)], originTicks: 0,
  });
  assert.match(tracksVerdict(stalled), /idle/);
  assert.match(tracksVerdict(stalled), /8\.00 ms/, "the longest gap is named");

  const fed = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 5), pass("B", 5, 5)], originTicks: 0,
  });
  assert.match(tracksVerdict(fed), /back to back/);
});

test("the wait between the submission and the GPU starting is measured and named", () => {
  // Submit ends at host 11; the pass starts at host 14. Neither total holds that 3 ms.
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 1]], { calibration: CALIBRATION }),
    passes: [pass("A", 4, 1)], originTicks: 0,
  });
  assert.ok(Math.abs(submitToFirstPassMs(t) - 3) < 1e-9);
  assert.match(tracksVerdict(t), /3\.00 ms after the submission before it/);
});

test("a pass that starts straight after its submission adds no latency note", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 1]], { calibration: CALIBRATION }),
    passes: [pass("A", 1.1, 1)], originTicks: 0,   // host 11.1, 0.1 ms after the submit ended
  });
  assert.doesNotMatch(tracksVerdict(t), /after the submission/);
});

test("with no submission before the first pass there is no wait to report", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "present", 20, 1]], { calibration: CALIBRATION }),
    passes: [pass("A", 0, 1)], originTicks: 0,
  });
  assert.equal(submitToFirstPassMs(t), null);
  assert.doesNotMatch(tracksVerdict(t), /after the submission/);
});

test("without a GPU lane the verdict talks about the threads instead", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 0, 5]]), passes: [], originTicks: null,
  });
  const v = tracksVerdict(t);
  assert.match(v, /Thread 4812/);
  assert.doesNotMatch(v, /GPU was busy/);
});

// The view: the stretch of the axis a card draws, and the boxes a lane of that width can hold.
// A frame's spans are sub-pixel at frame scale, so the drawing is only readable if it can be
// zoomed — and only affordable if a zoomed view costs its own width rather than the whole frame.

/** A track of `count` spans, each `durationMs` long, one per millisecond. */
function denseTrack(count, durationMs = 0.1) {
  const events = [];
  for (let i = 0; i < count; i++) events.push([0, "submit", i, durationMs]);
  return buildTimelineTracks({ timeline: timeline(events), passes: [], originTicks: null }).tracks[0];
}

test("a view opens on the whole range and is held inside it", () => {
  const t = buildTimelineTracks({ timeline: timeline([[0, "submit", 0, 1], [0, "submit", 9, 1]]), passes: [], originTicks: null });
  assert.deepEqual(fullView(t), { startMs: 0, spanMs: 10 });
  // Panned past either end, it stops at the end rather than leaving the range.
  assert.deepEqual(clampView({ startMs: -5, spanMs: 2 }, t), { startMs: 0, spanMs: 2 });
  assert.deepEqual(clampView({ startMs: 50, spanMs: 2 }, t), { startMs: 8, spanMs: 2 });
  // Zoomed out past the range, it is the range.
  assert.deepEqual(clampView({ startMs: 3, spanMs: 100 }, t), { startMs: 0, spanMs: 10 });
  assert.equal(clampView({ startMs: 0, spanMs: 1e-9 }, t).spanMs, MIN_VIEW_MS);
});

test("zooming keeps the time under the anchor where it was", () => {
  const t = buildTimelineTracks({ timeline: timeline([[0, "submit", 0, 1], [0, "submit", 9, 1]]), passes: [], originTicks: null });
  const view = zoomView(fullView(t), t, 2, 0.25);   // a quarter across: 2.5 ms in
  assert.equal(view.spanMs, 5);
  assert.equal(view.startMs + view.spanMs * 0.25, 2.5);
  // And panning moves by a fraction of the view, not of the range.
  assert.equal(panView(view, t, 0.5).startMs, view.startMs + 2.5);
});

test("a lane draws only what its view reaches", () => {
  const track = denseTrack(1000);
  // 10 ms of a 1,000 ms range, so 10 spans of the thousand.
  const boxes = visibleBoxes(track, { startMs: 100, spanMs: 10 }, 10 / 800);
  assert.equal(boxes.length, 10);
  assert.equal(boxes[0].count, 1);
  assert.equal(boxes[0].startMs, 100);
  assert.equal(boxes[0].span.label, "Submitting");
});

test("a span reaching into the view from before it is drawn", () => {
  const t = buildTimelineTracks({ timeline: timeline([[0, "submit", 0, 8], [0, "submit", 9, 1]]), passes: [], originTicks: null });
  // The view starts at 5, inside the long span that began at 0: it is still on screen.
  const boxes = visibleBoxes(t.tracks[0], { startMs: 5, spanMs: 2 }, 0.01);
  assert.equal(boxes.length, 1);
  assert.equal(boxes[0].startMs, 0);
  assert.equal(boxes[0].durationMs, 8);
});

test("spans too close together to draw apart become one box that says how many", () => {
  const track = denseTrack(1000);
  // The whole range on an 800-pixel lane: 1.25 ms a pixel, so the spans merge.
  const boxes = visibleBoxes(track, { startMs: 0, spanMs: 1000 }, 1000 / 800);
  assert.ok(boxes.length < 1000, "merged rather than a box per span");
  const spans = boxes.reduce((n, b) => n + b.count, 0);
  assert.equal(spans, 1000, "every span is still accounted for in some box");
  const merged = boxes.find((b) => b.count > 1);
  assert.equal(merged.span, null, "a merged box names no single span, so it cannot be clicked to one");
  assert.ok(merged.busyMs < merged.durationMs, "busy time is the spans, the extent is the stretch they cover");
});

test("zoomed in, the same spans separate again", () => {
  const track = denseTrack(1000);
  const wide = visibleBoxes(track, { startMs: 0, spanMs: 1000 }, 1000 / 800);
  const close = visibleBoxes(track, { startMs: 500, spanMs: 10 }, 10 / 800);
  assert.ok(close.every((b) => b.count === 1), "each span is its own box at this width");
  assert.ok(close.length < wide.reduce((n, b) => n + b.count, 0));
});

test("a merged box is colored by the kind holding most of its time", () => {
  const t = buildTimelineTracks({
    // Two brief submits beside a long wait, all within a pixel of each other.
    timeline: timeline([[0, "submit", 0, 0.01], [0, "waitFences", 0.02, 1], [0, "submit", 1.03, 0.01]]),
    passes: [], originTicks: null,
  });
  const boxes = visibleBoxes(t.tracks[0], { startMs: 0, spanMs: 1.04 }, 1.04);
  assert.equal(boxes.length, 1);
  assert.equal(boxes[0].count, 3);
  assert.equal(boxes[0].kind, "gpuWait");
});

test("a pass span carries the way to select the pass", () => {
  let selected = 0;
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [{ ...pass("Shadows", 0, 1), select: () => { selected++; } }], originTicks: 0,
  });
  const gpu = t.tracks.find((x) => x.kind === "gpu");
  gpu.spans[0].select();
  assert.equal(selected, 1);
});

test("the axis is marked in round numbers across the view, not at its ends", () => {
  const ticks = axisTicks({ startMs: 2.13, spanMs: 1 });
  assert.ok(ticks.length >= 4);
  // Steps of 0.2 ms here, and every mark is inside the view.
  assert.equal(ticks[0].label, "2.20");
  assert.ok(ticks.every((x) => x.ms >= 2.13 && x.ms <= 3.13));
  assert.equal(axisTicks({ startMs: 0, spanMs: 0 }).length, 0);
});


// ---------------------------------------------------------------------------------------------
// A lane per queue

/** A pass on a queue: `queue` is [id, label]. */
function queued(label, startMs, durationMs, [id, qlabel]) {
  return { ...pass(label, startMs, durationMs), queue: { id, label: qlabel } };
}
const DIRECT = [4, "Direct queue (direct)"];
const COMPUTE = [9, "Async compute (compute)"];

test("passes on one queue stay one lane named GPU", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [queued("A", 0, 1, DIRECT), queued("B", 2, 1, DIRECT)], originTicks: 0,
  });
  const gpu = t.tracks.filter((x) => x.kind === "gpu");
  assert.deepEqual(gpu.map((x) => x.label), ["GPU"]);
  assert.equal(gpu[0].spans.length, 2);
});

test("passes on several queues get a lane each, named for the queue, in the order the queues appear", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [queued("Shadows", 0, 1, DIRECT), queued("Particles", 0.5, 2, COMPUTE), queued("Main", 2, 1, DIRECT)],
    originTicks: 0,
  });
  const gpu = t.tracks.filter((x) => x.kind === "gpu");
  assert.deepEqual(gpu.map((x) => x.label), ["GPU: Direct queue (direct)", "GPU: Async compute (compute)"]);
  assert.deepEqual(gpu[0].spans.map((s) => s.label), ["Shadows", "Main"]);
  assert.deepEqual(gpu[1].spans.map((s) => s.label), ["Particles"]);
});

test("a pass no queue could be found for gets a lane of its own when others have queues", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [queued("A", 0, 1, DIRECT), pass("B", 2, 1)], originTicks: 0,
  });
  assert.deepEqual(t.tracks.filter((x) => x.kind === "gpu").map((x) => x.label),
    ["GPU: Direct queue (direct)", "GPU: unknown queue"]);
});

test("idle is time with no queue running: one queue's gap covered by another's work is not idle", () => {
  // Direct runs 0-1 and 5-6, compute 1-4: the only idle stretch is 4-5, which is under the 0.5 ms
  // default, so with a lower threshold it is exactly 1 ms.
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [queued("A", 0, 1, DIRECT), queued("B", 5, 1, DIRECT), queued("C", 1, 3, COMPUTE)], originTicks: 0,
  });
  const gaps = gpuGaps(t, 0.1);
  assert.equal(gaps.length, 1);
  assert.equal(gaps[0].durationMs, 1);
  const span = gpuSpan(t);
  assert.equal(span.endMs - span.startMs, 6, "the GPU's stretch reaches across every queue");
});

test("the verdict counts overlapping queues' time once and says how much they overlapped", () => {
  // Direct 0-4, compute 1-3 inside it: busy for 4 of 4 ms, not 6, and the compute work entirely
  // overlapped the direct queue's.
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [queued("A", 0, 4, DIRECT), queued("B", 1, 2, COMPUTE)], originTicks: 0,
  });
  const v = tracksVerdict(t);
  assert.match(v, /ran 2 passes on 2 queues over 4\.00 ms, busy for 100% of that/);
  assert.match(v, /running at once for 2\.00 ms \(100% of the lightest queue's work\)/);
});

test("queues that take turns are said to", () => {
  const t = buildTimelineTracks({
    timeline: timeline([[0, "submit", 10, 0.1]], { calibration: CALIBRATION }),
    passes: [queued("A", 0, 1, DIRECT), queued("B", 1, 1, COMPUTE), queued("C", 2, 1, DIRECT)], originTicks: 0,
  });
  assert.match(tracksVerdict(t), /taking turns rather than running at once/);
});

test("the queue of each command buffer is read from the submissions in the command stream", () => {
  const ref = (id, cls) => ({ __id: id, __class: cls });
  const cmd = (frame, method, object) => ({ index: 0, frame, method, object, args: null });
  const commands = [
    cmd(0, "ExecuteCommandLists", ref(4, "ID3D12CommandQueue")),
    cmd(0, "Reset", ref(20, "ID3D12GraphicsCommandList")),
    cmd(0, "DrawInstanced", ref(20, "ID3D12GraphicsCommandList")),
    cmd(0, "ExecuteCommandLists", ref(9, "ID3D12CommandQueue")),
    cmd(0, "Dispatch", ref(21, "ID3D12GraphicsCommandList")),
    cmd(0, "Present", ref(3, "IDXGISwapChain")),
    // The next frame submits list 20 to the compute queue: a queue is per frame, not per list.
    cmd(1, "vkQueueSubmit", ref(9, "VkQueue")),
    cmd(1, "vkCmdDraw", ref(20, "VkCommandBuffer")),
  ];
  const q = commandBufferQueues(commands);
  assert.equal(q.get("0:20"), 4);
  assert.equal(q.get("0:21"), 9);
  assert.equal(q.get("1:20"), 9);
});

test("with no queue in the stream (Metal) the caller's lookup answers", () => {
  const commands = [{ index: 0, frame: 0, method: "commit", object: { __id: 30, __class: "MTLCommandBuffer" }, args: null }];
  assert.equal(commandBufferQueues(commands, (cb) => (cb === 30 ? 7 : null)).get("0:30"), 7);
  assert.equal(commandBufferQueues(commands).size, 0);
});
