// Shader ablation: variants of a SPIR-V module with one part of a shader stage taken out, so that
// timing a draw with each variant measures what the part costs (vkinsp_replay's ablation, src/replay/src/
// ablation.cpp). The parts are the frames of the Shader Flame Graph:
//
//   stage     every write to the stage's outputs removed (a fragment shader's color outputs, a compute
//             shader's storage writes), which leaves the driver nothing to compute them for;
//   function  every call to it removed, or its result replaced;
//   line      every value a source line of a function computes replaced;
//   texture   every sample, fetch, gather or read of one bound texture replaced (these need no debug
//             information, so they are what an engine's generated shaders can be measured by).
//
// A value is replaced in place, by instructions that define the same id from a built-in input the
// invocation already has (gl_FragCoord.x, the vertex index, the global invocation id), so its uses stay
// valid and the driver's compiler drops whatever only fed it. The replacement has to be something the
// compiler cannot fold: a constant would let it compute everything downstream of the value at compile
// time (every hash of a constant coordinate, say) and charge that to the part. Where a type cannot be
// built from a scalar (arrays, structs) a constant stands in: 0.5, 1 for integers, false for booleans,
// never zero, which would let the compiler drop whatever the value is multiplied with.
//
// An ablation measures the time saved without the part, work that only feeds it included. So parts
// overlap: a line that combines the results of expensive lines saves their time too. Each variant says
// which other parts' values reach it (`upstream`), which is how shader_ablation.ts separates what a
// line does itself from what it only gathers.
//
// Values that decide control flow are never replaced, nor anything they are computed from (through
// SSA operands, variables, function arguments and return values): branch conditions, switch
// selectors and loop tests keep their values, so an ablation does not skip other work by sending the
// shader down another path. A part made only of such values has no variant.
import { parseSpirvDebugInfo } from "./spirv_debug.js";
import type { ShaderAnalysis } from "./spirv_analysis.js";
import type { ShaderStage } from "./spirv_reflect.js";

const enum Op {
  Name = 5, ExtInst = 12, EntryPoint = 15,
  TypeVoid = 19, TypeBool = 20, TypeInt = 21, TypeFloat = 22, TypeVector = 23, TypeMatrix = 24, TypeArray = 28, TypeStruct = 30, TypePointer = 32,
  ConstantFalse = 42, Constant = 43, ConstantComposite = 44, Function = 54, FunctionParameter = 55, FunctionEnd = 56, FunctionCall = 57,
  Variable = 59, Load = 61, Store = 62, AccessChain = 65, InBoundsAccessChain = 66, PtrAccessChain = 67, InBoundsPtrAccessChain = 70,
  Decorate = 71, MemberDecorate = 72, DecorationGroup = 73, GroupDecorate = 74, GroupMemberDecorate = 75,
  CompositeConstruct = 80, CompositeExtract = 81, CopyObject = 83, ImageWrite = 99,
  ConvertFToU = 109, ConvertFToS = 110, ConvertSToF = 111, ConvertUToF = 112, FConvert = 115, FOrdLessThan = 184,
  Phi = 245, LoopMerge = 246, SelectionMerge = 247, Label = 248, BranchConditional = 250, Switch = 251, ReturnValue = 254, DecorateId = 332,
  DecorateString = 5632, MemberDecorateString = 5633,
}

const enum StorageClass { Input = 1, Uniform = 2, Output = 3, StorageBuffer = 12, PhysicalStorageBuffer = 5349 }
const enum Decoration { BufferBlock = 3, BuiltIn = 11, Location = 30, Binding = 33, DescriptorSet = 34 }
const enum BuiltIn { Position = 0, FragCoord = 15, SampleMask = 20, FragDepth = 22, GlobalInvocationId = 28, VertexIndex = 42 }

/** What a variant takes out. */
export interface AblationPart {
  kind: "stage" | "function" | "line" | "texture";
  /** "fragment: main", "fbm", "shader.frag:31", "_MainTex". */
  name: string;
  functionId?: number;
  functionName?: string;
  file?: string;
  line?: number;
  /** Textures: the descriptor set and binding. */
  set?: number;
  binding?: number;
}

export interface AblationVariant extends AblationPart {
  spirv: Uint8Array;
  /** Instructions rewritten or removed. */
  edits: number;
  /** Indices into the plan's variants of the other parts whose values reach this one (for lines). */
  upstream: number[];
}

export interface AblationPlan {
  variants: AblationVariant[];
  /** Parts considered that have no variant, and why. */
  skipped: (AblationPart & { reason: string })[];
}

export interface AblationLimits {
  /** Functions measured, the costliest by the model first (default 16). */
  functions?: number;
  /** Source lines measured, the costliest by the model first (default 32). */
  lines?: number;
  /** Textures measured, the most sampled first (default 16). */
  textures?: number;
}

interface Instruction {
  op: number;
  start: number;
  len: number;
  ordinal: number;
  /** The function the instruction is in (0 outside functions). */
  fn: number;
  /** The loops the instruction is in, by their merge blocks' label ids, outermost first. */
  loops: number[];
  /** The loops and selections it is in, the same way. */
  nest: number[];
}

/** Ops whose value can be replaced: results computed from operands, without side effects on memory. */
function isValueOp(op: number): boolean {
  return (op >= 77 && op <= 84)            // vector and composite ops, OpCopyObject, OpTranspose
    || (op >= 87 && op <= 98)              // image samples, fetches, gathers, reads
    || (op >= 109 && op <= 205)            // conversions, arithmetic, relational and logical ops, bit ops, OpSelect
    || (op >= 207 && op <= 215)            // derivatives
    || (op >= 305 && op <= 320)            // sparse image ops
    || op === Op.ExtInst || op === Op.Load || op === Op.FunctionCall;
}

/** Ops inside functions that define no result id (stores, branches, merges, returns...), and labels, which define one nothing computes from. */
function definesValue(op: number): boolean {
  switch (op) {
    case 0: case 8: case 62: case 63: case 64: case 99: case 218: case 219: case 220: case 221: case 224: case 225: case 228:
    case 246: case 247: case 248: case 249: case 250: case 251: case 252: case 253: case 254: case 255: case 256: case 257:
    case 317: case 4416: case 5378: case 5379: case 5380:
      return false;
    default:
      return true;
  }
}

function isDecoration(op: number): boolean {
  return (op >= Op.Decorate && op <= Op.GroupMemberDecorate) || op === Op.DecorateId || op === Op.DecorateString || op === Op.MemberDecorateString;
}

function readString(words: Uint32Array, start: number, end: number): string {
  const bytes: number[] = [];
  for (let i = start; i < end; i++) {
    for (let b = 0; b < 4; b++) {
      const ch = (words[i] >>> (b * 8)) & 0xff;
      if (ch === 0) return new TextDecoder().decode(new Uint8Array(bytes));
      bytes.push(ch);
    }
  }
  return new TextDecoder().decode(new Uint8Array(bytes));
}

/** A module parsed once: its instructions, types and the facts the variants need. */
class Module {
  readonly words: Uint32Array;
  readonly instructions: Instruction[] = [];
  readonly defs = new Map<number, Instruction>();
  readonly types = new Map<number, Instruction>();
  readonly variableClass = new Map<number, number>();
  readonly builtIns = new Map<number, number>();
  readonly bufferBlocks = new Set<number>();
  readonly names = new Map<number, string>();
  readonly sets = new Map<number, number>();
  readonly bindings = new Map<number, number>();
  readonly entryPoints: { index: number; stage: number; functionId: number; name: string; interface: number[] }[] = [];
  readonly parameters = new Map<number, number[]>();       // function id -> parameter ids
  readonly calls: Instruction[] = [];
  readonly uses = new Map<number, number>();              // id -> times it appears as an operand word in functions
  /** OpConstant ids with their value's low word (array lengths). */
  readonly constants = new Map<number, number>();
  /** The stores into each variable, in module order. */
  readonly storesTo = new Map<number, Instruction[]>();
  private readonly _reaching = new Map<Instruction, Instruction[]>();
  firstFunction = -1;
  /** Where new decorations go: after the last one, or before the first type. */
  annotationEnd = -1;

  constructor(data: Uint8Array) {
    const bytes = data.byteOffset % 4 ? data.slice() : data;
    this.words = new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 4).slice();
    const w = this.words;
    let fn = 0;
    let ordinal = 0;
    let firstType = -1;
    // Structured constructs, by merge block: a loop's or selection's blocks are laid out between its
    // header and its merge block.
    let loops: number[] = [];
    let nest: number[] = [];
    for (let i = 5; i < w.length;) {
      const op = w[i] & 0xffff;
      const len = w[i] >>> 16;
      if (!len || i + len > w.length) throw new Error("malformed SPIR-V");
      if (op === Op.Label) {
        if (loops.includes(w[i + 1])) loops = loops.slice(0, loops.indexOf(w[i + 1]));
        if (nest.includes(w[i + 1])) nest = nest.slice(0, nest.indexOf(w[i + 1]));
      }
      const ins: Instruction = { op, start: i, len, ordinal, fn, loops, nest };
      if (op === Op.LoopMerge) {
        loops = [...loops, w[i + 1]];
        nest = [...nest, w[i + 1]];
      } else if (op === Op.SelectionMerge) {
        nest = [...nest, w[i + 1]];
      }
      const index = this.instructions.length;
      this.instructions.push(ins);
      const a = i + 1;
      if (isDecoration(op)) this.annotationEnd = index + 1;
      switch (op) {
        case Op.EntryPoint: {
          const name = readString(w, a + 2, i + len);
          const nameWords = Math.floor(new TextEncoder().encode(name).length / 4) + 1;
          this.entryPoints.push({ index, stage: w[a], functionId: w[a + 1], name, interface: Array.from(w.subarray(a + 2 + nameWords, i + len)) });
          break;
        }
        case Op.Name: this.names.set(w[a], readString(w, a + 1, i + len)); break;
        case Op.Decorate:
          if (w[a + 1] === Decoration.BuiltIn) this.builtIns.set(w[a], w[a + 2]);
          else if (w[a + 1] === Decoration.BufferBlock) this.bufferBlocks.add(w[a]);
          else if (w[a + 1] === Decoration.DescriptorSet) this.sets.set(w[a], w[a + 2]);
          else if (w[a + 1] === Decoration.Binding) this.bindings.set(w[a], w[a + 2]);
          break;
        case Op.TypeVoid: case Op.TypeBool: case Op.TypeInt: case Op.TypeFloat: case Op.TypeVector: case Op.TypeMatrix:
        case Op.TypeArray: case Op.TypeStruct: case Op.TypePointer: case 25: case 26: case 27: case 29: case 33:
          if (firstType < 0) firstType = index;
          this.types.set(w[a], ins);
          break;
        case Op.Constant:
          this.constants.set(w[a + 1], w[a + 2]);
          break;
        case Op.Function:
          if (this.firstFunction < 0) this.firstFunction = index;
          fn = w[a + 1];
          ins.fn = fn;
          this.parameters.set(fn, []);
          this.defs.set(w[a + 1], ins);
          break;
        case Op.FunctionEnd:
          fn = 0;
          loops = [];
          nest = [];
          break;
        case Op.FunctionParameter:
          this.parameters.get(fn)?.push(w[a + 1]);
          this.defs.set(w[a + 1], ins);
          break;
        case Op.Variable: {
          let cls = w[a + 2];
          const pointer = this.types.get(w[a]);
          if (cls === StorageClass.Uniform && pointer && this.bufferBlocks.has(w[pointer.start + 3])) cls = StorageClass.StorageBuffer;
          this.variableClass.set(w[a + 1], cls);
          this.defs.set(w[a + 1], ins);
          break;
        }
        default:
          if (fn && definesValue(op)) this.defs.set(w[a + 1], ins);
          if (op === Op.FunctionCall) this.calls.push(ins);
          break;
      }
      ordinal++;
      i += len;
    }
    if (this.annotationEnd < 0) this.annotationEnd = firstType >= 0 ? firstType : this.firstFunction;
    for (const ins of this.instructions) {
      if (ins.op !== Op.Store) continue;
      const v = this.baseVariable(w[ins.start + 1]);
      let list = this.storesTo.get(v);
      if (!list) this.storesTo.set(v, (list = []));
      list.push(ins);
    }
    // Uses, once every type is known (reading operands needs to tell debug instructions apart).
    for (const ins of this.instructions) {
      if (!ins.fn || ins.op === Op.Function) continue;
      for (const o of this.operandWords(ins)) this.uses.set(o, (this.uses.get(o) ?? 0) + 1);
      // Debug information naming a value still counts as a use of it.
      if (ins.op === Op.ExtInst && this.isVoid(w[ins.start + 1])) for (let k = ins.start + 5; k < ins.start + ins.len; k++) this.uses.set(w[k], (this.uses.get(w[k]) ?? 0) + 1);
    }
  }

  /** The id of an existing type declared with exactly these operands after the result id, or undefined. */
  findType(op: number, operands: number[]): number | undefined {
    const w = this.words;
    for (const [id, t] of this.types) {
      if (t.op !== op || t.len !== 2 + operands.length) continue;
      if (operands.every((o, k) => w[t.start + 2 + k] === o)) return id;
    }
    return undefined;
  }

  /** Whether a value of a type can be built from a scalar: scalars, vectors and matrices. */
  scalarBuilt(type: number): boolean {
    const t = this.types.get(type);
    if (!t) return false;
    if (t.op === Op.TypeBool || t.op === Op.TypeInt || t.op === Op.TypeFloat) return true;
    if (t.op === Op.TypeVector || t.op === Op.TypeMatrix) return this.scalarBuilt(this.words[t.start + 2]);
    return false;
  }

  /** Whether a value of a type can be replaced at all: built from a scalar, or a constant (arrays of a constant length, structs). */
  replaceable(type: number, depth = 0): boolean {
    const t = this.types.get(type);
    if (!t || depth > 16) return false;
    const w = this.words;
    switch (t.op) {
      case Op.TypeBool: case Op.TypeInt: case Op.TypeFloat: case Op.TypeVector: case Op.TypeMatrix: return true;
      case Op.TypeArray: {
        const length = this.constants.get(w[t.start + 3]);
        return length !== undefined && length <= 4096 && this.replaceable(w[t.start + 2], depth + 1);
      }
      case Op.TypeStruct:
        for (let k = t.start + 2; k < t.start + t.len; k++) if (!this.replaceable(w[k], depth + 1)) return false;
        return true;
      default: return false;
    }
  }

  isVoid(type: number): boolean {
    return this.types.get(type)?.op === Op.TypeVoid;
  }

  /** The variable a pointer is into, through access chains. */
  baseVariable(pointer: number): number {
    for (let depth = 0; depth < 64; depth++) {
      const d = this.defs.get(pointer);
      if (!d) return pointer;
      if (d.op === Op.AccessChain || d.op === Op.InBoundsAccessChain || d.op === Op.PtrAccessChain || d.op === Op.InBoundsPtrAccessChain) {
        pointer = this.words[d.start + 3];
        continue;
      }
      return pointer;
    }
    return pointer;
  }

  /**
   * The value ids an instruction reads: its operands without the literals among them (line numbers,
   * extended instruction numbers, composite indices, image operand masks), which would otherwise be
   * taken for ids they happen to equal.
   */
  operandWords(ins: Instruction): number[] {
    const w = this.words;
    const s = ins.start;
    const end = s + ins.len;
    const from = (k: number): number[] => Array.from(w.subarray(Math.min(s + k, end), end));
    switch (ins.op) {
      case Op.Store: return [w[s + 1], w[s + 2]];
      case Op.Load: return [w[s + 3]];
      case Op.ReturnValue: case Op.BranchConditional: case Op.Switch: return [w[s + 1]];
      case Op.FunctionCall: return from(4);
      case Op.ExtInst: return this.isVoid(w[s + 1]) ? [] : from(5);   // void: debug information
      case 79: return [w[s + 3], w[s + 4]];                            // OpVectorShuffle: then component literals
      case Op.CompositeExtract: return [w[s + 3]];                     // then index literals
      case 82: return [w[s + 3], w[s + 4]];                            // OpCompositeInsert: object, composite, then literals
      case Op.Phi: {
        const values: number[] = [];
        for (let k = s + 3; k + 1 < end; k += 2) values.push(w[k]);
        return values;
      }
      default:
        break;
    }
    if (ins.op >= 87 && ins.op <= 98) {
      // Image ops: the image, the coordinate, a Dref or component for some, then an operand mask literal and its ids.
      const fixed = ins.op === 89 || ins.op === 90 || ins.op === 93 || ins.op === 94 || ins.op === 96 || ins.op === 97 ? 3 : 2;
      if (ins.op === Op.ImageWrite) return [w[s + 1], w[s + 2], w[s + 3], ...Array.from(w.subarray(Math.min(s + 5, end), end))];
      return [...Array.from(w.subarray(s + 3, Math.min(s + 3 + fixed, end))), ...Array.from(w.subarray(Math.min(s + 4 + fixed, end), end))];
    }
    if (!definesValue(ins.op)) return [];   // lines, labels, merges, branches, returns, kills: no values
    return from(3);
  }

  /**
   * The stores whose values a read of a variable at `at` can see: going back from it in its function,
   * every store up to and including the first that writes the whole variable outside any branch or loop
   * the read is not in (it hides the ones before); the stores later in a loop around both (they reach
   * it through the back edge); and where nothing in the function hides them, what reaches the calls of
   * a parameter's function, or any store of a variable other functions write. Values an engine's
   * generated shaders keep in a few reused temporaries stay apart this way.
   */
  reachingStores(variable: number, at: Instruction, depth = 0): Instruction[] {
    const w = this.words;
    const out: Instruction[] = [];
    const stores = this.storesTo.get(variable) ?? [];
    const prefix = (a: number[], b: number[]): boolean => a.length <= b.length && a.every((x, i) => x === b[i]);
    let hidden = false;
    for (let k = stores.length - 1; k >= 0; k--) {
      const s = stores[k];
      if (s.fn !== at.fn || s.start >= at.start) continue;
      out.push(s);
      if (w[s.start + 1] === variable && prefix(s.nest, at.nest)) {
        hidden = true;
        break;
      }
    }
    for (const s of stores) {
      if (s.fn === at.fn && s.start > at.start && s.loops.some((l) => at.loops.includes(l))) out.push(s);
    }
    if (!hidden && depth < 8) {
      const def = this.defs.get(variable);
      if (def?.op === Op.FunctionParameter) {
        const index = this.parameters.get(def.fn)?.indexOf(variable) ?? -1;
        for (const call of this.calls) {
          if (w[call.start + 3] !== def.fn || call.len <= 4 + index || index < 0) continue;
          out.push(...this.reachingStores(this.baseVariable(w[call.start + 4 + index]), call, depth + 1));
        }
      } else {
        for (const s of stores) if (s.fn !== at.fn) out.push(s);
      }
    }
    return out;
  }

  /** The stores a load sees (reachingStores), once per load. */
  storesSeenBy(load: Instruction): Instruction[] {
    let list = this._reaching.get(load);
    if (!list) this._reaching.set(load, (list = this.reachingStores(this.baseVariable(this.words[load.start + 3]), load)));
    return list;
  }

  /**
   * The ids control flow depends on: conditions and selectors, and everything they are computed from,
   * followed through operands, the stores a load sees, function parameters (the arguments of every
   * call) and call results (the values the callee returns).
   */
  controlSlice(): Set<number> {
    const w = this.words;
    const slice = new Set<number>();
    const work: number[] = [];
    const add = (id: number): void => {
      if (!slice.has(id)) { slice.add(id); work.push(id); }
    };
    const returns = new Map<number, number[]>();         // function id -> values returned
    const paramIndex = new Map<number, { fn: number; index: number }>();
    for (const [fn, params] of this.parameters) params.forEach((p, index) => paramIndex.set(p, { fn, index }));
    for (const ins of this.instructions) {
      if (!ins.fn) continue;
      if (ins.op === Op.BranchConditional || ins.op === Op.Switch) add(w[ins.start + 1]);
      else if (ins.op === Op.ReturnValue) {
        let list = returns.get(ins.fn);
        if (!list) returns.set(ins.fn, (list = []));
        list.push(w[ins.start + 1]);
      }
    }
    while (work.length) {
      const id = work.pop()!;
      const d = this.defs.get(id);
      if (!d || !d.fn || d.op === Op.Function) continue;
      if (d.op === Op.FunctionParameter) {
        const p = paramIndex.get(id);
        if (p) for (const call of this.calls) if (w[call.start + 3] === p.fn && call.len > 4 + p.index) add(w[call.start + 4 + p.index]);
      } else if (d.op === Op.FunctionCall) {
        for (const v of returns.get(w[d.start + 3]) ?? []) add(v);
        for (const o of this.operandWords(d)) add(o);
      } else {
        for (const o of this.operandWords(d)) add(o);
        // A load brings the values stored where it reads (a loop counter's increment).
        if (d.op === Op.Load) for (const store of this.storesSeenBy(d)) {
          add(w[store.start + 2]);
          add(w[store.start + 1]);
        }
      }
    }
    return slice;
  }
}

// ---------------------------------------------------------------------------------------------
// Rewriting

/** The built-in input a stage's replacements are read from, and its type. */
function sourceBuiltIn(stage: ShaderStage): { builtIn: number; kind: "vec4" | "int" | "uvec3" } | null {
  if (stage === "fragment") return { builtIn: BuiltIn.FragCoord, kind: "vec4" };
  if (stage === "vertex") return { builtIn: BuiltIn.VertexIndex, kind: "int" };
  if (stage === "compute") return { builtIn: BuiltIn.GlobalInvocationId, kind: "uvec3" };
  return null;
}

/**
 * Rewrites a module: instructions replaced by ones that define the same value from the stage's built-in
 * input (or a constant), and instructions removed.
 */
function rewrite(m: Module, stage: ShaderStage, entryIndex: number, replace: Set<Instruction>, remove: Set<Instruction>,
                 written: Set<number> = new Set()): Uint8Array {
  const w = m.words;
  let bound = w[3];
  const annotations: number[] = [];
  const declarations: number[] = [];
  const typeIds = new Map<string, number>();
  const type = (op: number, operands: number[]): number => {
    const key = `${op}:${operands.join(",")}`;
    let id = typeIds.get(key) ?? m.findType(op, operands);
    if (id === undefined) {
      id = bound++;
      declarations.push(((2 + operands.length) << 16) | op, id, ...operands);
    }
    typeIds.set(key, id);
    return id;
  };
  const constants = new Map<number, number>();
  const constantOf = (t: number): number => {
    const existing = constants.get(t);
    if (existing !== undefined) return existing;
    const ti = m.types.get(t)!;
    let id: number;
    if (ti.op === Op.TypeBool) {
      id = bound++;
      declarations.push((3 << 16) | Op.ConstantFalse, t, id);
    } else if (ti.op === Op.TypeFloat) {
      const width = w[ti.start + 2];
      const literal = width === 64 ? [0, 0x3fe00000] : width === 16 ? [0x3800] : [0x3f000000];
      id = bound++;
      declarations.push(((3 + literal.length) << 16) | Op.Constant, t, id, ...literal);
    } else if (ti.op === Op.TypeInt) {
      const literal = w[ti.start + 2] === 64 ? [1, 0] : [1];
      id = bound++;
      declarations.push(((3 + literal.length) << 16) | Op.Constant, t, id, ...literal);
    } else {
      // Vectors and matrices (component type, count), arrays (element type, length constant), structs (members).
      let parts: number[];
      if (ti.op === Op.TypeVector || ti.op === Op.TypeMatrix) parts = new Array(w[ti.start + 3]).fill(w[ti.start + 2]);
      else if (ti.op === Op.TypeArray) parts = new Array(m.constants.get(w[ti.start + 3]) ?? 0).fill(w[ti.start + 2]);
      else parts = Array.from(w.subarray(ti.start + 2, ti.start + ti.len));
      const components = parts.map(constantOf);
      id = bound++;
      declarations.push(((3 + components.length) << 16) | Op.ConstantComposite, t, id, ...components);
    }
    constants.set(t, id);
    return id;
  };

  // The built-in input: an existing variable of the entry point's, or a new one added to its interface.
  const source = sourceBuiltIn(stage);
  let input = 0;
  let inputType = 0;
  let addToInterface = false;
  const float32 = (): number => type(Op.TypeFloat, [32]);
  if (source && [...replace].some((ins) => m.scalarBuilt(w[ins.start + 1]))) {
    for (const [id, builtIn] of m.builtIns) {
      if (builtIn === source.builtIn && m.variableClass.get(id) === StorageClass.Input) input = id;
    }
    const valueType = source.kind === "vec4" ? type(Op.TypeVector, [float32(), 4])
      : source.kind === "int" ? type(Op.TypeInt, [32, 1]) : type(Op.TypeVector, [type(Op.TypeInt, [32, 0]), 3]);
    if (input) {
      inputType = valueType;
    } else {
      const pointer = type(Op.TypePointer, [StorageClass.Input, valueType]);
      input = bound++;
      declarations.push((4 << 16) | Op.Variable, pointer, input, StorageClass.Input);
      annotations.push((4 << 16) | Op.Decorate, input, Decoration.BuiltIn, source.builtIn);
      inputType = valueType;
    }
    addToInterface = !m.entryPoints.find((e) => e.index === entryIndex)?.interface.includes(input);
  }

  // A float that differs between invocations, read where the replaced value was.
  const scalarSource = (out: number[]): number => {
    const loaded = bound++;
    out.push((4 << 16) | Op.Load, inputType, loaded, input);
    const f = float32();
    const x = bound++;
    if (source!.kind === "vec4") {
      out.push((5 << 16) | Op.CompositeExtract, f, x, loaded, 0);
    } else if (source!.kind === "int") {
      out.push((4 << 16) | Op.ConvertSToF, f, x, loaded);
    } else {
      const u = bound++;
      out.push((5 << 16) | Op.CompositeExtract, type(Op.TypeInt, [32, 0]), u, loaded, 0);
      out.push((4 << 16) | Op.ConvertUToF, f, x, u);
    }
    return x;
  };
  // Defines `result` of `t` from the float `x`: converted, or repeated into a vector or matrix.
  const build = (out: number[], t: number, x: number, result: number): void => {
    const ti = m.types.get(t)!;
    switch (ti.op) {
      case Op.TypeFloat:
        if (w[ti.start + 2] === 32) out.push((4 << 16) | Op.CopyObject, t, result, x);
        else out.push((4 << 16) | Op.FConvert, t, result, x);
        return;
      case Op.TypeInt:
        out.push((4 << 16) | (w[ti.start + 3] ? Op.ConvertFToS : Op.ConvertFToU), t, result, x);
        return;
      case Op.TypeBool:
        out.push((5 << 16) | Op.FOrdLessThan, t, result, x, constantOf(float32()));
        return;
      default: {
        const component = w[ti.start + 2];
        const count = w[ti.start + 3];
        const part = bound++;
        build(out, component, x, part);
        out.push(((3 + count) << 16) | Op.CompositeConstruct, t, result, ...new Array(count).fill(part));
      }
    }
  };
  // Inside a loop the replacement has to change every iteration as the value did, or the compiler hoists
  // whatever is computed from it out of the loop: a float from an operand defined in the same loop (and
  // not itself replaced), where the instruction has one. Not a load of a variable the part writes
  // (`written`): sum = sum.x would keep sum at its first value, a constant after all.
  const loopScalar = (out: number[], ins: Instruction): number | null => {
    if (!ins.loops.length) return null;
    const innermost = ins.loops[ins.loops.length - 1];
    for (const o of m.operandWords(ins)) {
      const d = m.defs.get(o);
      if (!d || !d.fn || d.op === Op.Variable || d.op === Op.FunctionParameter || replace.has(d) || !d.loops.includes(innermost) || !definesValue(d.op)) continue;
      if (d.op === Op.Load && written.has(m.baseVariable(w[d.start + 3]))) continue;
      const t = w[d.start + 1];
      const ti = m.types.get(t);
      const scalarType = ti?.op === Op.TypeVector ? w[ti.start + 2] : t;
      const scalar = m.types.get(scalarType);
      if (!scalar || (scalar.op !== Op.TypeFloat && scalar.op !== Op.TypeInt)) continue;
      let id = o;
      if (ti?.op === Op.TypeVector) {
        id = bound++;
        out.push((5 << 16) | Op.CompositeExtract, scalarType, id, o, 0);
      }
      if (scalar.op === Op.TypeFloat && w[scalar.start + 2] === 32) return id;
      const x = bound++;
      const convert = scalar.op === Op.TypeFloat ? Op.FConvert : w[scalar.start + 3] ? Op.ConvertSToF : Op.ConvertUToF;
      out.push((4 << 16) | convert, float32(), x, id);
      return x;
    }
    return null;
  };
  const replacement = new Map<Instruction, number[]>();
  for (const ins of replace) {
    const t = w[ins.start + 1];
    const result = w[ins.start + 2];
    const out: number[] = [];
    const varying = m.scalarBuilt(t) ? loopScalar(out, ins) : null;
    if (varying !== null) build(out, t, varying, result);
    else if (input && m.scalarBuilt(t)) build(out, t, scalarSource(out), result);
    else out.push((4 << 16) | Op.CopyObject, t, result, constantOf(t));
    replacement.set(ins, out);
  }

  const out: number[] = Array.from(w.subarray(0, 5));
  m.instructions.forEach((ins, index) => {
    if (index === m.annotationEnd) out.push(...annotations);
    if (index === m.firstFunction) out.push(...declarations);
    if (remove.has(ins)) return;
    const replaced = replacement.get(ins);
    if (replaced) {
      out.push(...replaced);
      return;
    }
    if (index === entryIndex && addToInterface) {
      out.push(((ins.len + 1) << 16) | Op.EntryPoint, ...w.subarray(ins.start + 1, ins.start + ins.len), input);
      return;
    }
    for (let k = ins.start; k < ins.start + ins.len; k++) out.push(w[k]);
  });
  out[3] = bound;
  return new Uint8Array(new Uint32Array(out).buffer);
}

// ---------------------------------------------------------------------------------------------
// Upstream parts

/**
 * For each part, the other parts whose values reach its instructions: through operands, variables,
 * and the values a called function returns (a function part reaches the lines that call it). Not
 * through function arguments: a line that passes a coordinate to a function is not what the function's
 * lines compute from as far as their cost goes.
 */
function upstreamParts(m: Module, parts: { results: Set<number>; instructions: Instruction[]; calls?: number }[]): number[][] {
  const w = m.words;
  const taint = new Map<number, bigint>();
  const variables = new Map<number, bigint>();
  const returns = new Map<number, bigint>();
  const get = (map: Map<number, bigint>, id: number): bigint => map.get(id) ?? 0n;
  const or = (map: Map<number, bigint>, id: number, bits: bigint): boolean => {
    const before = get(map, id);
    const after = before | bits;
    if (after === before) return false;
    map.set(id, after);
    return true;
  };
  parts.forEach((p, i) => { for (const id of p.results) or(taint, id, 1n << BigInt(i)); });
  const calledFunction = new Map<number, number>();    // part index of a function part, by function id
  parts.forEach((p, i) => { if (p.calls !== undefined) calledFunction.set(p.calls, i); });
  for (let pass = 0; pass < 16; pass++) {
    let changed = false;
    for (const ins of m.instructions) {
      if (!ins.fn || ins.op === Op.Function) continue;
      let bits = 0n;
      for (const o of m.operandWords(ins)) bits |= get(taint, o);
      if (ins.op === Op.Store) {
        changed = or(variables, m.baseVariable(w[ins.start + 1]), get(taint, w[ins.start + 2])) || changed;
      } else if (ins.op === Op.ReturnValue) {
        changed = or(returns, ins.fn, bits) || changed;
      } else if (definesValue(ins.op) && ins.op !== Op.FunctionParameter) {
        if (ins.op === Op.Load) bits |= get(variables, m.baseVariable(w[ins.start + 3]));
        if (ins.op === Op.FunctionCall) {
          // What the callee returns, not what it was given.
          bits = get(returns, w[ins.start + 3]);
          const part = calledFunction.get(w[ins.start + 3]);
          if (part !== undefined) bits |= 1n << BigInt(part);
        }
        changed = or(taint, w[ins.start + 2], bits) || changed;
      }
    }
    if (!changed) break;
  }
  return parts.map((p, i) => {
    let bits = 0n;
    for (const ins of p.instructions) {
      for (const o of m.operandWords(ins)) bits |= get(taint, o);
      if (ins.op === Op.Load) bits |= get(variables, m.baseVariable(w[ins.start + 3]));
      if (ins.op === Op.FunctionCall) bits |= get(returns, w[ins.start + 3]) | (calledFunction.has(w[ins.start + 3]) ? 1n << BigInt(calledFunction.get(w[ins.start + 3])!) : 0n);
    }
    bits &= ~(1n << BigInt(i));
    const out: number[] = [];
    for (let k = 0; k < parts.length; k++) if (bits & (1n << BigInt(k))) out.push(k);
    return out;
  });
}

// ---------------------------------------------------------------------------------------------

/** The variable a sampled image or image operand reads, through loads, OpSampledImage, OpImage and access chains. */
function textureVariable(m: Module, id: number): number | null {
  const w = m.words;
  for (let depth = 0; depth < 16; depth++) {
    const d = m.defs.get(id);
    if (!d) return null;
    if (d.op === Op.Variable) return m.variableClass.get(id) === 0 ? id : null;   // UniformConstant
    if (d.op === Op.Load) id = m.baseVariable(w[d.start + 3]);
    else if (d.op === 86 || d.op === 100) id = w[d.start + 3];                    // OpSampledImage, OpImage
    else return null;
  }
  return null;
}

/** Image reads: the samples, fetches, gathers and reads (not writes). */
function isImageRead(op: number): boolean {
  return (op >= 87 && op <= 98 && op !== Op.ImageWrite) || (op >= 305 && op <= 320);
}

/** How an entry point reads one bound texture. */
export interface TextureReads {
  set: number;
  binding: number;
  /** Reads per invocation, counting a function's reads once per call to it (loops once). */
  reads: number;
  /** Some read runs in a loop, or in a function called from one. */
  inLoop: boolean;
}

/**
 * How an entry point reads each texture it binds: whether a shader reads its input once per
 * invocation (as an input attachment could stand in for) or filters it. Null when the module
 * cannot be parsed or has no such entry point.
 */
export function textureReads(spirv: Uint8Array, entryPoint: string): TextureReads[] | null {
  let m: Module;
  try {
    m = new Module(spirv);
  } catch {
    return null;
  }
  const w = m.words;
  const entry = m.entryPoints.find((e) => e.name === entryPoint) ?? (m.entryPoints.length === 1 ? m.entryPoints[0] : undefined);
  if (!entry) return null;
  // Per function: its own reads by texture, and its calls (whether each is in a loop).
  const own = new Map<number, Map<number, { reads: number; inLoop: boolean }>>();
  for (const ins of m.instructions) {
    if (!ins.fn || !isImageRead(ins.op)) continue;
    const texture = textureVariable(m, w[ins.start + 3]);
    if (texture === null) continue;
    let byTexture = own.get(ins.fn);
    if (!byTexture) own.set(ins.fn, (byTexture = new Map()));
    const r = byTexture.get(texture) ?? { reads: 0, inLoop: false };
    r.reads++;
    if (ins.loops.length) r.inLoop = true;
    byTexture.set(texture, r);
  }
  const memo = new Map<number, Map<number, { reads: number; inLoop: boolean }>>();
  const total = (fn: number, stack: Set<number>): Map<number, { reads: number; inLoop: boolean }> => {
    const cached = memo.get(fn);
    if (cached) return cached;
    const out = new Map<number, { reads: number; inLoop: boolean }>();
    for (const [t, r] of own.get(fn) ?? []) out.set(t, { ...r });
    if (!stack.has(fn)) {
      stack.add(fn);
      for (const call of m.calls) {
        if (call.fn !== fn) continue;
        for (const [t, r] of total(w[call.start + 3], stack)) {
          const acc = out.get(t) ?? { reads: 0, inLoop: false };
          acc.reads += r.reads;
          acc.inLoop = acc.inLoop || r.inLoop || call.loops.length > 0;
          out.set(t, acc);
        }
      }
      stack.delete(fn);
    }
    memo.set(fn, out);
    return out;
  };
  const result: TextureReads[] = [];
  for (const [t, r] of total(entry.functionId, new Set())) {
    const set = m.sets.get(t);
    const binding = m.bindings.get(t);
    if (set !== undefined && binding !== undefined) result.push({ set, binding, ...r });
  }
  return result;
}

/**
 * The variants that measure a stage of a module: the stage itself, its costliest functions and its
 * costliest source lines, as the static analysis ranks them. `analysis` is the module's own.
 */
export function planAblation(spirv: Uint8Array, stage: ShaderStage, entryPoint: string, analysis: ShaderAnalysis, limits: AblationLimits = {}): AblationPlan {
  const plan: AblationPlan = { variants: [], skipped: [] };
  let m: Module;
  try {
    m = new Module(spirv);
  } catch {
    plan.skipped.push({ kind: "stage", name: `${stage}: ${entryPoint}`, reason: "the module could not be parsed" });
    return plan;
  }
  const w = m.words;
  const entry = analysis.entryPoints.find((e) => e.name === entryPoint && e.stage === stage) ?? analysis.entryPoints.find((e) => e.stage === stage);
  const moduleEntry = entry ? m.entryPoints.find((e) => e.functionId === entry.functionId) : undefined;
  if (!entry || !moduleEntry) {
    plan.skipped.push({ kind: "stage", name: `${stage}: ${entryPoint}`, reason: "the entry point is not in the module" });
    return plan;
  }
  const reachable = new Set(entry.functions.map((f) => f.id));
  const slice = m.controlSlice();
  const why = (ins: Instruction): string | null => {
    const t = w[ins.start + 1];
    if (slice.has(w[ins.start + 2])) return "control flow depends on it";
    if (m.isVoid(t)) return "void";
    if (!m.replaceable(t)) return "its type cannot be replaced";
    return null;
  };

  interface Candidate {
    part: AblationPart; replace: Set<Instruction>; remove: Set<Instruction>; results: Set<number>; instructions: Instruction[]; calls?: number;
    /** Variables the part stores to. */
    written: Set<number>;
  }
  const candidates: Candidate[] = [];
  let stageVariant: AblationVariant | null = null;

  // The stage: its outputs are not written. A fragment shader keeps the depth and sample mask it
  // writes (they change which fragments are shaded at all). A stage before rasterization loses its
  // position too: the replay times it with rasterization discarded (ablation.cpp), so what it
  // rasterizes changes nothing that is timed.
  {
    const part: AblationPart = { kind: "stage", name: `${stage}: ${entry.name}` };
    const outputs = new Set<number>();
    for (const v of moduleEntry.interface) {
      if (m.variableClass.get(v) !== StorageClass.Output) continue;
      const builtIn = m.builtIns.get(v);
      if (builtIn === BuiltIn.FragDepth || builtIn === BuiltIn.SampleMask) continue;
      outputs.add(v);
    }
    const remove = new Set<Instruction>();
    for (const ins of m.instructions) {
      if (!ins.fn || !reachable.has(ins.fn)) continue;
      if (ins.op === Op.Store) {
        const base = m.baseVariable(w[ins.start + 1]);
        const cls = m.variableClass.get(base);
        if (outputs.has(base) || (stage === "compute" && (cls === StorageClass.StorageBuffer || cls === StorageClass.PhysicalStorageBuffer))) remove.add(ins);
      } else if (ins.op === Op.ImageWrite && stage === "compute") {
        remove.add(ins);
      }
    }
    if (!remove.size) plan.skipped.push({ ...part, reason: "the stage writes no outputs that can be left out" });
    else stageVariant = { ...part, spirv: rewrite(m, stage, moduleEntry.index, new Set(), remove), edits: remove.size, upstream: [] };
  }

  // Functions: every call to one made a no-op.
  for (const f of entry.functions.filter((fn) => fn.id !== entry.functionId).slice(0, limits.functions ?? 16)) {
    const part: AblationPart = { kind: "function", name: f.name, functionId: f.id, functionName: f.name };
    const replace = new Set<Instruction>();
    const remove = new Set<Instruction>();
    let reason: string | null = null;
    for (const call of m.calls) {
      if (w[call.start + 3] !== f.id || !reachable.has(call.fn)) continue;
      if (m.isVoid(w[call.start + 1])) {
        if ((m.uses.get(w[call.start + 2]) ?? 0) > 0) reason = "a call's result id is used";
        else remove.add(call);
        continue;
      }
      const r = why(call);
      if (r) reason = r === "control flow depends on it" ? "control flow depends on what it returns" : "it returns a value that cannot be replaced";
      else replace.add(call);
    }
    if (reason) plan.skipped.push({ ...part, reason });
    else if (!replace.size && !remove.size) plan.skipped.push({ ...part, reason: "nothing calls it" });
    else candidates.push({ part, replace, remove, results: new Set(), instructions: [], calls: f.id, written: new Set() });
  }

  // Source lines: every value computed on one replaced.
  const debug = analysis.hasLines ? parseSpirvDebugInfo(spirv) : null;
  if (debug) {
    const baseName = (file: number): string => debug.files[file]?.name.replace(/^.*[\\/]/, "") ?? "";
    const lineOf = (ins: Instruction): string => {
      const loc = debug.locations[ins.ordinal];
      return loc ? `${loc.file}:${loc.line}` : "";
    };
    // Loop recurrences: a line that updates, every iteration of a loop, a value other lines read in that
    // loop (p = p * 2.0). Replacing it makes the loop's work the same every iteration, which a compiler
    // hoists out of the loop, so its saving would be the loop's. Through a variable, or an OpPhi's back edge.
    const loopLoads = new Map<number, Instruction[]>();   // variable -> loads inside loops
    const users = new Map<number, Instruction[]>();       // id -> instructions reading it
    const backEdges = new Map<number, Instruction[]>();   // value -> phis it reaches along a back edge
    m.instructions.forEach((ins) => {
      if (!ins.fn) return;
      if (ins.op === Op.Load && ins.loops.length) {
        const v = m.baseVariable(w[ins.start + 3]);
        let list = loopLoads.get(v);
        if (!list) loopLoads.set(v, (list = []));
        list.push(ins);
      }
      for (const o of m.operandWords(ins)) {
        let list = users.get(o);
        if (!list) users.set(o, (list = []));
        list.push(ins);
      }
      if (ins.op === Op.Phi) {
        for (let k = ins.start + 3; k + 1 < ins.start + ins.len; k += 2) {
          const def = m.defs.get(w[k]);
          if (!def || def.start <= ins.start) continue;   // defined before the phi: not a back edge
          let list = backEdges.get(w[k]);
          if (!list) backEdges.set(w[k], (list = []));
          list.push(ins);
        }
      }
    });
    const recurrence = (instructions: Instruction[], here: string): boolean => instructions.some((ins) => {
      if (ins.op === Op.Store && ins.loops.length) {
        // Carried into the next iteration: read in the loop, by another line, before this store.
        const loop = ins.loops[ins.loops.length - 1];
        return (loopLoads.get(m.baseVariable(w[ins.start + 1])) ?? [])
          .some((load) => load.loops.includes(loop) && load.start < ins.start && lineOf(load) !== here);
      }
      if (!definesValue(ins.op)) return false;
      return (backEdges.get(w[ins.start + 2]) ?? []).some((phi) => (users.get(w[phi.start + 2]) ?? []).some((u) => lineOf(u) !== here && lineOf(u) !== ""));
    });
    const lines = entry.functions.flatMap((f) => f.lines.map((l) => ({ f, l }))).sort((x, y) => y.l.weighted - x.l.weighted).slice(0, limits.lines ?? 32);
    for (const { f, l } of lines) {
      const part: AblationPart = { kind: "line", name: `${l.file ? `${l.file}:` : "line "}${l.line}`, functionId: f.id, functionName: f.name, file: l.file, line: l.line };
      const replace = new Set<Instruction>();
      const remove = new Set<Instruction>();
      const instructions: Instruction[] = [];
      let controlled = 0;
      for (const ins of m.instructions) {
        if (ins.fn !== f.id) continue;
        const loc = debug.locations[ins.ordinal];
        if (!loc || loc.line !== l.line || baseName(loc.file) !== l.file) continue;
        instructions.push(ins);
        if (!isValueOp(ins.op)) continue;
        if (ins.op === Op.FunctionCall && m.isVoid(w[ins.start + 1])) {
          if (!(m.uses.get(w[ins.start + 2]) ?? 0)) remove.add(ins);
          continue;
        }
        const r = why(ins);
        if (!r) replace.add(ins);
        else if (r === "control flow depends on it") controlled++;
      }
      // A load the line's other replaced instructions read is not the line's work: kept, it is also what a
      // replacement inside a loop derives an iteration's value from.
      const replacedOperands = new Set<number>();
      for (const ins of replace) if (ins.op !== Op.Load) for (const o of m.operandWords(ins)) replacedOperands.add(o);
      for (const ins of [...replace]) if (ins.op === Op.Load && replacedOperands.has(w[ins.start + 2])) replace.delete(ins);
      const here = instructions.length ? lineOf(instructions[0]) : "";
      if (!replace.size && !remove.size) {
        plan.skipped.push({ ...part, reason: controlled ? "control flow depends on what the line computes" : "the line computes nothing that can be replaced" });
      } else if (recurrence(instructions, here)) {
        plan.skipped.push({ ...part, reason: "it updates a value that other lines of its loop read every iteration: taking it out would let the compiler hoist the loop's work, and charge that to the line" });
      } else {
        const results = new Set<number>([...replace].map((ins) => w[ins.start + 2]));
        const written = new Set<number>(instructions.filter((ins) => ins.op === Op.Store).map((ins) => m.baseVariable(w[ins.start + 1])));
        candidates.push({ part, replace, remove, results, instructions, written });
      }
    }
  } else {
    plan.skipped.push({ kind: "line", name: "source lines", reason: "the module has no line information" });
  }

  // Textures: every sample, fetch, gather and read of one bound texture replaced. The texture is the
  // variable the image operand is loaded from, through OpSampledImage, OpImage and access chains.
  const textures = new Map<number, Instruction[]>();
  for (const ins of m.instructions) {
    if (!ins.fn || !reachable.has(ins.fn) || !isImageRead(ins.op)) continue;
    const texture = textureVariable(m, w[ins.start + 3]);
    if (texture === null) continue;
    let list = textures.get(texture);
    if (!list) textures.set(texture, (list = []));
    list.push(ins);
  }
  const rankedTextures = [...textures.entries()].sort((x, y) => y[1].length - x[1].length).slice(0, limits.textures ?? 16);
  for (const [texture, uses] of rankedTextures) {
    const set = m.sets.get(texture);
    const binding = m.bindings.get(texture);
    const name = m.names.get(texture) || (set !== undefined && binding !== undefined ? `set ${set}, binding ${binding}` : `texture ${texture}`);
    const part: AblationPart = { kind: "texture", name, ...(set !== undefined ? { set } : {}), ...(binding !== undefined ? { binding } : {}) };
    const replace = new Set(uses.filter((ins) => !why(ins)));
    if (!replace.size) plan.skipped.push({ ...part, reason: "control flow depends on what is read from it" });
    else candidates.push({ part, replace, remove: new Set(), results: new Set(), instructions: [], written: new Set() });
  }

  const upstream = upstreamParts(m, candidates);
  const offset = stageVariant ? 1 : 0;
  if (stageVariant) plan.variants.push(stageVariant);
  candidates.forEach((c, i) => {
    plan.variants.push({
      ...c.part, spirv: rewrite(m, stage, moduleEntry.index, c.replace, c.remove, c.written), edits: c.replace.size + c.remove.size,
      upstream: upstream[i].map((k) => k + offset),
    });
  });
  return plan;
}
