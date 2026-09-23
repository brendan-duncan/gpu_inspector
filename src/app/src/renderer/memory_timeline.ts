// Memory as a shape rather than an instant.
//
// memory_heaps.ts answers "how much is held now", from the object graph. That is the right answer
// to the wrong question when the complaint is "it runs out after twenty minutes": a renderer that
// has leaked a gigabyte and one that legitimately holds a gigabyte look identical at any single
// moment. What tells them apart is the direction — climbing frame after frame is a leak, sawtooth
// is a pool being emptied and refilled, flat is neither.
//
// So the capture libraries keep a running total per heap and send a MemorySample with each frame
// report (src/vulkan/src/cpu_timeline.h, src/d3d12/src/cpu_timeline.h). This turns that series into
// the two things worth saying about it: what it looks like, and whether it is growing.
import type { MemorySampleMessage } from "../shared/protocol.js";

/** One point of the series, flattened across heaps for the summary. */
export interface MemoryPoint {
  frame: number;
  /** Bytes this application holds across every heap, and in how many allocations. */
  allocated: number;
  allocations: number;
  /** The driver's resident total across every heap, where it reported one. */
  usage: number | null;
  /** D3D12: what the application evicted and paged back in since the previous sample, and whether the budget moved. */
  evicted: number;
  madeResident: number;
  budgetChanged: boolean;
}

/** What the series is doing, which is the question an instant cannot answer. */
export type MemoryTrend = "growing" | "sawtooth" | "flat" | "shrinking";

export interface MemoryTimeline {
  points: MemoryPoint[];
  /** Range of `allocated` over the series, for drawing. */
  minBytes: number;
  maxBytes: number;
  /** Frames the series covers. */
  frames: number;
  trend: MemoryTrend;
  /**
   * Bytes per frame the trend line climbs, over the whole series. Negative when shrinking, and near
   * zero for flat and sawtooth alike — which is why the trend is not read from this alone.
   */
  bytesPerFrame: number;
  /** Peak-to-trough swing, which is what separates a sawtooth from a flat line. */
  swingBytes: number;
  /** The driver reported residency, so `usage` is real rather than absent. */
  hasUsage: boolean;
  /** Residency over the series: what was evicted and paged back in, and how often the budget moved. */
  evictedBytes: number;
  residentBytes: number;
  budgetChanges: number;
}

/**
 * Growth worth calling a leak, as a share of the series' own peak. A renderer that ends 20% above
 * where it started over hundreds of frames is not refilling a pool; a threshold in bytes would
 * instead flag every large application and miss every small one.
 */
export const GROWTH_SHARE = 0.2;

/**
 * A swing this large relative to the peak means the total falls as well as rises, which is a pool
 * being emptied and refilled rather than a leak — even when the endpoints differ.
 */
export const SAWTOOTH_SHARE = 0.1;

/** How many samples to keep. At a sample every ~100 ms this is around an hour. */
export const MAX_SAMPLES = 36000;

function totalOf(m: MemorySampleMessage): MemoryPoint {
  let allocated = 0, allocations = 0, usage = 0, evicted = 0, madeResident = 0;
  let hasUsage = false, budgetChanged = false;
  for (const h of m.heaps ?? []) {
    allocated += h.allocated ?? 0;
    allocations += h.allocations ?? 0;
    if (typeof h.usage === "number") { usage += h.usage; hasUsage = true; }
    evicted += h.evicted ?? 0;
    madeResident += h.madeResident ?? 0;
    if (h.budgetChanged) budgetChanged = true;
  }
  return { frame: m.frame, allocated, allocations, usage: hasUsage ? usage : null, evicted, madeResident, budgetChanged };
}

/**
 * The slope of the least-squares line through the points, in bytes per frame. Least squares rather
 * than first-to-last because a single spike at either end would otherwise decide the answer.
 */
function slope(points: MemoryPoint[]): number {
  const n = points.length;
  if (n < 2) return 0;
  let sx = 0;
  let sy = 0;
  for (const p of points) {
    sx += p.frame;
    sy += p.allocated;
  }
  const mx = sx / n;
  const my = sy / n;
  let num = 0;
  let den = 0;
  for (const p of points) {
    const dx = p.frame - mx;
    num += dx * (p.allocated - my);
    den += dx * dx;
  }
  return den > 0 ? num / den : 0;
}

/**
 * The series as a shape, or null with fewer than two samples — one point has no direction, and
 * saying "flat" from it would be a guess rather than a reading.
 */
export function memoryTimeline(samples: MemorySampleMessage[]): MemoryTimeline | null {
  const points = samples.map(totalOf);
  if (points.length < 2) return null;

  let minBytes = Infinity;
  let maxBytes = -Infinity;
  for (const p of points) {
    minBytes = Math.min(minBytes, p.allocated);
    maxBytes = Math.max(maxBytes, p.allocated);
  }
  const first = points[0];
  const last = points[points.length - 1];
  const frames = Math.max(0, last.frame - first.frame);
  const bytesPerFrame = slope(points);
  const swingBytes = maxBytes - minBytes;
  const peak = maxBytes || 1;

  // What separates the shapes is how much the series moves *each way*, not where it happens to
  // start and end. A pool that empties and refills can finish on a peak, at a trough, or anywhere
  // between, purely by where the sampling stopped — so endpoints cannot tell it from a leak. Adding
  // up the rises and the falls separately can: a leak only ever rises, a release only ever falls,
  // and a pool does both.
  let roseBytes = 0;
  let fellBytes = 0;
  for (let i = 1; i < points.length; i++) {
    const delta = points[i].allocated - points[i - 1].allocated;
    if (delta > 0) roseBytes += delta;
    else fellBytes -= delta;
  }
  const net = last.allocated - first.allocated;
  const rose = roseBytes / peak >= SAWTOOTH_SHARE;
  const fell = fellBytes / peak >= SAWTOOTH_SHARE;
  let trend: MemoryTrend;
  if (rose && fell) trend = "sawtooth";
  else if (net / peak >= GROWTH_SHARE && bytesPerFrame > 0) trend = "growing";
  else if (-net / peak >= GROWTH_SHARE) trend = "shrinking";
  else trend = "flat";

  return {
    points, minBytes, maxBytes, frames, trend, bytesPerFrame, swingBytes,
    hasUsage: points.some((p) => p.usage !== null),
    evictedBytes: points.reduce((sum, p) => sum + p.evicted, 0),
    residentBytes: points.reduce((sum, p) => sum + p.madeResident, 0),
    budgetChanges: points.filter((p) => p.budgetChanged).length,
  };
}

function bytes(n: number): string {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let v = Math.abs(n);
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${n < 0 ? "-" : ""}${v.toFixed(v < 10 && i > 0 ? 1 : 0)} ${units[i]}`;
}

/** What the shape means, in words, with the numbers it was read from. */
/** The residency sentence for the series, or "" when nothing was evicted, paged in or re-budgeted. */
export function residencyText(t: MemoryTimeline): string {
  const parts: string[] = [];
  if (t.evictedBytes || t.residentBytes) parts.push(`the application evicted ${bytes(t.evictedBytes)} and paged ${bytes(t.residentBytes)} back in (the marks: red down for an eviction, green up for a page-in)`);
  if (t.budgetChanges) parts.push(`the driver changed its budget ${t.budgetChanges} time${t.budgetChanges === 1 ? "" : "s"} (the dashed marks)`);
  return parts.length ? `Over these samples ${parts.join(", and ")}.` : "";
}

export function memoryVerdict(t: MemoryTimeline): string {
  const over = t.frames > 0 ? ` over ${t.frames} frames` : "";
  const held = bytes(t.points[t.points.length - 1].allocated);
  switch (t.trend) {
    case "growing":
      return `Memory grew from ${bytes(t.minBytes)} to ${held}${over}, about ${bytes(t.bytesPerFrame)} a frame, `
        + "without giving it back. That is what a leak looks like: something is allocated each frame and never freed. "
        + "The Inspect tab's allocations, sorted by size, are where it will be.";
    case "sawtooth":
      return `Memory moved between ${bytes(t.minBytes)} and ${bytes(t.maxBytes)}${over}, ending at ${held}. `
        + "It falls as well as rises, so this is a pool being emptied and refilled rather than a leak — though "
        + "the peak is what has to fit, not the average.";
    case "shrinking":
      return `Memory fell from ${bytes(t.maxBytes)} to ${held}${over}, so the application is releasing what it held.`;
    default:
      return `Memory held steady near ${held}${over}, within ${bytes(t.swingBytes)}. Nothing is accumulating.`;
  }
}
