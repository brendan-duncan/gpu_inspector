// The capture's CPU and GPU activity as tracks on one time axis.
//
// "Where the CPU went" (cpu_timeline.ts) totals the time per category, and Pass Timings totals it
// per pass. Both answer "how much", and neither can answer "when", which is the question that
// matters once the totals look reasonable: a frame whose CPU and GPU totals are both well inside
// budget can still miss it, because the GPU sat idle waiting for a submission that came late. That
// gap has no size in any total — it is the space *between* the spans — so it is invisible until the
// spans are drawn against each other.
//
// Putting both on one axis needs the device clock related to the host clock. The layer samples that
// relation where the device offers VK_KHR_calibrated_timestamps (src/vulkan/src/cpu_timeline.h) and
// sends the tick the pass starts are measured from as `originTicks`. Without either the CPU lanes
// are still drawn — they are on the host clock already — and the GPU lane is left out rather than
// guessed at, because a GPU lane placed on the wrong origin would invent exactly the idle gaps this
// view exists to find.
import { CPU_CATEGORY_LABEL, cpuKindOf, gpuTicksToCpuMs, type CpuKind } from "./cpu_timeline.js";
import type { CpuTimelineMessage, PassTiming } from "../shared/protocol.js";

/** What a span is, which decides how it reads and how it is coloured. */
export type SpanKind = CpuKind | "gpu";

export interface TrackSpan {
  /** Milliseconds from the start of the drawn range. */
  startMs: number;
  durationMs: number;
  /** "vkQueueSubmit", "Shadow pass": what this span was. */
  label: string;
  kind: SpanKind;
}

export interface Track {
  /** "Thread 4812 (main)", "GPU". */
  label: string;
  kind: "cpu" | "gpu";
  spans: TrackSpan[];
  /** Time covered by the spans, which for a CPU lane is time inside timed calls. */
  busyMs: number;
}

export interface TimelineTracks {
  tracks: Track[];
  /** The drawn range: every span's startMs is relative to this, and none exceeds spanMs. */
  spanMs: number;
  /** The GPU lane is present. False when the device's clock could not be related to the host's. */
  hasGpu: boolean;
  /**
   * Why there is no GPU lane, for the view to show instead of leaving it unexplained. Null when
   * there is one.
   */
  gpuNote: string | null;
}

/** A pass with the label the UI shows for it, since PassTiming itself carries only ids. */
export interface LabelledPass {
  timing: PassTiming;
  label: string;
}

/**
 * A name for a timed pass from the pass itself. The fallback for a pass the UI has no block for,
 * which a multi-frame capture has: a command buffer submitted again is numbered from the start by
 * the layer but cumulatively by the pass metrics, so the metrics' name cannot be found for every
 * timed pass. The track is built from the timings rather than the metrics precisely so that a pass
 * the two disagree about is still drawn.
 */
export function defaultPassLabel(t: PassTiming): string {
  return `${t.kind === "compute" ? "Compute" : "Render"} pass ${t.passIndex} (frame ${t.frame})`;
}

export interface TimelineInput {
  timeline: CpuTimelineMessage | null;
  passes: LabelledPass[];
  /** CaptureData.passTimingOrigin: the device tick the pass starts are measured from. */
  originTicks: number | null;
}

/** Spans shorter than this are still drawn, but at this width, so a brief call does not vanish. */
export const MIN_SPAN_MS = 0.002;

/** How many spans one track keeps. A frame with thousands of passes would otherwise draw thousands. */
export const MAX_SPANS_PER_TRACK = 4000;

function threadLabel(tid: number, index: number, total: number): string {
  // The first thread the layer saw is the one that began the capture, which is the render thread in
  // every engine that has one. Saying so is more use than the bare OS id.
  return total > 1 && index === 0 ? `Thread ${tid} (main)` : `Thread ${tid}`;
}

/**
 * The tracks of a capture, or null when there is nothing to draw: no CPU events recorded, which is
 * a capture taken before the layer timed them.
 */
export function buildTimelineTracks(input: TimelineInput): TimelineTracks | null {
  const { timeline, passes, originTicks } = input;
  if (!timeline?.events?.length) return null;

  // The GPU lane needs both halves of the relation: the calibration that maps ticks to the host
  // clock, and the origin the pass starts were measured from. Either missing means no lane.
  const gpuOriginMs = originTicks !== null ? gpuTicksToCpuMs(timeline, originTicks) : null;
  const hasGpu = gpuOriginMs !== null && passes.length > 0;
  const gpuNote = hasGpu ? null
    : !passes.length ? "No passes were timed in this capture, so there is no GPU track."
    : !timeline.calibration
      ? "This device has no calibrated-timestamps extension, so the GPU's clock cannot be related to the CPU's. "
        + "The pass times are correct among themselves but cannot be placed beside these calls."
      : "This capture was taken before the layer recorded the GPU clock's origin, so the passes cannot be placed "
        + "on this axis. Capture again to see the GPU track.";

  // One lane per thread, in the order the layer listed them.
  const threads = timeline.threads ?? [];
  const byThread = new Map<number, TrackSpan[]>();
  let min = Infinity;
  let max = -Infinity;
  for (const e of timeline.events) {
    min = Math.min(min, e.startMs);
    max = Math.max(max, e.startMs + e.durationMs);
    let spans = byThread.get(e.thread);
    if (!spans) byThread.set(e.thread, (spans = []));
    spans.push({
      startMs: e.startMs, durationMs: e.durationMs,
      label: CPU_CATEGORY_LABEL[e.category] ?? e.category, kind: cpuKindOf(e.category),
    });
  }

  const gpuSpans: TrackSpan[] = [];
  if (hasGpu) {
    for (const p of passes) {
      const startMs = gpuOriginMs + p.timing.startMs;
      min = Math.min(min, startMs);
      max = Math.max(max, startMs + p.timing.durationMs);
      gpuSpans.push({ startMs, durationMs: p.timing.durationMs, label: p.label, kind: "gpu" });
    }
  }
  if (!(max > min)) return null;

  const finish = (spans: TrackSpan[]): { spans: TrackSpan[]; busyMs: number } => {
    spans.sort((a, b) => a.startMs - b.startMs);
    let busyMs = 0;
    for (const s of spans) busyMs += s.durationMs;
    // Rebased onto the drawn range, so a view only has to scale by spanMs.
    const rebased = spans.slice(0, MAX_SPANS_PER_TRACK).map((s) => ({ ...s, startMs: s.startMs - min }));
    return { spans: rebased, busyMs };
  };

  const tracks: Track[] = [];
  threads.forEach((tid, i) => {
    const spans = byThread.get(i);
    if (!spans?.length) return;
    const { spans: out, busyMs } = finish(spans);
    tracks.push({ label: threadLabel(tid, i, threads.length), kind: "cpu", spans: out, busyMs });
  });
  // An event whose thread index is not in the list would otherwise be dropped silently.
  for (const [index, spans] of byThread) {
    if (index < threads.length) continue;
    const { spans: out, busyMs } = finish(spans);
    tracks.push({ label: `Thread #${index}`, kind: "cpu", spans: out, busyMs });
  }
  if (hasGpu) {
    const { spans: out, busyMs } = finish(gpuSpans);
    tracks.push({ label: "GPU", kind: "gpu", spans: out, busyMs });
  }
  if (!tracks.length) return null;

  return { tracks, spanMs: max - min, hasGpu, gpuNote };
}

/** One stretch between two timed passes with nothing running. */
export interface GpuGap {
  startMs: number;
  durationMs: number;
}

/**
 * The GPU's own stretch of the axis: its first pass start to its last pass end. Null without a GPU
 * lane. The drawn range is wider than this, because it also covers the CPU calls.
 */
export function gpuSpan(t: TimelineTracks): { startMs: number; endMs: number } | null {
  const gpu = t.tracks.find((x) => x.kind === "gpu");
  if (!gpu?.spans.length) return null;
  let startMs = Infinity;
  let endMs = -Infinity;
  for (const s of gpu.spans) {
    startMs = Math.min(startMs, s.startMs);
    endMs = Math.max(endMs, s.startMs + s.durationMs);
  }
  return { startMs, endMs };
}

/**
 * Stretches *between* timed passes where the GPU had nothing running, longest first. This is what
 * the drawing exists to make visible: idle GPU inside a frame's work is time nothing is using, and
 * it never appears in a total.
 *
 * Only the interior counts. The axis reaches past the first and last pass to cover the CPU calls,
 * but the GPU was not necessarily idle there — outside the timed region it may well have been
 * running the previous frame's passes, which this capture never timed. Counting those ends as idle
 * would report a stall for every capture, and the largest one for the shortest frame.
 *
 * Gaps shorter than `minMs` are left out, since the space between two passes of one frame is
 * ordinary pipeline overhead rather than a stall.
 */
export function gpuGaps(t: TimelineTracks, minMs = 0.5): GpuGap[] {
  const gpu = t.tracks.find((x) => x.kind === "gpu");
  if (!gpu?.spans.length) return [];
  const gaps: GpuGap[] = [];
  // Passes can overlap (several queues), so the frontier is the furthest end seen, not the last.
  let frontier = gpu.spans[0].startMs;
  for (const s of gpu.spans) {
    if (s.startMs - frontier >= minMs) gaps.push({ startMs: frontier, durationMs: s.startMs - frontier });
    frontier = Math.max(frontier, s.startMs + s.durationMs);
  }
  return gaps.sort((a, b) => b.durationMs - a.durationMs);
}

/**
 * How long the GPU's first pass waited after the last submission that preceded it. Null when there
 * is no GPU lane or no submission before it.
 *
 * This is latency rather than throughput, and no total contains it: both processors can be well
 * inside budget while the work still lands late. What it does not say is *why* — the closest
 * submission is not necessarily the one that queued that pass, and a swapchain image the display
 * has not released yet will hold work back however early it was submitted.
 */
export function submitToFirstPassMs(t: TimelineTracks): number | null {
  const span = gpuSpan(t);
  if (!span) return null;
  let latestEnd = -Infinity;
  for (const track of t.tracks) {
    if (track.kind !== "cpu") continue;
    for (const s of track.spans) {
      if (s.kind !== "work") continue;
      const end = s.startMs + s.durationMs;
      if (end <= span.startMs) latestEnd = Math.max(latestEnd, end);
    }
  }
  return latestEnd > -Infinity ? span.startMs - latestEnd : null;
}

/** How much of a stretch each CPU track spent inside a timed call of each kind. */
export interface GapAttribution {
  displayWaitMs: number;
  gpuWaitMs: number;
  workMs: number;
  /** Time in the stretch with no timed call at all: the application's own work between them. */
  untimedMs: number;
}

/** What the CPU tracks were doing across these stretches, which is what says why the GPU was idle. */
export function attributeGaps(t: TimelineTracks, gaps: GpuGap[]): GapAttribution {
  const out: GapAttribution = { displayWaitMs: 0, gpuWaitMs: 0, workMs: 0, untimedMs: 0 };
  let total = 0;
  for (const g of gaps) {
    total += g.durationMs;
    const end = g.startMs + g.durationMs;
    for (const track of t.tracks) {
      if (track.kind !== "cpu") continue;
      for (const s of track.spans) {
        const overlap = Math.min(end, s.startMs + s.durationMs) - Math.max(g.startMs, s.startMs);
        if (overlap <= 0) continue;
        if (s.kind === "displayWait") out.displayWaitMs += overlap;
        else if (s.kind === "gpuWait") out.gpuWaitMs += overlap;
        else out.workMs += overlap;
      }
    }
  }
  out.untimedMs = Math.max(0, total - out.displayWaitMs - out.gpuWaitMs - out.workMs);
  return out;
}

/**
 * What the drawing shows, in words. The tracks make a stall visible; this names it, so the answer
 * does not depend on the reader spotting it — and, where the GPU idled, says what the CPU was doing
 * meanwhile, since that is what separates a stall from a frame simply paced by the display.
 */
export function tracksVerdict(t: TimelineTracks): string {
  if (!t.hasGpu) {
    const busiest = [...t.tracks].sort((a, b) => b.busyMs - a.busyMs)[0];
    const share = t.spanMs > 0 ? busiest.busyMs / t.spanMs : 0;
    return `${t.tracks.length} thread${t.tracks.length === 1 ? "" : "s"} over ${t.spanMs.toFixed(2)} ms. `
      + `${busiest.label} spent ${(100 * share).toFixed(0)}% of it inside calls the layer times.`;
  }
  const gpu = t.tracks.find((x) => x.kind === "gpu")!;
  const span = gpuSpan(t)!;
  const gpuSpanMs = span.endMs - span.startMs;
  const gaps = gpuGaps(t);
  const idle = gaps.reduce((sum, g) => sum + g.durationMs, 0);
  const head = `The GPU ran ${gpu.spans.length} pass${gpu.spans.length === 1 ? "" : "es"} over ${gpuSpanMs.toFixed(2)} ms, `
    + `busy for ${(100 * (gpuSpanMs > 0 ? gpu.busyMs / gpuSpanMs : 0)).toFixed(0)}% of that. `;

  // Latency the totals cannot hold: work handed over early can still start late.
  const wait = submitToFirstPassMs(t);
  const latency = wait !== null && wait >= 0.5
    ? ` Its first pass began ${wait.toFixed(2)} ms after the submission before it, so the work waited that long `
      + "between being handed over and starting — a swapchain image the display has not released yet is the usual "
      + "reason, and it costs latency rather than frame time."
    : "";

  if (!gaps.length) {
    return head + "Its passes ran back to back, so that time is the work itself rather than gaps in it." + latency;
  }
  const a = attributeGaps(t, gaps);
  const why = a.displayWaitMs >= idle * 0.4
    ? "The CPU was in present or acquire for most of that, so the frame is paced by the display and the idle GPU is "
      + "headroom rather than a stall."
    : a.workMs >= idle * 0.4
      ? "The CPU was inside submission for much of that, so the GPU is waiting on work the CPU had not finished "
        + "handing it: fewer, larger submissions would close the gap."
      : a.gpuWaitMs >= idle * 0.4
        ? "The CPU was waiting on a fence for most of that, which with an idle GPU means it is waiting on work "
          + "already finished: the fence is being waited on later than it is signalled."
        : "The CPU was outside the calls the layer times for most of that — its own work between them: building "
          + "command buffers, culling, simulation — so that is where the GPU's idle time is going.";
  return head + `It went idle between passes for ${idle.toFixed(2)} ms across ${gaps.length} gap`
    + `${gaps.length === 1 ? "" : "s"}, the longest ${gaps[0].durationMs.toFixed(2)} ms. ` + why + latency;
}
