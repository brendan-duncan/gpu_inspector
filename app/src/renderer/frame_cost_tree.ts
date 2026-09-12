// Frame-level shader cost tree: the captured frame's GPU work broken down by pass, pipeline (or
// draw), shader stage and function, for the Shader Flame Graph (after WebGPU Inspector's
// frame_cost_tree.js). Three sources feed it, not equally trustworthy:
//
//   measured  - pass GPU durations from the capture's timestamp queries ("Profile passes").
//   exact     - vertex, index and compute invocation counts read out of the captured draw and
//               dispatch arguments (indirect ones from the captured argument buffers).
//   modeled   - the per-invocation instruction mix from the static SPIR-V cost model
//               (spirv_analysis.ts), which distributes a pass's time across the shaders in it.
//   estimated - fragment invocations, which only rasterization knows. A pass whose GPU counters
//               carry `fragmentInvocations` (pipeline statistics on Vulkan, the statistic set on
//               Metal) gives its own exact total, split across its draws by scissor area; without
//               counters the scissor area is the estimate, an upper bound, or the stage is shown
//               unweighted.
//
// When every pass of the frame has a measured duration the tree reads in milliseconds: the root
// and each pass width are real, only the split within a pass is modeled. Otherwise it falls
// back to modeled op units, comparable to each other but not to wall-clock time.
//
// No DOM dependency; frame_flamegraph.ts renders what this returns.
import type { ArgValue, CaptureCommand } from "../shared/protocol.js";
import type { ObjectDatabase } from "./vulkan/object_database.js";
import type { CaptureData } from "./capture_data.js";
import { passKey } from "./capture_data.js";
import type { ShaderStage } from "./vulkan/spirv_reflect.js";
import { dominantDimension, weighCost, type CostDimension, type CostVec, type FunctionAnalysis, type ShaderAnalysis } from "./vulkan/spirv_analysis.js";
import { isAction } from "./command_sets.js";
import { drawStatsByCommand } from "./draw_stats.js";
import { isObject, num, refId, str } from "./vulkan/vulkan_object.js";
import type { FlameGraphNodeBase } from "./widget/flamegraph.js";

export type CostUnits = "ms" | "ops";
export type Confidence = "measured" | "exact" | "estimated" | "unknown";

/** One shader stage of a pipeline with its analysis, gathered by the panel before the build. */
export interface StageModel {
  stage: ShaderStage;
  entryPoint: string;
  /** The pipeline or shader module holding the code (to reveal it). */
  objectId: number;
  analysis: ShaderAnalysis | null;
  workgroupSize: [number, number, number] | null;
}

/** Line frames kept per function in the flame graph; the rest fold into one frame. */
const MAX_LINE_FRAMES = 16;

export interface FlameNode extends FlameGraphNodeBase<FlameNode> {
  kind: "frame" | "pass" | "item" | "stage" | "function" | "line" | "more";
  selfCost: number;
  children: FlameNode[];
  /** What dominates the cost (frame color). */
  dimension?: CostDimension;
  /** Includes modeled or estimated assumptions. */
  estimated?: boolean;
  /** The command to select (a pass's begin, an item's first draw). */
  command?: CaptureCommand;
  /** The pipeline or module to reveal. */
  objectId?: number;
  /** Stage, function and line frames: the stage the code runs in. */
  stage?: ShaderStage;
  /** Stage frames: the entry point. */
  entryPoint?: string;
  invocations?: number;
  confidence?: Confidence;
  /** Pass frames: the measured GPU duration, null without. */
  durationMs?: number | null;
  /** Stage frames that carry no weight: why. */
  reason?: string;
  /** Line frames: the source line (and file) the cost belongs to. */
  line?: number;
  file?: string;
}

export interface CostTreeOptions {
  data: CaptureData;
  db: ObjectDatabase;
  /** Stage models per pipeline object id. */
  models: Map<number, StageModel[]>;
  /** One frame per draw instead of one per pipeline. */
  perDraw?: boolean;
  /** Weight fragment stages by the scissor area. */
  estimateFragments?: boolean;
  /** Frames kept per pass; the costliest survive, the tail collapses into one. */
  maxFramesPerPass?: number;
}

export interface CostTreeResult {
  root: FlameNode;
  units: CostUnits;
  notes: string[];
  stats: { passes: number; items: number; unknownStages: number; estimatedStages: number; collapsed: number;
           /** Render passes whose fragment stages are weighted by measured invocations, not by area. */
           measuredFragmentPasses: number;
           /** Passes whose draws were timed one by one by the replay, which sets the split between them. */
           measuredDrawPasses: number };
}

export function formatCostValue(value: number, units: CostUnits): string {
  if (units === "ms") return `${value.toFixed(3)} ms`;
  if (value >= 1e9) return `${(value / 1e9).toFixed(1)}G ops`;
  if (value >= 1e6) return `${(value / 1e6).toFixed(1)}M ops`;
  if (value >= 1e3) return `${(value / 1e3).toFixed(1)}k ops`;
  return `${value.toFixed(1)} ops`;
}

function node(kind: FlameNode["kind"], name: string, totalCost = 0, children: FlameNode[] = []): FlameNode {
  return { kind, name, totalCost, selfCost: 0, children };
}

function rollup(n: FlameNode): FlameNode {
  let total = 0;
  for (const c of n.children) {
    total += c.totalCost;
    n.estimated = n.estimated || c.estimated;
  }
  n.totalCost = total;
  return n;
}

function scaleSubtree(n: FlameNode, factor: number): void {
  n.totalCost *= factor;
  n.selfCost *= factor;
  for (const c of n.children) scaleSubtree(c, factor);
}

// ---------------------------------------------------------------------------------------------
// Invocation counts

interface StageInvocations {
  model: StageModel;
  invocations: number | null;
  confidence: Confidence;
}

interface Item {
  command: CaptureCommand;
  pipelineId: number;
  kind: "draw" | "dispatch";
  stages: StageInvocations[];
  /** The draw's scissor (clipped to the render area): how a pass's measured fragments are split. */
  area: number | null;
  /** What the replay timed this draw at, null without per-draw measurements (draw_stats.ts). */
  ms: number | null;
}

interface Pass {
  key: string;
  kind: "render" | "compute";
  label: string;
  command: CaptureCommand | null;
  items: Item[];
  durationMs: number | null;
  /** Render area in pixels (fragment estimate bound). */
  area: number | null;
  /** Fragment shader invocations the pass's GPU counters measured, null without them. */
  measuredFragments: number | null;
}

function readU32(data: Uint8Array, offset: number): number | null {
  if (offset + 4 > data.byteLength) return null;
  return new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(offset, true);
}

/** Vertex (or index) invocations of a draw, from its arguments or its captured indirect buffer. */
function drawInvocations(cmd: CaptureCommand, data: CaptureData): number | null {
  const a = cmd.args;
  if (!a) return null;
  switch (cmd.method) {
    case "vkCmdDraw": return num(a.vertexCount) * Math.max(1, num(a.instanceCount));
    case "vkCmdDrawIndexed": return num(a.indexCount) * Math.max(1, num(a.instanceCount));
    case "vkCmdDrawIndirect":
    case "vkCmdDrawIndexedIndirect": {
      const captured = data.buffer(cmd.bufferData?.[0]);
      if (!captured || !captured.data) return null;
      const count = Math.max(0, num(a.drawCount));
      const stride = Math.max(16, num(a.stride));
      let total = 0;
      for (let i = 0; i < count; i++) {
        const n = readU32(captured.data, i * stride);
        const inst = readU32(captured.data, i * stride + 4);
        if (n === null || inst === null) return null;
        total += n * Math.max(1, inst);
      }
      return total;
    }
    default: return null;   // draw-count indirect, multi-draw, mesh tasks: not derivable here
  }
}

function dispatchGroups(cmd: CaptureCommand, data: CaptureData): number | null {
  const a = cmd.args;
  if (!a) return null;
  switch (cmd.method) {
    case "vkCmdDispatch":
    case "vkCmdDispatchBase": return num(a.groupCountX) * num(a.groupCountY) * num(a.groupCountZ);
    case "vkCmdDispatchIndirect": {
      const captured = data.buffer(cmd.bufferData?.[0]);
      if (!captured || !captured.data) return null;
      const x = readU32(captured.data, 0), y = readU32(captured.data, 4), z = readU32(captured.data, 8);
      return x === null || y === null || z === null ? null : x * y * z;
    }
    default: return null;
  }
}

function rectArea(v: ArgValue | undefined): number | null {
  if (!isObject(v)) return null;
  const extent = isObject(v.extent) ? v.extent : null;
  if (!extent) return null;
  const w = num(extent.width), h = num(extent.height);
  return w > 0 && h > 0 ? w * h : null;
}

// ---------------------------------------------------------------------------------------------
// The walk: passes and their draws / dispatches, mirroring the capture panel's pass numbering
// (render passes and compute runs counted per command buffer within each frame).

function collectPasses(o: CostTreeOptions): { passes: Pass[]; notes: string[]; measuredFragmentPasses: number } {
  const { data, models } = o;
  // Per-draw timings and counters, where the capture has been replayed for them.
  const drawStats = drawStatsByCommand(data.drawStats ?? []);
  const sets = data.sets;
  const passes: Pass[] = [];
  const notes: string[] = [];
  let missingModels = 0;

  for (let frame = 0; frame < Math.max(1, data.frames); frame++) {
    const commands = data.commandsForFrame(frame);
    const passCounters = new Map<number, number>();
    const computeCounters = new Map<number, number>();
    const bound = new Map<string, number>();       // "stream:bindPoint" -> pipeline id
    const scissor = new Map<string, number | null>(); // stream -> scissor area
    let currentCb = -1;
    let currentSecondary = 0;
    let renderPass: Pass | null = null;
    let compute: Pass | null = null;

    const closeCompute = (): void => { compute = null; };
    const closeSecondary = (): void => { closeCompute(); currentSecondary = 0; };
    const closeCommandBuffer = (): void => { closeSecondary(); currentCb = -1; renderPass = null; };

    for (const cmd of commands) {
      if (!cmd) continue;
      const objId = cmd.object?.__id ?? 0;
      if ((cmd.secondary ?? 0) !== currentSecondary) {
        closeSecondary();
        currentSecondary = cmd.secondary ?? 0;
      }
      if (sets.SUBMIT.has(cmd.method)) { closeCommandBuffer(); continue; }
      if (objId !== currentCb) {
        closeCommandBuffer();
        currentCb = objId;
        if (cmd.method.startsWith("<")) continue;
      }
      const stream = `${objId}:${cmd.secondary ?? 0}`;
      const a = cmd.args;

      if (sets.PASS_BEGIN.has(cmd.method)) {
        closeCompute();
        const index = passCounters.get(objId) ?? 0;
        passCounters.set(objId, index + 1);
        const key = passKey(frame, objId, index);
        const timing = data.passTimings.get(key);
        let area: number | null = null;
        if (a && isObject(a.pRenderPassBegin)) area = rectArea(a.pRenderPassBegin.renderArea);
        else if (a && isObject(a.pRenderingInfo)) area = rectArea(a.pRenderingInfo.renderArea);
        const fragments = timing?.counters?.fragmentInvocations;
        renderPass = { key, kind: "render", label: `Render Pass ${index}`, command: cmd, items: [], durationMs: timing ? timing.durationMs : null, area,
                       measuredFragments: typeof fragments === "number" && fragments > 0 ? fragments : null };
        passes.push(renderPass);
        continue;
      }
      if (sets.PASS_END.has(cmd.method)) { renderPass = null; continue; }
      if (sets.COMPUTE_PASS_END.has(cmd.method) || cmd.method === "vkEndCommandBuffer" || sets.LABEL_BEGIN.has(cmd.method) || sets.LABEL_END.has(cmd.method)) closeCompute();

      if (sets.BIND_PIPELINE.has(cmd.method) && a) {
        const id = refId(a.pipeline);
        if (id !== null) bound.set(`${stream}:${sets.pipelineBindPointOf(cmd.method, a)}`, id);
        continue;
      }
      if ((cmd.method === "vkCmdSetScissor" || cmd.method === "vkCmdSetScissorWithCount" || cmd.method === "vkCmdSetScissorWithCountEXT") && a) {
        const rects = a.pScissors;
        scissor.set(stream, Array.isArray(rects) && rects.length ? rectArea(rects[0]) : null);
        continue;
      }
      if (!isAction(sets, cmd.method)) continue;

      const isDispatch = sets.DISPATCH.has(cmd.method);
      const pipelineId = bound.get(`${stream}:${sets.bindPointOf(cmd.method)}`);
      if (pipelineId === undefined) continue;
      let pass: Pass | null;
      if (isDispatch && !renderPass) {
        if (!compute) {
          const index = computeCounters.get(objId) ?? 0;
          computeCounters.set(objId, index + 1);
          const key = passKey(frame, objId, index, true);
          const timing = data.passTimings.get(key);
          compute = { key, kind: "compute", label: `Compute ${index}`, command: cmd, items: [], durationMs: timing ? timing.durationMs : null, area: null,
                      measuredFragments: null };
          passes.push(compute);
        }
        pass = compute;
      } else {
        pass = renderPass;
      }
      if (!pass) continue;   // a draw outside any pass: not valid Vulkan, skipped

      const stageModels = models.get(pipelineId);
      if (!stageModels) { missingModels++; continue; }
      const stages: StageInvocations[] = [];
      let drawArea: number | null = null;
      // What the replay measured for this very draw: exact counts, indirect arguments included.
      const measured = drawStats.get(cmd.index);
      const counted = measured?.counted === true;
      if (isDispatch) {
        const groups = dispatchGroups(cmd, data);
        for (const m of stageModels) {
          const wg = m.workgroupSize ? m.workgroupSize[0] * m.workgroupSize[1] * m.workgroupSize[2] : null;
          const fromArgs = groups !== null && wg !== null ? groups * wg : null;
          const inv = counted && measured!.computeInvocations > 0 ? measured!.computeInvocations : fromArgs;
          stages.push({ model: m, invocations: inv, confidence: inv === null ? "unknown" : "exact" });
        }
      } else {
        const vertices = drawInvocations(cmd, data);
        const scissorArea = scissor.get(stream);
        drawArea = scissorArea != null && pass.area != null ? Math.min(scissorArea, pass.area) : scissorArea ?? pass.area ?? null;
        const fragmentArea = o.estimateFragments ? drawArea : null;
        for (const m of stageModels) {
          if (m.stage === "fragment") {
            if (counted) stages.push({ model: m, invocations: measured!.fragmentInvocations, confidence: "exact" });
            else stages.push({ model: m, invocations: fragmentArea, confidence: fragmentArea === null ? "unknown" : "estimated" });
          } else if (m.stage === "vertex") {
            const inv = counted && measured!.vertexInvocations > 0 ? measured!.vertexInvocations : vertices;
            stages.push({ model: m, invocations: inv, confidence: inv === null ? "unknown" : "exact" });
          } else {
            // Tessellation, geometry, mesh: scaled by the vertex count as a stand-in.
            stages.push({ model: m, invocations: vertices, confidence: vertices === null ? "unknown" : "estimated" });
          }
        }
      }
      pass.items.push({ command: cmd, pipelineId, kind: isDispatch ? "dispatch" : "draw", stages, area: drawArea,
                       ms: measured?.timed ? measured.ms : null });
    }
  }
  if (missingModels) notes.push(`${missingModels} draw(s) or dispatch(es) use a pipeline whose shaders could not be fetched and are left out.`);

  // Where the pass's counters measured its fragment invocations, that total replaces the area
  // estimate: it is what rasterization actually ran, overdraw and the depth test included. Nothing
  // measures one draw on its own yet, so a pass with several splits its total between them by
  // area, which is exact for the common pass that draws once.
  let measuredFragmentPasses = 0;
  for (const pass of passes) {
    const measured = pass.measuredFragments;
    if (measured === null) continue;
    const fragments: { stage: StageInvocations; area: number | null }[] = [];
    for (const item of pass.items) {
      for (const stage of item.stages) if (stage.model.stage === "fragment") fragments.push({ stage, area: item.area });
    }
    if (!fragments.length) continue;
    // Per-draw counts are already exact, and are not to be replaced by a share of the pass total.
    if (fragments.some((f) => f.stage.confidence === "exact")) continue;
    const totalArea = fragments.reduce((sum, f) => sum + (f.area ?? 0), 0);
    for (const f of fragments) {
      const share = totalArea > 0 ? (f.area ?? 0) / totalArea : 1 / fragments.length;
      f.stage.invocations = measured * share;
      f.stage.confidence = fragments.length === 1 ? "exact" : "estimated";
    }
    measuredFragmentPasses++;
  }
  return { passes, notes, measuredFragmentPasses };
}

// ---------------------------------------------------------------------------------------------
// Function subtrees from the analysis

function entryOf(model: StageModel) {
  const a = model.analysis;
  if (!a) return null;
  return a.entryPoints.find((e) => e.name === model.entryPoint && e.stage === model.stage)
    ?? a.entryPoints.find((e) => e.name === model.entryPoint)
    ?? a.entryPoints.find((e) => e.stage === model.stage)
    ?? null;
}

/** The call tree under a function, costs in ops per invocation (scaled later), recursion cut. */
function functionTree(fn: FunctionAnalysis, byId: Map<number, FunctionAnalysis>, factor: number, path: Set<number>, depth: number): FlameNode {
  const n = node("function", fn.name || `function ${fn.id}`);
  n.totalCost = weighCost(fn.inclusive) * factor;
  n.selfCost = weighCost(fn.cost) * factor;
  n.dimension = dominantDimension(fn.inclusive);
  if (depth < 24) {
    path.add(fn.id);
    for (const calleeId of fn.calls) {
      const callee = byId.get(calleeId);
      if (!callee || path.has(calleeId)) continue;
      n.children.push(functionTree(callee, byId, factor, path, depth + 1));
    }
    path.delete(fn.id);
  }
  // The function's own cost by source line (modules with line information): the costliest
  // lines as frames, the rest folded into one.
  if (fn.lines.length) {
    const shown = fn.lines.slice(0, MAX_LINE_FRAMES);
    for (const l of shown) {
      const ln = node("line", `${l.file ? `${l.file}:` : "line "}${l.line}`, l.weighted * factor);
      ln.selfCost = ln.totalCost;
      ln.dimension = l.dominant;
      ln.line = l.line;
      ln.file = l.file;
      n.children.push(ln);
    }
    const rest = fn.lines.slice(MAX_LINE_FRAMES).reduce((acc, l) => acc + l.weighted, 0);
    if (rest > 0) n.children.push(node("more", `+ ${fn.lines.length - MAX_LINE_FRAMES} more lines`, rest * factor));
  }
  // Children cannot exceed the parent: a call counted more than once in the inclusive cost
  // (loops) is squeezed proportionally so the graph stays consistent.
  const sum = n.children.reduce((s, c) => s + c.totalCost, 0);
  if (sum > n.totalCost && sum > 0) for (const c of n.children) scaleSubtree(c, n.totalCost / sum);
  return n;
}

// ---------------------------------------------------------------------------------------------

export function buildFrameCostTree(o: CostTreeOptions): CostTreeResult {
  const { db } = o;
  const maxFramesPerPass = o.maxFramesPerPass ?? 32;
  const { passes, notes, measuredFragmentPasses } = collectPasses(o);
  const stats = { passes: passes.length, items: 0, unknownStages: 0, estimatedStages: 0, collapsed: 0, measuredFragmentPasses, measuredDrawPasses: 0 };

  const measured = passes.filter((p) => p.durationMs !== null && p.durationMs > 0);
  const allMeasured = passes.length > 0 && measured.length === passes.length;
  const units: CostUnits = allMeasured ? "ms" : "ops";
  if (!allMeasured && measured.length > 0) {
    notes.push(`Only ${measured.length} of ${passes.length} passes have GPU timings, so the graph is in modeled op units rather than milliseconds.`);
  } else if (!allMeasured && passes.length > 0) {
    notes.push("No GPU pass timings in this capture, so the graph is in modeled op units. Capture with \"Profile passes\" to scale it to measured milliseconds.");
  }

  const passNodes: FlameNode[] = [];
  for (const pass of passes) {
    stats.items += pass.items.length;
    // Buckets: one per pipeline, or one per draw.
    const buckets = new Map<string, { key: string; pipelineId: number; items: Item[] }>();
    for (const item of pass.items) {
      const key = o.perDraw ? `d${item.command.index}` : `p${item.pipelineId}`;
      let b = buckets.get(key);
      if (!b) { b = { key, pipelineId: item.pipelineId, items: [] }; buckets.set(key, b); }
      b.items.push(item);
    }

    // Resolve every bucket to a scalar cost first; only the survivors of the cap get subtrees.
    interface Resolved { bucket: { key: string; pipelineId: number; items: Item[] }; stages: { model: StageModel; invocations: number; confidence: Confidence; unknown: boolean; cost: number }[]; cost: number }
    const resolved: Resolved[] = [];
    for (const bucket of buckets.values()) {
      const totals = new Map<string, { model: StageModel; invocations: number; confidence: Confidence; unknown: boolean }>();
      for (const item of bucket.items) {
        for (const s of item.stages) {
          const key = `${s.model.stage}:${s.model.objectId}:${s.model.entryPoint}`;
          let acc = totals.get(key);
          if (!acc) { acc = { model: s.model, invocations: 0, confidence: s.confidence, unknown: false }; totals.set(key, acc); }
          if (s.invocations === null) acc.unknown = true;
          else {
            acc.invocations += s.invocations;
            if (s.confidence === "estimated") acc.confidence = "estimated";
          }
        }
      }
      const stages: Resolved["stages"] = [];
      let cost = 0;
      for (const acc of totals.values()) {
        const entry = entryOf(acc.model);
        const usable = !!entry && !acc.unknown && acc.invocations > 0;
        if (!usable) stats.unknownStages++;
        else if (acc.confidence === "estimated") stats.estimatedStages++;
        const c = usable ? weighCost(entry!.cost) * acc.invocations : 0;
        cost += c;
        stages.push({ ...acc, cost: c });
      }
      resolved.push({ bucket, stages, cost });
    }

    // Where the replay timed every draw of the pass, those times set the split between the items
    // and the modeled costs only split each item between its stages. Mixed measurements would put
    // op units and milliseconds in one pass, so it is all or nothing.
    const bucketMs = (items: Item[]): number => items.reduce((sum, i) => sum + (i.ms ?? 0), 0);
    const timedItems = resolved.length > 0 && resolved.every((r) => r.bucket.items.every((i) => i.ms !== null));
    if (timedItems) stats.measuredDrawPasses++;

    let kept = resolved;
    let collapsed: { count: number; draws: number; cost: number } | null = null;
    if (resolved.length > maxFramesPerPass) {
      const sorted = resolved.slice().sort((x, y) => y.cost - x.cost);
      kept = sorted.slice(0, maxFramesPerPass);
      const tail = sorted.slice(maxFramesPerPass);
      collapsed = { count: tail.length, draws: tail.reduce((s, r) => s + r.bucket.items.length, 0),
                    cost: timedItems ? tail.reduce((s, r) => s + bucketMs(r.bucket.items), 0) : tail.reduce((s, r) => s + r.cost, 0) };
      stats.collapsed += tail.length;
    }

    const itemNodes: FlameNode[] = [];
    for (const { bucket, stages } of kept) {
      const stageNodes: FlameNode[] = [];
      for (const s of stages) {
        const label = `${s.model.stage}: ${s.model.entryPoint}`;
        const entry = entryOf(s.model);
        if (!entry || s.unknown || s.invocations <= 0) {
          const reason = !entry ? (s.model.analysis ? "entry point not found" : "not analyzable") : "invocation count unknown";
          const n = node("stage", `${label} (${reason})`);
          n.estimated = true;
          n.reason = reason;
          n.objectId = s.model.objectId;
          n.stage = s.model.stage;
          n.entryPoint = s.model.entryPoint;
          stageNodes.push(n);
          continue;
        }
        const byId = new Map(s.model.analysis!.functions.map((f) => [f.id, f]));
        const root = byId.get(entry.functionId);
        const suffix = s.confidence === "estimated" ? " estimated" : "";
        const n = node("stage", `${label}: ${s.invocations.toLocaleString()}${suffix} invocations`, s.cost);
        n.dimension = entry.dominant;
        n.invocations = s.invocations;
        n.confidence = s.confidence;
        n.estimated = s.confidence !== "exact";
        n.objectId = s.model.objectId;
        n.stage = s.model.stage;
        n.entryPoint = s.model.entryPoint;
        n.command = bucket.items[0].command;
        if (root) {
          const tree = functionTree(root, byId, s.invocations, new Set(), 0);
          const tag = (c: FlameNode): void => { c.objectId = s.model.objectId; c.stage = s.model.stage; for (const cc of c.children) tag(cc); };
          for (const c of tree.children) tag(c);
          n.children = tree.children;
          n.selfCost = tree.selfCost;
          // The entry's own cost is the stage cost; its subtree must not exceed it.
          const sum = n.children.reduce((acc, c) => acc + c.totalCost, 0);
          if (sum > n.totalCost && sum > 0) for (const c of n.children) scaleSubtree(c, n.totalCost / sum);
        }
        stageNodes.push(n);
      }
      const first = bucket.items[0];
      const pipeline = db.getObject(bucket.pipelineId);
      const count = bucket.items.length;
      const noun = first.kind === "draw" ? (count === 1 ? "draw" : "draws") : (count === 1 ? "dispatch" : "dispatches");
      const name = o.perDraw
        ? `${first.command.method.replace(/^vkCmd/, "")} #${first.command.index}${pipeline ? ` (${pipeline.name})` : ""}`
        : `${pipeline ? pipeline.name : `Pipeline ${bucket.pipelineId}`}: ${count} ${noun}`;
      const itemNode = rollup(node("item", name, 0, stageNodes));
      itemNode.command = first.command;
      itemNode.objectId = bucket.pipelineId;
      if (timedItems) {
        const ms = bucketMs(bucket.items);
        const modeled = itemNode.totalCost;
        if (modeled > 0) scaleSubtree(itemNode, ms / modeled);
        else itemNode.totalCost = ms;
        itemNode.durationMs = ms;
      }
      itemNodes.push(itemNode);
    }
    if (collapsed) {
      const label = o.perDraw ? `+ ${collapsed.count} more draws` : `+ ${collapsed.count} more pipelines (${collapsed.draws} draws)`;
      itemNodes.push(node("more", label, collapsed.cost));
    }

    const passNode = rollup(node("pass", pass.label, 0, itemNodes));
    passNode.command = pass.command ?? undefined;
    passNode.durationMs = pass.durationMs;
    if (units === "ms") {
      // The measured duration is authoritative: the modeled subtree fills exactly that time.
      const modeled = passNode.totalCost;
      if (modeled > 0) scaleSubtree(passNode, pass.durationMs! / modeled);
      else { passNode.children = []; passNode.estimated = true; }
      passNode.totalCost = pass.durationMs!;
    }
    passNodes.push(passNode);
  }

  const root = rollup(node("frame", "Frame", 0, passNodes));
  if (units === "ms") root.name = `Frame: ${root.totalCost.toFixed(2)} ms GPU`;
  if (stats.unknownStages > 0) notes.push(`${stats.unknownStages} shader stage(s) have no invocation count or no analysis and are shown unweighted (zero width).`);
  if (stats.measuredDrawPasses > 0) {
    notes.push(`The draws of ${stats.measuredDrawPasses} pass(es) were timed one at a time by replaying the frame, and those times set how each pass's measured duration is split between them. A draw's time overlaps its neighbours' on the GPU, so it is a share of the pass rather than what the draw costs alone.`);
  }
  if (stats.measuredFragmentPasses > 0) {
    notes.push(`Fragment stages in ${stats.measuredFragmentPasses} pass(es) are weighted by the fragment shader invocations the capture's GPU counters measured; a pass that draws more than once splits its measured total between its draws by scissor area.`);
  }
  if (stats.estimatedStages > 0) notes.push(`Fragment stages without measured counters are weighted by the scissor (or render) area: an upper bound without overdraw and before the depth test, so the split between vertex and fragment work is an estimate.`);
  else if (o.estimateFragments === false && stats.measuredFragmentPasses === 0) notes.push("Fragment stages are unweighted: only rasterization knows their invocation counts. Enable the scissor-area estimate to weight them.");
  if (stats.collapsed > 0) notes.push(`${stats.collapsed} lower-cost ${o.perDraw ? "draw" : "pipeline"} group(s) are collapsed into "+ more" frames (the ${maxFramesPerPass} costliest per pass are shown). Their cost still counts in the pass totals.`);
  return { root, units, notes, stats };
}

/** The color of a cost dimension (also the legend). */
export const DIMENSION_COLORS: Record<CostDimension, string> = { alu: "#4a8db8", sfu: "#c98a3a", texture: "#c0504d", memory: "#7b62c9" };
export const KIND_COLORS: Record<FlameNode["kind"], string> = { frame: "#3f7f5f", pass: "#3f7f5f", item: "#6c7a89", stage: "#4a8db8", function: "#4a8db8", line: "#4a8db8", more: "#555b62" };

export function costVecText(c: CostVec): string {
  return `ALU ${c.alu.toFixed(0)}, SFU ${c.sfu.toFixed(0)}, texture ${c.texture.toFixed(0)}, memory ${c.memory.toFixed(0)}`;
}
