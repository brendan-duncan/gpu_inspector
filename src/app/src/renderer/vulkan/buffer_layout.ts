// Buffer layouts written by hand, for the "Format" editor of the capture panel (WebGPU
// Inspector lets a buffer's type be overridden with WGSL; here the syntax is GLSL's):
//
//   struct Light { vec4 position; vec4 color; };
//   struct Uniforms { mat4 mvp; Light lights[4]; float time; uint flags; };
//
// The last struct is the buffer's type (or the one named like the reflected type). Offsets
// follow the std140 or std430 rules, which is how GLSL lays these blocks out; scalar/8-bit/16-bit
// types get their natural alignment. The reverse direction (layout -> text) seeds the editor.
import { typeName, type ArrayType, type MatrixType, type ReflType, type ScalarType, type StructType, type VectorType } from "./spirv_reflect.js";

export type LayoutRules = "std140" | "std430";

const SCALARS: Record<string, ScalarType> = {
  float: { kind: "scalar", base: "float", width: 32, size: 4 },
  double: { kind: "scalar", base: "float", width: 64, size: 8 },
  float16_t: { kind: "scalar", base: "float", width: 16, size: 2 },
  half: { kind: "scalar", base: "float", width: 16, size: 2 },
  int: { kind: "scalar", base: "int", width: 32, size: 4 },
  uint: { kind: "scalar", base: "uint", width: 32, size: 4 },
  bool: { kind: "scalar", base: "bool", width: 32, size: 4 },
  int8_t: { kind: "scalar", base: "int", width: 8, size: 1 },
  uint8_t: { kind: "scalar", base: "uint", width: 8, size: 1 },
  int16_t: { kind: "scalar", base: "int", width: 16, size: 2 },
  uint16_t: { kind: "scalar", base: "uint", width: 16, size: 2 },
  int64_t: { kind: "scalar", base: "int", width: 64, size: 8 },
  uint64_t: { kind: "scalar", base: "uint", width: 64, size: 8 },
};

const VEC_PREFIX: Record<string, string> = {
  vec: "float", dvec: "double", f16vec: "float16_t", ivec: "int", uvec: "uint", bvec: "bool",
  i8vec: "int8_t", u8vec: "uint8_t", i16vec: "int16_t", u16vec: "uint16_t", i64vec: "int64_t", u64vec: "uint64_t",
};
const MAT_PREFIX: Record<string, string> = { mat: "float", dmat: "double", f16mat: "float16_t" };

function alignUp(v: number, a: number): number {
  return a > 0 ? Math.ceil(v / a) * a : v;
}

/** Base alignment of a type under the given rules. */
function alignment(t: ReflType, rules: LayoutRules): number {
  switch (t.kind) {
    case "scalar": return t.size;
    case "vector": return (t.count === 3 ? 4 : t.count) * t.element.size;
    case "matrix": return rules === "std140" ? Math.max(16, t.stride) : t.stride;
    case "array": return rules === "std140" ? Math.max(16, alignment(t.element, rules)) : alignment(t.element, rules);
    case "struct": {
      let a = 1;
      for (const m of t.members) a = Math.max(a, alignment(m.type, rules));
      return rules === "std140" ? Math.max(16, a) : a;
    }
    default: return 4;
  }
}

/** Parses a type name (no array suffix) against the scalars, vectors, matrices and known structs. */
function parseTypeName(name: string, structs: Map<string, StructType>, rules: LayoutRules): ReflType | null {
  const s = SCALARS[name];
  if (s) return s;
  const st = structs.get(name);
  if (st) return st;
  const v = /^([a-z0-9]*vec)([234])$/.exec(name);
  if (v && VEC_PREFIX[v[1]]) {
    const element = SCALARS[VEC_PREFIX[v[1]]];
    const count = Number(v[2]);
    return { kind: "vector", element, count, size: count * element.size };
  }
  const m = /^([a-z0-9]*mat)([234])(?:x([234]))?$/.exec(name);
  if (m && MAT_PREFIX[m[1]]) {
    const element = SCALARS[MAT_PREFIX[m[1]]];
    const columns = Number(m[2]);
    const rows = m[3] ? Number(m[3]) : columns;
    const column: VectorType = { kind: "vector", element, count: rows, size: rows * element.size };
    let stride = alignment(column, rules);
    if (rules === "std140") stride = Math.max(16, stride);
    return { kind: "matrix", element, columns, rows, stride, rowMajor: false, size: columns * stride };
  }
  return null;
}

/**
 * Parses layout text into the buffer's struct type. Throws an Error with a message that names
 * the offending declaration when the text is malformed.
 */
export function parseLayout(text: string, rules: LayoutRules, preferredName?: string): StructType {
  const structs = new Map<string, StructType>();
  const src = text.replace(/\/\/[^\n]*/g, " ").replace(/\/\*[\s\S]*?\*\//g, " ");
  const re = /struct\s+([A-Za-z_]\w*)\s*\{([^}]*)\}\s*;?/g;
  let last: StructType | null = null;
  let match: RegExpExecArray | null;
  while ((match = re.exec(src))) {
    const name = match[1];
    const members: StructType["members"] = [];
    let offset = 0;
    let maxAlign = 1;
    for (const decl of match[2].split(";")) {
      const d = decl.trim();
      if (!d) continue;
      const dm = /^(?:layout\s*\([^)]*\)\s*)?(?:(?:highp|mediump|lowp)\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*((?:\[\s*\d*\s*\])*)$/.exec(d);
      if (!dm) throw new Error(`cannot parse "${d}"`);
      const base = parseTypeName(dm[1], structs, rules);
      if (!base) throw new Error(`unknown type "${dm[1]}" in "${d}"`);
      let type: ReflType = base;
      const dims = [...dm[3].matchAll(/\[\s*(\d*)\s*\]/g)].map((x) => (x[1] === "" ? 0 : Number(x[1]))).reverse();
      for (const count of dims) {
        const stride = alignUp(sizeOfType(type), alignment({ kind: "array", element: type, count, stride: 0, size: 0 }, rules));
        const arr: ArrayType = { kind: "array", element: type, count, stride, size: count * stride };
        type = arr;
      }
      const a = alignment(type, rules);
      offset = alignUp(offset, a);
      members.push({ name: dm[2], offset, type });
      offset += sizeOfType(type);
      maxAlign = Math.max(maxAlign, a);
    }
    const structAlign = rules === "std140" ? Math.max(16, maxAlign) : maxAlign;
    const st: StructType = { kind: "struct", name, members, size: alignUp(offset, structAlign) };
    structs.set(name, st);
    last = st;
  }
  if (!last) throw new Error("no struct declaration found");
  if (preferredName && structs.has(preferredName)) return structs.get(preferredName)!;
  return last;
}

function sizeOfType(t: ReflType): number {
  return t.kind === "opaque" ? 0 : t.size;
}

/** Writes a reflected type as layout text: nested structs first, the buffer's struct last. */
export function layoutText(type: ReflType, rootName = "Buffer"): string {
  const out: string[] = [];
  const names = new Map<StructType, string>();
  const usedNames = new Set<string>();
  const memberText = (t: ReflType): string => {
    const inner = innermost(t);
    if (inner.kind === "struct") return names.get(inner) ?? inner.name ?? "Struct";
    if (inner.kind === "matrix") return typeName(matrixColumnMajor(inner));
    return typeName(inner);
  };
  const emit = (s: StructType): void => {
    if (names.has(s)) return;
    // Nested structs are declared first (and get their names before they are referenced).
    for (const m of s.members) {
      const inner = innermost(m.type);
      if (inner.kind === "struct") emit(inner);
    }
    let name = s.name.replace(/[^A-Za-z0-9_]/g, "_") || "Struct";
    if (/^[0-9]/.test(name)) name = `_${name}`;
    while (usedNames.has(name)) name += "_";
    usedNames.add(name);
    names.set(s, name);
    const lines = s.members.map((m) => `    ${memberText(m.type)} ${m.name}${arraySuffix(m.type)};  // offset ${m.offset}`);
    out.push(`struct ${name} {\n${lines.join("\n")}\n};`);
  };
  if (type.kind === "struct") emit(type);
  else out.push(`struct ${rootName} {\n    ${memberText(type)} value${arraySuffix(type)};\n};`);
  return out.join("\n\n");
}

function innermost(t: ReflType): ReflType {
  return t.kind === "array" ? innermost(t.element) : t;
}

function matrixColumnMajor(m: MatrixType): MatrixType {
  return m.rowMajor ? { ...m, rowMajor: false } : m;
}

function arraySuffix(t: ReflType): string {
  let s = "";
  let cur: ReflType = t;
  while (cur.kind === "array") {
    s += `[${cur.count || ""}]`;
    cur = cur.element;
  }
  return s;
}
