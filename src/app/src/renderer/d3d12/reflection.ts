// D3D12 shader reflection, as the capture library attaches it to a pipeline state's descriptor
// (src/d3d12/src/shader_reflect.h): per stage, every constant buffer with its members, every bound
// resource with its register and space, the stage's inputs and outputs and a compute shader's
// thread group size, the layouts already in spirv_reflect.ts's ReflType shape. This reads it
// into the same ShaderReflection the Vulkan side builds from SPIR-V, so a draw's constant buffers
// render as typed blocks, the Reflection section works on a pipeline object and the flame graph
// weighs its stages, none of them knowing which API produced the pipeline.
//
// The JSON, one object per stage under `reflection[stage]`:
//   { "entryPoint"?, "target": "vs_6_0", "inputs": [{name, semantic, index, type}], "outputs": [...],
//     "threadGroupSize"?: [x, y, z],
//     "resources": [{ "kind": "cbuffer" | "srv" | "uav" | "sampler", "name", "register", "space", "count",
//                     "dimension": "buffer" | "structured" | "byteaddress" | "texture2d" | ... | "accelerationStructure",
//                     "stride"?, "returnType"?, "type"?: ReflType }] }
//
// A snapshot's bindings are keyed the same way (register and space, src/d3d12/README.md "Bound
// buffers and textures"), which is what findD3D12Resource matches them by.
import { ShaderReflection, typeName, type EntryPoint, type ReflType, type ResourceKind, type ShaderResource, type ShaderStage, type ShaderVariable } from "../vulkan/spirv_reflect.js";
import { isObject, num, str } from "../vulkan/vulkan_object.js";
import type { ArgObject, ArgValue, CaptureDescriptorBinding } from "../../shared/protocol.js";
import type { VulkanObject } from "../vulkan/vulkan_object.js";

const STAGES: ShaderStage[] = ["vertex", "tess_control", "tess_eval", "geometry", "fragment", "compute", "task", "mesh"];

/** Whether the object is a pipeline state the D3D12 library described, with reflection attached. */
export function hasD3D12Reflection(pipeline: VulkanObject | null | undefined): boolean {
  return !!pipeline && pipeline.type === "ID3D12PipelineState" && isObject(pipeline.descriptor?.reflection);
}

function asType(v: ArgValue | undefined): ReflType | null {
  if (!isObject(v)) return null;
  const kind = str(v.kind);
  if (!["scalar", "vector", "matrix", "array", "struct", "opaque", "format"].includes(kind)) return null;
  return v as unknown as ReflType;
}

/** "texture2d" -> "Texture2D", "texturecubearray" -> "TextureCubeArray", "buffer" -> "Buffer". */
function hlslDimension(dimension: string): string {
  const d = dimension.toLowerCase();
  const m = /^texture(1d|2d|3d|cube)(ms)?(array)?$/.exec(d);
  if (m) return `Texture${m[1].toUpperCase().replace("CUBE", "Cube")}${m[2] ? "MS" : ""}${m[3] ? "Array" : ""}`;
  if (d === "structured") return "StructuredBuffer";
  if (d === "byteaddress") return "ByteAddressBuffer";
  if (d === "buffer") return "Buffer";
  if (d === "accelerationstructure") return "RaytracingAccelerationStructure";
  if (d === "feedbacktexture2d") return "FeedbackTexture2D";
  return dimension;
}

/** The ShaderResource kind of a D3D12 resource, from its binding class and dimension. */
function kindOf(entry: ArgObject): ResourceKind {
  const kind = str(entry.kind).toLowerCase();
  const dimension = str(entry.dimension).toLowerCase();
  switch (kind) {
    case "cbuffer": return "uniform";
    case "sampler": return "sampler";
    case "srv":
      if (dimension === "structured" || dimension === "byteaddress") return "storage";
      if (dimension === "buffer") return "uniformTexelBuffer";
      if (dimension === "accelerationstructure") return "accelerationStructure";
      return "sampledImage";
    case "uav":
      if (dimension === "structured" || dimension === "byteaddress" || dimension === "") return "storage";
      if (dimension === "buffer") return "storageTexelBuffer";
      return "storageImage";
    default:
      return "unknown";
  }
}

function resourceOf(entry: ArgObject): ShaderResource | null {
  const kind = kindOf(entry);
  if (kind === "unknown") return null;
  const dimension = str(entry.dimension);
  const returnType = str(entry.returnType);
  const declared = asType(entry.type);
  let type: ReflType;
  let name: string;
  if (declared && (kind === "uniform" || kind === "storage")) {
    type = declared;
    name = declared.kind === "struct" ? declared.name || typeName(declared) : typeName(declared);
    if (kind === "storage" && dimension.toLowerCase() === "structured") {
      name = `${str(entry.kind).toLowerCase() === "uav" ? "RW" : ""}StructuredBuffer<${name}>`;
    }
  } else if (kind === "sampler") {
    type = { kind: "opaque", name: "SamplerState" };
    name = "SamplerState";
  } else if (kind === "storage") {
    // A byte address buffer: bytes, read as 32-bit words.
    type = { kind: "opaque", name: `${str(entry.kind).toLowerCase() === "uav" ? "RW" : ""}${hlslDimension(dimension || "byteaddress")}` };
    name = type.name;
  } else {
    const prefix = str(entry.kind).toLowerCase() === "uav" ? "RW" : "";
    const base = `${prefix}${hlslDimension(dimension || "texture2d")}`;
    type = { kind: "opaque", name: returnType ? `${base}<${returnType}>` : base };
    name = type.name;
  }
  const isUav = str(entry.kind).toLowerCase() === "uav";
  return {
    kind,
    set: num(entry.space),
    binding: num(entry.register),
    name: str(entry.name),
    typeName: name,
    type,
    count: entry.count === undefined ? 1 : num(entry.count),
    readOnly: !isUav && (kind === "storage" || kind === "uniformTexelBuffer"),
    writeOnly: false,
  };
}

function variables(list: ArgValue | undefined): ShaderVariable[] {
  if (!Array.isArray(list)) return [];
  const out: ShaderVariable[] = [];
  list.forEach((v, location) => {
    if (!isObject(v)) return;
    const semantic = str(v.semantic);
    const index = v.index === undefined ? "" : String(num(v.index));
    // Named by semantic ("TEXCOORD0"), which is how the input layout names the same attribute.
    const name = semantic ? `${semantic}${/\d$/.test(semantic) ? "" : index}` : str(v.name);
    const type = asType(v.type) ?? { kind: "opaque", name: str(v.type) || "float4" };
    out.push({ location, name, typeName: typeName(type), type });
  });
  return out;
}

/** The stage's reflection object out of the pipeline's descriptor, or null. */
function stageJson(pipeline: VulkanObject | null | undefined, stage: ShaderStage): ArgObject | null {
  const refl = pipeline?.descriptor?.reflection;
  if (!isObject(refl)) return null;
  const s = refl[stage];
  return isObject(s) ? s : null;
}

/** The reflection of one stage of a D3D12 pipeline as a ShaderReflection; null without one. */
export function d3d12Reflection(pipeline: VulkanObject | null | undefined, stage: ShaderStage): ShaderReflection | null {
  const s = stageJson(pipeline, stage);
  if (!s) return null;
  const r = new ShaderReflection();
  const target = str(s.target);
  r.version = target.includes("_6_") || target.includes("_6") ? "dxil" : target ? "dxbc" : "";
  const groups = Array.isArray(s.threadGroupSize) ? s.threadGroupSize.map(num) : null;
  const entry: EntryPoint = {
    name: str(s.entryPoint) || "main",
    stage,
    inputs: variables(s.inputs),
    outputs: variables(s.outputs),
    workgroupSize: groups && groups.length === 3 ? [groups[0], groups[1], groups[2]] : null,
  };
  r.entryPoints.push(entry);
  const resources = Array.isArray(s.resources) ? s.resources.filter(isObject) : [];
  r.resources = resources.map(resourceOf).filter((x): x is ShaderResource => !!x)
    .sort((a, b) => a.set - b.set || a.binding - b.binding);
  return r;
}

/** Every stage of a D3D12 pipeline with reflection, in stage order. */
export function d3d12StageReflections(pipeline: VulkanObject | null | undefined): Map<ShaderStage, ShaderReflection> {
  const out = new Map<ShaderStage, ShaderReflection>();
  if (!hasD3D12Reflection(pipeline)) return out;
  const refl = pipeline!.descriptor!.reflection as ArgObject;
  const known = STAGES.filter((s) => isObject(refl[s]));
  const others = Object.keys(refl).filter((k) => !STAGES.includes(k as ShaderStage) && isObject(refl[k])) as ShaderStage[];
  for (const stage of [...known, ...others]) {
    const r = d3d12Reflection(pipeline, stage);
    if (r) out.set(stage, r);
  }
  return out;
}

/** Whether a binding is a D3D12 one: a table range or a root view, keyed by register and space. */
export function isD3D12Binding(binding: CaptureDescriptorBinding): boolean {
  return binding.register !== undefined || binding.type.startsWith("D3D12_");
}

/** The kinds of reflected resource a range or root parameter type can hold. */
function kindsOfBindingType(type: string): ResourceKind[] {
  if (type.endsWith("_CBV")) return ["uniform"];
  if (type.endsWith("_SRV")) return ["sampledImage", "uniformTexelBuffer", "storage", "accelerationStructure"];
  if (type.endsWith("_UAV")) return ["storage", "storageImage", "storageTexelBuffer"];
  if (type.endsWith("_SAMPLER")) return ["sampler"];
  return [];
}

/**
 * The reflected resource a descriptor of a D3D12 binding feeds: the one declared in the
 * binding's register space at a register range covering `register + element`, of a kind the
 * range type can hold (an SRV range never feeds a UAV declaration, even at the same register).
 */
export function findD3D12Resource(reflection: ShaderReflection, binding: CaptureDescriptorBinding, element = 0): ShaderResource | null {
  const kinds = kindsOfBindingType(binding.type);
  const register = num(binding.register) + element;
  const space = num(binding.space);
  let best: ShaderResource | null = null;
  for (const r of reflection.resources) {
    if (r.set !== space || !kinds.includes(r.kind)) continue;
    // An SRV range holds only read-only storage declarations, a UAV range only writable ones.
    if (r.kind === "storage" && binding.type.endsWith("_SRV") !== r.readOnly) continue;
    const count = Math.max(r.count, 1);
    // A runtime-sized array (count 0) covers every register from its base.
    if (register < r.binding || (r.count !== 0 && register >= r.binding + count)) continue;
    if (!best || r.binding > best.binding) best = r;
  }
  return best;
}
