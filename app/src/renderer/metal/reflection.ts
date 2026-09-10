// Metal pipeline reflection, as the capture library attaches it to a pipeline's descriptor
// (metal/src/reflection.mm): per stage, the buffers, textures and samplers by index, with each
// buffer's layout already in spirv_reflect.ts's ReflType shape. This reads it into the same
// ShaderResource / ShaderReflection objects the Vulkan side builds from SPIR-V, so the buffer
// views, the Format editor and the Reflection section work on a Metal draw without knowing
// which API produced the pipeline.
import { ShaderReflection, typeName, type ReflType, type ShaderResource, type ShaderStage } from "../vulkan/spirv_reflect.js";
import { isObject, num, str } from "../vulkan/vulkan_object.js";
import type { ArgObject, ArgValue } from "../../shared/protocol.js";
import type { VulkanObject } from "../vulkan/vulkan_object.js";

/** A stage's resources by index. */
export interface MetalStageReflection {
  stage: string;
  buffers: Map<number, ShaderResource>;
  textures: Map<number, ShaderResource>;
  samplers: Map<number, ShaderResource>;
}

const STAGE_NAMES: Record<string, ShaderStage> = {
  vertex: "vertex", fragment: "fragment", compute: "compute", object: "task", mesh: "mesh", tile: "fragment",
};

/** Whether the object is a pipeline the Metal library described, with reflection attached. */
export function hasMetalReflection(pipeline: VulkanObject | null | undefined): boolean {
  return isObject(pipeline?.descriptor?.reflection);
}

function asType(v: ArgValue | undefined): ReflType | null {
  // The library writes the ReflType shape verbatim; the kind is the only thing worth checking.
  if (!isObject(v)) return null;
  const kind = str(v.kind);
  if (!["scalar", "vector", "matrix", "array", "struct", "opaque"].includes(kind)) return null;
  return v as unknown as ReflType;
}

function resource(entry: ArgObject, kind: ShaderResource["kind"], typeOverride?: ReflType): ShaderResource | null {
  const type = typeOverride ?? asType(entry.type);
  if (!type) return null;
  const access = str(entry.access);
  return {
    kind,
    set: 0,
    binding: num(entry.index),
    name: str(entry.name),
    typeName: type.kind === "struct" ? type.name : typeName(type),
    type,
    count: 1,
    readOnly: access === "readOnly",
    writeOnly: access === "writeOnly",
  };
}

/** The stages of a pipeline, from its descriptor's `reflection`. Empty for a pipeline without one. */
export function metalStages(pipeline: VulkanObject | null | undefined): MetalStageReflection[] {
  const refl = pipeline?.descriptor?.reflection;
  if (!isObject(refl)) return [];
  const stages: MetalStageReflection[] = [];
  for (const [stage, value] of Object.entries(refl)) {
    if (!isObject(value)) continue;
    const s: MetalStageReflection = { stage, buffers: new Map(), textures: new Map(), samplers: new Map() };
    for (const b of Array.isArray(value.buffers) ? value.buffers : []) {
      if (!isObject(b)) continue;
      // A buffer the shader only reads is a uniform block to the UI; anything writable is storage.
      const r = resource(b, str(b.access) === "readOnly" ? "uniform" : "storage");
      if (r) s.buffers.set(r.binding, r);
    }
    for (const t of Array.isArray(value.textures) ? value.textures : []) {
      if (!isObject(t)) continue;
      const name = `${str(t.textureType).replace(/^MTLTextureType/, "texture")}<${str(t.dataType) || "float"}>`;
      const r = resource(t, str(t.access) === "readOnly" ? "sampledImage" : "storageImage", { kind: "opaque", name });
      if (r) s.textures.set(r.binding, r);
    }
    for (const sa of Array.isArray(value.samplers) ? value.samplers : []) {
      if (!isObject(sa)) continue;
      const r = resource(sa, "sampler", { kind: "opaque", name: "sampler" });
      if (r) s.samplers.set(r.binding, r);
    }
    stages.push(s);
  }
  return stages;
}

/** The buffer a stage reads at an index, or null when the pipeline's reflection has none there. */
export function metalBufferResource(pipeline: VulkanObject | null | undefined, stage: string, index: number): ShaderResource | null {
  for (const s of metalStages(pipeline)) {
    if (s.stage === stage) return s.buffers.get(index) ?? null;
  }
  return null;
}

/**
 * The reflection as the Reflection section renders it: one entry point per stage, resources
 * with the stage's buffers, textures and samplers, "set" standing in for the stage. Binding
 * indices repeat across stages, so each stage is its own ShaderReflection.
 */
export function metalReflection(stage: MetalStageReflection): ShaderReflection {
  const r = new ShaderReflection();
  r.entryPoints.push({ name: stage.stage, stage: STAGE_NAMES[stage.stage] ?? "unknown", inputs: [], outputs: [], workgroupSize: null });
  const all = [...stage.buffers.values(), ...stage.textures.values(), ...stage.samplers.values()];
  r.resources = all.sort((a, b) => a.binding - b.binding);
  return r;
}
