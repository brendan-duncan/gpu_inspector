// Everything a shader debugger session needs from a capture, for one invocation of a Vulkan draw or
// dispatch: the stage's SPIR-V, the descriptor sets, push constants and specialization the command
// had, and the invocation's inputs.
//
//   * A vertex: its attributes decoded from the captured vertex buffers (mesh_input.ts) and its
//     built-ins.
//   * A fragment: the draw's vertex shader outputs (the replay's transform feedback, mesh_output.ts)
//     rasterized at the pixel: the front-most triangle covering the pixel's centre, clipped against
//     the near plane, its outputs interpolated (perspective-correct, flat or noperspective as the
//     fragment shader's inputs are decorated), for the pixel and the three others of its 2x2 quad.
//   * A compute invocation: its ids from the dispatch and the shader's local size.
//
// Shared by the debugger tab (shader_debugger_view.ts) and the MCP server's debug_shader.
import type { CaptureData, CapturedTexture } from "./capture_data.js";
import { drawState, findPass, type DrawState } from "./draw_state.js";
import { meshInput } from "./mesh_input.js";
import { positionOutput, primitiveKind, type MeshOutput } from "./mesh_output.js";
import { pipelineStages, type StageSource } from "./shader_cache.js";
import { Invocation, type DerivativeSource, type InvocationInputs, type ShaderBindings } from "./spirv/interpreter.js";
import { BuiltIn, Decoration, ExecutionModel, SpirvModule, StorageClass } from "./spirv/module.js";
import { PixelQuad } from "./spirv/quad.js";
import type { DebugSampler, DebugTexture, Value } from "./spirv/values.js";
import { decodeBase64 } from "./utils/base64.js";
import { decodeTexels, sliceBytes } from "./vulkan/texture_decode.js";
import { isObject, num, refId, str, type ObjectLookup } from "./vulkan/vulkan_object.js";
import type { ArgValue, CaptureCommand, CaptureDescriptor } from "../shared/protocol.js";

export type DebugTarget =
  | { stage: "vertex"; command: number; vertex: number; instance: number }
  | { stage: "fragment"; command: number; x: number; y: number }
  | { stage: "compute"; command: number; invocation: [number, number, number] };

/** Something that steps an invocation: the invocation itself, or a pixel quad around it. */
export interface Stepper {
  readonly invocation: Invocation;
  step(): ReturnType<Invocation["step"]>;
  run(): ReturnType<Invocation["run"]>;
}

export interface DebugSession {
  target: DebugTarget;
  module: SpirvModule;
  stage: StageSource;
  bindings: ShaderBindings;
  /** Starts the invocation from the beginning (Restart makes another). */
  start(): Stepper;
  /** How the inputs were found: the vertex or the triangle, the notes on what was left out. */
  description: string;
  notes: string[];
  /** What the invocation's range is, for the picker: vertices and instances of a draw, the dispatch size, the viewport. */
  limits: { vertices?: number; instances?: number; groups?: [number, number, number]; localSize?: [number, number, number]; width?: number; height?: number };
  /** Fragment: the render target's value at the pixel after the pass, for comparison. */
  targetPixel?: { image: number; attachment: number; value: number[]; format: string };
  /** Vertex: what the replay's transform feedback captured for the vertex, for comparison. */
  replayedOutputs?: { name: string; location?: number; builtin?: string; value: number[] }[];
}

export interface DebugContext {
  data: CaptureData;
  db: ObjectLookup & { blobData: Map<string, Uint8Array> };
  /** The replay's vertex shader outputs of a draw (a fragment needs them; a vertex compares with them). */
  meshOutput?: (command: number) => Promise<MeshOutput>;
  /** The vertex shader's input names, for attributes. */
  inputNames?: Map<number, string>;
}

const STAGE_MODEL = { vertex: ExecutionModel.Vertex, fragment: ExecutionModel.Fragment, compute: ExecutionModel.GLCompute } as const;

function bytesOf(v: ArgValue | undefined): Uint8Array | null {
  if (!isObject(v) || typeof v.base64 !== "string") return null;
  try {
    return decodeBase64(v.base64);
  } catch {
    return null;
  }
}

/** The pipeline stage a target debugs, with its SPIR-V. */
function stageOf(ctx: DebugContext, state: DrawState, stage: "vertex" | "fragment" | "compute"): { source: StageSource; module: SpirvModule } {
  const pipeline = state.pipeline;
  if (!pipeline) throw new Error("no pipeline is bound at the command");
  if (pipeline.type.startsWith("MTL")) throw new Error("the shader debugger runs SPIR-V: Metal shaders are not debugged yet");
  const source = pipelineStages(pipeline, ctx.db).find((s) => s.stage === stage);
  if (!source) throw new Error(`the pipeline has no ${stage} stage`);
  const bytes = ctx.db.blobData.get(`${source.object.id}:${source.blobIndex}`);
  if (!bytes) throw new Error(`the capture does not hold the ${stage} shader's SPIR-V`);
  return { source, module: new SpirvModule(bytes) };
}

/** Specialization constant bytes by SpecId, from the pipeline's create info for the stage. */
function specialization(state: DrawState, source: StageSource): Map<number, Uint8Array> {
  const out = new Map<number, Uint8Array>();
  const stages = state.pipeline?.descriptor?.pStages;
  const stageInfo = Array.isArray(stages) ? stages.find((s) => isObject(s) && str(s.stage) === source.stageFlag) : isObject(stages) ? stages : null;
  const spec = isObject(stageInfo) ? stageInfo.pSpecializationInfo : null;
  if (!isObject(spec)) return out;
  const data = bytesOf(spec.pData);
  if (!data || !Array.isArray(spec.pMapEntries)) return out;
  for (const e of spec.pMapEntries) {
    if (!isObject(e)) continue;
    const offset = num(e.offset), size = num(e.size);
    out.set(num(e.constantID), data.subarray(offset, offset + size));
  }
  return out;
}

function srgbToLinear(c: number): number {
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}

/** A VkComponentMapping as a channel index per output channel: 0-3 read R-A, -1 reads zero, -2 reads one. */
function swizzle(components: ArgValue | undefined): number[] | null {
  if (!isObject(components)) return null;
  const pick = (value: ArgValue | undefined, identity: number): number => {
    const v = str(value);
    if (!v || v.endsWith("IDENTITY")) return identity;
    if (v.endsWith("ZERO")) return -1;
    if (v.endsWith("ONE")) return -2;
    return "RGBA".indexOf(v.slice(-1));
  };
  const map = [pick(components.r, 0), pick(components.g, 1), pick(components.b, 2), pick(components.a, 3)];
  return map.every((m, i) => m === i) ? null : map;
}

/**
 * A captured sampled image as the interpreter reads it: levels and layers decoded when first sampled,
 * through the image view's component mapping (`components`) as a shader sees them.
 */
export function debugTexture(tex: CapturedTexture, components?: ArgValue): DebugTexture | null {
  const info = tex.info;
  const data = tex.data;
  if (!data) return null;
  const mips = Math.max(1, info.mips ?? 1);
  const layers = Math.max(1, info.layers);
  const srgb = info.format.includes("_SRGB");
  const mapping = swizzle(components);
  const levels = new Map<string, { width: number; height: number; texels: Float32Array } | null>();
  // Where each level starts: every level holds all its layers (or depth slices), back to back.
  const offsets: number[] = [];
  let at = 0;
  for (let k = 0; k < mips; k++) {
    offsets.push(at);
    const w = Math.max(1, info.width >> k), h = Math.max(1, info.height >> k);
    const slices = Math.max(layers, Math.max(1, info.depth >> k));
    at += sliceBytes({ format: info.format, aspect: info.aspect, width: w, height: h }) * slices;
  }
  return {
    width: info.width, height: info.height, depth: Math.max(1, info.depth), layers, mips, baseMip: info.mip, format: info.format,
    integer: /_UINT|_SINT/.test(info.format),
    level: (mip, layer) => {
      const k = mip - info.mip;
      if (k < 0 || k >= mips) return null;
      const key = `${k}/${layer}`;
      if (levels.has(key)) return levels.get(key) ?? null;
      const w = Math.max(1, info.width >> k), h = Math.max(1, info.height >> k);
      const texels = decodeTexels({ format: info.format, aspect: info.aspect, width: w, height: h }, data.subarray(offsets[k]), layer);
      let level: { width: number; height: number; texels: Float32Array } | null = null;
      if (texels) {
        const values = texels.values;
        for (let i = 0; i < w * h; i++) {
          const o = i * 4;
          if (texels.channels < 2) values[o + 1] = 0;
          if (texels.channels < 3) values[o + 2] = 0;
          if (texels.channels < 4) values[o + 3] = 1;
          if (srgb) for (let c = 0; c < 3; c++) values[o + c] = srgbToLinear(values[o + c]);
          if (mapping) {
            const src = [values[o], values[o + 1], values[o + 2], values[o + 3]];
            for (let c = 0; c < 4; c++) values[o + c] = mapping[c] === -1 ? 0 : mapping[c] === -2 ? 1 : src[mapping[c]];
          }
        }
        level = { width: w, height: h, texels: values };
      }
      levels.set(key, level);
      return level;
    },
  };
}

const ADDRESS: Record<string, DebugSampler["address"][number]> = {
  VK_SAMPLER_ADDRESS_MODE_REPEAT: "repeat", VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: "mirror", VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: "clamp",
  VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: "border", VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: "mirrorClamp",
};

const BORDERS: Record<string, number[]> = {
  VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK: [0, 0, 0, 0], VK_BORDER_COLOR_INT_TRANSPARENT_BLACK: [0, 0, 0, 0],
  VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK: [0, 0, 0, 1], VK_BORDER_COLOR_INT_OPAQUE_BLACK: [0, 0, 0, 1],
  VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE: [1, 1, 1, 1], VK_BORDER_COLOR_INT_OPAQUE_WHITE: [1, 1, 1, 1],
};

export function debugSampler(db: ObjectLookup, id: number | null): DebugSampler | null {
  const d = id ? db.getObject(id)?.descriptor : null;
  if (!d) return null;
  return {
    magFilter: str(d.magFilter).includes("NEAREST") ? "nearest" : "linear",
    minFilter: str(d.minFilter).includes("NEAREST") ? "nearest" : "linear",
    mipmapMode: str(d.mipmapMode).includes("NEAREST") ? "nearest" : "linear",
    address: [ADDRESS[str(d.addressModeU)] ?? "repeat", ADDRESS[str(d.addressModeV)] ?? "repeat", ADDRESS[str(d.addressModeW)] ?? "repeat"],
    border: BORDERS[str(d.borderColor)] ?? [0, 0, 0, 0],
    compareOp: d.compareEnable ? str(d.compareOp) : null,
    minLod: num(d.minLod),
    maxLod: d.maxLod === undefined ? 1000 : num(d.maxLod),
    lodBias: num(d.mipLodBias),
    unnormalized: Boolean(d.unnormalizedCoordinates),
  };
}

/** The descriptor sets, push constants and specialization a command had bound. */
export function commandBindings(ctx: DebugContext, state: DrawState, source: StageSource): ShaderBindings {
  const descriptor = (set: number, binding: number, element: number): CaptureDescriptor | null => {
    const bound = state.sets.get(set);
    const b = bound?.set.bindings.find((x) => x.binding === binding);
    return b?.descriptors[element] ?? null;
  };
  const textures = new Map<string, DebugTexture | null>();
  let push: Uint8Array | null = null;
  const stageBit = source.stageFlag;
  const updates = state.pushConstants.filter((p) => !p.stageFlags || p.stageFlags.includes(stageBit) || p.stageFlags.includes("ALL"));
  if (updates.length) {
    const size = Math.max(...updates.map((p) => p.offset + p.size));
    push = new Uint8Array(size);
    for (const p of updates) if (p.data) push.set(p.data.subarray(0, Math.min(p.data.byteLength, size - p.offset)), p.offset);
  }
  return {
    buffer: (set, binding, element) => {
      const d = descriptor(set, binding, element);
      return d?.data ? ctx.data.buffer(d.data)?.data ?? null : null;
    },
    texture: (set, binding, element) => {
      const key = `${set}/${binding}/${element}`;
      if (textures.has(key)) return textures.get(key) ?? null;
      const d = descriptor(set, binding, element);
      const captured = d?.data ? ctx.data.capturedImage(d.data) : null;
      const view = ctx.db.getObject(refId(d?.imageView) ?? captured?.info.view ?? 0)?.descriptor;
      const tex = captured ? debugTexture(captured, view?.components) : null;
      textures.set(key, tex);
      return tex;
    },
    sampler: (set, binding, element) => debugSampler(ctx.db, refId(descriptor(set, binding, element)?.sampler) ?? null),
    pushConstants: push,
    specialization: specialization(state, source),
  };
}

// ---------------------------------------------------------------------------------------------
// Rasterizing a pixel

interface ClipVertex { clip: number[]; outputs: Map<number, number[]> }

/** The draw's viewport: the dynamic one bound, else the pipeline's. */
function viewportOf(state: DrawState): { x: number; y: number; width: number; height: number; minDepth: number; maxDepth: number } | null {
  const pick = (v: ArgValue | null | undefined): ArgValue | null => (Array.isArray(v) ? v[0] ?? null : v ?? null);
  let vp = pick(state.viewports);
  if (!isObject(vp)) {
    const vs = state.pipeline?.descriptor?.pViewportState;
    vp = isObject(vs) ? pick(vs.pViewports) : null;
  }
  if (!isObject(vp)) return null;
  return { x: num(vp.x), y: num(vp.y), width: num(vp.width), height: num(vp.height), minDepth: num(vp.minDepth), maxDepth: vp.maxDepth === undefined ? 1 : num(vp.maxDepth) };
}

/** Clips a triangle against the w > epsilon half-space; the polygon left, as a fan. */
function clipNear(tri: ClipVertex[]): ClipVertex[] {
  const eps = 1e-6;
  const out: ClipVertex[] = [];
  for (let i = 0; i < tri.length; i++) {
    const a = tri[i], b = tri[(i + 1) % tri.length];
    const ain = a.clip[3] > eps, bin = b.clip[3] > eps;
    if (ain) out.push(a);
    if (ain !== bin) {
      const t = (eps - a.clip[3]) / (b.clip[3] - a.clip[3]);
      const lerp = (x: number[], y: number[]): number[] => x.map((v, k) => v + (y[k] - v) * t);
      const outputs = new Map<number, number[]>();
      for (const [loc, value] of a.outputs) outputs.set(loc, lerp(value, b.outputs.get(loc) ?? value));
      out.push({ clip: lerp(a.clip, b.clip), outputs });
    }
  }
  return out;
}

interface Covering {
  primitive: number;
  /** Window-space vertices of the (clipped) triangle covering the pixel. */
  window: { x: number; y: number; z: number; invW: number; v: ClipVertex }[];
  front: boolean;
  /** The provoking vertex's outputs (flat inputs). */
  provoking: ClipVertex;
}

function edge(ax: number, ay: number, bx: number, by: number, px: number, py: number): number {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/** Finds the triangle whose fragment wins at the pixel: covering its centre, not culled, front-most by the depth test. */
function coveringTriangle(state: DrawState, mesh: MeshOutput, px: number, py: number): { hit: Covering | null; triangles: number; reason: string } {
  const viewport = viewportOf(state);
  const pos = positionOutput(mesh);
  if (!viewport) return { hit: null, triangles: 0, reason: "the draw has no viewport" };
  if (!pos || !mesh.data) return { hit: null, triangles: 0, reason: "the replay captured no gl_Position for the draw" };
  if (primitiveKind(mesh.topology) !== "triangles") return { hit: null, triangles: 0, reason: "the draw does not draw triangles" };
  const view = new DataView(mesh.data.buffer, mesh.data.byteOffset, mesh.data.byteLength);
  const vertex = (i: number): ClipVertex => {
    const base = i * mesh.stride;
    const read = (offset: number, components: number, base2: string): number[] =>
      Array.from({ length: components }, (_, k) => (base2 === "float" ? view.getFloat32(base + offset + k * 4, true) : base2 === "int" ? view.getInt32(base + offset + k * 4, true) : view.getUint32(base + offset + k * 4, true)));
    const outputs = new Map<number, number[]>();
    for (const o of mesh.outputs) if (o.location !== undefined) outputs.set(o.location, read(o.offset, o.components, o.base));
    return { clip: read(pos.offset, 4, "float"), outputs };
  };
  const d = state.pipeline?.descriptor;
  const raster = isObject(d?.pRasterizationState) ? d!.pRasterizationState : null;
  const ds = isObject(d?.pDepthStencilState) ? d!.pDepthStencilState : null;
  const cull = str(raster?.cullMode);
  const ccwFront = !str(raster?.frontFace).includes("CLOCKWISE") || str(raster?.frontFace).includes("COUNTER");
  const compare = ds?.depthTestEnable ? str(ds.depthCompareOp) : "";
  const prefersLess = compare.includes("LESS");
  const prefersGreater = compare.includes("GREATER");
  const cx = px + 0.5, cy = py + 0.5;
  let best: Covering | null = null;
  let bestDepth = 0;
  const triangles = Math.floor(mesh.vertices / 3);
  for (let t = 0; t < triangles; t++) {
    const tri = [vertex(t * 3), vertex(t * 3 + 1), vertex(t * 3 + 2)];
    const poly = tri.some((v) => v.clip[3] <= 1e-6) ? clipNear(tri) : tri;
    if (poly.length < 3) continue;
    const win = poly.map((v) => {
      const w = v.clip[3];
      return {
        x: viewport.x + (v.clip[0] / w + 1) * viewport.width / 2,
        y: viewport.y + (v.clip[1] / w + 1) * viewport.height / 2,
        z: viewport.minDepth + (v.clip[2] / w) * (viewport.maxDepth - viewport.minDepth),
        invW: 1 / w,
        v,
      };
    });
    // Facing from the whole polygon's signed area (Vulkan's formula, in framebuffer coordinates).
    let area = 0;
    for (let i = 0; i < win.length; i++) {
      const a = win[i], b = win[(i + 1) % win.length];
      area += a.x * b.y - b.x * a.y;
    }
    area *= -0.5;
    const ccw = area > 0;
    const front = ccw === ccwFront;
    if ((cull.includes("BACK") && !front) || (cull.includes("FRONT") && front) || cull.includes("FRONT_AND_BACK")) continue;
    for (let f = 1; f + 1 < win.length; f++) {
      const a = win[0], b = win[f], c = win[f + 1];
      const total = edge(a.x, a.y, b.x, b.y, c.x, c.y);
      if (Math.abs(total) < 1e-12) continue;
      const l0 = edge(b.x, b.y, c.x, c.y, cx, cy) / total;
      const l1 = edge(c.x, c.y, a.x, a.y, cx, cy) / total;
      const l2 = edge(a.x, a.y, b.x, b.y, cx, cy) / total;
      if (l0 < 0 || l1 < 0 || l2 < 0) continue;
      const depth = l0 * a.z + l1 * b.z + l2 * c.z;
      const better = !best || (prefersLess ? depth <= bestDepth : prefersGreater ? depth >= bestDepth : true);
      if (better) {
        best = { primitive: t, window: [a, b, c], front, provoking: tri[0] };
        bestDepth = depth;
      }
    }
  }
  return { hit: best, triangles, reason: best ? "" : `none of the draw's ${triangles.toLocaleString()} triangles covers pixel (${px}, ${py})` };
}

/** The fragment inputs at a pixel centre from a covering triangle, extended past its edges for the quad's other pixels. */
function fragmentInputs(module: SpirvModule, hit: Covering, px: number, py: number): InvocationInputs {
  const [a, b, c] = hit.window;
  const cx = px + 0.5, cy = py + 0.5;
  const total = edge(a.x, a.y, b.x, b.y, c.x, c.y);
  const l = [edge(b.x, b.y, c.x, c.y, cx, cy) / total, edge(c.x, c.y, a.x, a.y, cx, cy) / total, edge(a.x, a.y, b.x, b.y, cx, cy) / total];
  const invW = l[0] * a.invW + l[1] * b.invW + l[2] * c.invW;
  const persp = [l[0] * a.invW / invW, l[1] * b.invW / invW, l[2] * c.invW / invW];
  const z = l[0] * a.z + l[1] * b.z + l[2] * c.z;
  const locations = new Map<number, number[]>();
  const decorations = new Map<number, { flat: boolean; noPerspective: boolean }>();
  for (const [id, g] of module.globals) {
    if (g.storage !== StorageClass.Input) continue;
    const location = module.decoration(id, Decoration.Location)?.[0];
    if (location === undefined) continue;
    decorations.set(location, { flat: module.decoration(id, Decoration.Flat) !== undefined, noPerspective: module.decoration(id, Decoration.NoPerspective) !== undefined });
  }
  for (const [location, provoking] of hit.provoking.outputs) {
    const deco = decorations.get(location);
    if (deco?.flat) {
      locations.set(location, provoking);
      continue;
    }
    const weights = deco?.noPerspective ? l : persp;
    const va = a.v.outputs.get(location) ?? provoking, vb = b.v.outputs.get(location) ?? provoking, vc = c.v.outputs.get(location) ?? provoking;
    locations.set(location, va.map((x, k) => weights[0] * x + weights[1] * vb[k] + weights[2] * vc[k]));
  }
  const builtins = new Map<number, Value>([
    [BuiltIn.FragCoord, [cx, cy, z, invW]],
    [BuiltIn.FrontFacing, hit.front],
    [BuiltIn.PrimitiveId, hit.primitive],
    [BuiltIn.SampleId, 0],
    [BuiltIn.SamplePosition, [0.5, 0.5]],
    [BuiltIn.HelperInvocation, false],
    [BuiltIn.PointCoord, [0.5, 0.5]],
    [BuiltIn.Layer, 0],
    [BuiltIn.ViewIndex, 0],
  ]);
  return { locations, builtins };
}

// ---------------------------------------------------------------------------------------------

/** Prepares a debugging session for a target; throws with the reason it cannot be debugged. */
export async function prepareDebugSession(ctx: DebugContext, target: DebugTarget): Promise<DebugSession> {
  const { data, db } = ctx;
  const cmd: CaptureCommand | undefined = data.commands[target.command];
  if (!cmd) throw new Error(`the capture has no command ${target.command}`);
  const sets = data.sets;
  if (target.stage === "compute" ? !sets.DISPATCH.has(cmd.method) : !sets.DRAW.has(cmd.method)) {
    throw new Error(`command ${target.command} (${cmd.method}) is not a ${target.stage === "compute" ? "dispatch" : "draw"}`);
  }
  const state = drawState(data, db, cmd);
  const { source, module } = stageOf(ctx, state, target.stage);
  const bindings = commandBindings(ctx, state, source);
  const model = STAGE_MODEL[target.stage];
  const notes: string[] = [];
  const entryPoint = source.entryPoint;
  const a = cmd.args ?? {};

  if (target.stage === "compute") {
    const entry = module.entryPoint(entryPoint, model);
    let localSize: [number, number, number] = [1, 1, 1];
    const literal = entry?.modes.get(17);
    const ids = entry?.modes.get(38);
    if (literal) localSize = [literal[0] ?? 1, literal[1] ?? 1, literal[2] ?? 1];
    else if (ids) localSize = ids.map((id) => Number(module.constants.get(id) ?? 1)) as [number, number, number];
    const groups: [number, number, number] = cmd.method.includes("Indirect") ? [1, 1, 1] : [Math.max(1, num(a.groupCountX)), Math.max(1, num(a.groupCountY)), Math.max(1, num(a.groupCountZ))];
    if (cmd.method.includes("Indirect")) notes.push("An indirect dispatch's group counts are in a buffer: gl_NumWorkGroups reads (1, 1, 1).");
    const g = target.invocation;
    const inputs: InvocationInputs = {
      locations: new Map(),
      builtins: new Map<number, Value>([
        [BuiltIn.GlobalInvocationId, g],
        [BuiltIn.LocalInvocationId, g.map((v, i) => v % localSize[i])],
        [BuiltIn.WorkgroupId, g.map((v, i) => Math.floor(v / localSize[i]))],
        [BuiltIn.LocalInvocationIndex, (g[2] % localSize[2]) * localSize[0] * localSize[1] + (g[1] % localSize[1]) * localSize[0] + (g[0] % localSize[0])],
        [BuiltIn.NumWorkgroups, groups],
        [BuiltIn.WorkgroupSize, localSize],
      ]),
    };
    return {
      target, module, stage: source, bindings, notes,
      description: `invocation (${g.join(", ")}) of a ${groups.join(" x ")} dispatch with local size ${localSize.join(" x ")}`,
      limits: { groups, localSize },
      start: () => new Invocation(module, { entryPoint, model, bindings, inputs }),
    };
  }

  if (target.stage === "vertex") {
    const input = meshInput(data, db, cmd, ctx.inputNames ?? new Map());
    notes.push(...input.notes);
    const order = target.vertex;
    if (order < 0 || order >= input.ids.length) throw new Error(`the draw reads ${input.ids.length.toLocaleString()} vertices: there is no vertex ${order}`);
    const locations = new Map<number, number[]>();
    input.attributes.forEach((attr, k) => {
      const values = input.values(order, k, target.instance);
      if (values) locations.set(attr.location, values);
    });
    const firstInstance = num(a.firstInstance);
    const baseVertex = num(a.vertexOffset ?? a.firstVertex);
    const vertexId = input.ids[order];
    const inputs: InvocationInputs = {
      locations,
      builtins: new Map<number, Value>([
        [BuiltIn.VertexIndex, vertexId], [BuiltIn.VertexId, vertexId], [BuiltIn.InstanceIndex, firstInstance + target.instance],
        [BuiltIn.InstanceId, target.instance], [BuiltIn.BaseVertex, baseVertex], [BuiltIn.BaseInstance, firstInstance], [BuiltIn.DrawIndex, 0],
        [BuiltIn.ViewIndex, 0],
      ]),
    };
    let replayedOutputs: DebugSession["replayedOutputs"];
    if (ctx.meshOutput) {
      try {
        const mesh = await ctx.meshOutput(target.command);
        // Transform feedback writes vertices in assembly order, instance after instance.
        const record = target.instance * input.ids.length + order;
        if (mesh.measured && mesh.data && primitiveKind(mesh.topology) === primitiveKind(input.topology) && !/STRIP|FAN/.test(mesh.topology) && record < mesh.vertices) {
          const view = new DataView(mesh.data.buffer, mesh.data.byteOffset, mesh.data.byteLength);
          replayedOutputs = mesh.outputs.map((o) => ({
            name: o.name, location: o.location, builtin: o.builtin,
            value: Array.from({ length: o.components }, (_, k) => {
              const at = record * mesh.stride + o.offset + k * 4;
              return o.base === "float" ? view.getFloat32(at, true) : o.base === "int" ? view.getInt32(at, true) : view.getUint32(at, true);
            }),
          }));
        }
      } catch {
        // Nothing to compare with.
      }
    }
    return {
      target, module, stage: source, bindings, notes, replayedOutputs,
      description: `vertex ${order} of the draw (gl_VertexIndex ${vertexId}), instance ${target.instance}`,
      limits: { vertices: input.ids.length, instances: Math.max(1, num(a.instanceCount) || 1) },
      start: () => new Invocation(module, { entryPoint, model, bindings, inputs }),
    };
  }

  // A fragment.
  if (!ctx.meshOutput) throw new Error("a fragment's inputs come from replaying the draw's vertex shader, and no replay is available here");
  const mesh = await ctx.meshOutput(target.command);
  if (!mesh.measured) throw new Error(`the draw's vertex shader outputs could not be captured: ${mesh.note ?? "the replay did not reach the draw"}`);
  const { x, y } = target;
  const { hit, triangles, reason } = coveringTriangle(state, mesh, x, y);
  if (!hit) throw new Error(reason);
  if (triangles !== Math.floor(mesh.vertices / 3)) notes.push("The replay truncated the draw's vertices.");
  const viewport = viewportOf(state);
  const { x0, y0, target: lane } = PixelQuad.place(x, y);
  // The render target's value at the pixel after the pass, for comparison.
  let targetPixel: DebugSession["targetPixel"];
  const passInfo = findPass(data, cmd);
  const colour = passInfo
    ? data.texturesForPass(cmd.frame, passInfo.passBegin.object?.__id ?? 0, passInfo.passIndex).find((t) => t.info.aspect === "color" && !t.info.resolve && t.data)
    : undefined;
  if (colour?.data && x < colour.info.width && y < colour.info.height) {
    const texels = decodeTexels({ format: colour.info.format, aspect: "color", width: colour.info.width, height: colour.info.height }, colour.data);
    if (texels) {
      const o = (y * texels.width + x) * 4;
      targetPixel = { image: colour.info.id, attachment: colour.info.attachment, value: Array.from(texels.values.subarray(o, o + Math.min(4, Math.max(texels.channels, 1)))), format: colour.info.format };
    }
  }
  return {
    target, module, stage: source, bindings, notes, targetPixel,
    description: `pixel (${x}, ${y}), from triangle ${hit.primitive.toLocaleString()} of ${triangles.toLocaleString()} (${hit.front ? "front" : "back"} facing)`,
    limits: { width: viewport ? Math.abs(viewport.width) : undefined, height: viewport ? Math.abs(viewport.height) : undefined },
    start: () => new PixelQuad((dx, dy, derivatives: DerivativeSource) => new Invocation(module, {
      entryPoint, model, bindings, inputs: fragmentInputs(module, hit, x0 + dx, y0 + dy), derivatives,
    }), lane),
  };
}

/** A pixel the draw covers, to open a fragment debugger on: the centre of its first front-facing visible triangle. */
export function coveredPixel(state: DrawState, mesh: MeshOutput): { x: number; y: number } | null {
  const viewport = viewportOf(state);
  const pos = positionOutput(mesh);
  if (!viewport || !pos || !mesh.data || primitiveKind(mesh.topology) !== "triangles") return null;
  const view = new DataView(mesh.data.buffer, mesh.data.byteOffset, mesh.data.byteLength);
  for (let t = 0; t * 3 + 2 < mesh.vertices; t++) {
    let sx = 0, sy = 0, ok = true;
    for (let k = 0; k < 3; k++) {
      const at = (t * 3 + k) * mesh.stride + pos.offset;
      const w = view.getFloat32(at + 12, true);
      if (!(w > 1e-6)) {
        ok = false;
        break;
      }
      sx += viewport.x + (view.getFloat32(at, true) / w + 1) * viewport.width / 2;
      sy += viewport.y + (view.getFloat32(at + 4, true) / w + 1) * viewport.height / 2;
    }
    if (!ok) continue;
    const x = Math.floor(sx / 3), y = Math.floor(sy / 3);
    const minX = Math.min(viewport.x, viewport.x + viewport.width), minY = Math.min(viewport.y, viewport.y + viewport.height);
    if (x < minX || y < minY || x >= minX + Math.abs(viewport.width) || y >= minY + Math.abs(viewport.height)) continue;
    if (coveringTriangle(state, mesh, x, y).hit) return { x, y };
  }
  return null;
}
