// Values of the shader debugger's SPIR-V interpreter, and reading them out of buffer bytes.
//
// A scalar is a number (32-bit integers normalized to their type's signedness, floats rounded to
// their width), a bigint for 64-bit integers, or a boolean; vectors, matrices (arrays of columns),
// arrays and structs are JavaScript arrays. Pointers, images and samplers are objects of the
// classes below. Buffer-backed variables (uniform, storage and push constant blocks) are read from
// their bytes as they are accessed rather than all at once, so a storage buffer with a million
// elements costs only what the shader touches; a store puts its value over the bytes.
import { Decoration, type SpirvModule } from "./module.js";

export type Value = number | bigint | boolean | Value[] | Pointer | ImageValue | SamplerValue | SampledImageValue | null;

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

/** The scalar type a type is made of (a vector's or matrix's element), or null for other types. */
export function scalarOf(m: SpirvModule, typeId: number): ScalarKind | null {
  const t = m.types.get(typeId);
  if (!t) return null;
  switch (t.kind) {
    case "bool": return { base: "bool", width: 1 };
    case "int": return { base: t.signed ? "int" : "uint", width: t.width };
    case "float": return { base: "float", width: t.width };
    case "vector": return scalarOf(m, t.element);
    case "matrix": return scalarOf(m, t.column);
    default: return null;
  }
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

// ---------------------------------------------------------------------------------------------
// Buffer layout: reading a block out of its bytes

/** Bytes a type takes in a buffer (its size at its alignment: the last member's end, or stride times length). */
export function layoutSize(m: SpirvModule, typeId: number, bytes = 0, offset = 0): number {
  const t = m.types.get(typeId);
  if (!t) return 0;
  switch (t.kind) {
    case "bool": return 4;
    case "int":
    case "float": return t.width / 8;
    case "vector": return layoutSize(m, t.element) * t.count;
    case "matrix": return layoutSize(m, t.column) * t.count;
    case "array": {
      const stride = m.decoration(typeId, Decoration.ArrayStride)?.[0] ?? layoutSize(m, t.element);
      return stride * t.length;
    }
    case "runtimeArray": {
      const stride = m.decoration(typeId, Decoration.ArrayStride)?.[0] ?? layoutSize(m, t.element);
      return stride ? Math.max(0, Math.floor((bytes - offset) / stride)) * stride : 0;
    }
    case "struct": {
      let end = 0;
      t.members.forEach((member, i) => {
        const at = m.memberDecoration(typeId, i, Decoration.Offset)?.[0] ?? end;
        end = Math.max(end, at + layoutSize(m, member, bytes, offset + at));
      });
      return end;
    }
    default: return 0;
  }
}

/** The length of a runtime array at `offset` in `bytes`. */
export function runtimeArrayLength(m: SpirvModule, typeId: number, bytes: number, offset: number): number {
  const t = m.types.get(typeId);
  if (t?.kind !== "runtimeArray") return 0;
  const stride = m.decoration(typeId, Decoration.ArrayStride)?.[0] ?? layoutSize(m, t.element);
  return stride ? Math.max(0, Math.floor((bytes - offset) / stride)) : 0;
}

interface MatrixLayout { stride: number; rowMajor: boolean }

function readScalar(view: DataView, at: number, m: SpirvModule, typeId: number): Value {
  const t = m.types.get(typeId);
  if (!t || at < 0) return 0;
  const size = t.kind === "bool" ? 4 : t.kind === "int" || t.kind === "float" ? t.width / 8 : 0;
  if (at + size > view.byteLength) return t.kind === "bool" ? false : t.kind === "int" && t.width === 64 ? 0n : 0;
  if (t.kind === "bool") return view.getUint32(at, true) !== 0;
  if (t.kind === "float") {
    if (t.width === 64) return view.getFloat64(at, true);
    if (t.width === 16) {
      const h = view.getUint16(at, true);
      const sign = h & 0x8000 ? -1 : 1;
      const e = (h >> 10) & 0x1f;
      const f = h & 0x3ff;
      return e === 0 ? sign * 2 ** -14 * (f / 1024) : e === 31 ? (f ? NaN : sign * Infinity) : sign * 2 ** (e - 15) * (1 + f / 1024);
    }
    return view.getFloat32(at, true);
  }
  if (t.kind === "int") {
    if (t.width === 64) return t.signed ? view.getBigInt64(at, true) : view.getBigUint64(at, true);
    if (t.width === 16) return t.signed ? view.getInt16(at, true) : view.getUint16(at, true);
    if (t.width === 8) return t.signed ? view.getInt8(at) : view.getUint8(at);
    return t.signed ? view.getInt32(at, true) : view.getUint32(at, true);
  }
  return 0;
}

/** Reads a value of a type at an offset of a buffer block, following its Offset, ArrayStride and MatrixStride decorations. */
export function readBuffer(m: SpirvModule, view: DataView, at: number, typeId: number, matrix?: MatrixLayout, limit = Infinity): Value {
  const t = m.types.get(typeId);
  if (!t) return 0;
  switch (t.kind) {
    case "bool": case "int": case "float":
      return readScalar(view, at, m, typeId);
    case "vector": {
      const size = layoutSize(m, t.element);
      return Array.from({ length: t.count }, (_, i) => readScalar(view, at + i * size, m, t.element));
    }
    case "matrix": {
      const column = m.types.get(t.column);
      const rows = column?.kind === "vector" ? column.count : 1;
      const element = column?.kind === "vector" ? column.element : t.column;
      const scalar = layoutSize(m, element);
      const stride = matrix?.stride ?? rows * scalar;
      return Array.from({ length: t.count }, (_, c) => Array.from({ length: rows }, (_, r) =>
        readScalar(view, matrix?.rowMajor ? at + r * stride + c * scalar : at + c * stride + r * scalar, m, element)));
    }
    case "array":
    case "runtimeArray": {
      const stride = m.decoration(typeId, Decoration.ArrayStride)?.[0] ?? layoutSize(m, t.element);
      const length = t.kind === "array" ? t.length : runtimeArrayLength(m, typeId, view.byteLength, at);
      return Array.from({ length: Math.min(length, limit) }, (_, i) => readBuffer(m, view, at + i * stride, t.element, matrix, limit));
    }
    case "struct":
      return t.members.map((member, i) => readBuffer(m, view, at + (m.memberDecoration(typeId, i, Decoration.Offset)?.[0] ?? 0), member,
        memberMatrix(m, typeId, i), limit));
    default:
      return null;
  }
}

function memberMatrix(m: SpirvModule, struct: number, member: number): MatrixLayout | undefined {
  const stride = m.memberDecoration(struct, member, Decoration.MatrixStride)?.[0];
  if (stride === undefined) return undefined;
  return { stride, rowMajor: m.memberDecoration(struct, member, Decoration.RowMajor) !== undefined };
}

/**
 * Where an access path lands in a block: the byte offset, the type there, and the matrix layout in
 * effect. Null when the path leaves the block's types (an index into a scalar).
 */
export function bufferLocation(m: SpirvModule, blockType: number, path: number[], bytes: number): { at: number; type: number; matrix?: MatrixLayout } | null {
  let at = 0;
  let type = blockType;
  let matrix: MatrixLayout | undefined;
  for (const index of path) {
    const t = m.types.get(type);
    if (!t) return null;
    if (t.kind === "struct") {
      at += m.memberDecoration(type, index, Decoration.Offset)?.[0] ?? 0;
      matrix = memberMatrix(m, type, index) ?? matrix;
      type = t.members[index];
    } else if (t.kind === "array" || t.kind === "runtimeArray") {
      const stride = m.decoration(type, Decoration.ArrayStride)?.[0] ?? layoutSize(m, t.element, bytes, at);
      at += index * stride;
      type = t.element;
    } else if (t.kind === "matrix") {
      const column = m.types.get(t.column);
      const rows = column?.kind === "vector" ? column.count : 1;
      const element = column?.kind === "vector" ? column.element : t.column;
      const stride = matrix?.stride ?? rows * layoutSize(m, element);
      if (matrix?.rowMajor) {
        // A row-major column is not contiguous: its elements are read one by one by the caller.
        return null;
      }
      at += index * stride;
      type = t.column;
      matrix = undefined;
    } else if (t.kind === "vector") {
      at += index * layoutSize(m, t.element);
      type = t.element;
    } else {
      return null;
    }
  }
  return { at, type, matrix };
}
