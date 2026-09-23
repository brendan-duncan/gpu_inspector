// The viewport and scissor overlay: the rectangles a draw's own state carries, drawn over the render
// target it drew into (capture_texture_view.ts). RenderDoc has the same overlay, and it is the one
// draw overlay that needs no replay and no measurement at all -- the rectangles are in the capture,
// in the draw's state, so it works on a saved capture of any API.
//
// What it answers: why a draw that runs, binds the right things and rasterizes has no pixels on the
// screen. A scissor smaller than the target, a viewport left at the size of a previous frame's
// window, and a flipped viewport all look the same in a capture's numbers and are obvious here.
import { drawScissors, drawViewports, type DrawRect, type DrawState } from "./draw_state.js";

/** What the overlay paints, and what the legend calls each of them. */
const VIEWPORT_COLOR: [number, number, number] = [80, 190, 255];
const SCISSOR_COLOR: [number, number, number] = [255, 210, 60];
/** Everything the scissor cuts away, darkened the way the highlight overlay darkens what it is not. */
const CUT_COLOR: [number, number, number, number] = [0, 0, 0, 190];

export const VIEWPORT_OVERLAY_LEGEND: { label: string; color: [number, number, number] }[] = [
  { label: "viewport", color: VIEWPORT_COLOR },
  { label: "scissor", color: SCISSOR_COLOR },
  { label: "cut away by the scissor", color: [40, 40, 40] },
];

export interface ViewportOverlay {
  viewports: DrawRect[];
  scissors: DrawRect[];
  /** Pixels of the target the scissors keep, and the ones they cut away. */
  keptPixels: number;
  cutPixels: number;
}

/** The rectangles of the draw's state, with what they do to a target of this size. */
export function viewportOverlayOf(state: DrawState, width: number, height: number): ViewportOverlay {
  const viewports = drawViewports(state);
  const scissors = drawScissors(state);
  let kept = 0;
  if (scissors.length) {
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        if (inAny(scissors, x, y)) kept++;
      }
    }
  } else {
    kept = width * height;   // no scissor: nothing is cut
  }
  return { viewports, scissors, keptPixels: kept, cutPixels: width * height - kept };
}

/** One line per rectangle, for the row under the toolbar. */
export function viewportOverlaySummary(o: ViewportOverlay, width: number, height: number): string {
  const parts: string[] = [];
  parts.push(o.viewports.length ? o.viewports.map(describe).join(", ") : "no viewport in the draw's state");
  if (!o.scissors.length) parts.push("no scissor");
  else if (!o.cutPixels) parts.push(`scissor ${o.scissors.map(describe).join(", ")} (cuts nothing of this ${width}x${height} target)`);
  else parts.push(`scissor ${o.scissors.map(describe).join(", ")}, cutting ${percent(o.cutPixels, width * height)} of the target away`);
  return parts.join(" · ");
}

function describe(r: DrawRect): string {
  return `${round(r.x)},${round(r.y)} ${round(r.width)}x${round(r.height)}${r.flippedY ? " (flipped Y)" : ""}`;
}

function round(v: number): number {
  return Math.round(v * 100) / 100;
}

function percent(part: number, whole: number): string {
  if (!whole) return "0%";
  const p = (part / whole) * 100;
  return `${p >= 10 ? Math.round(p) : Math.round(p * 10) / 10}%`;
}

function inAny(rects: DrawRect[], x: number, y: number): boolean {
  return rects.some((r) => x >= Math.floor(r.x) && y >= Math.floor(r.y) && x < Math.ceil(r.x + r.width) && y < Math.ceil(r.y + r.height));
}

/** The overlay's pixels: what the scissor cuts darkened, and each rectangle's edge on top. */
export function viewportOverlayRgba(o: ViewportOverlay, width: number, height: number): Uint8ClampedArray | null {
  if (!o.viewports.length && !o.scissors.length) return null;
  const rgba = new Uint8ClampedArray(width * height * 4);
  if (o.scissors.length) {
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        if (inAny(o.scissors, x, y)) continue;
        const i = (y * width + x) * 4;
        rgba[i] = CUT_COLOR[0];
        rgba[i + 1] = CUT_COLOR[1];
        rgba[i + 2] = CUT_COLOR[2];
        rgba[i + 3] = CUT_COLOR[3];
      }
    }
  }
  // The edges last, so a rectangle's outline is visible over what it cut.
  for (const r of o.scissors) outline(rgba, width, height, r, SCISSOR_COLOR);
  for (const r of o.viewports) outline(rgba, width, height, r, VIEWPORT_COLOR);
  return rgba;
}

/** A one-pixel outline of the rectangle, clipped to the target and drawn inside its edge. */
function outline(rgba: Uint8ClampedArray, width: number, height: number, r: DrawRect, color: [number, number, number]): void {
  const left = Math.max(0, Math.round(r.x));
  const top = Math.max(0, Math.round(r.y));
  const right = Math.min(width - 1, Math.round(r.x + r.width) - 1);
  const bottom = Math.min(height - 1, Math.round(r.y + r.height) - 1);
  if (right < left || bottom < top) return;
  const put = (x: number, y: number): void => {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const i = (y * width + x) * 4;
    rgba[i] = color[0];
    rgba[i + 1] = color[1];
    rgba[i + 2] = color[2];
    rgba[i + 3] = 255;
  };
  for (let x = left; x <= right; x++) {
    put(x, top);
    put(x, bottom);
  }
  for (let y = top; y <= bottom; y++) {
    put(left, y);
    put(right, y);
  }
}

/** What the overlay says about the pixel under the pointer. */
export function viewportOverlayLines(o: ViewportOverlay, x: number, y: number): string[] {
  const lines: string[] = [];
  if (o.viewports.length) lines.push(inAny(o.viewports, x, y) ? "Inside the viewport" : "Outside the viewport: nothing the draw rasterizes lands here");
  if (o.scissors.length) lines.push(inAny(o.scissors, x, y) ? "Kept by the scissor" : "Cut away by the scissor");
  return lines;
}
