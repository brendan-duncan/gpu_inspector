// The shader debugger's stepping, without a UI: a DebugSession's invocation run to breakpoints, over,
// into and out of source lines (or SPIR-V instructions, for a module without line information), with
// the values each line produced. Shared by the debugger tab (shader_debugger_view.ts) and the MCP
// server's debug_shader.
import type { DebugSession, Stepper } from "./shader_debug_setup.js";
import type { Invocation, StepResult } from "./spirv/interpreter.js";
import { Op, type Instruction, type SpirvModule } from "./spirv/module.js";
import { ImageValue, Pointer, SampledImageValue, SamplerValue, type Value } from "./spirv/values.js";
import type { DebugLocation } from "./vulkan/spirv_debug.js";

export type StepKind = "over" | "into" | "out" | "continue" | "instruction";

/** What a stop is keyed by: source lines ("f:line" keys) or instructions ("i:ordinal" keys). */
export type LineMode = "source" | "instruction";

/** A line's values: the results of the instructions that ran on it, the last time it ran. */
export interface LineValues {
  key: string | null;
  results: StepResult[];
}

const MAX_LINE_RESULTS = 2000;

interface PendingStep {
  kind: StepKind;
  /** Frame depth and line key the step started at. */
  depth: number;
  key: string | null;
  /** The last line key the step passed through. */
  lastKey: string | null;
}

export function sourceKey(file: number, line: number): string {
  return `f:${file}:${line}`;
}

export function instructionKey(ordinal: number): string {
  return `i:${ordinal}`;
}

/** Whether a module's instructions map to lines of source text it (or the host) has. */
export function hasLineInfo(module: SpirvModule): boolean {
  const info = module.debug;
  return !!info && info.form !== "none" && info.locations.some((l) => l !== null);
}

/** The instructions of the module's function bodies that execute (what a breakpoint can be put on). */
export function executableInstructions(module: SpirvModule): number[] {
  const out: number[] = [];
  for (const fn of module.functions.values()) {
    for (const block of fn.blocks) {
      for (let i = block.start + 1; i <= block.end; i++) {
        const inst = module.instructions[i];
        if (inst && !isNoop(module, inst)) out.push(i);
      }
    }
  }
  return out.sort((a, b) => a - b);
}

function isNoop(module: SpirvModule, inst: Instruction): boolean {
  switch (inst.op) {
    case Op.Nop: case Op.Line: case Op.NoLine: case Op.SelectionMerge: case Op.LoopMerge: case Op.Label:
      return true;
    case Op.ExtInst:
      return module.extSets.get(inst.words[2])?.startsWith("NonSemantic.") ?? false;
    default:
      return false;
  }
}

export class DebugController {
  readonly session: DebugSession;
  stepper: Stepper;
  /** Breakpoints, by line key (either mode's keys: the mode decides which apply). */
  readonly breakpoints = new Set<string>();
  private _mode: LineMode;
  /** Per frame depth, the values of the line running at that depth. */
  private _lines: LineValues[] = [];
  /** Per frame depth, the line before, for a line that computed nothing (a closing brace). */
  private _previous: LineValues[] = [];
  private _lastDepth = 1;
  private _pending: PendingStep | null = null;
  /** Instructions run by the current step so far. */
  stepSteps = 0;

  constructor(session: DebugSession, mode?: LineMode) {
    this.session = session;
    this._mode = mode ?? (hasLineInfo(session.module) ? "source" : "instruction");
    this.stepper = session.start();
    this._settle();
  }

  get module(): SpirvModule {
    return this.session.module;
  }

  get invocation(): Invocation {
    return this.stepper.invocation;
  }

  get finished(): boolean {
    return this.invocation.finished;
  }

  /** A step is under way (proceed() has more to run). */
  get running(): boolean {
    return this._pending !== null;
  }

  get mode(): LineMode {
    return this._mode;
  }

  /** Switches between source lines and instructions; the invocation stays where it is. */
  set mode(mode: LineMode) {
    if (mode === "source" && !hasLineInfo(this.module)) return;
    this._mode = mode;
    if (!this.running) this._settle();
  }

  /** The source location of an instruction, when the module's debug information has one. */
  location(inst: Instruction | null): DebugLocation | null {
    return inst ? this.module.debug?.locations[inst.index] ?? null : null;
  }

  /** The key a stop at an instruction has in the current mode; null where one cannot stop. */
  keyOf(inst: Instruction | null): string | null {
    if (!inst) return null;
    if (this._mode === "instruction") return instructionKey(inst.index);
    const loc = this.location(inst);
    return loc ? sourceKey(loc.file, loc.line) : null;
  }

  /** The key of where the invocation is stopped. */
  get currentKey(): string | null {
    return this.keyOf(this.invocation.current);
  }

  /** The values of the line that ran last (at the current depth, or the call it stepped into from). */
  get lastLine(): LineValues {
    const depth = Math.min(this._lastDepth, this.invocation.frames.length || this._lastDepth);
    const line = this._lines[depth - 1];
    if (line?.results.length) return line;
    return this._previous[depth - 1] ?? line ?? { key: null, results: [] };
  }

  toggleBreakpoint(key: string): boolean {
    if (this.breakpoints.has(key)) {
      this.breakpoints.delete(key);
      return false;
    }
    this.breakpoints.add(key);
    return true;
  }

  /** Starts the invocation again from the beginning, keeping the breakpoints. */
  restart(): void {
    this._pending = null;
    this._lines = [];
    this._previous = [];
    this._lastDepth = 1;
    this.stepper = this.session.start();
    this._settle();
  }

  /** Begins a step; proceed() runs it. */
  begin(kind: StepKind): void {
    if (this.finished) return;
    this._pending = { kind, depth: this.invocation.frames.length, key: this.currentKey, lastKey: this.currentKey };
    this.stepSteps = 0;
  }

  /** Runs up to `budget` instructions of the step begun; true when it has stopped (or the invocation finished). */
  proceed(budget = Infinity): boolean {
    const p = this._pending;
    if (!p) return true;
    const inv = this.invocation;
    let stopped = false;
    for (let n = 0; n < budget && !inv.finished; n++) {
      this._stepOnce();
      this.stepSteps++;
      if (inv.finished) break;
      const key = this.currentKey;
      if (key === null) continue;
      const entered = key !== p.lastKey;
      p.lastKey = key;
      // A breakpoint stops when its line is entered, not on each of its instructions.
      if ((entered && this.breakpoints.has(key)) || this._reached(p, key, inv.frames.length)) {
        stopped = true;
        break;
      }
    }
    if (stopped || inv.finished) this._pending = null;
    return this._pending === null;
  }

  /** Abandons the step under way, stopping at the next instruction a stop can be at. */
  cancel(): void {
    if (!this._pending) return;
    this._pending = null;
    this._settle();
  }

  /** begin() and proceed() to the end of the step. */
  advance(kind: StepKind, budget = Infinity): boolean {
    this.begin(kind);
    return this.proceed(budget);
  }

  private _reached(p: PendingStep, key: string, depth: number): boolean {
    switch (p.kind) {
      case "instruction": return true;
      case "into": return key !== p.key || depth !== p.depth;
      case "over": return depth < p.depth || (depth === p.depth && key !== p.key);
      case "out": return depth < p.depth;
      case "continue": return false;
    }
  }

  private _stepOnce(): void {
    const inv = this.invocation;
    const key = this.currentKey;
    const depth = inv.frames.length;
    this.stepper.step();
    const results = inv.takeResults();
    if (key !== null || !this._lines[depth - 1]) {
      const line = this._lines[depth - 1];
      if (!line || (key !== null && line.key !== key)) {
        if (line?.results.length) this._previous[depth - 1] = line;
        this._lines[depth - 1] = { key, results: [] };
      }
    }
    this._lines.length = Math.min(this._lines.length, depth);
    this._previous.length = Math.min(this._previous.length, depth);
    const line = this._lines[depth - 1];
    for (const r of results) if (line.results.length < MAX_LINE_RESULTS) line.results.push(r);
    this._lastDepth = depth;
  }

  /** Runs to the first instruction a stop can be at (source mode skips instructions without a line). */
  private _settle(): void {
    let guard = 0;
    while (!this.finished && this.currentKey === null && guard++ < 1_000_000) this._stepOnce();
  }
}

// ---------------------------------------------------------------------------------------------
// Values as text

/** The type of the value an id has: a variable's pointee, else its result type. */
export function valueType(m: SpirvModule, id: number): number {
  const type = m.idTypes.get(id) ?? m.globals.get(id)?.type ?? 0;
  const t = m.types.get(type);
  return t?.kind === "pointer" ? t.pointee : type;
}

/** A step result's type: the instruction's result type, or for a store the stored variable's. */
export function resultType(m: SpirvModule, r: StepResult): number {
  return r.inst.resultType || valueType(m, r.id);
}

function scalarText(v: Value): string {
  if (typeof v === "number") {
    if (Number.isInteger(v)) return String(v);
    if (Number.isNaN(v)) return "NaN";
    if (!Number.isFinite(v)) return v > 0 ? "inf" : "-inf";
    const a = Math.abs(v);
    return a !== 0 && (a >= 1e7 || a < 1e-4) ? v.toExponential(4) : String(+v.toPrecision(7));
  }
  if (typeof v === "bigint") return String(v);
  if (typeof v === "boolean") return v ? "true" : "false";
  return "?";
}

/** One line of text for a value: scalars and vectors in full, composites shortened past `limit` elements. */
export function valueText(module: SpirvModule, type: number, value: Value | undefined, limit = 16): string {
  if (value === undefined || value === null) return "undefined";
  if (value instanceof Pointer) return `→ ${module.nameOf(value.variable)}${value.path.length ? `[${value.path.join("][")}]` : ""}`;
  if (value instanceof SampledImageValue) return `${imageText(value.image)}, ${samplerText(value.sampler)}`;
  if (value instanceof ImageValue) return imageText(value);
  if (value instanceof SamplerValue) return samplerText(value);
  if (!Array.isArray(value)) return scalarText(value);
  const t = module.types.get(type);
  const inner = t?.kind === "vector" ? t.element : t?.kind === "matrix" ? t.column : t?.kind === "array" || t?.kind === "runtimeArray" ? t.element : 0;
  const parts: string[] = [];
  for (let i = 0; i < Math.min(value.length, limit); i++) {
    const memberType = t?.kind === "struct" ? t.members[i] : inner;
    const text = valueText(module, memberType, value[i], limit);
    const name = t?.kind === "struct" ? module.memberNames.get(type)?.get(i) : undefined;
    parts.push(name ? `${name}: ${text}` : text);
  }
  if (value.length > limit) parts.push(`… ${value.length - limit} more`);
  return t?.kind === "struct" ? `{ ${parts.join(", ")} }` : t?.kind === "array" || t?.kind === "runtimeArray" ? `[${parts.join(", ")}]` : `(${parts.join(", ")})`;
}

function imageText(image: ImageValue): string {
  const t = image.texture;
  return t ? `${image.binding}: ${t.format.replace(/^VK_FORMAT_/, "")} ${t.width}x${t.height}${t.layers > 1 ? `x${t.layers}` : ""}` : `${image.binding}: not captured`;
}

function samplerText(sampler: SamplerValue): string {
  const s = sampler.sampler;
  return s ? `${sampler.binding}: ${s.minFilter}/${s.magFilter} ${s.address[0]}${s.compareOp ? ` compare ${s.compareOp}` : ""}` : `${sampler.binding}: default sampler`;
}

/** Whether a value holds a NaN or an infinity. */
export function nonFinite(value: Value | undefined): boolean {
  if (typeof value === "number") return !Number.isFinite(value);
  return Array.isArray(value) && value.some(nonFinite);
}

/** Flattens a value's scalars to numbers (booleans 0 / 1), for comparisons. */
export function scalars(value: Value | undefined): number[] {
  if (Array.isArray(value)) return value.flatMap(scalars);
  if (typeof value === "number") return [value];
  if (typeof value === "bigint") return [Number(value)];
  if (typeof value === "boolean") return [value ? 1 : 0];
  return [];
}
