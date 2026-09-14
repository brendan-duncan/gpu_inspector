// The shader debugger's stepping, without a UI: a DebugSession's invocation run to breakpoints, over,
// into and out of source lines (or SPIR-V instructions, for a module without line information), with
// the values each line produced. Shared by the debugger tab (shader_debugger_view.ts) and the MCP
// server's debug_shader.
//
// Nothing here knows which language the shader was written in: it steps a DebugInvocation and asks
// its DebugProgram (debug/program.ts) which line an instruction is on, so a Vulkan capture's SPIR-V
// and a Metal capture's MSL step identically.
import type { DebugSession } from "./shader_debug_setup.js";
import {
  instructionKey, sourceKey,
  type DebugInvocation, type DebugLocation, type DebugProgram, type DebugStep, type LineMode, type StepResult, type Stepper,
} from "./debug/program.js";

export type { LineMode, StepResult };
export { instructionKey, sourceKey };

export type StepKind = "over" | "into" | "out" | "continue" | "instruction";

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
    const modes = session.program.modes;
    this._mode = mode && modes.includes(mode) ? mode : modes.includes("source") ? "source" : "instruction";
    this.stepper = session.start();
    this._settle();
  }

  get program(): DebugProgram {
    return this.session.program;
  }

  get invocation(): DebugInvocation {
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
    if (!this.program.modes.includes(mode)) return;
    this._mode = mode;
    if (!this.running) this._settle();
  }

  /** The source location of an instruction, when the program's debug information has one. */
  location(step: DebugStep | null): DebugLocation | null {
    return this.program.locationOf(step);
  }

  /** The key a stop at an instruction has in the current mode; null where one cannot stop. */
  keyOf(step: DebugStep | null): string | null {
    if (!step) return null;
    if (this._mode === "instruction") return instructionKey(step.index);
    const loc = this.location(step);
    return loc ? sourceKey(loc.file, loc.line) : null;
  }

  /** The key of where the invocation is stopped. */
  get currentKey(): string | null {
    return this.keyOf(this.invocation.current);
  }

  /** The values of the line that ran last (at the current depth, or the call it stepped into from). */
  get lastLine(): LineValues {
    const depth = Math.min(this._lastDepth, this.invocation.depth || this._lastDepth);
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
    this._pending = { kind, depth: this.invocation.depth, key: this.currentKey, lastKey: this.currentKey };
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
      if ((entered && this.breakpoints.has(key)) || this._reached(p, key, inv.depth)) {
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
    const depth = inv.depth;
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
