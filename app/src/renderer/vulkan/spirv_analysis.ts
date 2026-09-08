// Static shader analysis on SPIR-V: a modeled per-invocation cost (ALU, special-function,
// texture and memory ops, loops weighted by an assumed trip count) per function and per entry
// point, and findings for patterns with a well-known GPU cost, after WebGPU Inspector's shader
// cost model and performance analyzer (wgsl_reflect's cost_model.ts / perf_analyzer.ts), which
// work on WGSL source; this works on the SPIR-V the layer already holds, so it needs no shader
// source. Loop and branch nesting comes from the structured control flow SPIR-V requires
// (OpLoopMerge / OpSelectionMerge and their merge blocks), function names from OpName, and
// source lines from the module's debug information when the compiler embedded it.
//
// These are heuristics, not a profiler: costs compare shaders and functions with each other,
// and findings point at constructs that are expensive relative to their surroundings.
import { parseSpirvDebugInfo, type DebugLocation } from "./spirv_debug.js";
import type { ShaderStage } from "./spirv_reflect.js";

export interface CostVec { alu: number; sfu: number; texture: number; memory: number }
export type CostDimension = keyof CostVec;
export const COST_DIMENSIONS: CostDimension[] = ["alu", "sfu", "texture", "memory"];
/** Relative weight of each dimension in the single "cost" figure (WebGPU Inspector's defaults). */
export const COST_WEIGHTS: CostVec = { alu: 1, sfu: 4, texture: 20, memory: 8 };
/** Iterations assumed for a loop whose trip count is unknown (always, in SPIR-V). */
export const LOOP_TRIPS = 8;

export type Severity = "high" | "medium" | "low" | "info";
export type Confidence = "high" | "medium" | "low";

export interface Finding {
  rule: string;
  severity: Severity;
  confidence: Confidence;
  message: string;
  function: string;
  loopDepth: number;
  /** Source location from the module's debug information, when present. */
  file?: string;
  line?: number;
  /** How many instructions at this location raised the finding. */
  count: number;
}

/** The cost charged to one source line of a function (modules with line information). */
export interface LineCost {
  file: string;
  line: number;
  cost: CostVec;
  weighted: number;
  dominant: CostDimension;
  instructions: number;
}

export interface FunctionAnalysis {
  id: number;
  name: string;
  /** Own cost of one execution, loops weighted by LOOP_TRIPS per nesting level. */
  cost: CostVec;
  /** The own cost split by source line, costliest first (empty without line information). */
  lines: LineCost[];
  /** Own cost plus the cost of every function it calls (recursion cut). */
  inclusive: CostVec;
  instructions: number;
  loops: number;
  branches: number;
  calls: number[];
}

export interface EntryPointAnalysis {
  name: string;
  stage: ShaderStage;
  functionId: number;
  cost: CostVec;
  weighted: number;
  dominant: CostDimension;
  /** Functions reachable from the entry point, by inclusive cost. */
  functions: FunctionAnalysis[];
}

export interface AnalysisTotals {
  instructions: number;
  functions: number;
  loops: number;
  branches: number;
  textureOps: number;
  memoryOps: number;
  sfuOps: number;
  atomics: number;
  barriers: number;
  derivatives: number;
  discards: number;
}

export interface ShaderAnalysis {
  entryPoints: EntryPointAnalysis[];
  functions: FunctionAnalysis[];
  findings: Finding[];
  totals: AnalysisTotals;
  hasLines: boolean;
}

export function emptyCost(): CostVec {
  return { alu: 0, sfu: 0, texture: 0, memory: 0 };
}

export function addCost(dst: CostVec, src: CostVec, scale = 1): void {
  dst.alu += src.alu * scale;
  dst.sfu += src.sfu * scale;
  dst.texture += src.texture * scale;
  dst.memory += src.memory * scale;
}

export function weighCost(c: CostVec, w: CostVec = COST_WEIGHTS): number {
  return c.alu * w.alu + c.sfu * w.sfu + c.texture * w.texture + c.memory * w.memory;
}

export function dominantDimension(c: CostVec, w: CostVec = COST_WEIGHTS): CostDimension {
  let best: CostDimension = "alu";
  let bestValue = -1;
  for (const d of COST_DIMENSIONS) {
    const v = c[d] * w[d];
    if (v > bestValue) {
      bestValue = v;
      best = d;
    }
  }
  return best;
}

export const SEVERITY_RANK: Record<Severity, number> = { high: 3, medium: 2, low: 1, info: 0 };

export function worstSeverity(findings: Finding[]): Severity {
  let worst: Severity = "info";
  for (const f of findings) if (SEVERITY_RANK[f.severity] > SEVERITY_RANK[worst]) worst = f.severity;
  return worst;
}

/** "1 high, 2 medium": the non-empty severities, worst first. */
export function severitySummary(findings: Finding[]): string {
  const counts: Record<Severity, number> = { high: 0, medium: 0, low: 0, info: 0 };
  for (const f of findings) counts[f.severity]++;
  const parts: string[] = [];
  for (const s of ["high", "medium", "low", "info"] as Severity[]) if (counts[s]) parts.push(`${counts[s]} ${s}`);
  return parts.join(", ");
}

// ---------------------------------------------------------------------------------------------
// Opcodes

const enum Op {
  Name = 5, String = 7, ExtInstImport = 11, ExtInst = 12, EntryPoint = 15,
  TypePointer = 32, ConstantTrue = 41, ConstantFalse = 42, Constant = 43, ConstantComposite = 44,
  SpecConstantTrue = 48, SpecConstantFalse = 49, SpecConstant = 50, SpecConstantComposite = 51,
  Function = 54, FunctionEnd = 56, FunctionCall = 57, Variable = 59, Load = 61, Store = 62, Decorate = 71,
  AccessChain = 65, InBoundsAccessChain = 66, PtrAccessChain = 67, InBoundsPtrAccessChain = 70,
  Transpose = 84,
  ImageSampleImplicitLod = 87, ImageSampleExplicitLod = 88, ImageSampleDrefImplicitLod = 89, ImageSampleDrefExplicitLod = 90,
  ImageSampleProjImplicitLod = 91, ImageSampleProjExplicitLod = 92, ImageSampleProjDrefImplicitLod = 93, ImageSampleProjDrefExplicitLod = 94,
  ImageFetch = 95, ImageGather = 96, ImageDrefGather = 97, ImageRead = 98, ImageWrite = 99,
  ImageQuerySizeLod = 103, ImageQuerySize = 104, ImageQueryLod = 105, ImageQueryLevels = 106, ImageQuerySamples = 107,
  UDiv = 134, SDiv = 135, FDiv = 136, UMod = 137, SRem = 138, SMod = 139, FRem = 140, FMod = 141,
  VectorTimesScalar = 142, MatrixTimesScalar = 143, VectorTimesMatrix = 144, MatrixTimesVector = 145, MatrixTimesMatrix = 146,
  OuterProduct = 147, Dot = 148,
  DPdx = 207, DPdy = 208, Fwidth = 209, DPdxFine = 210, DPdyFine = 211, FwidthFine = 212, DPdxCoarse = 213, DPdyCoarse = 214, FwidthCoarse = 215,
  ControlBarrier = 224, MemoryBarrier = 225, AtomicLoad = 227, AtomicXor = 242,
  LoopMerge = 246, SelectionMerge = 247, Label = 248, Branch = 249, BranchConditional = 250, Switch = 251, Kill = 252,
  ImageSparseSampleImplicitLod = 305, ImageSparseRead = 320,
  TerminateInvocation = 4416, DemoteToHelperInvocation = 5380, AtomicFAddEXT = 6035, AtomicFMinEXT = 5614, AtomicFMaxEXT = 5615,
}

const enum StorageClass { UniformConstant = 0, Uniform = 2, Workgroup = 4, PushConstant = 9, Image = 11, StorageBuffer = 12, PhysicalStorageBuffer = 5349 }
const enum Dec { BufferBlock = 3 }

const STAGES: Record<number, ShaderStage> = {
  0: "vertex", 1: "tess_control", 2: "tess_eval", 3: "geometry", 4: "fragment", 5: "compute",
  5267: "task", 5268: "mesh", 5313: "raygen", 5314: "intersection", 5315: "any_hit", 5316: "closest_hit",
  5317: "miss", 5318: "callable", 5364: "task", 5365: "mesh",
};

// GLSL.std.450: name, cost, and the tier the loop rules use (3 = several SFU ops, 2 = one, 1 = ALU work).
interface ExtInfo { name: string; cost: CostVec; tier: 0 | 1 | 2 | 3 }
const c = (alu: number, sfu: number): CostVec => ({ alu, sfu, texture: 0, memory: 0 });
const GLSL_EXT: Record<number, ExtInfo> = {
  1: { name: "round", cost: c(1, 0), tier: 0 }, 2: { name: "roundEven", cost: c(1, 0), tier: 0 }, 3: { name: "trunc", cost: c(1, 0), tier: 0 },
  4: { name: "abs", cost: c(1, 0), tier: 0 }, 5: { name: "abs", cost: c(1, 0), tier: 0 }, 6: { name: "sign", cost: c(1, 0), tier: 0 },
  7: { name: "sign", cost: c(1, 0), tier: 0 }, 8: { name: "floor", cost: c(1, 0), tier: 0 }, 9: { name: "ceil", cost: c(1, 0), tier: 0 },
  10: { name: "fract", cost: c(1, 0), tier: 0 }, 11: { name: "radians", cost: c(1, 0), tier: 0 }, 12: { name: "degrees", cost: c(1, 0), tier: 0 },
  13: { name: "sin", cost: c(1, 1), tier: 2 }, 14: { name: "cos", cost: c(1, 1), tier: 2 }, 15: { name: "tan", cost: c(1, 2), tier: 2 },
  16: { name: "asin", cost: c(2, 2), tier: 3 }, 17: { name: "acos", cost: c(2, 2), tier: 3 }, 18: { name: "atan", cost: c(2, 2), tier: 3 },
  19: { name: "sinh", cost: c(2, 2), tier: 3 }, 20: { name: "cosh", cost: c(2, 2), tier: 3 }, 21: { name: "tanh", cost: c(3, 2), tier: 3 },
  22: { name: "asinh", cost: c(3, 2), tier: 3 }, 23: { name: "acosh", cost: c(3, 2), tier: 3 }, 24: { name: "atanh", cost: c(3, 2), tier: 3 },
  25: { name: "atan2", cost: c(3, 2), tier: 3 }, 26: { name: "pow", cost: c(1, 2), tier: 3 }, 27: { name: "exp", cost: c(0, 1), tier: 3 },
  28: { name: "log", cost: c(1, 1), tier: 3 }, 29: { name: "exp2", cost: c(0, 1), tier: 3 }, 30: { name: "log2", cost: c(0, 1), tier: 3 },
  31: { name: "sqrt", cost: c(0, 1), tier: 2 }, 32: { name: "inversesqrt", cost: c(0, 1), tier: 2 }, 33: { name: "determinant", cost: c(9, 0), tier: 2 },
  34: { name: "inverse", cost: c(30, 1), tier: 3 }, 35: { name: "modf", cost: c(2, 0), tier: 0 }, 36: { name: "modf", cost: c(2, 0), tier: 0 },
  37: { name: "min", cost: c(1, 0), tier: 0 }, 38: { name: "min", cost: c(1, 0), tier: 0 }, 39: { name: "min", cost: c(1, 0), tier: 0 },
  40: { name: "max", cost: c(1, 0), tier: 0 }, 41: { name: "max", cost: c(1, 0), tier: 0 }, 42: { name: "max", cost: c(1, 0), tier: 0 },
  43: { name: "clamp", cost: c(2, 0), tier: 0 }, 44: { name: "clamp", cost: c(2, 0), tier: 0 }, 45: { name: "clamp", cost: c(2, 0), tier: 0 },
  46: { name: "mix", cost: c(2, 0), tier: 0 }, 47: { name: "mix", cost: c(2, 0), tier: 0 }, 48: { name: "step", cost: c(1, 0), tier: 0 },
  49: { name: "smoothstep", cost: c(5, 0), tier: 1 }, 50: { name: "fma", cost: c(1, 0), tier: 0 }, 51: { name: "frexp", cost: c(2, 1), tier: 2 },
  52: { name: "frexp", cost: c(2, 1), tier: 2 }, 53: { name: "ldexp", cost: c(0, 1), tier: 2 },
  54: { name: "packSnorm4x8", cost: c(3, 0), tier: 1 }, 55: { name: "packUnorm4x8", cost: c(3, 0), tier: 1 }, 56: { name: "packSnorm2x16", cost: c(2, 0), tier: 1 },
  57: { name: "packUnorm2x16", cost: c(2, 0), tier: 1 }, 58: { name: "packHalf2x16", cost: c(2, 0), tier: 1 }, 59: { name: "packDouble2x32", cost: c(1, 0), tier: 0 },
  60: { name: "unpackSnorm2x16", cost: c(2, 0), tier: 1 }, 61: { name: "unpackUnorm2x16", cost: c(2, 0), tier: 1 }, 62: { name: "unpackHalf2x16", cost: c(2, 0), tier: 1 },
  63: { name: "unpackSnorm4x8", cost: c(3, 0), tier: 1 }, 64: { name: "unpackUnorm4x8", cost: c(3, 0), tier: 1 }, 65: { name: "unpackDouble2x32", cost: c(1, 0), tier: 0 },
  66: { name: "length", cost: c(3, 1), tier: 2 }, 67: { name: "distance", cost: c(4, 1), tier: 2 }, 68: { name: "cross", cost: c(6, 0), tier: 1 },
  69: { name: "normalize", cost: c(4, 1), tier: 2 }, 70: { name: "faceforward", cost: c(5, 0), tier: 1 }, 71: { name: "reflect", cost: c(6, 0), tier: 1 },
  72: { name: "refract", cost: c(10, 1), tier: 2 }, 73: { name: "findLSB", cost: c(1, 0), tier: 0 }, 74: { name: "findMSB", cost: c(1, 0), tier: 0 },
  75: { name: "findMSB", cost: c(1, 0), tier: 0 }, 76: { name: "interpolateAtCentroid", cost: c(2, 0), tier: 0 }, 77: { name: "interpolateAtSample", cost: c(2, 0), tier: 0 },
  78: { name: "interpolateAtOffset", cost: c(2, 0), tier: 0 }, 79: { name: "nmin", cost: c(1, 0), tier: 0 }, 80: { name: "nmax", cost: c(1, 0), tier: 0 },
  81: { name: "nclamp", cost: c(2, 0), tier: 0 },
};

const TEXTURE_SAMPLE_COST: CostVec = { alu: 2, sfu: 0, texture: 1, memory: 0 };
const TEXTURE_QUERY_COST: CostVec = { alu: 1, sfu: 0, texture: 0, memory: 0 };
const MEMORY_COST: CostVec = { alu: 0, sfu: 0, texture: 0, memory: 1 };
const ATOMIC_COST: CostVec = { alu: 1, sfu: 0, texture: 0, memory: 1 };
const BARRIER_COST: CostVec = { alu: 4, sfu: 0, texture: 0, memory: 0 };
const DERIVATIVE_COST: CostVec = { alu: 2, sfu: 0, texture: 0, memory: 0 };
const ALU: CostVec = { alu: 1, sfu: 0, texture: 0, memory: 0 };
const FDIV: CostVec = { alu: 0, sfu: 1, texture: 0, memory: 0 };
const FMOD: CostVec = { alu: 1, sfu: 1, texture: 0, memory: 0 };
const IDIV: CostVec = { alu: 2, sfu: 1, texture: 0, memory: 0 };

function isSample(op: number): boolean {
  return (op >= Op.ImageSampleImplicitLod && op <= Op.ImageDrefGather) || (op >= Op.ImageSparseSampleImplicitLod && op < Op.ImageSparseRead);
}
function isTextureOp(op: number): boolean {
  return isSample(op) || op === Op.ImageRead || op === Op.ImageWrite || op === Op.ImageSparseRead;
}
function isDerivative(op: number): boolean {
  return op >= Op.DPdx && op <= Op.FwidthCoarse;
}
function isAtomic(op: number): boolean {
  return (op >= Op.AtomicLoad && op <= Op.AtomicXor) || op === Op.AtomicFAddEXT || op === Op.AtomicFMinEXT || op === Op.AtomicFMaxEXT;
}
function isDiscard(op: number): boolean {
  return op === Op.Kill || op === Op.TerminateInvocation || op === Op.DemoteToHelperInvocation;
}
// Plain arithmetic, comparison, logic, conversion and composite ops: one ALU op each.
function isAlu(op: number): boolean {
  return (op >= 77 && op <= 84) || (op >= 109 && op <= 133) || (op >= 149 && op <= 205) || (op >= 79 && op <= 81);
}

function readString(words: Uint32Array, start: number, end: number): string {
  const bytes: number[] = [];
  for (let i = start; i < end; i++) {
    const w = words[i];
    for (let b = 0; b < 4; b++) {
      const ch = (w >>> (b * 8)) & 0xff;
      if (ch === 0) return new TextDecoder().decode(new Uint8Array(bytes));
      bytes.push(ch);
    }
  }
  return new TextDecoder().decode(new Uint8Array(bytes));
}

// ---------------------------------------------------------------------------------------------

/** Analyzes a SPIR-V module; null when the data is not SPIR-V. */
export function analyzeSpirv(data: Uint8Array): ShaderAnalysis | null {
  if (data.byteLength < 20 || data.byteLength % 4) return null;
  const bytes = data.byteOffset % 4 ? data.slice() : data;
  const words = new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 4);
  if (words[0] !== 0x07230203) return null;
  const debug = parseSpirvDebugInfo(data);
  const locations: (DebugLocation | null)[] = debug?.locations ?? [];
  const fileName = (loc: DebugLocation): string => debug?.files[loc.file]?.name.replace(/^.*[\\/]/, "") ?? "";

  const names = new Map<number, string>();
  const constants = new Set<number>();
  const pointerClass = new Map<number, number>();   // OpTypePointer id -> storage class
  const pointee = new Map<number, number>();        // OpTypePointer id -> pointed-to type
  const bufferBlocks = new Set<number>();           // struct types decorated BufferBlock (SPIR-V 1.0 storage buffers)
  const idClass = new Map<number, number>();        // variable / access chain id -> effective storage class
  const functions = new Map<number, FunctionAnalysis>();
  const entries: { name: string; stage: ShaderStage; functionId: number }[] = [];
  const findings = new Map<string, Finding>();
  const lineCosts = new Map<number, Map<string, LineCost>>();   // function id -> "file:line" -> cost
  const totals: AnalysisTotals = { instructions: 0, functions: 0, loops: 0, branches: 0, textureOps: 0, memoryOps: 0, sfuOps: 0, atomics: 0, barriers: 0, derivatives: 0, discards: 0 };
  let glslSet = 0;
  let fn: FunctionAnalysis | null = null;
  const loopStack: number[] = [];       // merge block ids of the loops being walked
  const selectionStack: number[] = [];  // merge block ids of the selections being walked
  let hasLines = false;

  const finding = (rule: string, severity: Severity, confidence: Confidence, message: string, ordinal: number): void => {
    const loc = locations[ordinal] ?? null;
    const key = `${rule}|${fn?.id ?? 0}|${loc ? `${loc.file}:${loc.line}` : "-"}`;
    const existing = findings.get(key);
    if (existing) {
      existing.count++;
      if (SEVERITY_RANK[severity] > SEVERITY_RANK[existing.severity]) existing.severity = severity;
      return;
    }
    findings.set(key, {
      rule, severity, confidence, message, function: fn?.name ?? "", loopDepth: loopStack.length, count: 1,
      ...(loc ? { file: fileName(loc), line: loc.line } : {}),
    });
    if (loc) hasLines = true;
  };
  const depthSeverity = (base: Severity, deeper: Severity): Severity => (loopStack.length >= 2 ? deeper : base);
  // Charges a cost to the current function, and to the source line of the current instruction.
  let ordinal = 0;
  const lineOf = (): LineCost | null => {
    const loc = fn ? locations[ordinal] : null;
    if (!fn || !loc) return null;
    const byLine = lineCosts.get(fn.id)!;
    const key = `${loc.file}:${loc.line}`;
    let entry = byLine.get(key);
    if (!entry) {
      entry = { file: fileName(loc), line: loc.line, cost: emptyCost(), weighted: 0, dominant: "alu", instructions: 0 };
      byLine.set(key, entry);
    }
    return entry;
  };
  const charge = (c: CostVec, s: number): void => {
    addCost(fn!.cost, c, s);
    const entry = lineOf();
    if (entry) addCost(entry.cost, c, s);
  };

  let i = 5;
  while (i < words.length) {
    const w = words[i];
    const op = w & 0xffff;
    const len = w >>> 16;
    if (len === 0) break;
    const a = i + 1;
    const end = Math.min(words.length, i + len);
    const inLoop = loopStack.length > 0;
    const scale = Math.pow(LOOP_TRIPS, loopStack.length);

    switch (op) {
      case Op.Name: names.set(words[a], readString(words, a + 1, end)); break;
      case Op.ExtInstImport: if (readString(words, a + 1, end) === "GLSL.std.450") glslSet = words[a]; break;
      case Op.EntryPoint: entries.push({ stage: STAGES[words[a]] ?? "unknown", functionId: words[a + 1], name: readString(words, a + 2, end) }); break;
      case Op.TypePointer:
        pointerClass.set(words[a], words[a + 1]);
        pointee.set(words[a], words[a + 2]);
        break;
      case Op.Decorate:
        if (words[a + 1] === Dec.BufferBlock) bufferBlocks.add(words[a]);
        break;
      case Op.Constant: case Op.ConstantTrue: case Op.ConstantFalse: case Op.ConstantComposite:
      case Op.SpecConstant: case Op.SpecConstantTrue: case Op.SpecConstantFalse: case Op.SpecConstantComposite:
        constants.add(words[a + 1]);
        break;
      case Op.Variable: {
        // A SPIR-V 1.0 storage buffer is a Uniform variable of a BufferBlock struct.
        let cls = words[a + 2];
        if (cls === StorageClass.Uniform && bufferBlocks.has(pointee.get(words[a]) ?? -1)) cls = StorageClass.StorageBuffer;
        idClass.set(words[a + 1], cls);
        break;
      }
      case Op.AccessChain: case Op.InBoundsAccessChain: case Op.PtrAccessChain: case Op.InBoundsPtrAccessChain: {
        // The base's effective class first (it knows about buffer blocks), else the pointer type's.
        const base = idClass.get(words[a + 2]);
        const cls = base ?? pointerClass.get(words[a]);
        if (cls !== undefined) idClass.set(words[a + 1], cls);
        break;
      }
      case Op.Function: {
        const id = words[a + 1];
        fn = { id, name: names.get(id) ?? `function_${id}`, cost: emptyCost(), inclusive: emptyCost(), instructions: 0, loops: 0, branches: 0, calls: [], lines: [] };
        lineCosts.set(id, new Map());
        functions.set(id, fn);
        totals.functions++;
        loopStack.length = 0;
        selectionStack.length = 0;
        break;
      }
      case Op.FunctionEnd: fn = null; break;
      case Op.Label: {
        const id = words[a];
        while (loopStack.length && loopStack[loopStack.length - 1] === id) loopStack.pop();
        while (selectionStack.length && selectionStack[selectionStack.length - 1] === id) selectionStack.pop();
        // Merge blocks can be nested out of stack order in unusual layouts; drop any match.
        const li = loopStack.indexOf(id);
        if (li >= 0) loopStack.splice(li);
        const si = selectionStack.indexOf(id);
        if (si >= 0) selectionStack.splice(si);
        break;
      }
      default:
        break;
    }

    if (fn && op !== Op.Function && op !== Op.FunctionEnd && op !== Op.Label) {
      fn.instructions++;
      totals.instructions++;
      const lineEntry = lineOf();
      if (lineEntry) { lineEntry.instructions++; hasLines = true; }
      switch (op) {
        case Op.LoopMerge:
          fn.loops++;
          totals.loops++;
          loopStack.push(words[a]);
          break;
        case Op.SelectionMerge:
          selectionStack.push(words[a]);
          break;
        case Op.BranchConditional: case Op.Switch:
          fn.branches++;
          totals.branches++;
          charge(ALU, scale);
          break;
        case Op.FunctionCall:
          fn.calls.push(words[a + 2]);
          break;
        case Op.Load: case Op.Store: {
          const cls = idClass.get(op === Op.Load ? words[a + 2] : words[a]);
          if (cls === StorageClass.Uniform || cls === StorageClass.StorageBuffer || cls === StorageClass.PhysicalStorageBuffer ||
              cls === StorageClass.Workgroup || cls === StorageClass.Image) {
            charge(MEMORY_COST, scale);
            totals.memoryOps++;
            if (inLoop && (cls === StorageClass.StorageBuffer || cls === StorageClass.PhysicalStorageBuffer)) {
              finding("storage-access-in-loop", depthSeverity("low", "medium"), "medium",
                `${op === Op.Load ? "Storage buffer read" : "Storage buffer write"} inside a loop: memory traffic scales with the trip count; load once outside the loop when the address does not change.`, ordinal);
            }
          } else if (cls === StorageClass.PushConstant) {
            charge(ALU, scale);
          }
          break;
        }
        case Op.FDiv: case Op.FRem: case Op.FMod: case Op.UDiv: case Op.SDiv: case Op.UMod: case Op.SRem: case Op.SMod: {
          const integer = op === Op.UDiv || op === Op.SDiv || op === Op.UMod || op === Op.SRem || op === Op.SMod;
          charge(integer ? IDIV : op === Op.FDiv ? FDIV : FMOD, scale);
          totals.sfuOps++;
          const divisorConstant = constants.has(words[a + 3]);
          if (inLoop && !divisorConstant) {
            finding("costly-arithmetic-in-loop", depthSeverity("low", "medium"), "high",
              `${integer ? "Integer" : "Floating-point"} ${op === Op.FDiv || op === Op.UDiv || op === Op.SDiv ? "division" : "modulo"} by a non-constant inside a loop; multiply by a reciprocal computed once outside the loop.`, ordinal);
          } else if (integer && !divisorConstant) {
            finding("integer-division", "info", "high", "Integer division or modulo by a non-constant: several instructions on most GPUs; shifts and masks when the divisor is a power of two.", ordinal);
          }
          break;
        }
        case Op.Dot: charge({ alu: 3, sfu: 0, texture: 0, memory: 0 }, scale); break;
        case Op.VectorTimesScalar: charge(ALU, scale); break;
        case Op.MatrixTimesScalar: case Op.VectorTimesMatrix: case Op.MatrixTimesVector: case Op.OuterProduct: case Op.Transpose:
          charge({ alu: 4, sfu: 0, texture: 0, memory: 0 }, scale);
          break;
        case Op.MatrixTimesMatrix: charge({ alu: 16, sfu: 0, texture: 0, memory: 0 }, scale); break;
        case Op.ExtInst: {
          if (words[a + 2] === glslSet) {
            const info = GLSL_EXT[words[a + 3]];
            if (info) {
              charge(info.cost, scale);
              if (info.cost.sfu) totals.sfuOps++;
              if (inLoop && info.tier >= 2) {
                finding("expensive-builtin-in-loop", info.tier === 3 ? depthSeverity("medium", "high") : depthSeverity("low", "medium"), "high",
                  `${info.name}() inside a loop${loopStack.length > 1 ? ` (depth ${loopStack.length})` : ""}: a special-function-unit operation repeated every iteration; hoist it out or replace it with cheaper arithmetic.`, ordinal);
              }
            } else {
              charge(ALU, scale);
            }
          } else {
            charge(ALU, scale);
          }
          break;
        }
        case Op.ControlBarrier: case Op.MemoryBarrier:
          charge(BARRIER_COST, scale);
          totals.barriers++;
          if (inLoop) finding("barrier-in-loop", "medium", "high", "A barrier inside a loop serializes the workgroup every iteration.", ordinal);
          break;
        case Op.ImageQuerySizeLod: case Op.ImageQuerySize: case Op.ImageQueryLod: case Op.ImageQueryLevels: case Op.ImageQuerySamples:
          charge(TEXTURE_QUERY_COST, scale);
          break;
        default:
          if (isTextureOp(op)) {
            charge(TEXTURE_SAMPLE_COST, scale);
            totals.textureOps++;
            if (inLoop) {
              finding("texture-sample-in-loop", "high", "high",
                `${op === Op.ImageWrite ? "Image store" : isSample(op) ? "Texture sample" : "Image load"} inside a loop${loopStack.length > 1 ? ` (depth ${loopStack.length})` : ""}: ${LOOP_TRIPS}+ texture operations per invocation; sample once outside the loop or reduce the iteration count.`, ordinal);
            }
          } else if (isAtomic(op)) {
            charge(ATOMIC_COST, scale);
            totals.atomics++;
            if (inLoop) finding("atomic-in-loop", "medium", "high", "An atomic operation inside a loop: a contention point repeated every iteration; accumulate locally and issue one atomic.", ordinal);
          } else if (isDerivative(op)) {
            charge(DERIVATIVE_COST, scale);
            totals.derivatives++;
            if (selectionStack.length) finding("derivative-in-branch", "medium", "medium", "A derivative (dFdx / dFdy / fwidth, or an implicit-LOD sample) inside a branch: undefined where neighbouring invocations take a different path, and it forces quad-wide execution.", ordinal);
          } else if (isSample(op) && selectionStack.length) {
            // (implicit-LOD samples in branches are reported with the derivative rule above)
          } else if (isDiscard(op)) {
            totals.discards++;
            finding("discard", "low", "medium", "discard / demote in a fragment shader disables early depth and stencil testing on many GPUs for every draw using it; prefer alpha blending or a depth pre-pass when the discard is rare.", ordinal);
          } else if (isAlu(op)) {
            charge(ALU, scale);
          }
          break;
      }
    }
    ordinal++;
    i += len;
  }

  // Inclusive costs over the call graph (recursion cut at the first repeat).
  const inclusive = (id: number, stack: Set<number>): CostVec => {
    const f = functions.get(id);
    if (!f) return emptyCost();
    if (stack.has(id)) return f.cost;
    stack.add(id);
    const total = { ...f.cost };
    for (const callee of f.calls) addCost(total, inclusive(callee, stack));
    stack.delete(id);
    return total;
  };
  for (const f of functions.values()) {
    f.inclusive = inclusive(f.id, new Set());
    f.lines = [...(lineCosts.get(f.id)?.values() ?? [])]
      .map((l) => ({ ...l, weighted: weighCost(l.cost), dominant: dominantDimension(l.cost) }))
      .filter((l) => l.weighted > 0)
      .sort((x, y) => y.weighted - x.weighted);
  }

  const entryPoints: EntryPointAnalysis[] = entries.map((e) => {
    const reachable = new Set<number>();
    const stack = [e.functionId];
    while (stack.length) {
      const id = stack.pop()!;
      if (reachable.has(id)) continue;
      reachable.add(id);
      for (const callee of functions.get(id)?.calls ?? []) stack.push(callee);
    }
    const fns = [...reachable].map((id) => functions.get(id)).filter((f): f is FunctionAnalysis => !!f)
      .sort((x, y) => weighCost(y.inclusive) - weighCost(x.inclusive));
    const cost = functions.get(e.functionId)?.inclusive ?? emptyCost();
    return { name: e.name, stage: e.stage, functionId: e.functionId, cost, weighted: weighCost(cost), dominant: dominantDimension(cost), functions: fns };
  });

  const sorted = [...findings.values()].sort((x, y) => SEVERITY_RANK[y.severity] - SEVERITY_RANK[x.severity] || y.loopDepth - x.loopDepth || y.count - x.count);
  // A discard in a module with no fragment entry point is not the early-Z concern.
  const hasFragment = entries.some((e) => e.stage === "fragment");
  const filtered = hasFragment ? sorted : sorted.filter((f) => f.rule !== "discard");
  return { entryPoints, functions: [...functions.values()], findings: filtered, totals, hasLines };
}

/** Analyses keyed by the module bytes, so re-inspecting does not re-parse. */
const cache = new WeakMap<Uint8Array, ShaderAnalysis | null>();

export function analyzeSpirvCached(data: Uint8Array): ShaderAnalysis | null {
  let a = cache.get(data);
  if (a === undefined) {
    try {
      a = analyzeSpirv(data);
    } catch (e) {
      console.warn("shader analysis failed", e);
      a = null;
    }
    cache.set(data, a);
  }
  return a;
}
