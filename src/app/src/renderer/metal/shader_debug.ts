// Everything a shader debugger session needs from a *Metal* capture: the entry point's Metal
// Shading Language, the buffers, textures and samplers the encoder had bound, and the invocation's
// inputs. The Vulkan half and the rasterizer both share are in ../shader_debug_setup.ts.
//
// Two things differ from the Vulkan path and are worth naming:
//
//   * There is no replay. A Vulkan fragment's inputs come from the replay's transform feedback;
//     here the draw's *vertex* shader is run in the same interpreter, once per vertex, and the
//     result is rasterized by the shared code. That costs an interpreted invocation per vertex, so
//     it is capped and cached per draw.
//   * A fragment's inputs are matched to a vertex's outputs by struct member name, not by a
//     location number, because that is how MSL pairs one stage's `[[stage_in]]` with the other's
//     return type.
import { MslProgram } from "../msl/program.js";
import { MslInvocation, type MslBindings, type MslFunctionConstants, type MslInputs } from "../msl/interpreter.js";
import { attributeNamed, type FunctionIr } from "../msl/ir.js";
import type { Attribute } from "../msl/types.js";
import { PixelQuad, type DerivativeSource } from "../debug/quad.js";
import type { Value } from "../debug/values.js";
import type { DebugSampler, DebugTexture } from "../debug/values.js";
import { meshInput } from "../mesh_input.js";
import { accelerationScene } from "../acceleration_scene.js";
import { buildRayScene, type RayFunctionTable, type RayScene } from "../msl/raytracing.js";
import type { MeshOutput, MeshOutputVariable } from "../mesh_output.js";
import {
  coveringTriangle, debugTexture, interpolate, packInterpretedMesh, passOfCommand, passPixel, rasterStateOf, scalarsOf,
  type Covering, type BasicTarget, type DebugContext, type DebugSession, type DebugTarget, type Interpolation, type RasterState,
} from "../shader_debug_setup.js";
import type { DrawState } from "../draw_state.js";
import type { StageSource } from "../shader_cache.js";
import { isObject, num, refId, str, type VulkanObject } from "../vulkan/vulkan_object.js";
import type { ArgValue, CaptureCommand } from "../../shared/protocol.js";

/** Vertices the draw's vertex shader is run for, to rasterize a fragment. */
const MAX_INTERPRETED_VERTICES = 20_000;

/** Which of a Metal pipeline's functions a stage debugs. */
const FUNCTION_KEY = { vertex: "vertexFunction", fragment: "fragmentFunction", compute: "function" } as const;

/** MSL's function qualifier for a stage. */
const QUALIFIER = { vertex: "vertex", fragment: "fragment", compute: "kernel" } as const;

type Stage = "vertex" | "fragment" | "compute";

export interface MetalStage {
  source: StageSource;
  program: MslProgram;
  entry: FunctionIr;
  /** What the application specialized the function with, from the capture. */
  constants: MslFunctionConstants;
}

/** Whether a command's pipeline is a Metal one, and so debugged through here. */
export function isMetalPipeline(pipeline: VulkanObject | null): boolean {
  return !!pipeline?.type.startsWith("MTL");
}

/**
 * The Metal Shading Language of a pipeline's stage: its MTLFunction names the entry point, and the
 * function's parent MTLLibrary carries the source the application compiled.
 */
export async function metalStage(ctx: DebugContext, state: DrawState, stage: Stage): Promise<MetalStage> {
  const pipeline = state.pipeline;
  if (!pipeline) throw new Error("no pipeline is bound at the command");
  const ref = pipeline.descriptor?.[FUNCTION_KEY[stage]] ?? (stage === "compute" ? pipeline.descriptor?.computeFunction : undefined);
  const fn = ctx.db.getObject(refId(ref));
  if (!fn) {
    // A pipeline built from a function the capture did not see keeps no reference to it.
    throw new Error(`the capture does not record which function this pipeline's ${stage} stage was built from`);
  }
  const entryPoint = str(fn.args?.name) || "main0";
  const library = ctx.db.getObject(fn.parentId);
  if (!library || library.type !== "MTLLibrary") throw new Error(`${entryPoint} has no library in the capture`);
  const blobIndex = library.blobs.findIndex((b) => b.name === "Metal Shading Language");
  if (blobIndex < 0) {
    throw new Error(library.blobs.some((b) => b.name === "metallib")
      ? "the library was loaded precompiled, so the capture holds no Metal Shading Language to step through"
      : "the capture holds no source for the library this shader came from");
  }
  const key = `${library.id}:${blobIndex}`;
  const bytes = ctx.db.blobData.get(key) ?? await ctx.fetchBlob?.(library.id, blobIndex) ?? null;
  if (!bytes) throw new Error("the library's source is not in the capture file, and the application is no longer connected");
  const program = MslProgram.of(new TextDecoder().decode(bytes), library.label || "shader.metal");
  const entry = program.entryPoint(entryPoint, QUALIFIER[stage]);
  if (!entry) {
    throw new Error(`the library's source has no ${QUALIFIER[stage]} function ${entryPoint}${program.diagnostics.length ? `; the source did not parse cleanly (line ${program.diagnostics[0].line}: ${program.diagnostics[0].message})` : ""}`);
  }
  const source: StageSource = {
    stage, stageFlag: stage, entryPoint: entry.name, object: library, blobIndex, module: library,
  };
  return { source, program, entry, constants: functionConstants(fn) };
}

/**
 * The `[[function_constant(n)]]` values a tracked MTLFunction was built with, which the capture
 * library recorded by watching the setters of the `MTLFunctionConstantValues` the application
 * filled in (src/metal/src/function_constants.h). A function created without any has none, and the
 * invocation says so.
 */
export function functionConstants(fn: VulkanObject | null): MslFunctionConstants {
  const byIndex = new Map<number, Value>();
  const byName = new Map<string, Value>();
  const list = fn?.args?.constantValues;
  if (!Array.isArray(list)) return { byIndex, byName };
  for (const entry of list) {
    if (!isObject(entry)) continue;
    // Written decoded by the capture library: a number, a boolean, or an array of them.
    const value = entry.value;
    if (value === undefined || value === null) continue;
    const decoded = (Array.isArray(value) ? value.map(scalarOf) : scalarOf(value)) as Value;
    if (entry.index !== undefined) byIndex.set(num(entry.index), decoded);
    const name = str(entry.name);
    if (name) byName.set(name, decoded);
  }
  return { byIndex, byName };
}

function scalarOf(v: unknown): number | boolean {
  return typeof v === "boolean" ? v : Number(v) || 0;
}

// ---------------------------------------------------------------------------------------------
// Bindings

const FILTER = ["nearest", "linear"] as const;
const ADDRESS = ["clamp", "mirrorClamp", "repeat", "mirror", "clamp", "border"] as const;
const BORDERS: number[][] = [[0, 0, 0, 0], [0, 0, 0, 1], [1, 1, 1, 1]];
const COMPARE = ["Never", "Less", "Equal", "LessEqual", "Greater", "NotEqual", "GreaterEqual", "Always"];

/** An MTLSamplerState's descriptor as the interpreter reads it. The enums arrive as their numbers. */
export function metalSampler(object: VulkanObject | null, clamps?: { lodMinClamp?: number; lodMaxClamp?: number }): DebugSampler | null {
  const d = object?.descriptor;
  if (!d) return null;
  const compare = num(d.compareFunction);
  return {
    magFilter: FILTER[num(d.magFilter)] ?? "nearest",
    minFilter: FILTER[num(d.minFilter)] ?? "nearest",
    // MTLSamplerMipFilter: 0 is not mipmapped, 1 nearest, 2 linear.
    mipmapMode: num(d.mipFilter) === 2 ? "linear" : "nearest",
    address: [ADDRESS[num(d.sAddressMode)] ?? "clamp", ADDRESS[num(d.tAddressMode)] ?? "clamp", ADDRESS[num(d.rAddressMode)] ?? "clamp"],
    border: BORDERS[num(d.borderColor)] ?? [0, 0, 0, 0],
    compareOp: compare > 0 && compare < COMPARE.length ? COMPARE[compare] : null,
    minLod: clamps?.lodMinClamp ?? num(d.lodMinClamp),
    maxLod: clamps?.lodMaxClamp ?? (d.lodMaxClamp === undefined ? 1000 : num(d.lodMaxClamp)),
    lodBias: 0,
    unnormalized: d.normalizedCoordinates === false,
  };
}

/** What a Metal draw or dispatch had bound at a stage, as the MSL interpreter reads it. */
export function metalBindings(ctx: DebugContext, state: DrawState, stage: Stage): MslBindings {
  const textures = new Map<number, DebugTexture | null>();
  const scenes = new Map<number, RayScene | null>();
  const tables = new Map<number, RayFunctionTable | null>();
  return {
    buffer: (index) => {
      const bound = state.stageBuffers.get(`${stage}:${index}`);
      if (!bound) return null;
      const captured = ctx.data.buffer(bound.dataId);
      return captured?.data ?? null;
    },
    texture: (index) => {
      if (textures.has(index)) return textures.get(index) ?? null;
      const bound = state.stageTextures.get(`${stage}:${index}`);
      let tex: DebugTexture | null = null;
      if (bound) {
        // The read-back the capture made for this bind, else any contents it holds for the texture.
        const captured = ctx.data.capturedImage(bound.dataId) ?? ctx.data.imageContents(refId(bound.texture) ?? 0);
        tex = captured ? debugTexture(captured) : null;
      }
      textures.set(index, tex);
      return tex;
    },
    sampler: (index) => {
      const bound = state.stageSamplers.get(`${stage}:${index}`);
      if (!bound) return null;
      return metalSampler(ctx.db.getObject(refId(bound.sampler)), bound);
    },
    // Ray queries: the scene and the function table the dispatch bound, for a kernel that
    // traverses (msl/raytracing.ts). Built once each, since a traversal asks for them per ray.
    accelerationStructure: (index) => {
      if (scenes.has(index)) return scenes.get(index) ?? null;
      const bound = state.rayBindings.get(`${stage}:${index}`);
      let built: RayScene | null = null;
      if (bound?.kind === "accelerationStructure") {
        const structureId = refId(bound.object) ?? 0;
        const scene = structureId ? accelerationScene(ctx.data, ctx.db, structureId) : null;
        built = scene ? buildRayScene(scene) : null;
      }
      scenes.set(index, built);
      return built;
    },
    functionTable: (index) => {
      if (tables.has(index)) return tables.get(index) ?? null;
      const bound = state.rayBindings.get(`${stage}:${index}`);
      const table = bound && bound.kind !== "accelerationStructure"
        ? metalFunctionTable(ctx, refId(bound.object) ?? 0) : null;
      tables.set(index, table);
      return table;
    },
    label: (kind, index) => `${kind}(${index})`,
  };
}

/**
 * An intersection function table as the capture recorded it: which function each entry runs, and
 * the buffers the table bound for them.
 *
 * Both are on the table object, and neither needs resolving against anything: an
 * `MTLFunctionHandle` carries its function's *name*, so the capture writes the name
 * (src/metal/src/raytracing.mm, SendTable) and the debugger looks that name up among the shader's
 * own functions. Where DXR would have a 32-byte export identifier to match against the state
 * object's exports, and Vulkan a group handle against the pipeline's, Metal has the name.
 */
function metalFunctionTable(ctx: DebugContext, tableId: number): RayFunctionTable | null {
  const object = tableId ? ctx.db.getObject(tableId) : null;
  const table = isObject(object?.updates?.table) ? object.updates.table : null;
  if (!table) return null;
  const entries: (string | null)[] = [];
  for (const entry of Array.isArray(table.entries) ? table.entries : []) {
    if (!isObject(entry)) continue;
    const at = num(entry.index);
    // An entry the application never set, or one holding Metal's own opaque triangle function,
    // runs nothing the shader declares.
    entries[at] = entry.empty === true || entry.opaque !== undefined ? null : str(entry.function) || null;
  }
  // The contents, from the read-back the capture library made as the capture started
  // (ReadBackTableBuffers in src/metal/src/raytracing.mm). A table's buffers are set once at setup,
  // so no command of the captured frame binds them and nothing else would have read them.
  const buffers = new Map<number, Uint8Array | null>();
  const read = isObject(object?.updates?.tableBuffers) ? object.updates.tableBuffers : null;
  for (const bound of Array.isArray(read?.buffers) ? read.buffers : []) {
    if (!isObject(bound)) continue;
    buffers.set(num(bound.index), ctx.data.buffer(num(bound.capture))?.data ?? null);
  }
  // A capture taken before that read-back existed still names the buffer objects, so any range of
  // one the frame happened to bind elsewhere is better than nothing.
  for (const bound of Array.isArray(table.buffers) ? table.buffers : []) {
    if (!isObject(bound)) continue;
    const at = num(bound.index);
    if (buffers.get(at)) continue;
    const objectId = num(bound.buffer);
    const range = [...ctx.data.buffers.values()].find((b) => b.info.buffer === objectId && b.data);
    buffers.set(at, range?.data ?? null);
  }
  return { entries, buffer: (index) => buffers.get(index) ?? null };
}

// ---------------------------------------------------------------------------------------------
// Inputs

/** The `[[stage_in]]` struct of an entry point, when it has one. */
function stageInType(program: MslProgram, entry: FunctionIr): number | null {
  for (const p of entry.params) {
    const symbol = program.ir.symbols[p.id];
    if (symbol?.binding?.kind === "stage_in") return p.type;
  }
  return null;
}

/**
 * MTLPrimitiveType as the shared rasterizer's topology names it. The capture records it by name
 * ("MTLPrimitiveTypeTriangleStrip"), falling back to its number for a value the layer had no name
 * for; both are read here so a strip is never mistaken for a list.
 */
function topologyOf(cmd: CaptureCommand): string {
  const name = str(cmd.args?.primitiveType);
  if (name) {
    if (name.endsWith("Point")) return "VK_PRIMITIVE_TOPOLOGY_POINT_LIST";
    if (name.endsWith("LineStrip")) return "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP";
    if (name.endsWith("Line")) return "VK_PRIMITIVE_TOPOLOGY_LINE_LIST";
    if (name.endsWith("TriangleStrip")) return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP";
    return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST";
  }
  switch (num(cmd.args?.primitiveType)) {
    case 0: return "VK_PRIMITIVE_TOPOLOGY_POINT_LIST";
    case 1: return "VK_PRIMITIVE_TOPOLOGY_LINE_LIST";
    case 2: return "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP";
    case 4: return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP";
    default: return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST";
  }
}

/**
 * The draw's vertex shader run over its vertices, packed the way a replay's transform feedback
 * would be, so the shared rasterizer can find the triangle covering a pixel.
 *
 * Strips are expanded to lists here rather than in the rasterizer: it walks vertices three at a
 * time, and a Metal draw's topology is an argument of the draw rather than pipeline state.
 */
export async function interpretedMeshOutput(ctx: DebugContext, cmd: CaptureCommand, state: DrawState): Promise<MeshOutput> {
  const { program, entry, constants } = await metalStage(ctx, state, "vertex");
  const input = meshInput(ctx.data, ctx.db, cmd, ctx.inputNames ?? new Map());
  const bindings = metalBindings(ctx, state, "vertex");
  const a = cmd.args ?? {};
  const firstInstance = num(a.baseInstance);
  const topology = topologyOf(cmd);
  const types = program.ir.types;
  const returnType = types.get(entry.returnType);

  // The outputs, from the entry point's return type: one record per vertex, four bytes a scalar.
  const outputs: MeshOutputVariable[] = [];
  let stride = 0;
  const members = returnType?.kind === "struct" ? returnType.members : [];
  const flatten = (name: string, type: number, attributes: Attribute[] | undefined): void => {
    const t = types.get(type);
    const components = t?.kind === "vector" ? t.count : t?.kind === "scalar" ? 1 : 0;
    if (!components) return;
    const base = types.isFloat(type) ? "float" : types.isSigned(type) ? "int" : "uint";
    outputs.push({
      name, offset: stride, components, base,
      ...(attributeNamed(attributes, "position") ? { builtin: "Position" } : {}),
    });
    stride += components * 4;
  };
  if (members.length) for (const m of members) flatten(m.name, m.type, m.attributes);
  else flatten("return", entry.returnType, entry.returnAttributes);

  // Every instance, one after another, the way transform feedback writes them: an instanced draw
  // puts the same triangle in several places, and a pixel is covered by whichever one got there.
  const instances = Math.max(1, num(a.instanceCount) || 1);
  const perInstance = Math.min(input.ids.length, Math.max(3, Math.floor(MAX_INTERPRETED_VERTICES / instances)));
  const instanceCount = Math.min(instances, Math.max(1, Math.floor(MAX_INTERPRETED_VERTICES / Math.max(1, perInstance))));
  const records: number[][] = [];
  const notes = [...input.notes];
  const warnings = new Set<string>();
  for (let instance = 0; instance < instanceCount; instance++) {
    for (let order = 0; order < perInstance; order++) {
      const attributes = new Map<number, number[]>();
      input.attributes.forEach((attr, k) => {
        const values = input.values(order, k, instance);
        if (values) attributes.set(attr.location, values);
      });
      const vertexId = input.ids[order];
      const inputs: MslInputs = {
        builtins: new Map<string, Value>([
          ["vertex_id", vertexId], ["instance_id", firstInstance + instance],
          ["base_vertex", num(a.baseVertex)], ["base_instance", firstInstance],
          ["vertex_amplification_id", 0], ["vertex_amplification_count", 1],
        ]),
        attributes,
        varyings: new Map(),
      };
      const invocation = new MslInvocation(program, { entryPoint: entry.name, stage: "vertex", bindings, inputs, constants });
      invocation.run();
      for (const w of invocation.warnings) warnings.add(w);
      const written = invocation.outputs();
      records.push(outputs.map((o) => {
        const value = members.length
          ? written.find((v) => v.name === o.name)?.value
          : written[0]?.value;
        return scalarsOf(value, o.components);
      }).flat());
    }
  }
  const truncated = perInstance < input.ids.length || instanceCount < instances;
  if (truncated) {
    notes.push(`The draw's vertex shader was run for ${perInstance.toLocaleString()} of its ${input.ids.length.toLocaleString()} vertices in ${instanceCount.toLocaleString()} of its ${instances.toLocaleString()} instances.`);
  }
  for (const w of warnings) notes.push(`Running the vertex shader: ${w}`);
  return packInterpretedMesh(cmd, topology, outputs, stride, records, perInstance, instanceCount, truncated, notes);
}

/**
 * A Metal draw's rasterizer state, with the viewport its render pass implies when the encoder set
 * none: Metal starts a pass with the viewport covering the whole render target, and a shader that
 * never changes it never records one.
 */
export function metalRasterState(ctx: DebugContext, cmd: CaptureCommand, state: DrawState): RasterState {
  const pass = passOfCommand(ctx.data, cmd);
  const target = pass
    ? ctx.data.texturesForPass(cmd.frame, pass.commandBuffer, pass.passIndex).find((t) => t.info.aspect === "color")
    : undefined;
  const whole = target ? { x: 0, y: 0, width: target.info.width, height: target.info.height, minDepth: 0, maxDepth: 1 } : null;
  return rasterStateOf(state, whole);
}

// ---------------------------------------------------------------------------------------------
// Sessions

/** A Metal draw or dispatch prepared for the debugger. */
export async function prepareMetalSession(ctx: DebugContext, target: BasicTarget, state: DrawState, cmd: CaptureCommand): Promise<DebugSession> {
  const stage: Stage = target.stage;
  const { source, program, entry, constants } = await metalStage(ctx, state, stage);
  const bindings = metalBindings(ctx, state, stage);
  const notes: string[] = [];
  if (program.diagnostics.length) {
    const first = program.diagnostics[0];
    notes.push(`The source has ${program.diagnostics.length} thing${program.diagnostics.length === 1 ? "" : "s"} the interpreter could not read, the first on line ${first.line}: ${first.message}`);
  }
  const a = cmd.args ?? {};

  if (stage === "compute") {
    const size = (v: ArgValue | undefined): [number, number, number] => (isObject(v) ? [Math.max(1, num(v.width)), Math.max(1, num(v.height)), Math.max(1, num(v.depth))] : [1, 1, 1]);
    const localSize = size(a.threadsPerThreadgroup);
    const indirect = cmd.method.includes("Indirect");
    // `dispatchThreads:` gives a grid of threads; `dispatchThreadgroups:` a grid of groups.
    const threadsPerGrid = a.threadsPerGrid !== undefined ? size(a.threadsPerGrid) : null;
    const groups: [number, number, number] = threadsPerGrid
      ? (threadsPerGrid.map((n, i) => Math.ceil(n / localSize[i])) as [number, number, number])
      : indirect ? [1, 1, 1] : size(a.threadgroupsPerGrid);
    if (indirect) notes.push("An indirect dispatch's threadgroup counts are in a buffer: they read as (1, 1, 1).");
    const g = target.stage === "compute" ? target.invocation : [0, 0, 0];
    const inputs = computeInputs(g as [number, number, number], localSize, groups, threadsPerGrid);
    return {
      target, program, stage: source, bindings, notes,
      description: `invocation (${g.join(", ")}) of a ${groups.join(" x ")} dispatch with threadgroups of ${localSize.join(" x ")}`,
      limits: { groups, localSize },
      start: () => new MslInvocation(program, { entryPoint: entry.name, stage: "kernel", bindings, inputs, constants }),
    };
  }

  if (stage === "vertex") {
    const input = meshInput(ctx.data, ctx.db, cmd, ctx.inputNames ?? new Map());
    notes.push(...input.notes);
    const order = target.stage === "vertex" ? target.vertex : 0;
    const instance = target.stage === "vertex" ? target.instance : 0;
    if (order < 0 || order >= input.ids.length) throw new Error(`the draw reads ${input.ids.length.toLocaleString()} vertices: there is no vertex ${order}`);
    const attributes = new Map<number, number[]>();
    input.attributes.forEach((attr, k) => {
      const values = input.values(order, k, instance);
      if (values) attributes.set(attr.location, values);
    });
    const firstInstance = num(a.baseInstance);
    const vertexId = input.ids[order];
    const inputs: MslInputs = {
      builtins: new Map<string, Value>([
        ["vertex_id", vertexId], ["instance_id", firstInstance + instance],
        ["base_vertex", num(a.baseVertex)], ["base_instance", firstInstance],
        ["vertex_amplification_id", 0], ["vertex_amplification_count", 1],
      ]),
      attributes,
      varyings: new Map(),
    };
    return {
      target, program, stage: source, bindings, notes,
      description: `vertex ${order} of the draw (vertex_id ${vertexId}), instance ${instance}`,
      limits: { vertices: input.ids.length, instances: Math.max(1, num(a.instanceCount) || 1) },
      start: () => new MslInvocation(program, { entryPoint: entry.name, stage: "vertex", bindings, inputs, constants }),
    };
  }

  // A fragment: its inputs come from running the draw's own vertex shader, since Metal has no replay.
  const mesh = await interpretedMeshOutput(ctx, cmd, state);
  if (mesh.note) notes.push(mesh.note);
  const raster = metalRasterState(ctx, cmd, state);
  const x = target.stage === "fragment" ? target.x : 0;
  const y = target.stage === "fragment" ? target.y : 0;
  const { hit, triangles, reason } = coveringTriangle(raster, mesh, x, y, (o) => (o.builtin === "Position" ? null : o.name));
  if (!hit) throw new Error(reason);
  const { x0, y0, target: lane } = PixelQuad.place(x, y);
  const stageIn = stageInType(program, entry);
  const interpolationOf = interpolationsOf(program, stageIn);
  const targetPixel = passPixel(ctx, cmd, x, y);
  return {
    target, program, stage: source, bindings, notes, targetPixel,
    description: `pixel (${x}, ${y}), from triangle ${hit.primitive.toLocaleString()} of ${triangles.toLocaleString()} (${hit.front ? "front" : "back"} facing), whose vertices the interpreter ran the vertex shader for`,
    limits: { width: raster.viewport ? Math.abs(raster.viewport.width) : undefined, height: raster.viewport ? Math.abs(raster.viewport.height) : undefined },
    start: () => new PixelQuad((dx, dy, derivatives: DerivativeSource) => new MslInvocation(program, {
      entryPoint: entry.name, stage: "fragment", bindings, derivatives, constants,
      inputs: fragmentInputs(hit, x0 + dx, y0 + dy, interpolationOf),
    }), lane),
  };
}

function computeInputs(g: [number, number, number], localSize: [number, number, number], groups: [number, number, number], threadsPerGrid: number[] | null): MslInputs {
  const inGroup = g.map((v, i) => v % localSize[i]);
  const grid = threadsPerGrid ?? groups.map((n, i) => n * localSize[i]);
  return {
    builtins: new Map<string, Value>([
      ["thread_position_in_grid", g],
      ["thread_position_in_threadgroup", inGroup],
      ["threadgroup_position_in_grid", g.map((v, i) => Math.floor(v / localSize[i]))],
      ["thread_index_in_threadgroup", inGroup[2] * localSize[0] * localSize[1] + inGroup[1] * localSize[0] + inGroup[0]],
      ["threads_per_threadgroup", localSize],
      ["threadgroups_per_grid", groups],
      ["threads_per_grid", grid],
      ["threads_per_simdgroup", 32],
      ["thread_index_in_simdgroup", 0],
      ["simdgroup_index_in_threadgroup", 0],
      ["simdgroups_per_threadgroup", 1],
      ["quad_index_in_threadgroup", 0],
      ["quad_index_in_simdgroup", 0],
    ]),
    attributes: new Map(),
    varyings: new Map(),
  };
}

/** How each member of the fragment's `[[stage_in]]` struct is interpolated. */
function interpolationsOf(program: MslProgram, stageIn: number | null): (key: string) => Interpolation {
  const how = new Map<string, Interpolation>();
  const t = stageIn === null ? undefined : program.ir.types.get(stageIn);
  if (t?.kind === "struct") {
    for (const m of t.members) {
      const names = m.attributes.map((x) => x.name);
      how.set(m.name, names.includes("flat") ? "flat"
        : names.some((n) => n.endsWith("no_perspective")) ? "noperspective" : "smooth");
    }
  }
  return (key) => how.get(key) ?? "smooth";
}

function fragmentInputs(hit: Covering, px: number, py: number, interpolationOf: (key: string) => Interpolation): MslInputs {
  const { values, fragCoord } = interpolate(hit, px, py, interpolationOf);
  return {
    builtins: new Map<string, Value>([
      ["position", fragCoord],
      ["front_facing", hit.front],
      ["primitive_id", hit.primitive],
      ["point_coord", [0.5, 0.5]],
      ["sample_id", 0],
      ["sample_mask", 0xffffffff],
      ["barycentric_coord", [1 / 3, 1 / 3, 1 / 3]],
      ["render_target_array_index", 0],
      ["viewport_array_index", 0],
      ["layer", 0],
    ]),
    attributes: new Map(),
    varyings: values,
  };
}


