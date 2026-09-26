// A SPIR-V module parsed for execution by the shader debugger (interpreter.ts): every instruction
// kept in module order (its ordinal is what spirv_debug.ts's source locations and spirv-dis's
// output are keyed by), the types, constants, decorations and names resolved, and the functions
// split into blocks. Specialization constants take their default values here; the interpreter
// applies a pipeline's specialization before it runs.
import { parseSpirvDebugInfo, type SpirvDebugInfo } from "../vulkan/spirv_debug.js";

export const enum Op {
  Nop = 0, Undef = 1, Name = 5, MemberName = 6, String = 7, Line = 8, Extension = 10, ExtInstImport = 11, ExtInst = 12,
  EntryPoint = 15, ExecutionMode = 16, Capability = 17,
  TypeVoid = 19, TypeBool = 20, TypeInt = 21, TypeFloat = 22, TypeVector = 23, TypeMatrix = 24, TypeImage = 25, TypeSampler = 26,
  TypeSampledImage = 27, TypeArray = 28, TypeRuntimeArray = 29, TypeStruct = 30, TypeOpaque = 31, TypePointer = 32, TypeFunction = 33,
  TypeForwardPointer = 39,
  ConstantTrue = 41, ConstantFalse = 42, Constant = 43, ConstantComposite = 44, ConstantSampler = 45, ConstantNull = 46,
  SpecConstantTrue = 48, SpecConstantFalse = 49, SpecConstant = 50, SpecConstantComposite = 51, SpecConstantOp = 52,
  Function = 54, FunctionParameter = 55, FunctionEnd = 56, FunctionCall = 57,
  Variable = 59, ImageTexelPointer = 60, Load = 61, Store = 62, CopyMemory = 63, CopyMemorySized = 64, AccessChain = 65,
  InBoundsAccessChain = 66, PtrAccessChain = 67, ArrayLength = 68, InBoundsPtrAccessChain = 70,
  Decorate = 71, MemberDecorate = 72, DecorationGroup = 73, GroupDecorate = 74, GroupMemberDecorate = 75,
  VectorExtractDynamic = 77, VectorInsertDynamic = 78, VectorShuffle = 79, CompositeConstruct = 80, CompositeExtract = 81,
  CompositeInsert = 82, CopyObject = 83, Transpose = 84,
  SampledImage = 86, ImageSampleImplicitLod = 87, ImageSampleExplicitLod = 88, ImageSampleDrefImplicitLod = 89,
  ImageSampleDrefExplicitLod = 90, ImageSampleProjImplicitLod = 91, ImageSampleProjExplicitLod = 92,
  ImageSampleProjDrefImplicitLod = 93, ImageSampleProjDrefExplicitLod = 94, ImageFetch = 95, ImageGather = 96,
  ImageDrefGather = 97, ImageRead = 98, ImageWrite = 99, Image = 100, ImageQueryFormat = 101, ImageQueryOrder = 102,
  ImageQuerySizeLod = 103, ImageQuerySize = 104, ImageQueryLod = 105, ImageQueryLevels = 106, ImageQuerySamples = 107,
  ConvertFToU = 109, ConvertFToS = 110, ConvertSToF = 111, ConvertUToF = 112, UConvert = 113, SConvert = 114, FConvert = 115,
  QuantizeToF16 = 116, SatConvertSToU = 118, SatConvertUToS = 119, Bitcast = 124,
  SNegate = 126, FNegate = 127, IAdd = 128, FAdd = 129, ISub = 130, FSub = 131, IMul = 132, FMul = 133, UDiv = 134, SDiv = 135,
  FDiv = 136, UMod = 137, SRem = 138, SMod = 139, FRem = 140, FMod = 141, VectorTimesScalar = 142, MatrixTimesScalar = 143,
  VectorTimesMatrix = 144, MatrixTimesVector = 145, MatrixTimesMatrix = 146, OuterProduct = 147, Dot = 148, IAddCarry = 149,
  ISubBorrow = 150, UMulExtended = 151, SMulExtended = 152,
  Any = 154, All = 155, IsNan = 156, IsInf = 157, IsFinite = 158, IsNormal = 159, SignBitSet = 160, LessOrGreater = 161,
  Ordered = 162, Unordered = 163, LogicalEqual = 164, LogicalNotEqual = 165, LogicalOr = 166, LogicalAnd = 167, LogicalNot = 168,
  Select = 169, IEqual = 170, INotEqual = 171, UGreaterThan = 172, SGreaterThan = 173, UGreaterThanEqual = 174,
  SGreaterThanEqual = 175, ULessThan = 176, SLessThan = 177, ULessThanEqual = 178, SLessThanEqual = 179,
  FOrdEqual = 180, FUnordEqual = 181, FOrdNotEqual = 182, FUnordNotEqual = 183, FOrdLessThan = 184, FUnordLessThan = 185,
  FOrdGreaterThan = 186, FUnordGreaterThan = 187, FOrdLessThanEqual = 188, FUnordLessThanEqual = 189,
  FOrdGreaterThanEqual = 190, FUnordGreaterThanEqual = 191,
  ShiftRightLogical = 194, ShiftRightArithmetic = 195, ShiftLeftLogical = 196, BitwiseOr = 197, BitwiseXor = 198,
  BitwiseAnd = 199, Not = 200, BitFieldInsert = 201, BitFieldSExtract = 202, BitFieldUExtract = 203, BitReverse = 204,
  BitCount = 205,
  DPdx = 207, DPdy = 208, Fwidth = 209, DPdxFine = 210, DPdyFine = 211, FwidthFine = 212, DPdxCoarse = 213, DPdyCoarse = 214,
  FwidthCoarse = 215,
  EmitVertex = 218, EndPrimitive = 219, EmitStreamVertex = 220, EndStreamPrimitive = 221, ControlBarrier = 224, MemoryBarrier = 225,
  Phi = 245, LoopMerge = 246, SelectionMerge = 247, Label = 248, Branch = 249, BranchConditional = 250, Switch = 251,
  Kill = 252, Return = 253, ReturnValue = 254, Unreachable = 255,
  NoLine = 317, ModuleProcessed = 330, ExecutionModeId = 331, DecorateId = 332,
  CopyLogical = 400, TerminateInvocation = 4416, DemoteToHelperInvocation = 5380, IsHelperInvocation = 5381,
  DecorateString = 5632, MemberDecorateString = 5633,
}

export const enum Decoration {
  SpecId = 1, Block = 2, BufferBlock = 3, RowMajor = 4, ColMajor = 5, ArrayStride = 6, MatrixStride = 7, BuiltIn = 11,
  NoPerspective = 13, Flat = 14, Patch = 15, Centroid = 16, Location = 30, Component = 31, Binding = 33, DescriptorSet = 34, Offset = 35,
}

export const enum StorageClass {
  UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, Workgroup = 4, Private = 6, Function = 7, PushConstant = 9,
  Image = 11, StorageBuffer = 12, PhysicalStorageBuffer = 5349,
}

export const enum BuiltIn {
  Position = 0, PointSize = 1, ClipDistance = 3, CullDistance = 4, VertexId = 5, InstanceId = 6, PrimitiveId = 7, InvocationId = 8,
  Layer = 9, ViewportIndex = 10, TessLevelOuter = 11, TessLevelInner = 12, TessCoord = 13, PatchVertices = 14,
  FragCoord = 15, PointCoord = 16, FrontFacing = 17, SampleId = 18, SamplePosition = 19, SampleMask = 20, FragDepth = 22,
  HelperInvocation = 23, NumWorkgroups = 24, WorkgroupSize = 25, WorkgroupId = 26, LocalInvocationId = 27,
  GlobalInvocationId = 28, LocalInvocationIndex = 29, VertexIndex = 42, InstanceIndex = 43, BaseVertex = 4424,
  BaseInstance = 4425, DrawIndex = 4426, ViewIndex = 4440,
}

export const BUILTIN_NAMES: Record<number, string> = {
  0: "gl_Position", 1: "gl_PointSize", 3: "gl_ClipDistance", 4: "gl_CullDistance", 5: "gl_VertexID", 6: "gl_InstanceID",
  7: "gl_PrimitiveID", 8: "gl_InvocationID", 9: "gl_Layer", 10: "gl_ViewportIndex", 11: "gl_TessLevelOuter",
  12: "gl_TessLevelInner", 13: "gl_TessCoord", 14: "gl_PatchVerticesIn", 15: "gl_FragCoord", 16: "gl_PointCoord", 17: "gl_FrontFacing", 18: "gl_SampleID",
  19: "gl_SamplePosition", 20: "gl_SampleMask", 22: "gl_FragDepth", 23: "gl_HelperInvocation", 24: "gl_NumWorkGroups",
  25: "gl_WorkGroupSize", 26: "gl_WorkGroupID", 27: "gl_LocalInvocationID", 28: "gl_GlobalInvocationID",
  29: "gl_LocalInvocationIndex", 42: "gl_VertexIndex", 43: "gl_InstanceIndex", 4424: "gl_BaseVertex", 4425: "gl_BaseInstance",
  4426: "gl_DrawID", 4440: "gl_ViewIndex",
};

export const enum ExecutionModel { Vertex = 0, TessellationControl = 1, TessellationEvaluation = 2, Geometry = 3, Fragment = 4, GLCompute = 5 }

export type SpirvType =
  | { kind: "void" }
  | { kind: "bool" }
  | { kind: "int"; width: number; signed: boolean }
  | { kind: "float"; width: number }
  | { kind: "vector"; element: number; count: number }
  | { kind: "matrix"; column: number; count: number }
  | { kind: "array"; element: number; length: number; lengthId: number }
  | { kind: "runtimeArray"; element: number }
  | { kind: "struct"; members: number[] }
  | { kind: "pointer"; storage: number; pointee: number }
  | { kind: "function"; returnType: number; params: number[] }
  | { kind: "image"; sampled: number; dim: number; depth: number; arrayed: boolean; ms: boolean; usage: number; format: number }
  | { kind: "sampler" }
  | { kind: "sampledImage"; image: number }
  | { kind: "opaque"; name: string };

/** One instruction: its opcode, its operand words (after the opcode word) and its ordinal in the module. */
export interface Instruction {
  op: number;
  words: Uint32Array;
  index: number;
  /** Result type id, 0 when the instruction has none. */
  resultType: number;
  /** Result id, 0 when the instruction has none. */
  result: number;
}

export interface Block {
  label: number;
  /** Ordinal of the OpLabel instruction; execution starts at the instruction after it. */
  start: number;
  /** Ordinal of the block's terminator. */
  end: number;
}

export interface FunctionInfo {
  id: number;
  returnType: number;
  /** Ordinal of OpFunction. */
  start: number;
  params: { id: number; type: number }[];
  blocks: Block[];
  blockByLabel: Map<number, Block>;
}

export interface EntryPointInfo {
  model: number;
  function: number;
  name: string;
  interface: number[];
  /** Execution modes by mode number: their literal operands. */
  modes: Map<number, number[]>;
}

/** A decoration's operands, by decoration number. */
export type Decorations = Map<number, number[]>;

/** A literal string, and how many words it took. */
export function literalString(words: Uint32Array, start: number): { text: string; words: number } {
  const bytes: number[] = [];
  for (let i = start; i < words.length; i++) {
    const w = words[i];
    for (let b = 0; b < 4; b++) {
      const c = (w >>> (b * 8)) & 0xff;
      if (c === 0) return { text: new TextDecoder().decode(new Uint8Array(bytes)), words: i - start + 1 };
      bytes.push(c);
    }
  }
  return { text: new TextDecoder().decode(new Uint8Array(bytes)), words: words.length - start };
}

/** Opcodes whose first two operands are a result type and a result id. */
function hasResultTypeAndId(op: number): boolean {
  if (op === Op.Undef || op === Op.ExtInst || op === Op.FunctionParameter || op === Op.FunctionCall || op === Op.Variable) return true;
  if (op === Op.Function) return true;
  if (op >= Op.ConstantTrue && op <= Op.SpecConstantOp && op !== 47) return true;
  if (op >= Op.ImageTexelPointer && op <= Op.InBoundsPtrAccessChain && op !== Op.Store && op !== Op.CopyMemory && op !== Op.CopyMemorySized) return true;
  if (op >= Op.VectorExtractDynamic && op <= Op.Transpose) return true;
  if (op >= Op.SampledImage && op <= Op.ImageQuerySamples && op !== Op.ImageWrite) return true;
  if (op >= Op.ConvertFToU && op <= Op.Bitcast) return true;
  if (op >= Op.SNegate && op <= Op.SMulExtended) return true;
  if (op >= Op.Any && op <= Op.FUnordGreaterThanEqual) return true;
  if (op >= Op.ShiftRightLogical && op <= Op.BitCount) return true;
  if (op >= Op.DPdx && op <= Op.FwidthCoarse) return true;
  if (op === Op.Phi || op === Op.CopyLogical || op === Op.IsHelperInvocation) return true;
  return false;
}

/** Opcodes with a result id and no result type. */
function hasResultIdOnly(op: number): boolean {
  return op === Op.String || op === Op.ExtInstImport || op === Op.Label || op === Op.DecorationGroup ||
    (op >= Op.TypeVoid && op <= Op.TypeFunction) || op === Op.TypeForwardPointer;
}

export class SpirvModule {
  readonly words: Uint32Array;
  readonly instructions: Instruction[] = [];
  readonly types = new Map<number, SpirvType>();
  /** Constant values (spec constants at their defaults), by id. */
  readonly constants = new Map<number, unknown>();
  /** The spec constants: id to SpecId, for a pipeline's specialization. */
  readonly specIds = new Map<number, number>();
  readonly names = new Map<number, string>();
  readonly memberNames = new Map<number, Map<number, string>>();
  readonly decorations = new Map<number, Decorations>();
  readonly memberDecorations = new Map<number, Map<number, Decorations>>();
  /** Global variables: id to pointer type and storage class, with their initializer when they have one. */
  readonly globals = new Map<number, { type: number; storage: number; initializer: number }>();
  readonly functions = new Map<number, FunctionInfo>();
  readonly entryPoints: EntryPointInfo[] = [];
  /** Extended instruction sets imported, by id: "GLSL.std.450", "NonSemantic.Shader.DebugInfo.100", ... */
  readonly extSets = new Map<number, string>();
  /** OpString contents by id. */
  readonly strings = new Map<number, string>();
  /** Source locations per instruction ordinal, from the module's debug information. */
  readonly debug: SpirvDebugInfo | null;
  /** NonSemantic debug info: a local variable's name, by the OpVariable its DebugDeclare names. */
  readonly debugVariableNames = new Map<number, string>();
  /** The result type of every id that has one (instructions, parameters, variables). */
  readonly idTypes = new Map<number, number>();

  constructor(data: Uint8Array) {
    if (data.byteLength < 20 || data.byteLength % 4 !== 0) throw new Error("not a SPIR-V module");
    const words = new Uint32Array(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
    if (words[0] !== 0x07230203) throw new Error("not a SPIR-V module (bad magic number)");
    this.words = words;
    this.debug = parseSpirvDebugInfo(data);

    let fn: FunctionInfo | null = null;
    let block: Block | null = null;
    const groups = new Map<number, Decorations>();
    const debugLocals = new Map<number, string>();   // DebugLocalVariable id -> name

    for (let at = 5, index = 0; at < words.length; index++) {
      const count = words[at] >>> 16;
      const op = words[at] & 0xffff;
      if (count === 0 || at + count > words.length) throw new Error(`malformed SPIR-V at word ${at}`);
      const operands = words.subarray(at + 1, at + count);
      let resultType = 0;
      let result = 0;
      if (hasResultTypeAndId(op)) {
        resultType = operands[0];
        result = operands[1];
      } else if (hasResultIdOnly(op)) {
        result = operands[0];
      }
      const inst: Instruction = { op, words: operands, index, resultType, result };
      this.instructions.push(inst);
      if (result && resultType) this.idTypes.set(result, resultType);
      at += count;

      switch (op) {
        case Op.Name:
          this.names.set(operands[0], literalString(operands, 1).text);
          break;
        case Op.MemberName: {
          let m = this.memberNames.get(operands[0]);
          if (!m) this.memberNames.set(operands[0], (m = new Map()));
          m.set(operands[1], literalString(operands, 2).text);
          break;
        }
        case Op.String:
          this.strings.set(operands[0], literalString(operands, 1).text);
          break;
        case Op.ExtInstImport:
          this.extSets.set(operands[0], literalString(operands, 1).text);
          break;
        case Op.EntryPoint: {
          const name = literalString(operands, 2);
          this.entryPoints.push({ model: operands[0], function: operands[1], name: name.text, interface: Array.from(operands.subarray(2 + name.words)), modes: new Map() });
          break;
        }
        case Op.ExecutionMode:
        case Op.ExecutionModeId:
          for (const e of this.entryPoints) if (e.function === operands[0]) e.modes.set(operands[1], Array.from(operands.subarray(2)));
          break;
        case Op.Decorate:
        case Op.DecorateId:
        case Op.DecorateString:
          this._decorate(this.decorations, operands[0], operands[1], Array.from(operands.subarray(2)));
          break;
        case Op.MemberDecorate:
        case Op.MemberDecorateString: {
          let m = this.memberDecorations.get(operands[0]);
          if (!m) this.memberDecorations.set(operands[0], (m = new Map()));
          this._decorate(m, operands[1], operands[2], Array.from(operands.subarray(3)));
          break;
        }
        case Op.DecorationGroup:
          groups.set(operands[0], this.decorations.get(operands[0]) ?? new Map());
          break;
        case Op.GroupDecorate: {
          const g = this.decorations.get(operands[0]);
          for (const target of operands.subarray(1)) for (const [d, v] of g ?? []) this._decorate(this.decorations, target, d, v);
          break;
        }
        case Op.GroupMemberDecorate: {
          const g = this.decorations.get(operands[0]);
          for (let i = 1; i + 1 < operands.length; i += 2) {
            let m = this.memberDecorations.get(operands[i]);
            if (!m) this.memberDecorations.set(operands[i], (m = new Map()));
            for (const [d, v] of g ?? []) this._decorate(m, operands[i + 1], d, v);
          }
          break;
        }
        case Op.TypeVoid: this.types.set(result, { kind: "void" }); break;
        case Op.TypeBool: this.types.set(result, { kind: "bool" }); break;
        case Op.TypeInt: this.types.set(result, { kind: "int", width: operands[1], signed: operands[2] !== 0 }); break;
        case Op.TypeFloat: this.types.set(result, { kind: "float", width: operands[1] }); break;
        case Op.TypeVector: this.types.set(result, { kind: "vector", element: operands[1], count: operands[2] }); break;
        case Op.TypeMatrix: this.types.set(result, { kind: "matrix", column: operands[1], count: operands[2] }); break;
        case Op.TypeImage:
          this.types.set(result, { kind: "image", sampled: operands[1], dim: operands[2], depth: operands[3], arrayed: operands[4] !== 0, ms: operands[5] !== 0, usage: operands[6], format: operands[7] });
          break;
        case Op.TypeSampler: this.types.set(result, { kind: "sampler" }); break;
        case Op.TypeSampledImage: this.types.set(result, { kind: "sampledImage", image: operands[1] }); break;
        case Op.TypeArray:
          this.types.set(result, { kind: "array", element: operands[1], lengthId: operands[2], length: Number(this.constants.get(operands[2]) ?? 0) });
          break;
        case Op.TypeRuntimeArray: this.types.set(result, { kind: "runtimeArray", element: operands[1] }); break;
        case Op.TypeStruct: this.types.set(result, { kind: "struct", members: Array.from(operands.subarray(1)) }); break;
        case Op.TypeOpaque: this.types.set(result, { kind: "opaque", name: literalString(operands, 1).text }); break;
        case Op.TypePointer: this.types.set(result, { kind: "pointer", storage: operands[1], pointee: operands[2] }); break;
        case Op.TypeFunction: this.types.set(result, { kind: "function", returnType: operands[1], params: Array.from(operands.subarray(2)) }); break;
        case Op.ConstantTrue:
        case Op.SpecConstantTrue:
          this.constants.set(result, true);
          break;
        case Op.ConstantFalse:
        case Op.SpecConstantFalse:
          this.constants.set(result, false);
          break;
        case Op.Constant:
        case Op.SpecConstant:
          this.constants.set(result, this.scalarFromWords(resultType, operands.subarray(2)));
          break;
        case Op.ConstantComposite:
        case Op.SpecConstantComposite:
          this.constants.set(result, Array.from(operands.subarray(2)).map((id) => this.constants.get(id)));
          break;
        case Op.ConstantNull:
          this.constants.set(result, this.zero(resultType));
          break;
        case Op.Variable:
          if (!fn) this.globals.set(result, { type: resultType, storage: operands[2], initializer: operands[3] ?? 0 });
          break;
        case Op.Function:
          fn = { id: result, returnType: resultType, start: index, params: [], blocks: [], blockByLabel: new Map() };
          this.functions.set(result, fn);
          break;
        case Op.FunctionParameter:
          fn?.params.push({ id: result, type: resultType });
          break;
        case Op.Label:
          if (fn) {
            block = { label: result, start: index, end: index };
            fn.blocks.push(block);
            fn.blockByLabel.set(result, block);
          }
          break;
        case Op.Branch: case Op.BranchConditional: case Op.Switch: case Op.Kill: case Op.Return: case Op.ReturnValue:
        case Op.Unreachable: case Op.TerminateInvocation:
          if (block) block.end = index;
          block = null;
          break;
        case Op.FunctionEnd:
          fn = null;
          break;
        case Op.ExtInst: {
          const set = this.extSets.get(operands[2]);
          if (set === "NonSemantic.Shader.DebugInfo.100") {
            const instruction = operands[3];
            // DebugLocalVariable (26): Name, Type, Source, Line, Column, Parent, Flags.
            if (instruction === 26) debugLocals.set(result, this.strings.get(operands[4]) ?? "");
            // DebugDeclare (28): Local Variable, Variable, Expression.
            if (instruction === 28) {
              const name = debugLocals.get(operands[4]);
              if (name) this.debugVariableNames.set(operands[5], name);
            }
          }
          break;
        }
        default:
          break;
      }
    }
    // Spec constants' SpecId decorations came before them, so their ids are known only now.
    for (const [id, decorations] of this.decorations) {
      const spec = decorations.get(Decoration.SpecId);
      if (spec) this.specIds.set(id, spec[0]);
    }
    void groups;
  }

  private _decorate(into: Map<number, Decorations>, target: number, decoration: number, operands: number[]): void {
    let d = into.get(target);
    if (!d) into.set(target, (d = new Map()));
    d.set(decoration, operands);
  }

  /** A scalar constant's value from its literal words: a number for 32-bit and float values, a bigint for 64-bit integers. */
  scalarFromWords(typeId: number, literal: Uint32Array): number | bigint | boolean {
    const t = this.types.get(typeId);
    if (!t) return 0;
    if (t.kind === "bool") return literal[0] !== 0;
    if (t.kind === "float") {
      const buf = new ArrayBuffer(8);
      const view = new DataView(buf);
      if (t.width === 64) {
        view.setUint32(0, literal[0], true);
        view.setUint32(4, literal[1] ?? 0, true);
        return view.getFloat64(0, true);
      }
      if (t.width === 16) {
        return float16(literal[0] & 0xffff);
      }
      view.setUint32(0, literal[0], true);
      return view.getFloat32(0, true);
    }
    if (t.kind === "int") {
      if (t.width === 64) {
        const v = (BigInt(literal[1] ?? 0) << 32n) | BigInt(literal[0]);
        return t.signed ? BigInt.asIntN(64, v) : v;
      }
      const bits = literal[0];
      if (t.width < 32) {
        const mask = (1 << t.width) - 1;
        const v = bits & mask;
        return t.signed && v & (1 << (t.width - 1)) ? v - (1 << t.width) : v;
      }
      return t.signed ? bits | 0 : bits >>> 0;
    }
    return 0;
  }

  /** The zero value of a type (OpConstantNull, OpUndef, uninitialized variables). */
  zero(typeId: number): unknown {
    const t = this.types.get(typeId);
    if (!t) return 0;
    switch (t.kind) {
      case "bool": return false;
      case "int": return t.width === 64 ? 0n : 0;
      case "float": return 0;
      case "vector": return Array.from({ length: t.count }, () => this.zero(t.element));
      case "matrix": return Array.from({ length: t.count }, () => this.zero(t.column));
      case "array": return Array.from({ length: t.length }, () => this.zero(t.element));
      case "runtimeArray": return [];
      case "struct": return t.members.map((m) => this.zero(m));
      default: return null;
    }
  }

  decoration(id: number, decoration: number): number[] | undefined {
    return this.decorations.get(id)?.get(decoration);
  }

  memberDecoration(struct: number, member: number, decoration: number): number[] | undefined {
    return this.memberDecorations.get(struct)?.get(member)?.get(decoration);
  }

  /** A readable name for an id: its OpName, its debug info name, else "%id". */
  nameOf(id: number): string {
    return this.names.get(id) || this.debugVariableNames.get(id) || `%${id}`;
  }

  /** The type's name as GLSL writes it. */
  typeName(typeId: number): string {
    const t = this.types.get(typeId);
    if (!t) return `%${typeId}`;
    switch (t.kind) {
      case "void": return "void";
      case "bool": return "bool";
      case "int": return t.width === 32 ? (t.signed ? "int" : "uint") : `${t.signed ? "int" : "uint"}${t.width}_t`;
      case "float": return t.width === 32 ? "float" : t.width === 64 ? "double" : `float${t.width}_t`;
      case "vector": {
        const e = this.types.get(t.element);
        const prefix = e?.kind === "bool" ? "b" : e?.kind === "int" ? (e.signed ? "i" : "u") : e?.kind === "float" && e.width === 64 ? "d" : "";
        return `${prefix}vec${t.count}`;
      }
      case "matrix": {
        const c = this.types.get(t.column);
        const rows = c?.kind === "vector" ? c.count : 0;
        return rows === t.count ? `mat${t.count}` : `mat${t.count}x${rows}`;
      }
      case "array": return `${this.typeName(t.element)}[${t.length}]`;
      case "runtimeArray": return `${this.typeName(t.element)}[]`;
      case "struct": return this.names.get(typeId) || "struct";
      case "pointer": return this.typeName(t.pointee);
      case "image": return "image";
      case "sampler": return "sampler";
      case "sampledImage": return "sampler2D";
      default: return t.kind;
    }
  }

  entryPoint(name?: string, model?: number): EntryPointInfo | null {
    return this.entryPoints.find((e) => (name === undefined || e.name === name) && (model === undefined || e.model === model))
      ?? this.entryPoints.find((e) => model === undefined || e.model === model) ?? null;
  }
}

export function float16(h: number): number {
  const sign = h & 0x8000 ? -1 : 1;
  const exponent = (h >> 10) & 0x1f;
  const mantissa = h & 0x3ff;
  if (exponent === 0) return sign * Math.pow(2, -14) * (mantissa / 1024);
  if (exponent === 31) return mantissa ? NaN : sign * Infinity;
  return sign * Math.pow(2, exponent - 15) * (1 + mantissa / 1024);
}
