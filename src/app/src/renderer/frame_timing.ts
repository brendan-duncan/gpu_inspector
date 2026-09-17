// A timing capture: every frame's wall time and where its CPU went, over seconds or minutes
// (src/vulkan/src/cpu_timeline.h).
//
// The two things the inspector could say about time until now were both the wrong shape for a
// hitch. A frame report averages over its interval — five or six frames at 60 Hz — so one slow
// frame among good ones disappears into the mean. A capture keeps every call, but only of a handful
// of frames, so it can only describe a hitch that was already caught in the act.
//
// A hitch is one frame, and the question about it is always the same: how bad, how often, and what
// was the CPU doing. This is what answers that.
import { CPU_CATEGORY_LABEL } from "./cpu_timeline.js";

export interface TimingFrame {
  frame: number;
  durationMs: number;
  /** Milliseconds in each category, indexed by the capture's `categories`. */
  categoryMs: number[];
}

export interface TimingCapture {
  /** Category names, in the order `categoryMs` is indexed by. */
  categories: string[];
  frames: TimingFrame[];
}

export interface CategoryTotal {
  category: string;
  label: string;
  ms: number;
  /** Of the range's total wall time, 0 to 1. */
  share: number;
}

export interface Hitch {
  /** Index into the range's frames, and the application's own frame number. */
  index: number;
  frame: number;
  durationMs: number;
  /** How many times the median frame this one took. */
  times: number;
  /**
   * The category that best explains the time over the median, or null when none of the timed calls
   * do — which means the application's own work between them, and is itself the finding.
   */
  cause: CategoryTotal | null;
}

export interface TimingSummary {
  frames: number;
  medianMs: number;
  p95Ms: number;
  p99Ms: number;
  maxMs: number;
  /** Wall time the range covers. */
  totalMs: number;
  /** Worst first. */
  hitches: Hitch[];
  /** Largest first, over the whole range. */
  categories: CategoryTotal[];
  verdict: string;
}

/** About twenty minutes at 60 Hz, matching the ring the layer keeps (cpu_timeline.cpp). */
export const MAX_TIMING_FRAMES = 72000;

/**
 * A frame is a hitch when it takes more than twice the median *and* at least this much longer than
 * it. The multiple alone would call ordinary jitter a hitch in an application running at 300 frames
 * a second, where twice the median is still three milliseconds and nobody would feel it.
 */
export const HITCH_FACTOR = 2;
export const HITCH_FLOOR_MS = 4;
/** A category has to account for this much of a frame's time over the median to be called its cause. */
export const CAUSE_SHARE = 0.5;

function percentile(sorted: number[], p: number): number {
  if (!sorted.length) return 0;
  const at = Math.min(sorted.length - 1, Math.max(0, Math.round(p * (sorted.length - 1))));
  return sorted[at];
}

function label(category: string): string {
  return CPU_CATEGORY_LABEL[category] ?? category;
}

/** The hitch threshold for a range whose median frame is `medianMs`. */
export function hitchThresholdMs(medianMs: number): number {
  return Math.max(medianMs * HITCH_FACTOR, medianMs + HITCH_FLOOR_MS);
}

/**
 * Summarizes a range of frames: the distribution, the hitches in it and what caused them.
 * Null for an empty range, so the caller can ask before checking.
 */
export function summarizeTiming(capture: TimingCapture, from = 0, to = capture.frames.length): TimingSummary | null {
  const frames = capture.frames.slice(Math.max(0, from), Math.max(0, to));
  if (!frames.length) return null;
  const durations = frames.map((f) => f.durationMs);
  const sorted = [...durations].sort((a, b) => a - b);
  const medianMs = percentile(sorted, 0.5);
  const totalMs = durations.reduce((sum, v) => sum + v, 0);

  const totals = capture.categories.map((category, i) => {
    const ms = frames.reduce((sum, f) => sum + (f.categoryMs[i] ?? 0), 0);
    return { category, label: label(category), ms, share: totalMs > 0 ? ms / totalMs : 0 };
  }).filter((t) => t.ms > 0).sort((a, b) => b.ms - a.ms);

  const threshold = hitchThresholdMs(medianMs);
  const hitches: Hitch[] = [];
  frames.forEach((f, index) => {
    if (f.durationMs <= threshold) return;
    // What the frame spent over an ordinary one is what has to be explained; a category that was
    // going to run anyway is not the cause of the hitch.
    const excess = f.durationMs - medianMs;
    let best: CategoryTotal | null = null;
    capture.categories.forEach((category, i) => {
      const ms = f.categoryMs[i] ?? 0;
      if (ms <= 0 || ms < excess * CAUSE_SHARE) return;
      if (!best || ms > best.ms) best = { category, label: label(category), ms, share: excess > 0 ? ms / excess : 0 };
    });
    hitches.push({ index, frame: f.frame, durationMs: f.durationMs, times: medianMs > 0 ? f.durationMs / medianMs : 0, cause: best });
  });
  hitches.sort((a, b) => b.durationMs - a.durationMs);

  return {
    frames: frames.length,
    medianMs,
    p95Ms: percentile(sorted, 0.95),
    p99Ms: percentile(sorted, 0.99),
    maxMs: sorted[sorted.length - 1],
    totalMs,
    hitches,
    categories: totals,
    verdict: verdictFor(frames.length, medianMs, hitches),
  };
}

function verdictFor(count: number, medianMs: number, hitches: Hitch[]): string {
  const fps = medianMs > 0 ? (1000 / medianMs).toFixed(0) : "?";
  if (!hitches.length) {
    return `${count} frames at a median of ${medianMs.toFixed(2)} ms (${fps} fps), with no frame taking more than `
      + `${hitchThresholdMs(medianMs).toFixed(2)} ms. Nothing here hitched.`;
  }
  const worst = hitches[0];
  const rate = hitches.length === 1 ? "once" : `${hitches.length} times`;
  const cause = worst.cause
    ? `${worst.cause.ms.toFixed(2)} ms of it inside ${worst.cause.label.toLowerCase()}`
    : "none of the calls the layer times account for it, so it went to the application's own work between them";
  return `${count} frames at a median of ${medianMs.toFixed(2)} ms (${fps} fps), hitching ${rate}. The worst was frame `
    + `${worst.frame} at ${worst.durationMs.toFixed(2)} ms, ${worst.times.toFixed(1)} times an ordinary frame: ${cause}.`;
}
