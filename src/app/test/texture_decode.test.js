// The compressed-texture decoders against reference vectors (tools/texture_vectors.py):
// every texel of every vector within the vector's tolerance, in 8-bit units.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const vectors = JSON.parse(readFileSync(join(here, "vectors", "texture_decode.json"), "utf8")).vectors;

// The decoder is TypeScript with .js import specifiers: bundle it the way the app is built.
const out = join(mkdtempSync(join(tmpdir(), "texdec-")), "texture_decode.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "vulkan", "texture_decode.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { decodeTexels, texelStats, channelHistogram, markTexels, MARK_COLOR, displayTexels } =
  await import(pathToFileURL(out).href);

function bytes(b64) {
  return new Uint8Array(Buffer.from(b64, "base64"));
}

for (const v of vectors) {
  test(`${v.format} ${v.width}x${v.height}`, () => {
    const data = bytes(v.data);
    const expected = bytes(v.expected);
    const tex = decodeTexels({ format: v.format, aspect: "color", width: v.width, height: v.height }, data);
    assert.ok(tex, "format decodes");
    const n = v.width * v.height;
    let worst = 0;
    let worstAt = "";
    // SNORM references store -1..1 as 0..255; float references clamp to 0..1 like UNORM.
    const signed = /SNORM/.test(v.format);
    // Punchthrough alpha: the specification makes a transparent texel black; the reference
    // keeps its color. Only the alpha is compared there.
    const punchthrough = /R8G8B8A1/.test(v.format);
    for (let i = 0; i < n; i++) {
      for (let c = 0; c < 4; c++) {
        if (punchthrough && c < 3 && expected[i * 4 + 3] === 0) continue;
        let value = tex.values[i * 4 + c];
        if (c >= tex.channels) value = c === 3 ? 1 : 0;
        // The references store signed and float data as 8-bit unsigned: -1..1 or 0..1 clamped.
        let got;
        if (signed && c < tex.channels) got = Math.round((Math.max(-1, Math.min(1, value)) * 0.5 + 0.5) * 255);
        else got = Math.round(Math.max(0, Math.min(1, value)) * 255);
        const diff = Math.abs(got - expected[i * 4 + c]);
        if (diff > worst) { worst = diff; worstAt = `texel (${i % v.width}, ${Math.floor(i / v.width)}) channel ${c}: got ${got}, expected ${expected[i * 4 + c]}`; }
      }
    }
    assert.ok(worst <= v.tolerance, `worst difference ${worst} > ${v.tolerance} at ${worstAt}`);
  });
}

// --- what an image holds past the picture of it -------------------------------------------------
//
// A NaN in a render target is invisible: it clamps to some ordinary color on screen and no other
// figure in a capture points at it. These cover the counting, the ranges that have to ignore it,
// and the marking that puts it where it can be seen.

/** An R32_SFLOAT image from the values given, one per texel. */
function floats(values) {
  const data = new Uint8Array(new Float32Array(values).buffer);
  return decodeTexels({ format: "VK_FORMAT_R32_SFLOAT", aspect: "color", width: values.length, height: 1 }, data);
}

test("NaN and the infinities are counted, per channel and per texel", () => {
  const tex = floats([0, 0.5, NaN, Infinity, -Infinity, 1]);
  const s = texelStats(tex);
  assert.equal(s.total, 6);
  assert.equal(s.nanTexels, 1);
  assert.equal(s.infTexels, 2, "one of each infinity");
  const r = s.channels[0];
  assert.equal(r.nan, 1);
  assert.equal(r.posInf, 1);
  assert.equal(r.negInf, 1);
  assert.equal(r.finite, 3);
});

test("the range is over the finite values, so one infinity does not become the whole image", () => {
  // The auto-ranged display divides by this range: with Infinity in it every other texel maps to
  // zero and the image goes black, hiding the very thing that is wrong with it.
  const tex = floats([0, 0.25, 0.5, Infinity]);
  assert.equal(tex.min[0], 0);
  assert.equal(tex.max[0], 0.5, "the infinity is not the maximum");
  const s = texelStats(tex);
  assert.equal(s.channels[0].max, 0.5);
  assert.equal(s.channels[0].mean, 0.25);
});

test("a channel with nothing finite in it reports a range of zero rather than infinities", () => {
  const tex = floats([NaN, Infinity, -Infinity]);
  assert.equal(tex.min[0], 0);
  assert.equal(tex.max[0], 0);
  const s = texelStats(tex);
  assert.equal(s.channels[0].finite, 0);
  assert.equal(s.channels[0].mean, 0);
});

test("an image of ordinary values is marked as nothing at all", () => {
  assert.equal(markTexels(floats([0, 0.5, 1]), false), null, "no overlay rather than a transparent one");
  assert.equal(markTexels(floats([0, 0.5, 1]), true), null);
});

test("NaN and the infinities are marked in their own colors", () => {
  const marks = markTexels(floats([0.5, NaN, Infinity, -Infinity]), false);
  assert.ok(marks, "an image with a NaN in it is marked");
  const at = (i) => [marks[i * 4], marks[i * 4 + 1], marks[i * 4 + 2]];
  assert.equal(marks[3], 0, "an ordinary texel is left transparent");
  assert.deepEqual(at(1), MARK_COLOR.nan);
  assert.deepEqual(at(2), MARK_COLOR.posInf);
  assert.deepEqual(at(3), MARK_COLOR.negInf);
});

test("clipping is marked only when asked for", () => {
  assert.equal(markTexels(floats([-0.5, 2]), false), null, "out of range is a picture's business, not an error");
  const marks = markTexels(floats([-0.5, 2]), true);
  assert.deepEqual([marks[0], marks[1], marks[2]], MARK_COLOR.below);
  assert.deepEqual([marks[4], marks[5], marks[6]], MARK_COLOR.above);
});

test("a histogram shows the shape a single outlier hides", () => {
  // Fifteen dark texels and one very bright one: the picture is black, the histogram is not.
  const values = new Array(15).fill(0.01);
  values.push(10000);
  const h = channelHistogram(floats(values), 0, 8);
  assert.equal(h.reduce((a, b) => a + b, 0), 16, "every finite texel lands in a bucket");
  assert.equal(h[0], 15, "the dark ones are together at the bottom");
  assert.equal(h[7], 1, "the outlier is alone at the top");
});

test("a histogram of a constant channel does not divide by an empty range", () => {
  const h = channelHistogram(floats([0.5, 0.5, 0.5]), 0, 4);
  assert.deepEqual(h, [3, 0, 0, 0]);
});

test("histogram buckets ignore what is not a number", () => {
  const h = channelHistogram(floats([0, 1, NaN, Infinity]), 0, 2);
  assert.equal(h.reduce((a, b) => a + b, 0), 2);
});
