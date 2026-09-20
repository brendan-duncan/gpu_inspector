// Everything a shader debugger session needs from a capture, for one invocation of a draw or
// dispatch: the stage's code, the resources the command had bound, and the invocation's inputs.
//
//   * A vertex: its attributes decoded from the captured vertex buffers (mesh_input.ts) and its
//     built-ins.
//   * A fragment: the draw's vertex shader outputs rasterized at the pixel: the front-most triangle
//     covering the pixel's center, clipped against the near plane, its outputs interpolated
//     (perspective-correct, flat or noperspective as the fragment shader's inputs say), for the
//     pixel and the three others of its 2x2 quad.
//   * A compute invocation: its ids from the dispatch and the shader's local size.
//
// This file holds the Vulkan half and the rasterizer every API shares; metal/shader_debug.ts holds
// the Metal one, d3d12/shader_debug.ts the Direct3D 12 one, and prepareDebugSession sends a command
// to whichever the pipeline came from. The rasterizer is shared because the three differ only in
// where the state comes from: a Vulkan draw's cull mode is in its pipeline, a Metal draw's is a
// command on the encoder, a D3D12 draw's is in its pipeline state with D3D's conventions.
//
// Shared by the debugger tab (shader_debugger_view.ts) and the MCP server's debug_shader.
import type { CaptureData, CapturedTexture } from "./capture_data.js";
import { drawState, dynamicValue, findPass, type DrawState } from "./draw_state.js";
import { interpretedMeshOutput, isMetalPipeline, metalRasterState, prepareMetalSession } from "./metal/shader_debug.js";
import { d3d12RasterState, interpretedD3D12MeshOutput, isD3D12Pipeline, prepareD3D12Session } from "./d3d12/shader_debug.js";
import { isD3D12Type } from "./d3d12/d3d12_object.js";
import type { MslBindings } from "./msl/interpreter.js";
import { meshInput } from "./mesh_input.js";
import { positionOutput, primitiveKind, type MeshOutput, type MeshOutputVariable } from "./mesh_output.js";
import { stateStages, type StageSource } from "./shader_cache.js";
import { Invocation, type InvocationInputs, type ShaderBindings } from "./spirv/interpreter.js";
import { BuiltIn, Decoration, ExecutionModel, SpirvModule, StorageClass } from "./spirv/module.js";
import { SpirvProgram } from "./spirv/program.js";
import { PixelQuad, type DerivativeSource } from "./debug/quad.js";
import type { DebugInvocation, DebugProgram, Stepper } from "./debug/program.js";
import { scalars } from "./debug/values.js";
import type { DebugSampler, DebugTexture, Value } from "./spirv/values.js";
import { decodeBase64 } from "./utils/base64.js";
import { decodeTexels, sliceBytes } from "./vulkan/texture_decode.js";
import { isObject, num, refId, str, type ObjectLookup } from "./vulkan/vulkan_object.js";
import type { ArgValue, CaptureCommand, CaptureDescriptor } from "../shared/protocol.js";

export type DebugTarget =
  | { stage: "vertex"; command: number; vertex: number; instance: number }
  | { stage: "fragment"; command: number; x: number; y: number }
  | { stage: "compute"; command: number; invocation: [number, number, number] };

export type { Stepper };

export interface DebugSession {
  target: DebugTarget;
  /** The shader as the debugger reads it: its source, names and value formatting. */
  program: DebugProgram;
  stage: StageSource;
  /** What the command had bound, as whichever interpreter reads it. Kept for a caller that wants it. */
  bindings: ShaderBindings | MslBindings;
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
  /**
   * When the program is a translation of the capture's module (DebugContext.translate): starts the
   * same invocation of the original, to check the translation computes what it does.
   */
  original?: () => Stepper;
}

export interface DebugContext {
  data: CaptureData;
  db: ObjectLookup & { blobData: Map<string, Uint8Array> };
  /** The replay's vertex shader outputs of a draw (a fragment needs them; a vertex compares with them). */
  meshOutput?: (command: number) => Promise<MeshOutput>;
  /** The vertex shader's input names, for attributes. */
  inputNames?: Map<number, string>;
  /**
   * Fetches an object's payload when the database does not already hold it: a live Metal capture's
   * library source, which is not requested until something asks to step it. A saved capture holds
   * every payload already, so this is optional.
   */
  fetchBlob?: (objectId: number, index: number) => Promise<Uint8Array | null>;
  /**
   * A Vulkan stage's SPIR-V is stepped as what this returns instead: the module decompiled to GLSL
   * and recompiled with line information, for a module without source. Throws with the reason it
   * could not translate. The session's `original` runs the capture's own module to compare with.
   */
  translate?: (spirv: Uint8Array, source: StageSource) => Promise<Uint8Array>;
  /**
   * A D3D12 stage's HLSL compiled to SPIR-V with line information, which is what the debugger
   * steps for it (d3d12/shader_debug.ts): given the stage's DXBC/DXIL container and its profile
   * from the reflection ("ps_6_0"). Throws with the reason it could not compile. Without it a
   * D3D12 command cannot be debugged.
   */
  compileHlsl?: (bytecode: Uint8Array, source: StageSource, target: string) => Promise<Uint8Array>;
}

/** What a session built with DebugContext.translate says about the code it steps. */
export const TRANSLATION_NOTE = "This steps GLSL that spirv-cross decompiled from the SPIR-V and glslang compiled back: " +
  "it should compute the same values, but it is not the module the GPU ran, so the result is checked against the original.";

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
function stageOf(ctx: DebugContext, state: DrawState, stage: "vertex" | "fragment" | "compute"): { source: StageSource; bytes: Uint8Array; module: SpirvModule } {
  if (!state.pipeline && !state.shaders.length) throw new Error("no pipeline or shader object is bound at the command");
  const source = stateStages(state, ctx.db).find((s) => s.stage === stage);
  if (!source) throw new Error(state.pipeline ? `the pipeline has no ${stage} stage` : `no ${stage} shader object is bound at the command`);
  const bytes = ctx.db.blobData.get(`${source.object.id}:${source.blobIndex}`);
  if (!bytes) throw new Error(`the capture does not hold the ${stage} shader's SPIR-V`);
  return { source, bytes, module: new SpirvModule(bytes) };
}

/** One output (or, for compute, one buffer) of a translation's invocation beside the original's. */
export interface ComparedValue {
  label: string;
  translated: number[] | null;
  original: number[] | null;
  matches: boolean;
}

export interface OriginalComparison {
  /** Whether the two ran to the same end with the same results. */
  matches: boolean;
  status: { translated: string; original: string; error?: string };
  values: ComparedValue[];
}

/** Whether a translation's scalar is the original's: equal to a few parts in a million, NaN to NaN. */
export function sameValue(x: number | undefined, y: number | undefined): boolean {
  if (x === undefined || y === undefined) return false;
  if (Number.isNaN(x) || Number.isNaN(y)) return Number.isNaN(x) && Number.isNaN(y);
  return x === y || Math.abs(x - y) <= 1e-5 * Math.max(1, Math.abs(x), Math.abs(y));
}

function sameScalars(a: number[] | null, b: number[] | null): boolean {
  return !!a && !!b && a.length === b.length && a.every((x, i) => sameValue(x, b[i]));
}

/**
 * The results of a translation's finished invocation against the original's: the stage outputs by
 * location and built-in (gl_Position whether it is a variable or gl_PerVertex's first member), and
 * for a compute shader, which writes nothing else, its buffers by set and binding.
 */
export function compareWithOriginal(translated: DebugInvocation, original: DebugInvocation, stage: DebugTarget["stage"]): OriginalComparison {
  const keyed = (inv: DebugInvocation): Map<string, { label: string; value: number[] }> => {
    const out = new Map<string, { label: string; value: number[] }>();
    const vars = stage === "compute" ? inv.resourceVariables().filter((v) => v.set !== undefined && v.binding !== undefined) : inv.outputs();
    for (const v of vars) {
      if (stage === "compute") {
        out.set(`b${v.set}/${v.binding}`, { label: `set ${v.set} binding ${v.binding} (${v.name})`, value: scalars(v.value) });
      } else if (v.location !== undefined) {
        out.set(`l${v.location}`, { label: `location ${v.location} (${v.name})`, value: scalars(v.value) });
      } else if (v.builtin !== undefined) {
        out.set(`b${v.builtin}`, { label: v.builtin === BuiltIn.Position ? "position" : `${v.name} (built-in ${v.builtin})`, value: scalars(v.value) });
      } else if (Array.isArray(v.value) && Array.isArray(v.value[0])) {
        // gl_PerVertex: a block whose first member is the position.
        out.set(`b${BuiltIn.Position}`, { label: "position", value: scalars(v.value[0]) });
      }
    }
    return out;
  };
  const a = keyed(translated), b = keyed(original);
  const values: ComparedValue[] = [];
  for (const key of new Set([...b.keys(), ...a.keys()])) {
    const t = a.get(key), o = b.get(key);
    values.push({ label: (o ?? t)!.label, translated: t?.value ?? null, original: o?.value ?? null, matches: sameScalars(t?.value ?? null, o?.value ?? null) });
  }
  const status = { translated: translated.status, original: original.status, error: original.error || undefined };
  // A discarded fragment or an error has no results worth comparing: the ends must agree.
  const ended = translated.status === "returned" && original.status === "returned";
  return { matches: translated.status === original.status && (!ended || values.every((v) => v.matches)), status, values: ended ? values : [] };
}

/** Specialization constant bytes by SpecId, from the pipeline's (or the shader object's) create info for the stage. */
function specialization(state: DrawState, source: StageSource): Map<number, Uint8Array> {
  const out = new Map<number, Uint8Array>();
  const stages = state.pipeline?.descriptor?.pStages;
  const stageInfo = source.object.type === "VkShaderEXT"
    ? source.object.descriptor
    : Array.isArray(stages) ? stages.find((s) => isObject(s) && str(s.stage) === source.stageFlag) : isObject(stages) ? stages : null;
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

export interface ClipVertex { clip: number[]; outputs: Map<string, number[]> }

/**
 * The rasterizer state a fragment's inputs depend on. A Vulkan draw keeps it in its pipeline (with
 * the viewport possibly dynamic); a Metal draw sets it with commands on the encoder. Both are read
 * into this, so the rasterizing below is written once.
 */
export interface RasterState {
  viewport: { x: number; y: number; width: number; height: number; minDepth: number; maxDepth: number } | null;
  cullFront: boolean;
  cullBack: boolean;
  /** Counter-clockwise in framebuffer coordinates is the front face. */
  ccwFront: boolean;
  /**
   * Clip-space +Y is the *top* of the render target, which is Metal's convention. Vulkan's +Y is
   * the bottom, so the viewport transform's sign differs — and a fragment debugged at a pixel
   * would otherwise be taken from the triangle mirrored about the middle of the screen.
   */
  yUp: boolean;
  /** Which of two triangles covering the pixel wins; "none" keeps the last one drawn. */
  depthPrefers: "less" | "greater" | "none";
}

/** The draw's viewport: the dynamic one bound, else the pipeline's (Vulkan) or the encoder's (Metal). */
function viewportOf(state: DrawState): RasterState["viewport"] {
  const pick = (v: ArgValue | null | undefined): ArgValue | null => (Array.isArray(v) ? v[0] ?? null : v ?? null);
  let vp = pick(state.viewports);
  if (!isObject(vp)) {
    const vs = state.pipeline?.descriptor?.pViewportState;
    vp = isObject(vs) ? pick(vs.pViewports) : null;
  }
  if (!isObject(vp)) return null;
  // D3D12_VIEWPORT: TopLeftX / TopLeftY, Width / Height, MinDepth / MaxDepth.
  if (vp.TopLeftX !== undefined || vp.Width !== undefined) {
    return {
      x: num(vp.TopLeftX), y: num(vp.TopLeftY), width: num(vp.Width), height: num(vp.Height),
      minDepth: num(vp.MinDepth), maxDepth: vp.MaxDepth === undefined ? 1 : num(vp.MaxDepth),
    };
  }
  // MTLViewport spells the same thing differently, and its depth range is znear..zfar.
  if (vp.originX !== undefined || vp.znear !== undefined) {
    return {
      x: num(vp.originX), y: num(vp.originY), width: num(vp.width), height: num(vp.height),
      minDepth: num(vp.znear), maxDepth: vp.zfar === undefined ? 1 : num(vp.zfar),
    };
  }
  return { x: num(vp.x), y: num(vp.y), width: num(vp.width), height: num(vp.height), minDepth: num(vp.minDepth), maxDepth: vp.maxDepth === undefined ? 1 : num(vp.maxDepth) };
}

/** MTLCompareFunction, which the capture records as its number. */
const METAL_COMPARE = ["Never", "Less", "Equal", "LessEqual", "Greater", "NotEqual", "GreaterEqual", "Always"];

/**
 * The rasterizer state of a draw, from wherever its API keeps it. `defaultViewport` stands in when
 * the draw set none: a Metal pass with no `setViewport:` covers its whole render target, which the
 * caller knows the size of and this does not.
 */
export function rasterStateOf(state: DrawState, defaultViewport?: RasterState["viewport"]): RasterState {
  const viewport = viewportOf(state) ?? defaultViewport ?? null;
  if (state.pipeline && isD3D12Type(state.pipeline.type)) {
    // D3D12: the pipeline state's rasterizer and depth-stencil descriptions. Clockwise is the
    // front face unless FrontCounterClockwise says otherwise, and clip +Y is the top of the target.
    const d = state.pipeline.descriptor;
    const raster = isObject(d?.RasterizerState) ? d!.RasterizerState : null;
    const ds = isObject(d?.DepthStencilState) ? d!.DepthStencilState : null;
    const cull = str(raster?.CullMode);
    const enabled = ds?.DepthEnable === true || ds?.DepthEnable === 1;
    const compare = enabled ? str(ds?.DepthFunc) : "";
    return {
      viewport,
      cullFront: cull.endsWith("_FRONT"),
      cullBack: cull.endsWith("_BACK"),
      ccwFront: raster?.FrontCounterClockwise === true || raster?.FrontCounterClockwise === 1,
      yUp: true,
      depthPrefers: compare.includes("LESS") ? "less" : compare.includes("GREATER") ? "greater" : "none",
    };
  }
  if (state.pipeline?.type.startsWith("MTL") || state.cullMode !== null || state.frontFace !== null) {
    // Metal: MTLCullModeNone is the default, and clockwise is the default front face. The
    // depth-stencil state's compare function is serialized as its MTLCompareFunction number.
    const cull = str(state.cullMode);
    const winding = str(state.frontFace);
    const compare = state.depthStencil?.descriptor?.depthCompareFunction;
    const compareName = typeof compare === "number" ? METAL_COMPARE[compare] ?? "" : str(compare);
    return {
      viewport,
      cullFront: cull.includes("Front"),
      cullBack: cull.includes("Back"),
      ccwFront: winding.includes("CounterClockwise"),
      yUp: true,
      depthPrefers: compareName.includes("Less") ? "less" : compareName.includes("Greater") ? "greater" : "none",
    };
  }
  const d = state.pipeline?.descriptor;
  const raster = isObject(d?.pRasterizationState) ? d!.pRasterizationState : null;
  const ds = isObject(d?.pDepthStencilState) ? d!.pDepthStencilState : null;
  const cull = str(dynamicValue(state, "cullMode", raster?.cullMode));
  const face = str(dynamicValue(state, "frontFace", raster?.frontFace));
  const testEnabled = dynamicValue(state, "depthTest", ds?.depthTestEnable);
  const compare = testEnabled === true || testEnabled === 1 ? str(dynamicValue(state, "depthCompare", ds?.depthCompareOp)) : "";
  return {
    viewport,
    cullFront: cull.includes("FRONT"),
    cullBack: cull.includes("BACK"),
    ccwFront: !face.includes("CLOCKWISE") || face.includes("COUNTER"),
    yUp: false,
    depthPrefers: compare.includes("LESS") ? "less" : compare.includes("GREATER") ? "greater" : "none",
  };
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
      const outputs = new Map<string, number[]>();
      for (const [key, value] of a.outputs) outputs.set(key, lerp(value, b.outputs.get(key) ?? value));
      out.push({ clip: lerp(a.clip, b.clip), outputs });
    }
  }
  return out;
}

export interface Covering {
  primitive: number;
  /** Window-space vertices of the (clipped) triangle covering the pixel. */
  window: { x: number; y: number; z: number; invW: number; v: ClipVertex }[];
  front: boolean;
  /** The provoking vertex's outputs (flat inputs). */
  provoking: ClipVertex;
}

/**
 * How a fragment's inputs are matched to a vertex's outputs. Vulkan pairs them by location; MSL
 * pairs the members of the two structs by name, so each API says which key an output has.
 */
export type VaryingKey = (output: MeshOutputVariable) => string | null;

/** Vulkan's: a varying is its location. */
export const locationKey: VaryingKey = (o) => (o.location === undefined ? null : String(o.location));

function edge(ax: number, ay: number, bx: number, by: number, px: number, py: number): number {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/** Finds the triangle whose fragment wins at the pixel: covering its center, not culled, front-most by the depth test. */
export function coveringTriangle(raster: RasterState, mesh: MeshOutput, px: number, py: number, keyOf: VaryingKey = locationKey): { hit: Covering | null; triangles: number; reason: string } {
  const viewport = raster.viewport;
  const pos = positionOutput(mesh);
  if (!viewport) return { hit: null, triangles: 0, reason: "the draw has no viewport" };
  if (!pos || !mesh.data) return { hit: null, triangles: 0, reason: "the draw's vertex shader outputs have no position" };
  if (primitiveKind(mesh.topology) !== "triangles") return { hit: null, triangles: 0, reason: "the draw does not draw triangles" };
  const view = new DataView(mesh.data.buffer, mesh.data.byteOffset, mesh.data.byteLength);
  const vertex = (i: number): ClipVertex => {
    const base = i * mesh.stride;
    const read = (offset: number, components: number, base2: string): number[] =>
      Array.from({ length: components }, (_, k) => (base2 === "float" ? view.getFloat32(base + offset + k * 4, true) : base2 === "int" ? view.getInt32(base + offset + k * 4, true) : view.getUint32(base + offset + k * 4, true)));
    const outputs = new Map<string, number[]>();
    for (const o of mesh.outputs) {
      const key = keyOf(o);
      if (key !== null) outputs.set(key, read(o.offset, o.components, o.base));
    }
    return { clip: read(pos.offset, 4, "float"), outputs };
  };
  const prefersLess = raster.depthPrefers === "less";
  const prefersGreater = raster.depthPrefers === "greater";
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
        y: viewport.y + (raster.yUp ? 1 - v.clip[1] / w : v.clip[1] / w + 1) * viewport.height / 2,
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
    const front = ccw === raster.ccwFront;
    if ((raster.cullBack && !front) || (raster.cullFront && front)) continue;
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

/** How a fragment shader wants a varying interpolated. */
export type Interpolation = "smooth" | "flat" | "noperspective";

/**
 * The varyings at a pixel center from a covering triangle, interpolated as the fragment shader
 * asks, with the position the fragment is at. Extended past the triangle's edges rather than
 * clipped, because the quad's other three pixels may fall outside it and a GPU shades them anyway.
 */
export function interpolate(hit: Covering, px: number, py: number, interpolationOf: (key: string) => Interpolation): {
  values: Map<string, number[]>;
  /** The x, y, depth and 1/w a fragment reads as its position. */
  fragCoord: [number, number, number, number];
} {
  const [a, b, c] = hit.window;
  const cx = px + 0.5, cy = py + 0.5;
  const total = edge(a.x, a.y, b.x, b.y, c.x, c.y);
  const l = [edge(b.x, b.y, c.x, c.y, cx, cy) / total, edge(c.x, c.y, a.x, a.y, cx, cy) / total, edge(a.x, a.y, b.x, b.y, cx, cy) / total];
  const invW = l[0] * a.invW + l[1] * b.invW + l[2] * c.invW;
  const persp = [l[0] * a.invW / invW, l[1] * b.invW / invW, l[2] * c.invW / invW];
  const z = l[0] * a.z + l[1] * b.z + l[2] * c.z;
  const values = new Map<string, number[]>();
  for (const [key, provoking] of hit.provoking.outputs) {
    const how = interpolationOf(key);
    if (how === "flat") {
      values.set(key, provoking);
      continue;
    }
    const weights = how === "noperspective" ? l : persp;
    const va = a.v.outputs.get(key) ?? provoking, vb = b.v.outputs.get(key) ?? provoking, vc = c.v.outputs.get(key) ?? provoking;
    values.set(key, va.map((x, k) => weights[0] * x + weights[1] * vb[k] + weights[2] * vc[k]));
  }
  return { values, fragCoord: [cx, cy, z, invW] };
}

/** A Vulkan fragment's inputs: the interpolated varyings by location, and its built-ins. */
function fragmentInputs(module: SpirvModule, hit: Covering, px: number, py: number): InvocationInputs {
  const decorations = new Map<string, Interpolation>();
  for (const [id, g] of module.globals) {
    if (g.storage !== StorageClass.Input) continue;
    const location = module.decoration(id, Decoration.Location)?.[0];
    if (location === undefined) continue;
    decorations.set(String(location), module.decoration(id, Decoration.Flat) !== undefined ? "flat"
      : module.decoration(id, Decoration.NoPerspective) !== undefined ? "noperspective" : "smooth");
  }
  const { values, fragCoord } = interpolate(hit, px, py, (key) => decorations.get(key) ?? "smooth");
  const locations = new Map<number, number[]>();
  for (const [key, value] of values) locations.set(Number(key), value);
  const builtins = new Map<number, Value>([
    [BuiltIn.FragCoord, fragCoord],
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
// Vertex outputs interpreted rather than replayed
//
// Metal and D3D12 captures have no replay. Their fragment inputs come from running the draw's
// own vertex shader in the interpreter, once per vertex, and packing what it wrote the way a
// replay's transform feedback would be, so the rasterizer above reads both alike. Each API's half
// runs its own interpreter over its own inputs; the packing is here.

/** Scalars of a value in order, padded or cut to a count. */
export function scalarsOf(value: Value | undefined, components: number): number[] {
  const flat: number[] = [];
  const walk = (v: Value | undefined): void => {
    if (Array.isArray(v)) {
      for (const x of v) walk(x);
      return;
    }
    flat.push(typeof v === "number" ? v : typeof v === "bigint" ? Number(v) : v === true ? 1 : 0);
  };
  walk(value);
  return Array.from({ length: components }, (_, i) => flat[i] ?? 0);
}

/** The vertex each position of an expanded list reads, for a strip or a fan. */
export function expandTopology(topology: string, vertices: number): number[] {
  if (/TRIANGLE_STRIP/.test(topology)) {
    const out: number[] = [];
    for (let i = 0; i + 2 < vertices; i++) out.push(i, i + 1 + (i % 2), i + 2 - (i % 2));
    return out;
  }
  if (/TRIANGLE_FAN/.test(topology)) {
    const out: number[] = [];
    for (let i = 0; i + 2 < vertices; i++) out.push(0, i + 1, i + 2);
    return out;
  }
  if (/LINE_STRIP/.test(topology)) {
    const out: number[] = [];
    for (let i = 0; i + 1 < vertices; i++) out.push(i, i + 1);
    return out;
  }
  return Array.from({ length: vertices }, (_, i) => i);
}

/**
 * The records an interpreted vertex shader produced (`perInstance` per instance, `instanceCount`
 * instances, each record the outputs' scalars in order) packed as a MeshOutput. Strips and fans
 * become lists here rather than in the rasterizer, which walks vertices three at a time; each
 * instance is expanded on its own, since a strip does not run from one instance into the next.
 */
export function packInterpretedMesh(cmd: CaptureCommand, topology: string, outputs: MeshOutputVariable[], stride: number, records: number[][],
                                    perInstance: number, instanceCount: number, truncated: boolean, notes: string[]): MeshOutput {
  const order: number[] = [];
  for (let instance = 0; instance < instanceCount; instance++) {
    const base = instance * perInstance;
    for (const at of expandTopology(topology, perInstance)) order.push(base + at);
  }
  const data = new Uint8Array(order.length * stride);
  const view = new DataView(data.buffer);
  const baseAt: MeshOutputVariable["base"][] = [];
  for (const o of outputs) for (let k = 0; k < o.components; k++) baseAt[o.offset / 4 + k] = o.base;
  order.forEach((from, to) => {
    const record = records[from] ?? [];
    for (let i = 0; i < stride / 4; i++) {
      const value = record[i] ?? 0;
      if (baseAt[i] === "int") view.setInt32(to * stride + i * 4, value | 0, true);
      else if (baseAt[i] === "uint") view.setUint32(to * stride + i * 4, value >>> 0, true);
      else view.setFloat32(to * stride + i * 4, value, true);
    }
  });
  return {
    command: cmd.index, method: cmd.method, frame: cmd.frame, commandBuffer: cmd.object?.__id ?? 0, passIndex: 0,
    measured: true, topology: /LINE/.test(topology) ? "VK_PRIMITIVE_TOPOLOGY_LINE_LIST" : /POINT/.test(topology) ? topology : "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST",
    stride, vertices: order.length, truncated, outputs, data,
    note: notes.length ? notes.join(" ") : undefined,
  };
}

/** The pass a command belongs to, counted among its command buffer's (or encoder's, or list's) pass beginnings. */
export function passOfCommand(data: CaptureData, cmd: CaptureCommand): { commandBuffer: number; passIndex: number } | null {
  const sets = data.sets;
  const commandBuffer = cmd.object?.__id ?? 0;
  let passIndex = -1;
  for (let i = 0; i <= cmd.index; i++) {
    const c = data.commands[i];
    if (!c || (c.object?.__id ?? 0) !== commandBuffer) continue;
    if (sets.PASS_BEGIN.has(c.method)) passIndex++;
  }
  return passIndex < 0 ? null : { commandBuffer, passIndex };
}

/** The render target's value at the pixel after the pass, for the end-of-run comparison (Metal and D3D12 passes). */
export function passPixel(ctx: DebugContext, cmd: CaptureCommand, x: number, y: number): DebugSession["targetPixel"] {
  const passInfo = passOfCommand(ctx.data, cmd);
  if (!passInfo) return undefined;
  const color = ctx.data.texturesForPass(cmd.frame, passInfo.commandBuffer, passInfo.passIndex)
    .find((t) => t.info.aspect === "color" && !t.info.resolve && t.data);
  if (!color?.data || x >= color.info.width || y >= color.info.height) return undefined;
  const texels = decodeTexels({ format: color.info.format, aspect: "color", width: color.info.width, height: color.info.height }, color.data);
  if (!texels) return undefined;
  const o = (y * texels.width + x) * 4;
  return {
    image: color.info.id, attachment: color.info.attachment,
    value: Array.from(texels.values.subarray(o, o + Math.min(4, Math.max(texels.channels, 1)))),
    format: color.info.format,
  };
}

/** Whether a draw's vertex outputs are interpreted here (Metal, D3D12) rather than replayed (Vulkan). */
export function interpretsVertexOutputs(state: DrawState): boolean {
  return isMetalPipeline(state.pipeline) || isD3D12Pipeline(state.pipeline);
}

/**
 * A draw's vertex shader outputs, from wherever its API gets them: the Vulkan replay through
 * `ctx.meshOutput`, or the interpreter itself for a Metal or D3D12 draw. What a fragment's inputs
 * are rasterized from, and what the pixel offered by default is found in.
 */
export function vertexOutputsOf(ctx: DebugContext, cmd: CaptureCommand, state: DrawState): Promise<MeshOutput> {
  if (isMetalPipeline(state.pipeline)) return interpretedMeshOutput(ctx, cmd, state);
  if (isD3D12Pipeline(state.pipeline)) return interpretedD3D12MeshOutput(ctx, cmd, state);
  if (!ctx.meshOutput) return Promise.reject(new Error("a fragment's inputs come from replaying the draw's vertex shader, and no replay is available here"));
  return ctx.meshOutput(cmd.index);
}

/** The rasterizer state a pixel is looked for with, including the API's default viewport where a draw set none. */
export function pixelRasterState(ctx: DebugContext, cmd: CaptureCommand, state: DrawState): RasterState {
  if (isMetalPipeline(state.pipeline)) return metalRasterState(ctx, cmd, state);
  if (isD3D12Pipeline(state.pipeline)) return d3d12RasterState(ctx, cmd, state);
  return rasterStateOf(state);
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
  // A Metal pipeline's shaders are Metal Shading Language, run by a different interpreter; a D3D12
  // pipeline state's are HLSL compiled to SPIR-V for this one.
  if (isMetalPipeline(state.pipeline)) return prepareMetalSession(ctx, target, state, cmd);
  if (isD3D12Pipeline(state.pipeline)) return prepareD3D12Session(ctx, target, state, cmd);
  const { source, bytes, module: captured } = stageOf(ctx, state, target.stage);
  const bindings = commandBindings(ctx, state, source);
  const model = STAGE_MODEL[target.stage];
  const notes: string[] = [];
  const entryPoint = source.entryPoint;
  const a = cmd.args ?? {};
  // A translation is what the tab steps; the capture's module still decides the invocation's
  // shape (the local size) and is what it is checked against.
  const translated = ctx.translate ? new SpirvModule(await ctx.translate(bytes, source)) : null;
  const module = translated ?? captured;
  if (translated) notes.push(TRANSLATION_NOTE);
  const program = SpirvProgram.of(module);
  const both = (start: (m: SpirvModule) => Stepper): Pick<DebugSession, "start" | "original"> => ({
    start: () => start(module),
    original: translated ? () => start(captured) : undefined,
  });

  if (target.stage === "compute") {
    const entry = captured.entryPoint(entryPoint, model);
    let localSize: [number, number, number] = [1, 1, 1];
    const literal = entry?.modes.get(17);
    const ids = entry?.modes.get(38);
    if (literal) localSize = [literal[0] ?? 1, literal[1] ?? 1, literal[2] ?? 1];
    else if (ids) localSize = ids.map((id) => Number(captured.constants.get(id) ?? 1)) as [number, number, number];
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
      target, program, stage: source, bindings, notes,
      description: `invocation (${g.join(", ")}) of a ${groups.join(" x ")} dispatch with local size ${localSize.join(" x ")}`,
      limits: { groups, localSize },
      ...both((m) => new Invocation(m, { entryPoint, model, bindings, inputs })),
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
      target, program, stage: source, bindings, notes, replayedOutputs,
      description: `vertex ${order} of the draw (gl_VertexIndex ${vertexId}), instance ${target.instance}`,
      limits: { vertices: input.ids.length, instances: Math.max(1, num(a.instanceCount) || 1) },
      ...both((m) => new Invocation(m, { entryPoint, model, bindings, inputs })),
    };
  }

  // A fragment.
  if (!ctx.meshOutput) throw new Error("a fragment's inputs come from replaying the draw's vertex shader, and no replay is available here");
  const mesh = await ctx.meshOutput(target.command);
  if (!mesh.measured) throw new Error(`the draw's vertex shader outputs could not be captured: ${mesh.note ?? "the replay did not reach the draw"}`);
  const { x, y } = target;
  const raster = rasterStateOf(state);
  const { hit, triangles, reason } = coveringTriangle(raster, mesh, x, y);
  if (!hit) throw new Error(reason);
  if (triangles !== Math.floor(mesh.vertices / 3)) notes.push("The replay truncated the draw's vertices.");
  const viewport = raster.viewport;
  const { x0, y0, target: lane } = PixelQuad.place(x, y);
  // The render target's value at the pixel after the pass, for comparison.
  let targetPixel: DebugSession["targetPixel"];
  const passInfo = findPass(data, cmd);
  const color = passInfo
    ? data.texturesForPass(cmd.frame, passInfo.passBegin.object?.__id ?? 0, passInfo.passIndex).find((t) => t.info.aspect === "color" && !t.info.resolve && t.data)
    : undefined;
  if (color?.data && x < color.info.width && y < color.info.height) {
    const texels = decodeTexels({ format: color.info.format, aspect: "color", width: color.info.width, height: color.info.height }, color.data);
    if (texels) {
      const o = (y * texels.width + x) * 4;
      targetPixel = { image: color.info.id, attachment: color.info.attachment, value: Array.from(texels.values.subarray(o, o + Math.min(4, Math.max(texels.channels, 1)))), format: color.info.format };
    }
  }
  return {
    target, program, stage: source, bindings, notes, targetPixel,
    description: `pixel (${x}, ${y}), from triangle ${hit.primitive.toLocaleString()} of ${triangles.toLocaleString()} (${hit.front ? "front" : "back"} facing)`,
    limits: { width: viewport ? Math.abs(viewport.width) : undefined, height: viewport ? Math.abs(viewport.height) : undefined },
    ...both((m) => new PixelQuad((dx, dy, derivatives: DerivativeSource) => new Invocation(m, {
      entryPoint, model, bindings, inputs: fragmentInputs(m, hit, x0 + dx, y0 + dy), derivatives,
    }), lane)),
  };
}

/** A pixel the draw covers, to open a fragment debugger on: the center of its first front-facing visible triangle. */
export function coveredPixel(state: DrawState, mesh: MeshOutput, raster: RasterState = rasterStateOf(state)): { x: number; y: number } | null {
  const viewport = raster.viewport;
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
      const ndcY = view.getFloat32(at + 4, true) / w;
      sx += viewport.x + (view.getFloat32(at, true) / w + 1) * viewport.width / 2;
      sy += viewport.y + (raster.yUp ? 1 - ndcY : ndcY + 1) * viewport.height / 2;
    }
    if (!ok) continue;
    const x = Math.floor(sx / 3), y = Math.floor(sy / 3);
    const minX = Math.min(viewport.x, viewport.x + viewport.width), minY = Math.min(viewport.y, viewport.y + viewport.height);
    if (x < minX || y < minY || x >= minX + Math.abs(viewport.width) || y >= minY + Math.abs(viewport.height)) continue;
    if (coveringTriangle(raster, mesh, x, y).hit) return { x, y };
  }
  return null;
}
