// Draw-call overlays (src/renderer/draw_overlay.ts): what `vkinsp_replay --overlay-data` writes, and
// the colours and tooltip lines the render target tab draws from it.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "drawoverlay-"));
const out = join(dir, "draw_overlay.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "draw_overlay.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { parseDrawOverlayFile, drawOverlayRgba, drawOverlayLines, drawOverlaySummary, OVERLAY_COVERED, OVERLAY_PASSED, OVERLAY_WIREFRAME } =
  await import(pathToFileURL(out).href);

/** A file laid out the way WriteOverlayData in replay/src/main.cpp lays it out. */
function overlayFile(draws, masks) {
  let offset = 0;
  const manifest = {
    format: "gpu-inspector-draw-overlay", version: 1, device: "Test GPU", problems: [],
    draws: draws.map((d, i) => {
      const mask = masks[i];
      const entry = { ...d, ...(mask ? { payload: [offset, mask.length] } : {}) };
      offset += mask ? mask.length : 0;
      return entry;
    }),
  };
  const json = new TextEncoder().encode(JSON.stringify(manifest));
  const magic = new TextEncoder().encode("OVERLAY 1\n");
  const bytes = new Uint8Array(magic.length + 4 + json.length + offset);
  bytes.set(magic, 0);
  new DataView(bytes.buffer).setUint32(magic.length, json.length, true);
  bytes.set(json, magic.length + 4);
  let at = magic.length + 4 + json.length;
  for (const m of masks) if (m) { bytes.set(m, at); at += m.length; }
  return bytes;
}

// A 3x2 target: the draw rasterized the top row, passed its tests on two of those pixels, and an
// edge of its wireframe runs down the first column.
const C = OVERLAY_COVERED, P = OVERLAY_PASSED, W = OVERLAY_WIREFRAME;
const mask = Uint8Array.from([C | P | W, C, C | P, W, 0, 0]);
const draw = {
  command: 17, method: "vkCmdDrawIndexed", frame: 0, commandBuffer: 7, passIndex: 0, measured: true, width: 3, height: 2,
  fragments: 4, pixelsCovered: 3, pixelsPassed: 2, pixelsRejected: 1, depthTested: true, wireframe: true,
};
const skipped = { command: 3, method: "vkQueueSubmit", frame: 0, commandBuffer: 0, passIndex: 0, measured: false, width: 0, height: 0,
  fragments: 0, pixelsCovered: 0, pixelsPassed: 0, pixelsRejected: 0, depthTested: false, wireframe: false,
  note: "command 3 is not in a render pass the replay drew" };

test("the replay's overlays are read back with their masks", () => {
  const file = parseDrawOverlayFile(overlayFile([draw, skipped], [mask, null]));
  assert.equal(file.device, "Test GPU");
  assert.equal(file.draws.length, 2);
  assert.deepEqual([...file.draws[0].mask], [...mask]);
  assert.equal(file.draws[1].mask, null);
  assert.match(drawOverlaySummary(file.draws[1]), /^Not drawn: command 3 is not in a render pass/);
  assert.match(drawOverlaySummary(file.draws[0]), /^3 pixels \(50\.0%\), 4 fragments, 2 passed depth and stencil, 1 rejected$/);
  assert.throws(() => parseDrawOverlayFile(new TextEncoder().encode("OVERDRAW 1\n\0\0\0\0")), /Not a draw overlay file/);
});

test("each overlay paints the pixels its bits say", () => {
  const o = parseDrawOverlayFile(overlayFile([draw], [mask])).draws[0];
  const px = (rgba, i) => [...rgba.subarray(i * 4, i * 4 + 4)];

  const depth = drawOverlayRgba(o, "depth");
  assert.equal(depth.length, 3 * 2 * 4);
  assert.deepEqual(px(depth, 0), px(depth, 2), "both passing pixels share a colour");
  assert.notDeepEqual(px(depth, 0), px(depth, 1), "a rejected pixel is painted apart from a passing one");
  assert.ok(px(depth, 1)[0] > px(depth, 1)[1], "rejected is red");
  assert.ok(px(depth, 0)[1] > px(depth, 0)[0], "passed is green");
  assert.deepEqual(px(depth, 4), [0, 0, 0, px(depth, 4)[3]], "outside the draw is darkened, not coloured");

  const highlight = drawOverlayRgba(o, "highlight");
  assert.deepEqual(px(highlight, 0), px(highlight, 1), "the highlight does not care about the tests");

  const wire = drawOverlayRgba(o, "wireframe");
  assert.ok(px(wire, 0)[3] > 0 && px(wire, 3)[3] > 0, "the edge pixels are painted");
  assert.equal(px(wire, 1)[3], 0, "a covered pixel off the edges is left alone");
});

test("the tooltip says what the draw did at the pixel", () => {
  const o = parseDrawOverlayFile(overlayFile([draw], [mask])).draws[0];
  assert.deepEqual(drawOverlayLines(o, 0, 0), ["Draw #17: passed depth and stencil here"]);
  assert.deepEqual(drawOverlayLines(o, 1, 0), ["Draw #17: rasterized here, rejected by depth or stencil"]);
  assert.deepEqual(drawOverlayLines(o, 0, 1), ["Draw #17: not here (an edge passes)"]);
  assert.deepEqual(drawOverlayLines(o, 9, 9), []);
  assert.deepEqual(drawOverlayLines({ ...o, depthTested: false }, 1, 0), ["Draw #17: rasterized here"]);
});
