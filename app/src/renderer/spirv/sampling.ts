// Texture reads of the shader debugger's interpreter, on the CPU from the captured texels: what a
// sampler with its filters, address modes, level of detail and comparison gives, and texel fetches.
// The levels a capture did not read back are clamped to the ones it did.
import type { DebugSampler, DebugTexture } from "./values.js";

/** SPIR-V Dim: 1D, 2D, 3D, Cube, Rect, Buffer, SubpassData. */
export const enum Dim { D1 = 0, D2 = 1, D3 = 2, Cube = 3, Rect = 4, Buffer = 5, SubpassData = 6 }

const DEFAULT_SAMPLER: DebugSampler = {
  magFilter: "linear", minFilter: "linear", mipmapMode: "linear", address: ["repeat", "repeat", "repeat"], border: [0, 0, 0, 0],
  compareOp: null, minLod: 0, maxLod: 1000, lodBias: 0, unnormalized: false,
};

function wrap(i: number, size: number, mode: DebugSampler["address"][number]): number | null {
  switch (mode) {
    case "repeat": return ((i % size) + size) % size;
    case "mirror": {
      const period = size * 2;
      const t = ((i % period) + period) % period;
      return t < size ? t : period - 1 - t;
    }
    case "mirrorClamp": {
      const t = i < 0 ? -1 - i : i;
      return Math.min(t, size - 1);
    }
    case "border": return i < 0 || i >= size ? null : i;
    default: return Math.min(Math.max(i, 0), size - 1);
  }
}

function texel(level: { width: number; height: number; texels: Float32Array }, x: number, y: number, s: DebugSampler): number[] {
  const wx = wrap(x, level.width, s.address[0]);
  const wy = wrap(y, level.height, s.address[1]);
  if (wx === null || wy === null) return s.border.slice(0, 4);
  const o = (wy * level.width + wx) * 4;
  return [level.texels[o], level.texels[o + 1], level.texels[o + 2], level.texels[o + 3]];
}

function compare(op: string | null, reference: number, value: number): number {
  switch (op) {
    case "VK_COMPARE_OP_NEVER": return 0;
    case "VK_COMPARE_OP_LESS": return reference < value ? 1 : 0;
    case "VK_COMPARE_OP_EQUAL": return reference === value ? 1 : 0;
    case "VK_COMPARE_OP_LESS_OR_EQUAL": return reference <= value ? 1 : 0;
    case "VK_COMPARE_OP_GREATER": return reference > value ? 1 : 0;
    case "VK_COMPARE_OP_NOT_EQUAL": return reference !== value ? 1 : 0;
    case "VK_COMPARE_OP_GREATER_OR_EQUAL": return reference >= value ? 1 : 0;
    default: return 1;
  }
}

/** One level filtered at normalized coordinates. `dref` compares each tap before filtering. */
function filterLevel(level: { width: number; height: number; texels: Float32Array }, u: number, v: number, linear: boolean, s: DebugSampler, dref?: number): number[] {
  const x = s.unnormalized ? u : u * level.width;
  const y = s.unnormalized ? v : v * level.height;
  const tap = (tx: number, ty: number): number[] => {
    const t = texel(level, tx, ty, s);
    return dref === undefined ? t : [compare(s.compareOp, dref, t[0]), 0, 0, 1];
  };
  if (!linear) return tap(Math.floor(x), Math.floor(y));
  const fx = x - 0.5;
  const fy = y - 0.5;
  const x0 = Math.floor(fx);
  const y0 = Math.floor(fy);
  const ax = fx - x0;
  const ay = fy - y0;
  const t00 = tap(x0, y0), t10 = tap(x0 + 1, y0), t01 = tap(x0, y0 + 1), t11 = tap(x0 + 1, y0 + 1);
  return t00.map((c, i) => (c * (1 - ax) + t10[i] * ax) * (1 - ay) + (t01[i] * (1 - ax) + t11[i] * ax) * ay);
}

/** A cube direction's face (+X, -X, +Y, -Y, +Z, -Z) and its coordinates on the face. */
export function cubeFace(dir: number[]): { face: number; u: number; v: number } {
  const [x, y, z] = dir;
  const ax = Math.abs(x), ay = Math.abs(y), az = Math.abs(z);
  let face: number, sc: number, tc: number, ma: number;
  if (ax >= ay && ax >= az) {
    ma = ax;
    face = x >= 0 ? 0 : 1;
    sc = x >= 0 ? -z : z;
    tc = -y;
  } else if (ay >= az) {
    ma = ay;
    face = y >= 0 ? 2 : 3;
    sc = x;
    tc = y >= 0 ? z : -z;
  } else {
    ma = az;
    face = z >= 0 ? 4 : 5;
    sc = z >= 0 ? x : -x;
    tc = -y;
  }
  return { face, u: 0.5 * (sc / (ma || 1) + 1), v: 0.5 * (tc / (ma || 1) + 1) };
}

export interface SampleRequest {
  dim: number;
  arrayed: boolean;
  /** The coordinate as the shader passed it (a projective one already divided). */
  coord: number[];
  /** Level of detail before the sampler's bias and clamps. */
  lod: number;
  dref?: number;
  /** texelFetch-like offsets (ConstOffset / Offset operands). */
  offset?: number[];
}

/** The level of detail implicit sampling uses, from the coordinate's derivatives across the pixel quad. */
export function implicitLod(texture: DebugTexture, dx: number[], dy: number[]): number {
  const size = [texture.width, texture.height, texture.depth];
  let px = 0;
  let py = 0;
  for (let i = 0; i < Math.min(dx.length, 3); i++) {
    px += (dx[i] * size[i]) ** 2;
    py += (dy[i] * size[i]) ** 2;
  }
  const rho = Math.sqrt(Math.max(px, py));
  return rho > 0 ? Math.log2(rho) : -Infinity;
}

/** A sampled read: filtered, with mip levels blended as the sampler says. Returns RGBA. */
export function sample(texture: DebugTexture | null, sampler: DebugSampler | null, req: SampleRequest): number[] {
  if (!texture) return [0, 0, 0, 1];
  const s = sampler ?? DEFAULT_SAMPLER;
  let layer = 0;
  let u = req.coord[0] ?? 0;
  let v = req.dim === Dim.D1 ? 0.5 : req.coord[1] ?? 0;
  if (req.dim === Dim.Cube) {
    const f = cubeFace(req.coord);
    const cubeLayer = req.arrayed ? Math.max(0, Math.round(req.coord[3] ?? 0)) * 6 : 0;
    layer = cubeLayer + f.face;
    u = f.u;
    v = f.v;
  } else if (req.arrayed) {
    layer = Math.max(0, Math.round(req.dim === Dim.D1 ? req.coord[1] ?? 0 : req.coord[2] ?? 0));
  } else if (req.dim === Dim.D3) {
    // Nearest depth slice: 3D textures are held slice by slice.
    layer = Math.min(texture.depth - 1, Math.max(0, Math.floor((req.coord[2] ?? 0) * texture.depth)));
  }
  layer = Math.min(layer, Math.max(0, (req.dim === Dim.D3 ? texture.depth : texture.layers) - 1));
  if (req.offset && !s.unnormalized) {
    u += (req.offset[0] ?? 0) / texture.width;
    v += (req.offset[1] ?? 0) / texture.height;
  }

  const lod = Math.min(Math.max(req.lod + s.lodBias, s.minLod), s.maxLod);
  const magnified = !(lod > 0);
  const linear = magnified ? s.magFilter === "linear" : s.minFilter === "linear";
  const first = texture.baseMip;
  const last = first + texture.mips - 1;
  const clampLevel = (l: number): number => Math.min(Math.max(l, first), last);
  const at = Number.isFinite(lod) ? Math.max(0, lod) : 0;
  const read = (mip: number): number[] => {
    const level = texture.level(clampLevel(mip), layer);
    return level ? filterLevel(level, u, v, linear, s, req.dref) : [0, 0, 0, 1];
  };
  if (s.mipmapMode === "nearest" || Math.floor(at) === at) return read(Math.round(at));
  const lo = Math.floor(at);
  const t = at - lo;
  const a = read(lo);
  const b = read(lo + 1);
  return a.map((c, i) => c * (1 - t) + b[i] * t);
}

/** texelFetch: an unfiltered texel at integer coordinates and level; out of range reads zero. */
export function fetch(texture: DebugTexture | null, coord: number[], mip: number, dim: number, arrayed: boolean): number[] {
  if (!texture) return [0, 0, 0, 0];
  const layer = arrayed ? coord[dim === Dim.D1 ? 1 : 2] ?? 0 : dim === Dim.D3 ? coord[2] ?? 0 : 0;
  const key = `${mip}/${layer}/${coord[0]}/${coord[1] ?? 0}`;
  const written = texture.writes?.get(key);
  if (written) return written.slice(0, 4);
  const level = texture.level(texture.baseMip + Math.max(0, mip), layer);
  if (!level) return [0, 0, 0, 0];
  const x = coord[0] ?? 0;
  const y = dim === Dim.D1 ? 0 : coord[1] ?? 0;
  if (x < 0 || y < 0 || x >= level.width || y >= level.height) return [0, 0, 0, 0];
  const o = (y * level.width + x) * 4;
  return [level.texels[o], level.texels[o + 1], level.texels[o + 2], level.texels[o + 3]];
}

/** textureGather: one channel of the four texels a bilinear sample would read, in (i0 j1, i1 j1, i1 j0, i0 j0) order. */
export function gather(texture: DebugTexture | null, sampler: DebugSampler | null, coord: number[], component: number, dref?: number): number[] {
  if (!texture) return [0, 0, 0, 0];
  const s = sampler ?? DEFAULT_SAMPLER;
  const level = texture.level(texture.baseMip, 0);
  if (!level) return [0, 0, 0, 0];
  const fx = coord[0] * level.width - 0.5;
  const fy = coord[1] * level.height - 0.5;
  const x0 = Math.floor(fx);
  const y0 = Math.floor(fy);
  const pick = (x: number, y: number): number => {
    const t = texel(level, x, y, s);
    return dref === undefined ? t[component] : compare(s.compareOp, dref, t[0]);
  };
  return [pick(x0, y0 + 1), pick(x0 + 1, y0 + 1), pick(x0 + 1, y0), pick(x0, y0)];
}
