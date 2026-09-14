// BC6H and BC7 block decoding (the D3D11 "BC6H/BC7 Format" specification). Both are 16-byte
// blocks of 4x4 texels with up to three subsets per block, each subset an endpoint pair with
// per-texel interpolation indices; BC7 adds alpha and channel rotation, BC6H is half-float RGB
// with delta-coded endpoints. Blocks decode to RGBA floats, 16 texels row-major into `px`.
//
// The partition and fix-up tables are the ones in the specification, shared by both formats.

/** Subset of each texel, per partition, for 2- and 3-subset blocks (64 partitions of 16). */
const PARTITIONS_2 = new Uint8Array([
  0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1, 0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1, 0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1, 0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1,
  0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1, 0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1, 0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1,
  0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1, 0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1,
  0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1, 0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,
  0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1, 0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0, 0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0,
  0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0, 0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0, 0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0, 0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1,
  0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0, 0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0, 0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0, 0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0,
  0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0, 0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0, 0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0, 0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0,
  0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1, 0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1, 0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0, 0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0,
  0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0, 0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0, 0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1, 0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1,
  0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0, 0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0, 0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0, 0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0,
  0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0, 0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1, 0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1, 0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0,
  0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0, 0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0, 0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0, 0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0,
  0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1, 0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1, 0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0, 0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0,
  0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1, 0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1, 0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1, 0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1,
  0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1, 0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0, 0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0, 0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1,
]);

const PARTITIONS_3 = new Uint8Array([
  0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2, 0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1, 0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1, 0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1,
  0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2, 0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2, 0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1, 0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1,
  0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2, 0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2, 0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2, 0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,
  0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2, 0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2, 0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2, 0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0,
  0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2, 0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0, 0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2, 0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1,
  0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2, 0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1, 0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2, 0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0,
  0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0, 0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2, 0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0, 0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1,
  0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2, 0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2, 0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1, 0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1,
  0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2, 0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1, 0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2, 0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0,
  0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0, 0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0, 0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0, 0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1,
  0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1, 0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2, 0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1, 0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2,
  0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1, 0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1, 0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1, 0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1,
  0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2, 0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1, 0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2, 0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2,
  0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2, 0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2, 0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2, 0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2,
  0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2, 0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2, 0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2, 0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2,
  0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1, 0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2, 0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2, 0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0,
]);

/** The texel whose index has one bit fewer (its top bit is implied 0): second subset of 2-subset partitions. */
const ANCHOR_2 = new Uint8Array([
  15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15, 15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
  15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6,  6, 2, 6, 8,15,15, 2, 2,15,15,15,15,15, 2, 2,15,
]);
/** Second and third subsets of 3-subset partitions. */
const ANCHOR_3A = new Uint8Array([
   3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3,  3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
   8,15, 3, 5, 6,10, 8,15,15, 3,15, 5,15,15,15,15,  3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3,
]);
const ANCHOR_3B = new Uint8Array([
  15, 8, 8, 3,15,15, 3, 8,15,15,15,15,15,15,15, 8, 15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
  15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8, 15, 3,15,15,15,15,15,15,15,15,15,15, 3,15,15, 8,
]);

const WEIGHTS_2 = [0, 21, 43, 64];
const WEIGHTS_3 = [0, 9, 18, 27, 37, 46, 55, 64];
const WEIGHTS_4 = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];

/** Little-endian bit reader over a 16-byte block. */
class Bits {
  private _pos = 0;
  constructor(private readonly s: DataView, private readonly base: number) {}
  get pos(): number { return this._pos; }
  bit(): number {
    const p = this._pos++;
    return (this.s.getUint8(this.base + (p >> 3)) >> (p & 7)) & 1;
  }
  read(n: number): number {
    let v = 0;
    for (let i = 0; i < n; i++) v |= this.bit() << i;
    return v;
  }
}

function subsetOf(subsets: number, partition: number, texel: number): number {
  if (subsets === 1) return 0;
  return subsets === 2 ? PARTITIONS_2[partition * 16 + texel] : PARTITIONS_3[partition * 16 + texel];
}

function isAnchor(subsets: number, partition: number, texel: number): boolean {
  if (texel === 0) return true;
  if (subsets === 2) return ANCHOR_2[partition] === texel;
  if (subsets === 3) return ANCHOR_3A[partition] === texel || ANCHOR_3B[partition] === texel;
  return false;
}

// ---------------------------------------------------------------------------------------------
// BC7

interface Bc7Mode {
  subsets: number;
  partitionBits: number;
  rotationBits: number;
  indexSelectionBits: number;
  colorBits: number;
  alphaBits: number;
  /** 0: none, 1: one p-bit per endpoint, 2: one p-bit per subset (shared by its endpoints). */
  pBits: number;
  indexBits: number;
  index2Bits: number;
}

const BC7_MODES: Bc7Mode[] = [
  { subsets: 3, partitionBits: 4, rotationBits: 0, indexSelectionBits: 0, colorBits: 4, alphaBits: 0, pBits: 1, indexBits: 3, index2Bits: 0 },
  { subsets: 2, partitionBits: 6, rotationBits: 0, indexSelectionBits: 0, colorBits: 6, alphaBits: 0, pBits: 2, indexBits: 3, index2Bits: 0 },
  { subsets: 3, partitionBits: 6, rotationBits: 0, indexSelectionBits: 0, colorBits: 5, alphaBits: 0, pBits: 0, indexBits: 2, index2Bits: 0 },
  { subsets: 2, partitionBits: 6, rotationBits: 0, indexSelectionBits: 0, colorBits: 7, alphaBits: 0, pBits: 1, indexBits: 2, index2Bits: 0 },
  { subsets: 1, partitionBits: 0, rotationBits: 2, indexSelectionBits: 1, colorBits: 5, alphaBits: 6, pBits: 0, indexBits: 2, index2Bits: 3 },
  { subsets: 1, partitionBits: 0, rotationBits: 2, indexSelectionBits: 0, colorBits: 7, alphaBits: 8, pBits: 0, indexBits: 2, index2Bits: 2 },
  { subsets: 1, partitionBits: 0, rotationBits: 0, indexSelectionBits: 0, colorBits: 7, alphaBits: 7, pBits: 1, indexBits: 4, index2Bits: 0 },
  { subsets: 2, partitionBits: 6, rotationBits: 0, indexSelectionBits: 0, colorBits: 5, alphaBits: 5, pBits: 1, indexBits: 2, index2Bits: 0 },
];

/** An endpoint value of `bits` bits (p-bit already appended) widened to 8 by bit replication. */
function expand8(v: number, bits: number): number {
  return bits >= 8 ? v : (v << (8 - bits)) | (v >> (2 * bits - 8));
}

function weightsFor(bits: number): number[] {
  return bits === 2 ? WEIGHTS_2 : bits === 3 ? WEIGHTS_3 : WEIGHTS_4;
}

function lerp8(a: number, b: number, w: number): number {
  return ((64 - w) * a + w * b + 32) >> 6;
}

/** Decodes one BC7 block at `block` into 16 RGBA texels (0..1) in `px`. */
export function decodeBc7Block(s: DataView, block: number, px: Float32Array): void {
  const bits = new Bits(s, block);
  let mode = 0;
  while (mode < 8 && bits.bit() === 0) mode++;
  if (mode === 8) {
    // Reserved: the specification says a decoder returns transparent black.
    px.fill(0);
    return;
  }
  const m = BC7_MODES[mode];
  const partition = bits.read(m.partitionBits);
  const rotation = bits.read(m.rotationBits);
  const indexSelection = bits.read(m.indexSelectionBits);
  const endpoints = m.subsets * 2;
  // Endpoints as [endpoint][channel], channel-major in the stream: all reds, all greens...
  const e: number[][] = [];
  for (let i = 0; i < endpoints; i++) e.push([0, 0, 0, 255]);
  for (let c = 0; c < 3; c++) for (let i = 0; i < endpoints; i++) e[i][c] = bits.read(m.colorBits);
  if (m.alphaBits) for (let i = 0; i < endpoints; i++) e[i][3] = bits.read(m.alphaBits);
  // P-bits: a low bit shared by every channel of an endpoint (or of both endpoints of a subset).
  let colorBits = m.colorBits;
  let alphaBits = m.alphaBits;
  if (m.pBits) {
    for (let i = 0; i < endpoints; i++) {
      if (m.pBits === 2 && (i & 1)) continue;
      const p = bits.bit();
      const last = m.pBits === 2 ? i + 1 : i;
      for (let j = i; j <= last; j++) {
        for (let c = 0; c < 3; c++) e[j][c] = (e[j][c] << 1) | p;
        if (m.alphaBits) e[j][3] = (e[j][3] << 1) | p;
      }
    }
    colorBits++;
    if (m.alphaBits) alphaBits++;
  }
  for (let i = 0; i < endpoints; i++) {
    for (let c = 0; c < 3; c++) e[i][c] = expand8(e[i][c], colorBits);
    if (m.alphaBits) e[i][3] = expand8(e[i][3], alphaBits);
  }
  // Indices: the anchor texel of each subset has one bit fewer.
  const index1 = new Uint8Array(16);
  const index2 = new Uint8Array(16);
  for (let t = 0; t < 16; t++) index1[t] = bits.read(isAnchor(m.subsets, partition, t) ? m.indexBits - 1 : m.indexBits);
  if (m.index2Bits) for (let t = 0; t < 16; t++) index2[t] = bits.read(t === 0 ? m.index2Bits - 1 : m.index2Bits);

  const w1 = weightsFor(m.indexBits);
  const w2 = m.index2Bits ? weightsFor(m.index2Bits) : w1;
  for (let t = 0; t < 16; t++) {
    const subset = subsetOf(m.subsets, partition, t);
    const a = e[subset * 2];
    const b = e[subset * 2 + 1];
    // Mode 4 chooses which index set drives color and which alpha; mode 5 always splits them.
    let colorWeight: number;
    let alphaWeight: number;
    if (m.index2Bits) {
      if (indexSelection) { colorWeight = w2[index2[t]]; alphaWeight = w1[index1[t]]; }
      else { colorWeight = w1[index1[t]]; alphaWeight = w2[index2[t]]; }
    } else {
      colorWeight = alphaWeight = w1[index1[t]];
    }
    let r = lerp8(a[0], b[0], colorWeight);
    let g = lerp8(a[1], b[1], colorWeight);
    let bl = lerp8(a[2], b[2], colorWeight);
    let al = m.alphaBits ? lerp8(a[3], b[3], alphaWeight) : 255;
    // Rotation swaps alpha with one color channel, so alpha can carry the high-precision channel.
    if (rotation === 1) { const x = r; r = al; al = x; }
    else if (rotation === 2) { const x = g; g = al; al = x; }
    else if (rotation === 3) { const x = bl; bl = al; al = x; }
    px[t * 4] = r / 255; px[t * 4 + 1] = g / 255; px[t * 4 + 2] = bl / 255; px[t * 4 + 3] = al / 255;
  }
}

// ---------------------------------------------------------------------------------------------
// BC6H
//
// Fourteen modes, each a fixed scatter of endpoint bits over the 82 bits before the indices (77
// for the single-subset modes). The layouts below list, in stream order, which bit of which
// field each bit feeds: field ids 0..11 are r0 g0 b0 r1 g1 b1 r2 g2 b2 r3 g3 b3 (w x y z in the
// specification's naming), "P" the partition, and a number of consecutive bits per entry.

interface Bc6Mode {
  /** The 2- or 5-bit mode value. */
  code: number;
  subsets: number;
  /** Bits of the base endpoint (r0 g0 b0). */
  endpointBits: number;
  /** Bits of each delta, per channel; equal to endpointBits when not transformed. */
  deltaBits: [number, number, number];
  transformed: boolean;
  /** [field, lowBit, count] in stream order after the mode bits; field 12 is the partition, negative counts read the bits reversed. */
  layout: [number, number, number][];
}

const R0 = 0, G0 = 1, B0 = 2, R1 = 3, G1 = 4, B1 = 5, R2 = 6, G2 = 7, B2 = 8, R3 = 9, G3 = 10, B3 = 11, PART = 12;

const BC6_MODES: Bc6Mode[] = [
  { code: 0b00, subsets: 2, endpointBits: 10, deltaBits: [5, 5, 5], transformed: true, layout: [
    [G2, 4, 1], [B2, 4, 1], [B3, 4, 1], [R0, 0, 10], [G0, 0, 10], [B0, 0, 10], [R1, 0, 5], [G3, 4, 1], [G2, 0, 4], [G1, 0, 5], [B3, 0, 1], [G3, 0, 4],
    [B1, 0, 5], [B3, 1, 1], [B2, 0, 4], [R2, 0, 5], [B3, 2, 1], [R3, 0, 5], [B3, 3, 1], [PART, 0, 5] ] },
  { code: 0b01, subsets: 2, endpointBits: 7, deltaBits: [6, 6, 6], transformed: true, layout: [
    [G2, 5, 1], [G3, 4, 1], [G3, 5, 1], [R0, 0, 7], [B3, 0, 1], [B3, 1, 1], [B2, 4, 1], [G0, 0, 7], [B2, 5, 1], [B3, 2, 1], [G2, 4, 1], [B0, 0, 7],
    [B3, 3, 1], [B3, 5, 1], [B3, 4, 1], [R1, 0, 6], [G2, 0, 4], [G1, 0, 6], [G3, 0, 4], [B1, 0, 6], [B2, 0, 4], [R2, 0, 6], [R3, 0, 6], [PART, 0, 5] ] },
  { code: 0b00010, subsets: 2, endpointBits: 11, deltaBits: [5, 4, 4], transformed: true, layout: [
    [R0, 0, 10], [G0, 0, 10], [B0, 0, 10], [R1, 0, 5], [R0, 10, 1], [G2, 0, 4], [G1, 0, 4], [G0, 10, 1], [B3, 0, 1], [G3, 0, 4], [B1, 0, 4], [B0, 10, 1],
    [B3, 1, 1], [B2, 0, 4], [R2, 0, 5], [B3, 2, 1], [R3, 0, 5], [B3, 3, 1], [PART, 0, 5] ] },
  { code: 0b00110, subsets: 2, endpointBits: 11, deltaBits: [4, 5, 4], transformed: true, layout: [
    [R0, 0, 10], [G0, 0, 10], [B0, 0, 10], [R1, 0, 4], [R0, 10, 1], [G3, 4, 1], [G2, 0, 4], [G1, 0, 5], [G0, 10, 1], [G3, 0, 4], [B1, 0, 4], [B0, 10, 1],
    [B3, 1, 1], [B2, 0, 4], [R2, 0, 4], [B3, 0, 1], [B3, 2, 1], [R3, 0, 4], [G2, 4, 1], [B3, 3, 1], [PART, 0, 5] ] },
  { code: 0b01010, subsets: 2, endpointBits: 11, deltaBits: [4, 4, 5], transformed: true, layout: [
    [R0, 0, 10], [G0, 0, 10], [B0, 0, 10], [R1, 0, 4], [R0, 10, 1], [B2, 4, 1], [G2, 0, 4], [G1, 0, 4], [G0, 10, 1], [B3, 0, 1], [G3, 0, 4], [B1, 0, 5],
    [B0, 10, 1], [B2, 0, 4], [R2, 0, 4], [B3, 1, 1], [B3, 2, 1], [R3, 0, 4], [B3, 4, 1], [B3, 3, 1], [PART, 0, 5] ] },
  { code: 0b01110, subsets: 2, endpointBits: 9, deltaBits: [5, 5, 5], transformed: true, layout: [
    [R0, 0, 9], [B2, 4, 1], [G0, 0, 9], [G2, 4, 1], [B0, 0, 9], [B3, 4, 1], [R1, 0, 5], [G3, 4, 1], [G2, 0, 4], [G1, 0, 5], [B3, 0, 1], [G3, 0, 4],
    [B1, 0, 5], [B3, 1, 1], [B2, 0, 4], [R2, 0, 5], [B3, 2, 1], [R3, 0, 5], [B3, 3, 1], [PART, 0, 5] ] },
  { code: 0b10010, subsets: 2, endpointBits: 8, deltaBits: [6, 5, 5], transformed: true, layout: [
    [R0, 0, 8], [G3, 4, 1], [B2, 4, 1], [G0, 0, 8], [B3, 2, 1], [G2, 4, 1], [B0, 0, 8], [B3, 3, 1], [B3, 4, 1], [R1, 0, 6], [G2, 0, 4], [G1, 0, 5],
    [B3, 0, 1], [G3, 0, 4], [B1, 0, 5], [B3, 1, 1], [B2, 0, 4], [R2, 0, 6], [R3, 0, 6], [PART, 0, 5] ] },
  { code: 0b10110, subsets: 2, endpointBits: 8, deltaBits: [5, 6, 5], transformed: true, layout: [
    [R0, 0, 8], [B3, 0, 1], [B2, 4, 1], [G0, 0, 8], [G2, 5, 1], [G2, 4, 1], [B0, 0, 8], [G3, 5, 1], [B3, 4, 1], [R1, 0, 5], [G3, 4, 1], [G2, 0, 4],
    [G1, 0, 6], [G3, 0, 4], [B1, 0, 5], [B3, 1, 1], [B2, 0, 4], [R2, 0, 5], [B3, 2, 1], [R3, 0, 5], [B3, 3, 1], [PART, 0, 5] ] },
  { code: 0b11010, subsets: 2, endpointBits: 8, deltaBits: [5, 5, 6], transformed: true, layout: [
    [R0, 0, 8], [B3, 1, 1], [B2, 4, 1], [G0, 0, 8], [B2, 5, 1], [G2, 4, 1], [B0, 0, 8], [B3, 5, 1], [B3, 4, 1], [R1, 0, 5], [G3, 4, 1], [G2, 0, 4],
    [G1, 0, 5], [B3, 0, 1], [G3, 0, 4], [B1, 0, 6], [B2, 0, 4], [R2, 0, 5], [B3, 2, 1], [R3, 0, 5], [B3, 3, 1], [PART, 0, 5] ] },
  { code: 0b11110, subsets: 2, endpointBits: 6, deltaBits: [6, 6, 6], transformed: false, layout: [
    [R0, 0, 6], [G3, 4, 1], [B3, 0, 1], [B3, 1, 1], [B2, 4, 1], [G0, 0, 6], [G2, 5, 1], [B2, 5, 1], [B3, 2, 1], [G2, 4, 1], [B0, 0, 6], [G3, 5, 1],
    [B3, 3, 1], [B3, 5, 1], [B3, 4, 1], [R1, 0, 6], [G2, 0, 4], [G1, 0, 6], [G3, 0, 4], [B1, 0, 6], [B2, 0, 4], [R2, 0, 6], [R3, 0, 6], [PART, 0, 5] ] },
  { code: 0b00011, subsets: 1, endpointBits: 10, deltaBits: [10, 10, 10], transformed: false, layout: [
    [R0, 0, 10], [G0, 0, 10], [B0, 0, 10], [R1, 0, 10], [G1, 0, 10], [B1, 0, 10] ] },
  { code: 0b00111, subsets: 1, endpointBits: 11, deltaBits: [9, 9, 9], transformed: true, layout: [
    [R0, 0, 10], [G0, 0, 10], [B0, 0, 10], [R1, 0, 9], [R0, 10, 1], [G1, 0, 9], [G0, 10, 1], [B1, 0, 9], [B0, 10, 1] ] },
  { code: 0b01011, subsets: 1, endpointBits: 12, deltaBits: [8, 8, 8], transformed: true, layout: [
    [R0, 0, 10], [G0, 0, 10], [B0, 0, 10], [R1, 0, 8], [R0, 10, -2], [G1, 0, 8], [G0, 10, -2], [B1, 0, 8], [B0, 10, -2] ] },
  { code: 0b01111, subsets: 1, endpointBits: 16, deltaBits: [4, 4, 4], transformed: true, layout: [
    [R0, 0, 10], [G0, 0, 10], [B0, 0, 10], [R1, 0, 4], [R0, 10, -6], [G1, 0, 4], [G0, 10, -6], [B1, 0, 4], [B0, 10, -6] ] },
];

const BC6_BY_CODE = new Map<number, Bc6Mode>(BC6_MODES.map((m) => [m.code, m]));

function signExtend(v: number, bits: number): number {
  const shift = 32 - bits;
  return (v << shift) >> shift;
}

function unquantize(v: number, bits: number, signed: boolean): number {
  if (signed) {
    if (bits >= 16) return v;
    const negative = v < 0;
    const x = negative ? -v : v;
    let unq: number;
    if (x === 0) unq = 0;
    else if (x >= (1 << (bits - 1)) - 1) unq = 0x7fff;
    else unq = ((x << 15) + 0x4000) >> (bits - 1);
    return negative ? -unq : unq;
  }
  if (bits >= 15) return v;
  if (v === 0) return 0;
  if (v === (1 << bits) - 1) return 0xffff;
  return ((v << 15) + 0x4000) >> (bits - 1);
}

/** The interpolated value to a half-float bit pattern. */
function finishBc6(v: number, signed: boolean): number {
  // Signed results are sign-magnitude, the way a half-float is.
  if (signed) return v < 0 ? (((-v) * 31) >> 5) | 0x8000 : (v * 31) >> 5;
  return (v * 31) >> 6;
}

function halfBitsToFloat(h: number): number {
  const sign = (h & 0x8000) ? -1 : 1;
  const exp = (h >> 10) & 0x1f;
  const frac = h & 0x3ff;
  if (exp === 0) return sign * Math.pow(2, -14) * (frac / 1024);
  if (exp === 31) return frac ? NaN : sign * Infinity;
  return sign * Math.pow(2, exp - 15) * (1 + frac / 1024);
}

/** Decodes one BC6H block at `block` into 16 RGB float texels (alpha 1) in `px`. */
export function decodeBc6hBlock(s: DataView, block: number, px: Float32Array, signed: boolean): void {
  const bits = new Bits(s, block);
  let code = bits.read(2);
  if (code >= 2) code |= bits.read(3) << 2;
  const m = BC6_BY_CODE.get(code);
  if (!m) {
    // Reserved modes decode to zero.
    for (let t = 0; t < 16; t++) { px[t * 4] = 0; px[t * 4 + 1] = 0; px[t * 4 + 2] = 0; px[t * 4 + 3] = 1; }
    return;
  }
  const fields = new Int32Array(13);
  for (const [field, low, count] of m.layout) {
    if (count < 0) {
      // High bits stored top-down: the first stream bit is the field's highest.
      for (let i = -count - 1; i >= 0; i--) fields[field] |= bits.bit() << (low + i);
    } else {
      for (let i = 0; i < count; i++) fields[field] |= bits.bit() << (low + i);
    }
  }
  const partition = fields[PART];
  const epb = m.endpointBits;
  const endpoints = m.subsets * 2;
  const e: number[][] = [];
  for (let i = 0; i < endpoints; i++) e.push([fields[i * 3], fields[i * 3 + 1], fields[i * 3 + 2]]);
  // The base endpoint is sign-extended in signed images; deltas always are.
  if (signed) for (let c = 0; c < 3; c++) e[0][c] = signExtend(e[0][c], epb);
  if (m.transformed) {
    for (let i = 1; i < endpoints; i++) {
      for (let c = 0; c < 3; c++) {
        const delta = signExtend(e[i][c], m.deltaBits[c]);
        const sum = (e[0][c] + delta) & ((1 << epb) - 1);
        e[i][c] = signed ? signExtend(sum, epb) : sum;
      }
    }
  } else if (signed) {
    for (let i = 1; i < endpoints; i++) for (let c = 0; c < 3; c++) e[i][c] = signExtend(e[i][c], epb);
  }
  for (let i = 0; i < endpoints; i++) for (let c = 0; c < 3; c++) e[i][c] = unquantize(e[i][c], epb, signed);

  const indexBits = m.subsets === 1 ? 4 : 3;
  const weights = indexBits === 4 ? WEIGHTS_4 : WEIGHTS_3;
  for (let t = 0; t < 16; t++) {
    const anchor = t === 0 || (m.subsets === 2 && ANCHOR_2[partition] === t);
    const index = bits.read(anchor ? indexBits - 1 : indexBits);
    const w = weights[index];
    const subset = m.subsets === 2 ? PARTITIONS_2[partition * 16 + t] : 0;
    const a = e[subset * 2];
    const b = e[subset * 2 + 1];
    for (let c = 0; c < 3; c++) {
      const v = ((64 - w) * a[c] + w * b[c] + 32) >> 6;
      px[t * 4 + c] = halfBitsToFloat(finishBc6(v, signed));
    }
    px[t * 4 + 3] = 1;
  }
}
