// The Metal standard library, as far as a debugged shader needs it: the maths, the geometric and
// matrix operations, the relational ones, bit twiddling, atomics, derivatives and texture access.
//
// Texture reads go through ../debug/sampling.ts, which is the same code the SPIR-V interpreter
// samples with — a captured Metal texture and a captured Vulkan image arrive in the same
// DebugTexture, so `source.sample(smp, uv)` and `texture(sampler2D, uv)` give the same answer.
//
// A function that is not here returns its first argument and adds a warning, so a shader that uses
// something unimplemented still runs to the end and says what was wrong rather than stopping.
import { fetch, gather, implicitLod, sample, Dim } from "../debug/sampling.js";
import {
  ImageValue, OpaqueValue, SampledImageValue, SamplerValue, mapScalars, zipScalars,
  type DebugSampler, type DebugTexture, type Value,
} from "../debug/values.js";
import type { TypeTable } from "./types.js";
import type { IntersectorHandle } from "./raytracing.js";

/** A builtin that cannot finish yet: its invocation waits for the rest of the pixel quad. */
export const BLOCKED = Symbol("blocked");

export interface BuiltinContext {
  types: TypeTable;
  resultType: number;
  argTypes: number[];
  warn(message: string): void;
  /** The screen-space derivatives of a value, or BLOCKED while the quad's other lanes catch up. */
  derivative(operand: Value): { dx: Value; dy: Value } | typeof BLOCKED;
  /** Reads and writes through a pointer value, for the atomics. */
  load(ptr: Value): Value;
  store(ptr: Value, value: Value): void;
}

function num(v: Value): number {
  return typeof v === "number" ? v : typeof v === "bigint" ? Number(v) : v === true ? 1 : 0;
}

function flat(v: Value): number[] {
  if (Array.isArray(v)) return v.flatMap(flat);
  return [num(v)];
}

/** Applies a scalar function across one, two or three values of the same shape (scalars broadcast). */
function map1(a: Value, f: (x: number) => number): Value {
  return mapScalars(a, (x) => f(num(x)));
}

function map2(a: Value, b: Value, f: (x: number, y: number) => number): Value {
  if (Array.isArray(a) && !Array.isArray(b)) return a.map((x) => map2(x, b, f));
  if (!Array.isArray(a) && Array.isArray(b)) return b.map((y) => map2(a, y, f));
  return zipScalars(a, b, (x, y) => f(num(x), num(y)));
}

function map3(a: Value, b: Value, c: Value, f: (x: number, y: number, z: number) => number): Value {
  const shape = Array.isArray(a) ? a : Array.isArray(b) ? b : c;
  if (!Array.isArray(shape)) return f(num(a), num(b), num(c));
  const at = (v: Value, i: number): Value => (Array.isArray(v) ? v[i] ?? 0 : v);
  return shape.map((_, i) => map3(at(a, i), at(b, i), at(c, i), f));
}

function dot(a: Value, b: Value): number {
  const x = flat(a), y = flat(b);
  let sum = 0;
  for (let i = 0; i < Math.min(x.length, y.length); i++) sum += x[i] * y[i];
  return sum;
}

function length(a: Value): number {
  return Math.sqrt(dot(a, a));
}

function normalize(a: Value): Value {
  const l = length(a);
  return l === 0 ? a : map1(a, (x) => x / l);
}

function clamp(x: number, lo: number, hi: number): number {
  return x < lo ? lo : x > hi ? hi : x;
}

function step(edge: number, x: number): number {
  return x < edge ? 0 : 1;
}

function smoothstep(e0: number, e1: number, x: number): number {
  const t = clamp(e1 === e0 ? 0 : (x - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
}

function sign(x: number): number {
  return x > 0 ? 1 : x < 0 ? -1 : Number.isNaN(x) ? 0 : x;
}

function fract(x: number): number {
  return x - Math.floor(x);
}

/** MSL's `%` on floats, which keeps the sign of the numerator. */
function fmod(x: number, y: number): number {
  return y === 0 ? NaN : x - y * Math.trunc(x / y);
}

const MATH1: Record<string, (x: number) => number> = {
  sqrt: Math.sqrt, rsqrt: (x) => 1 / Math.sqrt(x), fast_sqrt: Math.sqrt,
  sin: Math.sin, cos: Math.cos, tan: Math.tan, asin: Math.asin, acos: Math.acos, atan: Math.atan,
  sinh: Math.sinh, cosh: Math.cosh, tanh: Math.tanh, asinh: Math.asinh, acosh: Math.acosh, atanh: Math.atanh,
  sinpi: (x) => Math.sin(x * Math.PI), cospi: (x) => Math.cos(x * Math.PI), tanpi: (x) => Math.tan(x * Math.PI),
  exp: Math.exp, exp2: (x) => 2 ** x, exp10: (x) => 10 ** x,
  log: Math.log, log2: Math.log2, log10: Math.log10, log1p: Math.log1p, expm1: Math.expm1,
  floor: Math.floor, ceil: Math.ceil, trunc: Math.trunc, rint: Math.round, nearbyint: Math.round,
  round: (x) => Math.sign(x) * Math.round(Math.abs(x)),
  fract, sign, saturate: (x) => clamp(x, 0, 1), abs: Math.abs, fabs: Math.abs,
  cbrt: Math.cbrt, erf: erf, erfc: (x) => 1 - erf(x),
  degrees: (x) => (x * 180) / Math.PI, radians: (x) => (x * Math.PI) / 180,
  recip: (x) => 1 / x,
};

const MATH2: Record<string, (x: number, y: number) => number> = {
  pow: (x, y) => x ** y, powr: (x, y) => x ** y, atan2: Math.atan2, fmod, fdim: (x, y) => (x > y ? x - y : 0),
  copysign: (x, y) => (y < 0 || Object.is(y, -0) ? -Math.abs(x) : Math.abs(x)),
  ldexp: (x, y) => x * 2 ** y, hypot: Math.hypot, nextafter: (x, y) => (x === y ? y : x + Math.sign(y - x) * Number.EPSILON * Math.abs(x || 1)),
  step, powr_fast: (x, y) => x ** y,
};

/** `erf` is not in JavaScript; this is the Abramowitz and Stegun approximation shaders can live with. */
function erf(x: number): number {
  const s = Math.sign(x);
  const a = Math.abs(x);
  const t = 1 / (1 + 0.3275911 * a);
  const y = 1 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * Math.exp(-a * a);
  return s * y;
}

/**
 * Runs a standard library call. Returns undefined for a name that is not one, so the caller can
 * say so; BLOCKED when the call needs the pixel quad's other invocations first.
 */
export function callBuiltin(name: string, args: Value[], ctx: BuiltinContext): Value | undefined | typeof BLOCKED {
  const a = args[0], b = args[1], c = args[2];
  const types = ctx.types;

  if (name.startsWith("texture.")) return textureCall(name.slice(8), args, ctx);
  if (name.startsWith("rt.")) return rayCall(name.slice(3), args, ctx);

  const math1 = MATH1[name];
  if (math1 && args.length >= 1) {
    // abs of an integer stays an integer, so it is not put through the float path.
    if (name === "abs" && !types.isFloat(ctx.argTypes[0] ?? 0)) return mapScalars(a, (x) => (typeof x === "bigint" ? (x < 0n ? -x : x) : Math.abs(num(x))));
    return map1(a, math1);
  }
  const math2 = MATH2[name];
  if (math2 && args.length >= 2) return map2(a, b, math2);

  switch (name) {
    case "min": return args.slice(1).reduce((acc, x) => map2(acc, x, (p, q) => (Number.isNaN(p) ? q : Number.isNaN(q) ? p : Math.min(p, q))), a);
    case "max": return args.slice(1).reduce((acc, x) => map2(acc, x, (p, q) => (Number.isNaN(p) ? q : Number.isNaN(q) ? p : Math.max(p, q))), a);
    case "fmin": return map2(a, b, Math.min);
    case "fmax": return map2(a, b, Math.max);
    case "clamp": return map3(a, b, c, clamp);
    case "mix": return map3(a, b, c, (x, y, t) => x + (y - x) * t);
    case "lerp": return map3(a, b, c, (x, y, t) => x + (y - x) * t);
    case "smoothstep": return map3(a, b, c, smoothstep);
    case "fma": case "mad": return map3(a, b, c, (x, y, z) => x * y + z);
    case "select": return map3(a, b, c, (x, y, t) => (t ? y : x));
    case "absdiff": return map2(a, b, (x, y) => Math.abs(x - y));

    case "dot": return dot(a, b);
    case "length": case "fast_length": return length(a);
    case "length_squared": return dot(a, a);
    case "distance": case "fast_distance": return length(map2(a, b, (x, y) => x - y));
    case "distance_squared": {
      const d = map2(a, b, (x, y) => x - y);
      return dot(d, d);
    }
    case "normalize": case "fast_normalize": return normalize(a);
    case "cross": {
      const x = flat(a), y = flat(b);
      return [x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]];
    }
    case "reflect": {
      const d = 2 * dot(a, b);
      return map2(a, b, (i, n) => i - d * n);
    }
    case "refract": {
      const eta = num(c);
      const cosi = dot(b, a);
      const k = 1 - eta * eta * (1 - cosi * cosi);
      if (k < 0) return map1(a, () => 0);
      const scale = eta * cosi + Math.sqrt(k);
      return map2(a, b, (i, n) => eta * i - scale * n);
    }
    case "faceforward": return dot(c, b) < 0 ? a : map1(a, (x) => -x);

    case "all": return flat(a).every((x) => x !== 0);
    case "any": return flat(a).some((x) => x !== 0);
    case "isnan": return mapScalars(a, (x) => Number.isNaN(num(x)));
    case "isinf": return mapScalars(a, (x) => !Number.isFinite(num(x)) && !Number.isNaN(num(x)));
    case "isfinite": return mapScalars(a, (x) => Number.isFinite(num(x)));
    case "isnormal": return mapScalars(a, (x) => Number.isFinite(num(x)) && num(x) !== 0);
    case "signbit": return mapScalars(a, (x) => num(x) < 0 || Object.is(num(x), -0));

    case "transpose": {
      const m = Array.isArray(a) ? a : [];
      const rows = Array.isArray(m[0]) ? (m[0] as Value[]).length : 0;
      return Array.from({ length: rows }, (_, r) => m.map((col) => (Array.isArray(col) ? col[r] ?? 0 : 0)));
    }
    case "determinant": return determinant(a);
    case "matrix.multiply": {
      // Column-major: result column j is a * b's column j.
      const bm = Array.isArray(b) ? b : [];
      return bm.map((col) => matrixTimesVector(a, col));
    }
    case "matrix.times.vector": return matrixTimesVector(a, b);
    case "vector.times.matrix": {
      const bm = Array.isArray(b) ? b : [];
      return bm.map((col) => dot(a, col));
    }
    case "outer_product": {
      const y = flat(b);
      return y.map((s) => map1(a, (x) => x * s));
    }

    case "popcount": return mapScalars(a, (x) => bitCount(num(x) >>> 0));
    case "clz": return mapScalars(a, (x) => Math.clz32(num(x) >>> 0));
    case "ctz": return mapScalars(a, (x) => {
      const v = num(x) >>> 0;
      return v === 0 ? 32 : 31 - Math.clz32(v & -v);
    });
    case "reverse_bits": return mapScalars(a, (x) => reverseBits(num(x) >>> 0));
    case "rotate": return map2(a, b, (x, y) => {
      const n = y & 31;
      return ((x >>> 0) << n | (x >>> 0) >>> (32 - n || 32)) >>> 0;
    });
    case "extract_bits": return map3(a, b, c, (x, offset, count) => {
      if (count <= 0) return 0;
      const mask = count >= 32 ? 0xffffffff : ((1 << count) - 1) >>> 0;
      return ((x >>> offset) & mask) >>> 0;
    });
    case "insert_bits": {
      const offset = num(args[2]), count = num(args[3]);
      return map2(a, b, (x, y) => {
        if (count <= 0) return x;
        const mask = (count >= 32 ? 0xffffffff : ((1 << count) - 1) >>> 0) << offset;
        return (((x >>> 0) & ~mask) | (((y >>> 0) << offset) & mask)) >>> 0;
      });
    }

    // Packing: the conversions an engine uses to squeeze varyings and buffers.
    case "pack_float_to_unorm4x8": case "pack_half_to_unorm4x8": {
      const v = flat(a);
      let out = 0;
      for (let i = 0; i < 4; i++) out |= (Math.round(clamp(v[i] ?? 0, 0, 1) * 255) & 0xff) << (i * 8);
      return out >>> 0;
    }
    case "pack_float_to_snorm4x8": {
      const v = flat(a);
      let out = 0;
      for (let i = 0; i < 4; i++) out |= (Math.round(clamp(v[i] ?? 0, -1, 1) * 127) & 0xff) << (i * 8);
      return out >>> 0;
    }
    case "unpack_unorm4x8_to_float": {
      const v = num(a) >>> 0;
      return [0, 1, 2, 3].map((i) => ((v >>> (i * 8)) & 0xff) / 255);
    }
    case "unpack_snorm4x8_to_float": {
      const v = num(a) >>> 0;
      return [0, 1, 2, 3].map((i) => Math.max(-1, (((v >>> (i * 8)) & 0xff) << 24 >> 24) / 127));
    }

    // Derivatives: the pixel quad's other invocations have to reach the same point first.
    case "dfdx": case "dpdx": case "ddx": {
      const d = ctx.derivative(a);
      return d === BLOCKED ? BLOCKED : d.dx;
    }
    case "dfdy": case "dpdy": case "ddy": {
      const d = ctx.derivative(a);
      return d === BLOCKED ? BLOCKED : d.dy;
    }
    case "fwidth": {
      const d = ctx.derivative(a);
      if (d === BLOCKED) return BLOCKED;
      return map2(map1(d.dx, Math.abs), map1(d.dy, Math.abs), (x, y) => x + y);
    }

    // Atomics: one invocation, so the operation is simply read-modify-write.
    case "atomic_load_explicit": return ctx.load(a);
    case "atomic_store_explicit":
      ctx.store(a, b);
      return null;
    case "atomic_exchange_explicit": {
      const before = ctx.load(a);
      ctx.store(a, b);
      return before;
    }
    case "atomic_compare_exchange_weak_explicit": {
      // (object, expected, desired, ...): `expected` is a pointer, updated when the swap fails.
      const before = ctx.load(a);
      const expected = ctx.load(b);
      if (num(before) === num(expected)) {
        ctx.store(a, c);
        return true;
      }
      ctx.store(b, before);
      return false;
    }
    case "atomic_fetch_add_explicit": case "atomic_fetch_sub_explicit": case "atomic_fetch_and_explicit":
    case "atomic_fetch_or_explicit": case "atomic_fetch_xor_explicit": case "atomic_fetch_min_explicit":
    case "atomic_fetch_max_explicit": {
      const before = ctx.load(a);
      const x = num(before), y = num(b);
      const after = name === "atomic_fetch_add_explicit" ? x + y
        : name === "atomic_fetch_sub_explicit" ? x - y
        : name === "atomic_fetch_and_explicit" ? x & y
        : name === "atomic_fetch_or_explicit" ? x | y
        : name === "atomic_fetch_xor_explicit" ? x ^ y
        : name === "atomic_fetch_min_explicit" ? Math.min(x, y)
        : Math.max(x, y);
      ctx.store(a, after);
      return before;
    }

    // One invocation is its own threadgroup and its own simdgroup here.
    case "threadgroup_barrier": case "simdgroup_barrier": case "threadgroup_imageblock_barrier":
      return null;
    case "simd_is_first": case "quad_is_first":
      return true;
    case "simd_broadcast": case "simd_broadcast_first": case "simd_shuffle": case "simd_shuffle_up":
    case "simd_shuffle_down": case "simd_shuffle_xor": case "simd_shuffle_rotate_down":
    case "quad_broadcast": case "quad_shuffle": case "quad_shuffle_up": case "quad_shuffle_down": case "quad_shuffle_xor":
      ctx.warn(`${name} reads another thread's value, which a single debugged invocation does not have: its own is used`);
      return a;
    case "simd_sum": case "quad_sum": case "simd_product": case "quad_product":
    case "simd_min": case "simd_max": case "quad_min": case "quad_max":
      ctx.warn(`${name} reduces across threads, which a single debugged invocation cannot do: its own value is used`);
      return a;
    case "simd_all": case "quad_all": return flat(a).every((x) => x !== 0);
    case "simd_any": case "quad_any": return flat(a).some((x) => x !== 0);
    case "simd_ballot": return flat(a).some((x) => x !== 0) ? 1 : 0;
    case "simd_active_threads_mask": return 1;

    case "is_null_texture": return args[0] === null || (a instanceof ImageValue && !a.texture);
    case "modf": {
      // (x, &integral): the fractional part returned, the integral part written through.
      const whole = map1(a, Math.trunc);
      ctx.store(b, whole);
      return map2(a, whole, (x, w) => x - w);
    }
    case "frexp": case "sincos": {
      ctx.warn(`${name} is not implemented by the interpreter`);
      return a;
    }
    default:
      // `level(1.5)`, `bias(-0.5)`, `gradient2d(dx, dy)`: ordinary calls in the text that a
      // sampling call reads as its options.
      return sampleOption(name, args);
  }
}

function bitCount(v: number): number {
  let x = v >>> 0;
  x = x - ((x >>> 1) & 0x55555555);
  x = (x & 0x33333333) + ((x >>> 2) & 0x33333333);
  return (((x + (x >>> 4)) & 0x0f0f0f0f) * 0x01010101) >>> 24;
}

function reverseBits(v: number): number {
  let x = v >>> 0;
  x = ((x & 0x55555555) << 1) | ((x >>> 1) & 0x55555555);
  x = ((x & 0x33333333) << 2) | ((x >>> 2) & 0x33333333);
  x = ((x & 0x0f0f0f0f) << 4) | ((x >>> 4) & 0x0f0f0f0f);
  x = ((x & 0x00ff00ff) << 8) | ((x >>> 8) & 0x00ff00ff);
  return ((x << 16) | (x >>> 16)) >>> 0;
}

function matrixTimesVector(m: Value, v: Value): Value {
  const columns = Array.isArray(m) ? m : [];
  const x = flat(v);
  if (!columns.length) return v;
  const rows = Array.isArray(columns[0]) ? (columns[0] as Value[]).length : 1;
  const out = new Array<number>(rows).fill(0);
  columns.forEach((col, j) => {
    const c = flat(col);
    for (let r = 0; r < rows; r++) out[r] += c[r] * (x[j] ?? 0);
  });
  return out;
}

function determinant(m: Value): number {
  const columns = (Array.isArray(m) ? m : []).map(flat);
  const n = columns.length;
  if (n === 2) return columns[0][0] * columns[1][1] - columns[1][0] * columns[0][1];
  if (n === 3) {
    const [a, b, c] = columns;
    return a[0] * (b[1] * c[2] - c[1] * b[2]) - b[0] * (a[1] * c[2] - c[1] * a[2]) + c[0] * (a[1] * b[2] - b[1] * a[2]);
  }
  if (n === 4) {
    // Laplace expansion along the first column.
    let sum = 0;
    for (let i = 0; i < 4; i++) {
      const minor = columns.filter((_, j) => j !== 0).map((col) => col.filter((_, r) => r !== i));
      sum += (i % 2 ? -1 : 1) * columns[0][i] * determinant(minor);
    }
    return sum;
  }
  return n === 1 ? columns[0][0] : 0;
}

// ---------------------------------------------------------------------------------------------
// Ray tracing
//
// Every method on an `intersector` other than `intersect` is a *setter*: it says how the traversal
// that follows is to behave and returns nothing. So each one changes the intersector's handle in
// place, which the next `rayQuery` instruction reads (interpreter.ts). `intersect` itself is not
// here — it may have to call the shader's own intersection function, which a builtin cannot do.

/** A method on an intersector or an acceleration structure. */
function rayCall(method: string, args: Value[], ctx: BuiltinContext): Value | undefined {
  const handle = args[0] instanceof OpaqueValue ? args[0].handle as IntersectorHandle | undefined : undefined;
  // A setter called with no argument means "on": `accept_any_intersection()` is the same as
  // `accept_any_intersection(true)`.
  const on = (v: Value): boolean => v === undefined ? true : num(v) !== 0 || v === true;
  switch (method) {
    case "accept_any_intersection":
      if (handle) handle.options.acceptAny = on(args[1]);
      return null;
    case "assume_geometry_type": {
      // `geometry_type::triangle` is 0, `bounding_box` 1, `curve` 2 (lower.ts, RAY_TRACING_ENUMS).
      // `assume_geometry_type` is a promise about the scene, so the traversal takes it as one and
      // stops testing the kind the shader says is not there.
      const kind = Math.trunc(num(args[1]));
      if (handle) {
        handle.options.triangles = kind === 0;
        handle.options.boundingBoxes = kind === 1;
      }
      return null;
    }
    case "force_opacity":
      // `forced_opacity::opaque` (1) means no intersection function is called even for a box; the
      // traversal already refuses one for a geometry the build marked opaque, and this says the
      // same thing for the whole query.
      if (handle) handle.options.forceOpaque = Math.trunc(num(args[1])) === 1;
      return null;
    case "set_geometry_cull_mode":
    case "set_opacity_cull_mode":
    case "set_triangle_cull_mode":
    case "assume_identity_transforms":
    case "accept_first_intersection":
      if (method === "accept_first_intersection" && handle) handle.options.acceptAny = on(args[1]);
      else ctx.warn(`intersector::${method} is recorded but does not change what the traversal finds`);
      return null;
    case "get_max_levels":
      return 1;
    default:
      ctx.warn(`intersector::${method} is not something the interpreter knows: it does nothing`);
      return null;
  }
}

// ---------------------------------------------------------------------------------------------
// Textures

function textureOf(v: Value): { texture: DebugTexture | null; binding: string } | null {
  if (v instanceof ImageValue) return { texture: v.texture, binding: v.binding };
  if (v instanceof SampledImageValue) return { texture: v.image.texture, binding: v.image.binding };
  return null;
}

function samplerOf(v: Value): DebugSampler | null {
  if (v instanceof SamplerValue) return v.sampler;
  if (v instanceof SampledImageValue) return v.sampler.sampler;
  return null;
}

/** A texture method: `tex.sample(smp, uv)`, `tex.read(coord)`, `tex.get_width()`. */
function textureCall(method: string, args: Value[], ctx: BuiltinContext): Value | undefined | typeof BLOCKED {
  const image = textureOf(args[0]);
  const types = ctx.types;
  const type = types.get(ctx.argTypes[0] ?? 0);
  const shape = type?.kind === "texture" ? type : null;
  if (!image) {
    ctx.warn(`${method} was called on something that is not a texture`);
    return null;
  }
  const texture = image.texture;
  if (!texture && method.startsWith("get_")) return 0;
  if (!texture) ctx.warn(`${image.binding} was not captured, so reading it gives zero`);

  const dim = shape?.dim ?? Dim.D2;
  const arrayed = shape?.arrayed ?? false;
  const depth = shape?.depth ?? false;
  const coordinates = shape ? coordinateCount(dim, false) : 2;

  switch (method) {
    case "get_width": return texture ? Math.max(1, texture.width >> mipArg(args, 1)) : 0;
    case "get_height": return texture ? Math.max(1, texture.height >> mipArg(args, 1)) : 0;
    case "get_depth": return texture ? Math.max(1, texture.depth >> mipArg(args, 1)) : 0;
    case "get_num_mip_levels": return texture ? texture.mips : 0;
    case "get_array_size": return texture ? texture.layers : 0;
    case "get_num_samples": return 1;
    case "write": {
      // A shader writing a texture: kept beside the captured texels so a later read sees it.
      if (!texture) return null;
      const value = flat(args[1]);
      const coord = flat(args[2]);
      const mip = args.length > 3 ? num(args[3]) : 0;
      const layer = arrayed ? num(args[3] ?? 0) : 0;
      texture.writes = texture.writes ?? new Map();
      texture.writes.set(`${mip}/${layer}/${Math.floor(coord[0] ?? 0)}/${Math.floor(coord[1] ?? 0)}`, value);
      return null;
    }
    case "read": {
      const coord = flat(args[1]);
      const layer = arrayed ? num(args[2]) : 0;
      const mip = arrayed ? num(args[3] ?? 0) : num(args[2] ?? 0);
      const texel = fetch(texture, [...coord, layer], mip, dim, arrayed);
      return depth ? texel[0] : texel;
    }
    case "sample": case "sample_compare": {
      const sampler = samplerOf(args[1]);
      const coord = flat(args[2]);
      const compare = method === "sample_compare" ? num(args[3]) : undefined;
      let rest = method === "sample_compare" ? 4 : 3;
      const layer = arrayed ? num(args[rest++]) : 0;
      // `level(n)`, `bias(n)` and `gradient2d(dx, dy)` arrive as the trailing arguments.
      const options = readSampleOptions(args, rest);
      let lod = options.lod;
      if (lod === undefined && texture) {
        if (options.gradient) {
          lod = implicitLod(texture, options.gradient.dx, options.gradient.dy);
        } else {
          // An implicit level of detail needs the quad's derivatives of the coordinate.
          const d = ctx.derivative(args[2]);
          if (d === BLOCKED) return BLOCKED;
          lod = implicitLod(texture, flat(d.dx), flat(d.dy));
        }
      }
      // SPIR-V's convention, which sampling.ts follows: an array layer is the coordinate's last component.
      const full = arrayed ? [...coord.slice(0, coordinateCount(dim, false)), layer] : coord;
      const texel = sample(texture, sampler, {
        coord: full, dim, arrayed, lod: (lod ?? 0) + (options.bias ?? 0), dref: compare, offset: options.offset,
      });
      return depth || compare !== undefined ? texel[0] : texel;
    }
    case "gather": case "gather_compare": {
      const sampler = samplerOf(args[1]);
      const coord = flat(args[2]);
      const compare = method === "gather_compare" ? num(args[3]) : undefined;
      const component = method === "gather" ? componentArg(args) : 0;
      return gather(texture, sampler, [...coord.slice(0, coordinates), arrayed ? num(args[3]) : 0], component, compare);
    }
    default:
      ctx.warn(`texture.${method} is not implemented by the interpreter`);
      return null;
  }
}

function mipArg(args: Value[], at: number): number {
  const v = args[at];
  return typeof v === "number" ? Math.max(0, Math.trunc(v)) : 0;
}

function componentArg(args: Value[]): number {
  // `component::y` reaches here as the string the parser kept, or as a number.
  const last = args[args.length - 1];
  if (typeof last === "number") return Math.max(0, Math.min(3, Math.trunc(last)));
  return 0;
}

function coordinateCount(dim: Dim, arrayed: boolean): number {
  const base = dim === Dim.D1 ? 1 : dim === Dim.D3 || dim === Dim.Cube ? 3 : 2;
  return base + (arrayed ? 1 : 0);
}

/** `level(1.5)`, `bias(-0.5)`, `gradient2d(dx, dy)`: the sampling options a call may end with. */
function readSampleOptions(args: Value[], from: number): { lod?: number; bias?: number; gradient?: { dx: number[]; dy: number[] }; offset?: number[] } {
  const out: { lod?: number; bias?: number; gradient?: { dx: number[]; dy: number[] }; offset?: number[] } = {};
  for (let i = from; i < args.length; i++) {
    const v = args[i];
    if (v instanceof OpaqueValue) {
      if (v.kind === "level") out.lod = num(v.values[0]);
      else if (v.kind === "bias") out.bias = num(v.values[0]);
      else if (v.kind === "gradient") out.gradient = { dx: flat(v.values[0]), dy: flat(v.values[1]) };
      continue;
    }
    // A bare trailing vector is the constant offset.
    if (Array.isArray(v)) out.offset = flat(v);
  }
  return out;
}

/** The sampling-option constructors, which are ordinary calls in the shader's text. */
export function sampleOption(name: string, args: Value[]): OpaqueValue | undefined {
  switch (name) {
    case "level": case "bias": case "min_lod_clamp": return new OpaqueValue(name, args);
    case "gradient2d": case "gradient3d": case "gradientcube": return new OpaqueValue("gradient", args);
    default: return undefined;
  }
}
