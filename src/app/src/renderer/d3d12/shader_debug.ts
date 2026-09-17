// Everything a shader debugger session needs from a *Direct3D 12* capture: the stage's HLSL made
// steppable, the root parameters the command had bound, and the invocation's inputs. The Vulkan
// half and the rasterizer all three APIs share are in ../shader_debug_setup.ts; the Metal half is
// in ../metal/shader_debug.ts.
//
// There is no DXIL interpreter here. What a D3D12 capture holds for a stage is a DXBC/DXIL
// container, and what the container holds (when the build used -Zi, or left a PDB the symbol
// directories find) is the HLSL it was compiled from. That HLSL is compiled again, to SPIR-V, by
// dxc on this machine (main/shader_tools.ts compileHlslForDebugging), and the result is stepped in
// the SPIR-V interpreter exactly as a Vulkan capture's module is: source lines, locals, resources.
// It is the same source the GPU ran, so it should compute the same values, but it is not the same
// module, and unlike the Vulkan decompile route there is no original to run beside it.
//
// Three things tie the translation back to the capture:
//
//   * Registers. dxc maps a register space to a descriptor set and shifts each register class
//     (b, t, s, u) to its own range of bindings (shared/hlsl_debug.ts). A binding the interpreter
//     asks for is turned back into a register and looked up in the draw's root descriptor tables,
//     root views, root constants and static samplers, which the capture keys by register and space.
//   * Semantics. dxc numbers stage inputs and outputs by declaration order, which need not agree
//     between a vertex shader and a pixel shader, so `-fspv-reflect` keeps each variable's HLSL
//     semantic and the two are paired by that: a vertex input to the input layout's element, a
//     pixel input to the vertex shader's output.
//   * The rasterizer. There is no replay; the draw's vertex shader is run in the interpreter, once
//     per vertex, and the shared rasterizer finds the triangle covering the pixel, with D3D's
//     conventions (clip +Y up, clockwise front by default) read from the pipeline state.
import { PixelQuad, type DerivativeSource } from "../debug/quad.js";
import type { DebugSampler, DebugTexture, Value } from "../debug/values.js";
import type { DrawState } from "../draw_state.js";
import { meshInput, type MeshInput } from "../mesh_input.js";
import type { MeshOutput, MeshOutputVariable } from "../mesh_output.js";
import {
  coveringTriangle, debugTexture, interpolate, packInterpretedMesh, passOfCommand, passPixel, rasterStateOf, scalarsOf,
  type Covering, type DebugContext, type DebugSession, type DebugTarget, type Interpolation, type RasterState,
} from "../shader_debug_setup.js";
import { stateStages, type StageSource } from "../shader_cache.js";
import { Invocation, type InvocationInputs, type ShaderBindings } from "../spirv/interpreter.js";
import { BuiltIn, Decoration, ExecutionModel, SpirvModule, StorageClass, literalString } from "../spirv/module.js";
import { SpirvProgram } from "../spirv/program.js";
import { scalarOf } from "../spirv/values.js";
import { d3d12InputElements, isD3D12Type, type D3D12InputElement } from "./d3d12_object.js";
import { hlslBindingName, hlslRegisterOf } from "../../shared/hlsl_debug.js";
import { isObject, num, refId, str, type ObjectLookup, type VulkanObject } from "../vulkan/vulkan_object.js";
import type { ArgObject, ArgValue, CaptureCommand, CaptureDescriptor, CaptureDescriptorBinding } from "../../shared/protocol.js";

/** Vertices the draw's vertex shader is run for, to rasterize a fragment. */
const MAX_INTERPRETED_VERTICES = 20_000;

/** SPV_GOOGLE_hlsl_functionality1: the HLSL semantic a stage variable was declared with. */
const DECORATION_USER_SEMANTIC = 5635;

type Stage = "vertex" | "fragment" | "compute";

const STAGE_MODEL = { vertex: ExecutionModel.Vertex, fragment: ExecutionModel.Fragment, compute: ExecutionModel.GLCompute } as const;

/** What a session built through here says about the code it steps. */
export const D3D12_TRANSLATION_NOTE = "This steps the HLSL the capture holds for the stage, compiled to SPIR-V by dxc on this machine, " +
  "rather than the DXIL the GPU ran: the same source, so it should compute the same values, but there is no DXIL interpreter to check it against.";

/** Whether a command's pipeline is a D3D12 pipeline state, and so debugged through here. */
export function isD3D12Pipeline(pipeline: VulkanObject | null | undefined): boolean {
  return !!pipeline && isD3D12Type(pipeline.type);
}

/** "TEXCOORD1", "SV_POSITION0": a semantic upper-cased, with the index a bare name leaves out. */
export function normalizeSemantic(semantic: string): string {
  const s = semantic.trim().toUpperCase();
  return /\d$/.test(s) ? s : `${s}0`;
}

/** The HLSL semantic a translated module's stage variable carries, null for one without. */
export function semanticOf(module: SpirvModule, id: number): string | null {
  const words = module.decoration(id, DECORATION_USER_SEMANTIC);
  if (!words || !words.length) return null;
  const text = literalString(new Uint32Array(words), 0).text;
  return text ? normalizeSemantic(text) : null;
}

// ---------------------------------------------------------------------------------------------
// The stage

export interface D3D12Stage {
  source: StageSource;
  module: SpirvModule;
  program: SpirvProgram;
  entryPoint: string;
}

/** The compiled stages of a capture, so a pixel's vertex shader is compiled once for the pixel search and the session. */
const _stages = new WeakMap<DebugContext["data"], Map<string, Promise<D3D12Stage>>>();

/**
 * A pipeline state's stage as the debugger steps it: its container fetched from the capture (or
 * the layer), its HLSL compiled to SPIR-V through the context's compiler, and read into a module.
 */
export function d3d12Stage(ctx: DebugContext, state: DrawState, stage: Stage): Promise<D3D12Stage> {
  const pipeline = state.pipeline;
  if (!pipeline) throw new Error("no pipeline state is bound at the command");
  const source = stateStages(state, ctx.db).find((s) => s.stage === stage);
  if (!source) throw new Error(`the pipeline state has no ${stage} stage`);
  const key = `${source.object.id}:${source.blobIndex}:${source.entryPoint}`;
  let byKey = _stages.get(ctx.data);
  if (!byKey) _stages.set(ctx.data, (byKey = new Map()));
  let stagePromise = byKey.get(key);
  if (!stagePromise) {
    stagePromise = (async (): Promise<D3D12Stage> => {
      if (!ctx.compileHlsl) throw new Error("a D3D12 shader is stepped as its HLSL compiled to SPIR-V, and no compiler is available here");
      const bytes = ctx.db.blobData.get(`${source.object.id}:${source.blobIndex}`) ?? await ctx.fetchBlob?.(source.object.id, source.blobIndex) ?? null;
      if (!bytes) throw new Error(`the capture does not hold the ${stage} shader's bytecode`);
      const reflection = pipeline.descriptor?.reflection;
      const target = isObject(reflection) && isObject(reflection[stage]) ? str(reflection[stage].target) : "";
      const spirv = await ctx.compileHlsl(bytes, source, target);
      const module = new SpirvModule(spirv);
      const program = SpirvProgram.of(module);
      // The resources table names a binding the way the HLSL does ("t0", not "set 0 binding 65536").
      program.bindingName = hlslBindingName;
      return { source, module, program, entryPoint: source.entryPoint };
    })();
    // A failure is not kept: the tools may be there on the next try.
    stagePromise.catch(() => byKey!.delete(key));
    byKey.set(key, stagePromise);
  }
  return stagePromise;
}

/**
 * dxc wraps the HLSL entry point: the SPIR-V entry point loads the stage inputs into a struct,
 * calls the function the source wrote (`src.<name>`), and stores its outputs. Stepping starts
 * inside that function, so the first line is the shader's first line and Step Over does not run
 * the whole shader as one call.
 */
function enteredSource(inv: Invocation, entryPoint: string): Invocation {
  const target = `src.${entryPoint}`;
  let wrapped = false;
  for (const name of inv.module.names.values()) {
    if (name === target) {
      wrapped = true;
      break;
    }
  }
  if (!wrapped) return inv;
  let guard = 0;
  while (!inv.finished && inv.callStack()[0]?.name !== target && guard++ < 100_000) {
    if (inv.step() === "blocked") break;
  }
  inv.takeResults();
  return inv;
}

// ---------------------------------------------------------------------------------------------
// Bindings

/** MTLSamplerAddressMode has its own table in the Metal half; these are D3D12_TEXTURE_ADDRESS_MODE_*. */
const ADDRESS: Record<string, DebugSampler["address"][number]> = { WRAP: "repeat", MIRROR: "mirror", CLAMP: "clamp", BORDER: "border", MIRROR_ONCE: "mirrorClamp" };

const COMPARE: Record<string, string> = {
  NEVER: "VK_COMPARE_OP_NEVER", LESS: "VK_COMPARE_OP_LESS", EQUAL: "VK_COMPARE_OP_EQUAL", LESS_EQUAL: "VK_COMPARE_OP_LESS_OR_EQUAL",
  GREATER: "VK_COMPARE_OP_GREATER", NOT_EQUAL: "VK_COMPARE_OP_NOT_EQUAL", GREATER_EQUAL: "VK_COMPARE_OP_GREATER_OR_EQUAL", ALWAYS: "VK_COMPARE_OP_ALWAYS",
};

const STATIC_BORDERS: Record<string, number[]> = { TRANSPARENT_BLACK: [0, 0, 0, 0], OPAQUE_BLACK: [0, 0, 0, 1], OPAQUE_WHITE: [1, 1, 1, 1] };

/**
 * A D3D12_FILTER name taken apart: "MIN_POINT_MAG_LINEAR_MIP_POINT" names each stage's filter
 * after the stages it applies to, "ANISOTROPIC" is linear everywhere, and a COMPARISON_, MINIMUM_
 * or MAXIMUM_ prefix says how the taps are combined (only the comparison changes what is read).
 */
export function d3d12Filter(name: string): { min: "nearest" | "linear"; mag: "nearest" | "linear"; mip: "nearest" | "linear"; comparison: boolean } {
  let f = name.replace(/^D3D12_FILTER_/, "");
  const comparison = f.startsWith("COMPARISON_");
  f = f.replace(/^(COMPARISON|MINIMUM|MAXIMUM)_/, "");
  const out = { min: "nearest" as "nearest" | "linear", mag: "nearest" as "nearest" | "linear", mip: "nearest" as "nearest" | "linear", comparison };
  if (f.includes("ANISOTROPIC")) return { ...out, min: "linear", mag: "linear", mip: "linear" };
  const pending: ("min" | "mag" | "mip")[] = [];
  for (const token of f.split("_")) {
    if (token === "POINT" || token === "LINEAR") {
      for (const stage of pending) out[stage] = token === "POINT" ? "nearest" : "linear";
      pending.length = 0;
    } else if (token === "MIN" || token === "MAG" || token === "MIP") {
      pending.push(token.toLowerCase() as "min" | "mag" | "mip");
    }
  }
  return out;
}

/**
 * A D3D12_SAMPLER_DESC (a sampler descriptor's `samplerDesc`) or a D3D12_STATIC_SAMPLER_DESC (from
 * the root signature) as the interpreter reads it. The enums arrive by name.
 */
export function d3d12Sampler(desc: ArgValue | null | undefined): DebugSampler | null {
  if (!isObject(desc)) return null;
  const filter = d3d12Filter(str(desc.Filter));
  const address = (v: ArgValue | undefined): DebugSampler["address"][number] => ADDRESS[str(v).replace(/^D3D12_TEXTURE_ADDRESS_MODE_/, "")] ?? "clamp";
  let border: number[] = [0, 0, 0, 0];
  const b = desc.BorderColor ?? desc.FloatBorderColor ?? desc.UintBorderColor;
  if (Array.isArray(b)) border = b.map((x) => num(x));
  else if (typeof b === "string") border = STATIC_BORDERS[b.replace(/^D3D12_STATIC_BORDER_COLOR_/, "").replace(/_UINT$/, "")] ?? border;
  return {
    magFilter: filter.mag, minFilter: filter.min, mipmapMode: filter.mip,
    address: [address(desc.AddressU), address(desc.AddressV), address(desc.AddressW)],
    border,
    compareOp: filter.comparison ? COMPARE[str(desc.ComparisonFunc).replace(/^D3D12_COMPARISON_FUNC_/, "")] ?? null : null,
    minLod: num(desc.MinLOD),
    maxLod: desc.MaxLOD === undefined ? 1000 : Math.min(1000, num(desc.MaxLOD)),
    lodBias: num(desc.MipLODBias),
    unnormalized: false,
  };
}

/** The root signature's description (whichever version it was made with), from the object the draw bound or the pipeline names. */
function rootSignatureDesc(db: ObjectLookup, state: DrawState): ArgObject | null {
  const bound = [...state.sets.values()].map((s) => refId(s.set.layout)).find((id) => id !== null);
  const id = bound ?? refId(state.pipeline?.descriptor?.pRootSignature);
  const d = id !== null ? db.getObject(id)?.descriptor : null;
  if (!isObject(d)) return null;
  for (const key of ["Desc_1_2", "Desc_1_1", "Desc_1_0"]) if (isObject(d[key])) return d[key];
  return d;
}

const RANGE_SUFFIX = { b: "_CBV", t: "_SRV", s: "_SAMPLER", u: "_UAV" } as const;

/** An SRV's Shader4ComponentMapping as a VkComponentMapping, for debugTexture's swizzle; null for the identity. */
function componentMapping(view: ArgValue | null | undefined): ArgObject | null {
  if (!isObject(view) || typeof view.Shader4ComponentMapping !== "number") return null;
  const names = ["R", "G", "B", "A", "ZERO", "ONE"];
  const pick = (i: number): string => `VK_COMPONENT_SWIZZLE_${names[(view.Shader4ComponentMapping as number >> (3 * i)) & 7] ?? names[i]}`;
  const mapping = { r: pick(0), g: pick(1), b: pick(2), a: pick(3) };
  return [mapping.r, mapping.g, mapping.b, mapping.a].every((v, i) => v.endsWith(`_${names[i]}`)) ? null : mapping;
}

/** The bytes of a cbuffer bound as root constants: the parameter naming its register, and the values set for it, in order. */
function rootConstants(state: DrawState, root: ArgObject | null, space: number, register: number): Uint8Array | null {
  const params = Array.isArray(root?.pParameters) ? root!.pParameters : [];
  const index = params.findIndex((p) => isObject(p) && str(p.ParameterType).endsWith("32BIT_CONSTANTS") && isObject(p.Constants)
    && num(p.Constants.ShaderRegister) === register && num(p.Constants.RegisterSpace) === space);
  if (index < 0) return null;
  const updates = state.pushConstants.filter((p) => num(p.cmd.args?.RootParameterIndex) === index);
  if (!updates.length) return null;
  const constants = params[index] as ArgObject;
  const size = Math.max(num((constants.Constants as ArgObject).Num32BitValues) * 4, ...updates.map((p) => p.offset + p.size));
  const out = new Uint8Array(size);
  for (const p of updates) if (p.data) out.set(p.data.subarray(0, Math.min(p.data.byteLength, size - p.offset)), p.offset);
  return out;
}

/**
 * What a D3D12 draw or dispatch had bound, as the SPIR-V interpreter reads it: a set and binding of
 * the translated module turned back into a register, and the register found in the root descriptor
 * tables and root views the command bound (keyed by register and space, like the reflection), in the
 * root constants set for a constants parameter naming it, or among the root signature's static samplers.
 */
export function d3d12Bindings(ctx: DebugContext, state: DrawState): ShaderBindings {
  const root = rootSignatureDesc(ctx.db, state);
  const textures = new Map<string, DebugTexture | null>();
  const find = (set: number, binding: number, element: number): { binding: CaptureDescriptorBinding; descriptor: CaptureDescriptor | null } | null => {
    const { kind, register, space } = hlslRegisterOf(set, binding);
    const suffix = RANGE_SUFFIX[kind];
    for (const bound of state.sets.values()) {
      for (const b of bound.set.bindings) {
        if (num(b.space) !== space || !b.type.endsWith(suffix)) continue;
        const at = register + element - num(b.register);
        if (at < 0 || at >= Math.max(1, b.descriptors.length)) continue;
        return { binding: b, descriptor: b.descriptors[at] ?? null };
      }
    }
    return null;
  };
  return {
    buffer: (set, binding, element) => {
      const { kind, register, space } = hlslRegisterOf(set, binding);
      const d = find(set, binding, element)?.descriptor;
      if (d?.data !== undefined && d.data !== null) return ctx.data.buffer(d.data)?.data ?? null;
      return kind === "b" ? rootConstants(state, root, space, register) : null;
    },
    texture: (set, binding, element) => {
      const key = `${set}/${binding}/${element}`;
      if (textures.has(key)) return textures.get(key) ?? null;
      const d = find(set, binding, element)?.descriptor;
      let tex: DebugTexture | null = null;
      if (d) {
        // The read-back the capture made for this descriptor, else any contents it holds for the resource.
        const captured = ctx.data.capturedImage(d.data) ?? ctx.data.imageContents(refId(d.resource) ?? 0);
        tex = captured ? debugTexture(captured, componentMapping(d.view) ?? undefined) : null;
      }
      textures.set(key, tex);
      return tex;
    },
    sampler: (set, binding, element) => {
      const d = find(set, binding, element)?.descriptor;
      if (d?.samplerDesc) return d3d12Sampler(d.samplerDesc);
      const { register, space } = hlslRegisterOf(set, binding);
      const statics = Array.isArray(root?.pStaticSamplers) ? root!.pStaticSamplers : [];
      const s = statics.find((x) => isObject(x) && num(x.ShaderRegister) === register + element && num(x.RegisterSpace) === space);
      return d3d12Sampler(s);
    },
    label: (set, binding, element) => hlslBindingName(set, binding + element),
    pushConstants: null,
    specialization: new Map(),
  };
}

// ---------------------------------------------------------------------------------------------
// Inputs

/** The translated vertex shader's inputs paired with the input layout's elements, by semantic: location -> attribute index in the mesh input. */
function vertexAttributeMap(module: SpirvModule, elements: D3D12InputElement[], input: MeshInput, notes: string[]): Map<number, number> {
  const out = new Map<number, number>();
  for (const [id, g] of module.globals) {
    if (g.storage !== StorageClass.Input) continue;
    const location = module.decoration(id, Decoration.Location)?.[0];
    if (location === undefined) continue;
    const semantic = semanticOf(module, id);
    const element = semantic ? elements.find((e) => normalizeSemantic(e.name) === semantic) : undefined;
    const k = element ? input.attributes.findIndex((a) => a.location === element.location) : -1;
    if (k < 0) {
      notes.push(`The input layout has no element for the vertex shader's ${semantic ?? module.nameOf(id)}: it reads as zero.`);
      continue;
    }
    out.set(location, k);
  }
  return out;
}

function vertexInputs(input: MeshInput, attributeOf: Map<number, number>, order: number, instance: number, cmd: CaptureCommand): InvocationInputs {
  const a = cmd.args ?? {};
  const locations = new Map<number, number[]>();
  for (const [location, k] of attributeOf) {
    const values = input.values(order, k, instance);
    if (values) locations.set(location, values);
  }
  const vertexId = input.ids[order];
  const firstInstance = num(a.StartInstanceLocation);
  // SV_VertexID counts from the base vertex (the mesh input applied it); SV_InstanceID from zero,
  // without the start instance, which is where D3D differs from Vulkan's gl_InstanceIndex.
  return {
    locations,
    builtins: new Map<number, Value>([
      [BuiltIn.VertexIndex, vertexId], [BuiltIn.VertexId, vertexId], [BuiltIn.InstanceIndex, instance], [BuiltIn.InstanceId, instance],
      [BuiltIn.BaseVertex, num(a.BaseVertexLocation ?? a.StartVertexLocation)], [BuiltIn.BaseInstance, firstInstance], [BuiltIn.DrawIndex, 0],
      [BuiltIn.ViewIndex, 0],
    ]),
  };
}

/**
 * The draw's vertex shader run over its vertices, packed the way a replay's transform feedback
 * would be, so the shared rasterizer can find the triangle covering a pixel. Each output is
 * named by its semantic, which is what the pixel shader's inputs are matched by.
 */
export async function interpretedD3D12MeshOutput(ctx: DebugContext, cmd: CaptureCommand, state: DrawState): Promise<MeshOutput> {
  const { module, entryPoint } = await d3d12Stage(ctx, state, "vertex");
  const input = meshInput(ctx.data, ctx.db, cmd, ctx.inputNames ?? new Map());
  const bindings = d3d12Bindings(ctx, state);
  const notes = [...input.notes];
  const attributeOf = vertexAttributeMap(module, d3d12InputElements(state.pipeline), input, notes);
  const a = cmd.args ?? {};
  const entry = module.entryPoint(entryPoint, ExecutionModel.Vertex);
  if (!entry) throw new Error(`the translated vertex shader has no entry point ${entryPoint}`);

  // The outputs: the module's output variables, a record per vertex, four bytes a scalar.
  const outputs: MeshOutputVariable[] = [];
  const ids: { id: number; components: number }[] = [];
  let stride = 0;
  for (const id of entry.interface) {
    const g = module.globals.get(id);
    if (!g || g.storage !== StorageClass.Output) continue;
    const ptr = module.types.get(g.type);
    const pointee = ptr?.kind === "pointer" ? ptr.pointee : 0;
    const t = module.types.get(pointee);
    const components = t?.kind === "vector" ? t.count : t?.kind === "float" || t?.kind === "int" || t?.kind === "bool" ? 1 : 0;
    if (!components) continue;
    const builtin = module.decoration(id, Decoration.BuiltIn)?.[0];
    const location = module.decoration(id, Decoration.Location)?.[0];
    if (builtin !== undefined && builtin !== BuiltIn.Position) continue;
    if (builtin === undefined && location === undefined) continue;
    const s = scalarOf(module, pointee);
    outputs.push({
      name: builtin === BuiltIn.Position ? "SV_POSITION0" : semanticOf(module, id) ?? module.nameOf(id),
      offset: stride, components,
      base: s?.base === "int" ? "int" : s?.base === "uint" || s?.base === "bool" ? "uint" : "float",
      ...(builtin === BuiltIn.Position ? { builtin: "Position" } : {}),
      ...(location !== undefined ? { location } : {}),
    });
    ids.push({ id, components });
    stride += components * 4;
  }

  // Every instance, one after another, the way transform feedback writes them.
  const instances = Math.max(1, num(a.InstanceCount) || 1);
  const perInstance = Math.min(input.ids.length, Math.max(3, Math.floor(MAX_INTERPRETED_VERTICES / instances)));
  const instanceCount = Math.min(instances, Math.max(1, Math.floor(MAX_INTERPRETED_VERTICES / Math.max(1, perInstance))));
  const records: number[][] = [];
  const warnings = new Set<string>();
  for (let instance = 0; instance < instanceCount; instance++) {
    for (let order = 0; order < perInstance; order++) {
      const invocation = new Invocation(module, { entryPoint, model: ExecutionModel.Vertex, bindings, inputs: vertexInputs(input, attributeOf, order, instance, cmd) });
      invocation.run();
      for (const w of invocation.warnings) warnings.add(w);
      const written = invocation.outputs();
      records.push(ids.map(({ id, components }) => scalarsOf(written.find((v) => v.id === id)?.value, components)).flat());
    }
  }
  const truncated = perInstance < input.ids.length || instanceCount < instances;
  if (truncated) {
    notes.push(`The draw's vertex shader was run for ${perInstance.toLocaleString()} of its ${input.ids.length.toLocaleString()} vertices in ${instanceCount.toLocaleString()} of its ${instances.toLocaleString()} instances.`);
  }
  for (const w of warnings) notes.push(`Running the vertex shader: ${w}`);
  return packInterpretedMesh(cmd, input.topology, outputs, stride, records, perInstance, instanceCount, truncated, notes);
}

/**
 * A D3D12 draw's rasterizer state, with the viewport its render pass implies when the list set
 * none: the pipeline's cull mode and front face, the depth test, and clip +Y as the top of the
 * target, which is D3D's convention (rasterStateOf reads the pipeline state; this adds the fallback).
 */
export function d3d12RasterState(ctx: DebugContext, cmd: CaptureCommand, state: DrawState): RasterState {
  const pass = passOfCommand(ctx.data, cmd);
  const target = pass
    ? ctx.data.texturesForPass(cmd.frame, pass.commandBuffer, pass.passIndex).find((t) => t.info.aspect === "color")
    : undefined;
  const whole = target ? { x: 0, y: 0, width: target.info.width, height: target.info.height, minDepth: 0, maxDepth: 1 } : null;
  return rasterStateOf(state, whole);
}

/** A D3D12 fragment's inputs: the varyings interpolated by semantic, put at the pixel shader's locations, and its built-ins. */
function fragmentInputs(module: SpirvModule, hit: Covering, px: number, py: number): InvocationInputs {
  const interpolations = new Map<string, Interpolation>();
  const keys = new Map<number, string>();   // location -> semantic
  for (const [id, g] of module.globals) {
    if (g.storage !== StorageClass.Input) continue;
    const location = module.decoration(id, Decoration.Location)?.[0];
    if (location === undefined) continue;
    const key = semanticOf(module, id) ?? module.nameOf(id);
    keys.set(location, key);
    interpolations.set(key, module.decoration(id, Decoration.Flat) !== undefined ? "flat"
      : module.decoration(id, Decoration.NoPerspective) !== undefined ? "noperspective" : "smooth");
  }
  const { values, fragCoord } = interpolate(hit, px, py, (key) => interpolations.get(key) ?? "smooth");
  const locations = new Map<number, number[]>();
  for (const [location, key] of keys) {
    const v = values.get(key);
    if (v) locations.set(location, v);
  }
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
// Sessions

/** A D3D12 draw or dispatch prepared for the debugger. */
export async function prepareD3D12Session(ctx: DebugContext, target: DebugTarget, state: DrawState, cmd: CaptureCommand): Promise<DebugSession> {
  const stage: Stage = target.stage;
  const { source, module, program, entryPoint } = await d3d12Stage(ctx, state, stage);
  const bindings = d3d12Bindings(ctx, state);
  const model = STAGE_MODEL[stage];
  const notes: string[] = [D3D12_TRANSLATION_NOTE];
  const a = cmd.args ?? {};
  const start = (inputs: InvocationInputs, derivatives?: DerivativeSource): Invocation =>
    enteredSource(new Invocation(module, { entryPoint, model, bindings, inputs, derivatives }), entryPoint);

  if (target.stage === "compute") {
    const entry = module.entryPoint(entryPoint, model);
    const literal = entry?.modes.get(17);
    const localSize: [number, number, number] = literal ? [literal[0] ?? 1, literal[1] ?? 1, literal[2] ?? 1] : [1, 1, 1];
    const indirect = cmd.method === "ExecuteIndirect";
    const groups: [number, number, number] = indirect ? [1, 1, 1] : [Math.max(1, num(a.ThreadGroupCountX)), Math.max(1, num(a.ThreadGroupCountY)), Math.max(1, num(a.ThreadGroupCountZ))];
    if (indirect) notes.push("An indirect dispatch's group counts are in a buffer: they read as (1, 1, 1).");
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
      description: `thread (${g.join(", ")}) of a ${groups.join(" x ")} dispatch with thread groups of ${localSize.join(" x ")}`,
      limits: { groups, localSize },
      start: () => start(inputs),
    };
  }

  if (target.stage === "vertex") {
    const input = meshInput(ctx.data, ctx.db, cmd, ctx.inputNames ?? new Map());
    notes.push(...input.notes);
    const attributeOf = vertexAttributeMap(module, d3d12InputElements(state.pipeline), input, notes);
    const order = target.vertex;
    const instance = target.instance;
    if (order < 0 || order >= input.ids.length) throw new Error(`the draw reads ${input.ids.length.toLocaleString()} vertices: there is no vertex ${order}`);
    const inputs = vertexInputs(input, attributeOf, order, instance, cmd);
    return {
      target, program, stage: source, bindings, notes,
      description: `vertex ${order} of the draw (SV_VertexID ${input.ids[order]}), instance ${instance}`,
      limits: { vertices: input.ids.length, instances: Math.max(1, num(a.InstanceCount) || 1) },
      start: () => start(inputs),
    };
  }

  // A pixel: its inputs come from running the draw's own vertex shader, since there is no replay.
  const mesh = await interpretedD3D12MeshOutput(ctx, cmd, state);
  if (mesh.note) notes.push(mesh.note);
  const raster = d3d12RasterState(ctx, cmd, state);
  const { x, y } = target;
  const { hit, triangles, reason } = coveringTriangle(raster, mesh, x, y, (o) => (o.builtin === "Position" ? null : o.name));
  if (!hit) throw new Error(reason);
  const { x0, y0, target: lane } = PixelQuad.place(x, y);
  const targetPixel = passPixel(ctx, cmd, x, y);
  return {
    target, program, stage: source, bindings, notes, targetPixel,
    description: `pixel (${x}, ${y}), from triangle ${hit.primitive.toLocaleString()} of ${triangles.toLocaleString()} (${hit.front ? "front" : "back"} facing), whose vertices the interpreter ran the vertex shader for`,
    limits: { width: raster.viewport ? Math.abs(raster.viewport.width) : undefined, height: raster.viewport ? Math.abs(raster.viewport.height) : undefined },
    start: () => new PixelQuad((dx, dy, derivatives: DerivativeSource) => start(fragmentInputs(module, hit, x0 + dx, y0 + dy), derivatives), lane),
  };
}
