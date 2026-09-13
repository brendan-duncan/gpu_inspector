// The shader debugger's SPIR-V interpreter: one invocation of an entry point (a vertex, a fragment,
// a compute invocation) executed an instruction at a time, so a debugger can stop anywhere, show
// every value, and step by source line. Frames are explicit rather than JavaScript recursion.
//
// Inputs come from the capture: the vertex's attributes or the fragment's interpolated varyings
// (InvocationInputs), and the descriptor sets and push constants the draw had bound (ShaderBindings).
// Derivatives (dFdx, implicit-LOD sampling) need the neighbouring invocations of the pixel quad;
// a DerivativeSource (quad.ts) runs them in lockstep, and without one they are zero.
import {
  BUILTIN_NAMES, BuiltIn, Decoration, ExecutionModel, Op, StorageClass, type EntryPointInfo, type FunctionInfo, type Instruction,
  type SpirvModule,
} from "./module.js";
import { Dim, fetch, gather, implicitLod, sample } from "./sampling.js";
import {
  ImageValue, Pointer, SampledImageValue, SamplerValue, bufferLocation, cloneValue, mapScalars, normalize, readBuffer,
  runtimeArrayLength, scalarOf, zipScalars, type BufferStorage, type Cell, type DebugSampler, type DebugTexture, type ScalarKind,
  type Value,
} from "./values.js";

export interface ShaderBindings {
  /** A buffer descriptor's bound range (dynamic offset applied); null when not captured. */
  buffer(set: number, binding: number, element: number): Uint8Array | null;
  texture(set: number, binding: number, element: number): DebugTexture | null;
  sampler(set: number, binding: number, element: number): DebugSampler | null;
  /** The push constant bytes, from offset 0. */
  pushConstants: Uint8Array | null;
  /** Specialization: bytes of each constant's value, by SpecId. */
  specialization: Map<number, Uint8Array>;
}

export interface InvocationInputs {
  /** Built-in inputs by BuiltIn number (gl_FragCoord a vec4, gl_VertexIndex a number, ...). */
  builtins: Map<number, Value>;
  /** Input variables by location: their scalars in order. */
  locations: Map<number, number[]>;
}

export interface DerivativeSource {
  /** The screen-space derivatives of a value at a derivative point; "blocked" while the other invocations catch up. */
  derivative(invocation: Invocation, inst: Instruction, operand: Value): { dx: Value; dy: Value } | "blocked";
}

export type InvocationStatus = "running" | "blocked" | "returned" | "discarded" | "error";

export interface Frame {
  fn: FunctionInfo;
  /** Ordinal of the next instruction to execute. */
  pc: number;
  block: number;
  previousBlock: number;
  values: Map<number, Value>;
  /** The function's local variables, in the order they were declared. */
  locals: { id: number; cell: Cell; type: number }[];
  /** The caller's OpFunctionCall result id, 0 for the entry point. */
  resultId: number;
}

/** One result the stepped instructions produced: what the watch shows as "values on this line". */
export interface StepResult {
  inst: Instruction;
  id: number;
  value: Value;
}

export interface VariableView {
  id: number;
  name: string;
  type: number;
  value: Value;
  storage: number;
  location?: number;
  builtin?: number;
  set?: number;
  binding?: number;
}

const GLSL_STD_450 = "GLSL.std.450";
const MAX_STEPS = 50_000_000;

/** SPIR-V ImageOperands bits. */
const enum ImageOperand { Bias = 0x1, Lod = 0x2, Grad = 0x4, ConstOffset = 0x8, Offset = 0x10, ConstOffsets = 0x20, Sample = 0x40, MinLod = 0x80 }

function num(v: Value): number {
  return typeof v === "number" ? v : typeof v === "bigint" ? Number(v) : v === true ? 1 : 0;
}

function big(v: Value): bigint {
  return typeof v === "bigint" ? v : BigInt(Math.trunc(num(v)));
}

/** A value as the signed integer its bits say. */
function signed(v: Value, width: number): number | bigint {
  if (width === 64) return BigInt.asIntN(64, big(v));
  const n = num(v);
  if (width === 32) return n | 0;
  const mod = 2 ** width;
  const u = ((n % mod) + mod) % mod;
  return u >= mod / 2 ? u - mod : u;
}

/** A value as the unsigned integer its bits say. */
function unsigned(v: Value, width: number): number | bigint {
  if (width === 64) return BigInt.asUintN(64, big(v));
  const n = num(v);
  if (width === 32) return n >>> 0;
  const mod = 2 ** width;
  return ((n % mod) + mod) % mod;
}

function flat(v: Value): number[] {
  if (Array.isArray(v)) return v.flatMap(flat);
  return [num(v)];
}

export class Invocation {
  readonly module: SpirvModule;
  readonly entry: EntryPointInfo;
  readonly bindings: ShaderBindings;
  readonly inputs: InvocationInputs;
  derivatives: DerivativeSource | null;
  status: InvocationStatus = "running";
  error = "";
  /** Things the interpreter could not do faithfully: uncaptured resources, unsupported operations. */
  readonly warnings = new Set<string>();
  readonly frames: Frame[] = [];
  /** Global variables: their pointers, by id. */
  readonly globals = new Map<number, Pointer>();
  readonly constants: Map<number, unknown>;
  /** Demoted to a helper invocation (its outputs are discarded, execution goes on). */
  helper = false;
  steps = 0;
  /** Results of the instructions executed since takeResults(). */
  private _results: StepResult[] = [];
  /** Called with every value an instruction produces (the MCP tool's trace). */
  onResult: ((r: StepResult) => void) | null = null;

  constructor(module: SpirvModule, options: { entryPoint?: string; model?: number; bindings: ShaderBindings; inputs: InvocationInputs; derivatives?: DerivativeSource | null }) {
    this.module = module;
    const entry = module.entryPoint(options.entryPoint, options.model);
    if (!entry) throw new Error(`the module has no ${options.entryPoint ?? ""} entry point`);
    this.entry = entry;
    this.bindings = options.bindings;
    this.inputs = options.inputs;
    this.derivatives = options.derivatives ?? null;
    this.constants = new Map(module.constants);
    this._specialize();
    this._createGlobals();
    const fn = module.functions.get(entry.function);
    if (!fn || !fn.blocks.length) throw new Error(`the entry point ${entry.name} has no body`);
    this.frames.push(this._frame(fn, 0));
    this._skipNoops();
  }

  get stage(): "vertex" | "fragment" | "compute" | "other" {
    const m = this.entry.model;
    return m === ExecutionModel.Vertex ? "vertex" : m === ExecutionModel.Fragment ? "fragment" : m === ExecutionModel.GLCompute ? "compute" : "other";
  }

  /** Itself: an invocation steps itself (a PixelQuad steps one of four). */
  get invocation(): Invocation {
    return this;
  }

  get finished(): boolean {
    return this.status === "returned" || this.status === "discarded" || this.status === "error";
  }

  /** The instruction about to execute, null when finished. */
  get current(): Instruction | null {
    const frame = this.frames[this.frames.length - 1];
    return frame && !this.finished ? this.module.instructions[frame.pc] ?? null : null;
  }

  /** The results produced since the last call, and forgets them. */
  takeResults(): StepResult[] {
    const r = this._results;
    this._results = [];
    return r;
  }

  /** Executes one instruction (instructions that do nothing, like OpLine, are passed over with it). */
  step(): InvocationStatus {
    if (this.finished) return this.status;
    if (++this.steps > MAX_STEPS) return this._fail(`stopped after ${MAX_STEPS.toLocaleString()} instructions: an endless loop?`);
    try {
      let guard = 0;
      while (!this.finished) {
        const frame = this.frames[this.frames.length - 1];
        const inst = this.module.instructions[frame.pc];
        if (!inst) return this._fail("ran off the end of a function");
        if (this._isNoop(inst)) {
          frame.pc++;
          if (++guard > 100000) return this._fail("too many instructions without effect");
          continue;
        }
        const r = this._execute(frame, inst);
        this.status = r === "blocked" ? "blocked" : this.finished ? this.status : "running";
        // Stop on the next instruction that does something, so `current` has the source line about to run.
        if (r !== "blocked") this._skipNoops();
        return this.status;
      }
    } catch (e) {
      return this._fail(e instanceof Error ? e.message : String(e));
    }
    return this.status;
  }

  private _skipNoops(): void {
    const frame = this.frames[this.frames.length - 1];
    if (!frame || this.finished) return;
    while (frame.pc < this.module.instructions.length && this._isNoop(this.module.instructions[frame.pc])) frame.pc++;
  }

  /** Runs to the end (or a block on derivatives); for tests and the trace. */
  run(): InvocationStatus {
    while (!this.finished) {
      if (this.step() === "blocked") return "blocked";
    }
    return this.status;
  }

  // ---------------------------------------------------------------------------------------
  // What a debugger shows

  /** The outputs the invocation wrote: by location and built-in. */
  outputs(): VariableView[] {
    return this._interfaceVariables(StorageClass.Output);
  }

  inputVariables(): VariableView[] {
    return this._interfaceVariables(StorageClass.Input);
  }

  /** Uniform, storage and push constant blocks, images and samplers. */
  resourceVariables(): VariableView[] {
    const out: VariableView[] = [];
    for (const [id, ptr] of this.globals) {
      if (ptr.storage === StorageClass.Input || ptr.storage === StorageClass.Output) continue;
      if (ptr.storage === StorageClass.Private || ptr.storage === StorageClass.Workgroup) continue;
      out.push(this._view(id, ptr));
    }
    return out;
  }

  /** Private and workgroup variables. */
  privateVariables(): VariableView[] {
    const out: VariableView[] = [];
    for (const [id, ptr] of this.globals) {
      if (ptr.storage === StorageClass.Private || ptr.storage === StorageClass.Workgroup) out.push(this._view(id, ptr));
    }
    return out;
  }

  /** A frame's local variables and parameters (depth 0 is the innermost frame). */
  locals(depth = 0): VariableView[] {
    const frame = this.frames[this.frames.length - 1 - depth];
    if (!frame) return [];
    const out: VariableView[] = [];
    for (const p of frame.fn.params) {
      const v = frame.values.get(p.id);
      const value = v instanceof Pointer ? this._load(v) : v ?? null;
      out.push({ id: p.id, name: this.module.nameOf(p.id), type: v instanceof Pointer ? v.type : p.type, value, storage: StorageClass.Function });
    }
    for (const l of frame.locals) {
      out.push({ id: l.id, name: this.module.nameOf(l.id), type: l.type, value: cloneValue(l.cell.value), storage: StorageClass.Function });
    }
    return out;
  }

  /** A value an id has in a frame, for hovering over a name: SSA values and variables. */
  valueOf(id: number, depth = 0): Value | undefined {
    const frame = this.frames[this.frames.length - 1 - depth];
    const v = frame?.values.get(id) ?? this.globals.get(id);
    if (v === undefined) return this.constants.get(id) as Value | undefined;
    return v instanceof Pointer ? this._load(v) : v;
  }

  // ---------------------------------------------------------------------------------------
  // Setup

  private _specialize(): void {
    const m = this.module;
    if (this.bindings.specialization.size) {
      for (const [id, specId] of m.specIds) {
        const bytes = this.bindings.specialization.get(specId);
        if (!bytes) continue;
        const inst = m.instructions.find((i) => i.result === id && (i.op === Op.SpecConstant || i.op === Op.SpecConstantTrue || i.op === Op.SpecConstantFalse));
        if (!inst) continue;
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        if (inst.op === Op.SpecConstantTrue || inst.op === Op.SpecConstantFalse) {
          this.constants.set(id, bytes.byteLength >= 4 ? view.getUint32(0, true) !== 0 : bytes[0] !== 0);
        } else if (inst.op === Op.SpecConstant) {
          const words = new Uint32Array(Math.max(1, Math.ceil(bytes.byteLength / 4)));
          for (let i = 0; i < words.length && (i + 1) * 4 <= bytes.byteLength; i++) words[i] = view.getUint32(i * 4, true);
          this.constants.set(id, m.scalarFromWords(inst.resultType, words));
        }
      }
    }
    // Composites and operations over spec constants, in module order.
    for (const inst of m.instructions) {
      if (inst.op === Op.SpecConstantComposite) {
        this.constants.set(inst.result, Array.from(inst.words.subarray(2)).map((id) => this.constants.get(id)));
      } else if (inst.op === Op.SpecConstantOp) {
        const opcode = inst.words[2];
        const operands = inst.words.subarray(3);
        const fake: Instruction = { op: opcode, words: new Uint32Array([inst.resultType, inst.result, ...operands]), index: inst.index, resultType: inst.resultType, result: inst.result };
        const frame = { values: new Map<number, Value>() } as Frame;
        try {
          const value = this._compute(frame, fake);
          if (value !== undefined) this.constants.set(inst.result, value);
        } catch {
          this.warnings.add(`specialization constant operation ${opcode} is not evaluated`);
        }
      }
    }
    // Array lengths that depend on specialization.
    for (const t of m.types.values()) if (t.kind === "array") t.length = Number(this.constants.get(t.lengthId) ?? t.length);
  }

  private _createGlobals(): void {
    const m = this.module;
    for (const [id, g] of m.globals) {
      const ptrType = m.types.get(g.type);
      if (ptrType?.kind !== "pointer") continue;
      const pointee = ptrType.pointee;
      const cell: Cell = { value: null };
      const set = m.decoration(id, Decoration.DescriptorSet)?.[0] ?? 0;
      const binding = m.decoration(id, Decoration.Binding)?.[0] ?? 0;
      switch (g.storage) {
        case StorageClass.Uniform:
        case StorageClass.StorageBuffer:
        case StorageClass.PushConstant: {
          const t = m.types.get(pointee);
          if (g.storage !== StorageClass.PushConstant && (t?.kind === "array" || t?.kind === "runtimeArray")) {
            // An array of blocks: one buffer per element.
            const element = t.kind === "array" || t.kind === "runtimeArray" ? t.element : pointee;
            const count = t.kind === "array" ? t.length : 1;
            cell.value = Array.from({ length: count }, (_, i) => {
              const bytes = this.bindings.buffer(set, binding, i);
              if (!bytes) this.warnings.add(`set ${set} binding ${binding}[${i}] (${m.nameOf(id)}) was not captured: it reads as zeros`);
              return { buffer: { bytes: bytes ?? new Uint8Array(0), type: element, overrides: new Map() } } as unknown as Value;
            });
            cell.bufferArray = true;
          } else {
            const bytes = g.storage === StorageClass.PushConstant ? this.bindings.pushConstants : this.bindings.buffer(set, binding, 0);
            if (!bytes) {
              this.warnings.add(g.storage === StorageClass.PushConstant ? "the push constants were not captured: they read as zeros"
                : `set ${set} binding ${binding} (${m.nameOf(id)}) was not captured: it reads as zeros`);
            }
            cell.buffer = { bytes: bytes ?? new Uint8Array(0), type: pointee, overrides: new Map() };
          }
          break;
        }
        case StorageClass.UniformConstant:
          cell.value = this._resource(id, pointee, set, binding, 0);
          break;
        case StorageClass.Input:
          cell.value = this._input(id, pointee);
          break;
        case StorageClass.Output:
        case StorageClass.Private:
        case StorageClass.Workgroup:
        default:
          cell.value = g.initializer ? cloneValue(this.constants.get(g.initializer) as Value) : (m.zero(pointee) as Value);
          break;
      }
      this.globals.set(id, new Pointer(cell, [], pointee, g.storage, id));
    }
  }

  private _resource(id: number, type: number, set: number, binding: number, element: number): Value {
    const m = this.module;
    const t = m.types.get(type);
    const label = `set ${set} binding ${binding}${element ? `[${element}]` : ""} (${m.nameOf(id)})`;
    if (t?.kind === "array" || t?.kind === "runtimeArray") {
      const count = t.kind === "array" ? t.length : 1;
      return Array.from({ length: count }, (_, i) => this._resource(id, t.element, set, binding, i));
    }
    const texture = (): DebugTexture | null => {
      const tex = this.bindings.texture(set, binding, element);
      if (!tex) this.warnings.add(`${label}: its image was not captured, so it reads as black`);
      return tex;
    };
    if (t?.kind === "image") return new ImageValue(texture(), label);
    if (t?.kind === "sampler") return new SamplerValue(this.bindings.sampler(set, binding, element), label);
    if (t?.kind === "sampledImage") return new SampledImageValue(new ImageValue(texture(), label), new SamplerValue(this.bindings.sampler(set, binding, element), label));
    return null;
  }

  /** An input variable's value: a built-in, or the scalars at its location shaped into its type. */
  private _input(id: number, type: number): Value {
    const m = this.module;
    const builtin = m.decoration(id, Decoration.BuiltIn)?.[0];
    if (builtin !== undefined) {
      const v = this.inputs.builtins.get(builtin);
      if (v === undefined) {
        this.warnings.add(`${BUILTIN_NAMES[builtin] ?? `built-in ${builtin}`} has no value here: it reads as zero`);
        return m.zero(type) as Value;
      }
      return this._shape(type, flat(v), { at: 0 });
    }
    const t = m.types.get(type);
    if (t?.kind === "struct") {
      // An input block: each member at its own location.
      const base = m.decoration(id, Decoration.Location)?.[0] ?? 0;
      return t.members.map((member, i) => {
        const location = m.memberDecoration(type, i, Decoration.Location)?.[0] ?? base + i;
        return this._shape(member, this.inputs.locations.get(location) ?? [], { at: 0 });
      });
    }
    const location = m.decoration(id, Decoration.Location)?.[0];
    if (location === undefined) return m.zero(type) as Value;
    const scalars = this.inputs.locations.get(location);
    if (!scalars) {
      this.warnings.add(`input ${m.nameOf(id)} (location ${location}) has no value here: it reads as zero`);
      return m.zero(type) as Value;
    }
    // Arrays and matrices take consecutive locations.
    if (t?.kind === "array" || t?.kind === "matrix") {
      const count = t.kind === "array" ? t.length : t.count;
      const element = t.kind === "array" ? t.element : t.column;
      return Array.from({ length: count }, (_, i) => this._shape(element, this.inputs.locations.get(location + i) ?? [], { at: 0 }));
    }
    return this._shape(type, scalars, { at: 0 });
  }

  /** Scalars poured into a type in order (missing ones zero, a missing alpha one). */
  private _shape(type: number, scalars: number[], cursor: { at: number }): Value {
    const m = this.module;
    const t = m.types.get(type);
    if (!t) return 0;
    const s = scalarOf(m, type);
    switch (t.kind) {
      case "bool": case "int": case "float": {
        const v = scalars[cursor.at++] ?? 0;
        return s ? normalize(v, s) : v;
      }
      case "vector":
        return Array.from({ length: t.count }, () => this._shape(t.element, scalars, cursor));
      case "matrix":
        return Array.from({ length: t.count }, () => this._shape(t.column, scalars, cursor));
      case "array":
        return Array.from({ length: t.length }, () => this._shape(t.element, scalars, cursor));
      case "struct":
        return t.members.map((member) => this._shape(member, scalars, cursor));
      default:
        return null;
    }
  }

  private _frame(fn: FunctionInfo, resultId: number): Frame {
    const first = fn.blocks[0];
    return { fn, pc: first.start + 1, block: first.label, previousBlock: 0, values: new Map(), locals: [], resultId };
  }

  private _interfaceVariables(storage: number): VariableView[] {
    const out: VariableView[] = [];
    for (const id of this.entry.interface) {
      const ptr = this.globals.get(id);
      if (!ptr || ptr.storage !== storage) continue;
      out.push(this._view(id, ptr));
    }
    return out;
  }

  private _view(id: number, ptr: Pointer): VariableView {
    const m = this.module;
    // An unnamed block variable (gl_PerVertex) goes by its block's name.
    const name = m.names.get(id) || BUILTIN_NAMES[m.decoration(id, Decoration.BuiltIn)?.[0] ?? -1] || m.names.get(ptr.type) || m.nameOf(id);
    return {
      id, name, type: ptr.type, value: this._load(ptr, 256), storage: ptr.storage,
      location: m.decoration(id, Decoration.Location)?.[0], builtin: m.decoration(id, Decoration.BuiltIn)?.[0],
      set: m.decoration(id, Decoration.DescriptorSet)?.[0], binding: m.decoration(id, Decoration.Binding)?.[0],
    };
  }

  // ---------------------------------------------------------------------------------------
  // Execution

  private _fail(message: string): InvocationStatus {
    const inst = this.current;
    const loc = inst ? this.module.debug?.locations[inst.index] : null;
    this.error = loc ? `${message} (line ${loc.line})` : message;
    this.status = "error";
    return this.status;
  }

  private _isNoop(inst: Instruction): boolean {
    switch (inst.op) {
      case Op.Nop: case Op.Line: case Op.NoLine: case Op.SelectionMerge: case Op.LoopMerge: case Op.Label:
        return true;
      case Op.ExtInst:
        return this.module.extSets.get(inst.words[2])?.startsWith("NonSemantic.") ?? false;
      default:
        return false;
    }
  }

  value(frame: Frame, id: number): Value {
    const v = frame.values.get(id);
    if (v !== undefined) return v;
    const g = this.globals.get(id);
    if (g) return g;
    if (this.constants.has(id)) return cloneValue(this.constants.get(id) as Value);
    return 0;
  }

  private _record(frame: Frame, inst: Instruction, value: Value): void {
    frame.values.set(inst.result, value);
    const r = { inst, id: inst.result, value };
    this._results.push(r);
    if (this._results.length > 4096) this._results.splice(0, 2048);
    this.onResult?.(r);
  }

  private _branch(frame: Frame, label: number): void {
    const block = frame.fn.blockByLabel.get(label);
    if (!block) throw new Error(`branch to a missing block %${label}`);
    frame.previousBlock = frame.block;
    frame.block = label;
    frame.pc = block.start + 1;
  }

  private _execute(frame: Frame, inst: Instruction): "ok" | "blocked" {
    const w = inst.words;
    switch (inst.op) {
      case Op.Branch:
        this._branch(frame, w[0]);
        return "ok";
      case Op.BranchConditional:
        this._branch(frame, this.value(frame, w[0]) ? w[1] : w[2]);
        return "ok";
      case Op.Switch: {
        const selector = this.value(frame, w[0]);
        const selectorType = this._typeOfId(frame, w[0]);
        const wide = selectorType ? (scalarOf(this.module, selectorType)?.width ?? 32) > 32 : false;
        let target = w[1];
        for (let i = 2; i < w.length; i += wide ? 3 : 2) {
          const literal = wide ? (BigInt(w[i + 1]) << 32n) | BigInt(w[i]) : w[i];
          const matches = wide ? big(selector) === BigInt.asIntN(64, literal as bigint) || big(selector) === (literal as bigint)
            : (num(selector) >>> 0) === ((literal as number) >>> 0);
          if (matches) {
            target = w[i + (wide ? 2 : 1)];
            break;
          }
        }
        this._branch(frame, target);
        return "ok";
      }
      case Op.Return:
      case Op.ReturnValue: {
        const value = inst.op === Op.ReturnValue ? cloneValue(this.value(frame, w[0])) : null;
        this.frames.pop();
        const caller = this.frames[this.frames.length - 1];
        if (!caller) {
          this.status = "returned";
          return "ok";
        }
        if (frame.resultId) this._record(caller, { ...this.module.instructions[caller.pc], result: frame.resultId }, value);
        caller.pc++;
        return "ok";
      }
      case Op.Kill:
      case Op.TerminateInvocation:
        this.status = "discarded";
        return "ok";
      case Op.DemoteToHelperInvocation:
        this.helper = true;
        frame.pc++;
        return "ok";
      case Op.Unreachable:
        this._fail("reached OpUnreachable");
        return "ok";
      case Op.FunctionCall: {
        const fn = this.module.functions.get(w[2]);
        if (!fn) throw new Error(`call to a missing function %${w[2]}`);
        const callee = this._frame(fn, inst.result);
        fn.params.forEach((p, i) => callee.values.set(p.id, this.value(frame, w[3 + i])));
        this.frames.push(callee);
        return "ok";
      }
      case Op.Store: {
        const ptr = this.value(frame, w[0]);
        if (!(ptr instanceof Pointer)) throw new Error("OpStore through something that is not a pointer");
        this._store(ptr, cloneValue(this.value(frame, w[1])));
        frame.pc++;
        this._results.push({ inst, id: ptr.variable, value: this._load(ptr) });
        return "ok";
      }
      case Op.CopyMemory: {
        const target = this.value(frame, w[0]);
        const source = this.value(frame, w[1]);
        if (target instanceof Pointer && source instanceof Pointer) this._store(target, this._load(source));
        frame.pc++;
        return "ok";
      }
      case Op.ImageWrite: {
        const image = this.value(frame, w[0]);
        const coord = flat(this.value(frame, w[1]));
        const texel = flat(this.value(frame, w[2]));
        if (image instanceof ImageValue && image.texture) {
          image.texture.writes ??= new Map();
          image.texture.writes.set(`0/${coord[2] ?? 0}/${coord[0]}/${coord[1] ?? 0}`, [...texel, 0, 0, 0, 1].slice(0, 4));
        }
        frame.pc++;
        return "ok";
      }
      case Op.Variable: {
        const ptrType = this.module.types.get(inst.resultType);
        const pointee = ptrType?.kind === "pointer" ? ptrType.pointee : 0;
        const cell: Cell = { value: w[3] ? cloneValue(this.value(frame, w[3])) : (this.module.zero(pointee) as Value) };
        frame.locals.push({ id: inst.result, cell, type: pointee });
        this._record(frame, inst, new Pointer(cell, [], pointee, StorageClass.Function, inst.result));
        frame.pc++;
        return "ok";
      }
      case Op.Phi: {
        let value: Value = 0;
        for (let i = 2; i + 1 < w.length; i += 2) {
          if (w[i + 1] === frame.previousBlock) {
            value = cloneValue(this.value(frame, w[i]));
            break;
          }
        }
        this._record(frame, inst, value);
        frame.pc++;
        return "ok";
      }
      default: {
        if (!inst.result) {
          // Instructions without a result the interpreter has no use for.
          frame.pc++;
          return "ok";
        }
        const value = this._compute(frame, inst);
        if (value === "blocked" as unknown) return "blocked";
        this._record(frame, inst, value as Value);
        if (this.status === "running" || this.status === "blocked") frame.pc++;
        return "ok";
      }
    }
  }

  private _typeOfId(_frame: Frame, id: number): number {
    return this.module.idTypes.get(id) ?? 0;
  }

  /** The value an instruction with a result computes. May return "blocked" for derivative points. */
  private _compute(frame: Frame, inst: Instruction): Value {
    const m = this.module;
    const w = inst.words;
    const v = (i: number): Value => this.value(frame, w[i]);
    const rt = inst.resultType;
    const s = scalarOf(m, rt);
    const norm = (x: Value): Value => (s ? mapScalars(x, (e) => normalize(e, s)) : x);
    const opWidth = (i: number): number => {
      const t = this._typeOfId(frame, w[i]);
      return scalarOf(m, t)?.width ?? 32;
    };

    switch (inst.op) {
      case Op.Undef:
      case Op.ConstantNull:
        return m.zero(rt) as Value;
      case Op.Load: {
        const ptr = v(2);
        if (!(ptr instanceof Pointer)) throw new Error("OpLoad of something that is not a pointer");
        return this._load(ptr);
      }
      case Op.AccessChain:
      case Op.InBoundsAccessChain:
      case Op.PtrAccessChain:
      case Op.InBoundsPtrAccessChain: {
        const base = v(2);
        if (!(base instanceof Pointer)) throw new Error("access chain on something that is not a pointer");
        const first = inst.op === Op.PtrAccessChain || inst.op === Op.InBoundsPtrAccessChain ? 4 : 3;
        const indices = Array.from(w.subarray(first)).map((id) => num(this.value(frame, id)));
        const ptrType = m.types.get(rt);
        return new Pointer(base.cell, [...base.path, ...indices], ptrType?.kind === "pointer" ? ptrType.pointee : 0, base.storage, base.variable);
      }
      case Op.ArrayLength: {
        const ptr = v(2);
        const member = w[3];
        if (!(ptr instanceof Pointer)) return 0;
        const storage = this._bufferOf(ptr);
        if (!storage) {
          const value = this._load(ptr);
          return Array.isArray(value) && Array.isArray(value[member]) ? (value[member] as Value[]).length : 0;
        }
        const struct = m.types.get(ptr.type);
        if (struct?.kind !== "struct") return 0;
        const loc = bufferLocation(m, storage.buffer.type, [...storage.path, member], storage.buffer.bytes.byteLength);
        return loc ? runtimeArrayLength(m, struct.members[member], storage.buffer.bytes.byteLength, loc.at) : 0;
      }
      case Op.CopyObject:
      case Op.CopyLogical:
        return cloneValue(v(2));
      case Op.CompositeConstruct: {
        const parts = Array.from(w.subarray(2)).map((id) => cloneValue(this.value(frame, id)));
        const t = m.types.get(rt);
        if (t?.kind === "vector") return norm(parts.flatMap((p) => (Array.isArray(p) ? p : [p])));
        return parts;
      }
      case Op.CompositeExtract: {
        let value = v(2);
        for (const index of w.subarray(3)) value = Array.isArray(value) ? value[index] : 0;
        return cloneValue(value);
      }
      case Op.CompositeInsert: {
        const composite = cloneValue(v(3));
        const indices = Array.from(w.subarray(4));
        let at = composite as Value[];
        for (let i = 0; i < indices.length - 1; i++) at = at[indices[i]] as Value[];
        at[indices[indices.length - 1]] = cloneValue(v(2));
        return composite;
      }
      case Op.VectorExtractDynamic: {
        const vec = v(2) as Value[];
        const index = num(v(3));
        return cloneValue(vec[index] ?? 0);
      }
      case Op.VectorInsertDynamic: {
        const vec = cloneValue(v(2)) as Value[];
        const index = num(v(4));
        if (index >= 0 && index < vec.length) vec[index] = v(3);
        return vec;
      }
      case Op.VectorShuffle: {
        const a = v(2) as Value[];
        const b = v(3) as Value[];
        return Array.from(w.subarray(4)).map((c) => (c === 0xffffffff ? 0 : c < a.length ? a[c] : b[c - a.length]));
      }
      case Op.Transpose: {
        const mat = v(2) as number[][];
        const rows = mat[0]?.length ?? 0;
        return Array.from({ length: rows }, (_, r) => mat.map((col) => col[r]));
      }
      case Op.SampledImage: {
        const image = v(2);
        const sampler = v(3);
        return new SampledImageValue(image instanceof ImageValue ? image : new ImageValue(null, "?"), sampler instanceof SamplerValue ? sampler : new SamplerValue(null, "?"));
      }
      case Op.Image: {
        const si = v(2);
        return si instanceof SampledImageValue ? si.image : si;
      }

      // Conversions
      case Op.ConvertFToU:
      case Op.ConvertFToS:
        return norm(mapScalars(v(2), (x) => {
          const n = num(x);
          return Number.isFinite(n) ? Math.trunc(n) : 0;
        }));
      case Op.ConvertSToF: {
        const width = opWidth(2);
        return norm(mapScalars(v(2), (x) => Number(signed(x, width))));
      }
      case Op.ConvertUToF: {
        const width = opWidth(2);
        return norm(mapScalars(v(2), (x) => Number(unsigned(x, width))));
      }
      case Op.UConvert: {
        const width = opWidth(2);
        return norm(mapScalars(v(2), (x) => unsigned(x, width)));
      }
      case Op.SConvert: {
        const width = opWidth(2);
        return norm(mapScalars(v(2), (x) => signed(x, width)));
      }
      case Op.FConvert:
        return norm(v(2));
      case Op.QuantizeToF16:
        return norm(mapScalars(v(2), (x) => {
          const n = num(x);
          if (Math.abs(n) > 65504) return n > 0 ? Infinity : -Infinity;
          return Math.abs(n) < 2 ** -14 ? 0 : n;
        }));
      case Op.SatConvertSToU:
        return norm(mapScalars(v(2), (x) => Math.max(0, num(x))));
      case Op.SatConvertUToS:
        return norm(v(2));
      case Op.Bitcast:
        return this._bitcast(v(2), this._typeOfId(frame, w[2]), rt);

      // Arithmetic
      case Op.SNegate:
        return norm(mapScalars(v(2), (x) => (typeof x === "bigint" ? -x : -num(x))));
      case Op.FNegate:
        return norm(mapScalars(v(2), (x) => -num(x)));
      case Op.IAdd:
        return norm(zipScalars(v(2), v(3), (a, b) => (s?.width === 64 ? big(a) + big(b) : num(a) + num(b))));
      case Op.ISub:
        return norm(zipScalars(v(2), v(3), (a, b) => (s?.width === 64 ? big(a) - big(b) : num(a) - num(b))));
      case Op.IMul:
        return norm(zipScalars(v(2), v(3), (a, b) => (s?.width === 64 ? big(a) * big(b) : Math.imul(num(a), num(b)))));
      case Op.UDiv:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const width = s?.width ?? 32;
          const x = unsigned(a, width), y = unsigned(b, width);
          if (typeof x === "bigint") return y === 0n ? 0n : x / (y as bigint);
          return y === 0 ? 0 : Math.floor(x / (y as number));
        }));
      case Op.SDiv:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const width = s?.width ?? 32;
          const x = signed(a, width), y = signed(b, width);
          if (typeof x === "bigint") return y === 0n ? 0n : x / (y as bigint);
          return y === 0 ? 0 : Math.trunc(x / (y as number));
        }));
      case Op.UMod:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const width = s?.width ?? 32;
          const x = unsigned(a, width), y = unsigned(b, width);
          if (typeof x === "bigint") return y === 0n ? 0n : x % (y as bigint);
          return y === 0 ? 0 : x % (y as number);
        }));
      case Op.SRem:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const width = s?.width ?? 32;
          const x = signed(a, width), y = signed(b, width);
          if (typeof x === "bigint") return y === 0n ? 0n : x % (y as bigint);
          return y === 0 ? 0 : x % (y as number);
        }));
      case Op.SMod:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const width = s?.width ?? 32;
          const x = signed(a, width), y = signed(b, width);
          if (typeof x === "bigint") {
            if (y === 0n) return 0n;
            const r = x % (y as bigint);
            return r !== 0n && (r < 0n) !== ((y as bigint) < 0n) ? r + (y as bigint) : r;
          }
          if (y === 0) return 0;
          const r = x % (y as number);
          return r !== 0 && (r < 0) !== ((y as number) < 0) ? r + (y as number) : r;
        }));
      case Op.FAdd:
        return norm(zipScalars(v(2), v(3), (a, b) => num(a) + num(b)));
      case Op.FSub:
        return norm(zipScalars(v(2), v(3), (a, b) => num(a) - num(b)));
      case Op.FMul:
        return norm(zipScalars(v(2), v(3), (a, b) => num(a) * num(b)));
      case Op.FDiv:
        return norm(zipScalars(v(2), v(3), (a, b) => num(a) / num(b)));
      case Op.FRem:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const x = num(a), y = num(b);
          return x - y * Math.trunc(x / y);
        }));
      case Op.FMod:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const x = num(a), y = num(b);
          return x - y * Math.floor(x / y);
        }));
      case Op.VectorTimesScalar:
      case Op.MatrixTimesScalar: {
        const k = num(v(3));
        return norm(mapScalars(v(2), (x) => num(x) * k));
      }
      case Op.VectorTimesMatrix: {
        const vec = flat(v(2));
        const mat = v(3) as number[][];
        return norm(mat.map((col) => col.reduce((sum, c, r) => sum + num(c) * vec[r], 0)));
      }
      case Op.MatrixTimesVector: {
        const mat = v(2) as number[][];
        const vec = flat(v(3));
        const rows = mat[0]?.length ?? 0;
        return norm(Array.from({ length: rows }, (_, r) => mat.reduce((sum, col, c) => sum + num(col[r]) * vec[c], 0)));
      }
      case Op.MatrixTimesMatrix: {
        const a = v(2) as number[][];
        const b = v(3) as number[][];
        const rows = a[0]?.length ?? 0;
        return norm(b.map((bcol) => Array.from({ length: rows }, (_, r) => a.reduce((sum, acol, k) => sum + num(acol[r]) * num(bcol[k]), 0))));
      }
      case Op.OuterProduct: {
        const a = flat(v(2));
        const b = flat(v(3));
        return norm(b.map((bc) => a.map((ar) => ar * bc)));
      }
      case Op.Dot: {
        const a = flat(v(2));
        const b = flat(v(3));
        return norm(a.reduce((sum, x, i) => sum + x * b[i], 0));
      }
      case Op.IAddCarry:
      case Op.ISubBorrow:
      case Op.UMulExtended:
      case Op.SMulExtended:
        return this._extendedArithmetic(inst.op, v(2), v(3), rt);

      // Logic and comparison
      case Op.Any:
        return flat(v(2)).some((x) => x !== 0);
      case Op.All:
        return flat(v(2)).every((x) => x !== 0);
      case Op.IsNan:
        return mapScalars(v(2), (x) => Number.isNaN(num(x)));
      case Op.IsInf:
        return mapScalars(v(2), (x) => !Number.isFinite(num(x)) && !Number.isNaN(num(x)));
      case Op.IsFinite:
        return mapScalars(v(2), (x) => Number.isFinite(num(x)));
      case Op.IsNormal:
        return mapScalars(v(2), (x) => Number.isFinite(num(x)) && num(x) !== 0);
      case Op.SignBitSet:
        return mapScalars(v(2), (x) => num(x) < 0 || Object.is(num(x), -0));
      case Op.LogicalEqual:
        return zipScalars(v(2), v(3), (a, b) => Boolean(a) === Boolean(b));
      case Op.LogicalNotEqual:
        return zipScalars(v(2), v(3), (a, b) => Boolean(a) !== Boolean(b));
      case Op.LogicalOr:
        return zipScalars(v(2), v(3), (a, b) => Boolean(a) || Boolean(b));
      case Op.LogicalAnd:
        return zipScalars(v(2), v(3), (a, b) => Boolean(a) && Boolean(b));
      case Op.LogicalNot:
        return mapScalars(v(2), (a) => !a);
      case Op.Select: {
        const c = v(2);
        const a = v(3);
        const b = v(4);
        if (Array.isArray(c)) return (a as Value[]).map((x, i) => (c[i] ? x : (b as Value[])[i]));
        return cloneValue(c ? a : b);
      }
      case Op.IEqual:
        return zipScalars(v(2), v(3), (a, b) => (typeof a === "bigint" || typeof b === "bigint" ? BigInt.asUintN(64, big(a)) === BigInt.asUintN(64, big(b)) : num(a) >>> 0 === num(b) >>> 0 || num(a) === num(b)));
      case Op.INotEqual:
        return zipScalars(v(2), v(3), (a, b) => (typeof a === "bigint" || typeof b === "bigint" ? BigInt.asUintN(64, big(a)) !== BigInt.asUintN(64, big(b)) : !(num(a) >>> 0 === num(b) >>> 0 || num(a) === num(b))));
      case Op.UGreaterThan: case Op.UGreaterThanEqual: case Op.ULessThan: case Op.ULessThanEqual: {
        const width = opWidth(2);
        return zipScalars(v(2), v(3), (a, b) => {
          const x = unsigned(a, width), y = unsigned(b, width);
          return inst.op === Op.UGreaterThan ? x > y : inst.op === Op.UGreaterThanEqual ? x >= y : inst.op === Op.ULessThan ? x < y : x <= y;
        });
      }
      case Op.SGreaterThan: case Op.SGreaterThanEqual: case Op.SLessThan: case Op.SLessThanEqual: {
        const width = opWidth(2);
        return zipScalars(v(2), v(3), (a, b) => {
          const x = signed(a, width), y = signed(b, width);
          return inst.op === Op.SGreaterThan ? x > y : inst.op === Op.SGreaterThanEqual ? x >= y : inst.op === Op.SLessThan ? x < y : x <= y;
        });
      }
      case Op.FOrdEqual: case Op.FUnordEqual: case Op.FOrdNotEqual: case Op.FUnordNotEqual: case Op.FOrdLessThan:
      case Op.FUnordLessThan: case Op.FOrdGreaterThan: case Op.FUnordGreaterThan: case Op.FOrdLessThanEqual:
      case Op.FUnordLessThanEqual: case Op.FOrdGreaterThanEqual: case Op.FUnordGreaterThanEqual: case Op.LessOrGreater:
      case Op.Ordered: case Op.Unordered:
        return zipScalars(v(2), v(3), (a, b) => floatCompare(inst.op, num(a), num(b)));

      // Bits
      case Op.ShiftRightLogical:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const width = s?.width ?? 32;
          if (width === 64) return BigInt.asUintN(64, big(a)) >> big(b);
          return (unsigned(a, width) as number) >>> num(b);
        }));
      case Op.ShiftRightArithmetic:
        return norm(zipScalars(v(2), v(3), (a, b) => {
          const width = s?.width ?? 32;
          if (width === 64) return BigInt.asIntN(64, big(a)) >> big(b);
          return (signed(a, width) as number) >> num(b);
        }));
      case Op.ShiftLeftLogical:
        return norm(zipScalars(v(2), v(3), (a, b) => (s?.width === 64 ? big(a) << big(b) : num(a) << num(b))));
      case Op.BitwiseOr:
        return norm(zipScalars(v(2), v(3), (a, b) => (s?.width === 64 ? big(a) | big(b) : num(a) | num(b))));
      case Op.BitwiseXor:
        return norm(zipScalars(v(2), v(3), (a, b) => (s?.width === 64 ? big(a) ^ big(b) : num(a) ^ num(b))));
      case Op.BitwiseAnd:
        return norm(zipScalars(v(2), v(3), (a, b) => (s?.width === 64 ? big(a) & big(b) : num(a) & num(b))));
      case Op.Not:
        return norm(mapScalars(v(2), (a) => (s?.width === 64 ? ~big(a) : ~num(a))));
      case Op.BitFieldInsert: {
        const offset = num(v(4)), count = num(v(5));
        return norm(zipScalars(v(2), v(3), (base, insert) => {
          const mask = count >= 32 ? 0xffffffff : ((1 << count) - 1) << offset;
          return (num(base) & ~mask) | ((num(insert) << offset) & mask);
        }));
      }
      case Op.BitFieldSExtract:
      case Op.BitFieldUExtract: {
        const offset = num(v(3)), count = num(v(4));
        return norm(mapScalars(v(2), (base) => {
          if (count === 0) return 0;
          const shifted = (num(base) >>> offset) & (count >= 32 ? 0xffffffff : (1 << count) - 1);
          if (inst.op === Op.BitFieldUExtract) return shifted;
          return count < 32 && shifted & (1 << (count - 1)) ? shifted - (1 << count) : shifted | 0;
        }));
      }
      case Op.BitReverse:
        return norm(mapScalars(v(2), (a) => {
          let x = num(a) >>> 0, r = 0;
          for (let i = 0; i < 32; i++) {
            r = (r << 1) | (x & 1);
            x >>>= 1;
          }
          return r >>> 0;
        }));
      case Op.BitCount:
        return norm(mapScalars(v(2), (a) => {
          let x = num(a) >>> 0, c = 0;
          while (x) {
            c += x & 1;
            x >>>= 1;
          }
          return c;
        }));

      // Derivatives
      case Op.DPdx: case Op.DPdy: case Op.Fwidth: case Op.DPdxFine: case Op.DPdyFine: case Op.FwidthFine:
      case Op.DPdxCoarse: case Op.DPdyCoarse: case Op.FwidthCoarse: {
        const operand = v(2);
        const d = this._derivative(inst, operand);
        if (d === "blocked") return "blocked" as unknown as Value;
        const x = inst.op === Op.DPdx || inst.op === Op.DPdxFine || inst.op === Op.DPdxCoarse;
        const y = inst.op === Op.DPdy || inst.op === Op.DPdyFine || inst.op === Op.DPdyCoarse;
        if (x) return norm(d.dx);
        if (y) return norm(d.dy);
        return norm(zipScalars(d.dx, d.dy, (a, b) => Math.abs(num(a)) + Math.abs(num(b))));
      }
      case Op.IsHelperInvocation:
        return this.helper;

      // Images
      case Op.ImageSampleImplicitLod: case Op.ImageSampleExplicitLod: case Op.ImageSampleDrefImplicitLod:
      case Op.ImageSampleDrefExplicitLod: case Op.ImageSampleProjImplicitLod: case Op.ImageSampleProjExplicitLod:
      case Op.ImageSampleProjDrefImplicitLod: case Op.ImageSampleProjDrefExplicitLod:
        return this._sample(frame, inst, s);
      case Op.ImageFetch:
      case Op.ImageRead: {
        const image = v(2);
        const img = image instanceof SampledImageValue ? image.image : image;
        const type = img instanceof ImageValue ? this._imageType(frame, w[2]) : null;
        const coord = flat(v(3)).map(Math.trunc);
        let lod = 0;
        if (inst.op === Op.ImageFetch && w.length > 4) {
          const mask = w[4];
          if (mask & ImageOperand.Lod) lod = num(this.value(frame, w[5]));
        }
        const texel = fetch(img instanceof ImageValue ? img.texture : null, coord, lod, type?.dim ?? Dim.D2, type?.arrayed ?? false);
        return this._texelResult(texel, rt, s, img instanceof ImageValue ? img.texture : null);
      }
      case Op.ImageGather:
      case Op.ImageDrefGather: {
        const si = v(2);
        const coord = flat(v(3));
        const dref = inst.op === Op.ImageDrefGather ? num(v(4)) : undefined;
        const component = inst.op === Op.ImageGather ? num(v(4)) : 0;
        const tex = si instanceof SampledImageValue ? si.image.texture : null;
        const smp = si instanceof SampledImageValue ? si.sampler.sampler : null;
        return norm(gather(tex, smp, coord, component, dref));
      }
      case Op.ImageQuerySize:
      case Op.ImageQuerySizeLod: {
        const image = v(2);
        const img = image instanceof SampledImageValue ? image.image : image;
        const tex = img instanceof ImageValue ? img.texture : null;
        const lod = inst.op === Op.ImageQuerySizeLod ? num(v(3)) : 0;
        const type = this._imageType(frame, w[2]);
        const size = tex ? [Math.max(1, tex.width >> lod), Math.max(1, tex.height >> lod), Math.max(1, tex.depth >> lod)] : [0, 0, 0];
        const dims = type?.dim === Dim.D1 ? 1 : type?.dim === Dim.D3 ? 3 : 2;
        const out = size.slice(0, dims);
        if (type?.arrayed) out.push(tex ? (type.dim === Dim.Cube ? tex.layers / 6 : tex.layers) : 0);
        const t = m.types.get(rt);
        return norm(t?.kind === "vector" ? out.slice(0, t.count) : out[0]);
      }
      case Op.ImageQueryLevels: {
        const image = v(2);
        const img = image instanceof SampledImageValue ? image.image : image;
        return norm(img instanceof ImageValue && img.texture ? img.texture.baseMip + img.texture.mips : 0);
      }
      case Op.ImageQuerySamples:
        return norm(1);
      case Op.ImageQueryLod: {
        const si = v(2);
        const coord = v(3);
        const d = this._derivative(inst, coord);
        if (d === "blocked") return "blocked" as unknown as Value;
        const tex = si instanceof SampledImageValue ? si.image.texture : null;
        const lod = tex ? implicitLod(tex, flat(d.dx), flat(d.dy)) : 0;
        return norm([Math.max(0, lod), lod]);
      }
      case Op.ImageTexelPointer:
        this.warnings.add("pointers to storage image texels (imageAtomic operations) are not followed");
        return null;

      case Op.ExtInst: {
        const set = m.extSets.get(w[2]);
        if (set !== GLSL_STD_450) throw new Error(`the extended instruction set ${set ?? `%${w[2]}`} is not interpreted`);
        return this._glsl(frame, inst, w[3], Array.from(w.subarray(4)), s);
      }
      default:
        throw new Error(`the interpreter does not handle opcode ${inst.op} yet`);
    }
  }

  // ---------------------------------------------------------------------------------------
  // Memory

  private _bufferOf(ptr: Pointer): { buffer: BufferStorage; path: number[] } | null {
    if (ptr.cell.buffer) return { buffer: ptr.cell.buffer, path: ptr.path };
    if (ptr.cell.bufferArray) {
      const element = (ptr.cell.value as unknown as { buffer: BufferStorage }[])[ptr.path[0] ?? 0];
      return element ? { buffer: element.buffer, path: ptr.path.slice(1) } : null;
    }
    return null;
  }

  /** The value a pointer points at (`limit` caps runtime arrays read for display). */
  private _load(ptr: Pointer, limit = Infinity): Value {
    const storage = this._bufferOf(ptr);
    if (storage) return this._loadBuffer(storage.buffer, storage.path, limit);
    if (ptr.cell.bufferArray) {
      return (ptr.cell.value as unknown as { buffer: BufferStorage }[]).map((e) => this._loadBuffer(e.buffer, [], limit));
    }
    let value = ptr.cell.value;
    for (const index of ptr.path) value = Array.isArray(value) ? value[index] : 0;
    return value instanceof ImageValue || value instanceof SamplerValue || value instanceof SampledImageValue ? value : cloneValue(value ?? 0);
  }

  private _store(ptr: Pointer, value: Value): void {
    const storage = this._bufferOf(ptr);
    if (storage) {
      const key = storage.path.join("/");
      for (const k of [...storage.buffer.overrides.keys()]) if (k.startsWith(key ? `${key}/` : "")) storage.buffer.overrides.delete(k);
      storage.buffer.overrides.set(key, value);
      return;
    }
    if (!ptr.path.length) {
      ptr.cell.value = value;
      return;
    }
    let parent = ptr.cell.value as Value[];
    for (let i = 0; i < ptr.path.length - 1; i++) parent = parent[ptr.path[i]] as Value[];
    parent[ptr.path[ptr.path.length - 1]] = value;
  }

  private _loadBuffer(buffer: BufferStorage, path: number[], limit = Infinity): Value {
    const m = this.module;
    const key = path.join("/");
    // A store at this path or above it.
    for (let n = path.length; n >= 0; n--) {
      const k = path.slice(0, n).join("/");
      const stored = buffer.overrides.get(k);
      if (stored === undefined) continue;
      let value = stored;
      for (const index of path.slice(n)) value = Array.isArray(value) ? value[index] : 0;
      return cloneValue(value);
    }
    const view = new DataView(buffer.bytes.buffer, buffer.bytes.byteOffset, buffer.bytes.byteLength);
    let value: Value;
    const loc = bufferLocation(m, buffer.type, path, buffer.bytes.byteLength);
    if (loc) {
      value = readBuffer(m, view, loc.at, loc.type, loc.matrix, limit);
      if (loc.at >= buffer.bytes.byteLength && buffer.bytes.byteLength) this.warnings.add("a read past the captured end of a buffer reads zeros (the capture's Max KB truncated it?)");
    } else {
      // A path through a row-major matrix: read the whole block and walk it.
      value = readBuffer(m, view, 0, buffer.type, undefined, limit);
      for (const index of path) value = Array.isArray(value) ? value[index] : 0;
    }
    // Stores below this path.
    for (const [k, stored] of buffer.overrides) {
      if (!k.startsWith(key ? `${key}/` : "") || k === key) continue;
      const rest = (key ? k.slice(key.length + 1) : k).split("/").map(Number);
      let at = value as Value[];
      for (let i = 0; i < rest.length - 1 && Array.isArray(at); i++) at = at[rest[i]] as Value[];
      if (Array.isArray(at)) at[rest[rest.length - 1]] = cloneValue(stored);
    }
    return value;
  }

  // ---------------------------------------------------------------------------------------
  // Helpers

  private _derivative(inst: Instruction, operand: Value): { dx: Value; dy: Value } | "blocked" {
    if (!this.derivatives) {
      this.warnings.add("derivatives are zero outside a fragment shader's pixel quad");
      const zero = mapScalars(operand, () => 0);
      return { dx: zero, dy: zero };
    }
    return this.derivatives.derivative(this, inst, operand);
  }

  private _imageType(frame: Frame, id: number): { dim: number; arrayed: boolean; ms: boolean } | null {
    let type = this._typeOfId(frame, id);
    if (!type) {
      const g = this.globals.get(id);
      if (g) type = g.type;
    }
    let t = this.module.types.get(type);
    if (t?.kind === "sampledImage") t = this.module.types.get(t.image);
    if (t?.kind === "image") return { dim: t.dim, arrayed: t.arrayed, ms: t.ms };
    // A value loaded from a pointer: its type is the load's result type, already looked up.
    return null;
  }

  private _texelResult(texel: number[], rt: number, s: ScalarKind | null, texture: DebugTexture | null): Value {
    const t = this.module.types.get(rt);
    const values = t?.kind === "vector" ? texel.slice(0, t.count) : texel[0];
    void texture;
    return s ? mapScalars(values as Value, (x) => normalize(x, s)) : (values as Value);
  }

  private _sample(frame: Frame, inst: Instruction, s: ScalarKind | null): Value {
    const w = inst.words;
    const op = inst.op;
    const si = this.value(frame, w[2]);
    if (!(si instanceof SampledImageValue)) throw new Error("sampling something that is not a sampled image");
    const dref = op === Op.ImageSampleDrefImplicitLod || op === Op.ImageSampleDrefExplicitLod || op === Op.ImageSampleProjDrefImplicitLod || op === Op.ImageSampleProjDrefExplicitLod;
    const proj = op >= Op.ImageSampleProjImplicitLod && op <= Op.ImageSampleProjDrefExplicitLod;
    const implicit = op === Op.ImageSampleImplicitLod || op === Op.ImageSampleDrefImplicitLod || op === Op.ImageSampleProjImplicitLod || op === Op.ImageSampleProjDrefImplicitLod;
    let coord = flat(this.value(frame, w[3]));
    let next = 4;
    let reference = dref ? num(this.value(frame, w[next++])) : undefined;
    if (proj) {
      const q = coord[coord.length - 1] || 1;
      coord = coord.slice(0, -1).map((c) => c / q);
      if (reference !== undefined) reference /= q;
    }
    const imageTypeInst = this.module.types.get(this._typeOfId(frame, w[2]));
    const imageType = imageTypeInst?.kind === "sampledImage" ? this.module.types.get(imageTypeInst.image) : null;
    const dim = imageType?.kind === "image" ? imageType.dim : Dim.D2;
    const arrayed = imageType?.kind === "image" ? imageType.arrayed : false;

    let bias = 0;
    let lod: number | null = null;
    let grad: { dx: number[]; dy: number[] } | null = null;
    let offset: number[] | undefined;
    if (next < w.length) {
      const mask = w[next++];
      if (mask & ImageOperand.Bias) bias = num(this.value(frame, w[next++]));
      if (mask & ImageOperand.Lod) lod = num(this.value(frame, w[next++]));
      if (mask & ImageOperand.Grad) {
        grad = { dx: flat(this.value(frame, w[next])), dy: flat(this.value(frame, w[next + 1])) };
        next += 2;
      }
      if (mask & ImageOperand.ConstOffset) offset = flat(this.value(frame, w[next++]));
      if (mask & ImageOperand.Offset) offset = flat(this.value(frame, w[next++]));
      if (mask & ImageOperand.ConstOffsets) next++;
      if (mask & ImageOperand.MinLod) next++;
    }
    const texture = si.image.texture;
    if (implicit) {
      const d = this._derivative(inst, coord.slice(0, dim === Dim.Cube ? 3 : dim === Dim.D1 ? 1 : 2));
      if (d === "blocked") return "blocked" as unknown as Value;
      lod = (texture ? implicitLod(texture, flat(d.dx), flat(d.dy)) : 0) + bias;
    } else if (grad && texture) {
      lod = implicitLod(texture, grad.dx, grad.dy);
    }
    const rgba = sample(texture, si.sampler.sampler, { dim, arrayed, coord, lod: lod ?? 0, dref: reference, offset });
    if (dref) return s ? normalize(rgba[0], s) : rgba[0];
    const t = this.module.types.get(inst.resultType);
    const out = t?.kind === "vector" ? rgba.slice(0, t.count) : rgba[0];
    return s ? mapScalars(out as Value, (x) => normalize(x, s)) : (out as Value);
  }

  private _bitcast(value: Value, fromType: number, toType: number): Value {
    const from = scalarOf(this.module, fromType);
    const to = scalarOf(this.module, toType);
    if (!to) return value;
    const buf = new DataView(new ArrayBuffer(8));
    return mapScalars(value, (x) => {
      if (from?.base === "float" && from.width === 32) buf.setFloat32(0, num(x), true);
      else if (from?.base === "float" && from.width === 64) buf.setFloat64(0, num(x), true);
      else if (from?.width === 64) buf.setBigUint64(0, BigInt.asUintN(64, big(x)), true);
      else buf.setUint32(0, num(x) >>> 0, true);
      if (to.base === "float") return to.width === 64 ? buf.getFloat64(0, true) : buf.getFloat32(0, true);
      if (to.width === 64) return to.base === "int" ? buf.getBigInt64(0, true) : buf.getBigUint64(0, true);
      return to.base === "int" ? buf.getInt32(0, true) : buf.getUint32(0, true);
    });
  }

  private _extendedArithmetic(op: number, a: Value, b: Value, rt: number): Value {
    const t = this.module.types.get(rt);
    const member = t?.kind === "struct" ? t.members[0] : 0;
    const s = scalarOf(this.module, member) ?? { base: "uint", width: 32 };
    const lo: Value = zipScalars(a, b, (x, y) => {
      const X = BigInt.asUintN(32, big(x)), Y = BigInt.asUintN(32, big(y));
      const r = op === Op.IAddCarry ? X + Y : op === Op.ISubBorrow ? X - Y : op === Op.UMulExtended ? X * Y : BigInt.asIntN(32, X) * BigInt.asIntN(32, Y);
      return normalize(Number(BigInt.asUintN(32, r)), s);
    });
    const hi: Value = zipScalars(a, b, (x, y) => {
      const X = BigInt.asUintN(32, big(x)), Y = BigInt.asUintN(32, big(y));
      if (op === Op.IAddCarry) return X + Y > 0xffffffffn ? 1 : 0;
      if (op === Op.ISubBorrow) return Y > X ? 1 : 0;
      const r = op === Op.UMulExtended ? X * Y : BigInt.asIntN(32, X) * BigInt.asIntN(32, Y);
      return normalize(Number(BigInt.asUintN(32, r >> 32n)), s);
    });
    return [lo, hi];
  }

  // ---------------------------------------------------------------------------------------
  // GLSL.std.450

  private _glsl(frame: Frame, inst: Instruction, number: number, args: number[], s: ScalarKind | null): Value {
    const a = (i: number): Value => this.value(frame, args[i]);
    const norm = (x: Value): Value => (s ? mapScalars(x, (e) => normalize(e, s)) : x);
    const unary = (f: (x: number) => number): Value => norm(mapScalars(a(0), (x) => f(num(x))));
    const binary = (f: (x: number, y: number) => number): Value => norm(zipScalars(a(0), a(1), (x, y) => f(num(x), num(y))));
    const ternary = (f: (x: number, y: number, z: number) => number): Value => {
      const x0 = a(0), y0 = a(1), z0 = a(2);
      const at = (value: Value, i: number): number => (Array.isArray(value) ? num(value[i]) : num(value));
      if (Array.isArray(x0)) return norm(x0.map((_, i) => f(at(x0, i), at(y0, i), at(z0, i))));
      return norm(f(num(x0), num(y0), num(z0)));
    };
    const vec = (i: number): number[] => flat(a(i));
    const width = s?.width ?? 32;
    switch (number) {
      case 1: return unary((x) => (x < 0 ? -Math.round(-x) : Math.round(x)));
      case 2: return unary((x) => {
        const r = Math.round(x);
        return Math.abs(x % 1) === 0.5 ? 2 * Math.round(x / 2) : r;
      });
      case 3: return unary(Math.trunc);
      case 4: return unary(Math.abs);
      case 5: return norm(mapScalars(a(0), (x) => Math.abs(Number(signed(x, width)))));
      case 6: return unary((x) => (x > 0 ? 1 : x < 0 ? -1 : 0));
      case 7: return norm(mapScalars(a(0), (x) => Math.sign(Number(signed(x, width)))));
      case 8: return unary(Math.floor);
      case 9: return unary(Math.ceil);
      case 10: return unary((x) => x - Math.floor(x));
      case 11: return unary((x) => (x * Math.PI) / 180);
      case 12: return unary((x) => (x * 180) / Math.PI);
      case 13: return unary(Math.sin);
      case 14: return unary(Math.cos);
      case 15: return unary(Math.tan);
      case 16: return unary(Math.asin);
      case 17: return unary(Math.acos);
      case 18: return unary(Math.atan);
      case 19: return unary(Math.sinh);
      case 20: return unary(Math.cosh);
      case 21: return unary(Math.tanh);
      case 22: return unary(Math.asinh);
      case 23: return unary(Math.acosh);
      case 24: return unary(Math.atanh);
      case 25: return binary(Math.atan2);
      case 26: return binary(Math.pow);
      case 27: return unary(Math.exp);
      case 28: return unary(Math.log);
      case 29: return unary((x) => 2 ** x);
      case 30: return unary(Math.log2);
      case 31: return unary(Math.sqrt);
      case 32: return unary((x) => 1 / Math.sqrt(x));
      case 33: return norm(determinant(a(0) as number[][]));
      case 34: return norm(inverse(a(0) as number[][]));
      case 35: {
        // modf(x, out i): the fraction, the whole part stored through the pointer.
        const x = a(0);
        const whole = mapScalars(x, (e) => Math.trunc(num(e)));
        const ptr = a(1);
        if (ptr instanceof Pointer) this._store(ptr, norm(whole));
        return norm(zipScalars(x, whole, (e, w) => num(e) - num(w)));
      }
      case 36: {
        const x = a(0);
        const whole = mapScalars(x, (e) => Math.trunc(num(e)));
        const member = this.module.types.get(inst.resultType);
        const fs = member?.kind === "struct" ? scalarOf(this.module, member.members[0]) : s;
        const n = (value: Value): Value => (fs ? mapScalars(value, (e) => normalize(e, fs)) : value);
        return [n(zipScalars(x, whole, (e, w) => num(e) - num(w))), n(whole)];
      }
      case 37: return binary((x, y) => (y < x ? y : x));
      case 38: return norm(zipScalars(a(0), a(1), (x, y) => (unsigned(y, width) < unsigned(x, width) ? y : x)));
      case 39: return norm(zipScalars(a(0), a(1), (x, y) => (signed(y, width) < signed(x, width) ? y : x)));
      case 40: return binary((x, y) => (x < y ? y : x));
      case 41: return norm(zipScalars(a(0), a(1), (x, y) => (unsigned(x, width) < unsigned(y, width) ? y : x)));
      case 42: return norm(zipScalars(a(0), a(1), (x, y) => (signed(x, width) < signed(y, width) ? y : x)));
      case 43: return ternary((x, lo, hi) => Math.min(Math.max(x, lo), hi));
      case 44: return ternary((x, lo, hi) => Math.min(Math.max(x >>> 0, lo >>> 0), hi >>> 0));
      case 45: return ternary((x, lo, hi) => Math.min(Math.max(x | 0, lo | 0), hi | 0));
      case 46: return ternary((x, y, t) => x * (1 - t) + y * t);
      case 47: return ternary((x, y, t) => (t ? y : x));
      case 48: return binary((edge, x) => (x < edge ? 0 : 1));
      case 49: return ternary((e0, e1, x) => {
        const t = Math.min(Math.max((x - e0) / (e1 - e0), 0), 1);
        return t * t * (3 - 2 * t);
      });
      case 50: return ternary((x, y, z) => x * y + z);
      case 51: {
        // frexp(x, out e)
        const x = a(0);
        const exps = mapScalars(x, (e) => frexp(num(e))[1]);
        const ptr = a(1);
        if (ptr instanceof Pointer) this._store(ptr, exps);
        return norm(mapScalars(x, (e) => frexp(num(e))[0]));
      }
      case 52: {
        const x = a(0);
        return [norm(mapScalars(x, (e) => frexp(num(e))[0])), mapScalars(x, (e) => frexp(num(e))[1])];
      }
      case 53: return norm(zipScalars(a(0), a(1), (x, e) => num(x) * 2 ** num(e)));
      case 54: return packNorm(vec(0), 8, true);
      case 55: return packNorm(vec(0), 8, false);
      case 56: return packNorm(vec(0), 16, true);
      case 57: return packNorm(vec(0), 16, false);
      case 58: return packHalf(vec(0));
      case 60: return unpackNorm(num(a(0)), 16, true, 2);
      case 61: return unpackNorm(num(a(0)), 16, false, 2);
      case 62: return unpackHalf(num(a(0)));
      case 63: return unpackNorm(num(a(0)), 8, true, 4);
      case 64: return unpackNorm(num(a(0)), 8, false, 4);
      case 66: return norm(Math.hypot(...vec(0)));
      case 67: {
        const p = vec(0), q = vec(1);
        return norm(Math.hypot(...p.map((x, i) => x - q[i])));
      }
      case 68: {
        const [x1, y1, z1] = vec(0), [x2, y2, z2] = vec(1);
        return norm([y1 * z2 - z1 * y2, z1 * x2 - x1 * z2, x1 * y2 - y1 * x2]);
      }
      case 69: {
        const x = a(0);
        if (!Array.isArray(x)) return norm(Math.sign(num(x)));
        const len = Math.hypot(...flat(x));
        return norm(x.map((e) => num(e) / len));
      }
      case 70: {
        const n = vec(0), i = vec(1), nref = vec(2);
        const d = nref.reduce((sum, x, k) => sum + x * i[k], 0);
        return norm(d < 0 ? n : n.map((x) => -x));
      }
      case 71: {
        const i = vec(0), n = vec(1);
        const d = n.reduce((sum, x, k) => sum + x * i[k], 0);
        return norm(i.map((x, k) => x - 2 * d * n[k]));
      }
      case 72: {
        const i = vec(0), n = vec(1), eta = num(a(2));
        const d = n.reduce((sum, x, k) => sum + x * i[k], 0);
        const k = 1 - eta * eta * (1 - d * d);
        return norm(k < 0 ? i.map(() => 0) : i.map((x, j) => eta * x - (eta * d + Math.sqrt(k)) * n[j]));
      }
      case 73: return norm(mapScalars(a(0), (x) => {
        const n = num(x) >>> 0;
        return n === 0 ? -1 : 31 - Math.clz32(n & -n);
      }));
      case 74: return norm(mapScalars(a(0), (x) => {
        const n = num(x) | 0;
        const m = n < 0 ? ~n : n;
        return m === 0 ? -1 : 31 - Math.clz32(m);
      }));
      case 75: return norm(mapScalars(a(0), (x) => {
        const n = num(x) >>> 0;
        return n === 0 ? -1 : 31 - Math.clz32(n);
      }));
      case 76: case 77: case 78: {
        // interpolateAt*: the input as the fragment has it (the interpolation point is the pixel's centre).
        const ptr = a(0);
        this.warnings.add("interpolateAtCentroid / AtSample / AtOffset read the input at the pixel centre");
        return ptr instanceof Pointer ? this._load(ptr) : ptr;
      }
      case 79: return binary((x, y) => (Number.isNaN(x) ? y : Number.isNaN(y) ? x : Math.min(x, y)));
      case 80: return binary((x, y) => (Number.isNaN(x) ? y : Number.isNaN(y) ? x : Math.max(x, y)));
      case 81: return ternary((x, lo, hi) => (Number.isNaN(x) ? lo : Math.min(Math.max(x, lo), hi)));
      default:
        throw new Error(`GLSL.std.450 instruction ${number} is not interpreted`);
    }
  }
}

function floatCompare(op: number, a: number, b: number): boolean {
  const unordered = Number.isNaN(a) || Number.isNaN(b);
  switch (op) {
    case Op.FOrdEqual: return !unordered && a === b;
    case Op.FUnordEqual: return unordered || a === b;
    case Op.FOrdNotEqual: return !unordered && a !== b;
    case Op.FUnordNotEqual: return unordered || a !== b;
    case Op.FOrdLessThan: return !unordered && a < b;
    case Op.FUnordLessThan: return unordered || a < b;
    case Op.FOrdGreaterThan: return !unordered && a > b;
    case Op.FUnordGreaterThan: return unordered || a > b;
    case Op.FOrdLessThanEqual: return !unordered && a <= b;
    case Op.FUnordLessThanEqual: return unordered || a <= b;
    case Op.FOrdGreaterThanEqual: return !unordered && a >= b;
    case Op.FUnordGreaterThanEqual: return unordered || a >= b;
    case Op.LessOrGreater: return !unordered && a !== b;
    case Op.Ordered: return !unordered;
    case Op.Unordered: return unordered;
    default: return false;
  }
}

function frexp(x: number): [number, number] {
  if (x === 0 || !Number.isFinite(x)) return [x, 0];
  const e = Math.floor(Math.log2(Math.abs(x))) + 1;
  let m = x / 2 ** e;
  // Rounding in log2 can leave the mantissa just outside [0.5, 1).
  if (Math.abs(m) >= 1) return [m / 2, e + 1];
  if (Math.abs(m) < 0.5) m *= 2;
  return Math.abs(x / 2 ** e) < 0.5 ? [m, e - 1] : [m, e];
}

function determinant(m: number[][]): number {
  const n = m.length;
  if (n === 2) return m[0][0] * m[1][1] - m[1][0] * m[0][1];
  if (n === 3) {
    return m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2]) - m[1][0] * (m[0][1] * m[2][2] - m[2][1] * m[0][2]) + m[2][0] * (m[0][1] * m[1][2] - m[1][1] * m[0][2]);
  }
  let det = 0;
  for (let c = 0; c < n; c++) {
    const minor = m.filter((_, i) => i !== c).map((col) => col.slice(1));
    det += (c % 2 ? -1 : 1) * m[c][0] * determinant(minor);
  }
  return det;
}

function inverse(m: number[][]): number[][] {
  const n = m.length;
  // Gauss-Jordan on rows (matrices are column arrays: element [c][r]).
  const a = Array.from({ length: n }, (_, r) => [...Array.from({ length: n }, (_, c) => m[c][r]), ...Array.from({ length: n }, (_, c) => (c === r ? 1 : 0))]);
  for (let col = 0; col < n; col++) {
    let pivot = col;
    for (let r = col + 1; r < n; r++) if (Math.abs(a[r][col]) > Math.abs(a[pivot][col])) pivot = r;
    [a[col], a[pivot]] = [a[pivot], a[col]];
    const p = a[col][col];
    if (p === 0) return m.map((c) => c.map(() => NaN));
    for (let c = 0; c < 2 * n; c++) a[col][c] /= p;
    for (let r = 0; r < n; r++) {
      if (r === col) continue;
      const f = a[r][col];
      for (let c = 0; c < 2 * n; c++) a[r][c] -= f * a[col][c];
    }
  }
  return Array.from({ length: n }, (_, c) => Array.from({ length: n }, (_, r) => a[r][n + c]));
}

function packNorm(v: number[], bits: number, isSigned: boolean): number {
  const max = 2 ** (isSigned ? bits - 1 : bits) - 1;
  let out = 0;
  v.forEach((x, i) => {
    const clamped = isSigned ? Math.min(Math.max(x, -1), 1) : Math.min(Math.max(x, 0), 1);
    const q = Math.round(clamped * max) & (2 ** bits - 1);
    out += q * 2 ** (bits * i);
  });
  return out >>> 0;
}

function unpackNorm(p: number, bits: number, isSigned: boolean, count: number): number[] {
  const max = 2 ** (isSigned ? bits - 1 : bits) - 1;
  return Array.from({ length: count }, (_, i) => {
    let q = Math.floor((p >>> 0) / 2 ** (bits * i)) % 2 ** bits;
    if (isSigned && q >= 2 ** (bits - 1)) q -= 2 ** bits;
    return Math.fround(isSigned ? Math.max(q / max, -1) : q / max);
  });
}

function toHalf(x: number): number {
  const f = new Float32Array([x]);
  const bits = new Uint32Array(f.buffer)[0];
  const sign = (bits >>> 16) & 0x8000;
  const exponent = ((bits >>> 23) & 0xff) - 127 + 15;
  const mantissa = bits & 0x7fffff;
  if (exponent <= 0) return sign;
  if (exponent >= 31) return sign | 0x7c00 | (((bits >>> 23) & 0xff) === 0xff && mantissa ? 0x200 : 0);
  return sign | (exponent << 10) | (mantissa >>> 13);
}

function packHalf(v: number[]): number {
  return ((toHalf(v[1] ?? 0) << 16) | toHalf(v[0] ?? 0)) >>> 0;
}

function unpackHalf(p: number): number[] {
  const half = (h: number): number => {
    const sign = h & 0x8000 ? -1 : 1;
    const e = (h >> 10) & 0x1f;
    const f = h & 0x3ff;
    return e === 0 ? sign * 2 ** -14 * (f / 1024) : e === 31 ? (f ? NaN : sign * Infinity) : sign * 2 ** (e - 15) * (1 + f / 1024);
  };
  return [Math.fround(half(p & 0xffff)), Math.fround(half((p >>> 16) & 0xffff))];
}
