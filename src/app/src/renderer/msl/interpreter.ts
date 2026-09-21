// The MSL interpreter: one invocation of an entry point (a vertex, a fragment, a compute
// invocation) executed an instruction at a time, so the debugger can stop anywhere, show every
// value, and step by source line.
//
// It runs the linear form the lowering produced (ir.ts), with a program counter and explicit call
// frames rather than JavaScript recursion, which is what lets a step stop between any two
// instructions. That is the same shape as the SPIR-V interpreter (../spirv/interpreter.ts), and
// both implement DebugInvocation, so the stepping, the tab and the MCP tool drive either.
//
// Inputs come from the capture: the vertex's attributes or the fragment's interpolated varyings
// (MslInputs), and the buffers, textures and samplers the draw had bound (MslBindings).
// Derivatives (dfdx, implicit-LOD sampling) need the neighboring invocations of the pixel quad;
// a DerivativeSource (../debug/quad.ts) runs them in lockstep, and without one they are zero.
import type { DerivativeSource } from "../debug/quad.js";
import type {
  DebugFrameView, DebugInvocation, DebugStep, InvocationStatus, StepResult, VariableView,
} from "../debug/program.js";
import {
  ImageValue, OpaqueValue, Pointer, SamplerValue, cloneValue, mapScalars, normalize, zipScalars,
  type BufferStorage, type Cell, type DebugSampler, type DebugTexture, type Value,
} from "../debug/values.js";
import { BLOCKED, callBuiltin, type BuiltinContext } from "./stdlib.js";
import {
  boxHit, isAccelerationStructure, isFunctionTable, noHit, traceRay,
  INTERSECTION_BOUNDING_BOX, INTERSECTION_NONE, INTERSECTOR_KIND, SCENE_KIND, TABLE_KIND,
  type BoxCandidate, type DebugRay, type IntersectorHandle, type RayFunctionTable, type RayHit,
  type RayScene, type SceneHandle, type TableHandle,
} from "./raytracing.js";
import type { FunctionIr, Instr, Symbol } from "./ir.js";
import type { MslProgram } from "./program.js";
import type { TypeTable } from "./types.js";

export interface MslBindings {
  /** The bytes bound at `[[buffer(index)]]` for the stage, or null when the capture has none. */
  buffer(index: number): Uint8Array | null;
  texture(index: number): DebugTexture | null;
  sampler(index: number): DebugSampler | null;
  /**
   * The scene bound at `[[buffer(index)]]`, for a parameter whose type is an acceleration
   * structure. Metal binds one at a buffer index like anything else, so the index is the same
   * namespace as `buffer` above and only the parameter's *type* says which of the two it is.
   */
  accelerationStructure?(index: number): RayScene | null;
  /** The intersection function table bound at `[[buffer(index)]]`. */
  functionTable?(index: number): RayFunctionTable | null;
  /** What to call a binding in the values table: "buffer(1)", "texture(0)". */
  label?(kind: "buffer" | "texture" | "sampler", index: number): string;
}

/**
 * What an application specialized a shader with: the values it set on the
 * `MTLFunctionConstantValues` it built the function from, by `[[function_constant(n)]]` index and
 * by name. A Unity shader is one library of constant-guarded variants, so running it without
 * these steps the wrong branches.
 */
export interface MslFunctionConstants {
  byIndex: Map<number, Value>;
  byName: Map<string, Value>;
}

export interface MslInputs {
  /** Built-ins by MSL attribute name: "vertex_id", "position", "thread_position_in_grid". */
  builtins: Map<string, Value>;
  /** `[[stage_in]]` members by `[[attribute(n)]]` index: a vertex's attributes. */
  attributes: Map<number, number[]>;
  /** `[[stage_in]]` members by name: a fragment's varyings, which MSL matches by member. */
  varyings: Map<string, number[]>;
}

interface Frame {
  fn: FunctionIr;
  /** Ordinal of the next instruction to execute. */
  pc: number;
  values: Map<number, Value>;
  /** Cells of the function's parameters and locals, by symbol id, in declaration order. */
  cells: Map<number, Cell>;
  locals: { id: number; cell: Cell; type: number }[];
  /** The caller's destination register, -1 for the entry point. */
  resultId: number;
  /**
   * A ray query part way through (the `rayQuery` instruction). One per frame is enough: a traversal
   * runs to completion before the instruction that started it finishes, so a second cannot begin
   * in the same frame — and a nested one, inside the intersection function, is a frame of its own.
   */
  query?: RayQueryState;
}

/**
 * A ray query in progress: the traversal is done, and what is left is asking the shader's own
 * intersection function about each bounding box the ray entered.
 *
 * The interpreter is a stepping machine — `call` pushes a frame and returns — so a builtin cannot
 * call a shader function and wait for it. What it can do is *not advance the program counter*: the
 * callee's `return` writes into the caller's destination register and leaves the caller on the same
 * instruction, so `rayQuery` runs again, reads what the function said out of that register, and
 * asks about the next box. When the candidates run out it writes the result and moves on.
 */
interface RayQueryState {
  /** The instruction that started it, so a different query in the same frame starts fresh. */
  at: number;
  ray: DebugRay;
  scene: RayScene;
  table: RayFunctionTable | null;
  candidates: BoxCandidate[];
  /** The next candidate to ask about. */
  next: number;
  /** The nearest hit so far: a triangle from the traversal, or a box a function accepted. */
  hit: RayHit;
  limit: number;
  acceptAny: boolean;
  /** The candidate the shader's function is answering about right now. */
  pending: BoxCandidate | null;
}

const MAX_STEPS = 20_000_000;

export class MslInvocation implements DebugInvocation {
  readonly program: MslProgram;
  readonly entry: FunctionIr;
  readonly bindings: MslBindings;
  readonly inputs: MslInputs;
  derivatives: DerivativeSource | null;
  status: InvocationStatus = "running";
  error = "";
  readonly warnings = new Set<string>();
  readonly frames: Frame[] = [];
  /** Globals: their cells, by symbol id. */
  readonly globals = new Map<number, Cell>();
  /** Entry-point parameters as the debugger shows them, with what bound each. */
  readonly boundParams: VariableView[] = [];
  /** Function constants the application set, by index: what `is_function_constant_defined` answers from. */
  readonly definedConstants = new Set<number>();
  /** What the entry point returned, once it has. */
  returned: Value = null;
  steps = 0;
  private _results: StepResult[] = [];
  onResult: ((r: StepResult) => void) | null = null;
  /** A builtin that blocked: the same instruction runs again next step. */
  private _blocked = false;

  constructor(program: MslProgram, options: {
    entryPoint?: string; stage?: string; bindings: MslBindings; inputs: MslInputs;
    derivatives?: DerivativeSource | null; constants?: MslFunctionConstants;
  }) {
    this.program = program;
    const entry = program.entryPoint(options.entryPoint, options.stage);
    if (!entry) throw new Error(`the library has no ${options.entryPoint ?? options.stage ?? ""} entry point`);
    this.entry = entry;
    this.bindings = options.bindings;
    this.inputs = options.inputs;
    this.derivatives = options.derivatives ?? null;
    for (const g of program.ir.globals) this.globals.set(g.symbol.id, { value: cloneValue(g.value) });
    this._specialize(options.constants);
    this.frames.push(this._frame(entry, -1));
    this._bindEntry(this.frames[0]);
  }

  /**
   * Gives the `[[function_constant(n)]]` globals the values the function was built with. A
   * constant the capture has no value for keeps its zero and is reported as undefined, which is
   * what `is_function_constant_defined` is in the shader to ask.
   */
  private _specialize(constants?: MslFunctionConstants): void {
    // A library declares its constants at file scope, so this entry point sees every one of them;
    // only the ones it reads had to be specialized for it, and only those are worth warning about.
    const used = this.program.functionConstantsUsedBy(this.entry);
    const missing: string[] = [];
    for (const { symbol, index } of this.program.ir.functionConstants) {
      const value = constants?.byIndex.get(index) ?? constants?.byName.get(symbol.name);
      if (value === undefined) {
        if (used.has(index)) missing.push(`${symbol.name} [[function_constant(${index})]]`);
        continue;
      }
      this.definedConstants.add(index);
      this.globals.set(symbol.id, { value: this._fit(value, symbol.type) });
    }
    if (missing.length) {
      this.warn(`the capture does not record what this function was specialized with, so ${missing.join(", ")} ${missing.length === 1 ? "reads" : "read"} as zero`);
    }
  }

  private get _types(): TypeTable {
    return this.program.ir.types;
  }

  private get _instructions(): Instr[] {
    return this.program.ir.instructions;
  }

  private _symbol(id: number): Symbol | undefined {
    return this.program.ir.symbols[id];
  }

  // ---------------------------------------------------------------------------------------
  // DebugInvocation

  get invocation(): MslInvocation {
    return this;
  }

  get finished(): boolean {
    return this.status === "returned" || this.status === "discarded" || this.status === "error";
  }

  get depth(): number {
    return this.frames.length;
  }

  /** The instruction about to execute, null when finished. */
  get current(): DebugStep | null {
    const frame = this.frames[this.frames.length - 1];
    return frame && !this.finished ? this._instructions[frame.pc] ?? null : null;
  }

  takeResults(): StepResult[] {
    const r = this._results;
    this._results = [];
    return r;
  }

  callStack(): DebugFrameView[] {
    const out: DebugFrameView[] = [];
    for (let d = 0; d < this.frames.length; d++) {
      const frame = this.frames[this.frames.length - 1 - d];
      out.push({ name: frame.fn.name, step: d === 0 ? this.current : this._instructions[frame.pc] ?? null });
    }
    return out;
  }

  valueOf(id: number, depth = 0): Value | undefined {
    const frame = this.frames[this.frames.length - 1 - depth];
    const cell = frame?.cells.get(id) ?? this.globals.get(id);
    if (cell) return cloneValue(cell.value);
    return frame?.values.get(id);
  }

  frameOwns(depth: number, id: number): boolean {
    const frame = this.frames[this.frames.length - 1 - depth];
    return !!frame && (frame.cells.has(id) || frame.values.has(id));
  }

  locals(depth = 0): VariableView[] {
    const frame = this.frames[this.frames.length - 1 - depth];
    if (!frame) return [];
    const out: VariableView[] = [];
    for (const p of frame.fn.params) {
      const cell = frame.cells.get(p.id);
      const value = cell ? cloneValue(cell.value) : frame.values.get(p.id) ?? null;
      out.push({ id: p.id, name: p.name, type: p.type, value, storage: 0 });
    }
    for (const l of frame.locals) {
      out.push({ id: l.id, name: this._symbol(l.id)?.name ?? "", type: l.type, value: cloneValue(l.cell.value), storage: 0 });
    }
    return out;
  }

  /** The entry point's arguments: the stage-in struct, the built-ins and the bound resources. */
  inputVariables(): VariableView[] {
    return this.boundParams.filter((v) => v.binding === undefined);
  }

  /** The bound buffers, textures and samplers. */
  resourceVariables(): VariableView[] {
    return this.boundParams.filter((v) => v.binding !== undefined);
  }

  /** File-scope `constant` variables, which is the nearest MSL has to a private global. */
  privateVariables(): VariableView[] {
    const constants = new Map(this.program.ir.functionConstants.map((c) => [c.symbol.id, c.index]));
    const out: VariableView[] = [];
    for (const [id, cell] of this.globals) {
      const symbol = this._symbol(id);
      if (!symbol) continue;
      const index = constants.get(id);
      out.push({
        id, name: symbol.name, type: symbol.type, value: cloneValue(cell.value), storage: 1,
        // A function constant shows where it came from, the way a bound resource does.
        ...(index === undefined ? {} : { binding: index, set: 3 }),
      });
    }
    return out;
  }

  /**
   * What the entry point wrote: the members of the struct it returned, with `[[position]]` reported
   * as built-in 0 (which is what the debugger's comparison with the GPU looks for) and
   * `[[color(n)]]` or a plain varying as location n.
   */
  outputs(): VariableView[] {
    if (this.status !== "returned") return [];
    const type = this.entry.returnType;
    const t = this._types.get(type);
    if (t?.kind === "struct") {
      return t.members.map((m, i) => {
        const attribute = m.attributes.find((a) => OUTPUT_ATTRIBUTES.has(a.name)) ?? m.attributes[0];
        const value = Array.isArray(this.returned) ? this.returned[i] ?? null : null;
        return {
          id: -1 - i, name: m.name, type: m.type, value, storage: 3,
          ...outputWhere(attribute?.name, attribute?.args[0], i),
        };
      });
    }
    if (t?.kind === "void") return [];
    const attribute = this.entry.returnAttributes[0];
    return [{
      id: -1, name: "return", type, value: this.returned, storage: 3,
      ...outputWhere(attribute?.name, attribute?.args[0], 0),
    }];
  }

  // ---------------------------------------------------------------------------------------
  // Setup

  private _frame(fn: FunctionIr, resultId: number): Frame {
    const frame: Frame = { fn, pc: fn.entry, values: new Map(), cells: new Map(), locals: [], resultId };
    for (const l of fn.locals) {
      const cell: Cell = { value: this._types.zero(l.type) };
      frame.cells.set(l.id, cell);
      frame.locals.push({ id: l.id, cell, type: l.type });
    }
    return frame;
  }

  /** Fills the entry point's parameters from the capture. */
  private _bindEntry(frame: Frame): void {
    for (const p of this.entry.params) {
      const symbol = this._symbol(p.id);
      const binding = symbol?.binding;
      const t = this._types.get(p.type);
      let value: Value = this._types.zero(p.type);
      let where: Partial<VariableView> = {};
      // An acceleration structure or a function table binds at a buffer index, so the type is what
      // tells it from a pointer to bytes (msl/raytracing.ts).
      if (binding?.kind === "buffer" && t?.kind === "opaque" && isAccelerationStructure(t.name)) {
        const scene = binding.index >= 0 ? this.bindings.accelerationStructure?.(binding.index) ?? null : null;
        if (!scene) {
          this.warn(`${this.label("buffer", binding.index)} (${p.name}) is an acceleration structure the capture `
                    + "does not hold the builds of, so every ray misses");
        } else if (scene.missing) {
          this.warn(`${scene.missing} of the scene's instances name a bottom level whose build is not in the `
                    + "capture, so a ray cannot be told what is in them");
        }
        value = new OpaqueValue(SCENE_KIND, [],
          { scene, binding: this.label("buffer", binding.index) } satisfies SceneHandle);
        frame.values.set(p.id, value);
        where = { binding: binding.index, set: 0 };
      } else if (binding?.kind === "buffer" && t?.kind === "opaque" && isFunctionTable(t.name)) {
        const table = binding.index >= 0 ? this.bindings.functionTable?.(binding.index) ?? null : null;
        if (!table) {
          this.warn(`${this.label("buffer", binding.index)} (${p.name}) is an intersection function table the `
                    + "capture did not record, so a bounding box cannot be asked about");
        }
        value = new OpaqueValue(TABLE_KIND, [],
          { table, binding: this.label("buffer", binding.index) } satisfies TableHandle);
        frame.values.set(p.id, value);
        where = { binding: binding.index, set: 0 };
      } else if (binding?.kind === "buffer") {
        const index = binding.index;
        const bytes = index >= 0 ? this.bindings.buffer(index) : null;
        const pointee = t?.kind === "pointer" ? t.pointee : p.type;
        if (!bytes) {
          this.warn(`${this.label("buffer", index)} (${p.name}) was not captured, so it reads as zero`);
        }
        // The bytes are copied: a shader that writes through a `device` pointer must not change
        // the capture's own data, and a later read has to see what it wrote.
        const storage: BufferStorage = {
          bytes: bytes ? new Uint8Array(bytes) : new Uint8Array(Math.max(4, this._types.sizeOf(pointee))),
          type: pointee,
          overrides: new Map(),
        };
        const cell: Cell = { value: null, buffer: storage };
        // A `device T*` indexes past its pointee: the cell holds an unsized array of it.
        if (t?.kind === "pointer" && !t.reference) storage.type = this._types.array(pointee, -1);
        const pointer = new Pointer(cell, [], storage.type, 0, p.id);
        frame.values.set(p.id, pointer);
        value = pointer;
        where = { binding: index, set: 0 };
      } else if (binding?.kind === "texture") {
        const texture = binding.index >= 0 ? this.bindings.texture(binding.index) : null;
        if (!texture) this.warn(`${this.label("texture", binding.index)} (${p.name}) was not captured, so sampling it gives zero`);
        value = new ImageValue(texture, this.label("texture", binding.index));
        frame.values.set(p.id, value);
        where = { binding: binding.index, set: 1 };
      } else if (binding?.kind === "sampler") {
        const sampler = binding.index >= 0 ? this.bindings.sampler(binding.index) : null;
        value = new SamplerValue(sampler, this.label("sampler", binding.index));
        frame.values.set(p.id, value);
        where = { binding: binding.index, set: 2 };
      } else if (binding?.kind === "stage_in") {
        value = this._stageIn(p.type, p.name);
        this._cell(frame, p.id, p.type, value);
      } else if (binding?.kind === "builtin") {
        value = this._builtinInput(binding.name, p.type, p.name);
        this._cell(frame, p.id, p.type, value);
        where = { builtin: 0 };
      } else if (binding?.kind === "threadgroup") {
        this.warn(`${p.name} is threadgroup memory, which one debugged invocation sees as zeroed`);
        this._cell(frame, p.id, p.type, value);
      } else {
        this._cell(frame, p.id, p.type, value);
      }
      this.boundParams.push({ id: p.id, name: p.name, type: p.type, value, storage: binding ? 2 : 0, ...where });
    }
  }

  private _cell(frame: Frame, id: number, type: number, value: Value): void {
    const cell: Cell = { value };
    frame.cells.set(id, cell);
    frame.locals.push({ id, cell, type });
  }

  label(kind: "buffer" | "texture" | "sampler", index: number): string {
    if (this.bindings.label) return this.bindings.label(kind, index);
    return index < 0 ? `an unnumbered ${kind}` : `${kind}(${index})`;
  }

  /** The `[[stage_in]]` struct, member by member, from the invocation's inputs. */
  private _stageIn(type: number, name: string): Value {
    const t = this._types.get(type);
    if (t?.kind !== "struct") {
      this.warn(`${name} is marked [[stage_in]] but is not a struct`);
      return this._types.zero(type);
    }
    return t.members.map((m) => {
      const attribute = m.attributes.find((a) => a.name === "attribute");
      const builtin = m.attributes.find((a) => BUILTIN_INPUT_NAMES.has(a.name));
      const user = m.attributes.find((a) => a.name === "user");
      let scalars: number[] | undefined;
      if (attribute) scalars = this.inputs.attributes.get(attribute.args[0] ?? 0);
      else if (builtin) {
        const value = this.inputs.builtins.get(builtin.name);
        if (value !== undefined) return this._fit(value, m.type);
      }
      // A varying matches by name; `[[user(locn2)]]` names it too.
      if (!scalars) scalars = this.inputs.varyings.get(m.name) ?? (user?.text ? this.inputs.varyings.get(user.text) : undefined);
      if (!scalars && user?.text) {
        const locn = /locn(\d+)/.exec(user.text);
        if (locn) scalars = this.inputs.attributes.get(Number(locn[1]));
      }
      if (!scalars) {
        this.warn(`the capture has no input for ${name}.${m.name}, so it reads as zero`);
        return this._types.zero(m.type);
      }
      return this._fromScalars(scalars, m.type);
    });
  }

  private _builtinInput(attribute: string, type: number, name: string): Value {
    const value = this.inputs.builtins.get(attribute);
    if (value === undefined) {
      this.warn(`the capture does not say what [[${attribute}]] was for ${name}, so it reads as zero`);
      return this._types.zero(type);
    }
    return this._fit(value, type);
  }

  /** A value brought to a type's shape: a scalar into a vector, a vector truncated or extended. */
  private _fit(value: Value, type: number): Value {
    const t = this._types.get(type);
    if (t?.kind === "vector") {
      const source = Array.isArray(value) ? value : [value];
      return Array.from({ length: t.count }, (_, i) => this._normalize(source[i] ?? 0, t.element));
    }
    if (t?.kind === "scalar") return this._normalize(Array.isArray(value) ? value[0] ?? 0 : value, type);
    return value;
  }

  private _fromScalars(scalars: number[], type: number): Value {
    const t = this._types.get(type);
    if (t?.kind === "vector") return Array.from({ length: t.count }, (_, i) => this._normalize(scalars[i] ?? 0, t.element));
    if (t?.kind === "matrix") {
      const column = this._types.get(t.column);
      const rows = column?.kind === "vector" ? column.count : 1;
      return Array.from({ length: t.columns }, (_, c) => Array.from({ length: rows }, (_, r) => this._normalize(scalars[c * rows + r] ?? 0, t.column)));
    }
    return this._normalize(scalars[0] ?? 0, type);
  }

  private _normalize(value: Value, type: number): Value {
    const scalar = this._types.scalarOf(type);
    return scalar ? normalize(value, scalar) : value;
  }

  warn(message: string): void {
    if (this.warnings.size < 64) this.warnings.add(message);
  }

  // ---------------------------------------------------------------------------------------
  // Stepping

  step(): InvocationStatus {
    if (this.finished) return this.status;
    if (++this.steps > MAX_STEPS) return this._fail(`stopped after ${MAX_STEPS.toLocaleString()} instructions: an endless loop?`);
    const frame = this.frames[this.frames.length - 1];
    const instr = this._instructions[frame.pc];
    if (!instr) return this._fail("ran off the end of a function");
    try {
      this._blocked = false;
      this._execute(frame, instr);
    } catch (e) {
      return this._fail(e instanceof Error ? e.message : String(e));
    }
    if (this._blocked) {
      this.status = "blocked";
      return this.status;
    }
    if (!this.finished) this.status = "running";
    return this.status;
  }

  run(): InvocationStatus {
    while (!this.finished) {
      if (this.step() === "blocked") return "blocked";
    }
    return this.status;
  }

  private _fail(message: string): InvocationStatus {
    this.error = message;
    this.status = "error";
    return this.status;
  }

  private _record(instr: Instr, id: number, value: Value): void {
    const result: StepResult = { inst: instr, id, value: cloneValue(value) };
    this._results.push(result);
    this.onResult?.(result);
  }

  private _set(frame: Frame, instr: Instr, id: number, value: Value): void {
    frame.values.set(id, value);
    this._record(instr, id, value);
  }

  private _get(frame: Frame, id: number): Value {
    if (id < 0) return null;
    const value = frame.values.get(id);
    if (value !== undefined) return value;
    const cell = frame.cells.get(id) ?? this.globals.get(id);
    return cell ? cell.value : null;
  }

  private _execute(frame: Frame, instr: Instr): void {
    const types = this._types;
    switch (instr.op) {
      case "const":
        frame.pc++;
        this._set(frame, instr, instr.dst, instr.value);
        return;
      case "move":
        frame.pc++;
        this._set(frame, instr, instr.dst, cloneValue(this._get(frame, instr.src)));
        return;
      case "addr": {
        frame.pc++;
        const cell = frame.cells.get(instr.variable) ?? this.globals.get(instr.variable);
        if (!cell) {
          this._fail(`${this._symbol(instr.variable)?.name ?? "a variable"} has no storage`);
          return;
        }
        const t = types.get(instr.type);
        const pointee = t?.kind === "pointer" ? t.pointee : instr.type;
        frame.values.set(instr.dst, new Pointer(cell, [], pointee, 0, instr.variable));
        return;
      }
      case "member":
      case "index": {
        frame.pc++;
        const base = this._get(frame, instr.ptr);
        if (!(base instanceof Pointer)) {
          this._fail("a member or element was read through something that is not a pointer");
          return;
        }
        const at = instr.op === "member" ? instr.member : Math.trunc(numberOf(this._get(frame, instr.at)));
        const t = types.get(instr.type);
        const pointee = t?.kind === "pointer" ? t.pointee : instr.type;
        frame.values.set(instr.dst, new Pointer(base.cell, [...base.path, at], pointee, base.storage, base.variable));
        return;
      }
      case "load": {
        frame.pc++;
        const ptr = this._get(frame, instr.ptr);
        this._set(frame, instr, instr.dst, this._load(ptr, instr.type));
        return;
      }
      case "store": {
        frame.pc++;
        const ptr = this._get(frame, instr.ptr);
        const value = this._get(frame, instr.value);
        this._store(ptr, value);
        // A store's "result" is the variable it wrote, which is what the values table shows.
        if (ptr instanceof Pointer) this._record(instr, ptr.variable, value);
        return;
      }
      case "extract": {
        frame.pc++;
        const value = this._get(frame, instr.value);
        this._set(frame, instr, instr.dst, extract(value, instr.indices));
        return;
      }
      case "extractAt": {
        frame.pc++;
        const value = this._get(frame, instr.value);
        const at = Math.trunc(numberOf(this._get(frame, instr.at)));
        this._set(frame, instr, instr.dst, Array.isArray(value) ? cloneValue(value[at] ?? types.zero(instr.type)) : value);
        return;
      }
      case "insert": {
        frame.pc++;
        const value = cloneValue(this._get(frame, instr.value));
        const element = this._get(frame, instr.element);
        this._set(frame, instr, instr.dst, insert(value, element, instr.indices));
        return;
      }
      case "insertAt": {
        frame.pc++;
        const value = cloneValue(this._get(frame, instr.value));
        const element = this._get(frame, instr.element);
        const at = Math.trunc(numberOf(this._get(frame, instr.at)));
        if (Array.isArray(value) && at >= 0 && at < value.length) value[at] = element;
        this._set(frame, instr, instr.dst, value);
        return;
      }
      case "unary": {
        frame.pc++;
        const a = this._get(frame, instr.a);
        const scalar = types.scalarOf(instr.type);
        const value = mapScalars(a, (x) => {
          switch (instr.kind) {
            case "-": return typeof x === "bigint" ? -x : -numberOf(x);
            case "+": return x;
            case "!": return !truthy(x);
            case "~": return typeof x === "bigint" ? ~x : ~numberOf(x);
          }
        });
        this._set(frame, instr, instr.dst, scalar ? mapScalars(value, (x) => normalize(x, scalar)) : value);
        return;
      }
      case "binary": {
        frame.pc++;
        const a = this._get(frame, instr.a);
        const b = this._get(frame, instr.b);
        this._set(frame, instr, instr.dst, this._binary(instr.kind, a, b, instr.type));
        return;
      }
      case "convert": {
        frame.pc++;
        const a = this._get(frame, instr.a);
        this._set(frame, instr, instr.dst, this._convert(a, instr.type));
        return;
      }
      case "bitcast": {
        frame.pc++;
        const a = this._get(frame, instr.a);
        this._set(frame, instr, instr.dst, this._bitcast(a, instr.a, instr.type));
        return;
      }
      case "construct": {
        frame.pc++;
        const args = instr.args.map((r) => this._get(frame, r));
        this._set(frame, instr, instr.dst, this._construct(args, instr.type));
        return;
      }
      case "builtin": {
        const args = instr.args.map((r) => this._get(frame, r));
        // Answered here rather than in the standard library: what the function was specialized
        // with belongs to the invocation, not to the language.
        if (instr.name === "is_function_constant_defined") {
          frame.pc++;
          this._set(frame, instr, instr.dst, this.definedConstants.has(Math.trunc(numberOf(args[0]))));
          return;
        }
        const context: BuiltinContext = {
          types,
          resultType: instr.type,
          argTypes: instr.args.map((r) => this._symbol(r)?.type ?? 0),
          warn: (m) => this.warn(m),
          derivative: (operand) => {
            if (!this.derivatives) return { dx: mapScalars(operand, () => 0), dy: mapScalars(operand, () => 0) };
            const d = this.derivatives.derivative(this, instr, operand);
            return d === "blocked" ? BLOCKED : d;
          },
          load: (ptr) => this._load(ptr, 0),
          store: (ptr, value) => this._store(ptr, value),
        };
        const result = callBuiltin(instr.name, args, context);
        if (result === BLOCKED) {
          // The pixel quad's other lanes have to reach here first; this instruction runs again.
          this._blocked = true;
          return;
        }
        frame.pc++;
        if (result === undefined) {
          this.warn(`${instr.name} is not a function this shader declares or the interpreter knows: it gives its first argument`);
          this._set(frame, instr, instr.dst, args[0] ?? null);
          return;
        }
        const scalar = result instanceof OpaqueValue ? null : types.scalarOf(instr.type);
        this._set(frame, instr, instr.dst, scalar ? mapScalars(result, (x) => normalize(x, scalar)) : result);
        return;
      }
      case "rayQuery":
        this._rayQuery(frame, instr);
        return;
      case "call": {
        const fn = this.program.ir.functions[instr.target];
        if (!fn) {
          frame.pc++;
          this._fail("a call to a function that is not in the shader");
          return;
        }
        frame.pc++;
        const args = instr.args.map((r) => this._get(frame, r));
        const next = this._frame(fn, instr.dst);
        fn.params.forEach((p, i) => {
          const value = args[i] === undefined ? this._types.zero(p.type) : cloneValue(args[i]);
          const t = types.get(p.type);
          // A pointer, texture or sampler parameter is a value; anything else gets a cell so it
          // can be assigned to inside the function.
          if (t?.kind === "pointer" || t?.kind === "texture" || t?.kind === "sampler") next.values.set(p.id, value);
          else {
            const cell: Cell = { value };
            next.cells.set(p.id, cell);
            next.locals.unshift({ id: p.id, cell, type: p.type });
          }
        });
        this.frames.push(next);
        return;
      }
      case "return": {
        const value = instr.value >= 0 ? cloneValue(this._get(frame, instr.value)) : this._types.zero(instr.type);
        this.frames.pop();
        if (!this.frames.length) {
          this.returned = value;
          this.status = "returned";
          return;
        }
        const caller = this.frames[this.frames.length - 1];
        if (frame.resultId >= 0) this._set(caller, instr, frame.resultId, value);
        return;
      }
      case "jump":
        frame.pc = instr.target;
        return;
      case "branch": {
        const cond = truthy(this._get(frame, instr.cond));
        frame.pc = cond ? instr.then : instr.otherwise;
        return;
      }
      case "discard":
        frame.pc++;
        this.status = "discarded";
        return;
    }
  }

  // ---------------------------------------------------------------------------------------
  // Ray queries

  /**
   * `intersector::intersect(ray, structure, mask, table)`.
   *
   * Runs in as many visits as there are bounding boxes to ask the shader about, plus one. The first
   * visit traverses the scene: triangles are settled there and then, and the boxes the ray entered
   * come back as candidates. Each later visit reads what the shader's intersection function said
   * about the previous candidate and pushes a frame for the next, leaving the program counter where
   * it is so that the function's `return` brings it back here.
   *
   * The debugger steps *through* the intersection function while this is happening, which is the
   * whole point: on Metal the traversal is in the shader, so the intersection function is the one
   * place a "why is nothing hit" question is answered.
   */
  private _rayQuery(frame: Frame, instr: Instr & { op: "rayQuery" }): void {
    const state = frame.query?.at === instr.index ? frame.query : this._beginRayQuery(frame, instr);
    if (!state) {
      frame.pc++;
      this._set(frame, instr, instr.dst, this._resultValue(noHit(), null));
      return;
    }
    // A candidate the shader was answering about: its result is in this instruction's own register,
    // which the callee's `return` wrote.
    if (state.pending) {
      const answer = this._get(frame, instr.dst);
      const accepted = this._acceptedIntersection(answer, state.pending);
      if (accepted !== null && accepted.distance >= state.ray.minDistance && accepted.distance <= state.limit) {
        state.hit = boxHit(state.scene, state.pending, accepted.distance);
        state.limit = accepted.distance;
        if (state.acceptAny) {
          this._finishRayQuery(frame, instr, state);
          return;
        }
      }
      state.pending = null;
    }

    // The next candidate the traversal found. One past the nearest hit so far cannot beat it, and
    // the candidates are in entry order, so the rest cannot either.
    while (state.next < state.candidates.length) {
      const candidate = state.candidates[state.next++];
      if (candidate.tMin > state.limit) break;
      // An opaque geometry, or a query forced opaque: Metal takes the box itself, at the point the
      // ray enters it, and calls nothing.
      if (candidate.opaque) {
        state.hit = boxHit(state.scene, candidate, candidate.tMin);
        state.limit = candidate.tMin;
        if (state.acceptAny) {
          this._finishRayQuery(frame, instr, state);
          return;
        }
        continue;
      }
      const fn = this._intersectionFunction(state.table, candidate);
      if (!fn) continue;
      state.pending = candidate;
      this._callIntersectionFunction(frame, instr, state, fn, candidate);
      return;    // the program counter stays here; the function's return brings us back
    }
    this._finishRayQuery(frame, instr, state);
  }

  /** Reads the arguments, traverses the scene, and starts a query; null when there is nothing to trace. */
  private _beginRayQuery(frame: Frame, instr: Instr & { op: "rayQuery" }): RayQueryState | null {
    const args = instr.args.map((r) => this._get(frame, r));
    const intersector = args[0] instanceof OpaqueValue && args[0].kind === INTERSECTOR_KIND
      ? args[0].handle as IntersectorHandle : null;
    const ray = this._rayOf(args[1]);
    const sceneHandle = args[2] instanceof OpaqueValue && args[2].kind === SCENE_KIND
      ? args[2].handle as SceneHandle : null;
    if (!sceneHandle?.scene) {
      this.warn("intersect was given something that is not an acceleration structure the capture holds, "
                + "so the ray misses");
      return null;
    }
    // `intersect(ray, structure)`, `intersect(ray, structure, mask)`,
    // `intersect(ray, structure, table)` and `intersect(ray, structure, mask, table)` are all
    // written, so the trailing arguments are read by what they are rather than by position.
    let mask = 0xFF;
    let table: RayFunctionTable | null = null;
    for (const arg of args.slice(3)) {
      if (arg instanceof OpaqueValue && arg.kind === TABLE_KIND) table = (arg.handle as TableHandle).table;
      else if (typeof arg === "number" || typeof arg === "bigint") mask = Math.trunc(numberOf(arg)) & 0xFF;
    }
    const options = intersector?.options ?? {};
    const { hit, candidates } = traceRay(sceneHandle.scene, ray, mask, options);
    const state: RayQueryState = {
      at: instr.index, ray, scene: sceneHandle.scene, table, candidates, next: 0,
      hit, limit: hit.type === INTERSECTION_NONE ? ray.maxDistance : hit.distance,
      acceptAny: options.acceptAny === true, pending: null,
    };
    if (options.forceOpaque) for (const c of state.candidates) c.opaque = true;
    frame.query = state;
    return state;
  }

  private _finishRayQuery(frame: Frame, instr: Instr & { op: "rayQuery" }, state: RayQueryState): void {
    frame.query = undefined;
    frame.pc++;
    this._set(frame, instr, instr.dst, this._resultValue(state.hit, state.ray));
  }

  /** MSL's `ray` struct, as the traversal reads one. */
  private _rayOf(value: Value): DebugRay {
    const members = Array.isArray(value) ? value : [];
    const vec3 = (v: Value): [number, number, number] => {
      const a = Array.isArray(v) ? v : [];
      return [numberOf(a[0] ?? 0), numberOf(a[1] ?? 0), numberOf(a[2] ?? 0)];
    };
    return {
      origin: vec3(members[0]),
      direction: vec3(members[1]),
      minDistance: numberOf(members[2] ?? 0),
      // A `ray` built with no max distance has INFINITY in it, which the traversal is happy with.
      maxDistance: numberOf(members[3] ?? Infinity),
    };
  }

  /** The `intersection_result` struct, in the member order types.ts declares. */
  private _resultValue(hit: RayHit, ray: DebugRay | null): Value {
    const matrix = (m: number[]): Value =>
      // 3x4 row-major as the instance holds it, into MSL's `float4x3`: four columns of three.
      [0, 1, 2, 3].map((c) => [m[c], m[4 + c], m[8 + c]]);
    return [
      hit.type,
      hit.distance,
      hit.primitiveId,
      hit.geometryId,
      hit.instanceId,
      hit.userInstanceId,
      [hit.barycentric[0], hit.barycentric[1]],
      hit.frontFacing,
      ray ? [...ray.origin] : [0, 0, 0],
      ray ? [...ray.direction] : [0, 0, 0],
      matrix(hit.objectToWorld),
      matrix(hit.worldToObject),
    ];
  }

  /** The shader's function for a candidate's table entry, or null with a warning. */
  private _intersectionFunction(table: RayFunctionTable | null, candidate: BoxCandidate): FunctionIr | null {
    const name = table?.entries[candidate.functionTableOffset] ?? null;
    if (!name) {
      this.warn(`no intersection function is bound at entry ${candidate.functionTableOffset} of the table, so `
                + `box ${candidate.primitive} of instance ${candidate.instance} cannot be asked about`);
      return null;
    }
    const fn = this.program.ir.functions.find((f) => f.name === name);
    if (!fn) {
      this.warn(`the table's entry ${candidate.functionTableOffset} runs '${name}', which is not a function of `
                + "this shader: it was linked in from another library, so the box cannot be asked about");
      return null;
    }
    return fn;
  }

  /**
   * Pushes a frame for the intersection function, with its parameters filled from the candidate.
   *
   * Its parameters bind the way an entry point's do — `[[origin]]`, `[[primitive_id]]`,
   * `[[buffer(0)]]` — except that the buffers come from the *table* rather than from the encoder:
   * a table binds its own for its functions (`setBuffer:offset:atIndex:` on the table), which the
   * capture records with the table's entries.
   */
  private _callIntersectionFunction(frame: Frame, instr: Instr & { op: "rayQuery" }, state: RayQueryState,
                                    fn: FunctionIr, candidate: BoxCandidate): void {
    const next = this._frame(fn, instr.dst);
    for (const p of fn.params) {
      const symbol = this._symbol(p.id);
      const binding = symbol?.binding;
      const t = this._types.get(p.type);
      let value: Value = this._types.zero(p.type);
      if (binding?.kind === "builtin") {
        switch (binding.name) {
          case "origin": value = [...state.ray.origin]; break;
          case "direction": value = [...state.ray.direction]; break;
          case "min_distance": value = state.ray.minDistance; break;
          // The interval the function is asked about: from the ray's own start to the nearest hit
          // so far, which is what makes a function that reports a farther hit be ignored.
          case "max_distance": value = state.limit; break;
          case "primitive_id": value = candidate.primitive; break;
          case "geometry_id": value = candidate.geometry; break;
          case "instance_id": value = candidate.instance; break;
          case "user_instance_id":
            value = state.scene.instances.find((i) => i.index === candidate.instance)?.userId ?? 0;
            break;
          case "geometry_intersection_function_table_offset": value = candidate.functionTableOffset; break;
          default:
            this.warn(`the intersection function's ${p.name} is [[${binding.name}]], which the traversal does `
                      + "not fill: it reads as zero");
            break;
        }
        this._cell(next, p.id, p.type, value);
        continue;
      }
      if (binding?.kind === "buffer") {
        const bytes = binding.index >= 0 ? state.table?.buffer(binding.index) ?? null : null;
        if (!bytes) {
          this.warn(`the intersection function's ${p.name} is buffer(${binding.index}) of the table, which the `
                    + "capture does not hold: it reads as zero");
        }
        const pointee = t?.kind === "pointer" ? t.pointee : p.type;
        const storage: BufferStorage = {
          bytes: bytes ? new Uint8Array(bytes) : new Uint8Array(Math.max(4, this._types.sizeOf(pointee))),
          type: t?.kind === "pointer" && !t.reference ? this._types.array(pointee, -1) : pointee,
          overrides: new Map(),
        };
        const cell: Cell = { value: null, buffer: storage };
        next.values.set(p.id, new Pointer(cell, [], storage.type, 0, p.id));
        continue;
      }
      this._cell(next, p.id, p.type, value);
    }
    this.frames.push(next);
  }

  /**
   * What an intersection function returned: whether it accepted, and at what distance.
   *
   * MSL lets it be a bare `bool` — accepted at the point the ray enters the box — or a struct whose
   * members carry `[[accept_intersection]]` and `[[distance]]`. The struct's members are read by
   * *position* here, since the returned value is a plain composite by the time it arrives: the
   * accept flag is the boolean member and the distance the float one, which is unambiguous for the
   * two-member struct the form requires.
   */
  private _acceptedIntersection(answer: Value, candidate: BoxCandidate): { distance: number } | null {
    if (typeof answer === "boolean") return answer ? { distance: candidate.tMin } : null;
    if (typeof answer === "number" || typeof answer === "bigint") {
      return numberOf(answer) !== 0 ? { distance: candidate.tMin } : null;
    }
    if (Array.isArray(answer)) {
      const accepted = answer.find((m) => typeof m === "boolean");
      if (accepted === false) return null;
      const distance = answer.find((m) => typeof m === "number");
      return { distance: typeof distance === "number" ? distance : candidate.tMin };
    }
    return null;
  }

  // ---------------------------------------------------------------------------------------
  // Memory

  /** Reads through a pointer: out of a buffer's bytes, or out of a cell's value. */
  private _load(ptr: Value, type: number): Value {
    if (!(ptr instanceof Pointer)) return ptr;
    const cell = ptr.cell;
    if (cell.buffer) {
      const located = this._types.locate(cell.buffer.type, ptr.path);
      if (!located) {
        this.warn("a buffer was read outside the type it was bound as");
        return this._types.zero(type || ptr.type);
      }
      const view = new DataView(cell.buffer.bytes.buffer, cell.buffer.bytes.byteOffset, cell.buffer.bytes.byteLength);
      return this._types.read(view, located.at, located.type);
    }
    let value: Value = cell.value;
    for (const index of ptr.path) {
      if (!Array.isArray(value)) return value;
      value = value[index] ?? null;
    }
    return cloneValue(value);
  }

  private _store(ptr: Value, value: Value): void {
    if (!(ptr instanceof Pointer)) return;
    const cell = ptr.cell;
    if (cell.buffer) {
      const located = this._types.locate(cell.buffer.type, ptr.path);
      if (!located) {
        this.warn("a buffer was written outside the type it was bound as");
        return;
      }
      const view = new DataView(cell.buffer.bytes.buffer, cell.buffer.bytes.byteOffset, cell.buffer.bytes.byteLength);
      this._types.write(view, located.at, located.type, value);
      return;
    }
    if (!ptr.path.length) {
      cell.value = cloneValue(value);
      return;
    }
    let target: Value = cell.value;
    for (let i = 0; i < ptr.path.length - 1; i++) {
      if (!Array.isArray(target)) return;
      target = target[ptr.path[i]] ?? null;
    }
    if (Array.isArray(target)) target[ptr.path[ptr.path.length - 1]] = cloneValue(value);
  }

  // ---------------------------------------------------------------------------------------
  // Operations

  private _binary(kind: string, a: Value, b: Value, type: number): Value {
    const scalar = this._types.scalarOf(type);
    const signed = scalar?.base !== "uint";
    const float = scalar?.base === "float";
    const comparison = ["==", "!=", "<", ">", "<=", ">="].includes(kind);
    // The result's scalar type is a bool for a comparison, so the arithmetic kind comes from the
    // operands: their shape is already the same, so either one says what they are.
    const apply = (x: Value, y: Value): Value => {
      if (typeof x === "bigint" || typeof y === "bigint") return bigintOp(kind, big(x), big(y));
      const p = numberOf(x), q = numberOf(y);
      switch (kind) {
        case "+": return p + q;
        case "-": return p - q;
        case "*": return p * q;
        case "/": return float ? p / q : q === 0 ? 0 : Math.trunc(p / q);
        case "%": return float ? p - q * Math.trunc(p / q) : q === 0 ? 0 : p % q;
        case "==": return p === q;
        case "!=": return p !== q;
        case "<": return p < q;
        case ">": return p > q;
        case "<=": return p <= q;
        case ">=": return p >= q;
        case "&": return signed ? p & q : (p & q) >>> 0;
        case "|": return signed ? p | q : (p | q) >>> 0;
        case "^": return signed ? p ^ q : (p ^ q) >>> 0;
        case "<<": return signed ? p << q : (p << q) >>> 0;
        case ">>": return signed ? p >> q : p >>> q;
        case "&&": return truthy(x) && truthy(y);
        case "||": return truthy(x) || truthy(y);
        default: return 0;
      }
    };
    const value = Array.isArray(a) && !Array.isArray(b) ? a.map((x) => this._binary(kind, x, b, type))
      : !Array.isArray(a) && Array.isArray(b) ? b.map((y) => this._binary(kind, a, y, type))
      : zipScalars(a, b, apply);
    if (comparison || !scalar) return value;
    return mapScalars(value, (x) => normalize(x, scalar));
  }

  private _convert(a: Value, type: number): Value {
    const t = this._types.get(type);
    if (t?.kind === "vector" && !Array.isArray(a)) {
      return Array.from({ length: t.count }, () => this._convertScalar(a, t.element));
    }
    if (t?.kind === "vector" && Array.isArray(a)) {
      return Array.from({ length: t.count }, (_, i) => this._convertScalar(a[i] ?? 0, t.element));
    }
    if (t?.kind === "matrix" && Array.isArray(a)) {
      return a.map((col) => this._convert(col, t.column));
    }
    if (t?.kind === "struct" || t?.kind === "array") return a;
    return this._convertScalar(a, type);
  }

  private _convertScalar(a: Value, type: number): Value {
    const scalar = this._types.scalarOf(type);
    if (!scalar) return a;
    const value = Array.isArray(a) ? a[0] ?? 0 : a;
    // Converting a float to an integer truncates towards zero, as C has it.
    if (scalar.base !== "float" && scalar.base !== "bool" && typeof value === "number") {
      return normalize(Math.trunc(value), scalar);
    }
    return normalize(value, scalar);
  }

  /** `as_type<T>(x)`: the same bits read as another type. */
  private _bitcast(a: Value, sourceRegister: number, type: number): Value {
    const from = this._symbol(sourceRegister)?.type ?? type;
    const size = Math.max(this._types.sizeOf(from), this._types.sizeOf(type), 4);
    const bytes = new Uint8Array(size);
    const view = new DataView(bytes.buffer);
    this._types.write(view, 0, from, a);
    return this._types.read(view, 0, type);
  }

  /** MSL's construction rules: a vector fills from the arguments' scalars, a matrix by columns. */
  private _construct(args: Value[], type: number): Value {
    const types = this._types;
    const t = types.get(type);
    if (!t) return null;
    if (t.kind === "vector") {
      const scalars: Value[] = [];
      for (const a of args) {
        if (Array.isArray(a)) scalars.push(...a.flat(2));
        else scalars.push(a);
      }
      const element = t.element;
      if (scalars.length === 1) return Array.from({ length: t.count }, () => this._convertScalar(scalars[0], element));
      return Array.from({ length: t.count }, (_, i) => this._convertScalar(scalars[i] ?? 0, element));
    }
    if (t.kind === "matrix") {
      const column = types.get(t.column);
      const rows = column?.kind === "vector" ? column.count : 1;
      // One scalar makes a diagonal matrix, which is what `float4x4(1.0)` is for.
      if (args.length === 1 && !Array.isArray(args[0])) {
        const d = args[0];
        return Array.from({ length: t.columns }, (_, c) =>
          Array.from({ length: rows }, (_, r) => (r === c ? this._convertScalar(d, types.elementOf(t.column)) : 0)));
      }
      if (args.every((a) => Array.isArray(a)) && args.length === t.columns) {
        return args.map((col) => this._convert(col, t.column));
      }
      const scalars: Value[] = [];
      for (const a of args) {
        if (Array.isArray(a)) scalars.push(...a.flat(2));
        else scalars.push(a);
      }
      return Array.from({ length: t.columns }, (_, c) =>
        Array.from({ length: rows }, (_, r) => this._convertScalar(scalars[c * rows + r] ?? 0, types.elementOf(t.column))));
    }
    if (t.kind === "array") {
      const length = t.length < 0 ? args.length : t.length;
      return Array.from({ length }, (_, i) => (args[i] === undefined ? types.zero(t.element) : this._convert(args[i], t.element)));
    }
    if (t.kind === "struct") {
      return t.members.map((m, i) => (args[i] === undefined ? types.zero(m.type) : this._convert(args[i], m.type)));
    }
    return args.length ? this._convert(args[0], type) : types.zero(type);
  }
}

// ---------------------------------------------------------------------------------------------

/** MSL attributes that name an input a fragment reads through `[[stage_in]]`. */
const BUILTIN_INPUT_NAMES = new Set([
  "position", "point_coord", "front_facing", "primitive_id", "sample_id", "sample_mask", "barycentric_coord",
  "render_target_array_index", "viewport_array_index", "layer",
]);

/** Attributes that say what an output is, so a member carrying several is read by the right one. */
const OUTPUT_ATTRIBUTES = new Set(["position", "point_size", "depth", "color", "user", "sample_mask", "stencil"]);

function outputWhere(attribute: string | undefined, arg: number | undefined, index: number): Partial<VariableView> {
  // `[[position]]` is built-in 0, which is what the debugger's comparison with the GPU looks for.
  if (attribute === "position") return { builtin: 0 };
  if (attribute === "point_size") return { builtin: 1 };
  if (attribute === "depth") return { builtin: 3 };
  if (attribute === "color") return { location: arg ?? 0 };
  if (attribute === "user") return { location: index };
  return { location: index };
}

function numberOf(v: Value): number {
  return typeof v === "number" ? v : typeof v === "bigint" ? Number(v) : v === true ? 1 : 0;
}

function big(v: Value): bigint {
  return typeof v === "bigint" ? v : BigInt(Math.trunc(numberOf(v)));
}

function truthy(v: Value): boolean {
  if (Array.isArray(v)) return v.length > 0 && truthy(v[0]);
  return typeof v === "bigint" ? v !== 0n : typeof v === "boolean" ? v : numberOf(v) !== 0;
}

function bigintOp(kind: string, x: bigint, y: bigint): Value {
  switch (kind) {
    case "+": return x + y;
    case "-": return x - y;
    case "*": return x * y;
    case "/": return y === 0n ? 0n : x / y;
    case "%": return y === 0n ? 0n : x % y;
    case "==": return x === y;
    case "!=": return x !== y;
    case "<": return x < y;
    case ">": return x > y;
    case "<=": return x <= y;
    case ">=": return x >= y;
    case "&": return x & y;
    case "|": return x | y;
    case "^": return x ^ y;
    case "<<": return x << y;
    case ">>": return x >> y;
    default: return 0n;
  }
}

function extract(value: Value, indices: number[]): Value {
  if (indices.length === 1) {
    if (!Array.isArray(value)) return value;
    return cloneValue(value[indices[0]] ?? null);
  }
  if (!Array.isArray(value)) return Array.from({ length: indices.length }, () => value);
  return indices.map((i) => cloneValue(value[i] ?? null));
}

function insert(value: Value, element: Value, indices: number[]): Value {
  if (!Array.isArray(value)) return element;
  if (indices.length === 1) {
    value[indices[0]] = Array.isArray(element) ? element[0] ?? null : element;
    return value;
  }
  indices.forEach((at, i) => {
    value[at] = Array.isArray(element) ? element[i] ?? null : element;
  });
  return value;
}

