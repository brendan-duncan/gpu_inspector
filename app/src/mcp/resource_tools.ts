// The MCP server's resource tools: the images and buffer ranges a capture read back, the vertices
// a draw read, and shaders (reflection, embedded source, cross-compiled text, static analysis).
import { shaderText } from "../main/shader_tools.js";
import { drawState, vertexLayout } from "../renderer/draw_state.js";
import { buildFrameCostTree, type FlameNode, type StageModel } from "../renderer/frame_cost_tree.js";
import { metalStages } from "../renderer/metal/reflection.js";
import { pipelineStages, pipelineUses } from "../renderer/shader_cache.js";
import { layoutText, parseLayout } from "../renderer/vulkan/buffer_layout.js";
import { SEVERITY_RANK, analyzeSpirvCached, weighCost, type CostVec } from "../renderer/vulkan/spirv_analysis.js";
import { describeDebugInfo, hasEmbeddedSource } from "../renderer/vulkan/spirv_debug.js";
import type { ReflType, ShaderReflection, ShaderResource, ShaderVariable, StructType } from "../renderer/vulkan/spirv_reflect.js";
import { decodeTexels, displayTexels, isFormatSupported, sliceBytes, type TexelData } from "../renderer/vulkan/texture_decode.js";
import { vertexFormat } from "../renderer/vulkan/vk_format.js";
import { num, str, type VulkanObject } from "../renderer/vulkan/vulkan_object.js";
import type { Capture, CaptureStore } from "./capture_store.js";
import { indexSize, readIndex, vertexInputs, vertexStruct } from "./command_tools.js";
import {
  CAPTURE_PARAM, PAGE_PARAMS, boolArg, clip, enumArg, intArg, jsonResult, numberArg, optionalInt, page, readTyped, refText, requireInt, round,
  schema, stringArg, textureBrief, tidy,
} from "./describe.js";
import { encodePng, fitPixels } from "./png.js";
import { codeAt, debugInfoWithSources, searchPaths, sourceLineTexts, type LineTexts } from "./search_paths.js";
import type { ToolArgs, ToolDefinition, ToolResult } from "./stdio_server.js";

const COST_MODEL = "Modeled cost of one invocation, not a measurement: instructions weighted ALU 1, special functions 4, texture 20, memory 8, with loops counted as 8 iterations per nesting level. It ranks shaders and functions against each other.";
const FLAME_MS = "Milliseconds. Each pass is its measured GPU time; the split inside a pass is modeled (each stage's modeled cost times its invocations), so compare frames inside a pass with each other rather than with the clock.";
const FLAME_OPS = "Modeled op units (each stage's modeled cost times its invocations): they rank frames against each other and are not time. A capture with Profile passes scales each pass to its measured milliseconds.";
const SHADER_VIEWS = ["reflection", "source", "analysis", "glsl", "hlsl", "msl", "disassembly"] as const;
const CHANNELS = ["rgb", "r", "g", "b", "a", "luminance"] as const;
const SCALAR_BYTES = { float32: 4, uint32: 4, int32: 4, uint16: 2, int16: 2, uint8: 1 } as const;

/** How read_texture and read_live_image show an image. */
export const IMAGE_PARAMS = {
  channels: { type: "string", enum: CHANNELS, description: "What the image shows (default rgb)." },
  exposure: { type: "number", description: "Multiplier applied before display (default 1)." },
  autoRange: { type: "boolean", description: "Stretch the value range to black..white (default on for depth and integer formats)." },
  image: { type: "boolean", description: "Return the PNG (default true); false for the numbers alone." },
  maxSize: { type: "integer", minimum: 16, maximum: 2048, description: "Longest side of the returned image in pixels (default 512)." },
  texels: { type: "array", items: { type: "array", items: { type: "integer" }, minItems: 2, maxItems: 2 }, description: "[x, y] texel coordinates to read exactly (up to 64)." },
};

interface ShaderSource { stage: string; entryPoint: string; object: VulkanObject; blobIndex: number }

function texelStats(tex: TexelData): Record<string, unknown>[] {
  const n = tex.width * tex.height;
  const out: Record<string, unknown>[] = [];
  for (let ch = 0; ch < tex.channels; ch++) {
    let sum = 0;
    let finite = 0;
    let nan = 0;
    let infinite = 0;
    let zero = 0;
    for (let i = 0; i < n; i++) {
      const v = tex.values[i * 4 + ch];
      if (Number.isNaN(v)) {
        nan++;
      } else if (!Number.isFinite(v)) {
        infinite++;
      } else {
        sum += v;
        finite++;
        if (v === 0) zero++;
      }
    }
    out.push({
      channel: tex.names[ch], min: tidy(tex.min[ch]), max: tidy(tex.max[ch]), mean: finite ? tidy(sum / finite) : undefined,
      zeroFraction: round(zero / n) || undefined, nan: nan || undefined, infinite: infinite || undefined,
    });
  }
  return out;
}

function texel(tex: TexelData, x: number, y: number): number[] {
  const o = (y * tex.width + x) * 4;
  const v: number[] = [];
  for (let ch = 0; ch < tex.channels; ch++) v.push(tidy(tex.values[o + ch]));
  return v;
}

/** A decoded image as the image tools answer: its numbers, a 3x3 grid of texel values, the texels asked for, and the PNG unless `image` is false. */
export function texelAnswer(tex: TexelData, args: ToolArgs, head: Record<string, unknown>, aspect: string): ToolResult {
  const grid: Record<string, unknown>[] = [];
  for (let gy = 0; gy < 3; gy++) {
    for (let gx = 0; gx < 3; gx++) {
      const x = Math.min(tex.width - 1, Math.floor(((gx + 0.5) * tex.width) / 3));
      const y = Math.min(tex.height - 1, Math.floor(((gy + 0.5) * tex.height) / 3));
      grid.push({ x, y, value: texel(tex, x, y) });
    }
  }
  const requested = Array.isArray(args.texels) ? args.texels.slice(0, 64) : [];
  const texels = requested.map((pt) => {
    const x = Array.isArray(pt) ? Number(pt[0]) : NaN;
    const y = Array.isArray(pt) ? Number(pt[1]) : NaN;
    if (!(x >= 0 && x < tex.width && y >= 0 && y < tex.height)) throw new Error(`texel [${String(pt)}] is outside the ${tex.width}x${tex.height} image.`);
    return { x: Math.floor(x), y: Math.floor(y), value: texel(tex, Math.floor(x), Math.floor(y)) };
  });
  const stats = texelStats(tex);
  const uniform = tex.min.slice(0, tex.channels).every((v, ch) => v === tex.max[ch]);
  const result = jsonResult({
    ...head, width: tex.width, height: tex.height,
    channels: tex.names, linear: tex.linear || undefined, integer: tex.integer || undefined,
    uniform: uniform || undefined, stats, sampleGrid: grid, texels: texels.length ? texels : undefined,
  });
  if (boolArg(args, "image", true)) {
    const rgba = displayTexels(tex, {
      channels: enumArg(args, "channels", CHANNELS, "rgb"),
      exposure: numberArg(args, "exposure") ?? 1,
      autoRange: boolArg(args, "autoRange", aspect !== "color" || tex.integer),
    });
    const fit = fitPixels(rgba, tex.width, tex.height, intArg(args, "maxSize", 512, 16, 2048));
    result.content.unshift({ type: "image", data: Buffer.from(encodePng(fit.rgba, fit.width, fit.height)).toString("base64"), mimeType: "image/png" });
  }
  return result;
}

/** The commands that bound a captured buffer range (by its data id), the first eight. */
function usersOf(c: Capture, dataId: number): number[] {
  const out: number[] = [];
  for (const cmd of c.data.commands) {
    if (out.length >= 8) break;
    if (cmd.bufferData?.includes(dataId) || cmd.descriptors?.sets.some((s) => s.bindings.some((b) => b.descriptors.some((d) => d?.data === dataId)))) out.push(cmd.index);
  }
  return out;
}

/** Per attribute, the smallest and largest value of each component over the captured vertices, and the NaNs. */
function attributeBounds(element: StructType, view: DataView, stride: number, vertices: number): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  const n = Math.min(vertices, 1_000_000);
  for (const m of element.members) {
    if (m.type.kind !== "format") continue;
    const f = vertexFormat(m.type.format);
    if (!f) continue;
    const min = new Array<number>(f.channels.length).fill(Infinity);
    const max = new Array<number>(f.channels.length).fill(-Infinity);
    let nan = 0;
    for (let v = 0; v < n; v++) {
      const at = v * stride + m.offset;
      if (at + f.size > view.byteLength) break;
      const values = f.read(view, at);
      for (let k = 0; k < values.length; k++) {
        const x = values[k];
        if (Number.isNaN(x)) {
          nan++;
        } else {
          if (x < min[k]) min[k] = x;
          if (x > max[k]) max[k] = x;
        }
      }
    }
    out[m.name] = { min: min.map(tidy), max: max.map(tidy), nan: nan || undefined };
  }
  return out;
}

function roundCost(c: CostVec): Record<string, number | undefined> {
  return { alu: round(c.alu), sfu: round(c.sfu), texture: round(c.texture), memory: round(c.memory) };
}

function reflectionDetail(r: ShaderReflection | null): Record<string, unknown> {
  if (!r) return { note: "The SPIR-V could not be reflected." };
  const variable = (v: ShaderVariable): Record<string, unknown> => ({ location: v.location, name: v.name || undefined, type: v.typeName });
  const resource = (res: ShaderResource): Record<string, unknown> => ({
    set: res.set, binding: res.binding, kind: res.kind, name: res.name || undefined, type: res.typeName, count: res.count !== 1 ? res.count : undefined,
    readOnly: res.readOnly || undefined, writeOnly: res.writeOnly || undefined, layout: res.type.kind === "struct" ? layoutText(res.type) : undefined,
  });
  return {
    spirvVersion: r.version || undefined,
    entryPoints: r.entryPoints.map((e) => ({ name: e.name, stage: e.stage, workgroupSize: e.workgroupSize ?? undefined, inputs: e.inputs.map(variable), outputs: e.outputs.map(variable) })),
    resources: r.resources.map(resource),
    pushConstants: r.pushConstants.length ? r.pushConstants.map(resource) : undefined,
  };
}

function sourceDetail(spirv: Uint8Array, maxChars: number): Record<string, unknown> {
  const { info, found } = debugInfoWithSources(spirv);
  if (!info || !hasEmbeddedSource(info)) {
    const named = (info?.files ?? []).map((f) => f.name).filter(Boolean);
    const roots = searchPaths("sourceRoots").dirs;
    return {
      debugInfo: describeDebugInfo(info),
      note: named.length
        ? `The debug information names ${named.join(", ")} without the text, and ${roots.length ? `the source roots (${roots.join("; ")}) do not hold it` : "no source roots are set"}: set_search_paths with the directory holding the shader sources finds it. The "glsl" or "hlsl" view cross-compiles the SPIR-V instead.`
        : "No source is embedded. Compiling with -g (glslc, glslangValidator), -gVS (glslangValidator) or -fspv-debug=vulkan-with-source (dxc) embeds it; the \"glsl\" or \"hlsl\" view cross-compiles the SPIR-V instead.",
    };
  }
  let left = maxChars;
  const files = info.files.map((f, i) => ({ f, i })).filter((x) => x.f.text !== null).map(({ f, i }) => {
    const text = clip(f.text!, Math.max(200, left));
    left -= f.text!.length;
    return { name: f.name || undefined, main: i === info.mainFile || undefined, fromThisMachine: f.fromHost || undefined, text };
  });
  return { language: info.language, debugInfo: describeDebugInfo(info), foundOnThisMachine: found.length ? found : undefined, files };
}

function analysisDetail(spirv: Uint8Array, entryPoint: string): Record<string, unknown> {
  const a = analyzeSpirvCached(spirv);
  if (!a) return { note: "The SPIR-V could not be analyzed." };
  const texts = sourceLineTexts(debugInfoWithSources(spirv).info);
  const named = a.entryPoints.filter((e) => e.name === entryPoint);
  const entries = named.length ? named : a.entryPoints;
  const lines = a.functions.flatMap((f) => f.lines.map((l) => ({ fn: f.name, l }))).sort((x, y) => y.l.weighted - x.l.weighted).slice(0, 12);
  const findings = [...a.findings].sort((x, y) => SEVERITY_RANK[y.severity] - SEVERITY_RANK[x.severity]);
  return {
    model: COST_MODEL,
    entryPoints: entries.map((e) => ({
      name: e.name, stage: e.stage, cost: round(e.weighted), dominant: e.dominant, breakdown: roundCost(e.cost),
      functions: e.functions.slice(0, 10).map((f) => ({
        name: f.name, inclusive: round(weighCost(f.inclusive)), own: round(weighCost(f.cost)), loops: f.loops || undefined, branches: f.branches || undefined,
      })),
    })),
    findings: findings.map((f) => ({
      rule: f.rule, severity: f.severity, confidence: f.confidence, function: f.function,
      line: f.line ? (f.file ? `${f.file}:${f.line}` : String(f.line)) : undefined, loopDepth: f.loopDepth || undefined, count: f.count > 1 ? f.count : undefined,
      code: codeAt(texts, f.file, f.line), message: f.message,
    })),
    costliestLines: a.hasLines ? lines.map(({ fn, l }) => ({ line: `${l.file}:${l.line}`, code: codeAt(texts, l.file, l.line), function: fn, cost: round(l.weighted), dominant: l.dominant })) : undefined,
    totals: a.totals,
  };
}

function utf8Text(data: Uint8Array): string | null {
  for (const b of data.subarray(0, 4096)) if (b === 0) return null;
  return new TextDecoder().decode(data);
}

/** A Metal library's source (a function's scrolled to its definition), or a Metal pipeline's reflection. */
function metalShader(c: Capture, o: VulkanObject, view: string, maxChars: number): Record<string, unknown> {
  const db = c.db;
  if (o.type === "MTLRenderPipelineState" || o.type === "MTLComputePipelineState") {
    const resource = (r: ShaderResource): Record<string, unknown> => ({
      index: r.binding, name: r.name || undefined, type: r.typeName, access: r.readOnly ? "read" : r.writeOnly ? "write" : "read_write",
      layout: r.type.kind === "struct" ? layoutText(r.type) : undefined,
    });
    return {
      capture: c.id, object: refText(db, o.id),
      functions: [...o.dependencies].filter((x) => x.type === "MTLFunction").map((x) => refText(db, x.id)),
      stages: metalStages(o).map((s) => ({ stage: s.stage, buffers: [...s.buffers.values()].map(resource), textures: [...s.textures.values()].map(resource), samplers: [...s.samplers.values()].map(resource) })),
      note: view === "reflection" ? undefined : "A Metal pipeline's code is its functions': get_shader on one of its MTLFunction objects shows the source.",
    };
  }
  const library = o.type === "MTLFunction" ? db.getObject(o.parentId) : o.type === "MTLLibrary" ? o : null;
  if (!library || library.type !== "MTLLibrary") {
    throw new Error(`${refText(db, o.id)} has no shader code: get_shader takes a VkPipeline, a VkShaderModule, an MTLLibrary, an MTLFunction or a Metal pipeline state.`);
  }
  const fn = o.type === "MTLFunction" ? str(o.args?.name) : "";
  const payloads = library.blobs.map((b, i) => {
    const data = db.blobData.get(`${library.id}:${i}`);
    if (!data) return { name: b.name, bytes: b.size, note: "Not in the capture file." };
    const text = utf8Text(data);
    if (text === null) return { name: b.name, bytes: data.byteLength, note: "A compiled metallib: the application did not build this library from source, so there is none." };
    if (fn) {
      const lines = text.split("\n");
      const pattern = new RegExp(`\\b${fn.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\s*\\(`);
      const at = lines.findIndex((l) => pattern.test(l));
      if (at >= 0) {
        return {
          name: b.name, function: fn, line: at + 1, text: clip(lines.slice(Math.max(0, at - 8), at + 150).join("\n"), maxChars),
          note: "From eight lines above the function's definition; get_shader on the MTLLibrary shows the whole source.",
        };
      }
    }
    return { name: b.name, text: clip(text, maxChars) };
  });
  return {
    capture: c.id, object: refText(db, o.id), library: refText(db, library.id),
    functionNames: Array.isArray(library.args?.functionNames) ? library.args.functionNames : undefined, payloads,
  };
}

/** The stages of every pipeline the frame bound, analyzed from the capture's SPIR-V, for the flame graph. */
function stageModels(c: Capture): { models: Map<number, StageModel[]>; spirv: Map<string, Uint8Array> } {
  const db = c.db;
  const models = new Map<number, StageModel[]>();
  /** Each stage's SPIR-V by "object|stage", for the code of its lines. */
  const spirv = new Map<string, Uint8Array>();
  for (const pipelineId of pipelineUses(c.data).keys()) {
    const pipeline = db.getObject(pipelineId);
    if (!pipeline) continue;
    models.set(pipelineId, pipelineStages(pipeline, db).map((s) => {
      const bytes = c.spirv(s.object, s.blobIndex);
      if (bytes) spirv.set(`${s.object.id}|${s.stage}`, bytes);
      // Compute invocations are the dispatched groups times the workgroup size, from reflection.
      const reflection = s.stage === "compute" ? c.reflection(s.object, s.blobIndex) : null;
      const entry = reflection?.entryPoints.find((e) => e.name === s.entryPoint) ?? reflection?.entryPoints[0] ?? null;
      return {
        stage: s.stage, entryPoint: s.entryPoint, objectId: s.object.id,
        analysis: bytes ? analyzeSpirvCached(bytes) : null, workgroupSize: entry?.workgroupSize ?? null,
      };
    }));
  }
  return { models, spirv };
}

interface FlameView { c: Capture; total: number; depth: number; minShare: number }

function shareOf(cost: number, total: number): number | undefined {
  return total > 0 ? round(cost / total) : undefined;
}

/** A flame graph frame with its children, costliest first, down to `depth`; children below `minShare` fold into one. */
function flameFrame(v: FlameView, n: FlameNode, level: number): Record<string, unknown> {
  const db = v.c.db;
  const out: Record<string, unknown> = { kind: n.kind, name: n.name, cost: round(n.totalCost), share: shareOf(n.totalCost, v.total) };
  if (n.kind === "pass") {
    const pass = n.command ? v.c.passOf(n.command.index) : -1;
    if (pass >= 0) {
      out.pass = pass;
      out.name = v.c.passName(pass);
    }
    out.command = n.command?.index;
    out.measuredMs = round(n.durationMs);
    if (!n.children.length && n.totalCost > 0) out.note = "Nothing in this pass could be weighed: it has no draws or dispatches, or their shaders have no analysis.";
  } else if (n.kind === "item") {
    out.command = n.command?.index;
    out.pipeline = refText(db, n.objectId);
  } else if (n.kind === "stage") {
    if (n.stage) out.name = `${n.stage}: ${n.entryPoint}`;
    out.shader = refText(db, n.objectId);
    out.invocations = n.invocations;
    out.invocationCount = n.confidence;
    out.unweighted = n.reason;
  } else if (n.kind === "function") {
    out.own = round(n.selfCost) || undefined;
  }
  out.dominant = n.dimension;
  if (!n.children.length) return out;
  if (level >= v.depth) {
    out.hiddenChildren = n.children.length;
    return out;
  }
  const sorted = [...n.children].sort((x, y) => y.totalCost - x.totalCost);
  const kept = sorted.filter((ch) => v.total <= 0 || ch.totalCost / v.total >= v.minShare);
  const folded = sorted.slice(kept.length);
  const children = kept.map((ch) => flameFrame(v, ch, level + 1));
  const foldedCost = folded.reduce((sum, ch) => sum + ch.totalCost, 0);
  // A fold under a thousandth of the total is noise (a vertex stage beside its fragment stage): left out.
  if (folded.length && (v.total <= 0 || foldedCost / v.total >= 0.001)) {
    const cost = foldedCost;
    children.push({ kind: "other", name: `${folded.length} smaller frame${folded.length === 1 ? "" : "s"}`, cost: round(cost), share: shareOf(cost, v.total) });
  }
  out.children = children;
  return out;
}

/** The costliest functions (own cost) and source lines under a frame, summed over every path that reaches them, and the stages left unweighted. */
function flameHotspots(
  c: Capture, root: FlameNode, total: number, top: number,
  codeOf: (object: number | undefined, stage: string | undefined, file: string | undefined, line: number | undefined) => string | undefined,
): Record<string, unknown> {
  interface Spot { function: string; stage?: string; object?: number; line?: string; file?: string; lineNo?: number; cost: number }
  const functions = new Map<string, Spot>();
  const lines = new Map<string, Spot>();
  const unweighted = new Map<string, Record<string, unknown>>();
  const add = (map: Map<string, Spot>, key: string, spot: Omit<Spot, "cost">, cost: number): void => {
    const s = map.get(key) ?? { ...spot, cost: 0 };
    s.cost += cost;
    map.set(key, s);
  };
  const walk = (n: FlameNode, fn: string): void => {
    let name = fn;
    if (n.kind === "stage" || n.kind === "function") {
      name = n.kind === "stage" ? n.entryPoint ?? "" : n.name;
      if (n.selfCost > 0) add(functions, `${n.objectId}|${n.stage}|${name}`, { function: name, stage: n.stage, object: n.objectId }, n.selfCost);
      if (n.reason) {
        unweighted.set(`${n.objectId}|${n.stage}|${name}`, { stage: n.stage, entryPoint: n.entryPoint, shader: refText(c.db, n.objectId), reason: n.reason });
      }
    } else if (n.kind === "line") {
      add(lines, `${n.objectId}|${n.stage}|${n.name}`, { function: fn, stage: n.stage, object: n.objectId, line: n.name, file: n.file, lineNo: n.line }, n.totalCost);
    }
    for (const ch of n.children) walk(ch, name);
  };
  walk(root, "");
  const ranked = (map: Map<string, Spot>): Record<string, unknown>[] => [...map.values()].sort((x, y) => y.cost - x.cost).slice(0, top).map((s) => ({
    function: s.function, line: s.line, code: s.lineNo ? codeOf(s.object, s.stage, s.file, s.lineNo) : undefined,
    stage: s.stage, shader: refText(c.db, s.object), cost: round(s.cost), share: shareOf(s.cost, total),
  }));
  const f = ranked(functions);
  const l = ranked(lines);
  return {
    hottestFunctions: f.length ? f : undefined,
    hottestLines: l.length ? l : undefined,
    unweightedStages: unweighted.size ? [...unweighted.values()].slice(0, 20) : undefined,
  };
}

export function resourceTools(store: CaptureStore): ToolDefinition[] {
  return [
    {
      name: "list_textures",
      description: "List the images a capture read back: every render pass attachment at the end of its pass (kind attachment, " +
        "with the pass number) and the images bound through descriptor sets (kind sampled), with format, size, mips and " +
        "layers, and why a read-back failed. The texture numbers are what read_texture takes.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        kind: { type: "string", enum: ["all", "attachment", "sampled"], description: "Which read-backs (default all)." },
        pass: { type: "integer", minimum: 0, description: "Only the attachments of this pass." },
        image: { type: "integer", minimum: 0, description: "Only read-backs of this image object id." },
        ...PAGE_PARAMS,
      }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const kind = enumArg(args, "kind", ["all", "attachment", "sampled"] as const, "all");
        const pass = optionalInt(args, "pass");
        const image = optionalInt(args, "image");
        const list = c.data.textures.filter((t) => (kind === "all" || (kind === "sampled") === (t.info.kind === "sampled"))
          && (image === undefined || t.info.id === image) && (pass === undefined || c.passOfTexture(t.info) === pass));
        const p = page(list, args, 100, 500);
        return jsonResult({ capture: c.id, total: p.total, offset: p.offset, nextOffset: p.nextOffset, textures: p.items.map((t) => textureBrief(c, t)) });
      },
    },
    {
      name: "read_texture",
      description: "Look at a read-back image: returns it as a PNG (scaled to fit maxSize, displayed like GPU Inspector's image " +
        "viewer: linear data sRGB-encoded, depth auto-ranged) together with per-channel minimum, maximum and mean, the share " +
        "of zero texels, NaN and infinity counts, a 3x3 grid of sampled texel values, and exact values at requested texels. " +
        "Use it to see what a pass rendered and to find where a rendering problem appears.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        texture: { type: "integer", minimum: 0, description: "The texture number from list_textures or get_command's renderTargets." },
        mip: { type: "integer", minimum: 0, description: "Mip level, for sampled images read back with their mips (default the first read back)." },
        layer: { type: "integer", minimum: 0, description: "Array layer or 3D slice (default 0)." },
        ...IMAGE_PARAMS,
      }, ["texture"]),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const d = c.data;
        const index = requireInt(args, "texture");
        const t = d.textures[index];
        if (!t) throw new Error(`No texture ${index}: ${c.id} read back ${d.textures.length} images (list_textures).`);
        const info = t.info;
        const brief = textureBrief(c, t);
        if (info.error) return jsonResult({ ...brief, note: "The read-back failed, so there are no pixels." });
        if (!t.data) return jsonResult({ ...brief, note: "The capture has no pixel data for this image." });
        // The data holds every read-back mip back to back, each with all its layers or slices (image_view.ts).
        const mips = Math.max(1, info.mips ?? 1);
        const mip = intArg(args, "mip", info.mip, info.mip, info.mip + mips - 1);
        const dims = (m: number): { width: number; height: number; depth: number } => ({
          width: Math.max(1, info.width >> (m - info.mip)), height: Math.max(1, info.height >> (m - info.mip)), depth: Math.max(1, (info.depth || 1) >> (m - info.mip)),
        });
        const layers = Math.max(1, info.layers || 1);
        const bytesOf = (m: number): number => {
          const dd = dims(m);
          return sliceBytes({ format: info.format, aspect: info.aspect, width: dd.width, height: dd.height }) * Math.max(dd.depth, layers);
        };
        let offset = 0;
        for (let m = info.mip; m < mip; m++) offset += bytesOf(m);
        const size = dims(mip);
        const imageInfo = { format: info.format, aspect: info.aspect, width: size.width, height: size.height };
        if (!isFormatSupported(imageInfo)) return jsonResult({ ...brief, note: `Decoding ${info.format} is not supported.` });
        const slices = Math.max(size.depth, layers);
        const layer = intArg(args, "layer", 0, 0, slices - 1);
        const tex = decodeTexels(imageInfo, t.data.subarray(offset, offset + bytesOf(mip)), layer);
        if (!tex) return jsonResult({ ...brief, note: "The pixel data is shorter than the image's size says." });
        return texelAnswer(tex, args, { ...brief, mip, layer, slices: slices > 1 ? slices : undefined }, info.aspect);
      },
    },
    {
      name: "read_buffer",
      description: "Read a buffer range the capture read back (its data id from get_command: a descriptor binding, vertex, index " +
        "or indirect buffer) as numbers of one scalar type, as hex, or through a GLSL struct layout (std140 or std430 offsets), " +
        "optionally as an array of that struct. Also says which commands bound the range. Captured ranges stop at the " +
        "capture's buffer size limit (64 KB by default); truncatedFrom gives the bound size.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        data: { type: "integer", minimum: 1, description: "The captured range's data id." },
        as: { type: "string", enum: [...Object.keys(SCALAR_BYTES), "hex"], description: "Scalar type to read the bytes as when no layout is given (default float32)." },
        layout: { type: "string", description: "GLSL struct declarations; the last struct is the type: \"struct Light { vec4 position; vec4 color; }; struct Lights { Light lights[8]; uint count; };\"." },
        rules: { type: "string", enum: ["std140", "std430"], description: "Layout rules for `layout` (default std430; uniform blocks are std140)." },
        count: { type: "integer", minimum: 1, description: "With a layout: read this many structs one after another. Without: how many scalars (default 256) or bytes of hex." },
        offset: { type: "integer", minimum: 0, description: "Byte offset into the captured range (default 0)." },
      }, ["data"]),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const id = requireInt(args, "data");
        const b = c.data.buffer(id);
        if (!b) throw new Error(`No captured buffer range ${id} in ${c.id}: get_command lists the data ids of a command's bindings.`);
        const base = {
          capture: c.id, data: id, buffer: refText(c.db, b.info.buffer), bufferOffset: b.info.offset, capturedBytes: b.data?.byteLength ?? 0,
          truncatedFrom: b.info.originalSize, boundBy: usersOf(c, id),
        };
        if (b.info.error) return jsonResult({ ...base, captureError: b.info.error });
        const bytes = b.data ?? new Uint8Array(0);
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const offset = intArg(args, "offset", 0, 0, bytes.byteLength);
        const layout = stringArg(args, "layout");
        if (layout) {
          const struct = parseLayout(layout, enumArg(args, "rules", ["std140", "std430"] as const, "std430"));
          const count = optionalInt(args, "count");
          const type: ReflType = count ? { kind: "array", element: struct, count, stride: struct.size, size: count * struct.size } : struct;
          return jsonResult({ ...base, offset, layout: layoutText(struct), structBytes: struct.size, values: readTyped(type, view, offset, { values: 8192 }) });
        }
        const as = enumArg(args, "as", ["float32", "uint32", "int32", "uint16", "int16", "uint8", "hex"] as const, "float32");
        if (as === "hex") {
          const end = Math.min(bytes.byteLength, offset + intArg(args, "count", 256, 1, 16384));
          const lines: string[] = [];
          for (let at = offset; at < end; at += 16) {
            lines.push(`${at.toString(16).padStart(6, "0")}: ${Buffer.from(bytes.subarray(at, Math.min(end, at + 16))).toString("hex").replace(/(..)(?!$)/g, "$1 ")}`);
          }
          return jsonResult({ ...base, offset, hex: lines });
        }
        const size = SCALAR_BYTES[as];
        const count = Math.min(intArg(args, "count", 256, 1, 16384), Math.floor((bytes.byteLength - offset) / size));
        const values: number[] = [];
        for (let i = 0; i < count; i++) {
          const at = offset + i * size;
          switch (as) {
            case "float32": values.push(tidy(view.getFloat32(at, true))); break;
            case "uint32": values.push(view.getUint32(at, true)); break;
            case "int32": values.push(view.getInt32(at, true)); break;
            case "uint16": values.push(view.getUint16(at, true)); break;
            case "int16": values.push(view.getInt16(at, true)); break;
            default: values.push(view.getUint8(at)); break;
          }
        }
        return jsonResult({ ...base, offset, as, values });
      },
    },
    {
      name: "read_vertices",
      description: "Decode the vertices a draw read, through its pipeline's vertex layout with attribute names from the vertex " +
        "shader: for an indexed draw the indices from firstIndex and the vertices they name, otherwise the run from " +
        "firstVertex. Also gives each attribute's minimum and maximum over every captured vertex and its NaN count, which " +
        "finds collapsed, exploded or uninitialized geometry at a glance.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        command: { type: "integer", minimum: 0, description: "The draw command's index." },
        count: { type: "integer", minimum: 1, maximum: 256, description: "Vertices (or indices) to decode (default 8)." },
        first: { type: "integer", minimum: 0, description: "Start at this index or vertex instead of the draw's own first one." },
        binding: { type: "integer", minimum: 0, description: "Only this vertex buffer binding." },
      }, ["command"]),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const d = c.data;
        const index = requireInt(args, "command");
        const cmd = d.commands[index];
        if (!cmd || !d.sets.DRAW.has(cmd.method)) throw new Error(`Command ${index} is not a draw: read_vertices takes a draw command (list_commands with kind draw).`);
        const state = drawState(d, c.db, cmd);
        const inputs = vertexInputs(c, state.pipeline);
        const a = cmd.args ?? {};
        const count = intArg(args, "count", 8, 1, 256);
        const first = optionalInt(args, "first");
        const indexed = /Indexed/i.test(cmd.method) && state.indexBuffer ? state.indexBuffer : null;
        let vertexIds: number[] = [];
        let indices: Record<string, unknown> | undefined;
        const ibData = indexed ? d.buffer(indexed.dataId)?.data : null;
        const ibSize = indexed ? indexSize(indexed.indexType) : 0;
        if (indexed && ibData && ibSize) {
          const firstIndex = first ?? num(a.firstIndex);
          const baseVertex = num(a.vertexOffset ?? a.baseVertex);
          const view = new DataView(ibData.buffer, ibData.byteOffset, ibData.byteLength);
          const values: number[] = [];
          for (let i = firstIndex; i < firstIndex + count && (i + 1) * ibSize <= ibData.byteLength; i++) values.push(readIndex(view, i * ibSize, ibSize));
          vertexIds = values.map((v) => v + baseVertex);
          indices = { first: firstIndex, baseVertex: baseVertex || undefined, values, truncatedFrom: d.buffer(indexed.dataId)?.info.originalSize };
        } else {
          const start = first ?? num(a.firstVertex ?? a.vertexStart);
          for (let i = 0; i < count; i++) vertexIds.push(start + i);
        }
        const wanted = optionalInt(args, "binding");
        const bindings = [...state.vertexBuffers.values()].filter((vb) => wanted === undefined || vb.binding === wanted).sort((x, y) => x.binding - y.binding);
        const out: unknown[] = [];
        for (const vb of bindings) {
          const layout = vertexLayout(state, vb.binding, vb);
          const head = { binding: vb.binding, buffer: refText(c.db, vb.buffer), offset: vb.offset, data: vb.dataId || undefined };
          if (!layout?.stride) {
            if (!d.sets.BIND_STAGE_BUFFER) out.push({ ...head, note: "The bound pipeline has no vertex layout for this binding." });
            continue;
          }
          const captured = d.buffer(vb.dataId);
          if (!captured?.data) {
            out.push({ ...head, stride: layout.stride, note: captured?.info.error ?? "The contents were not captured." });
            continue;
          }
          const view = new DataView(captured.data.buffer, captured.data.byteOffset, captured.data.byteLength);
          const element = vertexStruct(layout, inputs);
          const available = Math.floor(captured.data.byteLength / layout.stride);
          const perInstance = layout.rate.includes("INSTANCE");
          const ids = perInstance ? Array.from({ length: Math.min(count, Math.max(1, num(a.instanceCount))) }, (_, i) => num(a.firstInstance) + i) : vertexIds;
          out.push({
            ...head, stride: layout.stride, perInstance: perInstance || undefined, capturedVertices: available, truncatedFrom: captured.info.originalSize,
            attributes: element.members.map((m) => ({ name: m.name, format: m.type.kind === "format" ? m.type.format : undefined, offset: m.offset })),
            bounds: attributeBounds(element, view, layout.stride, available),
            vertices: ids.map((id) => (id < available ? { vertex: id, ...(readTyped(element, view, id * layout.stride) as Record<string, unknown>) } : { vertex: id, beyondCapturedRange: true })),
          });
        }
        return jsonResult({
          capture: c.id, command: index, method: cmd.method, pipeline: refText(c.db, state.pipeline?.id), indices,
          bindings: out.length ? out : undefined,
          note: bindings.length ? undefined : "No vertex buffers are bound at this draw (the vertices may come from the shader, or from a storage buffer).",
        });
      },
    },
    {
      name: "get_shader",
      description: "A shader of a capture. For a VkPipeline (every stage, or one with `stage`) or a VkShaderModule: view " +
        "\"reflection\" (entry points, inputs and outputs, resources by set and binding with struct layouts, push constants), " +
        "\"source\" (the source the compiler embedded, when it did), \"glsl\" / \"hlsl\" / \"msl\" (cross-compiled with " +
        "spirv-cross), \"disassembly\" (spirv-dis), or \"analysis\" (the modeled per-invocation cost by function and source " +
        "line, and findings for expensive constructs). For Metal: an MTLLibrary's or MTLFunction's source, or a pipeline " +
        "state's reflection.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        object: { type: "integer", minimum: 0, description: "The pipeline, shader module, library, function or pipeline state object id." },
        view: { type: "string", enum: SHADER_VIEWS, description: "What to show (default reflection)." },
        stage: { type: "string", description: "Only this stage of a pipeline: vertex, fragment, compute, ..." },
        maxChars: { type: "integer", minimum: 1000, maximum: 200000, description: "Longest text to return (default 40000)." },
      }, ["object"]),
      readOnly: true,
      handler: async (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const db = c.db;
        const id = requireInt(args, "object");
        const o = db.getObject(id);
        if (!o) throw new Error(`No object ${id} in ${c.id}.`);
        const view = enumArg(args, "view", SHADER_VIEWS, "reflection");
        const maxChars = intArg(args, "maxChars", 40000, 1000, 200000);
        if (o.type.startsWith("MTL")) return jsonResult(metalShader(c, o, view, maxChars));
        let sources: ShaderSource[];
        if (o.type === "VkPipeline") sources = pipelineStages(o, db);
        else if (o.type === "VkShaderModule") sources = o.blobs.length ? [{ stage: str(o.updates.stage) || "unknown", entryPoint: "", object: o, blobIndex: 0 }] : [];
        else throw new Error(`${refText(db, id)} has no shader code: get_shader takes a VkPipeline, a VkShaderModule, an MTLLibrary, an MTLFunction or a Metal pipeline state.`);
        const stage = stringArg(args, "stage")?.toLowerCase();
        if (stage) sources = sources.filter((s) => s.stage.startsWith(stage));
        const stages: Record<string, unknown>[] = [];
        for (const s of sources) {
          const spirv = c.spirv(s.object, s.blobIndex);
          const head = { stage: s.stage, entryPoint: s.entryPoint || undefined, shader: refText(db, s.object.id), spirvBytes: spirv?.byteLength };
          if (!spirv) {
            stages.push({ ...head, note: "The capture file carries no SPIR-V for this stage." });
            continue;
          }
          if (view === "reflection") {
            stages.push({ ...head, ...reflectionDetail(c.reflection(s.object, s.blobIndex)) });
          } else if (view === "source") {
            stages.push({ ...head, ...sourceDetail(spirv, maxChars) });
          } else if (view === "analysis") {
            stages.push({ ...head, ...analysisDetail(spirv, s.entryPoint) });
          } else {
            const r = await shaderText(spirv, view === "disassembly" ? "dis" : view);
            stages.push(r.ok
              ? { ...head, text: clip(r.text.replace(/\r\n/g, "\n"), maxChars) }
              : { ...head, error: r.text, note: "Cross-compiling and disassembling use spirv-cross and spirv-dis from the Vulkan SDK: set VULKAN_SDK or INSPECTOR_TOOLS_DIR, or put them on PATH." });
          }
        }
        return jsonResult({
          capture: c.id, object: refText(db, id), view, stages,
          note: sources.length ? undefined : "No shader stages with code were found for this object.",
        });
      },
    },
    {
      name: "analyze_shaders",
      description: "Rank the shaders a Vulkan capture's frame used: for each pipeline its draws and dispatches bound, every stage's " +
        "modeled per-invocation cost, the dominant kind of work, how many draws or dispatches used it, and its analysis " +
        "findings by severity, ordered by uses times cost. get_shader with view \"analysis\" explains one in detail.",
      inputSchema: schema({ capture: CAPTURE_PARAM, ...PAGE_PARAMS }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const d = c.data;
        const db = c.db;
        if (d.api === "metal") {
          return jsonResult({ capture: c.id, note: "The static shader analysis reads SPIR-V, so it covers Vulkan captures. For Metal shaders, GPU Inspector's Xcode Trace button writes a .gputrace whose shader profiler has per-line costs." });
        }
        const rows: { score: number; row: Record<string, unknown> }[] = [];
        for (const [pipelineId, uses] of pipelineUses(d)) {
          const p = db.getObject(pipelineId);
          if (!p) continue;
          for (const s of pipelineStages(p, db)) {
            const spirv = c.spirv(s.object, s.blobIndex);
            const a = spirv ? analyzeSpirvCached(spirv) : null;
            const e = a?.entryPoints.find((x) => x.name === s.entryPoint) ?? a?.entryPoints[0];
            const findings: Record<string, number> = {};
            for (const f of a?.findings ?? []) findings[f.severity] = (findings[f.severity] ?? 0) + 1;
            const worst = [...(a?.findings ?? [])].sort((x, y) => SEVERITY_RANK[y.severity] - SEVERITY_RANK[x.severity]).slice(0, 3);
            rows.push({
              score: uses * (e?.weighted ?? 0),
              row: {
                pipeline: refText(db, p.id), stage: s.stage, entryPoint: s.entryPoint, shader: refText(db, s.object.id), uses,
                cost: round(e?.weighted), dominant: e?.dominant,
                findings: Object.keys(findings).length ? findings : undefined,
                worst: worst.length ? worst.map((f) => `${f.severity} ${f.rule}${f.line ? ` (${f.file ? `${f.file}:` : "line "}${f.line})` : ""}: ${f.message}`) : undefined,
                note: spirv ? (a ? undefined : "The SPIR-V could not be analyzed.") : "No SPIR-V in the capture file.",
              },
            });
          }
        }
        rows.sort((x, y) => y.score - x.score);
        const p = page(rows, args, 30, 200);
        return jsonResult({
          capture: c.id, model: COST_MODEL, pipelines: pipelineUses(d).size, total: p.total, offset: p.offset, nextOffset: p.nextOffset,
          stages: p.items.map((r) => r.row),
        });
      },
    },
    {
      name: "get_shader_flame_graph",
      description: "The Shader Flame Graph of a Vulkan capture: the frame's GPU work by pass, pipeline (or draw), shader stage, " +
        "function and source line, with the frame's hottest functions and lines. Each stage weighs its modeled per-invocation " +
        "cost times its invocations: vertex and compute counts are exact (from the draw and dispatch arguments, indirect ones " +
        "from the captured buffers), fragment counts come from the pass's measured GPU counters where it has them (split " +
        "between its draws by scissor area) and from the scissor area otherwise. When every pass was timed (Profile " +
        "passes) the costs are milliseconds, each pass its measured GPU time with only the split inside it modeled; otherwise " +
        "modeled op units. Where analyze_shaders ranks shaders, this shows where the frame's shading work goes.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        pass: { type: "integer", minimum: 0, description: "Only this pass (the pass number get_bottlenecks and list_commands give); shares are then of the pass." },
        perDraw: { type: "boolean", description: "One frame per draw or dispatch instead of one per pipeline (default false)." },
        estimateFragments: { type: "boolean", description: "Weight fragment stages by the scissor or render area where the pass has no measured fragment counters, an upper bound without overdraw (default true); false leaves those stages unweighted." },
        depth: { type: "integer", minimum: 1, maximum: 32, description: "Levels to show: 1 passes, 2 pipelines or draws, 3 stages, then functions, their callees and source lines (default 6)." },
        minShare: { type: "number", minimum: 0, maximum: 1, description: "Fold frames below this share of the total into one \"other\" frame, left out when it is under 0.001 (default 0.01)." },
        top: { type: "integer", minimum: 0, maximum: 100, description: "How many of the hottest functions and lines to list (default 15)." },
      }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        if (c.data.api === "metal") {
          return jsonResult({ capture: c.id, note: "The flame graph weighs SPIR-V shaders, so it covers Vulkan captures. For Metal, get_bottlenecks has each pass's vertex/fragment split, and GPU Inspector's Xcode Trace button writes a .gputrace whose shader profiler has per-line costs." });
        }
        const { models, spirv } = stageModels(c);
        const result = buildFrameCostTree({
          data: c.data, db: c.db, models, perDraw: boolArg(args, "perDraw", false), estimateFragments: boolArg(args, "estimateFragments", true),
        });
        const texts = new Map<string, LineTexts>();
        const codeOf = (object: number | undefined, stage: string | undefined, file: string | undefined, line: number | undefined): string | undefined => {
          const key = `${object}|${stage}`;
          let t = texts.get(key);
          if (!t) {
            const bytes = spirv.get(key);
            t = bytes ? sourceLineTexts(debugInfoWithSources(bytes).info) : new Map();
            texts.set(key, t);
          }
          return codeAt(t, file, line);
        };
        let root = result.root;
        const pass = optionalInt(args, "pass");
        if (pass !== undefined) {
          const found = root.children.find((n) => n.command && c.passOf(n.command.index) === pass);
          if (!found) throw new Error(`Pass ${pass} is not in the flame graph: it has ${root.children.length} passes with draws or dispatches (get_bottlenecks lists every pass).`);
          root = found;
        }
        const total = root.totalCost;
        const view: FlameView = { c, total, depth: intArg(args, "depth", 6, 1, 32), minShare: Math.min(1, Math.max(0, numberArg(args, "minShare") ?? 0.01)) };
        return jsonResult({
          capture: c.id, units: result.units, meaning: result.units === "ms" ? FLAME_MS : FLAME_OPS, model: COST_MODEL,
          total: round(total), passes: pass === undefined ? result.stats.passes : undefined, drawsAndDispatches: pass === undefined ? result.stats.items : undefined,
          graph: flameFrame(view, root, 0),
          ...flameHotspots(c, root, total, intArg(args, "top", 15, 0, 100), codeOf),
          notes: result.notes.length ? result.notes : undefined,
        });
      },
    },
  ];
}
