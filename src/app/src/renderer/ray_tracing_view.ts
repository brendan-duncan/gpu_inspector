// Ray tracing in the Inspect panel and the command details: a ray tracing pipeline's shader groups,
// an acceleration structure with what its last build put in it, and a trace command's shader
// binding table regions.
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { objectLink, type LinkHandler } from "./args_view.js";
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

/** An acceleration structure: its type, size and storage, and the geometries its last build held. */
export function renderAccelerationStructure(parent: Widget, object: VulkanObject, db: ObjectLookup, onLink: LinkHandler): void {
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
