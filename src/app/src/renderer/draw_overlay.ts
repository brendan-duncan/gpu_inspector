// Draw-call overlays: where one draw of a Vulkan capture landed, drawn over its pass's render target
// (capture_texture_view.ts). RenderDoc's highlight drawcall, depth test and wireframe overlays
// (vk_overlay.cpp), measured the same way: `vkinsp_replay --overlay <command>` draws the pass again up
// to the draw, the draw with a constant fragment shader (src/replay/src/overlay.cpp). What it cannot see
// is a fragment the draw's own shader discards: the constant shader does not discard, so alpha-tested
// geometry covers its whole quad.

/** Mask bits per pixel (OverlayResult in src/replay/src/replayer.h). */
export const OVERLAY_COVERED = 1;
export const OVERLAY_PASSED = 2;
export const OVERLAY_WIREFRAME = 4;
/** The fragment passed the stencil test on its own (the depth test is OVERLAY_PASSED's business). */
export const OVERLAY_STENCIL_PASSED = 8;
/**
 * The draw's culling left nothing here: a back-facing fragment landed on the pixel and no
 * front-facing one did. Every pixel of a closed mesh has a back face behind it, so the bit is only
 * set where the culling actually removed what would have been drawn.
 */
export const OVERLAY_BACK_FACING = 16;

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
  /** The stencil test alone was replayed, so OVERLAY_STENCIL_PASSED means something. */
  stencilTested: boolean;
  /** The draw was re-issued with its culling off, so OVERLAY_BACK_FACING means something. */
  backFaceTested: boolean;
  /** Pixels the stencil test alone rejected, and pixels the draw's culling removed. */
  pixelsStencilRejected: number;
  pixelsBackFacing: number;
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
export type DrawOverlayKind = "highlight" | "depth" | "stencil" | "backface" | "wireframe";

export const DRAW_OVERLAY_MAGIC = "OVERLAY 1\n";

/** Parses `vkinsp_replay --overlay-data` (WriteOverlayData in src/replay/src/main.cpp). */
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
  // RenderDoc's highlight: the draw in a flat color, everything else darkened.
  highlight: { covered: [255, 40, 200, 255], passed: [255, 40, 200, 255], wire: null, outside: [0, 0, 0, 170] },
  depth: { covered: [230, 40, 40, 255], passed: [40, 210, 70, 255], wire: null, outside: [0, 0, 0, 120] },
  // The stencil test on its own, so a draw the stencil rejected is not confused with one the
  // depth did; and the faces the draw's own culling removed, which is the answer to "the geometry
  // is there and nothing is drawn".
  stencil: { covered: [230, 40, 40, 255], passed: [40, 210, 70, 255], wire: null, outside: [0, 0, 0, 120] },
  // What the draw drew, and what its culling took away, the way RenderDoc's backface overlay
  // reads: green is geometry that survived, red is a pixel where only back faces of it landed.
  backface: { covered: [230, 40, 40, 255], passed: [40, 210, 70, 255], wire: null, outside: [0, 0, 0, 120] },
  wireframe: { covered: [0, 0, 0, 0], passed: [0, 0, 0, 0], wire: [255, 230, 40, 255], outside: [0, 0, 0, 0] },
};

export const DRAW_OVERLAY_LEGEND: Record<DrawOverlayKind, { label: string; color: [number, number, number] }[]> = {
  highlight: [{ label: "the draw", color: [255, 40, 200] }],
  depth: [{ label: "passed depth and stencil", color: [40, 210, 70] }, { label: "rejected", color: [230, 40, 40] }],
  stencil: [{ label: "passed the stencil test", color: [40, 210, 70] }, { label: "rejected by it", color: [230, 40, 40] }],
  backface: [{ label: "drawn", color: [40, 210, 70] }, { label: "culled away: only back faces here", color: [230, 40, 40] }],
  wireframe: [{ label: "edges", color: [255, 230, 40] }],
};

/** The overlay's colors, the render target's size, or null when the replay drew nothing. */
export function drawOverlayRgba(o: DrawOverlay, kind: DrawOverlayKind): Uint8ClampedArray | null {
  if (!o.mask || o.mask.length < o.width * o.height) return null;
  const paint = PAINT[kind];
  const rgba = new Uint8ClampedArray(o.width * o.height * 4);
  for (let i = 0, n = o.width * o.height; i < n; i++) {
    const m = o.mask[i];
    // Each overlay reads its own bit: the back-facing one marks pixels the draw's culling kept out
    // of the rasterized mask altogether, so it is the only one not gated on OVERLAY_COVERED.
    const c = kind === "wireframe" ? (m & OVERLAY_WIREFRAME ? paint.wire! : paint.outside)
      : kind === "backface" ? (m & OVERLAY_BACK_FACING ? paint.covered : m & OVERLAY_COVERED ? paint.passed : paint.outside)
      : kind === "stencil" ? (m & OVERLAY_COVERED ? (m & OVERLAY_STENCIL_PASSED ? paint.passed : paint.covered) : paint.outside)
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
  const back = m & OVERLAY_BACK_FACING ? " (its culling removed what was here)" : "";
  if (!(m & OVERLAY_COVERED)) {
    if (m & OVERLAY_BACK_FACING) return [`${name}: culled away here — only back faces of it reach this pixel`];
    return [`${name}: not here${m & OVERLAY_WIREFRAME ? " (an edge passes)" : ""}`];
  }
  if (!o.depthTested) return [`${name}: rasterized here${back}`];
  const tests = m & OVERLAY_PASSED ? "passed depth and stencil here" : "rasterized here, rejected by depth or stencil";
  const stencil = o.stencilTested ? `; the stencil test alone ${m & OVERLAY_STENCIL_PASSED ? "passed" : "rejected it"}` : "";
  return [`${name}: ${tests}${stencil}${back}`];
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
