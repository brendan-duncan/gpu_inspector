// Signed BC6H, against blocks whose result the D3D11 specification fixes exactly.
//
// The random-block vectors (texture_decode.test.js) cover the unsigned form, where Pillow is a
// usable reference. For the signed form no reference here decodes correctly, so these cases are
// derived from the specification instead. Both use mode 11 and mode 7, whose layouts are simple
// enough to build by hand: mode 11 is one subset with 10-bit endpoints and no transform, mode 7
// one subset with 11-bit endpoints and 9-bit deltas.
//
// Between them they pin every step of the signed path: the sign extension of the endpoints, the
// wrap and sign extension of a transformed endpoint, the saturation in the signed unquantize,
// the interpolation across zero, and the sign-magnitude half the result is written as.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "bc6h-")), "bc67_decode.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "vulkan", "bc67_decode.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { decodeBc6hBlock } = await import(pathToFileURL(out).href);

/** The largest half, which every saturated endpoint decodes to. */
const MAX_HALF = 65504;

/** Packs fields into a 16-byte block, LSB first, starting at the given bit. */
function pack(fields) {
  let v = 0n;
  let pos = 0n;
  for (const [value, width] of fields) {
    v |= (BigInt(value) & ((1n << BigInt(width)) - 1n)) << pos;
    pos += BigInt(width);
  }
  const bytes = new Uint8Array(16);
  for (let i = 0; i < 16; i++) bytes[i] = Number((v >> BigInt(i * 8)) & 0xffn);
  return bytes;
}

/** Sixteen 4-bit indices, the first anchored to 3 bits. */
function indices(value) {
  const out = [[value & 7, 3]];
  for (let i = 1; i < 16; i++) out.push([value & 15, 4]);
  return out;
}

/** Mode 11: five mode bits, then both endpoints as 10 bits per channel, then the indices. */
function mode11(e0, e1, index) {
  return pack([[0b00011, 5], ...[...e0, ...e1].map((v) => [v, 10]), ...indices(index)]);
}

/**
 * Mode 7: five mode bits, the base endpoint as 10 low bits per channel, then each channel's
 * 9-bit delta followed by the base channel's 11th bit, then the indices.
 */
function mode7(base, delta, index) {
  return pack([
    [0b00111, 5], [base[0], 10], [base[1], 10], [base[2], 10],
    [delta[0], 9], [base[0] >> 10, 1], [delta[1], 9], [base[1] >> 10, 1], [delta[2], 9], [base[2] >> 10, 1],
    ...indices(index),
  ]);
}

function decode(bytes) {
  const px = new Float32Array(64);
  decodeBc6hBlock(new DataView(bytes.buffer, bytes.byteOffset, 16), 0, px, true);
  return px;
}

/** The red channel of one texel. */
const red = (px, texel) => px[texel * 4];

test("signed endpoints saturate to the largest half, either sign", () => {
  // 511 is the largest 10-bit signed value, which the unquantize saturates to 0x7FFF, and the
  // 31/32 of the final step makes 0x7BFF: the largest half.
  const positive = decode(mode11([511, 511, 511], [0, 0, 0], 0));
  const negative = decode(mode11([-511, -511, -511], [0, 0, 0], 0));
  for (let t = 0; t < 16; t++) {
    assert.equal(red(positive, t), MAX_HALF, `texel ${t}`);
    assert.equal(red(negative, t), -MAX_HALF, `texel ${t}`);
  }
});

test("a signed endpoint below saturation unquantizes to its scaled value", () => {
  // 256 is half of the 10-bit range: ((256 << 15) + 0x4000) >> 9 = 16416, scaled by 31/32 to
  // 15903, which is the half 1.5302734.
  const px = decode(mode11([256, 256, 256], [0, 0, 0], 0));
  assert.ok(Math.abs(red(px, 0) - 1.5302734) < 1e-6, `got ${red(px, 0)}`);
});

test("the anchor texel of a signed block uses one index bit fewer", () => {
  // Every index is 15, but texel 0 keeps only three bits of it, so its weight is 30 of 64
  // rather than 64: the endpoints 0 and 0x7FFF interpolate to the half 0.765625.
  const px = decode(mode11([0, 0, 0], [511, 511, 511], 15));
  assert.equal(red(px, 0), 0.765625);
  for (let t = 1; t < 16; t++) assert.equal(red(px, t), MAX_HALF, `texel ${t}`);
});

test("a transformed signed endpoint wraps and is sign extended", () => {
  // Base +1023 plus a delta of 2 wraps to 1025 in eleven bits, which as a signed value is
  // -1023: the two endpoints are the largest half of either sign. Without the sign extension
  // the second endpoint would saturate positive instead, so this case separates the two.
  const px = decode(mode7([1023, 1023, 1023], [2, 2, 2], 15));
  assert.equal(red(px, 1), -MAX_HALF, "the far endpoint is negative");
  // Texel 0 keeps three index bits, so it is 34 of 64 towards the positive endpoint and 30
  // towards the negative one: just above zero.
  assert.ok(red(px, 0) > 0 && red(px, 0) < 0.01, `got ${red(px, 0)}`);
});

test("a signed block interpolating across zero stays continuous", () => {
  // Base -1023 with a delta of -2 wraps to +1023: the same pair the other way round.
  const px = decode(mode7([-1023 & 0x7ff, -1023 & 0x7ff, -1023 & 0x7ff], [-2, -2, -2], 15));
  assert.equal(red(px, 1), MAX_HALF, "the far endpoint is positive");
  assert.ok(red(px, 0) < 0 && red(px, 0) > -0.01, `got ${red(px, 0)}`);
});
