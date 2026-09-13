// Values of the shader debugger's interpreters, independent of the shading language they came from.
//
// A scalar is a number (32-bit integers normalized to their type's signedness, floats rounded to
// their width), a bigint for 64-bit integers, or a boolean; vectors, matrices (arrays of columns),
// arrays and structs are JavaScript arrays. Pointers, images and samplers are objects of the
// classes below. Buffer-backed variables (a uniform or storage block, an MSL `device` pointer) are
// read from their bytes as they are accessed rather than all at once, so a storage buffer with a
// million elements costs only what the shader touches; a store puts its value over the bytes.
//
// The SPIR-V interpreter (../spirv/) and the MSL interpreter (../msl/) both work in these values,
// which is what lets the debugger's UI, its stepping and the MCP tool read either one. A "type"
// here is a number each interpreter reads in its own type table: SPIR-V a result type id, MSL an
// index into the program's types. Only the DebugProgram (program.ts) turns one into a name.

export type Value = number | bigint | boolean | Value[] | Pointer | ImageValue | SamplerValue | SampledImageValue | OpaqueValue | null;

/**
 * Something a language has that is neither a number nor a composite and that only its own
 * interpreter reads: MSL's `level(1.5)` and `gradient2d(dx, dy)` sampling options, for instance.
 * Keeping one carrier here means a register can hold it without the shared value type growing a
 * case per language.
 */
export class OpaqueValue {
  constructor(readonly kind: string, readonly values: Value[]) {}
}

/** Where a variable's value lives. */
export interface Cell {
  value: Value;
  /** Buffer-backed: the bytes the value is read from, with the stores made over them. */
  buffer?: BufferStorage;
  /** An array of blocks: the value is one { buffer } per element. */
  bufferArray?: boolean;
}

export interface BufferStorage {
  bytes: Uint8Array;
  /** The block's type. */
  type: number;
  /** Values stored over the bytes, by access path ("0/3/1"). */
  overrides: Map<string, Value>;
}

export class Pointer {
  constructor(
    readonly cell: Cell,
    readonly path: number[],
    /** The type pointed at. */
    readonly type: number,
    readonly storage: number,
    /** The variable the pointer is into, for names and the watch view. */
    readonly variable: number,
  ) {}
}

/** An image a descriptor bound, with the captured texels the shader reads. */
export interface DebugTexture {
  width: number;
  height: number;
  depth: number;
  layers: number;
  /** Mip levels the capture holds, from `baseMip`. */
  mips: number;
  baseMip: number;
  format: string;
  /** UINT / SINT formats: fetched as integers, not normalized. */
  integer: boolean;
  /** RGBA per texel (a missing channel reads 0, alpha 1), for one level and layer; null where not captured. */
  level(mip: number, layer: number): { width: number; height: number; texels: Float32Array } | null;
  /** Texels a shader wrote (storage images), by "mip/layer/x/y". */
  writes?: Map<string, number[]>;
}

export interface DebugSampler {
  magFilter: "nearest" | "linear";
  minFilter: "nearest" | "linear";
  mipmapMode: "nearest" | "linear";
  address: ("repeat" | "mirror" | "clamp" | "border" | "mirrorClamp")[];
  border: number[];
  compareOp: string | null;
  minLod: number;
  maxLod: number;
  lodBias: number;
  unnormalized: boolean;
}

export class ImageValue {
  constructor(readonly texture: DebugTexture | null, readonly binding: string) {}
}

export class SamplerValue {
  constructor(readonly sampler: DebugSampler | null, readonly binding: string) {}
}

export class SampledImageValue {
  constructor(readonly image: ImageValue, readonly sampler: SamplerValue) {}
}

export function isComposite(v: Value): v is Value[] {
  return Array.isArray(v);
}

/** A deep copy of a value (a load or a store must not share arrays with the variable). */
export function cloneValue(v: Value): Value {
  return Array.isArray(v) ? v.map(cloneValue) : v;
}

// ---------------------------------------------------------------------------------------------
// Scalars

export interface ScalarKind {
  base: "float" | "int" | "uint" | "bool";
  width: number;
}

/** A scalar brought into its type's range: floats rounded to their width, integers wrapped to theirs. */
export function normalize(v: Value, s: ScalarKind): Value {
  if (s.base === "bool") return Boolean(v);
  if (s.base === "float") {
    const n = typeof v === "bigint" ? Number(v) : typeof v === "boolean" ? (v ? 1 : 0) : (v as number);
    return s.width === 32 ? Math.fround(n) : s.width === 16 ? Math.fround(n) : n;
  }
  if (s.width === 64) {
    const b = typeof v === "bigint" ? v : BigInt(Math.trunc(typeof v === "boolean" ? (v ? 1 : 0) : (v as number)));
    return s.base === "int" ? BigInt.asIntN(64, b) : BigInt.asUintN(64, b);
  }
  let n = typeof v === "bigint" ? Number(BigInt.asIntN(32, v)) : typeof v === "boolean" ? (v ? 1 : 0) : Math.trunc(v as number);
  if (s.width < 32) {
    const mod = 2 ** s.width;
    n = ((n % mod) + mod) % mod;
    return s.base === "int" && n >= mod / 2 ? n - mod : n;
  }
  return s.base === "int" ? n | 0 : n >>> 0;
}

/** Applies `f` to every scalar of a value (a scalar, or a vector's or matrix's elements). */
export function mapScalars(v: Value, f: (x: Value) => Value): Value {
  return Array.isArray(v) ? v.map((e) => mapScalars(e, f)) : f(v);
}

/** Applies `f` to matching scalars of two values of the same shape. */
export function zipScalars(a: Value, b: Value, f: (x: Value, y: Value) => Value): Value {
  if (Array.isArray(a)) return a.map((e, i) => zipScalars(e, Array.isArray(b) ? b[i] : b, f));
  return f(a, b);
}

/** Whether a value holds a NaN or an infinity. */
export function nonFinite(value: Value | undefined): boolean {
  if (typeof value === "number") return !Number.isFinite(value);
  return Array.isArray(value) && value.some(nonFinite);
}

/** Flattens a value's scalars to numbers (booleans 0 / 1), for comparisons. */
export function scalars(value: Value | undefined): number[] {
  if (Array.isArray(value)) return value.flatMap(scalars);
  if (typeof value === "number") return [value];
  if (typeof value === "bigint") return [Number(value)];
  if (typeof value === "boolean") return [value ? 1 : 0];
  return [];
}

/** One scalar as text: integers in full, floats to seven digits, NaN and infinities by name. */
export function scalarText(v: Value): string {
  if (typeof v === "number") {
    if (Number.isInteger(v)) return String(v);
    if (Number.isNaN(v)) return "NaN";
    if (!Number.isFinite(v)) return v > 0 ? "inf" : "-inf";
    const a = Math.abs(v);
    return a !== 0 && (a >= 1e7 || a < 1e-4) ? v.toExponential(4) : String(+v.toPrecision(7));
  }
  if (typeof v === "bigint") return String(v);
  if (typeof v === "boolean") return v ? "true" : "false";
  return "?";
}

/** An image or sampler value as text, for the watch and the values table. */
export function imageText(image: ImageValue): string {
  const t = image.texture;
  return t ? `${image.binding}: ${t.format.replace(/^VK_FORMAT_/, "")} ${t.width}x${t.height}${t.layers > 1 ? `x${t.layers}` : ""}` : `${image.binding}: not captured`;
}

export function samplerText(sampler: SamplerValue): string {
  const s = sampler.sampler;
  return s ? `${sampler.binding}: ${s.minFilter}/${s.magFilter} ${s.address[0]}${s.compareOp ? ` compare ${s.compareOp}` : ""}` : `${sampler.binding}: default sampler`;
}
