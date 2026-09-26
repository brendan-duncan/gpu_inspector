// The shader debugger's geometry and tessellation stages on a Vulkan capture: where an invocation's
// inputs come from. A vertex shader reads the draw's attributes, which the capture holds; the stages
// after it read what the stage before them wrote, which it does not. So those stages are run here,
// in the same interpreter, for the vertices the debugged invocation needs:
//
//   * Geometry: the input primitive's vertices through the vertex shader (the primitive assembled
//     from the draw's topology, strips and fans in the order the rasterizer takes them).
//   * Tessellation control: the patch's vertices through the vertex shader; the patch's control
//     invocations then run together, sharing their outputs across barrier() (spirv/group.ts).
//   * Tessellation evaluation: which patch and which point of it an invocation was given is the
//     tessellator's choice, which the capture cannot know, so a DS Out record of the replay names
//     both (identity_patch.cpp in the replay writes gl_PrimitiveID and gl_TessCoord beside the
//     outputs). The patch's control shader runs to its end for the evaluation shader's inputs.
//
// The replay's transform feedback captured what the last stage before rasterization wrote, so a
// geometry invocation's emitted vertices, and a tessellation evaluation invocation's outputs when
// no geometry shader follows it, are compared with what the GPU computed.
import { Invocation, type InputVertex, type InvocationInputs, type ShaderBindings } from "../spirv/interpreter.js";
import { InvocationGroup } from "../spirv/group.js";
import { BuiltIn, Decoration, ExecutionModel, type SpirvModule } from "../spirv/module.js";
import type { SpirvProgram } from "../spirv/program.js";
import type { Value } from "../spirv/values.js";
import type { DrawState } from "../draw_state.js";
import { dynamicValue } from "../draw_state.js";
import { meshInput, type MeshInput } from "../mesh_input.js";
import { outputValues, type MeshOutput } from "../mesh_output.js";
import { stateStages, type StageSource } from "../shader_cache.js";
import type { Stepper, VariableView } from "../debug/program.js";
import {
  commandBindings, stageOf, vertexInvocationInputs, type DebugContext, type DebugSession, type DebugTarget, type ReplayedValue,
} from "../shader_debug_setup.js";
import { isObject, num, str } from "./vulkan_object.js";
import type { CaptureCommand } from "../../shared/protocol.js";

type PrimitiveTarget = Extract<DebugTarget, { stage: "geometry" | "tess_control" | "tess_eval" }>;

/** What prepareDebugSession has already made of the stage, for any target. */
export interface StageParts {
  source: StageSource;
  captured: SpirvModule;
  bindings: ShaderBindings;
  notes: string[];
  program: SpirvProgram;
  both: (start: (m: SpirvModule) => Stepper) => Pick<DebugSession, "start" | "original">;
  model: number;
}

/** ExecutionMode numbers read here. */
const enum Mode { Invocations = 0, OutputVertices = 26, OutputPoints = 27, OutputLineStrip = 28 }

function flat(v: Value): number[] {
  if (Array.isArray(v)) return v.flatMap(flat);
  return [typeof v === "number" ? v : typeof v === "bigint" ? Number(v) : v === true ? 1 : 0];
}

/**
 * The draw's input primitives per instance, and the vertices (in draw order) of primitive `p`, as the
 * primitive assembler hands them to the next stage. A string where the topology is not followed.
 */
export function assemble(topology: string, count: number, patchSize: number): { primitives: number; vertices: (p: number) => number[] } | string {
  const t = topology.replace(/^VK_PRIMITIVE_TOPOLOGY_/, "").replace(/ \(dynamic\)$/, "");
  const run = (n: number) => (p: number): number[] => Array.from({ length: n }, (_, k) => p * n + k);
  switch (t) {
    case "POINT_LIST": return { primitives: count, vertices: (p) => [p] };
    case "LINE_LIST": return { primitives: Math.floor(count / 2), vertices: run(2) };
    case "LINE_STRIP": return { primitives: Math.max(0, count - 1), vertices: (p) => [p, p + 1] };
    case "TRIANGLE_LIST": return { primitives: Math.floor(count / 3), vertices: run(3) };
    case "TRIANGLE_STRIP": return { primitives: Math.max(0, count - 2), vertices: (p) => [p, p + 1 + (p % 2), p + 2 - (p % 2)] };
    case "TRIANGLE_FAN": return { primitives: Math.max(0, count - 2), vertices: (p) => [p + 1, p + 2, 0] };
    case "LINE_LIST_WITH_ADJACENCY": return { primitives: Math.floor(count / 4), vertices: run(4) };
    case "LINE_STRIP_WITH_ADJACENCY": return { primitives: Math.max(0, count - 3), vertices: (p) => [p, p + 1, p + 2, p + 3] };
    case "TRIANGLE_LIST_WITH_ADJACENCY": return { primitives: Math.floor(count / 6), vertices: run(6) };
    case "PATCH_LIST": return patchSize > 0 ? { primitives: Math.floor(count / patchSize), vertices: run(patchSize) } : "the patch size is not known";
    default: return `a ${t.toLowerCase().replace(/_/g, " ")} topology is not assembled here`;
  }
}

/** A draw's patch size: the pipeline's tessellation state, or the dynamic state the draw set. */
function patchSizeOf(state: DrawState): number {
  const d = state.pipeline?.descriptor;
  const baked = d && isObject(d.pTessellationState) ? d.pTessellationState.patchControlPoints : undefined;
  return num(dynamicValue(state, "patchControlPoints", baked) ?? 0);
}

type Interface = "all" | "patch" | { vertex: number };

/**
 * A stage's outputs as the next stage's input: built-ins by number and the rest by location. `which`
 * picks a tessellation control shader's per-vertex outputs of one vertex (element `vertex` of every
 * arrayed output), or its patch outputs (the tessellation levels and those decorated Patch).
 */
export function outputsAsInputs(module: SpirvModule, outputs: VariableView[], which: Interface = "all"): InputVertex {
  const builtins = new Map<number, Value>();
  const locations = new Map<number, number[]>();
  const put = (id: number, type: number, value: Value): void => {
    const builtin = module.decoration(id, Decoration.BuiltIn)?.[0];
    if (builtin !== undefined) {
      builtins.set(builtin, value);
      return;
    }
    const t = module.types.get(type);
    const base = module.decoration(id, Decoration.Location)?.[0];
    if (t?.kind === "struct") {
      t.members.forEach((member, i) => {
        const v = Array.isArray(value) ? value[i] : 0;
        const memberBuiltin = module.memberDecoration(type, i, Decoration.BuiltIn)?.[0];
        if (memberBuiltin !== undefined) {
          builtins.set(memberBuiltin, v);
          return;
        }
        const location = module.memberDecoration(type, i, Decoration.Location)?.[0] ?? (base === undefined ? undefined : base + i);
        if (location !== undefined) locations.set(location, flat(v));
        void member;
      });
      return;
    }
    if (base === undefined) return;
    // Arrays and matrices take consecutive locations.
    if ((t?.kind === "array" || t?.kind === "matrix") && Array.isArray(value)) {
      value.forEach((e, i) => locations.set(base + i, flat(e)));
      return;
    }
    locations.set(base, flat(value));
  };
  for (const v of outputs) {
    const builtin = module.decoration(v.id, Decoration.BuiltIn)?.[0];
    const patch = !!module.decoration(v.id, Decoration.Patch) || builtin === BuiltIn.TessLevelOuter || builtin === BuiltIn.TessLevelInner;
    if (which === "all") {
      put(v.id, v.type, v.value);
    } else if (which === "patch") {
      if (patch) put(v.id, v.type, v.value);
    } else if (!patch) {
      const t = module.types.get(v.type);
      if (t?.kind !== "array" || !Array.isArray(v.value)) continue;
      put(v.id, t.element, v.value[which.vertex] ?? 0);
    }
  }
  return { builtins, locations };
}

/** Which invocation made each record of the replay's output past the vertex stage, from the columns identity_patch.cpp adds. */
export interface RecordIdentity {
  instance: number;
  primitive: number;
  invocation: number;
  tessCoord: number[] | null;
}

export function recordIdentities(mesh: MeshOutput): RecordIdentity[] | null {
  const primitive = mesh.outputs.find((o) => o.added && o.builtin === "PrimitiveId");
  if (!primitive || !mesh.data) return null;
  const invocation = mesh.outputs.find((o) => o.added && o.builtin === "InvocationId");
  const coord = mesh.outputs.find((o) => o.added && o.builtin === "TessCoord");
  const out: RecordIdentity[] = [];
  let instance = 0;
  let previous: [number, number] | null = null;
  for (let r = 0; r < mesh.vertices; r++) {
    const p = outputValues(mesh, primitive, r)[0] ?? 0;
    const i = invocation ? outputValues(mesh, invocation, r)[0] ?? 0 : 0;
    // Records come primitive after primitive (a geometry shader's invocations in order), so an earlier
    // primitive than the last one's is the next instance's.
    if (previous && (p < previous[0] || (p === previous[0] && i < previous[1]))) instance++;
    previous = [p, i];
    out.push({ instance, primitive: p, invocation: i, tessCoord: coord ? outputValues(mesh, coord, r) : null });
  }
  return out;
}

/** The geometry invocation a GS Out record came from, from the replay's identity columns. */
export function geometryTargetOfRecord(mesh: MeshOutput, record: number): { primitive: number; instance: number; invocation: number } {
  const ids = recordIdentities(mesh);
  if (!ids) throw new Error("the replay's GS Out does not say which primitive each record came from");
  const id = ids[record];
  if (!id) throw new Error(`the replay's GS Out has ${ids.length.toLocaleString()} records: there is no record ${record}`);
  return { primitive: id.primitive, instance: id.instance, invocation: id.invocation };
}

/** A record's outputs as the debugger compares them: every one but those the replay added. */
function recordValues(mesh: MeshOutput, record: number): ReplayedValue[] {
  return mesh.outputs.filter((o) => !o.added).map((o) => ({ name: o.name, location: o.location, builtin: o.builtin, value: outputValues(mesh, o, record) }));
}

export async function preparePrimitiveSession(ctx: DebugContext, target: PrimitiveTarget, state: DrawState, cmd: CaptureCommand, parts: StageParts): Promise<DebugSession> {
  const { source, bindings, notes, program, both, model, captured } = parts;
  const entryPoint = source.entryPoint;
  const stages = new Set(stateStages(state, ctx.db).map((s) => s.stage));
  const input = meshInput(ctx.data, ctx.db, cmd, ctx.inputNames ?? new Map());
  notes.push(...input.notes);
  const a = cmd.args ?? {};
  const instances = Math.max(1, num(a.instanceCount) || 1);

  // The vertex shader over the draw's vertices, each run once however many primitives share it.
  const vs = stageOf(ctx, state, "vertex");
  const vsBindings = commandBindings(ctx, state, vs.source);
  const ran = new Map<string, InputVertex>();
  const runVertex = (order: number, instance: number): InputVertex => {
    const key = `${order}:${instance}`;
    const hit = ran.get(key);
    if (hit) return hit;
    if (order < 0 || order >= input.ids.length) throw new Error(`the primitive needs vertex ${order}, but the draw reads ${input.ids.length.toLocaleString()}`);
    const inv = new Invocation(vs.module, { entryPoint: vs.source.entryPoint, model: ExecutionModel.Vertex, bindings: vsBindings, inputs: vertexInvocationInputs(input, a, order, instance) });
    inv.run();
    if (inv.status !== "returned") throw new Error(`vertex ${order} of the draw did not finish in the vertex shader: ${inv.error || inv.status}`);
    const vertex = outputsAsInputs(vs.module, inv.outputs());
    ran.set(key, vertex);
    return vertex;
  };
  const shaped = (sizeFrom: string): { primitives: number; vertices: (p: number) => number[] } => {
    const topology = str(dynamicValue(state, "topology", isObject(state.pipeline?.descriptor?.pInputAssemblyState) ? state.pipeline!.descriptor!.pInputAssemblyState.topology : undefined)) || input.topology;
    const shape = assemble(topology, input.ids.length, patchSizeOf(state));
    if (typeof shape === "string") throw new Error(`${sizeFrom}: ${shape}`);
    return shape;
  };
  const replay = async (): Promise<MeshOutput | null> => {
    if (!ctx.meshOutput) return null;
    try {
      const mesh = await ctx.meshOutput(cmd.index);
      return mesh.measured ? mesh : null;
    } catch {
      return null;
    }
  };
  const entry = captured.entryPoint(entryPoint, model);
  const mode = (m: number): number | undefined => entry?.modes.get(m)?.[0];

  if (target.stage === "geometry") {
    if (stages.has("tess_eval")) throw new Error("this geometry shader's inputs are the tessellator's, which is not run here: debug the tessellation evaluation shader, or its DS Out");
    const shape = shaped("the geometry shader's input primitives");
    const invocations = Math.max(1, mode(Mode.Invocations) ?? 1);
    const { primitive, instance, invocation } = target;
    if (primitive >= shape.primitives) throw new Error(`the draw assembles ${shape.primitives.toLocaleString()} primitives: there is no primitive ${primitive}`);
    if (instance >= instances) throw new Error(`the draw has ${instances} instances: there is no instance ${instance}`);
    if (invocation >= invocations) throw new Error(`the geometry shader runs ${invocations} invocations per primitive: there is no invocation ${invocation}`);
    const orders = shape.vertices(primitive);
    const vertices = orders.map((o) => runVertex(o, instance));
    const inputs: InvocationInputs = {
      locations: new Map(), vertices,
      builtins: new Map<number, Value>([[BuiltIn.PrimitiveId, primitive], [BuiltIn.InvocationId, invocation], [BuiltIn.ViewIndex, 0]]),
    };
    // What the GPU emitted for this invocation, from the replay's GS Out.
    let replayedEmitted: DebugSession["replayedEmitted"];
    const mesh = await replay();
    const ids = mesh && mesh.stage === "geometry" ? recordIdentities(mesh) : null;
    if (mesh && ids) {
      const records = ids.map((id, r) => ({ id, r })).filter(({ id }) => id.instance === instance && id.primitive === primitive && id.invocation === invocation);
      const topology = entry?.modes.has(Mode.OutputPoints) ? "POINT_LIST" : entry?.modes.has(Mode.OutputLineStrip) ? "LINE_LIST" : "TRIANGLE_LIST";
      replayedEmitted = { topology, records: records.map(({ r }) => recordValues(mesh, r)) };
    } else {
      notes.push("The replay's GS Out was not available, so what the invocation emits is not compared with the GPU's.");
    }
    return {
      target, program, stage: source, bindings, notes, replayedEmitted,
      description: `primitive ${primitive} of instance ${instance} (the draw's vertices ${orders.join(", ")}), geometry invocation ${invocation}`,
      limits: { primitives: shape.primitives, instances, invocations },
      ...both((m) => new Invocation(m, { entryPoint, model, bindings, inputs })),
    };
  }

  if (target.stage === "tess_control") {
    const shape = shaped("the draw's patches");
    const outputs = Math.max(1, mode(Mode.OutputVertices) ?? 1);
    const { patch, instance, invocation } = target;
    if (patch >= shape.primitives) throw new Error(`the draw has ${shape.primitives.toLocaleString()} patches: there is no patch ${patch}`);
    if (instance >= instances) throw new Error(`the draw has ${instances} instances: there is no instance ${instance}`);
    if (invocation >= outputs) throw new Error(`the tessellation control shader runs ${outputs} invocations per patch: there is no invocation ${invocation}`);
    const orders = shape.vertices(patch);
    const vertices = orders.map((o) => runVertex(o, instance));
    const inputsOf = (i: number): InvocationInputs => ({
      locations: new Map(), vertices,
      builtins: new Map<number, Value>([[BuiltIn.InvocationId, i], [BuiltIn.PrimitiveId, patch], [BuiltIn.PatchVertices, orders.length], [BuiltIn.ViewIndex, 0]]),
    });
    notes.push(`The patch's ${outputs} invocations run together, sharing their outputs: a barrier() runs the others up to it before this one goes on, and they finish when it does.`);
    notes.push("The replay does not capture a tessellation control shader's outputs; the evaluation shader's DS Out is compared with the GPU's.");
    return {
      target, program, stage: source, bindings, notes,
      description: `patch ${patch} of instance ${instance} (the draw's vertices ${orders.join(", ")}), control invocation ${invocation}`,
      limits: { patches: shape.primitives, instances, invocations: outputs },
      ...both((m) => new InvocationGroup(outputs, invocation, (i, group, shared) => new Invocation(m, {
        entryPoint, model, bindings, inputs: inputsOf(i), sharedOutputs: shared?.outputCells, barrier: group,
      }))),
    };
  }

  // Tessellation evaluation: a DS Out record names the patch and the point.
  if (stages.has("geometry")) throw new Error("a geometry shader follows the tessellation evaluation shader, so the replay captured the geometry shader's output, not which point of a patch each invocation was given");
  const mesh = await replay();
  if (!mesh) throw new Error("a tessellation evaluation invocation is found through the replay's DS Out, which was not available");
  const ids = recordIdentities(mesh);
  if (!ids || !ids.length || !ids[0].tessCoord) throw new Error("the replay's DS Out does not say which patch and point each record came from");
  if (target.record >= ids.length) throw new Error(`the replay's DS Out has ${ids.length.toLocaleString()} records: there is no record ${target.record}`);
  const id = ids[target.record];
  const shape = shaped("the draw's patches");
  const orders = shape.vertices(id.primitive);
  const patchVertices = orders.map((o) => runVertex(o, id.instance));
  // The patch's control shader, every invocation to its end: its outputs are this stage's inputs.
  const tcs = stageOf(ctx, state, "tess_control");
  const tcsEntry = tcs.module.entryPoint(tcs.source.entryPoint, ExecutionModel.TessellationControl);
  const outputs = Math.max(1, tcsEntry?.modes.get(Mode.OutputVertices)?.[0] ?? 1);
  const tcsBindings = commandBindings(ctx, state, tcs.source);
  const group = new InvocationGroup(outputs, 0, (i, g, shared) => new Invocation(tcs.module, {
    entryPoint: tcs.source.entryPoint, model: ExecutionModel.TessellationControl, bindings: tcsBindings,
    inputs: {
      locations: new Map(), vertices: patchVertices,
      builtins: new Map<number, Value>([[BuiltIn.InvocationId, i], [BuiltIn.PrimitiveId, id.primitive], [BuiltIn.PatchVertices, orders.length], [BuiltIn.ViewIndex, 0]]),
    },
    sharedOutputs: shared?.outputCells, barrier: g,
  }));
  group.runAll();
  const failed = group.lanes.find((l) => l.status !== "returned");
  if (failed) throw new Error(`the patch's tessellation control shader did not finish: ${failed.error || failed.status}`);
  const controlOutputs = group.lanes[0].outputs();
  const vertices = Array.from({ length: outputs }, (_, i) => outputsAsInputs(tcs.module, controlOutputs, { vertex: i }));
  const patchInputs = outputsAsInputs(tcs.module, controlOutputs, "patch");
  const builtins = new Map<number, Value>([
    ...patchInputs.builtins,
    [BuiltIn.TessCoord, id.tessCoord!], [BuiltIn.PrimitiveId, id.primitive], [BuiltIn.PatchVertices, outputs], [BuiltIn.ViewIndex, 0],
  ]);
  const inputs: InvocationInputs = { locations: patchInputs.locations, vertices, builtins };
  if (mesh.views && mesh.views.length > 1) notes.push("The draw is multiview: this is view 0's record, with gl_ViewIndex 0.");
  const coord = id.tessCoord!.map((c) => +c.toPrecision(6)).join(", ");
  return {
    target, program, stage: source, bindings, notes, replayedOutputs: recordValues(mesh, target.record),
    description: `DS Out record ${target.record}: patch ${id.primitive} of instance ${id.instance} at gl_TessCoord (${coord})`,
    limits: { records: ids.length },
    ...both((m) => new Invocation(m, { entryPoint, model, bindings, inputs })),
  };
}
