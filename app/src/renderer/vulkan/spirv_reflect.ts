// SPIR-V reflection: entry points, stage inputs/outputs, descriptor resources and push constant
// blocks with their memory layout (member offsets, array and matrix strides). This is what
// WebGPU Inspector gets from wgsl_reflect; here it comes from the binary the application handed
// to vkCreateShaderModule, so uniform and storage buffer contents can be shown as typed values.
//
// Only the declarations are parsed (types, constants, decorations, variables, entry points);
// function bodies are skipped.

export type ScalarBase = "float" | "int" | "uint" | "bool";

export interface ScalarType { kind: "scalar"; base: ScalarBase; width: number; size: number }
export interface VectorType { kind: "vector"; element: ScalarType; count: number; size: number }
export interface MatrixType {
  kind: "matrix";
  element: ScalarType;
  columns: number;
  rows: number;
  /** Byte stride between columns (column-major) or rows (row-major). */
  stride: number;
  rowMajor: boolean;
  size: number;
}
export interface ArrayType { kind: "array"; element: ReflType; count: number; stride: number; size: number }
export interface StructMember { name: string; offset: number; type: ReflType }
export interface StructType { kind: "struct"; name: string; members: StructMember[]; size: number }
/** Images, samplers, acceleration structures: no memory layout. */
export interface OpaqueType { kind: "opaque"; name: string }
/** A vertex attribute: bytes laid out per a VkFormat (used by the buffer views, not SPIR-V). */
export interface FormatType { kind: "format"; format: string; size: number }
export type ReflType = ScalarType | VectorType | MatrixType | ArrayType | StructType | OpaqueType | FormatType;

export type ResourceKind =
  | "uniform" | "storage" | "pushConstant"
  | "sampledImage" | "combinedImageSampler" | "storageImage" | "sampler"
  | "uniformTexelBuffer" | "storageTexelBuffer" | "inputAttachment" | "accelerationStructure" | "unknown";

export interface ShaderResource {
  kind: ResourceKind;
  set: number;
  binding: number;
  name: string;
  typeName: string;
  type: ReflType;
  /** Array size for arrays of resources (1 otherwise, 0 for runtime-sized). */
  count: number;
  /** Storage buffers / images declared read-only or write-only. */
  readOnly: boolean;
  writeOnly: boolean;
}

export interface ShaderVariable {
  location: number;
  name: string;
  typeName: string;
  type: ReflType;
}

export type ShaderStage =
  | "vertex" | "tess_control" | "tess_eval" | "geometry" | "fragment" | "compute" | "task" | "mesh"
  | "raygen" | "intersection" | "any_hit" | "closest_hit" | "miss" | "callable" | "unknown";

export interface EntryPoint {
  name: string;
  stage: ShaderStage;
  inputs: ShaderVariable[];
  outputs: ShaderVariable[];
  workgroupSize: [number, number, number] | null;
}

export class ShaderReflection {
  entryPoints: EntryPoint[] = [];
  resources: ShaderResource[] = [];
  pushConstants: ShaderResource[] = [];
  /** SPIR-V version as "1.5". */
  version = "";

  findResource(set: number, binding: number): ShaderResource | null {
    return this.resources.find((r) => r.set === set && r.binding === binding) ?? null;
  }

  entryPoint(name?: string): EntryPoint | null {
    if (name) {
      const e = this.entryPoints.find((ep) => ep.name === name);
      if (e) return e;
    }
    return this.entryPoints[0] ?? null;
  }
}

// ---------------------------------------------------------------------------------------------
// Type names, GLSL style (the language Vulkan shaders are usually written in)

function scalarName(s: ScalarType): string {
  switch (s.base) {
    case "bool": return "bool";
    case "float": return s.width === 16 ? "float16_t" : s.width === 64 ? "double" : "float";
    case "int": return s.width === 32 ? "int" : `int${s.width}_t`;
    case "uint": return s.width === 32 ? "uint" : `uint${s.width}_t`;
  }
}

function vectorPrefix(s: ScalarType): string {
  switch (s.base) {
    case "bool": return "bvec";
    case "float": return s.width === 16 ? "f16vec" : s.width === 64 ? "dvec" : "vec";
    case "int": return s.width === 32 ? "ivec" : `i${s.width}vec`;
    case "uint": return s.width === 32 ? "uvec" : `u${s.width}vec`;
  }
}

export function typeName(t: ReflType | null | undefined): string {
  if (!t) return "";
  switch (t.kind) {
    case "scalar": return scalarName(t);
    case "vector": return `${vectorPrefix(t.element)}${t.count}`;
    case "matrix": {
      const p = t.element.width === 64 ? "dmat" : t.element.width === 16 ? "f16mat" : "mat";
      return t.columns === t.rows ? `${p}${t.columns}` : `${p}${t.columns}x${t.rows}`;
    }
    case "array": return `${typeName(t.element)}[${t.count || ""}]`;
    case "struct": return t.name || "struct";
    case "opaque": return t.name;
    case "format": return t.format.replace(/^VK_FORMAT_/, "");
  }
}

// ---------------------------------------------------------------------------------------------
// Parser

const enum Op {
  Name = 5, MemberName = 6, EntryPoint = 15, ExecutionMode = 16,
  TypeVoid = 19, TypeBool = 20, TypeInt = 21, TypeFloat = 22, TypeVector = 23, TypeMatrix = 24, TypeImage = 25,
  TypeSampler = 26, TypeSampledImage = 27, TypeArray = 28, TypeRuntimeArray = 29, TypeStruct = 30, TypePointer = 32,
  ConstantTrue = 41, ConstantFalse = 42, Constant = 43, SpecConstantTrue = 48, SpecConstantFalse = 49, SpecConstant = 50,
  Function = 54, Variable = 59, Decorate = 71, MemberDecorate = 72, ExecutionModeId = 331,
  TypeAccelerationStructureKHR = 5341,
}

const enum Dec {
  Block = 2, BufferBlock = 3, RowMajor = 4, ColMajor = 5, ArrayStride = 6, MatrixStride = 7, BuiltIn = 11,
  NonWritable = 24, NonReadable = 25, Location = 30, Binding = 33, DescriptorSet = 34, Offset = 35,
}

const enum StorageClass { UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, PushConstant = 9, StorageBuffer = 12 }

const STAGES: Record<number, ShaderStage> = {
  0: "vertex", 1: "tess_control", 2: "tess_eval", 3: "geometry", 4: "fragment", 5: "compute",
  5267: "task", 5268: "mesh", 5313: "raygen", 5314: "intersection", 5315: "any_hit", 5316: "closest_hit",
  5317: "miss", 5318: "callable", 5364: "task", 5365: "mesh",
};

interface RawType { op: Op; operands: number[] }
interface RawVariable { id: number; typeId: number; storageClass: number }
interface RawEntry { model: number; id: number; name: string; interfaces: number[] }

function readString(words: Uint32Array, start: number, end: number): { text: string; next: number } {
  const bytes: number[] = [];
  for (let i = start; i < end; i++) {
    const w = words[i];
    for (let b = 0; b < 4; b++) {
      const c = (w >>> (b * 8)) & 0xff;
      if (c === 0) return { text: new TextDecoder().decode(new Uint8Array(bytes)), next: i + 1 };
      bytes.push(c);
    }
  }
  return { text: new TextDecoder().decode(new Uint8Array(bytes)), next: end };
}

class Parser {
  names = new Map<number, string>();
  memberNames = new Map<number, Map<number, string>>();
  decorations = new Map<number, Map<number, number[]>>();
  memberDecorations = new Map<number, Map<number, Map<number, number[]>>>();
  types = new Map<number, RawType>();
  constants = new Map<number, number>();
  variables: RawVariable[] = [];
  entries: RawEntry[] = [];
  localSize = new Map<number, [number, number, number]>();
  localSizeIds = new Map<number, [number, number, number]>();
  private _cache = new Map<string, ReflType>();

  parse(words: Uint32Array): void {
    let i = 5;
    while (i < words.length) {
      const w = words[i];
      const op = w & 0xffff;
      const len = w >>> 16;
      if (len === 0) break;
      const end = Math.min(words.length, i + len);
      this._instruction(op as Op, words, i + 1, end);
      // Declarations all precede the first function; stop there.
      if (op === Op.Function) break;
      i += len;
    }
  }

  private _instruction(op: Op, words: Uint32Array, a: number, end: number): void {
    const operands = (): number[] => Array.from(words.subarray(a, end));
    switch (op) {
      case Op.Name:
        this.names.set(words[a], readString(words, a + 1, end).text);
        break;
      case Op.MemberName: {
        let m = this.memberNames.get(words[a]);
        if (!m) { m = new Map(); this.memberNames.set(words[a], m); }
        m.set(words[a + 1], readString(words, a + 2, end).text);
        break;
      }
      case Op.EntryPoint: {
        const s = readString(words, a + 2, end);
        this.entries.push({ model: words[a], id: words[a + 1], name: s.text, interfaces: Array.from(words.subarray(s.next, end)) });
        break;
      }
      case Op.ExecutionMode:
        if (words[a + 1] === 17) this.localSize.set(words[a], [words[a + 2], words[a + 3], words[a + 4]]);
        break;
      case Op.ExecutionModeId:
        if (words[a + 1] === 38) this.localSizeIds.set(words[a], [words[a + 2], words[a + 3], words[a + 4]]);
        break;
      case Op.TypeVoid: case Op.TypeBool: case Op.TypeInt: case Op.TypeFloat: case Op.TypeVector: case Op.TypeMatrix:
      case Op.TypeImage: case Op.TypeSampler: case Op.TypeSampledImage: case Op.TypeArray: case Op.TypeRuntimeArray:
      case Op.TypeStruct: case Op.TypePointer: case Op.TypeAccelerationStructureKHR:
        this.types.set(words[a], { op, operands: operands() });
        break;
      case Op.Constant: case Op.SpecConstant:
        // Only the low word matters for the array sizes this reflection needs.
        this.constants.set(words[a + 1], words[a + 2]);
        break;
      case Op.ConstantTrue: case Op.SpecConstantTrue:
        this.constants.set(words[a + 1], 1);
        break;
      case Op.ConstantFalse: case Op.SpecConstantFalse:
        this.constants.set(words[a + 1], 0);
        break;
      case Op.Variable:
        this.variables.push({ typeId: words[a], id: words[a + 1], storageClass: words[a + 2] });
        break;
      case Op.Decorate: {
        let m = this.decorations.get(words[a]);
        if (!m) { m = new Map(); this.decorations.set(words[a], m); }
        m.set(words[a + 1], Array.from(words.subarray(a + 2, end)));
        break;
      }
      case Op.MemberDecorate: {
        let s = this.memberDecorations.get(words[a]);
        if (!s) { s = new Map(); this.memberDecorations.set(words[a], s); }
        let m = s.get(words[a + 1]);
        if (!m) { m = new Map(); s.set(words[a + 1], m); }
        m.set(words[a + 2], Array.from(words.subarray(a + 3, end)));
        break;
      }
      default:
        break;
    }
  }

  decoration(id: number, dec: Dec): number[] | undefined {
    return this.decorations.get(id)?.get(dec);
  }

  memberDecoration(structId: number, member: number, dec: Dec): number[] | undefined {
    return this.memberDecorations.get(structId)?.get(member)?.get(dec);
  }

  hasMemberDecoration(structId: number, dec: Dec): boolean {
    const s = this.memberDecorations.get(structId);
    if (!s) return false;
    for (const m of s.values()) if (m.has(dec)) return true;
    return false;
  }

  allMembersDecorated(structId: number, memberCount: number, dec: Dec): boolean {
    if (memberCount === 0) return false;
    for (let i = 0; i < memberCount; i++) if (!this.memberDecoration(structId, i, dec)) return false;
    return true;
  }

  /** Follows pointers. */
  pointee(typeId: number): number {
    const t = this.types.get(typeId);
    return t && t.op === Op.TypePointer ? this.pointee(t.operands[2]) : typeId;
  }

  /** Strips array wrappers, returning the element type id and the total element count (0 = runtime). */
  unwrapArrays(typeId: number): { id: number; count: number } {
    let count = 1;
    let id = typeId;
    for (;;) {
      const t = this.types.get(id);
      if (!t) break;
      if (t.op === Op.TypeArray) {
        count *= this.constants.get(t.operands[2]) ?? 0;
        id = t.operands[1];
      } else if (t.op === Op.TypeRuntimeArray) {
        count = 0;
        id = t.operands[1];
      } else {
        break;
      }
    }
    return { id, count };
  }

  resolve(typeId: number, matrixStride = 0, rowMajor = false): ReflType {
    const key = `${typeId}:${matrixStride}:${rowMajor ? 1 : 0}`;
    const cached = this._cache.get(key);
    if (cached) return cached;
    const t = this._resolve(typeId, matrixStride, rowMajor);
    this._cache.set(key, t);
    return t;
  }

  private _resolve(typeId: number, matrixStride: number, rowMajor: boolean): ReflType {
    const t = this.types.get(typeId);
    if (!t) return { kind: "opaque", name: "?" };
    const o = t.operands;
    switch (t.op) {
      case Op.TypeBool: return { kind: "scalar", base: "bool", width: 32, size: 4 };
      case Op.TypeInt: return { kind: "scalar", base: o[2] ? "int" : "uint", width: o[1], size: o[1] / 8 };
      case Op.TypeFloat: return { kind: "scalar", base: "float", width: o[1], size: o[1] / 8 };
      case Op.TypeVector: {
        const e = this.resolve(o[1]);
        const element: ScalarType = e.kind === "scalar" ? e : { kind: "scalar", base: "float", width: 32, size: 4 };
        return { kind: "vector", element, count: o[2], size: o[2] * element.size };
      }
      case Op.TypeMatrix: {
        const col = this.resolve(o[1]);
        const column: VectorType = col.kind === "vector" ? col : { kind: "vector", element: { kind: "scalar", base: "float", width: 32, size: 4 }, count: 4, size: 16 };
        const columns = o[2];
        const rows = column.count;
        const vecLen = rowMajor ? columns : rows;
        const stride = matrixStride || (vecLen === 3 ? 4 : vecLen) * column.element.size;
        return { kind: "matrix", element: column.element, columns, rows, stride, rowMajor, size: (rowMajor ? rows : columns) * stride };
      }
      case Op.TypeArray: {
        const element = this.resolve(o[1], matrixStride, rowMajor);
        const count = this.constants.get(o[2]) ?? 0;
        const stride = this.decoration(typeId, Dec.ArrayStride)?.[0] ?? sizeOf(element);
        return { kind: "array", element, count, stride, size: count * stride };
      }
      case Op.TypeRuntimeArray: {
        const element = this.resolve(o[1], matrixStride, rowMajor);
        const stride = this.decoration(typeId, Dec.ArrayStride)?.[0] ?? sizeOf(element);
        return { kind: "array", element, count: 0, stride, size: 0 };
      }
      case Op.TypeStruct: {
        const members: StructMember[] = [];
        let running = 0;
        let size = 0;
        for (let i = 1; i < o.length; i++) {
          const m = i - 1;
          const ms = this.memberDecoration(typeId, m, Dec.MatrixStride)?.[0] ?? 0;
          const rm = this.memberDecoration(typeId, m, Dec.RowMajor) !== undefined;
          const type = this.resolve(o[i], ms, rm);
          const offset = this.memberDecoration(typeId, m, Dec.Offset)?.[0] ?? running;
          members.push({ name: this.memberNames.get(typeId)?.get(m) ?? `member${m}`, offset, type });
          running = offset + sizeOf(type);
          if (running > size) size = running;
        }
        return { kind: "struct", name: this.names.get(typeId) ?? "", members, size };
      }
      case Op.TypeImage: return { kind: "opaque", name: this.imageName(t, false) };
      case Op.TypeSampler: return { kind: "opaque", name: "sampler" };
      case Op.TypeSampledImage: {
        const img = this.types.get(o[1]);
        return { kind: "opaque", name: img && img.op === Op.TypeImage ? this.imageName(img, true) : "sampler" };
      }
      case Op.TypePointer: return this.resolve(o[2], matrixStride, rowMajor);
      case Op.TypeAccelerationStructureKHR: return { kind: "opaque", name: "accelerationStructureEXT" };
      default: return { kind: "opaque", name: "?" };
    }
  }

  imageName(t: RawType, combined: boolean): string {
    const o = t.operands;
    const sampled = this.resolve(o[1]);
    const dim = o[2];
    const depth = o[3] === 1;
    const arrayed = o[4] === 1;
    const ms = o[5] === 1;
    const storage = o[6] === 2;
    let prefix = "";
    if (sampled.kind === "scalar" && sampled.base === "int") prefix = "i";
    else if (sampled.kind === "scalar" && sampled.base === "uint") prefix = "u";
    if (dim === 6) return `${prefix}subpassInput${ms ? "MS" : ""}`;
    const base = storage ? "image" : combined ? "sampler" : "texture";
    const dims: Record<number, string> = { 0: "1D", 1: "2D", 2: "3D", 3: "Cube", 4: "2DRect", 5: "Buffer" };
    return `${prefix}${base}${dims[dim] ?? "2D"}${ms ? "MS" : ""}${arrayed ? "Array" : ""}${depth && combined ? "Shadow" : ""}`;
  }
}

export function sizeOf(t: ReflType): number {
  return t.kind === "opaque" ? 0 : t.size;
}

/** Parses a SPIR-V module. Returns null when the data is not SPIR-V. */
export function reflectSpirv(data: Uint8Array): ShaderReflection | null {
  if (data.byteLength < 20) return null;
  // Word-align a copy so a Uint32Array view is possible whatever the source offset.
  const bytes = new Uint8Array(data.byteLength & ~3);
  bytes.set(data.subarray(0, bytes.byteLength));
  const words = new Uint32Array(bytes.buffer);
  if (words[0] === 0x03022307) {
    // Big-endian module: swap every word.
    for (let i = 0; i < words.length; i++) {
      const w = words[i];
      words[i] = ((w & 0xff) << 24) | ((w & 0xff00) << 8) | ((w >>> 8) & 0xff00) | (w >>> 24);
    }
  }
  if (words[0] !== 0x07230203) return null;

  const p = new Parser();
  p.parse(words);

  const r = new ShaderReflection();
  r.version = `${(words[1] >>> 16) & 0xff}.${(words[1] >>> 8) & 0xff}`;

  const location = (id: number): number | undefined => p.decoration(id, Dec.Location)?.[0];
  const isBuiltIn = (v: RawVariable): boolean => {
    if (p.decoration(v.id, Dec.BuiltIn)) return true;
    const pointee = p.pointee(v.typeId);
    const t = p.types.get(pointee);
    return !!t && t.op === Op.TypeStruct && p.hasMemberDecoration(pointee, Dec.BuiltIn);
  };

  const ioVariable = (v: RawVariable): ShaderVariable | null => {
    if (isBuiltIn(v)) return null;
    const loc = location(v.id);
    if (loc === undefined) return null;
    const type = p.resolve(p.pointee(v.typeId));
    return { location: loc, name: p.names.get(v.id) ?? "", typeName: typeName(type), type };
  };

  for (const e of p.entries) {
    const inputs: ShaderVariable[] = [];
    const outputs: ShaderVariable[] = [];
    const inInterface = (id: number): boolean => e.interfaces.length === 0 || e.interfaces.includes(id);
    for (const v of p.variables) {
      if (!inInterface(v.id)) continue;
      if (v.storageClass === StorageClass.Input) {
        const io = ioVariable(v);
        if (io) inputs.push(io);
      } else if (v.storageClass === StorageClass.Output) {
        const io = ioVariable(v);
        if (io) outputs.push(io);
      }
    }
    inputs.sort((a, b) => a.location - b.location);
    outputs.sort((a, b) => a.location - b.location);
    let workgroupSize: [number, number, number] | null = p.localSize.get(e.id) ?? null;
    const ids = p.localSizeIds.get(e.id);
    if (!workgroupSize && ids) workgroupSize = [p.constants.get(ids[0]) ?? 1, p.constants.get(ids[1]) ?? 1, p.constants.get(ids[2]) ?? 1];
    r.entryPoints.push({ name: e.name, stage: STAGES[e.model] ?? "unknown", inputs, outputs, workgroupSize });
  }

  for (const v of p.variables) {
    const sc = v.storageClass;
    if (sc !== StorageClass.Uniform && sc !== StorageClass.StorageBuffer && sc !== StorageClass.UniformConstant && sc !== StorageClass.PushConstant) continue;
    const pointee = p.pointee(v.typeId);
    const inner = p.unwrapArrays(pointee);
    const innerType = p.types.get(inner.id);
    if (!innerType) continue;
    const type = p.resolve(inner.id);
    let kind: ResourceKind = "unknown";
    if (sc === StorageClass.PushConstant) kind = "pushConstant";
    else if (sc === StorageClass.StorageBuffer) kind = "storage";
    else if (sc === StorageClass.Uniform) kind = p.decoration(inner.id, Dec.BufferBlock) ? "storage" : "uniform";
    else {
      switch (innerType.op) {
        case Op.TypeSampledImage: kind = "combinedImageSampler"; break;
        case Op.TypeSampler: kind = "sampler"; break;
        case Op.TypeAccelerationStructureKHR: kind = "accelerationStructure"; break;
        case Op.TypeImage: {
          const dim = innerType.operands[2];
          const storage = innerType.operands[6] === 2;
          if (dim === 5) kind = storage ? "storageTexelBuffer" : "uniformTexelBuffer";
          else if (dim === 6) kind = "inputAttachment";
          else kind = storage ? "storageImage" : "sampledImage";
          break;
        }
        default: break;
      }
    }
    const memberCount = innerType.op === Op.TypeStruct ? innerType.operands.length - 1 : 0;
    const readOnly = !!p.decoration(v.id, Dec.NonWritable) || (memberCount > 0 && p.allMembersDecorated(inner.id, memberCount, Dec.NonWritable));
    const writeOnly = !!p.decoration(v.id, Dec.NonReadable) || (memberCount > 0 && p.allMembersDecorated(inner.id, memberCount, Dec.NonReadable));
    const structName = type.kind === "struct" ? type.name : "";
    const name = p.names.get(v.id) || structName || "";
    const res: ShaderResource = {
      kind,
      set: p.decoration(v.id, Dec.DescriptorSet)?.[0] ?? 0,
      binding: p.decoration(v.id, Dec.Binding)?.[0] ?? 0,
      name,
      typeName: structName || typeName(type),
      type,
      count: inner.count,
      readOnly,
      writeOnly,
    };
    if (kind === "pushConstant") r.pushConstants.push(res);
    else r.resources.push(res);
  }
  r.resources.sort((a, b) => a.set - b.set || a.binding - b.binding);
  return r;
}
