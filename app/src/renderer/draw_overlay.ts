// Draw-call overlays: where one draw of a Vulkan capture landed, drawn over its pass's render target
// (capture_texture_view.ts). RenderDoc's highlight drawcall, depth test and wireframe overlays
// (vk_overlay.cpp), measured the same way: `vkinsp_replay --overlay <command>` draws the pass again up
// to the draw, the draw with a constant fragment shader (replay/src/overlay.cpp). What it cannot see
// is a fragment the draw's own shader discards: the constant shader does not discard, so alpha-tested
// geometry covers its whole quad.

/** Mask bits per pixel (OverlayResult in replay/src/replayer.h). */
export const OVERLAY_COVERED = 1;
export const OVERLAY_PASSED = 2;
export const OVERLAY_WIREFRAME = 4;

/** One draw's overlay as the replay wrote it. */
export interface DrawOverlay {
  command: number;
  method: string;
  frame: number;
  commandBuffer: number;
  passIndex: number;
  /** The replay drew it; false with a note saying why not. */
  measured: boolean;
  width: number;
  height: number;
  /** Fragments the draw rasterized, its own overdraw included. */
  fragments: number;
  pixelsCovered: number;
  pixelsPassed: number;
  pixelsRejected: number;
  /** The pass has depth or stencil to test against, and the tests were replayed. */
  depthTested: boolean;
  /** The wireframe bit was drawn. */
  wireframe: boolean;
  note?: string;
  /** One byte per pixel, row by row: OVERLAY_* bits. */
  mask: Uint8Array | null;
}

export interface DrawOverlayFile {
  device: string;
  draws: DrawOverlay[];
  problems: string[];
}

/** What the overlay draws over the render target. */
export type DrawOverlayKind = "highlight" | "depth" | "wireframe";

export const DRAW_OVERLAY_MAGIC = "OVERLAY 1\n";

/** Parses `vkinsp_replay --overlay-data` (WriteOverlayData in replay/src/main.cpp). */
export function parseDrawOverlayFile(bytes: Uint8Array): DrawOverlayFile {
  const magic = new TextEncoder().encode(DRAW_OVERLAY_MAGIC);
  if (bytes.byteLength < magic.byteLength + 4 || magic.some((b, i) => bytes[i] !== b)) throw new Error("Not a draw overlay file from vkinsp_replay.");
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(magic.byteLength, true);
  const start = magic.byteLength + 4;
  const base = start + length;
  if (base > bytes.byteLength) throw new Error("The draw overlay file is truncated.");
  const manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(start, base))) as {
    device?: string;
    draws?: (Omit<DrawOverlay, "mask"> & { payload?: [number, number] })[];
    problems?: string[];
  };
  const draws = (manifest.draws ?? []).map(({ payload, ...info }): DrawOverlay => {
    let mask: Uint8Array | null = null;
    if (payload) {
      const [offset, size] = payload;
      if (base + offset + size > bytes.byteLength) throw new Error("The draw overlay file is truncated (mask out of range).");
      mask = bytes.slice(base + offset, base + offset + size);
    }
    return { ...info, mask };
  });
  return { device: manifest.device ?? "", draws, problems: manifest.problems ?? [] };
}

type Rgba = [number, number, number, number];

/** What each overlay paints: the draw's pixels by what happened to them, and what it leaves around them. */
const PAINT: Record<DrawOverlayKind, { covered: Rgba; passed: Rgba; wire: Rgba | null; outside: Rgba }> = {
  // RenderDoc's highlight: the draw in a flat colour, everything else darkened.
  highlight: { covered: [255, 40, 200, 255], passed: [255, 40, 200, 255], wire: null, outside: [0, 0, 0, 170] },
  depth: { covered: [230, 40, 40, 255], passed: [40, 210, 70, 255], wire: null, outside: [0, 0, 0, 120] },
  wireframe: { covered: [0, 0, 0, 0], passed: [0, 0, 0, 0], wire: [255, 230, 40, 255], outside: [0, 0, 0, 0] },
};

export const DRAW_OVERLAY_LEGEND: Record<DrawOverlayKind, { label: string; color: [number, number, number] }[]> = {
  highlight: [{ label: "the draw", color: [255, 40, 200] }],
  depth: [{ label: "passed depth and stencil", color: [40, 210, 70] }, { label: "rejected", color: [230, 40, 40] }],
  wireframe: [{ label: "edges", color: [255, 230, 40] }],
};

/** The overlay's colours, the render target's size, or null when the replay drew nothing. */
export function drawOverlayRgba(o: DrawOverlay, kind: DrawOverlayKind): Uint8ClampedArray | null {
  if (!o.mask || o.mask.length < o.width * o.height) return null;
  const paint = PAINT[kind];
  const rgba = new Uint8ClampedArray(o.width * o.height * 4);
  for (let i = 0, n = o.width * o.height; i < n; i++) {
    const m = o.mask[i];
    const c = kind === "wireframe"
      ? (m & OVERLAY_WIREFRAME ? paint.wire! : paint.outside)
      : m & OVERLAY_COVERED ? (m & OVERLAY_PASSED ? paint.passed : paint.covered) : paint.outside;
    rgba.set(c, i * 4);
  }
  return rgba;
}

/** What the draw did at a pixel, for the tooltip. */
export function drawOverlayLines(o: DrawOverlay, x: number, y: number): string[] {
  if (!o.mask || x < 0 || y < 0 || x >= o.width || y >= o.height) return [];
  const m = o.mask[y * o.width + x];
  const name = `Draw #${o.command}`;
  if (!(m & OVERLAY_COVERED)) return [`${name}: not here${m & OVERLAY_WIREFRAME ? " (an edge passes)" : ""}`];
  if (!o.depthTested) return [`${name}: rasterized here`];
  return [`${name}: ${m & OVERLAY_PASSED ? "passed depth and stencil here" : "rasterized here, rejected by depth or stencil"}`];
}

/** One line: how much of the target the draw covers and what its tests did. */
export function drawOverlaySummary(o: DrawOverlay): string {
  if (!o.measured) return `Not drawn: ${o.note ?? "the replay could not draw it"}`;
  const pixels = o.width * o.height;
  const share = pixels ? ` (${((o.pixelsCovered / pixels) * 100).toFixed(o.pixelsCovered / pixels < 0.01 ? 2 : 1)}%)` : "";
  const tests = o.depthTested
    ? `, ${o.pixelsPassed.toLocaleString()} passed depth and stencil, ${o.pixelsRejected.toLocaleString()} rejected`
    : "";
  return `${o.pixelsCovered.toLocaleString()} pixels${share}, ${o.fragments.toLocaleString()} fragments${tests}`;
}
