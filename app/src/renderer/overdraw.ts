// Overdraw measurements: how many fragments landed on each pixel of a render pass, when the pass
// was drawn again with a counting fragment shader. A Metal capture taken with "Overdraw" carries
// them (metal/src/overdraw.h); vkinsp_replay --overdraw measures the same for a Vulkan capture
// file (docs/REPLAY.md). Each pass has two: the fragments that passed its depth and stencil tests
// in draw order, and every fragment its draws rasterized.
import type { OverdrawMeasurement } from "../shared/protocol.js";
import type { CapturedOverdraw } from "./capture_data.js";

/** A render pass of a capture: which frame, which command buffer, and its index in that buffer. */
export interface OverdrawPassKey {
  frame: number;
  commandBuffer: number;
  passIndex: number;
}

export const samePass = (a: OverdrawPassKey, b: OverdrawPassKey): boolean =>
  a.frame === b.frame && a.commandBuffer === b.commandBuffer && a.passIndex === b.passIndex;

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

/** The ramp's steps as a legend: the counts each colour stands for. */
export const OVERDRAW_LEGEND: { label: string; color: [number, number, number] }[] = RAMP.map(([upTo, r, g, b], i) => {
  const from = i === 0 ? 0 : RAMP[i - 1][0] + 1;
  const label = i === RAMP.length - 1 ? `${from}+` : from === upTo ? String(upTo) : `${from}-${upTo}`;
  return { label, color: [r, g, b] };
});

export function heatColor(n: number): [number, number, number] {
  for (const [upTo, r, g, b] of RAMP) if (n <= upTo) return [r, g, b];
  return [255, 255, 255];
}

/**
 * The measurement as an RGBA heatmap, or null without per-pixel data. `transparentZero` leaves the
 * pixels nothing landed on fully transparent, for drawing the heat over the pass's render target.
 */
export function overdrawRgba(o: CapturedOverdraw, transparentZero = false): Uint8ClampedArray<ArrayBuffer> | null {
  const { width, height } = o.info;
  const pixels = width * height;
  if (!o.data || o.data.byteLength < pixels * 2) return null;
  const out = new Uint8ClampedArray(pixels * 4);
  for (let p = 0; p < pixels; p++) {
    const count = o.data[p * 2] | (o.data[p * 2 + 1] << 8);
    const [r, g, b] = heatColor(count);
    out[p * 4] = r;
    out[p * 4 + 1] = g;
    out[p * 4 + 2] = b;
    out[p * 4 + 3] = transparentZero && count === 0 ? 0 : 255;
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

/** What vkinsp_replay --overdraw-data wrote: the measurements of a Vulkan capture, replayed. */
export interface OverdrawFile {
  device: string;
  measurements: CapturedOverdraw[];
  /** What the replay could not rebuild (the first hundred). */
  problems: string[];
}

const OVERDRAW_MAGIC = "OVERDRAW 1\n";

/**
 * Parses vkinsp_replay's --overdraw-data file (replay/src/main.cpp): a magic line, a little-endian
 * u32 manifest length, the JSON manifest, then the counts it names as [offset, length].
 */
export function parseOverdrawFile(bytes: Uint8Array): OverdrawFile {
  const magic = new TextEncoder().encode(OVERDRAW_MAGIC);
  if (bytes.byteLength < magic.byteLength + 4 || magic.some((b, i) => bytes[i] !== b)) throw new Error("Not an overdraw file from vkinsp_replay.");
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(magic.byteLength, true);
  const start = magic.byteLength + 4;
  const base = start + length;
  if (base > bytes.byteLength) throw new Error("The overdraw file is truncated.");
  const manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(start, base))) as {
    device?: string;
    passes?: (OverdrawMeasurement & { payload?: [number, number] })[];
    problems?: string[];
  };
  const measurements = (manifest.passes ?? []).map(({ payload, ...info }) => {
    let data: Uint8Array | null = null;
    if (payload) {
      const [offset, size] = payload;
      if (base + offset + size > bytes.byteLength) throw new Error("The overdraw file is truncated (counts out of range).");
      // Copied: the counts outlive the file's buffer, and are read two bytes at a time.
      data = bytes.slice(base + offset, base + offset + size);
    }
    return { info, data };
  });
  return { device: manifest.device ?? "", measurements, problems: manifest.problems ?? [] };
}

/** The histogram as "1: 1200, 2: 340, ..." with the empty buckets left out. */
export function overdrawHistogramText(info: OverdrawMeasurement): string {
  return (info.histogram ?? []).map((n, i) => (n ? `${OVERDRAW_BUCKETS[i]}: ${n.toLocaleString()}` : "")).filter(Boolean).join(", ");
}
