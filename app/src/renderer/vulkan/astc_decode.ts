// ASTC block decoding (the Khronos Data Format Specification, "ASTC Compressed Texture Image
// Formats"), LDR profile: 128-bit blocks of any footprint from 4x4 to 12x12, up to four
// partitions with their own endpoint modes, a weight grid smaller than the block that is
// bilinearly stretched over it, and integer sequences quantized in trits and quints as well
// as bits. Blocks decode to RGBA floats, row-major into `px`; HDR endpoint modes and invalid
// blocks give the error colour the specification prescribes (magenta).

/** 0..64 weight and 0..255 colour unquantization: the trit/quint forms need bit patterns. */
interface Quant { levels: number; bits: number; trits: boolean; quints: boolean }

const q = (levels: number, bits: number, trits = false, quints = false): Quant => ({ levels, bits, trits, quints });

/** Weight quantization by the block mode's 3-bit range and its high-precision bit. */
const WEIGHT_QUANT: (Quant | null)[] = [
  null, null, q(2, 1), q(3, 0, true), q(4, 2), q(5, 0, false, true), q(6, 1, true), q(8, 3),
  null, null, q(10, 1, false, true), q(12, 2, true), q(16, 4), q(20, 2, false, true), q(24, 3, true), q(32, 5),
];

/** Colour quantization levels, ascending: the largest that fits the bits left is used. */
const COLOR_QUANT: Quant[] = [
  q(6, 1, true), q(8, 3), q(10, 1, false, true), q(12, 2, true), q(16, 4), q(20, 2, false, true), q(24, 3, true), q(32, 5),
  q(40, 3, false, true), q(48, 4, true), q(64, 6), q(80, 4, false, true), q(96, 5, true), q(128, 7), q(160, 5, false, true),
  q(192, 6, true), q(256, 8),
];

/** Bits an integer sequence of `count` values takes. */
function iseBits(count: number, quant: Quant): number {
  let bits = count * quant.bits;
  if (quant.trits) bits += Math.ceil(count * 8 / 5);
  if (quant.quints) bits += Math.ceil(count * 7 / 3);
  return bits;
}

/** LSB-first bit reader over 16 bytes. */
class Reader {
  pos = 0;
  constructor(private readonly bytes: Uint8Array) {}
  bit(): number {
    const p = this.pos++;
    return p < 128 ? (this.bytes[p >> 3] >> (p & 7)) & 1 : 0;
  }
  read(n: number): number {
    let v = 0;
    for (let i = 0; i < n; i++) v |= this.bit() << i;
    return v;
  }
  at(pos: number, n: number): number {
    this.pos = pos;
    return this.read(n);
  }
}

function tritsOf(t: number): number[] {
  let c: number;
  let t3: number;
  let t4: number;
  if (((t >> 2) & 7) === 7) {
    c = (((t >> 5) & 7) << 2) | (t & 3);
    t4 = 2; t3 = 2;
  } else {
    c = t & 0x1f;
    if (((t >> 5) & 3) === 3) { t4 = 2; t3 = (t >> 7) & 1; }
    else { t4 = (t >> 7) & 1; t3 = (t >> 5) & 3; }
  }
  let t0: number;
  let t1: number;
  let t2: number;
  if ((c & 3) === 3) {
    t2 = 2; t1 = (c >> 4) & 1;
    const c3 = (c >> 3) & 1;
    t0 = (c3 << 1) | (((c >> 2) & 1) & (c3 ^ 1));
  } else if (((c >> 2) & 3) === 3) {
    t2 = 2; t1 = 2; t0 = c & 3;
  } else {
    t2 = (c >> 4) & 1; t1 = (c >> 2) & 3;
    const c1 = (c >> 1) & 1;
    t0 = (c1 << 1) | ((c & 1) & (c1 ^ 1));
  }
  return [t0, t1, t2, t3, t4];
}

function quintsOf(qv: number): number[] {
  let q0: number;
  let q1: number;
  let q2: number;
  if (((qv >> 1) & 3) === 3 && ((qv >> 5) & 3) === 0) {
    const q0bit = qv & 1;
    q2 = (q0bit << 2) | ((((qv >> 4) & 1) & (q0bit ^ 1)) << 1) | (((qv >> 3) & 1) & (q0bit ^ 1));
    q1 = 4; q0 = 4;
  } else {
    let c: number;
    if (((qv >> 1) & 3) === 3) {
      q2 = 4;
      c = (((qv >> 3) & 3) << 3) | (((~qv >> 5) & 3) << 1) | (qv & 1);
    } else {
      q2 = (qv >> 5) & 3;
      c = qv & 0x1f;
    }
    if ((c & 7) === 5) { q1 = 4; q0 = (c >> 3) & 3; }
    else { q1 = (c >> 3) & 3; q0 = c & 7; }
  }
  return [q0, q1, q2];
}

/**
 * Reads `count` values of an integer sequence: each as (trit or quint) << 8 | bits. The
 * trit and quint bits are interleaved with the values in blocks of five and three, and a
 * final partial block stores only the bits its values need.
 */
function decodeIse(r: Reader, count: number, quant: Quant, out: Int32Array): void {
  if (quant.trits) {
    const sizes = [2, 2, 1, 2, 1];
    for (let i = 0; i < count; i += 5) {
      const n = Math.min(5, count - i);
      let t = 0;
      let shift = 0;
      const m: number[] = [];
      for (let j = 0; j < n; j++) {
        m.push(r.read(quant.bits));
        t |= r.read(sizes[j]) << shift;
        shift += sizes[j];
      }
      const trits = tritsOf(t);
      for (let j = 0; j < n; j++) out[i + j] = (trits[j] << 8) | m[j];
    }
  } else if (quant.quints) {
    const sizes = [3, 2, 2];
    for (let i = 0; i < count; i += 3) {
      const n = Math.min(3, count - i);
      let qv = 0;
      let shift = 0;
      const m: number[] = [];
      for (let j = 0; j < n; j++) {
        m.push(r.read(quant.bits));
        qv |= r.read(sizes[j]) << shift;
        shift += sizes[j];
      }
      const quints = quintsOf(qv);
      for (let j = 0; j < n; j++) out[i + j] = (quints[j] << 8) | m[j];
    }
  } else {
    for (let i = 0; i < count; i++) out[i] = r.read(quant.bits);
  }
}

/** A pattern like "b000b0bb0" over the value's bits (a = bit 0, b = bit 1, ...), as a number. */
function pattern(spec: string, m: number): number {
  let v = 0;
  for (let i = 0; i < spec.length; i++) {
    const ch = spec[i];
    const bit = ch === "0" ? 0 : (m >> (ch.charCodeAt(0) - 97)) & 1;
    v = (v << 1) | bit;
  }
  return v;
}

function replicate(m: number, bits: number, to: number): number {
  let r = m;
  let n = bits;
  while (n < to) { r = (r << n) | r; n *= 2; }
  return r >> (n - to);
}

const COLOR_TRIT: Record<number, [string, number]> = { 1: ["000000000", 204], 2: ["b000b0bb0", 93], 3: ["cb000cbcb", 44], 4: ["dcb000dcb", 22], 5: ["edcb000ed", 11], 6: ["fedcb000f", 5] };
const COLOR_QUINT: Record<number, [string, number]> = { 1: ["000000000", 113], 2: ["b0000bb00", 54], 3: ["cb0000cbc", 26], 4: ["dcb0000dc", 13], 5: ["edcb0000e", 6] };
const WEIGHT_TRIT: Record<number, [string, number]> = { 1: ["0000000", 50], 2: ["b000b0b", 23], 3: ["cb000cb", 11] };
const WEIGHT_QUINT: Record<number, [string, number]> = { 1: ["0000000", 28], 2: ["b0000b0", 13] };

function unquantizeColor(v: number, quant: Quant): number {
  const m = v & 0xff;
  const d = v >> 8;
  if (!quant.trits && !quant.quints) return replicate(m, quant.bits, 8);
  const [b, c] = quant.trits ? COLOR_TRIT[quant.bits] : COLOR_QUINT[quant.bits];
  const a = (m & 1) ? 0x1ff : 0;
  let t = d * c + pattern(b, m);
  t ^= a;
  return (a & 0x80) | (t >> 2);
}

function unquantizeWeight(v: number, quant: Quant): number {
  const m = v & 0xff;
  const d = v >> 8;
  // A lone trit or quint has no bits to fold in: three or five evenly spaced weights.
  if (quant.bits === 0) return quant.trits ? d * 32 : d * 16;
  let w: number;
  if (!quant.trits && !quant.quints) {
    switch (quant.bits) {
      case 1: w = m ? 63 : 0; break;
      case 2: w = m * 21; break;
      case 3: w = m * 9; break;
      case 4: w = (m << 2) | (m >> 2); break;
      default: w = (m << 1) | (m >> 4); break;
    }
  } else {
    const [b, c] = quant.trits ? WEIGHT_TRIT[quant.bits] : WEIGHT_QUINT[quant.bits];
    const a = (m & 1) ? 0x7f : 0;
    let t = d * c + pattern(b, m);
    t ^= a;
    w = (a & 0x20) | (t >> 2);
  }
  return w > 32 ? w + 1 : w;
}

// ---------------------------------------------------------------------------------------------
// Partitions: a hash of the texel position seeded by the partition index.

function hash52(p: number): number {
  p = (p ^ (p >>> 15)) >>> 0;
  p = (p - (p << 17)) >>> 0;
  p = (p + (p << 7)) >>> 0;
  p = (p + (p << 4)) >>> 0;
  p = (p ^ (p >>> 5)) >>> 0;
  p = (p + (p << 16)) >>> 0;
  p = (p ^ (p >>> 7)) >>> 0;
  p = (p ^ (p >>> 3)) >>> 0;
  p = (p ^ (p << 6)) >>> 0;
  p = (p ^ (p >>> 17)) >>> 0;
  return p;
}

function selectPartition(seed: number, x: number, y: number, count: number, small: boolean): number {
  if (small) { x <<= 1; y <<= 1; }
  seed += (count - 1) * 1024;
  const rnum = hash52(seed);
  const s: number[] = [];
  for (let i = 0; i < 12; i++) {
    const v = (rnum >>> (i * 4)) & 0xf;
    s.push(v * v);
  }
  let sh1: number;
  let sh2: number;
  if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = count === 3 ? 6 : 5; }
  else { sh1 = count === 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
  const sh3 = (seed & 0x10) ? sh1 : sh2;
  s[0] >>= sh1; s[1] >>= sh2; s[2] >>= sh1; s[3] >>= sh2; s[4] >>= sh1; s[5] >>= sh2; s[6] >>= sh1; s[7] >>= sh2;
  s[8] >>= sh3; s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3;
  let a = (s[0] * x + s[1] * y + (rnum >>> 14)) & 0x3f;
  let b = (s[2] * x + s[3] * y + (rnum >>> 10)) & 0x3f;
  let c = (s[4] * x + s[5] * y + (rnum >>> 6)) & 0x3f;
  let d = (s[6] * x + s[7] * y + (rnum >>> 2)) & 0x3f;
  if (count < 4) d = 0;
  if (count < 3) c = 0;
  if (a >= b && a >= c && a >= d) return 0;
  if (b >= c && b >= d) return 1;
  if (c >= d) return 2;
  return 3;
}

// ---------------------------------------------------------------------------------------------
// Endpoints: the colour integers of a partition to two RGBA endpoints, by endpoint mode.

function clamp255(v: number): number { return v < 0 ? 0 : v > 255 ? 255 : v; }

/** The base/offset forms move one bit between the pair: `v[i]` becomes the signed offset, `v[j]` the base. */
function bitTransfer(v: number[], i: number, j: number): void {
  let a = v[i];
  let b = v[j];
  b >>= 1;
  b |= a & 0x80;
  a >>= 1;
  a &= 0x3f;
  if (a & 0x20) a -= 0x40;
  v[i] = a;
  v[j] = b;
}

function blueContract(r: number, g: number, b: number, a: number): number[] {
  return [(r + b) >> 1, (g + b) >> 1, b, a];
}

/** Returns [e0, e1] as 8-bit RGBA, or null for an HDR mode. */
function decodeEndpoints(cem: number, v: number[]): [number[], number[]] | null {
  switch (cem) {
    case 0: return [[v[0], v[0], v[0], 255], [v[1], v[1], v[1], 255]];
    case 1: {
      const l0 = (v[0] >> 2) | (v[1] & 0xc0);
      const l1 = Math.min(255, l0 + (v[1] & 0x3f));
      return [[l0, l0, l0, 255], [l1, l1, l1, 255]];
    }
    case 4: return [[v[0], v[0], v[0], v[2]], [v[1], v[1], v[1], v[3]]];
    case 5: {
      bitTransfer(v, 1, 0);
      bitTransfer(v, 3, 2);
      const l1 = clamp255(v[0] + v[1]);
      return [[v[0], v[0], v[0], v[2]], [l1, l1, l1, clamp255(v[2] + v[3])]];
    }
    case 6: return [[(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255], [v[0], v[1], v[2], 255]];
    case 8: {
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) return [[v[0], v[2], v[4], 255], [v[1], v[3], v[5], 255]];
      return [blueContract(v[1], v[3], v[5], 255), blueContract(v[0], v[2], v[4], 255)];
    }
    case 9: {
      bitTransfer(v, 1, 0); bitTransfer(v, 3, 2); bitTransfer(v, 5, 4);
      const r1 = v[0] + v[1];
      const g1 = v[2] + v[3];
      const b1 = v[4] + v[5];
      let e0: number[];
      let e1: number[];
      if (v[1] + v[3] + v[5] >= 0) { e0 = [v[0], v[2], v[4], 255]; e1 = [r1, g1, b1, 255]; }
      else { e0 = blueContract(r1, g1, b1, 255); e1 = blueContract(v[0], v[2], v[4], 255); }
      return [e0.map(clamp255), e1.map(clamp255)];
    }
    case 10: return [[(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]], [v[0], v[1], v[2], v[5]]];
    case 12: {
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) return [[v[0], v[2], v[4], v[6]], [v[1], v[3], v[5], v[7]]];
      return [blueContract(v[1], v[3], v[5], v[7]), blueContract(v[0], v[2], v[4], v[6])];
    }
    case 13: {
      bitTransfer(v, 1, 0); bitTransfer(v, 3, 2); bitTransfer(v, 5, 4); bitTransfer(v, 7, 6);
      const r1 = v[0] + v[1];
      const g1 = v[2] + v[3];
      const b1 = v[4] + v[5];
      const a1 = v[6] + v[7];
      let e0: number[];
      let e1: number[];
      if (v[1] + v[3] + v[5] >= 0) { e0 = [v[0], v[2], v[4], v[6]]; e1 = [r1, g1, b1, a1]; }
      else { e0 = blueContract(r1, g1, b1, a1); e1 = blueContract(v[0], v[2], v[4], v[6]); }
      return [e0.map(clamp255), e1.map(clamp255)];
    }
    default:
      // 2, 3, 7, 11, 14, 15: HDR endpoints.
      return null;
  }
}

// ---------------------------------------------------------------------------------------------

function halfToFloat(h: number): number {
  const s = (h & 0x8000) ? -1 : 1;
  const e = (h >> 10) & 0x1f;
  const f = h & 0x3ff;
  if (e === 0) return s * Math.pow(2, -14) * (f / 1024);
  if (e === 31) return f ? NaN : s * Infinity;
  return s * Math.pow(2, e - 15) * (1 + f / 1024);
}

function errorColor(px: Float32Array, texels: number): void {
  for (let t = 0; t < texels; t++) { px[t * 4] = 1; px[t * 4 + 1] = 0; px[t * 4 + 2] = 1; px[t * 4 + 3] = 1; }
}

const scratchInts = new Int32Array(80);
const scratchWeights = new Int32Array(64);

/**
 * Decodes one ASTC block of `bw` x `bh` texels at `block` into `px` (bw*bh RGBA floats,
 * row-major). `srgb` selects the sRGB endpoint expansion; either way the result is what an
 * 8-bit UNORM texel reads as.
 */
export function decodeAstcBlock(s: DataView, block: number, bw: number, bh: number, px: Float32Array, srgb: boolean): void {
  const texels = bw * bh;
  const bytes = new Uint8Array(16);
  for (let i = 0; i < 16; i++) bytes[i] = s.getUint8(block + i);
  const r = new Reader(bytes);
  const mode = r.at(0, 11);

  // Void extent: one colour for the whole block, four 16-bit values from bit 64.
  if ((mode & 0x1ff) === 0x1fc) {
    const hdr = (mode >> 9) & 1;
    const c: number[] = [];
    for (let i = 0; i < 4; i++) c.push(r.at(64 + i * 16, 16));
    for (let t = 0; t < texels; t++) {
      for (let i = 0; i < 4; i++) px[t * 4 + i] = hdr ? halfToFloat(c[i]) : (c[i] >> 8) / 255;
    }
    return;
  }

  // Block mode: weight grid size, range, dual plane and high precision.
  let dual = (mode >> 10) & 1;
  let high = (mode >> 9) & 1;
  let gw: number;
  let gh: number;
  let range: number;
  const a = (mode >> 5) & 3;
  if (mode & 3) {
    range = (((mode >> 1) & 1) << 2) | ((mode & 1) << 1) | ((mode >> 4) & 1);
    const b = (mode >> 7) & 3;
    switch ((mode >> 2) & 3) {
      case 0: gw = b + 4; gh = a + 2; break;
      case 1: gw = b + 8; gh = a + 2; break;
      case 2: gw = a + 2; gh = b + 8; break;
      default:
        if (mode & 0x100) { gw = (b & 1) + 2; gh = a + 2; }
        else { gw = a + 2; gh = (b & 1) + 6; }
        break;
    }
  } else {
    range = (((mode >> 3) & 1) << 2) | (((mode >> 2) & 1) << 1) | ((mode >> 4) & 1);
    switch ((mode >> 7) & 3) {
      case 0: gw = 12; gh = a + 2; break;
      case 1: gw = a + 2; gh = 12; break;
      case 2: gw = a + 6; gh = ((mode >> 9) & 3) + 6; dual = 0; high = 0; break;
      default:
        if (a === 0) { gw = 6; gh = 10; }
        else if (a === 1) { gw = 10; gh = 6; }
        else { errorColor(px, texels); return; }
        break;
    }
  }
  const weightQuant = WEIGHT_QUANT[(high << 3) | range];
  const weightCount = gw * gh * (dual ? 2 : 1);
  if (!weightQuant || gw > bw || gh > bh || weightCount > 64) { errorColor(px, texels); return; }
  const weightBits = iseBits(weightCount, weightQuant);
  if (weightBits < 24 || weightBits > 96) { errorColor(px, texels); return; }

  // Partitions and endpoint modes.
  const partitions = r.at(11, 2) + 1;
  if (dual && partitions > 3) { errorColor(px, texels); return; }
  const cems: number[] = [];
  let partitionIndex = 0;
  let colorStart: number;
  let extraBits = 0;
  if (partitions === 1) {
    cems.push(r.at(13, 4));
    colorStart = 17;
  } else {
    partitionIndex = r.at(13, 10);
    colorStart = 29;
    let encoded = r.at(23, 6);
    const cls = encoded & 3;
    if (cls === 0) {
      for (let p = 0; p < partitions; p++) cems.push(encoded >> 2);
    } else {
      extraBits = 3 * partitions - 4;
      encoded |= r.at(128 - weightBits - extraBits, extraBits) << 6;
      for (let p = 0; p < partitions; p++) {
        const c = (encoded >> (2 + p)) & 1;
        const m = (encoded >> (2 + partitions + 2 * p)) & 3;
        cems.push(((cls - 1 + c) << 2) | m);
      }
    }
  }
  const ccs = dual ? r.at(128 - weightBits - extraBits - 2, 2) : 0;

  // The colour integers, at the largest quantization the remaining bits allow.
  let intCount = 0;
  for (const cem of cems) intCount += ((cem >> 2) + 1) * 2;
  const colorBits = 128 - weightBits - extraBits - (dual ? 2 : 0) - colorStart;
  let colorQuant: Quant | null = null;
  for (const cq of COLOR_QUANT) {
    if (iseBits(intCount, cq) <= colorBits) colorQuant = cq;
    else break;
  }
  if (!colorQuant || intCount > 18) { errorColor(px, texels); return; }
  r.pos = colorStart;
  decodeIse(r, intCount, colorQuant, scratchInts);
  const values: number[] = [];
  for (let i = 0; i < intCount; i++) values.push(unquantizeColor(scratchInts[i], colorQuant));
  const endpoints: [number[], number[]][] = [];
  let at = 0;
  for (const cem of cems) {
    const n = ((cem >> 2) + 1) * 2;
    const e = decodeEndpoints(cem, values.slice(at, at + n));
    if (!e) { errorColor(px, texels); return; }
    endpoints.push(e);
    at += n;
  }

  // Weights: stored from the top of the block downwards, so read them from the reversed bits.
  const reversed = new Uint8Array(16);
  for (let i = 0; i < 128; i++) {
    if ((bytes[15 - (i >> 3)] >> (7 - (i & 7))) & 1) reversed[i >> 3] |= 1 << (i & 7);
  }
  const wr = new Reader(reversed);
  decodeIse(wr, weightCount, weightQuant, scratchWeights);
  const weights = new Int32Array(weightCount);
  for (let i = 0; i < weightCount; i++) weights[i] = unquantizeWeight(scratchWeights[i], weightQuant);
  const planes = dual ? 2 : 1;
  const weightAt = (plane: number, tx: number, ty: number): number => {
    if (tx >= gw || ty >= gh) return 0;
    return weights[(ty * gw + tx) * planes + plane];
  };
  // Infill: the grid is stretched over the block with fixed-point bilinear weights.
  const ds = Math.floor((1024 + (bw >> 1)) / (bw - 1));
  const dt = Math.floor((1024 + (bh >> 1)) / (bh - 1));
  const small = texels < 31;
  for (let y = 0; y < bh; y++) {
    const gt = (dt * y * (gh - 1) + 32) >> 6;
    const jt = gt >> 4;
    const ft = gt & 0xf;
    for (let x = 0; x < bw; x++) {
      const gs = (ds * x * (gw - 1) + 32) >> 6;
      const js = gs >> 4;
      const fs = gs & 0xf;
      const w11 = (fs * ft + 8) >> 4;
      const w10 = ft - w11;
      const w01 = fs - w11;
      const w00 = 16 - fs - ft + w11;
      const infill = (plane: number): number =>
        (weightAt(plane, js, jt) * w00 + weightAt(plane, js + 1, jt) * w01 + weightAt(plane, js, jt + 1) * w10 + weightAt(plane, js + 1, jt + 1) * w11 + 8) >> 4;
      const w0 = infill(0);
      const w1 = dual ? infill(1) : w0;
      const part = partitions === 1 ? 0 : selectPartition(partitionIndex, x, y, partitions, small);
      const [e0, e1] = endpoints[part];
      const o = (y * bw + x) * 4;
      for (let c = 0; c < 4; c++) {
        const lo = srgb && c < 3 ? (e0[c] << 8) | 0x80 : (e0[c] << 8) | e0[c];
        const hi = srgb && c < 3 ? (e1[c] << 8) | 0x80 : (e1[c] << 8) | e1[c];
        const w = dual && c === ccs ? w1 : w0;
        const v = (lo * (64 - w) + hi * w + 32) >> 6;
        px[o + c] = (v >> 8) / 255;
      }
    }
  }
}
