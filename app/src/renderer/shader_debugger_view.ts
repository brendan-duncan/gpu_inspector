// The shader debugger in a tab of its own, after RenderDoc's shader viewer and WebGPU Inspector's
// shader debugger: one invocation of a draw's vertex or fragment shader, or of a dispatch's compute
// shader, run in the SPIR-V interpreter (spirv/) on the capture's inputs (shader_debug_setup.ts),
// stepped by source line (shader_debugger.ts) with breakpoints, the values each line computed,
// the locals, the call stack, the inputs, outputs and resources, and at the end how the outputs
// compare with what the GPU produced (the replay's vertex outputs, the render target's pixel).
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
import { DebugController, executableInstructions, hasLineInfo, instructionKey, nonFinite, resultType, scalars, sourceKey, valueText, valueType, type StepKind } from "./shader_debugger.js";
import { coveredPixel, prepareDebugSession, type DebugContext, type DebugSession, type DebugTarget } from "./shader_debug_setup.js";
import { resolveSourcesFromHost } from "./shader_source_view.js";
import type { SessionContext } from "./session_panel.js";
import { StorageClass, type SpirvModule } from "./spirv/module.js";
import type { VariableView } from "./spirv/interpreter.js";
import type { Value } from "./spirv/values.js";
import { disassemblyInstructions, sourceLanguageOf, sourceLineMap } from "./vulkan/spirv_debug.js";
import type { ObjectLookup } from "./vulkan/vulkan_object.js";
import type { CaptureCommand, ShaderTextResult } from "../shared/protocol.js";

/** What to debug; the parts left out are chosen (a pixel the draw covers, the first vertex or invocation). */
export type DebugRequest =
  | { stage: "vertex"; command: number; vertex?: number; instance?: number; /** A VS Out row: the vertex and instance it was. */ record?: number }
  | { stage: "fragment"; command: number; x?: number; y?: number }
  | { stage: "compute"; command: number; invocation?: [number, number, number] };

export interface ShaderDebuggerOptions {
  /** Testing aid: steps over this many lines once open (-1 runs to the end). */
  steps?: number;
}

/** What the view needs of the capture tab it belongs to. */
export interface ShaderDebuggerHost {
  readonly data: CaptureData;
  readonly db: ObjectLookup & { blobData: Map<string, Uint8Array> };
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
}

/** Instructions run between UI updates while a step runs. */
const STEP_BUDGET = 50_000;
const MAX_ROWS = 200;
const STAGE_LABEL = { vertex: "Vertex", fragment: "Pixel", compute: "Compute" } as const;

interface CodeLine {
  html: string;
  number: string;
  key: string | null;
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
    if (!sameShader) this._breakpoints.clear();
    this._request = request;
    this._options = options;
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
    const m = this._session?.module;
    return {
      request: this._request, preparing: this._preparing, error: this._error || null,
      description: this._session?.description ?? null, notes: this._session?.notes ?? [],
      mode: ctl?.mode ?? null, status: inv?.status ?? null, invocationError: inv?.error || null, steps: inv?.steps ?? 0,
      line: loc?.line ?? null, instruction: inv?.current?.index ?? null, depth: inv?.frames.length ?? 0, running: ctl?.running ?? false,
      breakpoints: [...this._breakpoints],
      outputs: inv && m ? inv.outputs().map((o) => ({ name: o.name, location: o.location, builtin: o.builtin, value: scalars(o.value) })) : [],
      targetPixel: this._session?.targetPixel ?? null,
      replayedOutputs: this._session?.replayedOutputs ?? null,
      lineValues: ctl?.lastLine.results.length ?? 0,
      codeLines: this._code?.element.querySelectorAll(".code-line").length ?? 0,
      warnings: inv ? [...inv.warnings] : [],
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
      if (this.host.session && session.module.debug) await resolveSourcesFromHost(session.module.debug, this.host.session).catch(() => false);
      if (token !== this._token) return;
      this._session = session;
      const ctl = new DebugController(session, hasSourceText(session.module) ? "source" : "instruction");
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
    const steps = this._options.steps;
    if (steps !== undefined) {
      if (steps < 0) this._ctl.advance("continue");
      else for (let i = 0; i < steps && !this._ctl.finished; i++) this._ctl.advance("over");
      this._refresh();
    }
  }

  private _context(): DebugContext {
    return { data: this.host.data, db: this.host.db, meshOutput: (command) => this.host.meshOutput(command), inputNames: this._inputNames };
  }

  /** The request with its choices made: a pixel the draw covers, a VS Out row's vertex and instance. */
  private async _resolve(token: number): Promise<DebugTarget | null> {
    const r = this._request;
    const cmd = this.host.data.commands[r.command];
    if (!cmd) throw new Error(`the capture has no command ${r.command}`);
    if (r.stage === "compute") return { stage: "compute", command: r.command, invocation: r.invocation ?? [0, 0, 0] };
    if (r.stage === "vertex") {
      if (r.record === undefined) return { stage: "vertex", command: r.command, vertex: r.vertex ?? 0, instance: r.instance ?? 0 };
      const first = await prepareDebugSession(this._context(), { stage: "vertex", command: r.command, vertex: 0, instance: 0 });
      const n = Math.max(1, first.limits.vertices ?? 1);
      const topology = await this.host.meshOutput(r.command).then((m) => m.topology, () => "");
      const { vertex, instance } = recordVertex(r.record, n, topology);
      return { stage: "vertex", command: r.command, vertex, instance };
    }
    if (r.x !== undefined && r.y !== undefined) return { stage: "fragment", command: r.command, x: r.x, y: r.y };
    this._setStatus("Replaying the capture for the draw's vertex shader outputs, to find a pixel it covers...");
    const mesh = await this.host.meshOutput(r.command);
    if (token !== this._token) return null;
    if (!mesh.measured) throw new Error(`the draw's vertex shader outputs could not be captured: ${mesh.note ?? "the replay did not reach the draw"}`);
    const pixel = coveredPixel(drawState(this.host.data, this.host.db, cmd), mesh);
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
    const button = (name: string, label: string, tooltip: string, callback: () => void): void => {
      this._buttons[name] = new Button(bar, { label, class: "btn btn-sm", tooltip, callback });
    };
    button("continue", "Continue", "Run to the next breakpoint or the end (F5); pauses a run", () => (this._timer ? this._pauseRun() : this._step("continue")));
    button("over", "Step Over", "Run to the next line, over function calls (F10)", () => this._step("over"));
    button("into", "Step Into", "Run to the next line, into function calls (F11)", () => this._step("into"));
    button("out", "Step Out", "Run until the function returns (Shift+F11)", () => this._step("out"));
    button("restart", "Restart", "Start the invocation again, keeping the breakpoints (Ctrl+Shift+F5)", () => this._restart());
    sep();
    const modes = ["Source", "Disassembly"];
    this._modeSelect = new Select(bar, {
      options: modes,
      index: this._ctl?.mode === "source" ? 0 : 1,
      onChange: (_v: string, index: number) => {
        if (!this._ctl) return;
        this._ctl.mode = index === 0 ? "source" : "instruction";
        void this._renderCode(this._token).then(() => this._refresh());
      },
    });
    this._modeSelect.tooltip = "Step by source line, or by SPIR-V instruction in the disassembly";
    if (this._ctl && !hasSourceText(this._ctl.module)) (this._modeSelect.element as HTMLSelectElement).disabled = true;
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
    const m = ctl.module;
    this._fileBar.html = "";
    let lines: CodeLine[];
    if (ctl.mode === "source") {
      const info = m.debug!;
      const current = ctl.location(ctl.invocation.current);
      if (current && info.files[current.file]?.text != null) this._file = current.file;
      if (this._file < 0 || info.files[this._file]?.text == null) {
        this._file = info.mainFile >= 0 && info.files[info.mainFile]?.text != null ? info.mainFile : info.files.findIndex((f) => f.text != null);
      }
      const withText = info.files.map((f, i) => ({ f, i })).filter((x) => x.f.text != null);
      if (withText.length > 1) {
        for (const { f, i } of withText) {
          const b = new Button(this._fileBar, { label: f.name, class: `btn btn-sm${i === this._file ? " active" : ""}`, callback: () => { this._file = i; void this._renderCode(this._token).then(() => this._refresh(false)); } });
          b.element.dataset.file = String(i);
        }
      }
      lines = this._sourceLines(m, this._file);
    } else {
      if (this._disassembly === null) {
        const bytes = new Uint8Array(m.words.buffer, m.words.byteOffset, m.words.byteLength);
        this._setStatus("Disassembling...");
        const r = await this.host.disassemble(bytes).catch((e: unknown) => ({ ok: false, text: String(e) }));
        if (token !== this._token) return;
        this._disassembly = r.ok ? r.text : "";
      }
      lines = this._disassemblyLines(m, this._disassembly);
    }
    const stoppable = this._stoppableKeys();
    let html = "";
    const width = Math.max(1, ...lines.map((l) => l.number.length));
    for (const l of lines) {
      const cls = `code-line${l.key && stoppable.has(l.key) ? " dbg-stoppable" : ""}${l.key && this._breakpoints.has(l.key) ? " dbg-breakpoint" : ""}`;
      html += `<span class="${cls}"${l.key ? ` data-key="${l.key}"` : ""}><span class="dbg-gutter"></span><span class="code-lineno">${l.number.padStart(width)}</span>${l.html}</span>\n`;
    }
    code.html = html;
  }

  private _sourceLines(m: SpirvModule, fileIndex: number): CodeLine[] {
    const file = m.debug?.files[fileIndex];
    if (!file || file.text == null) return [{ html: "No source text.", number: "", key: null }];
    const map = sourceLineMap(file.text);
    const language = sourceLanguageOf(m.debug);
    const html = language ? highlightLines(file.text, language) : file.text.split("\n").map(escapeHtml);
    return map.lines.map((_, i) => ({
      html: html[i] ?? "",
      number: map.lineOf[i] ? String(map.lineOf[i]) : "",
      key: map.lineOf[i] ? sourceKey(fileIndex, map.lineOf[i]) : null,
    }));
  }

  private _disassemblyLines(m: SpirvModule, text: string): CodeLine[] {
    if (!text) {
      // No spirv-dis: a listing of the executable instructions.
      return executableInstructions(m).map((i) => {
        const inst = m.instructions[i];
        return { html: escapeHtml(`${inst.result ? `${m.nameOf(inst.result)} = ` : ""}Op${inst.op}`), number: String(i), key: instructionKey(i) };
      });
    }
    const textLines = text.split("\n");
    const html = highlightLines(text, "spirv-asm");
    const keys = new Array<string | null>(textLines.length).fill(null);
    const numbers = new Array<string>(textLines.length).fill("");
    const instructions = disassemblyInstructions(textLines);
    if (instructions.length === m.instructions.length) {
      instructions.forEach((ls, k) => {
        keys[ls[0]] = instructionKey(k);
        numbers[ls[0]] = String(k);
      });
    }
    return textLines.map((_, i) => ({ html: html[i] ?? "", number: numbers[i], key: keys[i] }));
  }

  private _stoppableKeys(): Set<string> {
    const ctl = this._ctl!;
    const keys = new Set<string>();
    for (const i of executableInstructions(ctl.module)) {
      const k = ctl.keyOf(ctl.module.instructions[i]);
      if (k) keys.add(k);
    }
    return keys;
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
      const m = ctl.module;
      const ids = word.startsWith("%") && /^%\d+$/.test(word) ? [Number(word.slice(1))] : idsNamed(m, word.replace(/^%/, ""));
      const depth = Math.min(this._frame, Math.max(0, inv.frames.length - 1));
      const frame = inv.frames[inv.frames.length - 1 - depth];
      // Locals of the frame first, then anything else with the name.
      const ordered = frame ? [...ids.filter((id) => frame.values.has(id) || frame.locals.some((l) => l.id === id)), ...ids] : ids;
      for (const id of ordered) {
        const value = inv.valueOf(id, depth);
        if (value === undefined) continue;
        const type = valueType(m, id);
        title = `${m.nameOf(id)}: ${m.typeName(type)} = ${valueText(m, type, value, 32)}`;
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
      c.text = running ? "Pause" : "Continue";
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
    const m = ctl.module;
    this._updateButtons();

    // The file of the line stopped at.
    const current = ctl.mode === "source" ? ctl.location(inv.current) : null;
    if (scroll && current && current.file !== this._file && m.debug?.files[current.file]?.text != null) {
      this._file = current.file;
      void this._renderCode(this._token).then(() => this._refresh());
      return;
    }

    const where = (): string => {
      if (ctl.mode === "source") {
        const loc = ctl.location(inv.current);
        return loc ? `line ${loc.line}${(m.debug?.files.length ?? 0) > 1 ? ` of ${m.debug!.files[loc.file]?.name}` : ""}` : "an instruction without a line";
      }
      return `instruction ${inv.current?.index ?? "?"}`;
    };
    const steps = `${inv.steps.toLocaleString()} instruction${inv.steps === 1 ? "" : "s"}`;
    const warn = inv.warnings.size ? `; ${inv.warnings.size} warning${inv.warnings.size === 1 ? "" : "s"} below` : "";
    switch (inv.status) {
      case "returned": this._setStatus(`Returned after ${steps}${warn}`); break;
      case "discarded": this._setStatus(`Discarded after ${steps}: the fragment writes nothing${warn}`); break;
      case "error": this._setStatus(`Stopped: ${inv.error}${warn}`); break;
      default: this._setStatus(`Paused at ${where()} in ${m.nameOf(inv.frames[inv.frames.length - 1]?.fn.id ?? 0)}, after ${steps}${warn}`);
    }

    const code = this._code?.element;
    if (code) {
      for (const el of Array.from(code.querySelectorAll(".dbg-current, .dbg-frame"))) el.classList.remove("dbg-current", "dbg-frame");
      const key = inv.finished ? null : ctl.currentKey;
      const el = key ? code.querySelector(`[data-key="${key}"]`) : null;
      el?.classList.add("dbg-current");
      if (this._frame > 0) {
        const frame = inv.frames[inv.frames.length - 1 - this._frame];
        const fk = frame ? ctl.keyOf(m.instructions[frame.pc] ?? null) : null;
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
    const m = ctl.module;

    if (inv.finished) this._renderResult(this._section(side, "Result"), session);

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
      const type = resultType(m, r);
      addVariable(table, m, m.nameOf(r.id), type, r.value, !m.names.has(r.id) && !m.debugVariableNames.has(r.id));
    }

    const depth = Math.min(this._frame, Math.max(0, inv.frames.length - 1));
    if (!inv.finished || inv.frames.length) {
      const frame = inv.frames[inv.frames.length - 1 - depth];
      const locals = this._section(side, `Locals${frame ? ` of ${m.nameOf(frame.fn.id)}` : ""}`);
      this._variables(locals, m, inv.locals(depth), "No local variables.");

      const stack = this._section(side, "Call Stack");
      for (let d = 0; d < inv.frames.length; d++) {
        const f = inv.frames[inv.frames.length - 1 - d];
        const inst = d === 0 ? inv.current : m.instructions[f.pc] ?? null;
        const loc = ctl.location(inst);
        const row = new Div(stack, { class: `shader-debugger-frame${d === depth ? " selected" : ""}`, text: `${m.nameOf(f.fn.id)}${loc ? `  line ${loc.line}` : inst ? `  instruction ${inst.index}` : ""}` });
        row.element.onclick = () => { this._frame = d; this._refresh(); };
      }
    }

    this._variables(this._section(side, "Inputs"), m, inv.inputVariables(), "No inputs.");
    this._variables(this._section(side, "Outputs"), m, inv.outputs(), "No outputs.");
    this._variables(this._section(side, "Globals"), m, inv.privateVariables(), "No private variables.");
    this._variables(this._section(side, "Resources"), m, inv.resourceVariables(), "No resources.");
    side.element.scrollTop = scrollTop;
  }

  private _renderResult(body: Div, session: DebugSession): void {
    const inv = this._ctl!.invocation;
    const m = session.module;
    if (inv.status === "discarded") new Div(body, { text: "The fragment was discarded.", class: "text-muted" });
    if (inv.status === "error") new Div(body, { text: inv.error, class: "shader-debugger-warning" });
    if (inv.status !== "returned") return;
    const outs = inv.outputs();
    if (session.target.stage === "fragment") {
      const colour = outs.find((o) => o.location === 0);
      if (colour) new Div(body, { text: `Output location 0: ${valueText(m, colour.type, colour.value)}` });
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

  private _variables(body: Div, m: SpirvModule, vars: VariableView[], empty: string): void {
    if (!vars.length) {
      new Div(body, { text: empty, class: "text-muted" });
      return;
    }
    const table = variableTable(body);
    for (const v of vars) {
      const where = v.builtin !== undefined ? "" : v.location !== undefined ? `location ${v.location}` : v.binding !== undefined ? `set ${v.set ?? 0} binding ${v.binding}` : v.storage === StorageClass.PushConstant ? "push constants" : "";
      addVariable(table, m, v.name, v.type, v.value, false, where);
    }
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

/** Whether the module's lines have text to show (embedded, or found under the source roots). */
function hasSourceText(m: SpirvModule): boolean {
  return hasLineInfo(m) && !!m.debug?.files.some((f) => f.text != null);
}

function positionOf(outs: VariableView[]): Value | undefined {
  const direct = outs.find((o) => o.builtin === 0);
  if (direct) return direct.value;
  // gl_PerVertex: a block whose first member is the position.
  const block = outs.find((o) => Array.isArray(o.value) && Array.isArray(o.value[0]));
  return block && Array.isArray(block.value) ? block.value[0] : undefined;
}

const _namesByModule = new WeakMap<SpirvModule, Map<string, number[]>>();
function idsNamed(m: SpirvModule, name: string): number[] {
  let names = _namesByModule.get(m);
  if (!names) {
    names = new Map();
    for (const source of [m.names, m.debugVariableNames]) {
      for (const [id, n] of source) {
        if (m.types.has(id)) continue;
        const list = names.get(n) ?? [];
        list.push(id);
        names.set(n, list);
      }
    }
    _namesByModule.set(m, names);
  }
  return names.get(name) ?? [];
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
function addVariable(table: HTMLTableElement, m: SpirvModule, name: string, type: number, value: Value, temporary: boolean, where = "", indent = 0): void {
  const row = table.insertRow();
  row.dataset.indent = String(indent);
  const nameCell = row.insertCell();
  nameCell.className = `dbg-var-name${temporary ? " text-muted" : ""}`;
  nameCell.style.paddingLeft = `${4 + indent * 14}px`;
  const typeCell = row.insertCell();
  typeCell.className = "text-muted dbg-var-type";
  typeCell.textContent = m.typeName(type);
  if (where) typeCell.title = where;
  const valueCell = row.insertCell();
  valueCell.className = `dbg-var-value${nonFinite(value) ? " shader-debugger-warning" : ""}`;
  valueCell.textContent = valueText(m, type, value, 8);
  if (nonFinite(value)) valueCell.title = "NaN or infinity";

  const t = m.types.get(type);
  const children = Array.isArray(value) && (t?.kind === "struct" || t?.kind === "array" || t?.kind === "runtimeArray" || t?.kind === "matrix" || value.length > 8) ? value : null;
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
    children.slice(0, MAX_ROWS).forEach((child, i) => {
      const childType = t?.kind === "struct" ? t.members[i] : t?.kind === "matrix" ? t.column : t?.kind === "array" || t?.kind === "runtimeArray" ? t.element : t?.kind === "vector" ? t.element : 0;
      const childName = t?.kind === "struct" ? m.memberNames.get(type)?.get(i) ?? `[${i}]` : `[${i}]`;
      addVariable(scratch, m, childName, childType, child, false, "", indent + 1);
    });
    let after: Element = row;
    for (const r of Array.from(scratch.rows)) {
      after.after(r);
      after = r;
    }
  };
}
