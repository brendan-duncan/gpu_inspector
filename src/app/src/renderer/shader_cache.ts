// Shader reflection on demand: fetches an object's SPIR-V payload from the layer (RequestBlob)
// once, parses it, and keeps the result for every later draw that uses the same shader. Also
// where each stage of a pipeline keeps its code, and which pipelines a capture's draws used.
import { reflectSpirv, type ShaderReflection, type ShaderResource, type ShaderStage } from "./vulkan/spirv_reflect.js";
import { isObject, refId, str, type ObjectLookup, type VulkanObject } from "./vulkan/vulkan_object.js";
import { isAction, boundPipelineOf } from "./command_sets.js";
import { d3d12Reflection, findD3D12Resource, hasD3D12Reflection, isD3D12Binding } from "./d3d12/reflection.js";
import { isD3D12Type } from "./d3d12/d3d12_object.js";
import type { CaptureData } from "./capture_data.js";
import type { ObjectDatabase } from "./vulkan/object_database.js";
import type { ArgObject, CaptureCommand, CaptureDescriptorBinding, UiRequest } from "../shared/protocol.js";

/** Where a pipeline stage's code comes from: a blob of the pipeline or of its shader module. */
export interface StageSource {
  stage: ShaderStage;
  stageFlag: string;      // "VK_SHADER_STAGE_VERTEX_BIT"
  entryPoint: string;
  object: VulkanObject;   // pipeline or shader module
  blobIndex: number;
  module: VulkanObject | null;
  /** The stage's index in pStages, which a ray tracing pipeline's shader groups refer to. */
  stageIndex?: number;
}

/** A ray tracing pipeline's shader group, with the stages it names as their indices in pStages. */
export interface ShaderGroup {
  index: number;
  type: "general" | "triangles hit" | "procedural hit" | string;
  general?: number;
  closestHit?: number;
  anyHit?: number;
  intersection?: number;
}

/** The shader groups of a ray tracing pipeline (VkRayTracingPipelineCreateInfoKHR::pGroups); empty for any other pipeline. */
export function shaderGroups(pipeline: VulkanObject): ShaderGroup[] {
  const groups = pipeline.descriptor?.pGroups;
  if (!Array.isArray(groups)) return [];
  const UNUSED = 0xffffffff;
  const shader = (v: unknown): number | undefined => (typeof v === "number" && v !== UNUSED ? v : undefined);
  return groups.filter(isObject).map((g, index) => {
    const type = str(g.type).replace("VK_RAY_TRACING_SHADER_GROUP_TYPE_", "").replace("_KHR", "").replace("_GROUP", "").toLowerCase().replace("_", " ");
    return { index, type, general: shader(g.generalShader), closestHit: shader(g.closestHitShader), anyHit: shader(g.anyHitShader), intersection: shader(g.intersectionShader) };
  });
}

/**
 * The shader binding table regions of a vkCmdTraceRays* command, as record counts: which records
 * of the table each kind of shader occupies. The records' group handles are opaque, so which group
 * each record is cannot be read from the capture.
 */
export function bindingTableRegions(args: ArgObject | null | undefined): { region: string; records: number; stride: number; size: number }[] {
  if (!args) return [];
  const out: { region: string; records: number; stride: number; size: number }[] = [];
  for (const [key, region] of [["pRaygenShaderBindingTable", "raygen"], ["pMissShaderBindingTable", "miss"], ["pHitShaderBindingTable", "hit"], ["pCallableShaderBindingTable", "callable"]] as const) {
    const r = args[key];
    if (!isObject(r)) continue;
    const stride = typeof r.stride === "number" ? r.stride : 0;
    const size = typeof r.size === "number" ? r.size : 0;
    out.push({ region, stride, size, records: stride > 0 ? Math.floor(size / stride) : 0 });
  }
  return out;
}

const STAGE_FLAGS: Record<string, ShaderStage> = {
  VK_SHADER_STAGE_VERTEX_BIT: "vertex",
  VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: "tess_control",
  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: "tess_eval",
  VK_SHADER_STAGE_GEOMETRY_BIT: "geometry",
  VK_SHADER_STAGE_FRAGMENT_BIT: "fragment",
  VK_SHADER_STAGE_COMPUTE_BIT: "compute",
  VK_SHADER_STAGE_TASK_BIT_EXT: "task",
  VK_SHADER_STAGE_MESH_BIT_EXT: "mesh",
  VK_SHADER_STAGE_RAYGEN_BIT_KHR: "raygen",
  VK_SHADER_STAGE_ANY_HIT_BIT_KHR: "any_hit",
  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: "closest_hit",
  VK_SHADER_STAGE_MISS_BIT_KHR: "miss",
  VK_SHADER_STAGE_INTERSECTION_BIT_KHR: "intersection",
  VK_SHADER_STAGE_CALLABLE_BIT_KHR: "callable",
};

export function stageFromFlag(flag: string): ShaderStage {
  return STAGE_FLAGS[flag] ?? "unknown";
}

export function stageLabel(stage: ShaderStage): string {
  return stage.split("_").map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(" ");
}

/**
 * The shader stages of a pipeline. The layer attaches each stage's SPIR-V to the pipeline
 * ("vertex:main"); when it could not (module destroyed before the pipeline was created, older
 * captures), the stage's shader module is used instead.
 */
export function pipelineStages(pipeline: VulkanObject, db: ObjectLookup): StageSource[] {
  const d = pipeline.descriptor;
  if (!d) return [];
  const stages = Array.isArray(d.pStages) ? d.pStages : isObject(d.stage) ? [d.stage] : [];
  const out: StageSource[] = [];
  stages.forEach((s, stageIndex) => {
    if (!isObject(s)) return;
    const stageFlag = str(s.stage);
    const stage = stageFromFlag(stageFlag);
    const entryPoint = str(s.pName) || "main";
    const module = db.getObject(refId(s.module));
    // A ray tracing pipeline's payloads carry their index in pStages ("miss:main#1"): it often has
    // several stages of one kind.
    let blobIndex = pipeline.blobs.findIndex((b) => b.name === `${stage}:${entryPoint}#${stageIndex}`);
    if (blobIndex < 0) blobIndex = pipeline.blobs.findIndex((b) => b.name === `${stage}:${entryPoint}`);
    if (blobIndex < 0) blobIndex = pipeline.blobs.findIndex((b) => b.name.startsWith(`${stage}:`));
    if (blobIndex >= 0) out.push({ stage, stageFlag, entryPoint, object: pipeline, blobIndex, module, stageIndex });
    else if (module && module.blobs.length) out.push({ stage, stageFlag, entryPoint, object: module, blobIndex: 0, module, stageIndex });
  });
  // A pipeline linked from graphics pipeline libraries names none of their stages in its own create
  // info: the layer attaches their code to it ("fragment:main"), so those payloads are its stages too.
  // A D3D12 pipeline state names its stages only through its payloads too ("vertex:VSMain"), and
  // the library's ReplaceShader takes the UI's stage name, so that is what its stageFlag is.
  const d3d12 = isD3D12Type(pipeline.type ?? "");
  pipeline.blobs.forEach((b, blobIndex) => {
    const [stage, entry = "main"] = b.name.split(":");
    const entryPoint = entry.replace(/#\d+$/, "");
    const stageFlag = Object.keys(STAGE_FLAGS).find((f) => STAGE_FLAGS[f] === stage);
    if (!stageFlag || out.some((s) => s.stage === stage)) return;
    out.push({ stage: stage as ShaderStage, stageFlag: d3d12 ? stage : stageFlag, entryPoint, object: pipeline, blobIndex, module: null });
  });
  return out;
}

/**
 * The reflected resource a bound descriptor feeds. Vulkan and Metal declare resources by set and
 * binding, which the snapshot's set and binding index name directly; a D3D12 snapshot's binding
 * is a root parameter's range, keyed by register and space, and `element` says which descriptor
 * of the range (d3d12/reflection.ts).
 */
export function findBoundResource(reflection: ShaderReflection | null | undefined, set: number, binding: CaptureDescriptorBinding, element = 0): ShaderResource | null {
  if (!reflection) return null;
  if (isD3D12Binding(binding)) return findD3D12Resource(reflection, binding, element);
  return reflection.findResource(set, binding.binding);
}

/**
 * What a draw or dispatch runs: a pipeline, or the shader objects bound in its place with
 * vkCmdBindShadersEXT (VK_EXT_shader_object). Reports that group work by pipeline id group shader
 * objects by a program key instead: the pipeline's id, or a negative key standing for one set of
 * shader objects, the same for every draw of the capture that binds that set.
 */
export interface ShaderProgram {
  key: number;
  pipeline: VulkanObject | null;
  shaders: VulkanObject[];
  name: string;
}

const programKeys = new WeakMap<CaptureData, { byIds: Map<string, number>; ids: Map<number, number[]> }>();

/** The program key of a set of shader objects of a capture. */
export function shaderProgramKey(data: CaptureData, shaderIds: number[]): number {
  let keys = programKeys.get(data);
  if (!keys) programKeys.set(data, keys = { byIds: new Map(), ids: new Map() });
  const ids = [...new Set(shaderIds)].sort((x, y) => x - y);
  const text = ids.join(",");
  let key = keys.byIds.get(text);
  if (key === undefined) {
    key = -(keys.byIds.size + 1);
    keys.byIds.set(text, key);
    keys.ids.set(key, ids);
  }
  return key;
}

/** The program a key stands for; null when the key is unknown or its objects are not in the database. */
export function shaderProgram(data: CaptureData, db: ObjectLookup, key: number): ShaderProgram | null {
  if (key > 0) {
    const pipeline = db.getObject(key);
    return pipeline ? { key, pipeline, shaders: [], name: pipeline.name } : null;
  }
  const shaders = (programKeys.get(data)?.ids.get(key) ?? []).map((id) => db.getObject(id)).filter((o): o is VulkanObject => !!o);
  if (!shaders.length) return null;
  return { key, pipeline: null, shaders, name: shaders.map((s) => s.name).join(" + ") };
}

/** The stages of a program: its pipeline's, or one per shader object. */
export function programStages(program: { pipeline: VulkanObject | null; shaders: VulkanObject[] }, db: ObjectLookup): StageSource[] {
  if (program.pipeline) return pipelineStages(program.pipeline, db);
  return program.shaders.flatMap((s) => pipelineStages(s, db));
}

/**
 * Follows the pipelines and shader objects bound on each command stream and bind point, so a
 * forward walk over a capture can ask which program a draw or dispatch runs.
 */
export class ProgramTracker {
  private _data: CaptureData;
  private _bound = new Map<string, number>();                       // "stream:bindPoint" -> program key
  private _shaders = new Map<string, Map<string, number | null>>();  // "stream:bindPoint" -> stage -> shader id

  constructor(data: CaptureData) {
    this._data = data;
  }

  /** Follows a binding command; true when it bound a pipeline or shader objects. */
  note(c: CaptureCommand): boolean {
    const sets = this._data.sets;
    const a = c.args;
    if (!a) return false;
    const stream = `${c.object?.__id ?? 0}:${c.secondary ?? 0}`;
    if (sets.BIND_PIPELINE.has(c.method)) {
      const id = refId(boundPipelineOf(a));
      const at = `${stream}:${sets.pipelineBindPointOf(c.method, a)}`;
      this._shaders.delete(at);
      if (id !== null) this._bound.set(at, id);
      else this._bound.delete(at);
      return true;
    }
    if (c.method !== "vkCmdBindShadersEXT") return false;
    const stages = Array.isArray(a.pStages) ? a.pStages : [];
    const shaders = Array.isArray(a.pShaders) ? a.pShaders : [];
    stages.forEach((flag, k) => {
      const stage = str(flag);
      const at = `${stream}:${stage.includes("COMPUTE") ? "VK_PIPELINE_BIND_POINT_COMPUTE" : "VK_PIPELINE_BIND_POINT_GRAPHICS"}`;
      let bound = this._shaders.get(at);
      if (!bound) this._shaders.set(at, bound = new Map());
      bound.set(stage, refId(shaders[k]));
      const ids = [...bound.values()].filter((id): id is number => id !== null);
      if (ids.length) this._bound.set(at, shaderProgramKey(this._data, ids));
      else this._bound.delete(at);
    });
    return true;
  }

  /** The program key of a draw or dispatch; undefined when nothing is bound. */
  at(c: CaptureCommand): number | undefined {
    return this._bound.get(`${c.object?.__id ?? 0}:${c.secondary ?? 0}:${this._data.sets.bindPointOf(c.method)}`);
  }
}

/** Uses per program (see ShaderProgram): what is bound on the stream and bind point of each draw or dispatch. */
export function pipelineUses(data: CaptureData): Map<number, number> {
  const sets = data.sets;
  const tracker = new ProgramTracker(data);
  const uses = new Map<number, number>();
  for (const c of data.commands) {
    if (!c || sets.SUBMIT.has(c.method)) continue;
    if (tracker.note(c) || !isAction(sets, c.method)) continue;
    const key = tracker.at(c);
    if (key !== undefined) uses.set(key, (uses.get(key) ?? 0) + 1);
  }
  return uses;
}

/** The stages a draw state runs: its pipeline's, or its shader objects'. */
export function stateStages(state: { pipeline: VulkanObject | null; shaders: VulkanObject[] }, db: ObjectLookup): StageSource[] {
  return programStages(state, db);
}

export class ShaderReflectionCache {
  private _db: ObjectDatabase;
  private _send: (msg: UiRequest) => Promise<boolean>;
  private _done = new Map<string, ShaderReflection | null>();
  private _pending = new Map<string, ((r: ShaderReflection | null) => void)[]>();

  constructor(db: ObjectDatabase, send: (msg: UiRequest) => Promise<boolean>) {
    this._db = db;
    this._send = send;
    db.onObjectBlob.addListener((id, index, data) => this._blob(id, index, data));
    db.onReset.addListener(() => {
      this._done.clear();
      for (const waiters of this._pending.values()) for (const w of waiters) w(null);
      this._pending.clear();
    });
  }

  /** Reflection of one SPIR-V payload of an object; null when it cannot be obtained. */
  get(object: VulkanObject, blobIndex: number): Promise<ShaderReflection | null> {
    const key = `${object.id}:${blobIndex}`;
    const done = this._done.get(key);
    if (done !== undefined) return Promise.resolve(done);
    if (blobIndex < 0 || blobIndex >= object.blobs.length) return Promise.resolve(null);
    // A D3D12 pipeline's reflection came with the object (the library reflects DXBC/DXIL in the
    // process): read it off the descriptor for the payload's stage, no fetch needed.
    if (hasD3D12Reflection(object)) {
      const stage = object.blobs[blobIndex].name.split(":")[0] as ShaderStage;
      const reflection = d3d12Reflection(object, stage);
      this._done.set(key, reflection);
      return Promise.resolve(reflection);
    }
    return new Promise((resolve) => {
      const waiters = this._pending.get(key);
      if (waiters) {
        waiters.push(resolve);
        return;
      }
      this._pending.set(key, [resolve]);
      void this._send({ action: "RequestBlob", id: object.id, index: blobIndex }).then((ok) => {
        if (!ok) this._settle(key, null);
      });
    });
  }

  /** Reflection of every stage of a pipeline, in stage order (null entries for failures). */
  async stages(pipeline: VulkanObject): Promise<{ source: StageSource; reflection: ShaderReflection | null }[]> {
    return this._reflect(pipelineStages(pipeline, this._db));
  }

  /** Reflection of every stage a draw state runs, from its pipeline or its shader objects. */
  async stagesOf(state: { pipeline: VulkanObject | null; shaders: VulkanObject[] }): Promise<{ source: StageSource; reflection: ShaderReflection | null }[]> {
    return this._reflect(stateStages(state, this._db));
  }

  private async _reflect(sources: StageSource[]): Promise<{ source: StageSource; reflection: ShaderReflection | null }[]> {
    const reflections = await Promise.all(sources.map((s) => this.get(s.object, s.blobIndex)));
    return sources.map((source, i) => ({ source, reflection: reflections[i] }));
  }

  private _blob(id: number, index: number, data: Uint8Array | null): void {
    const key = `${id}:${index}`;
    if (!this._pending.has(key)) return;
    let reflection: ShaderReflection | null = null;
    if (data) {
      try {
        reflection = reflectSpirv(data);
      } catch (e) {
        console.warn("SPIR-V reflection failed", e);
      }
    }
    this._settle(key, reflection);
  }

  private _settle(key: string, reflection: ShaderReflection | null): void {
    this._done.set(key, reflection);
    const waiters = this._pending.get(key) ?? [];
    this._pending.delete(key);
    for (const w of waiters) w(reflection);
  }
}
