// PVRTC1 image decoding (Imagination's "PVRTC Texture Compression" specification), with the
// arithmetic of Imagination's own PVRTDecompress so results match it exactly. Unlike the other
// block formats a PVRTC texel depends on its neighbours: each 64-bit block holds two colours,
// and the colour of a texel is a bilinear blend of the colours of the four blocks whose centres
// surround it, then modulated between the two by the texel's own 1- or 2-bit value. Blocks are
// 4x4 texels at 4 bpp and 8x4 at 2 bpp, stored in Morton (Z) order, and the image wraps at its
// edges. So the whole image is decoded at once, into row-major RGBA floats.

interface Block {
  /** Colour A and B as 5-bit RGB with 4-bit alpha, the precision the blend runs at. */
  a: number[];
  b: number[];
  /** 0: the 1-bit (2 bpp) / 2-bit (4 bpp) values; 1: punchthrough (4 bpp) or interpolated in both directions (2 bpp); 2, 3: horizontal / vertical interpolation (2 bpp). */
  mode: number;
  /** Per texel (row-major within the block), the raw modulation value. */
  values: Uint8Array;
}

function unpackColors(word: number): [number[], number[]] {
  let b: number[];
  if (word & 0x80000000) {
    b = [(word >>> 26) & 31, (word >>> 21) & 31, (word >>> 16) & 31, 15];
  } else {
    const r = (word >>> 24) & 15;
    const g = (word >>> 20) & 15;
    const bl = (word >>> 16) & 15;
    b = [(r << 1) | (r >> 3), (g << 1) | (g >> 3), (bl << 1) | (bl >> 3), ((word >>> 28) & 7) << 1];
  }
  let a: number[];
  if (word & 0x8000) {
    const bl = (word >>> 1) & 15;
    a = [(word >>> 10) & 31, (word >>> 5) & 31, (bl << 1) | (bl >> 3), 15];
  } else {
    const r = (word >>> 8) & 15;
    const g = (word >>> 4) & 15;
    const bl = (word >>> 1) & 7;
    a = [(r << 1) | (r >> 3), (g << 1) | (g >> 3), (bl << 2) | (bl >> 1), ((word >>> 12) & 7) << 1];
  }
  return [a, b];
}

function unpackBlock(s: DataView, at: number, twoBpp: boolean): Block {
  let bits = s.getUint32(at, true);
  const colors = s.getUint32(at + 4, true);
  const [a, b] = unpackColors(colors);
  const flag = colors & 1;
  const values = new Uint8Array(32);
  let mode = 0;
  if (!twoBpp) {
    mode = flag;
    for (let i = 0; i < 16; i++) { values[i] = bits & 3; bits >>>= 2; }
  } else if (flag) {
    // A checkerboard of explicit 2-bit values; the rest are interpolated. Two bits of the
    // modulation word select the direction and are replaced by copies of their neighbours.
    mode = 1;
    if (bits & 1) {
      mode = (bits & (1 << 20)) ? 3 : 2;
      if (bits & (1 << 21)) bits |= 1 << 20; else bits &= ~(1 << 20);
    }
    if (bits & 2) bits |= 1; else bits &= ~1;
    for (let y = 0; y < 4; y++) {
      for (let x = 0; x < 8; x++) {
        if (((x ^ y) & 1) === 0) { values[y * 8 + x] = bits & 3; bits >>>= 2; }
      }
    }
  } else {
    for (let i = 0; i < 32; i++) { values[i] = (bits & 1) ? 3 : 0; bits >>>= 1; }
  }
  return { a, b, mode, values };
}

/** The Morton-order index of block (x, y) in a grid of w x h blocks: the smaller dimension interleaves, the rest is linear. */
function twiddle(w: number, h: number, x: number, y: number): number {
  const min = Math.min(w, h);
  let max = w > h ? x : y;
  let out = 0;
  let src = 1;
  let dst = 1;
  let shift = 0;
  while (src < min) {
    if (y & src) out |= dst;
    if (x & src) out |= dst << 1;
    src <<= 1;
    dst <<= 2;
    shift++;
  }
  max >>= shift;
  return out | (max << (2 * shift));
}

const WEIGHTS_STANDARD = [0, 3, 5, 8];
const WEIGHTS_PUNCHTHROUGH = [0, 4, 4, 8];

/**
 * Decodes a whole PVRTC1 image of `width` x `height` texels (each a power of two, at least a
 * block) into `values` (RGBA floats, row-major).
 */
export function decodePvrtc(s: DataView, width: number, height: number, twoBpp: boolean, values: Float32Array): void {
  const bw = twoBpp ? 8 : 4;
  const bh = 4;
  const blocksX = Math.max(1, width / bw);
  const blocksY = Math.max(1, height / bh);
  const blocks: Block[] = new Array(blocksX * blocksY);
  for (let by = 0; by < blocksY; by++) {
    for (let bx = 0; bx < blocksX; bx++) {
      const at = twiddle(blocksX, blocksY, bx, by) * 8;
      blocks[by * blocksX + bx] = at + 8 <= s.byteLength ? unpackBlock(s, at, twoBpp) : { a: [0, 0, 0, 15], b: [0, 0, 0, 15], mode: 0, values: new Uint8Array(32) };
    }
  }
  const blockAt = (bx: number, by: number): Block => blocks[((by % blocksY) + blocksY) % blocksY * blocksX + ((bx % blocksX) + blocksX) % blocksX];
  const rawValue = (x: number, y: number): number => {
    const xx = ((x % width) + width) % width;
    const yy = ((y % height) + height) % height;
    return blockAt(Math.floor(xx / bw), Math.floor(yy / bh)).values[(yy % bh) * bw + (xx % bw)];
  };
  // The blend weight (0..8 of colour B) of a texel, with the 2 bpp interpolation of the
  // texels off the checkerboard from their neighbours (which may sit in adjacent blocks).
  const modulation = (x: number, y: number, block: Block): { weight: number; punch: boolean } => {
    const v = block.values[(y % bh) * bw + (x % bw)];
    if (!twoBpp) {
      if (block.mode === 1) return { weight: WEIGHTS_PUNCHTHROUGH[v], punch: v === 2 };
      return { weight: WEIGHTS_STANDARD[v], punch: false };
    }
    if (block.mode === 0 || ((x ^ y) & 1) === 0) return { weight: WEIGHTS_STANDARD[v], punch: false };
    const left = WEIGHTS_STANDARD[rawValue(x - 1, y)];
    const right = WEIGHTS_STANDARD[rawValue(x + 1, y)];
    const up = WEIGHTS_STANDARD[rawValue(x, y - 1)];
    const down = WEIGHTS_STANDARD[rawValue(x, y + 1)];
    if (block.mode === 2) return { weight: (left + right + 1) >> 1, punch: false };
    if (block.mode === 3) return { weight: (up + down + 1) >> 1, punch: false };
    return { weight: (left + right + up + down + 2) >> 2, punch: false };
  };
  // The bilinear blend runs in fixed point scaled by the block area, then widens the 5- and
  // 4-bit channels to 8 by the shifts PVRTDecompress uses.
  const shift = twoBpp ? 5 : 4;
  const widen = (v: number, alpha: boolean): number =>
    alpha ? (v >> shift) + (v >> (shift - 4)) : (v >> (shift + 2)) + (v >> (shift - 3));

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      // The four blocks around the texel: the one whose centre is up-left of it and its neighbours.
      const px = x - (bw >> 1);
      const py = y - (bh >> 1);
      const bx = Math.floor(px / bw);
      const by = Math.floor(py / bh);
      const fx = px - bx * bw;
      const fy = py - by * bh;
      const p = blockAt(bx, by);
      const qb = blockAt(bx + 1, by);
      const r = blockAt(bx, by + 1);
      const sb = blockAt(bx + 1, by + 1);
      const { weight, punch } = modulation(x, y, blockAt(Math.floor(x / bw), Math.floor(y / bh)));
      const o = (y * width + x) * 4;
      for (let c = 0; c < 4; c++) {
        // Horizontal along the top and bottom block rows (scaled by the block width), then vertical (by 4).
        const top = (p.a[c] * bw + fx * (qb.a[c] - p.a[c]));
        const bottom = (r.a[c] * bw + fx * (sb.a[c] - r.a[c]));
        const ca = widen(top * 4 + fy * (bottom - top), c === 3);
        const topB = (p.b[c] * bw + fx * (qb.b[c] - p.b[c]));
        const bottomB = (r.b[c] * bw + fx * (sb.b[c] - r.b[c]));
        const cb = widen(topB * 4 + fy * (bottomB - topB), c === 3);
        let v = (ca * (8 - weight) + cb * weight) >> 3;
        if (punch && c === 3) v = 0;
        values[o + c] = v / 255;
      }
    }
  }
}
