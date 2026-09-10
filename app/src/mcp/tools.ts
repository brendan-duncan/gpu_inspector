// The MCP server's frame-level tools: opening capture files, and the reports over a whole capture
// (the summary, Frame Issues, GPU Bottlenecks, the render graph, two captures compared). The
// command and object tools are command_tools.ts; buffers, textures and shaders resource_tools.ts.
import fs from "node:fs";
import path from "node:path";
import { REFRESH_SOURCE_NOTE, frameBound } from "../renderer/capture_statistics.js";
import {
  BOUND_ADVICE, BOUND_LABEL, HEALTHY_OVERDRAW, LOW_REJECTION_RATE, MICROTRIANGLE_LIMIT, OVERDRAW_LIMIT,
  frameStageVerdict, passAdvice, type PassMetrics,
} from "../renderer/pass_metrics.js";
import type { GraphNode, GraphResource } from "../renderer/render_graph.js";
import { analyzeRenderGraph } from "../renderer/render_graph_analysis.js";
import { pipelineUses } from "../renderer/shader_cache.js";
import { SEVERITY_RANK, type Severity } from "../renderer/vulkan/spirv_analysis.js";
import { recentCaptureFiles, settingsFile, type Capture, type CaptureStore } from "./capture_store.js";
import {
  CAPTURE_PARAM, PAGE_PARAMS, enumArg, findingBrief, jsonResult, optionalInt, page, refText, requireString, round, schema, stringArg, validationBrief,
} from "./describe.js";
import { describeSearchPaths, setSearchPaths, splitPaths } from "./search_paths.js";
import type { ToolDefinition } from "./stdio_server.js";

const SEVERITIES = ["high", "medium", "low", "info"] as const;

function unique(values: number[]): number[] | undefined {
  return values.length ? [...new Set(values)] : undefined;
}

/** The frame timing the capture recorded, with the Frame Bound verdict when its passes were timed. */
function frameTiming(c: Capture): Record<string, unknown> {
  const db = c.db;
  const timings = [...c.data.passTimings.values()];
  let start = Infinity;
  let end = -Infinity;
  for (const t of timings) {
    start = Math.min(start, t.startMs);
    end = Math.max(end, t.startMs + t.durationMs);
  }
  const gpuSpanMs = timings.length ? end - start : 0;
  const bound = timings.length ? frameBound({ frameMs: db.frameTimeMs, refreshMs: db.refreshMs, submitMs: db.submitMs, gpuSpanMs, frames: c.data.frames }) : null;
  return {
    frameMs: round(db.frameTimeMs) || undefined,
    submitMs: round(db.submitMs) || undefined,
    refreshMs: round(db.refreshMs) || undefined,
    refreshSource: db.refreshSource ? REFRESH_SOURCE_NOTE[db.refreshSource] ?? db.refreshSource : undefined,
    frameBoundary: db.frameBoundary || undefined,
    profiled: timings.length > 0,
    gpuPassMs: timings.length ? round(c.metrics.gpuMs) : undefined,
    gpuSpanMs: timings.length ? round(gpuSpanMs) : undefined,
    frameBound: bound ? { verdict: bound.verdict, budgetMs: round(bound.budgetMs), gpuMsPerFrame: round(bound.gpuMs) } : undefined,
  };
}

/** What the capture lacks that would change what can be said about it. */
function captureNotes(c: Capture): string[] {
  const d = c.data;
  const notes: string[] = [];
  if (!d.passTimings.size) {
    notes.push("No pass timings: the capture was taken without \"Profile passes\", so it has no GPU times, no Frame Bound verdict and no GPU Bottlenecks report. Capture again with it on to profile.");
  } else if (!c.metrics.withCounters) {
    notes.push(d.api === "metal"
      ? "The passes carry timestamps only (the GPU exposes no statistic counters through public Metal), so overdraw and fragments per primitive are not measured."
      : "The passes carry timestamps but no pipeline statistics (the device lacks pipelineStatisticsQuery, or the layer could not enable it), so overdraw and fragments per primitive are not measured.");
  }
  const failedImages = d.textures.filter((t) => t.info.error).length;
  if (failedImages) notes.push(`${failedImages} image read-backs failed (list_textures says why).`);
  if (!d.textures.length) notes.push("No render targets or images were read back.");
  const buffers = [...d.buffers.values()];
  const failedBuffers = buffers.filter((b) => b.info.error).length;
  if (failedBuffers) notes.push(`${failedBuffers} buffer read-backs failed.`);
  const truncated = buffers.filter((b) => b.info.originalSize).length;
  if (truncated) notes.push(`${truncated} buffer ranges were cut to the capture's buffer size limit.`);
  return notes;
}

function passBrief(c: Capture, i: number): Record<string, unknown> {
  const p = c.metrics.passes[i];
  return { pass: i, label: c.passName(i), command: p.commandIndex, ms: round(p.durationMs), draws: p.draws, bound: p.bound ?? undefined };
}

/** Everything get_bottlenecks says about one pass. */
function passMeasurements(c: Capture, p: PassMetrics, i: number, gpuMs: number): Record<string, unknown> {
  const problems = passAdvice(p);
  return {
    pass: i, label: c.passName(i), command: p.commandIndex, kind: p.compute ? "compute" : "render",
    ms: round(p.durationMs), shareOfGpu: gpuMs > 0 && p.durationMs !== null ? round(p.durationMs / gpuMs) : undefined,
    vertexMs: round(p.vertexMs), fragmentMs: round(p.fragmentMs),
    draws: p.draws, vertices: p.vertices || undefined, pixels: p.pixels || undefined,
    overdraw: round(p.overdraw), fragmentsPerPrimitive: round(p.fragmentsPerPrimitive), depthRejectRate: round(p.depthRejectRate),
    nsPerVertex: round(p.nsPerVertex), nsPerFragment: round(p.nsPerFragment),
    cycleShare: p.cycleShare ? { vertex: round(p.cycleShare.vertex), fragment: round(p.cycleShare.fragment), target: round(p.cycleShare.target) } : undefined,
    bound: p.bound ?? undefined, boundReason: p.boundReason || undefined,
    problems: problems.length ? problems : undefined,
  };
}

export function captureSummary(c: Capture): Record<string, unknown> {
  const d = c.data;
  const db = c.db;
  const sets = d.sets;
  let draws = 0;
  let dispatches = 0;
  for (const cmd of d.commands) {
    if (sets.DRAW.has(cmd.method)) draws++;
    else if (sets.DISPATCH.has(cmd.method)) dispatches++;
  }
  const passes = c.metrics.passes;
  const findings = c.analysis.findings;
  const bySeverity: Record<string, number> = {};
  for (const f of findings) bySeverity[f.severity] = (bySeverity[f.severity] ?? 0) + 1;
  const [errors, warnings] = db.validationCounts;
  const sampled = d.textures.filter((t) => t.info.kind === "sampled").length;
  const g = c.graph;
  const slowest = passes.map((p, i) => ({ p, i })).filter((x) => x.p.durationMs !== null)
    .sort((a, b) => (b.p.durationMs ?? 0) - (a.p.durationMs ?? 0)).slice(0, 5);
  return {
    capture: c.id, file: c.path, application: c.manifest.source?.name || undefined, api: d.api, savedAt: c.manifest.savedAt,
    frame: d.frame, frames: d.frames,
    counts: {
      commands: d.commands.length, draws, dispatches,
      renderPasses: passes.filter((p) => !p.compute).length, computePasses: passes.filter((p) => p.compute).length,
      objects: db.allObjects.size + db.destroyedObjects.size, pipelinesUsed: pipelineUses(d).size,
      renderTargets: d.textures.length - sampled, sampledImages: sampled, bufferRanges: d.buffers.size,
    },
    timing: frameTiming(c),
    slowestPasses: slowest.length ? slowest.map((x) => passBrief(c, x.i)) : undefined,
    issues: { total: findings.length, bySeverity, top: findings.slice(0, 8).map((f) => findingBrief(c, f)) },
    validation: {
      errors, warnings, total: db.validation.length,
      first: db.validation.length ? db.validation.slice(0, 5).map((v) => validationBrief(c, v, 400)) : undefined,
    },
    renderGraph: {
      passes: g.nodes.length, resources: g.resources.length, externalInputs: g.externalInputs.length, unreadPasses: g.unreadNodes.length,
      criticalPathMs: round(g.criticalPathMs) || undefined, warnings: g.warnings.length ? g.warnings : undefined,
    },
    statistics: Object.fromEntries(c.statistics.sections().map((s) => [s.title, Object.fromEntries(s.rows.filter((r) => r.value).map((r) => [r.label, r.value]))])),
    notes: captureNotes(c),
  };
}

function nodeDetail(c: Capture, n: GraphNode): Record<string, unknown> {
  const db = c.db;
  const resource = (r: GraphResource): Record<string, unknown> => ({ resource: r.label, detail: r.detail || undefined, object: refText(db, r.objectId), key: r.key });
  return {
    capture: c.id, node: n.ordinal, label: n.label, kind: n.kind, command: n.commandIndex, ms: round(n.durationMs), draws: n.draws || undefined,
    pathMs: round(n.pathMs) || undefined, unread: n.unread || undefined, unresolvedReads: n.unresolvedReads || undefined,
    reads: n.reads.map((u) => ({
      ...resource(u.resource), usage: u.usage, version: u.version.index,
      from: u.version.producer ? u.version.producer.ordinal : "before the capture",
    })),
    writes: n.writes.map((u) => ({
      ...resource(u.resource), usage: u.usage, version: u.version.index, readBy: u.version.readers.map((r) => r.ordinal),
      replacesContents: u.discards || undefined, discardedByStoreOp: u.dropped || undefined, presented: u.resource.presented || undefined,
    })),
  };
}

/** A pass's identity across two captures: its kind, the debug groups around it and its own label, numbered by occurrence. */
function passKeys(c: Capture): Map<string, number> {
  const seen = new Map<string, number>();
  const out = new Map<string, number>();
  c.metrics.passes.forEach((p, i) => {
    const base = `${p.compute ? "compute" : "render"}|${c.labelsOf(p.commandIndex)}|${p.label.replace(/^[A-Za-z ]+ \d+:?\s*/, "")}`;
    const n = seen.get(base) ?? 0;
    seen.set(base, n + 1);
    out.set(`${base}#${n}`, i);
  });
  return out;
}

function change(before: number | null | undefined, after: number | null | undefined): Record<string, unknown> | undefined {
  if ((before ?? null) === null && (after ?? null) === null) return undefined;
  const out: Record<string, unknown> = { before: round(before), after: round(after) };
  if (typeof before === "number" && typeof after === "number") {
    out.change = round(after - before);
    if (before) out.percent = round(((after - before) / before) * 100);
  }
  return out;
}

export function captureTools(store: CaptureStore): ToolDefinition[] {
  return [
    {
      name: "open_capture",
      description: "Open a GPU Inspector capture file (.gpucap: a Vulkan or Metal frame saved from GPU Inspector's capture bar) " +
        "and return its summary. The capture stays open under the returned id (\"cap-1\") for the other tools; opening an " +
        "unchanged file again returns the capture already open.",
      inputSchema: schema({ path: { type: "string", description: "Path of the .gpucap file." } }, ["path"]),
      readOnly: true,
      handler: (args) => {
        const { capture, reused } = store.open(requireString(args, "path"));
        return jsonResult({ ...captureSummary(capture), reused: reused || undefined });
      },
    },
    {
      name: "list_captures",
      description: "List the captures open in this server, and the capture files GPU Inspector opened or saved most recently " +
        "(from its settings), so a capture can be found without asking for its path.",
      inputSchema: schema({}),
      readOnly: true,
      handler: () => {
        const open = store.list();
        const recent = [...new Set(recentCaptureFiles().map((p) => path.normalize(p)))];
        return jsonResult({
          open: open.map((c) => ({
            capture: c.id, file: c.path, application: c.manifest.source?.name || undefined, api: c.data.api, frame: c.data.frame,
            frames: c.data.frames > 1 ? c.data.frames : undefined, commands: c.data.commands.length, megabytes: round(c.fileBytes / 1048576),
          })),
          recent: recent.map((file) => ({
            file, missing: fs.existsSync(file) ? undefined : true, open: open.find((c) => c.path === path.resolve(file))?.id,
          })),
          note: recent.length ? undefined : `No recent captures in ${settingsFile()}.`,
        });
      },
    },
    {
      name: "set_search_paths",
      description: "Where to look on this machine for what captures only name. sourceRoots: the directories holding the " +
        "shader sources, for shaders compiled with line information but no embedded text (dxc -Zi, glslc without -g, " +
        "stripped builds), so get_shader shows their source and the analyses quote their costliest lines. symbolDirs: the " +
        "directories holding the application's unstripped libraries (the build tree), so stack frames named only by module " +
        "and offset (Android, Linux) resolve to functions, files and lines. A list replaces the previous one for this " +
        "server; an empty list goes back to GPU_INSPECTOR_SOURCE_ROOTS / GPU_INSPECTOR_SYMBOL_DIRS, else the directories " +
        "GPU Inspector's launch dialog used last. Without arguments it shows what is in effect.",
      inputSchema: schema({
        sourceRoots: { type: "array", items: { type: "string" }, description: "Directories searched (six levels deep) for the shader files debug information names." },
        symbolDirs: { type: "array", items: { type: "string" }, description: "Directories searched (five levels deep) for the libraries stack frames name." },
      }),
      handler: (args) => {
        if (args.sourceRoots !== undefined) setSearchPaths("sourceRoots", splitPaths(args.sourceRoots));
        if (args.symbolDirs !== undefined) setSearchPaths("symbolDirs", splitPaths(args.symbolDirs));
        return jsonResult(describeSearchPaths());
      },
    },
    {
      name: "close_capture",
      description: "Close an open capture and free its memory (captures with many read-back images can be hundreds of megabytes).",
      inputSchema: schema({ capture: { type: "string", description: "The capture's id or file path." } }, ["capture"]),
      handler: (args) => jsonResult({ closed: store.close(requireString(args, "capture")) }),
    },
    {
      name: "get_capture_summary",
      description: "Summarize a capture: counts (commands, draws, passes, objects, read-backs), the frame timing with the Frame " +
        "Bound verdict (GPU bound, CPU bound, vsync bound) when the passes were profiled, the slowest passes, the Frame Issues " +
        "by severity with the top ones, validation messages, the render graph in numbers, frame statistics, and notes on " +
        "what the capture lacks. Start here.",
      inputSchema: schema({ capture: CAPTURE_PARAM }),
      readOnly: true,
      handler: (args) => jsonResult(captureSummary(store.resolve(stringArg(args, "capture")))),
    },
    {
      name: "get_frame_issues",
      description: "The Frame Issues of a capture: the rules GPU Inspector runs over the frame (attachment load and store ops, " +
        "clears outside passes, transient and memoryless candidates, MSAA stores, one pass per eye without multiview, redundant " +
        "binds, barriers, tiny draws, overdraw and microtriangles from the GPU counters, unmipped textures, and the render " +
        "graph's unread stores, overwritten results and mergeable passes). Each finding names the command it is about; a " +
        "finding over many commands names the first and counts the rest. Worst first.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        severity: { type: "string", enum: SEVERITIES, description: "The lowest severity to list (default info: all)." },
        rule: { type: "string", description: "Only this rule, by name (\"tiny-draws\")." },
        ...PAGE_PARAMS,
      }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const min = SEVERITY_RANK[enumArg(args, "severity", SEVERITIES, "info")];
        const rule = stringArg(args, "rule");
        const all = c.analysis.findings;
        const rules: Record<string, { severity: Severity; findings: number; commands: number }> = {};
        for (const f of all) {
          const r = (rules[f.rule] ??= { severity: f.severity, findings: 0, commands: 0 });
          if (SEVERITY_RANK[f.severity] > SEVERITY_RANK[r.severity]) r.severity = f.severity;
          r.findings++;
          r.commands += f.count;
        }
        const list = all.filter((f) => SEVERITY_RANK[f.severity] >= min && (!rule || f.rule === rule));
        const p = page(list, args, 50, 200);
        return jsonResult({ capture: c.id, total: p.total, offset: p.offset, nextOffset: p.nextOffset, rules, findings: p.items.map((f) => findingBrief(c, f)) });
      },
    },
    {
      name: "get_bottlenecks",
      description: "The GPU Bottlenecks report: every timed pass, slowest first, measured the way a bottleneck is described — GPU " +
        "time and share of the frame, draws and vertices, overdraw (fragment shader runs per target pixel), fragments per " +
        "primitive (microtriangles below 4), depth rejection and the vertex/fragment split (Metal), which stage the pass is " +
        "bound by, and each measured problem with what usually causes it. Needs a capture taken with \"Profile passes\"; the " +
        "counters also need a GPU that exposes them. docs/PROFILING.md in GPU Inspector is the method behind it.",
      inputSchema: schema({ capture: CAPTURE_PARAM, ...PAGE_PARAMS }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const m = c.metrics;
        if (!m.passes.length) return jsonResult({ capture: c.id, note: "The capture has no passes." });
        if (!m.timed) {
          return jsonResult({
            capture: c.id, passes: m.passes.length,
            note: "No pass was timed: the capture was taken without \"Profile passes\". The timings and counters this report reads are sampled during the capture and cannot be recovered afterwards; capture again with Profile passes on.",
          });
        }
        const ranked = m.passes.map((p, i) => ({ p, i })).filter((x) => x.p.durationMs !== null)
          .sort((a, b) => (b.p.durationMs ?? 0) - (a.p.durationMs ?? 0));
        const slowest = ranked[0];
        const metal = c.data.api === "metal";
        const notes: string[] = [];
        if (!m.withCounters) {
          notes.push(metal
            ? "The GPU exposes only the timestamp counter set through public Metal, so overdraw, fragments per primitive and depth rejection are not measured."
            : "No pass carried pipeline statistics (the device may lack pipelineStatisticsQuery), so overdraw and fragments per primitive are not measured.");
        } else {
          notes.push(`${m.withCounters} of ${m.timed} timed passes carried counters.`);
        }
        if (!metal) notes.push("The vertex/fragment split and depth rejection are Metal only: Vulkan has no portable stage-boundary timestamps, and pipeline statistics do not count the fragments that survived the depth test.");
        const totals = m.totals;
        const p = page(ranked, args, 30, 200);
        return jsonResult({
          capture: c.id, api: c.data.api, verdict: frameStageVerdict(m),
          gpuMs: round(m.gpuMs), vertexMs: round(m.vertexMs) || undefined, fragmentMs: round(m.fragmentMs) || undefined,
          timedPasses: m.timed, untimedPasses: m.passes.length - m.timed || undefined,
          totals: totals ? {
            vertexInvocations: totals.vertexInvocations, fragmentInvocations: totals.fragmentInvocations, primitives: totals.primitives,
            fragmentsPerPrimitive: totals.primitives ? round(totals.fragmentInvocations / totals.primitives) : undefined,
          } : undefined,
          thresholds: { healthyOverdraw: HEALTHY_OVERDRAW, overdrawFlaggedAbove: OVERDRAW_LIMIT, microtrianglesBelow: MICROTRIANGLE_LIMIT, lowDepthRejectionBelow: LOW_REJECTION_RATE },
          slowest: slowest ? {
            pass: slowest.i, label: c.passName(slowest.i), ms: round(slowest.p.durationMs),
            bound: slowest.p.bound ? BOUND_LABEL[slowest.p.bound] : undefined, reason: slowest.p.boundReason || undefined,
            firstThingToTry: slowest.p.bound ? BOUND_ADVICE[slowest.p.bound] : undefined,
          } : undefined,
          total: p.total, offset: p.offset, nextOffset: p.nextOffset,
          passes: p.items.map((x) => passMeasurements(c, x.p, x.i, m.gpuMs)),
          notes,
        });
      },
    },
    {
      name: "get_render_graph",
      description: "The capture's render graph: its passes (nodes, numbered in execution order) with the passes they read from and " +
        "write for, the critical path by GPU time, resources read from before the capture, passes whose output nothing reads, " +
        "and the graph rules' suggestions. With `node`, one pass in full: every resource it reads (and which pass produced that " +
        "version) and writes (and which passes read it). Node numbers are the graph's own, not get_bottlenecks' pass numbers.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        node: { type: "integer", minimum: 0, description: "One node to show in full." },
        ...PAGE_PARAMS,
      }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const g = c.graph;
        const nodeIndex = optionalInt(args, "node");
        if (nodeIndex !== undefined) {
          const n = g.nodes.find((x) => x.ordinal === nodeIndex);
          if (!n) throw new Error(`No node ${nodeIndex}: the graph has ${g.nodes.length} nodes.`);
          return jsonResult(nodeDetail(c, n));
        }
        const critical = new Set(g.criticalPath.map((n) => n.ordinal));
        const suggestions = analyzeRenderGraph(g).findings;
        const p = page(g.nodes, args, 100, 500);
        return jsonResult({
          capture: c.id, nodes: g.nodes.length, resources: g.resources.length, edges: g.edges.length,
          criticalPath: g.criticalPath.map((n) => n.ordinal), criticalPathMs: round(g.criticalPathMs) || undefined,
          warnings: g.warnings.length ? g.warnings : undefined,
          externalInputs: g.externalInputs.length ? g.externalInputs.slice(0, 40).map((r) => (r.detail ? `${r.label} (${r.detail})` : r.label)) : undefined,
          unreadNodes: unique(g.unreadNodes.map((n) => n.ordinal)),
          suggestions: suggestions.length ? suggestions.map((f) => findingBrief(c, f)) : undefined,
          offset: p.offset, nextOffset: p.nextOffset,
          list: p.items.map((n) => ({
            node: n.ordinal, label: n.label, kind: n.kind, command: n.commandIndex, frame: c.data.frames > 1 ? n.frame : undefined,
            ms: round(n.durationMs), draws: n.draws || undefined, reads: n.reads.length, writes: n.writes.length,
            inputsFrom: unique(n.inputs.map((e) => e.from.ordinal)), outputsTo: unique(n.outputs.map((e) => e.to.ordinal)),
            unread: n.unread || undefined, critical: critical.has(n.ordinal) || undefined, unresolvedReads: n.unresolvedReads || undefined,
          })),
        });
      },
    },
    {
      name: "compare_captures",
      description: "Compare two captures of the same application, before and after a change: frame, submit and GPU time, the " +
        "statistics that differ, Frame Issues by rule, validation counts, and per pass (matched by debug groups and label) the " +
        "GPU time, draws, overdraw and fragments per primitive, largest change first. Confirms whether a fix moved anything.",
      inputSchema: schema({
        before: { type: "string", description: "The capture before the change: an open capture's id or a .gpucap path." },
        after: { type: "string", description: "The capture after the change: an open capture's id or a .gpucap path." },
      }, ["before", "after"]),
      readOnly: true,
      handler: (args) => {
        const a = store.resolve(requireString(args, "before"));
        const b = store.resolve(requireString(args, "after"));
        const sa = a.statistics;
        const sb = b.statistics;
        const counts: Record<string, unknown> = {};
        const keys = ["apiCalls", "submits", "draws", "dispatches", "renderPasses", "computePasses", "bindPipeline", "uniquePipelines",
          "descriptorSetsBound", "uniqueDescriptorSets", "totalVertices", "totalTriangles", "updateBufferBytes", "bufferCopyBytes"] as const;
        for (const key of keys) if (sa[key] !== sb[key]) counts[key] = change(sa[key], sb[key]);

        const ka = passKeys(a);
        const kb = passKeys(b);
        const rows: { label: string; ms?: Record<string, unknown>; [key: string]: unknown }[] = [];
        for (const [key, ia] of ka) {
          const ib = kb.get(key);
          if (ib === undefined) continue;
          const pa = a.metrics.passes[ia];
          const pb = b.metrics.passes[ib];
          rows.push({
            label: b.passName(ib), before: ia, after: ib, ms: change(pa.durationMs, pb.durationMs),
            draws: pa.draws !== pb.draws ? change(pa.draws, pb.draws) : undefined,
            overdraw: change(pa.overdraw, pb.overdraw), fragmentsPerPrimitive: change(pa.fragmentsPerPrimitive, pb.fragmentsPerPrimitive),
          });
        }
        const magnitude = (r: { ms?: Record<string, unknown> }): number => Math.abs(typeof r.ms?.change === "number" ? r.ms.change : 0);
        rows.sort((x, y) => magnitude(y) - magnitude(x));

        const ruleCounts = (c: Capture): Map<string, number> => {
          const out = new Map<string, number>();
          for (const f of c.analysis.findings) out.set(f.rule, (out.get(f.rule) ?? 0) + f.count);
          return out;
        };
        const ra = ruleCounts(a);
        const rb = ruleCounts(b);
        const issues = [...new Set([...ra.keys(), ...rb.keys()])]
          .map((rule) => ({ rule, before: ra.get(rule) ?? 0, after: rb.get(rule) ?? 0 }))
          .filter((r) => r.before !== r.after);
        const profiled = a.data.passTimings.size > 0 && b.data.passTimings.size > 0;
        const [ea, wa] = a.db.validationCounts;
        const [eb, wb] = b.db.validationCounts;
        return jsonResult({
          before: { capture: a.id, file: a.path, frame: a.data.frame },
          after: { capture: b.id, file: b.path, frame: b.data.frame },
          timing: {
            frameMs: change(a.db.frameTimeMs, b.db.frameTimeMs), submitMs: change(a.db.submitMs, b.db.submitMs),
            gpuPassMs: profiled ? change(a.metrics.gpuMs, b.metrics.gpuMs) : undefined,
          },
          counts: Object.keys(counts).length ? counts : undefined,
          issuesByRule: issues.length ? issues : undefined,
          validation: ea !== eb || wa !== wb ? { errors: change(ea, eb), warnings: change(wa, wb) } : undefined,
          passes: rows.slice(0, 60),
          onlyBefore: [...ka].filter(([key]) => !kb.has(key)).map(([, i]) => a.passName(i)),
          onlyAfter: [...kb].filter(([key]) => !ka.has(key)).map(([, i]) => b.passName(i)),
          notes: profiled ? undefined : ["At least one capture was taken without Profile passes, so GPU times cannot be compared."],
        });
      },
    },
  ];
}
