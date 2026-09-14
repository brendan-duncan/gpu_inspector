// The types of Metal Shading Language, and how a value of one sits in a buffer's bytes.
//
// Types are interned in a table and referred to by index, because the debugger's shared interfaces
// (../debug/program.ts) pass a type as a number — SPIR-V passes a result type id there, and this
// passes a table index.
//
// The layout rules are C++'s, which is what makes MSL buffers different from SPIR-V's: there are no
// Offset or ArrayStride decorations to follow, every member simply sits at its natural alignment.
// Two details matter and are easy to get wrong:
//
//   * `float3` is four floats wide and sixteen-byte aligned, like `float4`. `packed_float3` is the
//     three-float, four-byte-aligned one. An engine that writes a tightly packed struct on the CPU
//     uses `packed_`, and reading it as the unpacked type shifts every member after it.
//   * `bool` is one byte in memory (SPIR-V's is four), so a struct of bools reads differently.
import { Dim } from "../debug/sampling.js";
import type { ScalarKind, Value } from "../debug/values.js";

export type AddressSpace = "thread" | "device" | "constant" | "threadgroup" | "ray_data" | "object_data";

export type ScalarBase =
  | "bool" | "char" | "uchar" | "short" | "ushort" | "int" | "uint" | "long" | "ulong" | "half" | "float";

export type TextureAccess = "sample" | "read" | "write" | "read_write";

/** What an entry point's argument or a struct member is tagged with. */
export interface Attribute {
  name: string;
  /** The attribute's arguments, as written: `[[buffer(2)]]` is { name: "buffer", args: [2] }. */
  args: number[];
  /** A non-numeric argument, as written: `[[user(locn3)]]`. */
  text?: string;
}

export type MslType =
  | { kind: "void" }
  | { kind: "scalar"; base: ScalarBase }
  | { kind: "vector"; element: number; count: number; packed: boolean }
  | { kind: "matrix"; column: number; columns: number }
  | { kind: "array"; element: number; length: number }
  | { kind: "struct"; name: string; members: StructMember[] }
  | { kind: "pointer"; pointee: number; space: AddressSpace; reference: boolean }
  | { kind: "texture"; dim: Dim; arrayed: boolean; depth: boolean; multisampled: boolean; sampled: number; access: TextureAccess }
  | { kind: "sampler" }
  | { kind: "atomic"; element: number }
  | { kind: "opaque"; name: string };

export interface StructMember {
  name: string;
  type: number;
  /** Everything in its `[[...]]`: a member can be both `[[user(locn0)]]` and `[[flat]]`. */
  attributes: Attribute[];
}

const SCALAR_BYTES: Record<ScalarBase, number> = {
  bool: 1, char: 1, uchar: 1, short: 2, ushort: 2, int: 4, uint: 4, long: 8, ulong: 8, half: 2, float: 4,
};

const SCALAR_KIND: Record<ScalarBase, ScalarKind> = {
  bool: { base: "bool", width: 1 },
  char: { base: "int", width: 8 },
  uchar: { base: "uint", width: 8 },
  short: { base: "int", width: 16 },
  ushort: { base: "uint", width: 16 },
  int: { base: "int", width: 32 },
  uint: { base: "uint", width: 32 },
  long: { base: "int", width: 64 },
  ulong: { base: "uint", width: 64 },
  half: { base: "float", width: 16 },
  float: { base: "float", width: 32 },
};

/** Scalar names an MSL shader may write, including the aliases for sized integers. */
const SCALAR_ALIASES: Record<string, ScalarBase> = {
  bool: "bool", char: "char", uchar: "uchar", short: "short", ushort: "ushort", int: "int", uint: "uint",
  long: "long", ulong: "ulong", half: "half", float: "float",
  int8_t: "char", uint8_t: "uchar", int16_t: "short", uint16_t: "ushort", int32_t: "int", uint32_t: "uint",
  int64_t: "long", uint64_t: "ulong", size_t: "ulong", ptrdiff_t: "long", uintptr_t: "ulong",
  signed: "int", unsigned: "uint", "unsigned int": "uint", double: "float",
};

/** `texture2d_array` and the rest, as their shape. */
const TEXTURE_SHAPES: Record<string, { dim: Dim; arrayed: boolean; depth: boolean; multisampled: boolean }> = {
  texture1d: { dim: Dim.D1, arrayed: false, depth: false, multisampled: false },
  texture1d_array: { dim: Dim.D1, arrayed: true, depth: false, multisampled: false },
  texture2d: { dim: Dim.D2, arrayed: false, depth: false, multisampled: false },
  texture2d_array: { dim: Dim.D2, arrayed: true, depth: false, multisampled: false },
  texture2d_ms: { dim: Dim.D2, arrayed: false, depth: false, multisampled: true },
  texture2d_ms_array: { dim: Dim.D2, arrayed: true, depth: false, multisampled: true },
  texture3d: { dim: Dim.D3, arrayed: false, depth: false, multisampled: false },
  texturecube: { dim: Dim.Cube, arrayed: false, depth: false, multisampled: false },
  texturecube_array: { dim: Dim.Cube, arrayed: true, depth: false, multisampled: false },
  texture_buffer: { dim: Dim.Buffer, arrayed: false, depth: false, multisampled: false },
  depth2d: { dim: Dim.D2, arrayed: false, depth: true, multisampled: false },
  depth2d_array: { dim: Dim.D2, arrayed: true, depth: true, multisampled: false },
  depth2d_ms: { dim: Dim.D2, arrayed: false, depth: true, multisampled: true },
  depth2d_ms_array: { dim: Dim.D2, arrayed: true, depth: true, multisampled: true },
  depthcube: { dim: Dim.Cube, arrayed: false, depth: true, multisampled: false },
  depthcube_array: { dim: Dim.Cube, arrayed: true, depth: true, multisampled: false },
};

export function isTextureName(name: string): boolean {
  return name in TEXTURE_SHAPES;
}

export class TypeTable {
  readonly types: MslType[] = [];
  private _byKey = new Map<string, number>();
  private _structsByName = new Map<string, number>();

  readonly void_: number;
  readonly bool: number;
  readonly int: number;
  readonly uint: number;
  readonly float: number;
  readonly sampler: number;

  constructor() {
    this.void_ = this.intern({ kind: "void" });
    this.bool = this.scalar("bool");
    this.int = this.scalar("int");
    this.uint = this.scalar("uint");
    this.float = this.scalar("float");
    this.sampler = this.intern({ kind: "sampler" });
  }

  get(ref: number): MslType | undefined {
    return this.types[ref];
  }

  intern(type: MslType): number {
    const key = keyOf(type);
    const hit = this._byKey.get(key);
    if (hit !== undefined) return hit;
    const ref = this.types.length;
    this.types.push(type);
    this._byKey.set(key, ref);
    if (type.kind === "struct" && type.name) this._structsByName.set(type.name, ref);
    return ref;
  }

  scalar(base: ScalarBase): number {
    return this.intern({ kind: "scalar", base });
  }

  vector(element: number, count: number, packed = false): number {
    return this.intern({ kind: "vector", element, count, packed });
  }

  matrix(column: number, columns: number): number {
    return this.intern({ kind: "matrix", column, columns });
  }

  array(element: number, length: number): number {
    return this.intern({ kind: "array", element, length });
  }

  pointer(pointee: number, space: AddressSpace, reference = false): number {
    return this.intern({ kind: "pointer", pointee, space, reference });
  }

  /** A struct declared in the shader; members are filled in after interning so a self-reference resolves. */
  declareStruct(name: string): number {
    const existing = this._structsByName.get(name);
    if (existing !== undefined) return existing;
    const ref = this.types.length;
    this.types.push({ kind: "struct", name, members: [] });
    this._byKey.set(`struct:${name || ref}`, ref);
    this._structsByName.set(name, ref);
    return ref;
  }

  structNamed(name: string): number | undefined {
    return this._structsByName.get(name);
  }

  /**
   * A built-in type spelled by name: a scalar, `float4`, `half2x3`, `packed_float3`. Returns
   * undefined for a name that is not one, so the parser can look it up as a declared type.
   */
  builtin(name: string): number | undefined {
    const scalar = SCALAR_ALIASES[name];
    if (scalar) return this.scalar(scalar);
    const packed = name.startsWith("packed_");
    const bare = packed ? name.slice(7) : name;
    // floatNxM: N columns of M rows, which is how MSL (and GLSL) read it.
    const matrix = /^([a-z0-9_]+?)(\d)x(\d)$/.exec(bare);
    if (matrix && SCALAR_ALIASES[matrix[1]]) {
      const element = this.scalar(SCALAR_ALIASES[matrix[1]]);
      return this.matrix(this.vector(element, Number(matrix[3]), packed), Number(matrix[2]));
    }
    const vector = /^([a-z0-9_]+?)(\d)$/.exec(bare);
    if (vector && SCALAR_ALIASES[vector[1]] && Number(vector[2]) >= 2 && Number(vector[2]) <= 4) {
      return this.vector(this.scalar(SCALAR_ALIASES[vector[1]]), Number(vector[2]), packed);
    }
    if (name === "sampler") return this.sampler;
    if (name === "void") return this.void_;
    return undefined;
  }

  /** A texture type: `texture2d<float, access::sample>`. */
  texture(name: string, sampled: number, access: TextureAccess): number | undefined {
    const shape = TEXTURE_SHAPES[name];
    if (!shape) return undefined;
    return this.intern({ kind: "texture", ...shape, sampled, access });
  }

  // -------------------------------------------------------------------------------------------
  // Naming

  name(ref: number): string {
    const t = this.types[ref];
    if (!t) return "?";
    switch (t.kind) {
      case "void": return "void";
      case "scalar": return t.base;
      case "vector": return `${t.packed ? "packed_" : ""}${this.name(t.element)}${t.count}`;
      case "matrix": {
        const column = this.types[t.column];
        const rows = column?.kind === "vector" ? column.count : 1;
        const element = column?.kind === "vector" ? column.element : t.column;
        return `${this.name(element)}${t.columns}x${rows}`;
      }
      case "array": return `array<${this.name(t.element)}, ${t.length < 0 ? "" : t.length}>`;
      case "struct": return t.name || "struct";
      case "pointer": return `${t.space} ${this.name(t.pointee)}${t.reference ? "&" : "*"}`;
      case "texture": {
        const base = Object.keys(TEXTURE_SHAPES).find((k) => {
          const s = TEXTURE_SHAPES[k];
          return s.dim === t.dim && s.arrayed === t.arrayed && s.depth === t.depth && s.multisampled === t.multisampled;
        });
        return `${base ?? "texture"}<${this.name(t.sampled)}>`;
      }
      case "sampler": return "sampler";
      case "atomic": return `atomic<${this.name(t.element)}>`;
      case "opaque": return t.name;
    }
  }

  // -------------------------------------------------------------------------------------------
  // Shape

  /** The scalar a type is made of (a vector's or matrix's element), or null for other types. */
  scalarOf(ref: number): ScalarKind | null {
    const t = this.types[ref];
    if (!t) return null;
    switch (t.kind) {
      case "scalar": return SCALAR_KIND[t.base];
      case "vector": return this.scalarOf(t.element);
      case "matrix": return this.scalarOf(t.column);
      case "atomic": return this.scalarOf(t.element);
      default: return null;
    }
  }

  scalarBase(ref: number): ScalarBase | null {
    const t = this.types[ref];
    if (!t) return null;
    if (t.kind === "scalar") return t.base;
    if (t.kind === "vector") return this.scalarBase(t.element);
    if (t.kind === "matrix") return this.scalarBase(t.column);
    if (t.kind === "atomic") return this.scalarBase(t.element);
    return null;
  }

  /** How many scalars a value of the type has across its columns (1 for a scalar, 3 for float3). */
  components(ref: number): number {
    const t = this.types[ref];
    if (t?.kind === "vector") return t.count;
    return 1;
  }

  /** The element type of a vector or matrix column, or the type itself when it is a scalar. */
  elementOf(ref: number): number {
    const t = this.types[ref];
    if (t?.kind === "vector") return t.element;
    if (t?.kind === "matrix") return t.column;
    return ref;
  }

  isFloat(ref: number): boolean {
    return this.scalarOf(ref)?.base === "float";
  }

  isSigned(ref: number): boolean {
    return this.scalarOf(ref)?.base === "int";
  }

  isBool(ref: number): boolean {
    return this.scalarOf(ref)?.base === "bool";
  }

  /** The same shape as `ref` but made of `base`: float3 with "bool" becomes bool3. */
  withScalar(ref: number, base: ScalarBase): number {
    const t = this.types[ref];
    const scalar = this.scalar(base);
    if (t?.kind === "vector") return this.vector(scalar, t.count, t.packed);
    if (t?.kind === "matrix") {
      const column = this.types[t.column];
      const rows = column?.kind === "vector" ? column.count : 1;
      return this.matrix(this.vector(scalar, rows, false), t.columns);
    }
    return scalar;
  }

  // -------------------------------------------------------------------------------------------
  // Layout (C++ rules, which is what MSL uses)

  /** Bytes a value of the type occupies in a buffer, its size rounded up to its alignment. */
  sizeOf(ref: number): number {
    const t = this.types[ref];
    if (!t) return 0;
    switch (t.kind) {
      case "void": return 0;
      case "scalar": return SCALAR_BYTES[t.base];
      case "vector": {
        const element = this.sizeOf(t.element);
        // float3 occupies four elements unless it is packed_float3.
        return t.packed ? element * t.count : element * (t.count === 3 ? 4 : t.count);
      }
      case "matrix": return this.sizeOf(t.column) * t.columns;
      case "array": return t.length < 0 ? 0 : this.sizeOf(t.element) * t.length;
      case "struct": {
        let at = 0;
        for (const m of t.members) {
          at = align(at, this.alignOf(m.type)) + this.sizeOf(m.type);
        }
        return align(at, this.alignOf(ref));
      }
      case "pointer": return 8;
      case "atomic": return this.sizeOf(t.element);
      default: return 0;   // textures and samplers are not in buffer memory
    }
  }

  alignOf(ref: number): number {
    const t = this.types[ref];
    if (!t) return 1;
    switch (t.kind) {
      case "scalar": return SCALAR_BYTES[t.base];
      case "vector": {
        const element = this.alignOf(t.element);
        return t.packed ? element : element * (t.count === 3 ? 4 : t.count);
      }
      case "matrix": return this.alignOf(t.column);
      case "array": return this.alignOf(t.element);
      case "struct": return t.members.reduce((a, m) => Math.max(a, this.alignOf(m.type)), 1);
      case "pointer": return 8;
      case "atomic": return this.alignOf(t.element);
      default: return 1;
    }
  }

  /** The byte offset of a struct's member. */
  memberOffset(ref: number, index: number): number {
    const t = this.types[ref];
    if (t?.kind !== "struct") return 0;
    let at = 0;
    for (let i = 0; i < t.members.length; i++) {
      at = align(at, this.alignOf(t.members[i].type));
      if (i === index) return at;
      at += this.sizeOf(t.members[i].type);
    }
    return at;
  }

  /** The stride between elements of an array or the columns of a matrix. */
  strideOf(ref: number): number {
    const t = this.types[ref];
    if (t?.kind === "array") return this.sizeOf(t.element);
    if (t?.kind === "matrix") return this.sizeOf(t.column);
    if (t?.kind === "vector") return this.sizeOf(t.element);
    return this.sizeOf(ref);
  }

  /**
   * Where an access path lands in a buffer: the byte offset and the type there. Null when the path
   * leaves the type (an index into a scalar), which the interpreter reports rather than guessing at.
   */
  locate(ref: number, path: number[]): { at: number; type: number } | null {
    let at = 0;
    let type = ref;
    for (const index of path) {
      const t = this.types[type];
      if (!t) return null;
      if (t.kind === "struct") {
        if (index < 0 || index >= t.members.length) return null;
        at += this.memberOffset(type, index);
        type = t.members[index].type;
      } else if (t.kind === "array") {
        at += index * this.sizeOf(t.element);
        type = t.element;
      } else if (t.kind === "matrix") {
        at += index * this.sizeOf(t.column);
        type = t.column;
      } else if (t.kind === "vector") {
        at += index * this.sizeOf(t.element);
        type = t.element;
      } else if (t.kind === "atomic") {
        type = t.element;
      } else {
        return null;
      }
    }
    return { at, type };
  }

  /** The length of an unsized array filling `bytes` from `offset` (a `device T*` the shader indexes). */
  unsizedLength(element: number, bytes: number, offset: number): number {
    const stride = this.sizeOf(element);
    return stride ? Math.max(0, Math.floor((bytes - offset) / stride)) : 0;
  }

  // -------------------------------------------------------------------------------------------
  // Reading and writing bytes

  read(view: DataView, at: number, ref: number, limit = Infinity): Value {
    const t = this.types[ref];
    if (!t) return 0;
    switch (t.kind) {
      case "scalar": return readScalar(view, at, t.base);
      case "atomic": return this.read(view, at, t.element, limit);
      case "vector": {
        const size = this.sizeOf(t.element);
        return Array.from({ length: t.count }, (_, i) => this.read(view, at + i * size, t.element, limit));
      }
      case "matrix": {
        const size = this.sizeOf(t.column);
        return Array.from({ length: t.columns }, (_, i) => this.read(view, at + i * size, t.column, limit));
      }
      case "array": {
        const size = this.sizeOf(t.element);
        const length = t.length < 0 ? this.unsizedLength(t.element, view.byteLength, at) : t.length;
        return Array.from({ length: Math.min(length, limit) }, (_, i) => this.read(view, at + i * size, t.element, limit));
      }
      case "struct":
        return t.members.map((m, i) => this.read(view, at + this.memberOffset(ref, i), m.type, limit));
      default:
        return null;
    }
  }

  write(view: DataView, at: number, ref: number, value: Value): void {
    const t = this.types[ref];
    if (!t) return;
    switch (t.kind) {
      case "scalar":
        writeScalar(view, at, t.base, value);
        return;
      case "atomic":
        this.write(view, at, t.element, value);
        return;
      case "vector": {
        const size = this.sizeOf(t.element);
        const values = Array.isArray(value) ? value : [value];
        for (let i = 0; i < t.count; i++) this.write(view, at + i * size, t.element, values[i] ?? 0);
        return;
      }
      case "matrix": {
        const size = this.sizeOf(t.column);
        const values = Array.isArray(value) ? value : [];
        for (let i = 0; i < t.columns; i++) this.write(view, at + i * size, t.column, values[i] ?? null);
        return;
      }
      case "array": {
        const size = this.sizeOf(t.element);
        const values = Array.isArray(value) ? value : [];
        const length = t.length < 0 ? values.length : t.length;
        for (let i = 0; i < length; i++) this.write(view, at + i * size, t.element, values[i] ?? null);
        return;
      }
      case "struct": {
        const values = Array.isArray(value) ? value : [];
        t.members.forEach((m, i) => this.write(view, at + this.memberOffset(ref, i), m.type, values[i] ?? null));
        return;
      }
      default:
        return;
    }
  }

  /** A value of the type with everything zero: what an uninitialized variable holds. */
  zero(ref: number): Value {
    const t = this.types[ref];
    if (!t) return 0;
    switch (t.kind) {
      case "scalar": return t.base === "bool" ? false : t.base === "long" || t.base === "ulong" ? 0n : 0;
      case "atomic": return this.zero(t.element);
      case "vector": return Array.from({ length: t.count }, () => this.zero(t.element));
      case "matrix": return Array.from({ length: t.columns }, () => this.zero(t.column));
      case "array": return Array.from({ length: Math.max(0, t.length) }, () => this.zero(t.element));
      case "struct": return t.members.map((m) => this.zero(m.type));
      default: return null;
    }
  }
}

function align(at: number, to: number): number {
  return to > 1 ? Math.ceil(at / to) * to : at;
}

function keyOf(type: MslType): string {
  switch (type.kind) {
    case "void": return "void";
    case "scalar": return `s:${type.base}`;
    case "vector": return `v:${type.element}:${type.count}:${type.packed ? 1 : 0}`;
    case "matrix": return `m:${type.column}:${type.columns}`;
    case "array": return `a:${type.element}:${type.length}`;
    case "struct": return `struct:${type.name}`;
    case "pointer": return `p:${type.pointee}:${type.space}:${type.reference ? 1 : 0}`;
    case "texture": return `t:${type.dim}:${type.arrayed ? 1 : 0}:${type.depth ? 1 : 0}:${type.multisampled ? 1 : 0}:${type.sampled}:${type.access}`;
    case "sampler": return "sampler";
    case "atomic": return `at:${type.element}`;
    case "opaque": return `o:${type.name}`;
  }
}

function readScalar(view: DataView, at: number, base: ScalarBase): Value {
  const size = SCALAR_BYTES[base];
  if (at < 0 || at + size > view.byteLength) return base === "bool" ? false : base === "long" || base === "ulong" ? 0n : 0;
  switch (base) {
    case "bool": return view.getUint8(at) !== 0;
    case "char": return view.getInt8(at);
    case "uchar": return view.getUint8(at);
    case "short": return view.getInt16(at, true);
    case "ushort": return view.getUint16(at, true);
    case "int": return view.getInt32(at, true);
    case "uint": return view.getUint32(at, true);
    case "long": return view.getBigInt64(at, true);
    case "ulong": return view.getBigUint64(at, true);
    case "half": return float16(view.getUint16(at, true));
    case "float": return view.getFloat32(at, true);
  }
}

function writeScalar(view: DataView, at: number, base: ScalarBase, value: Value): void {
  const size = SCALAR_BYTES[base];
  if (at < 0 || at + size > view.byteLength) return;
  const n = typeof value === "bigint" ? Number(value) : typeof value === "boolean" ? (value ? 1 : 0) : typeof value === "number" ? value : 0;
  switch (base) {
    case "bool": view.setUint8(at, value ? 1 : 0); return;
    case "char": view.setInt8(at, n | 0); return;
    case "uchar": view.setUint8(at, n & 0xff); return;
    case "short": view.setInt16(at, n | 0, true); return;
    case "ushort": view.setUint16(at, n & 0xffff, true); return;
    case "int": view.setInt32(at, n | 0, true); return;
    case "uint": view.setUint32(at, n >>> 0, true); return;
    case "long": view.setBigInt64(at, typeof value === "bigint" ? value : BigInt(Math.trunc(n)), true); return;
    case "ulong": view.setBigUint64(at, BigInt.asUintN(64, typeof value === "bigint" ? value : BigInt(Math.trunc(n))), true); return;
    case "half": view.setUint16(at, toFloat16(n), true); return;
    case "float": view.setFloat32(at, n, true); return;
  }
}

/** An IEEE half as a number. */
export function float16(h: number): number {
  const sign = h & 0x8000 ? -1 : 1;
  const e = (h >> 10) & 0x1f;
  const f = h & 0x3ff;
  if (e === 0) return sign * 2 ** -14 * (f / 1024);
  if (e === 31) return f ? NaN : sign * Infinity;
  return sign * 2 ** (e - 15) * (1 + f / 1024);
}

/** A number as IEEE half bits, rounding to nearest even the way the GPU does. */
export function toFloat16(value: number): number {
  if (Number.isNaN(value)) return 0x7e00;
  const sign = value < 0 || Object.is(value, -0) ? 0x8000 : 0;
  const a = Math.abs(value);
  if (a === Infinity) return sign | 0x7c00;
  if (a === 0) return sign;
  if (a >= 65520) return sign | 0x7c00;
  if (a < 2 ** -24) return sign;
  if (a < 2 ** -14) {
    // Subnormal.
    return sign | Math.round(a / 2 ** -24);
  }
  let e = Math.floor(Math.log2(a));
  let f = a / 2 ** e - 1;
  let mantissa = Math.round(f * 1024);
  if (mantissa === 1024) {
    e++;
    mantissa = 0;
  }
  if (e > 15) return sign | 0x7c00;
  return sign | ((e + 15) << 10) | mantissa;
}

export { Dim };
