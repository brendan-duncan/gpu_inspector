// ETC2 and EAC block decoding (the Khronos Data Format Specification, "ETC2 Compressed Texture
// Image Formats"). ETC2 RGB is a 64-bit block of 4x4 texels in one of five modes: individual
// and differential (two base colours with a modifier table), T and H (four paint colours) and
// planar (a colour gradient). The punchthrough alpha variant repurposes the differential bit as
// an opaque flag; the RGBA8 variant prefixes an EAC alpha block. EAC R11 and RG11 are the same
// 64-bit alpha coding at 11-bit precision, one block per channel.
//
// Blocks are big-endian: the first byte holds bits 63..56. Texel order inside a block is
// column-major (index = x * 4 + y); everything here writes row-major RGBA floats into `px`.

const MODIFIERS = [
  [2, 8, -2, -8], [5, 17, -5, -17], [9, 29, -9, -29], [13, 42, -13, -42],
  [18, 60, -18, -60], [24, 80, -24, -80], [33, 106, -33, -106], [47, 183, -47, -183],
];

const DISTANCES = [3, 6, 11, 16, 23, 32, 41, 64];

const ALPHA_MODIFIERS = [
  [-3, -6, -9, -15, 2, 5, 8, 14], [-3, -7, -10, -13, 2, 6, 9, 12], [-2, -5, -8, -13, 1, 4, 7, 12], [-2, -4, -6, -13, 1, 3, 5, 12],
  [-3, -6, -8, -12, 2, 5, 7, 11], [-3, -7, -9, -11, 2, 6, 8, 10], [-4, -7, -8, -11, 3, 6, 7, 10], [-3, -5, -8, -11, 2, 4, 7, 10],
  [-2, -6, -8, -10, 1, 5, 7, 9], [-2, -5, -8, -10, 1, 4, 7, 9], [-2, -4, -8, -10, 1, 3, 7, 9], [-2, -5, -7, -10, 1, 4, 6, 9],
  [-3, -4, -7, -10, 2, 3, 6, 9], [-1, -2, -3, -10, 0, 1, 2, 9], [-4, -6, -8, -9, 3, 5, 7, 8], [-3, -5, -7, -9, 2, 4, 6, 8],
];

function clamp255(v: number): number {
  return v < 0 ? 0 : v > 255 ? 255 : v;
}

/** Bits [hi..lo] of the 32-bit word (hi >= lo). */
function field(word: number, hi: number, lo: number): number {
  return (word >>> lo) & ((1 << (hi - lo + 1)) - 1);
}

function expand4(v: number): number { return (v << 4) | v; }
function expand5(v: number): number { return (v << 3) | (v >> 2); }
function expand6(v: number): number { return (v << 2) | (v >> 4); }
function expand7(v: number): number { return (v << 1) | (v >> 6); }

function put(px: Float32Array, x: number, y: number, r: number, g: number, b: number, a: number): void {
  const o = (y * 4 + x) * 4;
  px[o] = r / 255; px[o + 1] = g / 255; px[o + 2] = b / 255; px[o + 3] = a;
}

/**
 * Decodes the colour half of an ETC2 block (the high word `hi` holds bits 63..32, `lo` bits
 * 31..0). With `punchthrough`, the differential bit is the opaque flag and a clear one makes
 * modifier index 2 a transparent texel.
 */
function decodeEtc2Color(hi: number, lo: number, px: Float32Array, punchthrough: boolean): void {
  const differential = punchthrough || field(hi, 1, 1) === 1;
  const opaque = !punchthrough || field(hi, 1, 1) === 1;
  let r1: number, g1: number, b1: number, r2: number, g2: number, b2: number;
  let mode: "block" | "t" | "h" | "planar" = "block";
  if (differential) {
    r1 = field(hi, 31, 27); g1 = field(hi, 23, 19); b1 = field(hi, 15, 11);
    const dr = (field(hi, 26, 24) << 29) >> 29;
    const dg = (field(hi, 18, 16) << 29) >> 29;
    const db = (field(hi, 10, 8) << 29) >> 29;
    r2 = r1 + dr; g2 = g1 + dg; b2 = b1 + db;
    // An overflowing delta selects, in this order, the T, H and planar modes.
    if (r2 < 0 || r2 > 31) mode = "t";
    else if (g2 < 0 || g2 > 31) mode = "h";
    else if (b2 < 0 || b2 > 31) mode = "planar";
    if (mode === "block") {
      r1 = expand5(r1); g1 = expand5(g1); b1 = expand5(b1);
      r2 = expand5(r2); g2 = expand5(g2); b2 = expand5(b2);
    }
  } else {
    r1 = expand4(field(hi, 31, 28)); g1 = expand4(field(hi, 23, 20)); b1 = expand4(field(hi, 15, 12));
    r2 = expand4(field(hi, 27, 24)); g2 = expand4(field(hi, 19, 16)); b2 = expand4(field(hi, 11, 8));
  }

  if (mode === "planar") {
    // Three colours: the origin, and the ones a block's width to the right and a block's
    // height below, each split around the bits that force the differential overflow.
    const ro = expand6(field(hi, 30, 25));
    const go = expand7((field(hi, 24, 24) << 6) | field(hi, 22, 17));
    const bo = expand6((field(hi, 16, 16) << 5) | (field(hi, 12, 11) << 3) | field(hi, 9, 7));
    const rh = expand6((field(hi, 6, 2) << 1) | field(hi, 0, 0));
    const gh = expand7(field(lo, 31, 25));
    const bh = expand6(field(lo, 24, 19));
    const rv = expand6(field(lo, 18, 13));
    const gv = expand7(field(lo, 12, 6));
    const bv = expand6(field(lo, 5, 0));
    for (let y = 0; y < 4; y++) {
      for (let x = 0; x < 4; x++) {
        const r = clamp255((x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2);
        const g = clamp255((x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2);
        const b = clamp255((x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
        put(px, x, y, r, g, b, 1);
      }
    }
    return;
  }

  // The modifier / paint index of each texel: msb in the high half of the low word, lsb in the low half.
  const indexOf = (x: number, y: number): number => {
    const i = x * 4 + y;
    return (((lo >>> (16 + i)) & 1) << 1) | ((lo >>> i) & 1);
  };

  if (mode === "t" || mode === "h") {
    let paints: number[][];
    if (mode === "t") {
      const ra = expand4((field(hi, 28, 27) << 2) | field(hi, 25, 24));
      const ga = expand4(field(hi, 23, 20)); const ba = expand4(field(hi, 19, 16));
      const rb = expand4(field(hi, 15, 12)); const gb = expand4(field(hi, 11, 8)); const bb = expand4(field(hi, 7, 4));
      const d = DISTANCES[(field(hi, 3, 2) << 1) | field(hi, 0, 0)];
      paints = [[ra, ga, ba], [clamp255(rb + d), clamp255(gb + d), clamp255(bb + d)], [rb, gb, bb], [clamp255(rb - d), clamp255(gb - d), clamp255(bb - d)]];
    } else {
      const ra = expand4(field(hi, 30, 27));
      const ga = expand4((field(hi, 26, 24) << 1) | field(hi, 20, 20));
      const ba = expand4((field(hi, 19, 19) << 3) | field(hi, 17, 15));
      const rb = expand4(field(hi, 14, 11)); const gb = expand4(field(hi, 10, 7)); const bb = expand4(field(hi, 6, 3));
      const va = (ra << 16) | (ga << 8) | ba;
      const vb = (rb << 16) | (gb << 8) | bb;
      const d = DISTANCES[(field(hi, 2, 2) << 2) | (field(hi, 0, 0) << 1) | (va >= vb ? 1 : 0)];
      paints = [[clamp255(ra + d), clamp255(ga + d), clamp255(ba + d)], [clamp255(ra - d), clamp255(ga - d), clamp255(ba - d)],
        [clamp255(rb + d), clamp255(gb + d), clamp255(bb + d)], [clamp255(rb - d), clamp255(gb - d), clamp255(bb - d)]];
    }
    for (let y = 0; y < 4; y++) {
      for (let x = 0; x < 4; x++) {
        const i = indexOf(x, y);
        if (!opaque && i === 2) put(px, x, y, 0, 0, 0, 0);
        else put(px, x, y, paints[i][0], paints[i][1], paints[i][2], 1);
      }
    }
    return;
  }

  // Individual / differential: two 2x4 halves, side by side or stacked, each with its own modifier table.
  const flip = field(hi, 0, 0) === 1;
  const table1 = MODIFIERS[field(hi, 7, 5)];
  const table2 = MODIFIERS[field(hi, 4, 2)];
  for (let y = 0; y < 4; y++) {
    for (let x = 0; x < 4; x++) {
      const second = flip ? y >= 2 : x >= 2;
      const table = second ? table2 : table1;
      const i = indexOf(x, y);
      if (!opaque && i === 2) { put(px, x, y, 0, 0, 0, 0); continue; }
      // Without the opaque bit the small modifier is zero (indices 0 and 2 collapse).
      const m = !opaque && i === 0 ? 0 : table[i];
      const r = second ? r2 : r1;
      const g = second ? g2 : g1;
      const b = second ? b2 : b1;
      put(px, x, y, clamp255(r + m), clamp255(g + m), clamp255(b + m), 1);
    }
  }
}

/**
 * Decodes an EAC 64-bit block into 16 values (column-major bit order, written row-major into
 * `out` at stride 4 from `channel`): `base` and the 4-bit multiplier scale a table entry.
 * With `eleven` the result is the 11-bit form (R11 / RG11), else the 8-bit alpha form.
 */
function decodeEac(hi: number, lo: number, out: Float32Array, channel: number, eleven: boolean, signed: boolean): void {
  let base = field(hi, 31, 24);
  const multiplier = field(hi, 23, 20);
  const table = ALPHA_MODIFIERS[field(hi, 19, 16)];
  if (signed) {
    base = (base << 24) >> 24;
    if (base === -128) base = -127;
  }
  // At 11 bits the table entry is scaled by eight times the multiplier, or by one when the
  // multiplier is zero.
  const scale = !eleven ? multiplier : multiplier === 0 ? 1 : multiplier * 8;
  for (let i = 0; i < 16; i++) {
    // 3-bit indices from bit 47 downwards, spanning the two words.
    const bit = 45 - i * 3;
    let sel: number;
    if (bit >= 32) sel = (hi >>> (bit - 32)) & 7;
    else if (bit === 31) sel = ((hi & 3) << 1) | (lo >>> 31);
    else if (bit === 30) sel = ((hi & 1) << 2) | (lo >>> 30);
    else sel = (lo >>> bit) & 7;
    const x = i >> 2;
    const y = i & 3;
    let v: number;
    if (!eleven) {
      v = clamp255(base + scale * table[sel]) / 255;
    } else if (signed) {
      const raw = base * 8 + scale * table[sel];
      v = Math.max(-1023, Math.min(1023, raw)) / 1023;
    } else {
      const raw = base * 8 + 4 + scale * table[sel];
      v = Math.max(0, Math.min(2047, raw)) / 2047;
    }
    out[(y * 4 + x) * 4 + channel] = v;
  }
}

function words(s: DataView, at: number): [number, number] {
  return [s.getUint32(at, false), s.getUint32(at + 4, false)];
}

/** ETC2 RGB8: one 64-bit block. */
export function decodeEtc2Rgb(s: DataView, block: number, px: Float32Array): void {
  const [hi, lo] = words(s, block);
  decodeEtc2Color(hi, lo, px, false);
}

/** ETC2 RGB8A1: one 64-bit block with punchthrough alpha. */
export function decodeEtc2Rgba1(s: DataView, block: number, px: Float32Array): void {
  const [hi, lo] = words(s, block);
  decodeEtc2Color(hi, lo, px, true);
}

/** ETC2 RGBA8: an EAC alpha block followed by an ETC2 colour block. */
export function decodeEtc2Rgba8(s: DataView, block: number, px: Float32Array): void {
  const [chi, clo] = words(s, block + 8);
  decodeEtc2Color(chi, clo, px, false);
  const [ahi, alo] = words(s, block);
  decodeEac(ahi, alo, px, 3, false, false);
}

/** EAC R11: one 64-bit block, one channel. */
export function decodeEacR11(s: DataView, block: number, px: Float32Array, signed: boolean): void {
  px.fill(0);
  const [hi, lo] = words(s, block);
  decodeEac(hi, lo, px, 0, true, signed);
}

/** EAC RG11: two 64-bit blocks, red then green. */
export function decodeEacRg11(s: DataView, block: number, px: Float32Array, signed: boolean): void {
  px.fill(0);
  const [rhi, rlo] = words(s, block);
  decodeEac(rhi, rlo, px, 0, true, signed);
  const [ghi, glo] = words(s, block + 8);
  decodeEac(ghi, glo, px, 1, true, signed);
}
