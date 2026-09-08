// Renders SPIR-V reflection (renderer/vulkan/spirv_reflect.ts) as the nested list WebGPU
// Inspector's shader info uses: entry points with their interface, resources by set and
// binding with struct members, push constants. Shared by the capture view (per draw) and the
// Inspect tab (per shader module / pipeline payload).
import { Widget } from "./widget/widget.js";
import { typeName, type ShaderReflection, type ShaderResource, type StructType } from "./vulkan/spirv_reflect.js";
import { stageLabel } from "./shader_cache.js";

export function kindLabel(kind: ShaderResource["kind"]): string {
  switch (kind) {
    case "uniform": return "UNIFORM";
    case "storage": return "STORAGE";
    case "pushConstant": return "PUSH CONSTANT";
    case "sampledImage": return "texture";
    case "combinedImageSampler": return "combined image sampler";
    case "storageImage": return "storage image";
    case "sampler": return "sampler";
    case "uniformTexelBuffer": return "uniform texel buffer";
    case "storageTexelBuffer": return "storage texel buffer";
    case "inputAttachment": return "input attachment";
    case "accelerationStructure": return "acceleration structure";
    default: return "resource";
  }
}

/** Members of a struct with their offsets and sizes (nested one level). */
export function renderTypeMembers(ul: Widget, type: StructType): void {
  const l = new Widget("ul", ul, { class: "shader-type-members" });
  for (const m of type.members) {
    new Widget("li", l, { text: `${m.name}: ${typeName(m.type)}  offset ${m.offset}  size ${m.type.kind === "opaque" ? "?" : m.type.size || "<runtime>"}` });
  }
}

export interface ReflectionViewOptions {
  /** Show only this entry point (a pipeline stage); every entry point of the module otherwise. */
  entryPoint?: string;
  /** Name the stage on each entry point (modules with several). */
  showStage?: boolean;
}

/** Appends the reflection as a list to `container`. */
export function renderReflection(container: Widget, reflection: ShaderReflection, options: ReflectionViewOptions = {}): void {
  const ul = new Widget("ul", container);
  if (reflection.version) new Widget("li", ul, { text: `SPIR-V ${reflection.version}` });
  const entries = options.entryPoint
    ? [reflection.entryPoint(options.entryPoint)].filter((e): e is NonNullable<typeof e> => !!e)
    : reflection.entryPoints;
  for (const entry of entries) {
    const stage = options.showStage || entries.length > 1 ? `${stageLabel(entry.stage)} entry: ` : "Entry: ";
    new Widget("li", ul, { text: `${stage}${entry.name}${entry.workgroupSize ? `  workgroup ${entry.workgroupSize.join("x")}` : ""}` });
    if (entry.inputs.length) {
      new Widget("li", ul, { text: `Inputs: ${entry.inputs.length}` });
      const l2 = new Widget("ul", ul);
      for (const v of entry.inputs) new Widget("li", l2, { text: `location ${v.location}: ${v.name || "(unnamed)"}  ${v.typeName}` });
    }
    if (entry.outputs.length) {
      new Widget("li", ul, { text: `Outputs: ${entry.outputs.length}` });
      const l2 = new Widget("ul", ul);
      for (const v of entry.outputs) new Widget("li", l2, { text: `location ${v.location}: ${v.name || "(unnamed)"}  ${v.typeName}` });
    }
  }
  if (reflection.resources.length) {
    new Widget("li", ul, { text: `Resources: ${reflection.resources.length}` });
    const l2 = new Widget("ul", ul);
    const sorted = [...reflection.resources].sort((a, b) => a.set - b.set || a.binding - b.binding);
    for (const r of sorted) {
      const access = r.kind === "storage" || r.kind === "storageImage" || r.kind === "storageTexelBuffer"
        ? (r.readOnly ? " read-only" : r.writeOnly ? " write-only" : " read-write") : "";
      new Widget("li", l2, { text: `set ${r.set} binding ${r.binding}: ${kindLabel(r.kind)}${access}  ${r.name}${r.count !== 1 ? `[${r.count || ""}]` : ""}: ${r.typeName}` });
      if (r.type.kind === "struct") renderTypeMembers(l2, r.type);
    }
  }
  if (reflection.pushConstants.length) {
    new Widget("li", ul, { text: "Push constants:" });
    const l2 = new Widget("ul", ul);
    for (const r of reflection.pushConstants) {
      new Widget("li", l2, { text: `${r.name}: ${r.typeName}${r.type.kind === "struct" || r.type.kind === "array" ? `  ${r.type.size} bytes` : ""}` });
      if (r.type.kind === "struct") renderTypeMembers(l2, r.type);
    }
  }
  if (!entries.length && !reflection.resources.length && !reflection.pushConstants.length) {
    new Widget("li", ul, { text: "No entry points or resources found.", class: "text-muted" });
  }
}
