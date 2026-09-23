// The Viewport / Scissor overlay (src/renderer/viewport_overlay.ts): the rectangles a draw's own
// state carries, read from the four spellings the APIs record them in, and what the overlay paints
// from them. Nothing here is measured or replayed, which is the point of this overlay.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "viewportoverlay-"));
const out = join(dir, "viewport_overlay.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "viewport_overlay.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { viewportOverlayOf, viewportOverlayRgba, viewportOverlayLines, viewportOverlaySummary } = await import(pathToFileURL(out).href);

/** Only the two fields the overlay reads of a draw's state. */
function state(viewports, scissors) {
  return { viewports, scissors };
}

test("a Vulkan draw's viewport and scissor", () => {
  const o = viewportOverlayOf(state(
    [{ x: 0, y: 0, width: 640, height: 480, minDepth: 0, maxDepth: 1 }],
    [{ offset: { x: 0, y: 0 }, extent: { width: 320, height: 480 } }]), 640, 480);
  assert.deepEqual(o.viewports, [{ x: 0, y: 0, width: 640, height: 480 }]);
  assert.deepEqual(o.scissors, [{ x: 0, y: 0, width: 320, height: 480 }]);
  assert.equal(o.keptPixels, 320 * 480);
  assert.equal(o.cutPixels, 320 * 480);
  assert.match(viewportOverlaySummary(o, 640, 480), /cutting 50% of the target away/);
});

test("a flipped Vulkan viewport keeps the rectangle it covers and says it is flipped", () => {
  // The VK_KHR_maintenance1 convention: y at the bottom and a negative height.
  const o = viewportOverlayOf(state([{ x: 0, y: 480, width: 640, height: -480 }], null), 640, 480);
  assert.deepEqual(o.viewports, [{ x: 0, y: 0, width: 640, height: 480, flippedY: true }]);
  assert.match(viewportOverlaySummary(o, 640, 480), /flipped Y/);
  // No scissor at all: nothing is cut, and the summary says so rather than reporting 0%.
  assert.equal(o.cutPixels, 0);
  assert.match(viewportOverlaySummary(o, 640, 480), /no scissor/);
});

test("a Direct3D 12 viewport and scissor rect", () => {
  const o = viewportOverlayOf(state(
    [{ TopLeftX: 16, TopLeftY: 8, Width: 200, Height: 100, MinDepth: 0, MaxDepth: 1 }],
    [{ left: 16, top: 8, right: 116, bottom: 58 }]), 256, 128);
  assert.deepEqual(o.viewports, [{ x: 16, y: 8, width: 200, height: 100 }]);
  assert.deepEqual(o.scissors, [{ x: 16, y: 8, width: 100, height: 50 }]);
  assert.equal(o.keptPixels, 100 * 50);
});

test("a Metal viewport and scissor rect", () => {
  // setViewport: and setScissorRect: record the struct itself rather than an array of them.
  const o = viewportOverlayOf(state(
    { originX: 0, originY: 0, width: 128, height: 64, znear: 0, zfar: 1 },
    { x: 32, y: 0, width: 32, height: 64 }), 128, 64);
  assert.deepEqual(o.viewports, [{ x: 0, y: 0, width: 128, height: 64 }]);
  assert.deepEqual(o.scissors, [{ x: 32, y: 0, width: 32, height: 64 }]);
  assert.equal(o.cutPixels, 128 * 64 - 32 * 64);
});

test("an empty or missing rectangle is left out rather than drawn", () => {
  const o = viewportOverlayOf(state([{ x: 0, y: 0, width: 0, height: 0 }], []), 8, 8);
  assert.deepEqual(o.viewports, []);
  assert.deepEqual(o.scissors, []);
  assert.equal(viewportOverlayRgba(o, 8, 8), null);
  assert.match(viewportOverlaySummary(o, 8, 8), /no viewport/);
});

test("the overlay darkens what the scissor cuts and outlines both rectangles", () => {
  const o = viewportOverlayOf(state(
    [{ x: 0, y: 0, width: 8, height: 8 }],
    [{ offset: { x: 0, y: 0 }, extent: { width: 4, height: 8 } }]), 8, 8);
  const rgba = viewportOverlayRgba(o, 8, 8);
  const at = (x, y) => [...rgba.slice((y * 8 + x) * 4, (y * 8 + x) * 4 + 4)];
  // Inside the scissor and away from any edge: the overlay paints nothing, so the target shows.
  assert.deepEqual(at(2, 4), [0, 0, 0, 0]);
  // Cut away: darkened.
  assert.equal(at(6, 4)[3], 190);
  // The scissor's own edge, and the viewport's, each in their color.
  assert.deepEqual(at(3, 4), [255, 210, 60, 255]);
  assert.deepEqual(at(7, 4), [80, 190, 255, 255]);
});

test("the tooltip says what each rectangle does to the pixel", () => {
  const o = viewportOverlayOf(state(
    [{ x: 0, y: 0, width: 8, height: 8 }],
    [{ offset: { x: 0, y: 0 }, extent: { width: 4, height: 8 } }]), 8, 8);
  assert.deepEqual(viewportOverlayLines(o, 1, 1), ["Inside the viewport", "Kept by the scissor"]);
  assert.deepEqual(viewportOverlayLines(o, 6, 1), ["Inside the viewport", "Cut away by the scissor"]);
});
