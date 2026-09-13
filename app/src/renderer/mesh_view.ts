// A draw's mesh in a tab of its own, after RenderDoc's Mesh Viewer: VS In (the vertices the draw read,
// decoded from the captured buffers: mesh_input.ts) and VS Out (what its vertex shader wrote, from a
// replay of a Vulkan capture: mesh_output.ts), each as a turnable wireframe (mesh_preview.ts) over a
// table of its vertices. Clicking a row marks that vertex in the preview. The draw list steps through
// the draws of the pass, keeping the view so their meshes line up.
import { Button } from "./widget/button.js";
import { Div } from "./widget/div.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import type { CaptureData } from "./capture_data.js";
import { listPositions, meshInput, type MeshInput } from "./mesh_input.js";
import { clipPositions, clipStats, meshSummary, outputValues, primitiveKind, type MeshOutput } from "./mesh_output.js";
import { MeshPreview } from "./mesh_preview.js";
import type { OverdrawPassKey } from "./overdraw.js";
import type { ObjectLookup } from "./vulkan/vulkan_object.js";
import type { CaptureCommand } from "../shared/protocol.js";

export type MeshStage = "in" | "out";

/** What the view needs of the capture tab it belongs to. */
export interface MeshViewHost {
  readonly data: CaptureData;
  readonly db: ObjectLookup;
  passLabelOf(key: OverdrawPassKey): string;
  /** The render pass a draw is in. */
  passOfDraw(cmd: CaptureCommand): OverdrawPassKey | null;
  drawsOfPass(key: OverdrawPassKey): CaptureCommand[];
  selectCommand(index: number): void;
  /** Vulkan: replays the capture for the draw's vertex shader outputs (and its pass's other draws, when there are few). */
  meshOutput(command: number, passDraws: CaptureCommand[]): Promise<MeshOutput>;
  /** The vertex shader's input names by location. */
  inputNames(cmd: CaptureCommand): Promise<Map<number, string>>;
}

export interface MeshViewOptions {
  stage?: MeshStage;
}

const ROWS_PER_PAGE = 100;

function formatValues(values: number[] | null): string {
  if (!values) return "—";
  return values.map((v) => (Number.isInteger(v) ? String(v) : Math.abs(v) >= 1e5 || (Math.abs(v) < 1e-3 && v !== 0) ? v.toExponential(3) : v.toFixed(4))).join(", ");
}

export class MeshView {
  readonly host: MeshViewHost;
  readonly root: Div;
  private _draw: CaptureCommand;
  private _stage: MeshStage;
  private _page = 0;
  private _selected: number | null = null;
  private _output: MeshOutput | null = null;
  private _outputRunning = false;
  private _outputError = "";
  private _input: MeshInput | null = null;
  /** VS In: the first preview vertex each draw-order position became. */
  private _inputListIndex = new Map<number, number>();
  private _token = 0;

  private _preview: MeshPreview | null = null;
  private _status: Span | null = null;
  private _notes: Div | null = null;
  private _table: Div | null = null;
  private _pager: Div | null = null;
  /** Whether the next mesh keeps the camera (stepping within a pass). */
  private _keepView = false;

  constructor(host: MeshViewHost, draw: CaptureCommand, options: MeshViewOptions = {}) {
    this.host = host;
    this._draw = draw;
    this._stage = options.stage ?? (host.data.api === "metal" ? "in" : "out");
    this.root = new Div(null, { class: "mesh-view" });
    this._rebuild();
  }

  get label(): string {
    return `Mesh #${this._draw.index}`;
  }

  /** Points the view at another draw. */
  show(draw: CaptureCommand, options: MeshViewOptions = {}): void {
    const samePass = this._samePass(draw);
    this._draw = draw;
    if (options.stage) this._stage = options.stage;
    this._keepView = samePass;
    this._rebuild();
  }

  dispose(): void {
    this._preview?.dispose();
  }

  /** The UI tests' view of the tab (tools/ui_tests.py). */
  debugState(): Record<string, unknown> {
    const o = this._output;
    const stats = o ? clipStats(o) : null;
    return {
      draw: this._draw.index, stage: this._stage, running: this._outputRunning, error: this._outputError || null,
      output: o ? { measured: o.measured, vertices: o.vertices, stride: o.stride, topology: o.topology, outputs: o.outputs.map((x) => x.name), note: o.note ?? null, stats } : null,
      input: this._input ? { vertices: this._input.ids.length, attributes: this._input.attributes.map((a) => a.name), position: this._input.position, notes: this._input.notes } : null,
      preview: this._preview?.debugState() ?? null,
      selected: this._selected,
    };
  }

  // ---------------------------------------------------------------------------------------

  private _samePass(draw: CaptureCommand): boolean {
    const a = this.host.passOfDraw(draw);
    const b = this.host.passOfDraw(this._draw);
    return !!a && !!b && a.frame === b.frame && a.commandBuffer === b.commandBuffer && a.passIndex === b.passIndex;
  }

  private _rebuild(): void {
    const token = ++this._token;
    this._preview?.dispose();
    this.root.html = "";
    this._page = 0;
    this._selected = null;
    this._output = null;
    this._outputError = "";
    this._input = null;
    const draw = this._draw;
    const pass = this.host.passOfDraw(draw);

    new Div(this.root, { class: "capture-texture-head", text: `${pass ? `${this.host.passLabelOf(pass)} — ` : ""}#${draw.index} ${draw.method}` });

    const bar = new Div(this.root, { class: "mesh-view-bar" });
    const stages: { stage: MeshStage; label: string }[] = [{ stage: "out", label: "VS Out" }, { stage: "in", label: "VS In" }];
    const stageSelect = new Select(bar, {
      options: stages.map((s) => s.label),
      index: stages.findIndex((s) => s.stage === this._stage),
      onChange: (_v: string, index: number) => {
        this._stage = stages[index].stage;
        this._keepView = false;
        this._rebuild();
      },
    });
    stageSelect.tooltip = "VS In: the vertices the draw read. VS Out: what its vertex shader wrote (replayed), in clip space divided by w.";

    const draws = pass ? this.host.drawsOfPass(pass) : [draw];
    const at = Math.max(0, draws.findIndex((c) => c.index === draw.index));
    const choose = (i: number): void => {
      const next = draws[Math.min(draws.length - 1, Math.max(0, i))];
      if (next && next.index !== this._draw.index) this.show(next);
    };
    if (draws.length > 1) new Button(bar, { label: "‹", class: "btn btn-sm", tooltip: "The pass's previous draw", disabled: at === 0, callback: () => choose(at - 1) });
    const drawSelect = new Select(bar, {
      options: draws.map((c) => `#${c.index} ${c.method.replace(/^vkCmd/, "")}`),
      index: at,
      onChange: (_v: string, index: number) => choose(index),
    });
    drawSelect.tooltip = `Draw ${at + 1} of the pass's ${draws.length}`;
    if (draws.length > 1) new Button(bar, { label: "›", class: "btn btn-sm", tooltip: "The pass's next draw", disabled: at === draws.length - 1, callback: () => choose(at + 1) });
    new Button(bar, { label: "Go to Draw", class: "btn btn-sm", tooltip: "Select the draw in the capture's tab", callback: () => this.host.selectCommand(this._draw.index) });
    new Button(bar, { label: "Reset View", class: "btn btn-sm", tooltip: "Frame the mesh again (or double-click the preview)", callback: () => this._preview?.resetView() });
    this._status = new Span(bar, { class: "text-muted" });
    this._notes = new Div(this.root, { class: "mesh-view-notes text-muted" });

    const body = new Div(this.root, { class: "mesh-view-body" });
    this._preview = new MeshPreview(body);
    const bottom = new Div(body, { class: "mesh-view-bottom" });
    this._pager = new Div(bottom, { class: "mesh-view-pager" });
    this._table = new Div(bottom, { class: "mesh-view-table" });

    if (this._stage === "in") {
      void this._showInput(token);
    } else {
      void this._showOutput(token, draws);
    }
  }

  private async _showInput(token: number): Promise<void> {
    const names = await this.host.inputNames(this._draw).catch(() => new Map<number, string>());
    if (token !== this._token) return;
    const input = meshInput(this.host.data, this.host.db, this._draw, names);
    this._input = input;
    const { positions, order } = listPositions(input);
    this._inputListIndex = new Map();
    order.forEach((o, i) => { if (!this._inputListIndex.has(o)) this._inputListIndex.set(o, i); });
    const kind = primitiveKind(input.topology);
    this._preview?.setMesh(positions.length ? { positions, kind, clip: false } : null, this._keepView);
    const name = input.position >= 0 ? input.attributes[input.position].name : "";
    this._setStatus(`${input.ids.length.toLocaleString()} vertices${name ? `, positions from ${name}` : ""}${input.topology ? `, ${input.topology.replace(/^VK_PRIMITIVE_TOPOLOGY_/, "")}` : ""}`);
    const notes = [...input.notes];
    if (input.position < 0 && input.attributes.length) notes.push("No input looks like a position, so the preview is empty; the table has every attribute.");
    this._setNotes(notes);
    this._renderTable();
  }

  private async _showOutput(token: number, draws: CaptureCommand[]): Promise<void> {
    if (this.host.data.api === "metal") {
      this._setStatus("");
      this._setNotes(["What a Metal draw's vertex function wrote needs a replay, which Metal captures do not have yet: VS In has the vertices it read."]);
      this._preview?.setMesh(null);
      return;
    }
    this._outputRunning = true;
    this._setStatus("Replaying the capture on this machine's GPU for the vertex shader's outputs...");
    try {
      const output = await this.host.meshOutput(this._draw.index, draws);
      if (token !== this._token) return;
      this._output = output;
    } catch (e) {
      if (token !== this._token) return;
      this._outputError = e instanceof Error ? e.message.split("\n")[0] : String(e);
    } finally {
      if (token === this._token) this._outputRunning = false;
    }
    const o = this._output;
    if (!o) {
      this._setStatus(`Not captured: ${this._outputError}`);
      this._preview?.setMesh(null);
      return;
    }
    const clip = clipPositions(o);
    this._preview?.setMesh(clip ? { positions: clip, kind: primitiveKind(o.topology), clip: true } : null, this._keepView);
    this._setStatus(meshSummary(o));
    const stats = clipStats(o);
    const notes: string[] = [];
    if (o.note && o.measured) notes.push(o.note);
    if (stats?.ndc) {
      const r = (k: number): string => `${stats.ndc!.min[k].toFixed(3)} to ${stats.ndc!.max[k].toFixed(3)}`;
      notes.push(`In front of the eye, the vertices span x ${r(0)}, y ${r(1)}, z ${r(2)} in normalized device coordinates (the view volume is -1 to 1, -1 to 1, 0 to 1).`);
    }
    this._setNotes(notes);
    this._renderTable();
  }

  private _setStatus(text: string): void {
    if (this._status) this._status.text = text;
  }

  private _setNotes(notes: string[]): void {
    if (!this._notes) return;
    this._notes.html = "";
    for (const n of notes) new Div(this._notes, { text: n });
  }

  /** The page of vertices, a row each, with the pager above it. */
  private _renderTable(): void {
    const table = this._table;
    const pager = this._pager;
    if (!table || !pager) return;
    table.html = "";
    pager.html = "";
    const o = this._stage === "out" ? this._output : null;
    const input = this._stage === "in" ? this._input : null;
    const rows = o ? (o.measured ? o.vertices : 0) : input ? input.ids.length : 0;
    if (!rows) return;
    const pages = Math.ceil(rows / ROWS_PER_PAGE);
    this._page = Math.min(this._page, pages - 1);
    const first = this._page * ROWS_PER_PAGE;
    const last = Math.min(rows, first + ROWS_PER_PAGE);
    if (pages > 1) {
      new Button(pager, { label: "‹", class: "btn btn-sm", disabled: this._page === 0, callback: () => { this._page--; this._renderTable(); } });
      new Span(pager, { text: `Vertices ${first.toLocaleString()}–${(last - 1).toLocaleString()} of ${rows.toLocaleString()}`, class: "text-muted" });
      new Button(pager, { label: "›", class: "btn btn-sm", disabled: this._page === pages - 1, callback: () => { this._page++; this._renderTable(); } });
    }

    const element = document.createElement("table");
    element.className = "mesh-table";
    const head = element.createTHead().insertRow();
    const header = (text: string, title = ""): void => {
      const th = document.createElement("th");
      th.textContent = text;
      if (title) th.title = title;
      head.appendChild(th);
    };
    header("#");
    if (o) {
      for (const out of o.outputs) header(out.name, `${out.builtin ? `${out.builtin}, ` : out.location !== undefined ? `location ${out.location}, ` : ""}${out.components} ${out.base}, offset ${out.offset}`);
    } else if (input) {
      if (input.indices) header("Index");
      header("Vertex");
      for (const a of input.attributes) header(a.name, `location ${a.location}, binding ${a.binding}, ${a.format}${a.perInstance ? ", per instance" : ""}`);
    }
    const body = element.createTBody();
    for (let r = first; r < last; r++) {
      const row = body.insertRow();
      row.className = r === this._selected ? "selected" : "";
      const cell = (text: string): void => { row.insertCell().textContent = text; };
      cell(String(r));
      if (o) {
        for (const out of o.outputs) cell(formatValues(outputValues(o, out, r)));
      } else if (input) {
        if (input.indices) cell(String(input.indices[r]));
        cell(String(input.ids[r]));
        input.attributes.forEach((_a, k) => cell(formatValues(input.values(r, k))));
      }
      row.onclick = () => {
        this._selected = this._selected === r ? null : r;
        for (const tr of Array.from(body.rows)) tr.className = "";
        if (this._selected !== null) row.className = "selected";
        const listIndex = this._selected === null ? null : o ? this._selected : this._inputListIndex.get(this._selected) ?? null;
        this._preview?.highlight(listIndex);
      };
    }
    table.element.appendChild(element);
  }
}
