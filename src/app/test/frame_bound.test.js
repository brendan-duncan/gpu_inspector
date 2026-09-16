// The Frame Bound verdict (src/renderer/capture_statistics.ts): what a frame waits on, and the
// case where it cannot be said at all.
//
// The GPU number is the span of the *captured* passes and the budget is the frame interval the
// application reaches *without* a capture. Capturing adds a timestamp, statistics and occlusion
// query around every pass and reads every render target back, so the captured frame is the more
// expensive one — on a real Unity player, twelve times more. Compared against each other those two
// numbers named the wrong bottleneck, and the tell is that a frame cannot be shorter than the GPU
// work it waits for.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "framebound-"));
const out = join(dir, "capture_statistics.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "capture_statistics.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { frameBound, CAPTURE_DISTORTION } = await import(pathToFileURL(out).href);

/** Defaults that make a plain, undistorted, vsync-off capture. */
const t = (over = {}) => frameBound({ frameMs: 16, refreshMs: 0, submitMs: 1, gpuSpanMs: 8, frames: 1, ...over });

test("no budget to compare against gives no verdict", () => {
  assert.equal(frameBound({ frameMs: 0, refreshMs: 0, submitMs: 0, gpuSpanMs: 0, frames: 1 }), null);
});

test("GPU time filling the frame is GPU bound", () => {
  const b = t({ gpuSpanMs: 15 });
  assert.equal(b.kind, "gpu");
  assert.equal(b.distorted, false);
  assert.match(b.verdict, /GPU bound/);
});

test("submit time filling the frame is CPU bound", () => {
  const b = t({ gpuSpanMs: 1, submitMs: 15 });
  assert.equal(b.kind, "cpu");
  assert.match(b.verdict, /CPU bound/);
});

test("both well under the frame leaves the GPU with headroom", () => {
  const b = t({ gpuSpanMs: 1, submitMs: 1 });
  assert.equal(b.kind, "idle");
  assert.equal(b.distorted, false);
});

test("a vsynced frame that meets the refresh is vsync bound", () => {
  const b = frameBound({ frameMs: 16.6, refreshMs: 16.6, submitMs: 1, gpuSpanMs: 4, frames: 1 });
  assert.equal(b.kind, "idle");
  assert.match(b.verdict, /Vsync bound/);
});

test("passes longer than the whole frame cannot both be true, so no bottleneck is named", () => {
  // The real case: a Unity player at 1475 fps (0.68 ms a frame) whose captured passes span 9.2 ms.
  const b = frameBound({ frameMs: 0.678, refreshMs: 0, submitMs: 0.022, gpuSpanMs: 9.204, frames: 1 });
  assert.equal(b.distorted, true);
  assert.equal(b.kind, "distorted");
  assert.doesNotMatch(b.verdict, /GPU bound/, "the old verdict, from numbers that cannot both hold");
  assert.match(b.verdict, /capture's own work/);
  assert.match(b.verdict, /cannot be shorter than the GPU/);
});

test("the distortion check comes before every other verdict", () => {
  // Submit time would otherwise read as CPU bound; the numbers still cannot be compared.
  const b = frameBound({ frameMs: 1, refreshMs: 0, submitMs: 0.95, gpuSpanMs: 20, frames: 1 });
  assert.equal(b.kind, "distorted");
});

test("a little over the frame is noise, not distortion", () => {
  // Two clocks and a frame boundary that does not line up exactly: a few percent means nothing.
  const b = frameBound({ frameMs: 16, refreshMs: 0, submitMs: 1, gpuSpanMs: 16 * 1.1, frames: 1 });
  assert.ok(1.1 < CAPTURE_DISTORTION);
  assert.equal(b.distorted, false);
  assert.equal(b.kind, "gpu", "still the GPU filling the frame");
});

test("a multi-frame capture is judged per frame, so it is not called distorted for being long", () => {
  // Four frames of 8 ms of passes against a 16 ms budget is 8 ms a frame, not 32.
  const b = frameBound({ frameMs: 16, refreshMs: 0, submitMs: 1, gpuSpanMs: 32, frames: 4 });
  assert.equal(b.distorted, false);
  assert.equal(b.gpuMs, 8);
});

test("a vsynced capture is measured against the refresh period, and can be distorted too", () => {
  // The budget is the display period; passes far beyond it are still the capture's own cost.
  const b = frameBound({ frameMs: 16.6, refreshMs: 16.6, submitMs: 1, gpuSpanMs: 200, frames: 1 });
  assert.equal(b.distorted, true);
  assert.equal(b.budgetMs, 16.6);
});
