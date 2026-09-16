// Ray tracing in the Inspect panel and the command details: a ray tracing pipeline's shader groups,
// an acceleration structure with what its last build put in it, and a trace command's shader
// binding table regions.
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { objectLink, type LinkHandler } from "./args_view.js";
import { MeshPreview } from "./mesh_preview.js";
import {
  instancePosition, instanceScene, isIdentity,
  type AccelerationInstance,
} from "./acceleration_structure.js";
import { bindingTableRegions, shaderGroups, stageFromFlag, stageLabel } from "./shader_cache.js";
import { fmt, fmtFlags, formatBytes, isObject, num, refId, str, type ObjectLookup, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { ArgObject } from "../shared/protocol.js";

function row(parent: Widget, label: string, value: string): Div {
  const r = new Div(parent, { class: "draw-state-row" });
  new Span(r, { text: `${label}: `, class: "text-muted" });
  new Span(r, { text: value });
  return r;
}

/** "Closest Hit #2 (main)": a stage of the pipeline by its index in pStages. */
function stageName(pipeline: VulkanObject, index: number | undefined): string {
  if (index === undefined) return "";
  const stages = Array.isArray(pipeline.descriptor?.pStages) ? pipeline.descriptor!.pStages : [];
  const s = stages[index];
  if (!isObject(s)) return `stage #${index}`;
  return `${stageLabel(stageFromFlag(str(s.stage)))} #${index} (${str(s.pName) || "main"})`;
}

/** A ray tracing pipeline's shader groups; nothing for any other pipeline. */
export function renderShaderGroups(parent: Widget, pipeline: VulkanObject): void {
  const groups = shaderGroups(pipeline);
  if (!groups.length) return;
  const grp = new collapsible(parent, { label: `Shader Groups (${groups.length})`, collapsed: false });
  const d = pipeline.descriptor;
  if (d?.maxPipelineRayRecursionDepth !== undefined) row(grp.body, "Max recursion depth", String(num(d.maxPipelineRayRecursionDepth)));
  for (const g of groups) {
    const parts = [
      g.general !== undefined ? stageName(pipeline, g.general) : "",
      g.closestHit !== undefined ? `closest hit ${stageName(pipeline, g.closestHit)}` : "",
      g.anyHit !== undefined ? `any hit ${stageName(pipeline, g.anyHit)}` : "",
      g.intersection !== undefined ? `intersection ${stageName(pipeline, g.intersection)}` : "",
    ].filter(Boolean);
    row(grp.body, `Group ${g.index} (${g.type})`, parts.join(", ") || "no shaders");
  }
}


/** What this needs of a database to resolve an instance's reference: every structure it knows. */
export interface StructureLookup {
  getObjectsOfType(type: string): Map<number, VulkanObject> | null;
}

/**
 * The address the layer recorded on each acceleration structure, so an instance's reference can be
 * turned back into the object it names (src/vulkan/src/hooks.cpp,
 * Hook_vkGetAccelerationStructureDeviceAddressKHR).
 */
export function structureAddresses(db: StructureLookup): Map<string, number> {
  const out = new Map<string, number>();
  for (const o of db.getObjectsOfType("VkAccelerationStructureKHR")?.values() ?? []) {
    const a = o.updates.deviceAddress;
    if (a !== undefined && a !== null) out.set(String(a), o.id);
  }
  return out;
}

/** The instances of a top level's last captured build, and the geometry of the levels they name. */
export interface AccelerationScene {
  instances: AccelerationInstance[];
  /** The triangles a bottom level was built from, or null when that build is not in the capture. */
  meshOf: (blas: number) => Float32Array | null;
}

/** A short description of an instance's placement: where it sits, or that it is not moved. */
function placement(i: AccelerationInstance): string {
  if (isIdentity(i.transform)) return "at the origin, unrotated";
  const [x, y, z] = instancePosition(i);
  return `at ${x.toFixed(3)}, ${y.toFixed(3)}, ${z.toFixed(3)}`;
}

/**
 * The instances a top level was built from, and the scene they make. This is the half of a ray
 * tracing scene that can actually be seen: an acceleration structure is opaque, so what it was
 * built out of is the only view of it there is.
 */
function renderInstances(parent: Widget, s: AccelerationScene, db: ObjectLookup, onLink: LinkHandler): void {
  const instances = s.instances;
  const grp = new collapsible(parent, { label: `Instances (${instances.length})`, collapsed: false });
  const scene = instanceScene(instances, s.meshOf);
  if (scene.mesh.length) {
    const box = new Div(grp.body, { class: "accel-preview" });
    const preview = new MeshPreview(box);
    preview.setMesh({ positions: scene.mesh, kind: scene.kind, clip: false });
    new Div(grp.body, {
      text: scene.kind === "triangles"
        ? `${scene.placed} of ${instances.length} instances drawn with the geometry their bottom level was built from.`
        : "The bottom levels' geometry is not in this capture — a bottom level is usually built once, "
          + "before any capture — so each instance is drawn as a box where its transform puts it.",
      class: "text-muted font-sm",
    });
  }
  for (const i of instances) {
    const blas = i.blas !== undefined ? db.getObject(i.blas) : null;
    const r = row(grp.body, `Instance ${i.index}`, "");
    if (blas) objectLink(r, blas, onLink);
    else new Span(r, { text: `structure at ${i.reference}`, class: "text-muted" });
    new Span(r, { text: `  ${placement(i)}, mask 0x${i.mask.toString(16).toUpperCase()}` });
    if (i.customIndex) new Span(r, { text: `, custom index ${i.customIndex}` });
    if (i.bindingTableOffset) new Span(r, { text: `, hit group +${i.bindingTableOffset}` });
    if (i.flagNames.length) new Span(r, { text: `, ${i.flagNames.join(" | ")}`, class: "text-muted" });
  }
}

/** An acceleration structure: its type, size and storage, and the geometries its last build held. */
export function renderAccelerationStructure(parent: Widget, object: VulkanObject, db: ObjectLookup, onLink: LinkHandler,
                                            scene?: AccelerationScene | null): void {
  const grp = new collapsible(parent, { label: "Acceleration Structure", collapsed: false });
  const d = object.descriptor;
  if (d) {
    row(grp.body, "Type", fmt(d.type));
    row(grp.body, "Size", formatBytes(num(d.size)));
    const buffer = db.getObject(refId(d.buffer));
    if (buffer) {
      const r = row(grp.body, "Buffer", "");
      objectLink(r, buffer, onLink);
      new Span(r, { text: `  at offset ${num(d.offset)}`, class: "text-muted" });
    }
  }
  const build = isObject(object.updates.build) ? object.updates.build as ArgObject : null;
  if (!build) {
    new Div(grp.body, { text: "Not built while the inspector was watching.", class: "text-muted" });
    return;
  }
  row(grp.body, "Last build", `${str(build.method)} (${fmt(build.mode)})`);
  if (str(build.flags)) row(grp.body, "Build flags", fmtFlags(build.flags));
  const geometries = Array.isArray(build.geometries) ? build.geometries.filter(isObject) : [];
  row(grp.body, "Primitives", `${num(build.primitiveCount).toLocaleString()} in ${geometries.length} geometr${geometries.length === 1 ? "y" : "ies"}`);
  geometries.forEach((g, i) => {
    const kind = fmt(g.geometryType);
    const detail = str(g.geometryType).includes("TRIANGLES")
      ? `${num(g.primitiveCount).toLocaleString()} triangles, ${fmt(g.vertexFormat)} vertices (stride ${num(g.vertexStride)}, up to vertex ${num(g.maxVertex)}), ${fmt(g.indexType)} indices`
      : str(g.geometryType).includes("AABBS") ? `${num(g.primitiveCount).toLocaleString()} boxes, stride ${num(g.stride)}`
      : `${num(g.primitiveCount).toLocaleString()} instances${g.arrayOfPointers ? " (array of pointers)" : ""}`;
    row(grp.body, `Geometry ${i} (${kind}${str(g.flags) ? `, ${fmtFlags(g.flags)}` : ""})`, detail);
  });
  // The instances are what a top level actually holds, and the only view of an otherwise opaque
  // object; a bottom level has geometry instead, drawn from the buffers its build read.
  if (scene && scene.instances.length) renderInstances(parent, scene, db, onLink);
}

/** A vkCmdTraceRays* command's shader binding table regions, as records of the table. */
export function renderBindingTable(parent: Widget, args: ArgObject | null): void {
  const regions = bindingTableRegions(args);
  if (!regions.length) return;
  const grp = new collapsible(parent, { label: "Shader Binding Table", collapsed: false });
  for (const r of regions) {
    row(grp.body, r.region, r.size ? `${r.records} record${r.records === 1 ? "" : "s"} (stride ${r.stride}, ${r.size} bytes)` : "none");
  }
  new Div(grp.body, { text: "Which shader group each record holds is in the application's table, as opaque handles the capture does not read.", class: "text-muted font-sm" });
}
