// Reading SPIR-V values out of buffer bytes: the layout rules (Offset, ArrayStride, MatrixStride)
// a uniform, storage or push constant block is read through.
//
// The values themselves — scalars, composites, pointers, images and samplers — are the debugger's
// shared ones (../debug/values.ts), which the MSL interpreter uses too; they are re-exported here
// so everything SPIR-V keeps importing them from one place.
import { Decoration, type SpirvModule } from "./module.js";
import type { ScalarKind, Value } from "../debug/values.js";

export * from "../debug/values.js";

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
