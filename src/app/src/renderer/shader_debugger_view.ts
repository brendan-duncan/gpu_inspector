// The shader debugger in a tab of its own, after RenderDoc's shader viewer and WebGPU Inspector's
// shader debugger: one invocation of a draw's vertex or fragment shader, or of a dispatch's compute
// shader, run in the interpreter of whichever language the shader is in — SPIR-V (spirv/) for a
// Vulkan capture, MSL (msl/) for a Metal one, and for a D3D12 capture the stage's HLSL compiled to
// SPIR-V by dxc (d3d12/shader_debug.ts) — on the capture's inputs (shader_debug_setup.ts),
// stepped by source line (shader_debugger.ts) with breakpoints, the values each line computed,
// the locals, the call stack, the inputs, outputs and resources, and at the end how the outputs
// compare with what the GPU produced (the replay's vertex outputs, the render target's pixel).
//
// Everything here goes through DebugProgram and DebugInvocation (debug/program.ts), so the tab
// itself knows nothing about either language.
import { Button } from "./widget/button.js";
import { Div } from "./widget/div.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import type { CaptureData } from "./capture_data.js";
import { escapeHtml, highlightLines } from "./code_editor.js";
import { drawState } from "./draw_state.js";
import type { MeshOutput } from "./mesh_output.js";
import type { OverdrawPassKey } from "./overdraw.js";
import { DebugController, sourceKey, type StepKind } from "./shader_debugger.js";
import { compareWithOriginal, coveredPixel, interpretsVertexOutputs, pixelRasterState, prepareDebugSession, sameValue, vertexOutputsOf, type DebugContext, type DebugSession, type DebugTarget, type Stepper } from "./shader_debug_setup.js";
import type { StageSource } from "./shader_cache.js";
import { resolveSourcesFromHost } from "./shader_source_view.js";
import type { SessionContext } from "./session_panel.js";
import type { DebugProgram, VariableView } from "./debug/program.js";
import { nonFinite, scalars, type Value } from "./debug/values.js";
import { SpirvProgram } from "./spirv/program.js";
import { sourceLineMap } from "./vulkan/spirv_debug.js";
import type { ObjectLookup } from "./vulkan/vulkan_object.js";
import type { StructureDatabase } from "./acceleration_scene.js";
import type { CaptureCommand, DebugTranslationResult, ShaderTextResult } from "../shared/protocol.js";

/** What to debug; the parts left out are chosen (a pixel the draw covers, the first vertex or invocation). */
export type DebugRequest =
  | { stage: "vertex"; command: number; vertex?: number; instance?: number; /** A VS Out row: the vertex and instance it was. */ record?: number }
  | { stage: "fragment"; command: number; x?: number; y?: number }
  | { stage: "compute"; command: number; invocation?: [number, number, number] };

export interface ShaderDebuggerOptions {
  /** Testing aid: steps over this many lines once open (-1 runs to the end). */
  steps?: number;
  /**
   * Testing aid: runs to this source line and stops there, so a test can read what the line before
   * it computed. Step counts cannot reach a line inside a loop, which is where a ray query is.
   */
  stopAtLine?: number;
  /** Debug GLSL decompiled from the SPIR-V instead of the SPIR-V (the tab's choice when left out). */
  decompiled?: boolean;
}

/** What the view needs of the capture tab it belongs to. */
export interface ShaderDebuggerHost {
  readonly data: CaptureData;
  readonly db: ObjectLookup & StructureDatabase & { blobData: Map<string, Uint8Array> };
  readonly session?: SessionContext;
  passLabelOf(key: OverdrawPassKey): string;
  passOfDraw(cmd: CaptureCommand): OverdrawPassKey | null;
  selectCommand(index: number): void;
  /** Vulkan: the replay's vertex shader outputs of a draw (a fragment's inputs are rasterized from them). */
  meshOutput(command: number): Promise<MeshOutput>;
  /** The vertex shader's input names by location; loads the pipeline's shaders from the layer too. */
  inputNames(cmd: CaptureCommand): Promise<Map<number, string>>;
  /** spirv-dis output for a module without source. */
  disassemble(spirv: Uint8Array): Promise<ShaderTextResult>;
  /** A module decompiled to GLSL and recompiled with line information; absent where the tools are not. */
  decompile?(spirv: Uint8Array, stage: string, entryPoint: string): Promise<DebugTranslationResult>;
  /** D3D12: a stage's HLSL compiled to SPIR-V with line information (`target` its profile, "ps_6_0"); absent where dxc is not. */
  compileHlsl?(bytecode: Uint8Array, stage: string, entryPoint: string, target: string): Promise<DebugTranslationResult>;
  /** Fetches an object's payload from the layer: a live Metal capture's library source. */
  fetchBlob?(objectId: number, index: number): Promise<Uint8Array | null>;
}

/** Instructions run between UI updates while a step runs. */
const STEP_BUDGET = 50_000;
const MAX_ROWS = 200;
const STAGE_LABEL = { vertex: "Vertex", fragment: "Pixel", compute: "Compute" } as const;

// Stepping icons (inline SVG in the button's text color), after a debugger's usual toolbar.
const ICON_CONTINUE = '<svg viewBox="0 0 16 16" aria-label="Continue"><path d="M3 3v10" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/><path d="M6 3l7.5 5L6 13z" fill="currentColor"/></svg>';
const ICON_PAUSE = '<svg viewBox="0 0 16 16" aria-label="Pause"><rect x="3.5" y="3" width="3" height="10" rx="0.6" fill="currentColor"/><rect x="9.5" y="3" width="3" height="10" rx="0.6" fill="currentColor"/></svg>';
const ICON_STEP_OVER = '<svg viewBox="0 0 16 16" aria-label="Step Over"><path d="M2.8 9.2a5.2 5 0 0 1 10.4 0" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><path d="M10.9 7.2l2.3 2.3 2.3-2.3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><circle cx="8" cy="13" r="1.6" fill="currentColor"/></svg>';
const ICON_STEP_INTO = '<svg viewBox="0 0 16 16" aria-label="Step Into"><path d="M8 1.5v7.2" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><path d="M5 6l3 3 3-3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><circle cx="8" cy="13" r="1.6" fill="currentColor"/></svg>';
const ICON_STEP_OUT = '<svg viewBox="0 0 16 16" aria-label="Step Out"><path d="M8 9.5V2.2" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><path d="M5 5l3-3 3 3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><circle cx="8" cy="13" r="1.6" fill="currentColor"/></svg>';
const ICON_RESTART = '<svg viewBox="0 0 16 16" aria-label="Restart"><path d="M13.2 9.2A5.3 5.3 0 1 1 12 4.3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/><path d="M13.6 1.8v3.6h-3.6" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>';
const TIP_CONTINUE = "Continue (F5): run to the next breakpoint or the end";
const TIP_PAUSE = "Pause (F5): stop the run where it is";

interface CodeLine {
  html: string;
  number: string;
  key: string | null;
}

/** The original module's run of a translation's invocation, to check the translation against. */
interface OriginalRun {
  stepper: Stepper | null;
  done: boolean;
  error: string;
}

export class ShaderDebuggerView {
  readonly host: ShaderDebuggerHost;
  readonly root: Div;
  private _request: DebugRequest;
  private _options: ShaderDebuggerOptions;
  private _session: DebugSession | null = null;
  private _ctl: DebugController | null = null;
  private _error = "";
  private _preparing = false;
  private _token = 0;
  private _timer = 0;
  private _pause = false;
  /** The call stack frame the locals and hovers are of (0 is the innermost). */
  private _frame = 0;
  /** The source file shown. */
  private _file = -1;
  private _disassembly: string | null = null;
  private _collapsed = new Map<string, boolean>([["Resources", true], ["Globals", true]]);
  /** Breakpoints outlive a restart and a new invocation of the same shader. */
  private _breakpoints = new Set<string>();
  private _inputNames = new Map<number, string>();
  /** Stepping GLSL decompiled from the SPIR-V (and compiled back) instead of the SPIR-V itself. */
  private _decompiled = false;
  /** Translations by stage module and entry point: picking another invocation reuses them. */
  private _translations = new Map<string, Promise<Uint8Array>>();
  /** The same invocation of the original module, run to its end to check a translation against. */
  private _original: OriginalRun | null = null;

  private _status: Span | null = null;
  private _notes: Div | null = null;
  private _code: Widget | null = null;
  private _fileBar: Div | null = null;
  private _side: Div | null = null;
  private _buttons: Record<string, Button> = {};
  private _modeSelect: Select | null = null;
  private _split = 58;
  private readonly _onKey = (e: KeyboardEvent): void => this._key(e);

  constructor(host: ShaderDebuggerHost, request: DebugRequest, options: ShaderDebuggerOptions = {}) {
    this.host = host;
    this._request = request;
    this._options = options;
    this._decompiled = options.decompiled ?? false;
    this.root = new Div(null, { class: "shader-debugger" });
    document.addEventListener("keydown", this._onKey);
    void this._open();
  }

  get label(): string {
    return `Debug ${STAGE_LABEL[this._request.stage]} #${this._request.command}`;
  }

  /** Points the view at another invocation. */
  show(request: DebugRequest, options: ShaderDebuggerOptions = {}): void {
    const sameShader = request.command === this._request.command && request.stage === this._request.stage;
    const sameCode = options.decompiled === undefined || options.decompiled === this._decompiled;
    if (!sameShader || !sameCode) this._breakpoints.clear();
    this._request = request;
    this._options = options;
    if (options.decompiled !== undefined) this._decompiled = options.decompiled;
    void this._open();
  }

  dispose(): void {
    this._stopTimer();
    this._token++;
    document.removeEventListener("keydown", this._onKey);
  }

  /** The UI tests' view of the tab (tools/ui_tests.py). */
  debugState(): Record<string, unknown> {
    const ctl = this._ctl;
    const inv = ctl?.invocation;
    const loc = ctl ? ctl.location(inv?.current ?? null) : null;
    return {
      request: this._request, preparing: this._preparing, error: this._error || null,
      description: this._session?.description ?? null, notes: this._session?.notes ?? [],
      decompiled: this._decompiled,
      original: this._original ? this._originalState() : null,
      mode: ctl?.mode ?? null, status: inv?.status ?? null, invocationError: inv?.error || null, steps: inv?.steps ?? 0,
      line: loc?.line ?? null, instruction: inv?.current?.index ?? null, depth: inv?.depth ?? 0, running: ctl?.running ?? false,
      breakpoints: [...this._breakpoints],
      outputs: inv ? inv.outputs().map((o) => ({ name: o.name, location: o.location, builtin: o.builtin, value: scalars(o.value) })) : [],
      targetPixel: this._session?.targetPixel ?? null,
      replayedOutputs: this._session?.replayedOutputs ?? null,
      lineValues: ctl?.lastLine.results.length ?? 0,
      // The values the last line computed, named, so a test can assert what a shader worked out
      // rather than only that it ran. A ray query needs this: "the kernel returned" says nothing
      // about whether the traversal found the triangle.
      lastValues: ctl ? ctl.lastLine.results.slice(-32).map((r) => ({
        name: this._session?.program.nameOf(r.id) ?? "", value: scalars(r.value),
      })) : [],
      codeLines: this._code?.element.querySelectorAll(".code-line").length ?? 0,
      warnings: inv ? [...inv.warnings] : [],
    };
  }

  private _originalState(): Record<string, unknown> {
    const c = this._comparison();
    return {
      done: this._original?.done ?? false, error: this._original?.error || null,
      matches: c?.matches ?? null, status: c?.status ?? null,
      compared: c?.values.map((v) => v.label) ?? [], differences: c?.values.filter((v) => !v.matches).map((v) => v.label) ?? [],
    };
  }

  // ---------------------------------------------------------------------------------------
  // Setup

  private async _open(): Promise<void> {
    const token = ++this._token;
    this._stopTimer();
    this._session = null;
    this._ctl = null;
    this._error = "";
    this._frame = 0;
    this._file = -1;
    this._disassembly = null;
    this._original = null;
    this.root.html = "";
    this._buildChrome();
    this._preparing = true;
    this._setStatus("Preparing the invocation...");
    try {
      // The shaders' SPIR-V comes from the layer on demand; reflection names the vertex attributes.
      const cmd = this.host.data.commands[this._request.command];
      this._inputNames = cmd ? await this.host.inputNames(cmd).catch(() => new Map<number, string>()) : new Map<number, string>();
      if (token !== this._token) return;
      const target = await this._resolve(token);
      if (token !== this._token || !target) return;
      const session = await prepareDebugSession(this._context(), target);
      if (token !== this._token) return;
      // SPIR-V compiled with only file names can have its source found under the host's source
      // roots; MSL is the capture's own text, so there is nothing to look for.
      const debug = session.program instanceof SpirvProgram ? session.program.module.debug : null;
      if (this.host.session && debug) await resolveSourcesFromHost(debug, this.host.session).catch(() => false);
      if (token !== this._token) return;
      this._session = session;
      const ctl = new DebugController(session, session.program.hasSourceText() ? "source" : "instruction");
      for (const b of this._breakpoints) ctl.breakpoints.add(b);
      this._ctl = ctl;
      this._request = requestOf(session.target);
    } catch (e) {
      if (token !== this._token) return;
      this._error = e instanceof Error ? e.message.split("\n")[0] : String(e);
    } finally {
      if (token === this._token) this._preparing = false;
    }
    this._buildChrome();
    if (!this._ctl) {
      this._setStatus(`Cannot debug: ${this._error}`);
      return;
    }
    await this._renderCode(token);
    if (token !== this._token) return;
    this._refresh();
    const stopAt = this._options.stopAtLine;
    if (stopAt !== undefined && stopAt > 0) {
      // File 0: an MSL program is one file, and a Vulkan one stepped as source is too.
      this._ctl.breakpoints.add(sourceKey(0, stopAt));
      this._breakpoints.add(sourceKey(0, stopAt));
      this._ctl.advance("continue");
      this._refresh();
      return;
    }
    const steps = this._options.steps;
    if (steps !== undefined) {
      if (steps < 0) this._ctl.advance("continue");
      else for (let i = 0; i < steps && !this._ctl.finished; i++) this._ctl.advance("over");
      this._refresh();
    }
  }

  private _context(): DebugContext {
    return {
      data: this.host.data, db: this.host.db, inputNames: this._inputNames,
      meshOutput: (command) => this.host.meshOutput(command),
      fetchBlob: this.host.fetchBlob ? (id, index) => this.host.fetchBlob!(id, index) : undefined,
      translate: this._decompiled && this.host.decompile ? (bytes, source) => this._translate(bytes, source) : undefined,
      compileHlsl: this.host.compileHlsl ? (bytes, source, target) => this._compileHlsl(bytes, source, target) : undefined,
    };
  }

  private _translate(bytes: Uint8Array, source: StageSource): Promise<Uint8Array> {
    return this._translated(source, "Decompiling the SPIR-V to GLSL and compiling it back with line information...", async () => {
      const r = await this.host.decompile!(bytes, source.stage, source.entryPoint);
      if (!r.ok || !r.spirv) throw new Error(`the SPIR-V could not be decompiled for debugging: ${r.log.trim() || `${r.tool} failed`}`);
      return r.spirv;
    });
  }

  private _compileHlsl(bytes: Uint8Array, source: StageSource, target: string): Promise<Uint8Array> {
    return this._translated(source, "Compiling the stage's HLSL to SPIR-V with line information...", async () => {
      const r = await this.host.compileHlsl!(bytes, source.stage, source.entryPoint, target);
      if (!r.ok || !r.spirv) throw new Error(`the HLSL could not be compiled for debugging: ${r.log.trim() || `${r.tool} failed`}`);
      return r.spirv;
    });
  }

  /** A stage's translation, made once and kept: picking another invocation of the same shader reuses it. */
  private _translated(source: StageSource, status: string, make: () => Promise<Uint8Array>): Promise<Uint8Array> {
    const key = `${source.object.id}:${source.blobIndex}:${source.stage}:${source.entryPoint}`;
    let translation = this._translations.get(key);
    if (!translation) {
      this._setStatus(status);
      translation = make();
      // A failure is not kept: the tools may be there on the next try.
      translation.catch(() => this._translations.delete(key));
      this._translations.set(key, translation);
    }
    return translation;
  }

  /** Switches between stepping the SPIR-V and the GLSL decompiled from it: the same invocation, restarted. */
  private _setDecompiled(on: boolean): void {
    if (on === this._decompiled) return;
    this._decompiled = on;
    // A breakpoint is a line of one or the other.
    this._breakpoints.clear();
    void this._open();
  }

  /** The request with its choices made: a pixel the draw covers, a VS Out row's vertex and instance. */
  private async _resolve(token: number): Promise<DebugTarget | null> {
    const r = this._request;
    const cmd = this.host.data.commands[r.command];
    if (!cmd) throw new Error(`the capture has no command ${r.command}`);
    if (r.stage === "compute") return { stage: "compute", command: r.command, invocation: r.invocation ?? [0, 0, 0] };
    const state = drawState(this.host.data, this.host.db, cmd);
    // Where the draw's vertex outputs come from: a Vulkan replay, or the interpreter itself (Metal, D3D12).
    const interpreted = interpretsVertexOutputs(state);
    const vertexOutputs = (): Promise<MeshOutput> => vertexOutputsOf(this._context(), cmd, state);
    if (r.stage === "vertex") {
      if (r.record === undefined) return { stage: "vertex", command: r.command, vertex: r.vertex ?? 0, instance: r.instance ?? 0 };
      const first = await prepareDebugSession(this._context(), { stage: "vertex", command: r.command, vertex: 0, instance: 0 });
      const n = Math.max(1, first.limits.vertices ?? 1);
      const topology = await vertexOutputs().then((m) => m.topology, () => "");
      const { vertex, instance } = recordVertex(r.record, n, topology);
      return { stage: "vertex", command: r.command, vertex, instance };
    }
    if (r.x !== undefined && r.y !== undefined) return { stage: "fragment", command: r.command, x: r.x, y: r.y };
    this._setStatus(interpreted
      ? "Running the draw's vertex shader, to find a pixel it covers..."
      : "Replaying the capture for the draw's vertex shader outputs, to find a pixel it covers...");
    const mesh = await vertexOutputs();
    if (token !== this._token) return null;
    if (!mesh.measured) throw new Error(`the draw's vertex shader outputs could not be captured: ${mesh.note ?? "the replay did not reach the draw"}`);
    const pixel = coveredPixel(state, mesh, pixelRasterState(this._context(), cmd, state));
    if (!pixel) throw new Error("no triangle of the draw is visible in its viewport: enter a pixel to debug");
    return { stage: "fragment", command: r.command, x: pixel.x, y: pixel.y };
  }

  // ---------------------------------------------------------------------------------------
  // Layout

  private _buildChrome(): void {
    this.root.html = "";
    const r = this._request;
    const cmd = this.host.data.commands[r.command];
    const pass = cmd ? this.host.passOfDraw(cmd) : null;
    const stage = this._session?.stage;
    new Div(this.root, {
      class: "capture-texture-head",
      text: `${pass ? `${this.host.passLabelOf(pass)} — ` : ""}#${r.command} ${cmd?.method ?? ""}${stage ? ` — ${stageName(r.stage)} shader, entry point ${stage.entryPoint}` : ""}`,
    });

    const bar = new Div(this.root, { class: "mesh-view-bar shader-debugger-bar" });
    this._buildPicker(bar);
    const sep = (): void => void new Span(bar, { class: "shader-debugger-sep" });
    sep();
    const button = (name: string, html: string, tooltip: string, callback: () => void): void => {
      this._buttons[name] = new Button(bar, { html, class: "btn btn-sm btn-icon", tooltip, callback });
    };
    button("continue", ICON_CONTINUE, TIP_CONTINUE, () => (this._timer ? this._pauseRun() : this._step("continue")));
    button("over", ICON_STEP_OVER, "Step Over (F10): run to the next line, over function calls", () => this._step("over"));
    button("into", ICON_STEP_INTO, "Step Into (F11): run to the next line, into function calls", () => this._step("into"));
    button("out", ICON_STEP_OUT, "Step Out (Shift+F11): run until the function returns", () => this._step("out"));
    button("restart", ICON_RESTART, "Restart (Ctrl+Shift+F5): start the invocation again, keeping the breakpoints", () => this._restart());
    sep();
    // Decompiling to GLSL is spirv-cross over the capture's SPIR-V: a Vulkan capture only.
    if (this.host.decompile && this.host.data.api === "vulkan") {
      const code = new Select(bar, {
        options: ["Original SPIR-V", "Decompiled GLSL"],
        index: this._decompiled ? 1 : 0,
        onChange: (_v: string, index: number) => this._setDecompiled(index === 1),
      });
      code.tooltip = "Debug the capture's SPIR-V, or GLSL that spirv-cross decompiles from it and glslang compiles back with line " +
        "information: source lines to step for a shader built without debug information, checked against the original at the end";
    }
    // A program with only source (MSL) offers one mode; SPIR-V offers its disassembly beside it.
    const modes = this._ctl ? [...this._ctl.program.modes] : (["source", "instruction"] as const).slice();
    this._modeSelect = new Select(bar, {
      options: modes.map((m) => (m === "source" ? "Source" : "Disassembly")),
      index: Math.max(0, modes.indexOf(this._ctl?.mode ?? "source")),
      onChange: (_v: string, index: number) => {
        if (!this._ctl) return;
        this._ctl.mode = modes[index] ?? "source";
        void this._renderCode(this._token).then(() => this._refresh());
      },
    });
    this._modeSelect.tooltip = modes.length > 1
      ? "Step by source line, or by SPIR-V instruction in the disassembly"
      : "This shader is stepped by source line";
    if (modes.length < 2 || (this._ctl && !this._ctl.program.hasSourceText())) {
      (this._modeSelect.element as HTMLSelectElement).disabled = true;
    }
    new Button(bar, { label: "Go to Command", class: "btn btn-sm", tooltip: "Select the command in the capture's tab", callback: () => this.host.selectCommand(this._request.command) });
    this._status = new Span(bar, { class: "text-muted shader-debugger-status" });
    this._notes = new Div(this.root, { class: "mesh-view-notes text-muted" });
    if (this._session) {
      const notes = [this._session.description[0].toUpperCase() + this._session.description.slice(1) + ".", ...this._session.notes];
      for (const n of notes) new Div(this._notes, { text: n });
    }

    const split = new Div(this.root, { class: "shader-debugger-split" });
    const left = new Div(split, { class: "shader-debugger-code" });
    left.element.style.flex = `0 0 ${this._split}%`;
    const handle = new Div(split, { class: "capture-texture-handle", tooltip: "Drag to give the code or the variables more room" });
    this._side = new Div(split, { class: "shader-debugger-side" });
    this._dragSplit(handle, split, left);
    this._fileBar = new Div(left, { class: "shader-toolbar shader-file-bar" });
    this._code = new Widget("pre", left, { class: "shader-text shader-debugger-text" });
    this._code.element.onclick = (e) => this._codeClick(e);
    this._code.element.onmousemove = (e) => this._hover(e);
    this._updateButtons();
  }

  private _buildPicker(bar: Div): void {
    const r = this._request;
    const inputs: HTMLInputElement[] = [];
    const field = (label: string, value: number | undefined, tooltip: string): void => {
      new Span(bar, { text: label, class: "text-muted" });
      const input = document.createElement("input");
      input.type = "number";
      input.min = "0";
      input.step = "1";
      input.className = "shader-debugger-number";
      input.value = value === undefined ? "" : String(value);
      input.title = tooltip;
      input.onkeydown = (e) => { if (e.key === "Enter") go(); };
      bar.element.appendChild(input);
      inputs.push(input);
    };
    const val = (i: number): number => Math.max(0, Math.floor(Number(inputs[i].value) || 0));
    const limits = this._session?.limits;
    const go = (): void => {
      if (r.stage === "vertex") this.show({ stage: "vertex", command: r.command, vertex: val(0), instance: val(1) });
      else if (r.stage === "fragment") this.show({ stage: "fragment", command: r.command, x: val(0), y: val(1) });
      else this.show({ stage: "compute", command: r.command, invocation: [val(0), val(1), val(2)] });
    };
    if (r.stage === "vertex") {
      field("Vertex", r.vertex, limits?.vertices ? `0 to ${limits.vertices - 1}: the draw's vertices in the order it read them` : "The vertex, in the order the draw read them");
      field("Instance", r.instance, limits?.instances ? `0 to ${limits.instances - 1}` : "The instance");
    } else if (r.stage === "fragment") {
      field("X", r.x, limits?.width ? `The pixel's column (the viewport is ${limits.width} wide)` : "The pixel's column");
      field("Y", r.y, limits?.height ? `The pixel's row (the viewport is ${limits.height} high)` : "The pixel's row");
    } else {
      const g = r.invocation ?? [0, 0, 0];
      const size = limits?.groups && limits.localSize ? limits.groups.map((n, i) => n * limits.localSize![i]) : null;
      ["X", "Y", "Z"].forEach((axis, i) => field(axis, g[i], size ? `gl_GlobalInvocationID.${axis.toLowerCase()}: 0 to ${size[i] - 1}` : `gl_GlobalInvocationID.${axis.toLowerCase()}`));
    }
    new Button(bar, { label: "Debug", class: "btn btn-sm", tooltip: "Debug this invocation instead", callback: go });
  }

  private _dragSplit(handle: Div, split: Div, left: Div): void {
    handle.element.onmousedown = (e: MouseEvent) => {
      e.preventDefault();
      const rect = split.element.getBoundingClientRect();
      const move = (m: MouseEvent): void => {
        this._split = Math.min(85, Math.max(15, ((m.clientX - rect.left) / Math.max(1, rect.width)) * 100));
        left.element.style.flex = `0 0 ${this._split}%`;
      };
      const up = (): void => {
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", up);
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", up);
    };
  }

  // ---------------------------------------------------------------------------------------
  // Code

  private async _renderCode(token: number): Promise<void> {
    const ctl = this._ctl;
    const code = this._code;
    if (!ctl || !code || !this._fileBar) return;
    const program = ctl.program;
    this._fileBar.html = "";
    let lines: CodeLine[];
    if (ctl.mode === "source") {
      const files = program.files;
      const current = ctl.location(ctl.invocation.current);
      if (current && files[current.file]?.text != null) this._file = current.file;
      if (this._file < 0 || files[this._file]?.text == null) {
        this._file = program.mainFile >= 0 && files[program.mainFile]?.text != null ? program.mainFile : files.findIndex((f) => f.text != null);
      }
      const withText = files.map((f, i) => ({ f, i })).filter((x) => x.f.text != null);
      if (withText.length > 1) {
        for (const { f, i } of withText) {
          const b = new Button(this._fileBar, { label: f.name, class: `btn btn-sm${i === this._file ? " active" : ""}`, callback: () => { this._file = i; void this._renderCode(this._token).then(() => this._refresh(false)); } });
          b.element.dataset.file = String(i);
        }
      }
      lines = this._sourceLines(program, this._file);
    } else {
      const disassembly = program.disassembly;
      if (!disassembly) return;
      if (this._disassembly === null) {
        this._setStatus("Disassembling...");
        const r = await this.host.disassemble(disassembly.bytes()).catch((e: unknown) => ({ ok: false, text: String(e) }));
        if (token !== this._token) return;
        this._disassembly = r.ok ? r.text : "";
      }
      lines = this._disassemblyLines(program, this._disassembly);
    }
    const stoppable = program.stopKeys(ctl.mode);
    let html = "";
    const width = Math.max(1, ...lines.map((l) => l.number.length));
    for (const l of lines) {
      const cls = `code-line${l.key && stoppable.has(l.key) ? " dbg-stoppable" : ""}${l.key && this._breakpoints.has(l.key) ? " dbg-breakpoint" : ""}`;
      html += `<span class="${cls}"${l.key ? ` data-key="${l.key}"` : ""}><span class="dbg-gutter"></span><span class="code-lineno">${l.number.padStart(width)}</span>${l.html}</span>\n`;
    }
    code.html = html;
  }

  private _sourceLines(program: DebugProgram, fileIndex: number): CodeLine[] {
    const file = program.files[fileIndex];
    if (!file || file.text == null) return [{ html: "No source text.", number: "", key: null }];
    const map = sourceLineMap(file.text);
    const language = program.language;
    const html = language ? highlightLines(file.text, language) : file.text.split("\n").map(escapeHtml);
    return map.lines.map((_, i) => ({
      html: html[i] ?? "",
      number: map.lineOf[i] ? String(map.lineOf[i]) : "",
      key: map.lineOf[i] ? sourceKey(fileIndex, map.lineOf[i]) : null,
    }));
  }

  private _disassemblyLines(program: DebugProgram, text: string): CodeLine[] {
    const disassembly = program.disassembly;
    if (!disassembly) return [{ html: "No disassembly.", number: "", key: null }];
    if (!text) {
      // No spirv-dis: the program's own listing of the instructions that execute.
      return disassembly.listing().map((l) => ({ html: escapeHtml(l.text), number: l.number, key: l.key }));
    }
    const textLines = text.split("\n");
    const html = highlightLines(text, "spirv-asm");
    const { keys, numbers } = disassembly.mapDisassembly(textLines);
    return textLines.map((_, i) => ({ html: html[i] ?? "", number: numbers[i], key: keys[i] }));
  }

  private _codeClick(e: MouseEvent): void {
    const ctl = this._ctl;
    const line = (e.target as HTMLElement).closest(".code-line") as HTMLElement | null;
    if (!ctl || !line?.dataset.key || !line.classList.contains("dbg-stoppable")) return;
    // Breakpoints toggle from the gutter and the line numbers, so text stays selectable.
    const onGutter = (e.target as HTMLElement).closest(".dbg-gutter, .code-lineno");
    if (!onGutter) return;
    const key = line.dataset.key;
    const on = ctl.toggleBreakpoint(key);
    if (on) this._breakpoints.add(key); else this._breakpoints.delete(key);
    line.classList.toggle("dbg-breakpoint", on);
  }

  /** A name under the mouse shows its value in the frame selected. */
  private _hover(e: MouseEvent): void {
    const ctl = this._ctl;
    const code = this._code;
    if (!ctl || !code) return;
    const word = wordAt(e.clientX, e.clientY);
    let title = "";
    if (word) {
      const inv = ctl.invocation;
      const program = ctl.program;
      const ids = word.startsWith("%") && /^%\d+$/.test(word) ? [Number(word.slice(1))] : program.idsNamed(word.replace(/^%/, ""));
      const depth = Math.min(this._frame, Math.max(0, inv.depth - 1));
      // Locals of the frame first, then anything else with the name.
      const ordered = [...ids.filter((id) => inv.frameOwns(depth, id)), ...ids];
      for (const id of ordered) {
        const value = inv.valueOf(id, depth);
        if (value === undefined) continue;
        const type = program.typeOfId(id);
        title = `${program.nameOf(id)}: ${program.typeName(type)} = ${program.valueText(type, value, 32)}`;
        break;
      }
    }
    if (code.element.title !== title) code.element.title = title;
  }

  // ---------------------------------------------------------------------------------------
  // Stepping

  private _step(kind: StepKind): void {
    const ctl = this._ctl;
    if (!ctl || ctl.finished || this._timer) return;
    this._frame = 0;
    this._pause = false;
    ctl.begin(kind);
    const tick = (): void => {
      this._timer = 0;
      if (this._ctl !== ctl) return;
      const done = ctl.proceed(STEP_BUDGET);
      if (!done && this._pause) ctl.cancel();
      if (done || this._pause) {
        this._updateButtons();
        this._refresh();
        return;
      }
      this._setStatus(`Running... ${ctl.stepSteps.toLocaleString()} instructions`);
      this._timer = window.setTimeout(tick, 0);
      this._updateButtons();
    };
    tick();
  }

  private _pauseRun(): void {
    this._pause = true;
  }

  private _restart(): void {
    if (!this._ctl) return;
    this._stopTimer();
    this._frame = 0;
    this._ctl.restart();
    this._refresh();
  }

  private _stopTimer(): void {
    if (this._timer) clearTimeout(this._timer);
    this._timer = 0;
    this._ctl?.cancel();
  }

  private _key(e: KeyboardEvent): void {
    // Only while the tab is the one shown.
    if (!this._ctl || !this.root.element.isConnected || this.root.element.offsetParent === null) return;
    if ((e.target as HTMLElement | null)?.tagName === "INPUT") return;
    let handled = true;
    if (e.key === "F5" && e.ctrlKey && e.shiftKey) this._restart();
    else if (e.key === "F5") this._timer ? this._pauseRun() : this._step("continue");
    else if (e.key === "F10") this._step("over");
    else if (e.key === "F11" && e.shiftKey) this._step("out");
    else if (e.key === "F11") this._step("into");
    else handled = false;
    if (handled) e.preventDefault();
  }

  private _updateButtons(): void {
    const ctl = this._ctl;
    const finished = !ctl || ctl.finished;
    const running = this._timer !== 0;
    for (const name of ["over", "into", "out"]) {
      const b = this._buttons[name];
      if (b) b.element.disabled = finished || running;
    }
    const c = this._buttons.continue;
    if (c) {
      c.element.disabled = finished && !running;
      // Only on a change: replacing the icon under the mouse would drop its hover and tooltip.
      if (c.element.dataset.running !== String(running)) {
        c.element.dataset.running = String(running);
        c.html = running ? ICON_PAUSE : ICON_CONTINUE;
        c.tooltip = running ? TIP_PAUSE : TIP_CONTINUE;
      }
    }
    const r = this._buttons.restart;
    if (r) r.element.disabled = !ctl || running;
  }

  // ---------------------------------------------------------------------------------------
  // State

  private _setStatus(text: string): void {
    if (this._status) this._status.text = text;
  }

  /** Shows where the invocation is: the status, the current line, and the variables. */
  private _refresh(scroll = true): void {
    const ctl = this._ctl;
    if (!ctl) return;
    const inv = ctl.invocation;
    const program = ctl.program;
    this._updateButtons();

    // The file of the line stopped at.
    const current = ctl.mode === "source" ? ctl.location(inv.current) : null;
    if (scroll && current && current.file !== this._file && program.files[current.file]?.text != null) {
      this._file = current.file;
      void this._renderCode(this._token).then(() => this._refresh());
      return;
    }

    const where = (): string => {
      if (ctl.mode === "source") {
        const loc = ctl.location(inv.current);
        return loc ? `line ${loc.line}${program.files.length > 1 ? ` of ${program.files[loc.file]?.name}` : ""}` : "an instruction without a line";
      }
      return `instruction ${inv.current?.index ?? "?"}`;
    };
    const steps = `${inv.steps.toLocaleString()} instruction${inv.steps === 1 ? "" : "s"}`;
    const warn = inv.warnings.size ? `; ${inv.warnings.size} warning${inv.warnings.size === 1 ? "" : "s"} below` : "";
    switch (inv.status) {
      case "returned": this._setStatus(`Returned after ${steps}${warn}`); break;
      case "discarded": this._setStatus(`Discarded after ${steps}: the fragment writes nothing${warn}`); break;
      case "error": this._setStatus(`Stopped: ${inv.error}${warn}`); break;
      default: this._setStatus(`Paused at ${where()} in ${inv.callStack()[0]?.name ?? ""}, after ${steps}${warn}`);
    }

    const code = this._code?.element;
    if (code) {
      for (const el of Array.from(code.querySelectorAll(".dbg-current, .dbg-frame"))) el.classList.remove("dbg-current", "dbg-frame");
      const key = inv.finished ? null : ctl.currentKey;
      const el = key ? code.querySelector(`[data-key="${key}"]`) : null;
      el?.classList.add("dbg-current");
      if (this._frame > 0) {
        const frame = inv.callStack()[this._frame];
        const fk = frame ? ctl.keyOf(frame.step) : null;
        const fel = fk ? code.querySelector(`[data-key="${fk}"]`) : null;
        fel?.classList.add("dbg-frame");
        if (scroll) scrollIntoViewIfNeeded(fel as HTMLElement | null, this._code!.element);
      } else if (scroll) {
        scrollIntoViewIfNeeded(el as HTMLElement | null, this._code!.element);
      }
    }
    this._renderSide();
  }

  private _renderSide(): void {
    const side = this._side;
    const ctl = this._ctl;
    const session = this._session;
    if (!side || !ctl || !session) return;
    const scrollTop = side.element.scrollTop;
    side.html = "";
    const inv = ctl.invocation;
    const program = ctl.program;

    if (inv.finished) this._renderResult(this._section(side, "Result"), session);
    if (inv.finished && session.original) this._renderOriginal(this._section(side, "Original SPIR-V"), session);

    if (inv.warnings.size) {
      const body = this._section(side, `Warnings (${inv.warnings.size})`);
      for (const w of inv.warnings) new Div(body, { text: w, class: "shader-debugger-warning" });
    }

    const last = ctl.lastLine;
    const lastLoc = last.results.length ? ctl.location(last.results[last.results.length - 1].inst) : null;
    const lineLabel = ctl.mode === "source" && lastLoc ? `line ${lastLoc.line}` : last.results.length ? `instruction ${last.results[last.results.length - 1].inst.index}` : "";
    const values = this._section(side, `Values computed${lineLabel ? ` on ${lineLabel}` : ""}`);
    if (!last.results.length) new Div(values, { text: "Nothing yet: step to see the values each line computes.", class: "text-muted" });
    const table = variableTable(values);
    for (const r of last.results.slice(-MAX_ROWS)) {
      addVariable(table, program, program.nameOf(r.id), program.resultType(r), r.value, program.resultTemporary(r));
    }

    const stack = inv.callStack();
    const depth = Math.min(this._frame, Math.max(0, stack.length - 1));
    if (!inv.finished || stack.length) {
      const locals = this._section(side, `Locals${stack[depth] ? ` of ${stack[depth].name}` : ""}`);
      this._variables(locals, program, inv.locals(depth), "No local variables.");

      const body = this._section(side, "Call Stack");
      stack.forEach((f, d) => {
        const loc = ctl.location(f.step);
        const row = new Div(body, { class: `shader-debugger-frame${d === depth ? " selected" : ""}`, text: `${f.name}${loc ? `  line ${loc.line}` : f.step ? `  instruction ${f.step.index}` : ""}` });
        row.element.onclick = () => { this._frame = d; this._refresh(); };
      });
    }

    this._variables(this._section(side, "Inputs"), program, inv.inputVariables(), "No inputs.");
    this._variables(this._section(side, "Outputs"), program, inv.outputs(), "No outputs.");
    this._variables(this._section(side, "Globals"), program, inv.privateVariables(), "No private variables.");
    this._variables(this._section(side, "Resources"), program, inv.resourceVariables(), "No resources.");
    side.element.scrollTop = scrollTop;
  }

  private _renderResult(body: Div, session: DebugSession): void {
    const inv = this._ctl!.invocation;
    const program = session.program;
    if (inv.status === "discarded") new Div(body, { text: "The fragment was discarded.", class: "text-muted" });
    if (inv.status === "error") new Div(body, { text: inv.error, class: "shader-debugger-warning" });
    if (inv.status !== "returned") return;
    const outs = inv.outputs();
    if (session.target.stage === "fragment") {
      const color = outs.find((o) => o.location === 0);
      if (color) new Div(body, { text: `Output location 0: ${program.valueText(color.type, color.value)}` });
      const t = session.targetPixel;
      if (t) {
        new Div(body, { text: `The render target after the pass: (${t.value.map((v) => +v.toFixed(4)).join(", ")}) ${t.format.replace(/^VK_FORMAT_/, "")}` });
        new Div(body, { text: "Blending and the pass's later draws come between the two; the pixel history has the value after this draw.", class: "text-muted font-sm" });
      }
      return;
    }
    const replayed = session.replayedOutputs;
    if (!replayed) {
      if (session.target.stage === "vertex") new Div(body, { text: "The replay's outputs of this vertex were not available to compare with.", class: "text-muted font-sm" });
      return;
    }
    const table = variableTable(body);
    for (const r of replayed) {
      const mine = r.builtin === "Position" ? positionOf(outs) : outs.find((o) => o.location === r.location)?.value;
      const values = scalars(mine ?? undefined);
      const diff = values.length ? Math.max(...r.value.map((x, i) => Math.abs(x - (values[i] ?? NaN)) / Math.max(1, Math.abs(x)))) : NaN;
      const same = diff < 1e-4;
      const row = table.insertRow();
      row.insertCell().textContent = r.name;
      row.insertCell().textContent = values.length ? `(${values.map((v) => +v.toPrecision(6)).join(", ")})` : "—";
      const cell = row.insertCell();
      cell.textContent = same ? "matches the GPU" : `GPU (${r.value.map((v) => +v.toPrecision(6)).join(", ")})`;
      cell.className = same ? "text-muted" : "shader-debugger-warning";
      if (!same) cell.title = "The replay's transform feedback differs: a driver may reorder floating-point operations the interpreter keeps in order.";
    }
  }

  /** A translation's results beside the original module's, which runs (once per invocation) when first needed. */
  private _renderOriginal(body: Div, session: DebugSession): void {
    const run = this._runOriginal(session);
    if (run.error) {
      new Div(body, { text: `The original could not be run to compare with: ${run.error}`, class: "shader-debugger-warning" });
      return;
    }
    if (!run.done) {
      new Div(body, { text: "Running the original to compare with...", class: "text-muted" });
      return;
    }
    const c = this._comparison();
    if (!c) return;
    if (c.status.translated !== c.status.original) {
      new Div(body, { text: `The translation ${c.status.translated}, but the original ${c.status.original}${c.status.error ? ` (${c.status.error})` : ""}: step the original SPIR-V instead.`, class: "shader-debugger-warning" });
      return;
    }
    new Div(body, {
      text: c.matches ? `The original ${c.status.original} with the same results.` : "The original computes different results: step the original SPIR-V instead.",
      class: c.matches ? "text-muted" : "shader-debugger-warning",
    });
    if (!c.values.length) return;
    const table = variableTable(body);
    const number = (x: number | undefined): string => (x === undefined ? "—" : String(+x.toPrecision(6)));
    const text = (v: number[] | null): string => (v ? `(${v.slice(0, 16).map(number).join(", ")}${v.length > 16 ? ", ..." : ""})` : "—");
    for (const v of c.values) {
      const row = table.insertRow();
      row.insertCell().textContent = v.label;
      row.insertCell().textContent = text(v.original);
      const cell = row.insertCell();
      cell.className = v.matches ? "text-muted" : "shader-debugger-warning";
      if (v.matches) {
        cell.textContent = "matches";
      } else if (!v.translated || !v.original) {
        cell.textContent = v.translated ? "only in the translation" : "missing from the translation";
      } else if (v.original.length > 16 || v.translated.length > 16) {
        // A buffer: where it first differs, which may be past what the row shows.
        const at = v.original.findIndex((x, i) => !sameValue(x, v.translated![i]));
        const i = at < 0 ? Math.min(v.original.length, v.translated.length) : at;
        cell.textContent = `scalar ${i}: translation ${number(v.translated[i])}, original ${number(v.original[i])}`;
      } else {
        cell.textContent = `translation ${text(v.translated)}`;
      }
    }
  }

  /** Starts (or returns) the original module's run of the session's invocation, a slice at a time. */
  private _runOriginal(session: DebugSession): OriginalRun {
    if (this._original) return this._original;
    const run: OriginalRun = { stepper: null, done: false, error: "" };
    this._original = run;
    try {
      run.stepper = session.original!();
    } catch (e) {
      run.error = e instanceof Error ? e.message : String(e);
      run.done = true;
      return run;
    }
    const stepper = run.stepper;
    const token = this._token;
    const tick = (): void => {
      if (token !== this._token || this._original !== run) return;
      const inv = stepper.invocation;
      for (let n = 0; n < STEP_BUDGET && !inv.finished; n++) stepper.step();
      inv.takeResults();
      if (inv.finished) {
        run.done = true;
        this._renderSide();
      } else {
        window.setTimeout(tick, 0);
      }
    };
    window.setTimeout(tick, 0);
    return run;
  }

  /** How the finished translation compares with the finished original, when both are. */
  private _comparison(): ReturnType<typeof compareWithOriginal> | null {
    const inv = this._ctl?.invocation;
    const original = this._original?.stepper?.invocation;
    if (!inv?.finished || !original?.finished || !this._session) return null;
    return compareWithOriginal(inv, original, this._session.target.stage);
  }

  private _section(parent: Div, label: string): Div {
    const key = label.replace(/ \(.*$| on .*$| of .*$/, "");
    const section = new Div(parent, { class: "shader-debugger-section" });
    const head = new Div(section, { class: "shader-debugger-section-head", text: label });
    const body = new Div(section, { class: "shader-debugger-section-body" });
    const collapsed = this._collapsed.get(key) ?? false;
    body.element.hidden = collapsed;
    head.element.classList.toggle("collapsed", collapsed);
    head.element.onclick = () => {
      const now = !body.element.hidden;
      body.element.hidden = now;
      head.element.classList.toggle("collapsed", now);
      this._collapsed.set(key, now);
    };
    return body;
  }

  private _variables(body: Div, program: DebugProgram, vars: VariableView[], empty: string): void {
    if (!vars.length) {
      new Div(body, { text: empty, class: "text-muted" });
      return;
    }
    const table = variableTable(body);
    for (const v of vars) addVariable(table, program, v.name, v.type, v.value, false, program.variableWhere(v));
  }
}

// ---------------------------------------------------------------------------------------------

function requestOf(t: DebugTarget): DebugRequest {
  return t.stage === "vertex" ? { stage: "vertex", command: t.command, vertex: t.vertex, instance: t.instance }
    : t.stage === "fragment" ? { stage: "fragment", command: t.command, x: t.x, y: t.y }
    : { stage: "compute", command: t.command, invocation: t.invocation };
}

/**
 * The vertex and instance a VS Out record was: transform feedback writes strips and fans as
 * lists, triangle strip i as (v[i], v[i+1+i%2], v[i+2-i%2]) and fan i as (v[i+1], v[i+2], v[0]).
 */
export function recordVertex(record: number, vertices: number, topology: string): { vertex: number; instance: number } {
  const strip = /STRIP/.test(topology), fan = /FAN/.test(topology);
  const per = /LINE_STRIP/.test(topology) ? 2 : 3;
  const perInstance = strip || fan ? Math.max(1, (vertices - (per - 1)) * per) : vertices;
  const instance = Math.floor(record / perInstance);
  const r = record % perInstance;
  if (!strip && !fan) return { vertex: r, instance };
  const i = Math.floor(r / per), k = r % per;
  if (per === 2) return { vertex: i + k, instance };
  const order = fan ? [i + 1, i + 2, 0] : [i, i + 1 + (i % 2), i + 2 - (i % 2)];
  return { vertex: order[k], instance };
}

function stageName(stage: DebugRequest["stage"]): string {
  return stage === "vertex" ? "Vertex" : stage === "fragment" ? "Fragment" : "Compute";
}

function positionOf(outs: VariableView[]): Value | undefined {
  const direct = outs.find((o) => o.builtin === 0);
  if (direct) return direct.value;
  // gl_PerVertex: a block whose first member is the position.
  const block = outs.find((o) => Array.isArray(o.value) && Array.isArray(o.value[0]));
  return block && Array.isArray(block.value) ? block.value[0] : undefined;
}

/** The identifier (or %id) under a point of the page. */
function wordAt(x: number, y: number): string {
  const range = document.caretRangeFromPoint?.(x, y);
  const node = range?.startContainer;
  if (!range || !node || node.nodeType !== Node.TEXT_NODE) return "";
  const text = node.textContent ?? "";
  const isWord = (c: string): boolean => /[A-Za-z0-9_%]/.test(c);
  let a = range.startOffset, b = range.startOffset;
  if (!isWord(text[a] ?? "") && !isWord(text[a - 1] ?? "")) return "";
  while (a > 0 && isWord(text[a - 1])) a--;
  while (b < text.length && isWord(text[b])) b++;
  const word = text.slice(a, b);
  return /^%?[A-Za-z_]\w*$|^%\d+$/.test(word) ? word : "";
}

function scrollIntoViewIfNeeded(el: HTMLElement | null, container: HTMLElement): void {
  if (!el) return;
  const scroller = container.parentElement ?? container;
  const r = el.getBoundingClientRect();
  const c = scroller.getBoundingClientRect();
  if (r.top < c.top + 8 || r.bottom > c.bottom - 8) el.scrollIntoView({ block: "center" });
}

function variableTable(parent: Div): HTMLTableElement {
  const table = document.createElement("table");
  table.className = "shader-debugger-vars";
  parent.element.appendChild(table);
  return table;
}

/** A variable's row: composites open to their elements (built when opened). */
function addVariable(table: HTMLTableElement, program: DebugProgram, name: string, type: number, value: Value, temporary: boolean, where = "", indent = 0): void {
  const row = table.insertRow();
  row.dataset.indent = String(indent);
  const nameCell = row.insertCell();
  nameCell.className = `dbg-var-name${temporary ? " text-muted" : ""}`;
  nameCell.style.paddingLeft = `${4 + indent * 14}px`;
  const typeCell = row.insertCell();
  typeCell.className = "text-muted dbg-var-type";
  typeCell.textContent = program.typeName(type);
  if (where) typeCell.title = where;
  const valueCell = row.insertCell();
  valueCell.className = `dbg-var-value${nonFinite(value) ? " shader-debugger-warning" : ""}`;
  valueCell.textContent = program.valueText(type, value, 8);
  if (nonFinite(value)) valueCell.title = "NaN or infinity";

  const children = program.children(type, value);
  if (!children) {
    nameCell.textContent = name;
    return;
  }
  const toggle = document.createElement("span");
  toggle.className = "dbg-var-toggle";
  toggle.textContent = "▸ ";
  nameCell.append(toggle, document.createTextNode(name));
  let open = false;
  nameCell.style.cursor = "pointer";
  nameCell.onclick = () => {
    open = !open;
    toggle.textContent = open ? "▾ " : "▸ ";
    if (!open) {
      // The element rows below, and theirs.
      let next = row.nextElementSibling as HTMLTableRowElement | null;
      while (next && Number(next.dataset.indent) > indent) {
        const after = next.nextElementSibling as HTMLTableRowElement | null;
        next.remove();
        next = after;
      }
      return;
    }
    // Rows go in after this one: build them in a scratch table, then move them.
    const scratch = document.createElement("table");
    for (const child of children.slice(0, MAX_ROWS)) {
      addVariable(scratch, program, child.name, child.type, child.value, false, "", indent + 1);
    }
    let after: Element = row;
    for (const r of Array.from(scratch.rows)) {
      after.after(r);
      after = r;
    }
  };
}
