// Frame-level performance analysis of a D3D12 capture: the rules of ../vulkan/frame_analysis.ts
// over D3D12's command stream. D3D12 has no load and store operations unless the application
// uses BeginRenderPass, so the attachment rules read that command's beginning and ending accesses
// and say nothing about an OMSetRenderTargets pass, which keeps and stores everything by definition.
//
//   undefined-load               BeginRenderPass PRESERVEs a target the previous render pass on it DISCARDed
//   clear-then-discard           a ClearRenderTargetView / ClearDepthStencilView of a target whose next
//                                BeginRenderPass discards or clears it again: the clear was wasted
//   empty-pass                   a render pass with no draws
//   redundant-pipeline-bind      SetPipelineState of the pipeline the list already has
//   redundant-root-signature-bind  binding the root signature already bound (which also resets every root parameter)
//   redundant-vertex-buffer-bind IASetVertexBuffers of the views already bound at those slots
//   tiny-draws                   many draws of a handful of vertices
//   single-threadgroup-dispatch  a dispatch of one thread group
//   unrecorded-list              a submitted command list the capture holds no commands of
//   suspended-pass               a render pass suspended across command lists, which carries no measurement
//
// The rules over the GPU counters (../counter_rules.ts), over sampling state (../sampling_rules.ts)
// and over the render graph (../render_graph_analysis.ts) are shared with Vulkan and Metal and run
// beside these (../vulkan/frame_analysis.ts, analyzeFrame).
//
// Every finding names the command it is about so the UI can jump to it.
import { D3D12_SETS, d3d12PipelineOf } from "./command_sets.js";
import { d3d12ViewSubresource } from "./d3d12_object.js";
import { isHandleRef, isObject, num, refId, str } from "../vulkan/vulkan_object.js";
import { SEVERITY_RANK, type Confidence, type Severity } from "../vulkan/spirv_analysis.js";
import type { FrameAnalysisDatabase, FrameFinding } from "../vulkan/frame_analysis.js";
import type { CaptureData } from "../capture_data.js";
import type { ArgObject, ArgValue, CaptureCommand } from "../../shared/protocol.js";

const TINY_DRAW_VERTICES = 12;
const TINY_DRAW_COUNT = 32;

const RULE_ORDER = ["unrecorded-list", "suspended-pass", "undefined-load", "clear-then-discard", "empty-pass", "tiny-draws",
  "redundant-pipeline-bind", "redundant-root-signature-bind", "redundant-vertex-buffer-bind", "single-threadgroup-dispatch"];

/** What the library emits for a submitted list whose commands it never recorded (src/d3d12/src/capture.cpp). */
const UNRECORDED_LIST = "<unrecorded command list>";

/** One finding per rule with the commands it applies to folded in: the first is named, the rest counted. */
class Folded {
  first: CaptureCommand | null = null;
  count = 0;
  commands: CaptureCommand[] = [];
  add(cmd: CaptureCommand): void {
    if (!this.first) this.first = cmd;
    this.count++;
    if (this.commands.length < 64) this.commands.push(cmd);
  }
}

function argKey(v: ArgValue | undefined): string {
  if (isHandleRef(v)) return `#${v.__id}`;
  if (Array.isArray(v)) return `[${v.map(argKey).join(",")}]`;
  if (isObject(v)) return `{${Object.entries(v).map(([k, e]) => `${k}:${argKey(e)}`).join(",")}}`;
  return str(v);
}

/** "D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR" -> "CLEAR". */
function accessType(v: ArgValue | undefined): string {
  const type = isObject(v) ? str(v.Type) : str(v);
  return type.replace(/^D3D12_RENDER_PASS_(BEGINNING|ENDING)_ACCESS_TYPE_/, "");
}

/** "resource:mip:slice" of a resolved handle {resource, view}, or null. */
function subresourceKey(handle: ArgValue | undefined): string | null {
  if (!isObject(handle)) return null;
  const id = refId(handle.resource);
  if (id === null) return null;
  const sub = d3d12ViewSubresource(handle.view);
  return `${id}:${sub.mip}:${sub.slice}`;
}

interface PassInfo {
  command: CaptureCommand;
  draws: number;
  /** BeginRenderPass only: the subresource key and how each target begins and ends. */
  targets: { key: string; begin: string; end: string }[];
}

/** What one command list has bound while it records; reset when the list is closed or reset. */
interface ListState {
  pipeline: Map<string, number>;        // bind point -> pipeline id
  rootSignature: Map<string, number>;   // bind point -> root signature id
  vertexBuffers: Map<number, string>;   // slot -> view key
}

export class D3D12FrameAnalysis {
  findings: FrameFinding[] = [];
  private _db: FrameAnalysisDatabase;
  private _byCommand = new Map<number, FrameFinding[]>();

  byCommand(): Map<number, FrameFinding[]> {
    return this._byCommand;
  }

  constructor(db: FrameAnalysisDatabase) {
    this._db = db;
  }

  analyze(data: CaptureData): FrameFinding[] {
    this.findings = [];
    this._byCommand = new Map();
    this._walk(data.commands);
    this.findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity] || RULE_ORDER.indexOf(a.rule) - RULE_ORDER.indexOf(b.rule));
    return this.findings;
  }

  private _walk(commands: CaptureCommand[]): void {
    const sets = D3D12_SETS;
    const lists = new Map<string, ListState>();
    const openPass = new Map<string, PassInfo>();          // stream -> the pass being recorded
    /** Subresource key -> the last BeginRenderPass ending access of it, and the clear command pending on it. */
    const lastEnding = new Map<string, string>();
    const pendingClear = new Map<string, CaptureCommand>();
    const redundantPipeline = new Folded();
    const redundantRootSignature = new Folded();
    const redundantVertexBuffers = new Folded();
    const tinyDraws = new Folded();
    const singleGroup = new Folded();
    const undefinedLoad = new Folded();
    const clearThenDiscard = new Folded();
    const emptyPass = new Folded();
    const unrecorded = new Folded();
    const suspended = new Folded();

    const stateOf = (stream: string): ListState => {
      let s = lists.get(stream);
      if (!s) lists.set(stream, (s = { pipeline: new Map(), rootSignature: new Map(), vertexBuffers: new Map() }));
      return s;
    };
    const closePass = (stream: string): void => {
      const pass = openPass.get(stream);
      if (!pass) return;
      openPass.delete(stream);
      if (pass.draws === 0) emptyPass.add(pass.command);
      for (const t of pass.targets) lastEnding.set(t.key, t.end);
    };

    for (const cmd of commands) {
      const a = cmd.args;
      const m = cmd.method;
      const stream = `${cmd.frame}:${cmd.object?.__id ?? 0}:${cmd.secondary ?? 0}`;

      // A list the capture holds no commands of: it was recorded before the capture asked for
      // anything, so everything it did is missing from the frame.
      if (m === UNRECORDED_LIST) {
        unrecorded.add(cmd);
        continue;
      }
      if (sets.SUBMIT.has(m)) continue;
      if (m === "Close" || m === "Reset") {
        closePass(stream);
        lists.delete(stream);
        continue;
      }
      if (sets.PASS_BEGIN.has(m)) {
        closePass(stream);
        const pass: PassInfo = { command: cmd, draws: 0, targets: [] };
        if (m === "BeginRenderPass" && a) {
          // A pass suspended here or resumed from another list: between the suspension and the
          // resume nothing may be added to the list, so the capture measures none of it.
          if (/SUSPENDING|RESUMING/.test(str(a.Flags))) suspended.add(cmd);
          const entries: [ArgObject, boolean][] = [];
          for (const rt of Array.isArray(a.pRenderTargets) ? a.pRenderTargets : []) if (isObject(rt)) entries.push([rt, false]);
          if (isObject(a.pDepthStencil)) entries.push([a.pDepthStencil, true]);
          for (const [rt, depth] of entries) {
            const key = subresourceKey(isObject(rt.cpuDescriptor) ? rt.cpuDescriptor : rt);
            if (!key) continue;
            const begin = accessType(depth ? rt.DepthBeginningAccess : rt.BeginningAccess);
            const end = accessType(depth ? rt.DepthEndingAccess : rt.EndingAccess);
            pass.targets.push({ key, begin, end });
            // Loading what the last pass on this subresource threw away: undefined contents.
            if (begin === "PRESERVE" && lastEnding.get(key) === "DISCARD") undefinedLoad.add(cmd);
            // A clear before the pass, and the pass discards or clears the target on entry.
            const clear = pendingClear.get(key);
            if (clear && (begin === "DISCARD" || begin === "CLEAR")) clearThenDiscard.add(clear);
            pendingClear.delete(key);
          }
        } else if (a) {
          // OMSetRenderTargets keeps its targets: a pending clear on one is consumed, not wasted.
          for (const h of Array.isArray(a.pRenderTargetDescriptors) ? a.pRenderTargetDescriptors : []) {
            const key = subresourceKey(h);
            if (key) { pendingClear.delete(key); lastEnding.delete(key); }
          }
          const depth = subresourceKey(a.pDepthStencilDescriptor);
          if (depth) { pendingClear.delete(depth); lastEnding.delete(depth); }
        }
        openPass.set(stream, pass);
        continue;
      }
      if (sets.PASS_END.has(m)) {
        closePass(stream);
        continue;
      }
      if (!a) continue;

      if (m === "ClearRenderTargetView" || m === "ClearDepthStencilView") {
        const key = subresourceKey(a[m === "ClearRenderTargetView" ? "RenderTargetView" : "DepthStencilView"]);
        // A clear inside an OMSetRenderTargets pass is a draw-like write; one outside it may be
        // wasted by the BeginRenderPass that follows.
        if (key && !openPass.has(stream)) pendingClear.set(key, cmd);
        if (key) lastEnding.delete(key);
        continue;
      }
      if (sets.BIND_PIPELINE.has(m)) {
        const id = refId(d3d12PipelineOf(a));
        const point = sets.pipelineBindPointOf(m, a);
        const state = stateOf(stream);
        if (id !== null) {
          if (state.pipeline.get(point) === id) redundantPipeline.add(cmd);
          state.pipeline.set(point, id);
        }
        continue;
      }
      if (m === "SetGraphicsRootSignature" || m === "SetComputeRootSignature") {
        const id = refId(a.pRootSignature);
        const point = m === "SetComputeRootSignature" ? "compute" : "graphics";
        const state = stateOf(stream);
        if (id !== null) {
          if (state.rootSignature.get(point) === id) redundantRootSignature.add(cmd);
          state.rootSignature.set(point, id);
        }
        continue;
      }
      if (sets.BIND_VERTEX.has(m)) {
        const state = stateOf(stream);
        const views = Array.isArray(a.pViews) ? a.pViews : [];
        const first = num(a.StartSlot);
        let same = views.length > 0;
        views.forEach((v, i) => {
          const key = argKey(v);
          if (state.vertexBuffers.get(first + i) !== key) same = false;
          state.vertexBuffers.set(first + i, key);
        });
        if (same) redundantVertexBuffers.add(cmd);
        continue;
      }
      if (sets.DRAW.has(m)) {
        const pass = openPass.get(stream);
        if (pass) pass.draws++;
        const vertices = num(a.IndexCountPerInstance) || num(a.VertexCountPerInstance);
        if (vertices > 0 && vertices <= TINY_DRAW_VERTICES) tinyDraws.add(cmd);
        continue;
      }
      if (m === "Dispatch") {
        if (num(a.ThreadGroupCountX) === 1 && num(a.ThreadGroupCountY) === 1 && num(a.ThreadGroupCountZ) === 1) singleGroup.add(cmd);
      }
    }
    for (const stream of [...openPass.keys()]) closePass(stream);

    if (unrecorded.count) {
      this._addFolded("unrecorded-list", "high", "high",
        `${unrecorded.count} submitted command list${unrecorded.count === 1 ? " holds" : "s hold"} no commands: `
        + `${unrecorded.count === 1 ? "it was" : "they were"} recorded before the capture began, so the draws, dispatches and state `
        + "in them are missing from this frame — an engine that records a frame ahead on worker threads (Unity does) records this way. "
        + "Turn on \"Record all command buffers\" in the capture bar and capture again: every list is then recorded as it is built, "
        + "whenever that happens.", unrecorded);
    }
    if (suspended.count) {
      this._addFolded("suspended-pass", "low", "high",
        `${suspended.count} render pass${suspended.count === 1 ? " is" : "es are"} suspended across command lists `
        + "(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS / _RESUMING_PASS). Between a suspension and its resume Direct3D allows "
        + "no work at all on the list, so these passes have no timings and their render targets were not read back; "
        + "their commands are all here.", suspended);
    }
    if (undefinedLoad.count) {
      this._addFolded("undefined-load", "high", "high", `${undefinedLoad.count} render pass${undefinedLoad.count === 1 ? "" : "es"} PRESERVE${undefinedLoad.count === 1 ? "s" : ""} a target the previous render pass on it ended with D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD: the contents are undefined. Either preserve it there, or begin with CLEAR or DISCARD here.`, undefinedLoad);
    }
    if (clearThenDiscard.count) {
      this._addFolded("clear-then-discard", "medium", "high", `${clearThenDiscard.count} clear${clearThenDiscard.count === 1 ? "" : "s"} of a render target that the BeginRenderPass after it discards or clears again on entry: the clear is wasted. Let the pass clear it (D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR), which a tiled GPU does in tile memory.`, clearThenDiscard);
    }
    if (emptyPass.count) {
      this._addFolded("empty-pass", "low", "medium", `${emptyPass.count} render pass${emptyPass.count === 1 ? "" : "es"} with no draws: on a tiled GPU each still loads and stores its targets.`, emptyPass);
    }
    if (tinyDraws.count >= TINY_DRAW_COUNT) {
      this._addFolded("tiny-draws", "low", "medium", `${tinyDraws.count} draws of ${TINY_DRAW_VERTICES} vertices or fewer: candidates for instancing or merging into one buffer.`, tinyDraws);
    }
    if (redundantPipeline.count) {
      this._addFolded("redundant-pipeline-bind", "low", "high", `${redundantPipeline.count} SetPipelineState call${redundantPipeline.count === 1 ? "" : "s"} of the pipeline the command list already had.`, redundantPipeline);
    }
    if (redundantRootSignature.count) {
      this._addFolded("redundant-root-signature-bind", "low", "high", `${redundantRootSignature.count} root signature bind${redundantRootSignature.count === 1 ? "" : "s"} of the root signature already bound. Setting a root signature also clears every root parameter, so the tables and constants have to be set again after it.`, redundantRootSignature);
    }
    if (redundantVertexBuffers.count) {
      this._addFolded("redundant-vertex-buffer-bind", "low", "high", `${redundantVertexBuffers.count} IASetVertexBuffers call${redundantVertexBuffers.count === 1 ? "" : "s"} of the views already bound at those slots.`, redundantVertexBuffers);
    }
    if (singleGroup.count) {
      this._addFolded("single-threadgroup-dispatch", "low", "medium", `${singleGroup.count} dispatch${singleGroup.count === 1 ? "" : "es"} of a single thread group: the rest of the GPU idles while it runs.`, singleGroup);
    }
  }

  private _add(rule: string, severity: Severity, confidence: Confidence, message: string, cmd: CaptureCommand | null, count = 1): FrameFinding {
    const f: FrameFinding = { rule, severity, confidence, message, commandIndex: cmd?.index, count };
    this.findings.push(f);
    if (cmd) this._attach(cmd.index, f);
    return f;
  }

  private _addFolded(rule: string, severity: Severity, confidence: Confidence, message: string, folded: Folded): void {
    const f = this._add(rule, severity, confidence, message, folded.first, folded.count);
    for (const cmd of folded.commands) if (cmd !== folded.first) this._attach(cmd.index, f);
  }

  private _attach(index: number, f: FrameFinding): void {
    const list = this._byCommand.get(index);
    if (list) list.push(f); else this._byCommand.set(index, [f]);
  }
}

export function analyzeD3D12Frame(data: CaptureData, db: FrameAnalysisDatabase): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } {
  const analysis = new D3D12FrameAnalysis(db);
  const findings = analysis.analyze(data);
  return { findings, byCommand: analysis.byCommand() };
}
