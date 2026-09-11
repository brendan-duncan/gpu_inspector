// Overdraw measurements: how many fragments landed on each pixel of a render pass, when the pass
// was drawn again with a counting fragment shader. A Metal capture taken with "Overdraw" carries
// them (metal/src/overdraw.h); vkinsp_replay --overdraw measures the same for a Vulkan capture
// file (docs/REPLAY.md). Each pass has two: the fragments that passed its depth and stencil tests
// in draw order, and every fragment its draws rasterized.
import type { OverdrawMeasurement } from "../shared/protocol.js";
import type { CapturedOverdraw } from "./capture_data.js";

/** The histogram's buckets, as the measurement counts them. */
export const OVERDRAW_BUCKETS = ["1", "2", "3", "4", "5-8", "9-16", "17-32", "33+"];

/** The count of one pixel; 0 outside the pass or without per-pixel data. */
export function overdrawCount(o: CapturedOverdraw, x: number, y: number): number {
  const { width, height } = o.info;
  if (!o.data || x < 0 || y < 0 || x >= width || y >= height) return 0;
  const i = (y * width + x) * 2;
  return i + 1 < o.data.byteLength ? o.data[i] | (o.data[i + 1] << 8) : 0;
}

/**
 * The heat colour of a count: black for none, then blue, cyan, green, yellow, orange, red, magenta,
 * and white from 33. The same ramp as vkinsp_replay's heatmaps (replay/src/main.cpp).
 */
const RAMP: [number, number, number, number][] = [
  [0, 0, 0, 0], [1, 20, 40, 150], [2, 0, 120, 230], [3, 0, 190, 170], [4, 110, 210, 40],
  [6, 240, 210, 0], [10, 250, 120, 0], [16, 220, 20, 20], [32, 240, 0, 200], [65535, 255, 255, 255],
];

export function heatColor(n: number): [number, number, number] {
  for (const [upTo, r, g, b] of RAMP) if (n <= upTo) return [r, g, b];
  return [255, 255, 255];
}

/** The measurement as an RGBA heatmap, or null without per-pixel data. */
export function overdrawRgba(o: CapturedOverdraw): Uint8ClampedArray<ArrayBuffer> | null {
  const { width, height } = o.info;
  const pixels = width * height;
  if (!o.data || o.data.byteLength < pixels * 2) return null;
  const out = new Uint8ClampedArray(pixels * 4);
  for (let p = 0; p < pixels; p++) {
    const [r, g, b] = heatColor(o.data[p * 2] | (o.data[p * 2 + 1] << 8));
    out[p * 4] = r;
    out[p * 4 + 1] = g;
    out[p * 4 + 2] = b;
    out[p * 4 + 3] = 255;
  }
  return out;
}

/** Fragments per pixel of the pass, and per pixel something landed on. */
export function overdrawAverages(info: OverdrawMeasurement): { perPixel: number; perCovered: number } {
  const pixels = info.width * info.height;
  return {
    perPixel: pixels > 0 ? info.fragments / pixels : 0,
    perCovered: info.coveredPixels > 0 ? info.fragments / info.coveredPixels : 0,
  };
}

/** Whether the measurement has numbers (a pass that could not be measured says why in `note`). */
export function isMeasured(info: OverdrawMeasurement): boolean {
  return info.measured !== false;
}

/** One line: what was counted, per pixel and per covered pixel, the maximum and the draws. */
export function overdrawSummary(info: OverdrawMeasurement): string {
  const kind = info.depthTested ? "passing depth and stencil" : "rasterized";
  if (!isMeasured(info)) return `Fragments ${kind}: not measured${info.note ? ` (${info.note})` : ""}`;
  const a = overdrawAverages(info);
  const skipped = info.skippedDraws ? `, ${info.skippedDraws} not counted` : "";
  return `Fragments ${kind}: ${a.perPixel.toFixed(2)} per pixel, ${a.perCovered.toFixed(2)} per covered pixel, `
    + `max ${info.maxCount} (${info.fragments.toLocaleString()} over ${info.coveredPixels.toLocaleString()} pixels, ${info.draws} draws${skipped})`;
}

/** The histogram as "1: 1200, 2: 340, ..." with the empty buckets left out. */
export function overdrawHistogramText(info: OverdrawMeasurement): string {
  return (info.histogram ?? []).map((n, i) => (n ? `${OVERDRAW_BUCKETS[i]}: ${n.toLocaleString()}` : "")).filter(Boolean).join(", ");
}
