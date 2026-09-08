// Shader reflection on demand: fetches an object's SPIR-V payload from the layer (RequestBlob)
// once, parses it, and keeps the result for every later draw that uses the same shader.
import { reflectSpirv, type ShaderReflection, type ShaderStage } from "./vulkan/spirv_reflect.js";
import { isObject, refId, str, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { ObjectDatabase } from "./vulkan/object_database.js";
import type { UiRequest } from "../shared/protocol.js";

/** Where a pipeline stage's code comes from: a blob of the pipeline or of its shader module. */
export interface StageSource {
  stage: ShaderStage;
  stageFlag: string;      // "VK_SHADER_STAGE_VERTEX_BIT"
  entryPoint: string;
  object: VulkanObject;   // pipeline or shader module
  blobIndex: number;
  module: VulkanObject | null;
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
export function pipelineStages(pipeline: VulkanObject, db: ObjectDatabase): StageSource[] {
  const d = pipeline.descriptor;
  if (!d) return [];
  const stages = Array.isArray(d.pStages) ? d.pStages : isObject(d.stage) ? [d.stage] : [];
  const out: StageSource[] = [];
  for (const s of stages) {
    if (!isObject(s)) continue;
    const stageFlag = str(s.stage);
    const stage = stageFromFlag(stageFlag);
    const entryPoint = str(s.pName) || "main";
    const module = db.getObject(refId(s.module));
    let blobIndex = pipeline.blobs.findIndex((b) => b.name === `${stage}:${entryPoint}`);
    if (blobIndex < 0) blobIndex = pipeline.blobs.findIndex((b) => b.name.startsWith(`${stage}:`));
    if (blobIndex >= 0) out.push({ stage, stageFlag, entryPoint, object: pipeline, blobIndex, module });
    else if (module && module.blobs.length) out.push({ stage, stageFlag, entryPoint, object: module, blobIndex: 0, module });
  }
  return out;
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
    const sources = pipelineStages(pipeline, this._db);
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
