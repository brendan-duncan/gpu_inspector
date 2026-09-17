// Where a captured frame's CPU time went, beside where its GPU time went
// (src/vulkan/src/cpu_timeline.h).
//
// docs/PROFILING.md's first step asks whether the GPU is the problem at all, and until now it was
// answered by comparing aggregates: the frame interval, the time inside vkQueueSubmit, the passes'
// GPU time. That ratio cannot tell two very different frames apart. A frame blocked five
// milliseconds in vkWaitForFences and a frame spending five milliseconds building command buffers
// have the same CPU total and opposite fixes: the first is waiting for the GPU and wants less GPU
// work, the second is the bottleneck itself and wants cheaper submission.
//
// So the layer times the calls where the CPU actually goes, and this turns them into the verdict.
import type { CpuEvent, CpuTimelineMessage } from "../shared/protocol.js";

/** What each category means when it dominates, in the order the summary lists them. */
export const CPU_CATEGORY_LABEL: Record<string, string> = {
  submit: "Submitting",
  present: "Presenting",
  waitFences: "Waiting on fences",
  acquire: "Waiting for a swapchain image",
  waitIdle: "Waiting for idle",
  pipeline: "Creating pipelines",
};

/**
 * What a category means for the verdict. Waiting is not one thing: a frame blocked in
 * vkWaitForFences is waiting for the GPU to finish work, and a frame blocked in vkQueuePresentKHR
 * or vkAcquireNextImageKHR is being paced by the display, which on a vsynced swapchain means
 * neither processor is the limit. Calling both "blocked" would advise the same fix for opposite
 * situations.
 */
export type CpuKind = "gpuWait" | "displayWait" | "work" | "compile";

export const CPU_CATEGORY_KIND: Record<string, CpuKind> = {
  waitFences: "gpuWait",
  waitIdle: "gpuWait",
  present: "displayWait",
  acquire: "displayWait",
  submit: "work",
  // Kept apart from submission, though both are the application's own thread doing something
  // rather than waiting, because the two want opposite fixes: submission wants fewer and larger
  // submits, a compile wants the pipeline built before the frame that needs it.
  pipeline: "compile",
};

export function cpuKindOf(category: string): CpuKind {
  return CPU_CATEGORY_KIND[category] ?? "work";
}

export interface CpuCategoryTotal {
  category: string;
  label: string;
  calls: number;
  ms: number;
  kind: CpuKind;
}

export interface CpuTimelineSummary {
  /** Wall-clock span the events cover, from the first start to the last end. */
  spanMs: number;
  /** Time inside timed calls, split by what they were doing. Largest first. */
  totals: CpuCategoryTotal[];
  /** Time waiting for the GPU, time paced by the display, and time spent submitting. */
  gpuWaitMs: number;
  displayWaitMs: number;
  submitMs: number;
  /** Time inside pipeline and shader creation: a compile the frame stopped for. */
  compileMs: number;
  threads: number;
  frames: number;
  /** Events the capture could not keep. */
  dropped: number;
  /** The GPU's clock could be related to the CPU's, so both can share an axis. */
  calibrated: boolean;
}

/** Nothing recorded: a capture from a layer before the timeline, or one that timed no calls. */
export function summarizeCpuTimeline(timeline: CpuTimelineMessage | null): CpuTimelineSummary | null {
  if (!timeline || !timeline.events?.length) return null;
  const events = timeline.events;
  let start = Infinity;
  let end = -Infinity;
  const byCategory = new Map<string, { calls: number; ms: number }>();
  const frames = new Set<number>();
  for (const e of events) {
    start = Math.min(start, e.startMs);
    end = Math.max(end, e.startMs + e.durationMs);
    frames.add(e.frame);
    const t = byCategory.get(e.category) ?? { calls: 0, ms: 0 };
    t.calls++;
    t.ms += e.durationMs;
    byCategory.set(e.category, t);
  }
  const totals: CpuCategoryTotal[] = [...byCategory.entries()]
    .map(([category, t]) => ({
      category, label: CPU_CATEGORY_LABEL[category] ?? category, calls: t.calls, ms: t.ms, kind: cpuKindOf(category),
    }))
    .sort((a, b) => b.ms - a.ms);
  let gpuWaitMs = 0;
  let displayWaitMs = 0;
  let submitMs = 0;
  let compileMs = 0;
  for (const t of totals) {
    if (t.kind === "gpuWait") gpuWaitMs += t.ms;
    else if (t.kind === "displayWait") displayWaitMs += t.ms;
    else if (t.kind === "compile") compileMs += t.ms;
    else submitMs += t.ms;
  }
  return {
    spanMs: end > start ? end - start : 0,
    totals, gpuWaitMs, displayWaitMs, submitMs, compileMs,
    threads: timeline.threads?.length ?? 1,
    frames: frames.size,
    dropped: timeline.dropped ?? 0,
    calibrated: !!timeline.calibration,
  };
}

/**
 * The verdict in one sentence, in the terms docs/PROFILING.md's first step asks for: is the GPU the
 * limit, is submission the limit, or is the frame simply paced by the display.
 */
export function cpuVerdict(s: CpuTimelineSummary): string {
  const share = (ms: number): number => (s.spanMs > 0 ? ms / s.spanMs : 0);
  const gpu = share(s.gpuWaitMs);
  const display = share(s.displayWaitMs);
  const submit = share(s.submitMs);
  const compile = share(s.compileMs);
  const pct = (v: number): string => `${(100 * v).toFixed(0)}%`;
  // Read before the rest: a pipeline built inside a frame stops it, and no share of the others
  // explains that away. It is also the one finding here whose fix is not "do less of something"
  // but "do it earlier", so it would be wrong to fold into the submission verdict.
  if (compile >= 0.1) {
    const calls = s.totals.find((t) => t.kind === "compile")?.calls ?? 0;
    return `The CPU spent ${pct(compile)} of this capture creating pipelines — ${calls} `
      + `${calls === 1 ? "call" : "calls"} inside the captured frames. A pipeline built while the frame that needs it `
      + "is being recorded stops that frame for as long as the driver takes to compile it, which is the usual cause "
      + "of a hitch on first sight of a material or an effect. Build them at load, or from a pipeline cache.";
  }
  if (gpu >= 0.4) {
    return `The CPU spent ${pct(gpu)} of this capture waiting on fences, so it is ahead of the GPU and the GPU is `
      + "what sets the frame time. GPU Bottlenecks says which pass to shorten.";
  }
  if (submit >= 0.3) {
    return `The CPU spent ${pct(submit)} of this capture inside submission, which is a real cost at that share: `
      + "fewer and larger submissions, fewer command buffers, and less state churn per draw.";
  }
  if (display >= 0.4) {
    return `The CPU spent ${pct(display)} of this capture in present and acquire and only ${pct(gpu)} waiting on the `
      + "GPU, so the frame is paced by the display rather than limited by either processor. Neither has to get faster "
      + "for this frame rate; both would have to for a higher one.";
  }
  return `Only ${pct(gpu + display + submit + compile)} of this capture was inside calls the layer times, so most of `
    + "the frame went to the application's own work between them: building command buffers, culling, simulation.";
}

/** The events of one thread in time order, for a track of a timeline drawing. */
export function eventsOfThread(timeline: CpuTimelineMessage, thread: number): CpuEvent[] {
  return timeline.events.filter((e) => e.thread === thread);
}

/**
 * Where a GPU timestamp sits on the CPU axis, in milliseconds from the capture's origin. Null
 * without a calibration, which is a device with no calibrated-timestamps extension: the CPU events
 * and the GPU passes are then each correct on their own axis but cannot be laid over each other.
 */
export function gpuTicksToCpuMs(timeline: CpuTimelineMessage | null, ticks: number | string): number | null {
  const c = timeline?.calibration;
  if (!c) return null;
  // Both ticks arrive as decimal strings on a real device, since a GPU tick counter is past 2^53
  // and the layer quotes what a JSON number could not hold (protocol.ts). Number() rounds each to
  // the nearest representable value — 256 ns apart at that magnitude — so their difference is off by
  // at most half a microsecond, which is nothing beside the milliseconds this places things on.
  return c.hostMs + (Number(ticks) - Number(c.deviceTicks)) * c.timestampPeriod / 1e6;
}
