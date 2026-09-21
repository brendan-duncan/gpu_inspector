// Ray tracing in the Inspect panel and the command details: a pipeline's or state object's shader
// groups, an acceleration structure with what its last build put in it, and a trace command's
// shader binding table regions.
//
// Vulkan and D3D12 describe the same three things and spell almost nothing the same way, so what
// is drawn here takes normalized shapes and the per-API sourcing lives beside each API: Vulkan's
// just below (its descriptors are what the views were first written against), D3D12's in
// d3d12/raytracing.ts. Nothing in the drawing knows which API it came from.
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { Button } from "./widget/button.js";
import { objectLink, type LinkHandler } from "./args_view.js";
import { MeshPreview } from "./mesh_preview.js";
import {
  instancePosition, instanceScene, isIdentity, unresolvedReference, type GeometryPart,
  type AccelerationInstance,
} from "./acceleration_structure.js";
import { bindingTableRegions, shaderGroups, stageFromFlag, stageLabel } from "./shader_cache.js";
import { d3d12BindingTableRegions, d3d12ShaderGroups, stateObjectInfo } from "./d3d12/raytracing.js";
import {
  functionTableView, linkedFunctionNames, metalDescriptorOf, metalStructureBuild,
} from "./metal/raytracing.js";
import { unresolvedRecords, type BindingTableRecord } from "./binding_table.js";
import { fmt, fmtFlags, formatBytes, isObject, num, refId, str, type ObjectLookup, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { ArgObject, ArgValue } from "../shared/protocol.js";

function row(parent: Widget, label: string, value: string): Div {
  const r = new Div(parent, { class: "draw-state-row" });
  new Span(r, { text: `${label}: `, class: "text-muted" });
  new Span(r, { text: value });
  return r;
}

// ---------------------------------------------------------------------------------------------
// Shader groups

/** A pipeline's or state object's groups as they are drawn: facts above, then one row per group. */
export interface ShaderGroupView {
  /** Label/value rows for what governs every group: recursion depth, payload sizes, stack. */
  facts: [string, string][];
  /** One per group: its heading ("Group 0 (triangles hit)") and what it runs. */
  rows: { label: string; detail: string }[];
  /** A note under the rows, for what the capture cannot say. */
  note?: string;
}

/** "Closest Hit #2 (main)": a stage of a Vulkan ray tracing pipeline by its index in pStages. */
function stageName(pipeline: VulkanObject, index: number | undefined): string {
  if (index === undefined) return "";
  const stages = Array.isArray(pipeline.descriptor?.pStages) ? pipeline.descriptor!.pStages : [];
  const s = stages[index];
  if (!isObject(s)) return `stage #${index}`;
  return `${stageLabel(stageFromFlag(str(s.stage)))} #${index} (${str(s.pName) || "main"})`;
}

/** A Vulkan ray tracing pipeline's groups; null for any other pipeline. */
export function vulkanShaderGroupView(pipeline: VulkanObject): ShaderGroupView | null {
  const groups = shaderGroups(pipeline);
  if (!groups.length) return null;
  const facts: [string, string][] = [];
  const d = pipeline.descriptor;
  if (d?.maxPipelineRayRecursionDepth !== undefined) facts.push(["Max recursion depth", String(num(d.maxPipelineRayRecursionDepth))]);
  const rows = groups.map((g) => {
    const parts = [
      g.general !== undefined ? stageName(pipeline, g.general) : "",
      g.closestHit !== undefined ? `closest hit ${stageName(pipeline, g.closestHit)}` : "",
      g.anyHit !== undefined ? `any hit ${stageName(pipeline, g.anyHit)}` : "",
      g.intersection !== undefined ? `intersection ${stageName(pipeline, g.intersection)}` : "",
    ].filter(Boolean);
    return { label: `Group ${g.index} (${g.type})`, detail: parts.join(", ") || "no shaders" };
  });
  return { facts, rows };
}

/**
 * A D3D12 state object's groups: its hit groups with the exports they name, then every other export
 * the runtime gave an identifier for. Null for a state object with no ray tracing in it.
 */
export function d3d12ShaderGroupView(object: VulkanObject): ShaderGroupView | null {
  const groups = d3d12ShaderGroups(object);
  const info = stateObjectInfo(object);
  if (!groups.length && !info) return null;
  const facts: [string, string][] = [];
  if (info?.maxRecursionDepth !== null && info?.maxRecursionDepth !== undefined) facts.push(["Max recursion depth", String(info.maxRecursionDepth)]);
  if (info?.maxPayloadBytes) facts.push(["Max payload", `${info.maxPayloadBytes} bytes`]);
  if (info?.maxAttributeBytes) facts.push(["Max attributes", `${info.maxAttributeBytes} bytes`]);
  if (info?.pipelineStackSize) facts.push(["Pipeline stack", `${info.pipelineStackSize} bytes`]);

  const stackOf = (name: string | undefined): string => {
    const e = info?.exports.find((x) => x.name === name);
    return e && e.stackSize !== null && e.stackSize > 0 ? `, ${e.stackSize} byte stack` : "";
  };
  const rows = groups.map((g) => {
    const parts = [
      g.closestHitName ? `closest hit ${g.closestHitName}` : "",
      g.anyHitName ? `any hit ${g.anyHitName}` : "",
      g.intersectionName ? `intersection ${g.intersectionName}` : "",
    ].filter(Boolean);
    // A general export is the shader itself; a hit group is a name over the shaders it collects.
    const detail = parts.length ? parts.join(", ") : `${g.name ?? ""}${stackOf(g.name)}` || "no shaders";
    return { label: `${g.name ?? `Group ${g.index}`} (${g.type})`, detail };
  });
  // An export the runtime never gave an identifier for cannot be put in a binding table, so the
  // list is what the table could hold — but only where the description named its exports.
  const note = info?.unlistedExports
    ? "A DXIL library of this state object exports everything in it (NumExports 0), so only the exports the "
      + "application asked the runtime for an identifier for are listed. There may be more."
    : undefined;
  return { facts, rows, ...(note ? { note } : {}) };
}

/** Draws a groups view; nothing for a null one, so a caller can pass either API's straight through. */
export function renderShaderGroups(parent: Widget, view: ShaderGroupView | null): void {
  if (!view || !view.rows.length) return;
  const grp = new collapsible(parent, { label: `Shader Groups (${view.rows.length})`, collapsed: false });
  for (const [label, value] of view.facts) row(grp.body, label, value);
  for (const r of view.rows) row(grp.body, r.label, r.detail);
  if (view.note) new Div(grp.body, { text: view.note, class: "text-muted font-sm" });
}

/** The groups of whichever kind of object was selected, or null when it has none. */
export function shaderGroupViewOf(object: VulkanObject): ShaderGroupView | null {
  if (object.type === "VkPipeline") return vulkanShaderGroupView(object);
  if (object.type === "ID3D12StateObject") return d3d12ShaderGroupView(object);
  return null;
}

// ---------------------------------------------------------------------------------------------
// Acceleration structures

/**
 * The acceleration structure a descriptor names. Vulkan writes a handle reference; D3D12 has no
 * handle to write, so its capture library writes the address the shader will read and the object it
 * minted for that address beside it (src/d3d12/src/descriptors.cpp). An address no build has written
 * to resolves to nothing, which is worth saying plainly rather than calling the structure destroyed.
 */
export function boundStructure(value: ArgValue | undefined, db: ObjectLookup): { object: VulkanObject | null; label: string } {
  const missing = "(destroyed acceleration structure)";
  if (!isObject(value)) {
    const direct = db.getObject(refId(value as ArgValue));
    return direct ? { object: direct, label: direct.name } : { object: null, label: missing };
  }
  const direct = db.getObject(refId(value));
  if (direct) return { object: direct, label: direct.name };
  const structure = db.getObject(refId(value.structure));
  if (structure) return { object: structure, label: structure.name };
  const address = str(value.address);
  return { object: null, label: address ? `structure at ${address} (nothing built there)` : missing };
}

/** What this needs of a database to resolve an instance's reference: every structure it knows. */
export interface StructureLookup {
  getObjectsOfType(type: string): Map<number, VulkanObject> | null;
}

/**
 * The address the capture library recorded on each acceleration structure, so an instance's
 * reference can be turned back into the object it names (src/vulkan/src/hooks.cpp,
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
  /** The boxes a procedural bottom level was built from, which has no triangles to give. */
  boxesOf?: (blas: number) => Float32Array | null;
  /** Every geometry of a bottom level apart, triangles or boxes, or null when its build is not in the capture. */
  partsOf?: (blas: number) => GeometryPart[] | null;
  /**
   * Every geometry of a bottom level in the shape a ray traversal walks, which is not the shape that
   * draws one: a procedural geometry's boxes as their extents rather than as the endpoints of their
   * edges, and each geometry's own intersection function table offset and opaque flag, which a
   * traversal needs and a drawing does not (msl/raytracing.ts).
   */
  traversalOf?: (blas: number) => TraversalGeometry[] | null;
}

/** One geometry of a bottom level, for a ray traversal. */
export interface TraversalGeometry {
  /** Its index in the build, which is what `geometry_id` reports. */
  index: number;
  /** Triangles as nine floats each, in the bottom level's own space. */
  triangles: Float32Array | null;
  /** Boxes as six floats each, min then max. A procedural geometry has these instead. */
  extents: Float32Array | null;
  /** The entry of a bound intersection function table this geometry's boxes call. */
  functionTableOffset: number;
  /** The build marked it opaque, so no intersection function is called for it. */
  opaque: boolean;
}

/** An acceleration structure as the Inspect panel draws it, whichever API it came from. */
export interface StructureView {
  /** Label/value rows above the build: what kind it is and how big. */
  facts: [string, string][];
  /** The buffer the structure lives in, and where in it. */
  storage: { object: VulkanObject; note: string } | null;
  build: BuildView | null;
  /** What to say instead of a build, when none was seen. */
  notBuiltNote: string;
}

export interface BuildView {
  /** "vkCmdBuildAccelerationStructuresKHR (BUILD)". */
  title: string;
  flags: string;
  /** "3 primitives in 1 geometry". */
  primitives: string;
  /** One per geometry: its heading and what it was built from. */
  geometries: { label: string; detail: string }[];
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
  const scene = instanceScene(instances, s.meshOf, s.boxesOf);
  if (scene.mesh.length) {
    const box = new Div(grp.body, { class: "accel-preview" });
    const preview = new MeshPreview(box);
    preview.setMesh({ positions: scene.mesh, kind: scene.kind, clip: false });
    // What was drawn decides what can honestly be claimed: a stand-in cube per instance is not the
    // same statement as the geometry, and a procedural level's boxes are its real shape.
    const caption = scene.drawn === "triangles"
      ? `${scene.placed} of ${instances.length} instances drawn with the geometry their bottom level was built from.`
      : scene.drawn === "aabbs"
      ? `${scene.placed} of ${instances.length} instances drawn with the bounding boxes their bottom level was built from: `
        + "a procedural bottom level has no triangles, and its boxes are what the traversal tests against."
      : "The bottom levels' geometry is not in this capture — a bottom level is usually built once, "
        + "before any capture — so each instance is drawn as a box where its transform puts it.";
    new Div(grp.body, { text: caption, class: "text-muted font-sm" });
  }
  for (const i of instances) {
    const blas = i.blas !== undefined ? db.getObject(i.blas) : null;
    const r = row(grp.body, `Instance ${i.index}`, "");
    if (blas) objectLink(r, blas, onLink);
    else new Span(r, { text: unresolvedReference(i), class: "text-muted" });
    new Span(r, { text: `  ${placement(i)}, mask 0x${i.mask.toString(16).toUpperCase()}` });
    if (i.customIndex) new Span(r, { text: `, custom index ${i.customIndex}` });
    if (i.bindingTableOffset) new Span(r, { text: `, hit group +${i.bindingTableOffset}` });
    if (i.flagNames.length) new Span(r, { text: `, ${i.flagNames.join(" | ")}`, class: "text-muted" });
  }
}

/** A VkAccelerationStructureKHR, from its create info and the build the layer recorded on it. */
export function vulkanStructureView(object: VulkanObject, db: ObjectLookup): StructureView {
  const facts: [string, string][] = [];
  let storage: StructureView["storage"] = null;
  const d = object.descriptor;
  if (d) {
    facts.push(["Type", fmt(d.type)]);
    facts.push(["Size", formatBytes(num(d.size))]);
    const buffer = db.getObject(refId(d.buffer));
    if (buffer) storage = { object: buffer, note: `at offset ${num(d.offset)}` };
  }
  const build = isObject(object.updates.build) ? object.updates.build as ArgObject : null;
  return {
    facts, storage,
    notBuiltNote: "Not built while the inspector was watching.",
    build: build ? {
      title: `${str(build.method)} (${fmt(build.mode)})`,
      flags: str(build.flags) ? fmtFlags(build.flags) : "",
      primitives: countPhrase(num(build.primitiveCount), Array.isArray(build.geometries) ? build.geometries.filter(isObject).length : 0),
      geometries: (Array.isArray(build.geometries) ? build.geometries.filter(isObject) : []).map((g, i) => {
        const kind = fmt(g.geometryType);
        const detail = str(g.geometryType).includes("TRIANGLES")
          ? `${num(g.primitiveCount).toLocaleString()} triangles, ${fmt(g.vertexFormat)} vertices (stride ${num(g.vertexStride)}, up to vertex ${num(g.maxVertex)}), ${fmt(g.indexType)} indices`
          : str(g.geometryType).includes("AABBS") ? `${num(g.primitiveCount).toLocaleString()} boxes, stride ${num(g.stride)}`
          : `${num(g.primitiveCount).toLocaleString()} instances${g.arrayOfPointers ? " (array of pointers)" : ""}`;
        return { label: `Geometry ${i} (${kind}${str(g.flags) ? `, ${fmtFlags(g.flags)}` : ""})`, detail };
      }),
    } : null,
  };
}

/**
 * An ID3D12RaytracingAccelerationStructure: the object the capture library mints per destination
 * address, since D3D12 has none of its own (src/d3d12/src/raytracing.h). There is no create info to
 * read a type or a size from — the structure is whatever the last build wrote there — so both come
 * from the build, and the address stands in for a handle.
 */
export function d3d12StructureView(object: VulkanObject, db: ObjectLookup): StructureView {
  const facts: [string, string][] = [];
  let storage: StructureView["storage"] = null;
  const address = isObject(object.descriptor?.Address) ? object.descriptor!.Address as ArgObject : null;
  const build = isObject(object.updates.build) ? object.updates.build as ArgObject : null;
  if (build) facts.push(["Type", fmt(build.Type)]);
  if (address) {
    facts.push(["Address", str(address.address)]);
    const buffer = db.getObject(refId(address.buffer));
    if (buffer) storage = { object: buffer, note: `at offset ${num(address.offset)}` };
  }
  const copied = isObject(object.updates.copiedFrom) ? object.updates.copiedFrom as ArgObject : null;
  if (copied) facts.push(["Copied from", `${str(copied.sourceAddress)} (${fmt(copied.Mode)})`]);

  return {
    facts, storage,
    notBuiltNote: "No build of this structure is in the capture.",
    build: build ? {
      title: `${str(build.method)} (${build.update === true ? "UPDATE" : "BUILD"})`,
      flags: str(build.Flags) ? fmtFlags(build.Flags) : "",
      primitives: build.Type !== undefined && str(build.Type).includes("TOP_LEVEL")
        ? `${num(build.NumDescs).toLocaleString()} instance${num(build.NumDescs) === 1 ? "" : "s"}`
        : countPhrase(num(build.primitiveCount), num(build.NumDescs)),
      geometries: (Array.isArray(build.geometries) ? build.geometries.filter(isObject) : []).map((g, i) => {
        const type = str(g.Type);
        const kind = fmt(g.Type);
        let detail: string;
        if (type.includes("PROCEDURAL_PRIMITIVE_AABBS")) {
          const aabbs = isObject(g.AABBs) ? g.AABBs : {};
          detail = `${num(aabbs.AABBCount).toLocaleString()} boxes`;
        } else {
          const tri = isObject(g.Triangles) ? g.Triangles : {};
          const buffer = isObject(tri.VertexBuffer) ? tri.VertexBuffer : {};
          const indexed = str(tri.IndexFormat) !== "DXGI_FORMAT_UNKNOWN" && num(tri.IndexCount) > 0;
          detail = `${num(tri.VertexCount).toLocaleString()} ${fmt(tri.VertexFormat)} vertices (stride ${num(buffer.StrideInBytes)})`
                 + (indexed ? `, ${num(tri.IndexCount).toLocaleString()} ${fmt(tri.IndexFormat)} indices` : ", no indices")
                 + (tri.Transform3x4 ? ", with a transform" : "");
        }
        return { label: `Geometry ${i} (${kind}${str(g.Flags) ? `, ${fmtFlags(g.Flags)}` : ""})`, detail };
      }),
    } : null,
  };
}

function countPhrase(primitives: number, geometries: number): string {
  return `${primitives.toLocaleString()} in ${geometries} geometr${geometries === 1 ? "y" : "ies"}`;
}

/**
 * An MTLAccelerationStructure: an ordinary Metal object, so unlike D3D12 there is nothing to mint
 * and unlike Vulkan there is no buffer and offset to report — the structure *is* the allocation,
 * and it carries its own size.
 *
 * A structure built before the capture is the common case and still says what it is: the library
 * records the build whether or not anything is capturing, so `build` is there either way, and
 * `captureInputs` is what was read back of it when the capture began
 * (src/metal/src/raytracing.h).
 */
export function metalStructureView(object: VulkanObject, db: ObjectLookup): StructureView {
  const facts: [string, string][] = [];
  const d = object.descriptor;
  const build = metalStructureBuild(object);
  const descriptor = metalDescriptorOf(build) ?? (isObject(d?.descriptor) ? d!.descriptor as ArgObject : null);
  if (descriptor) facts.push(["Type", str(descriptor.kind) === "instance" ? "Instance (top level)" : "Primitive (bottom level)"]);
  if (d) facts.push(["Size", formatBytes(num(d.size))]);
  // A structure in a heap is the one case where it has storage to name that is not its own.
  const heap = db.getObject(refId(d?.heap));
  const storage = heap ? { object: heap, note: `at offset ${num(d?.heapOffset)}` } : null;

  const copied = build && num(build.copiedFrom) ? db.getObject(num(build.copiedFrom)) : null;
  if (copied) facts.push(["Copied from", copied.name]);

  return {
    facts, storage,
    notBuiltNote: "No build of this structure was seen. A structure is built by an acceleration structure "
      + "encoder, and one built before the inspector attached is not recorded.",
    build: build && descriptor ? {
      title: `${str(build.method).replace(/:.*/, ":")} (${str(build.mode) || "BUILD"})`,
      flags: str(descriptor.usage) && str(descriptor.usage) !== "None" ? fmtFlags(descriptor.usage) : "",
      primitives: str(descriptor.kind) === "instance"
        ? `${num(descriptor.instanceCount).toLocaleString()} instance${num(descriptor.instanceCount) === 1 ? "" : "s"}`
        : countPhrase(num(descriptor.primitiveCount),
                      Array.isArray(descriptor.geometries) ? descriptor.geometries.filter(isObject).length : 0),
      geometries: metalGeometryRows(descriptor),
    } : null,
  };
}

/** One row per geometry of a Metal build: what it is and what it was built from. */
function metalGeometryRows(descriptor: ArgObject): { label: string; detail: string }[] {
  if (str(descriptor.kind) === "instance") {
    const structures = Array.isArray(descriptor.instancedAccelerationStructures)
      ? descriptor.instancedAccelerationStructures.length : 0;
    const detail = `${num(descriptor.instanceCount).toLocaleString()} instances of stride `
      + `${num(descriptor.instanceDescriptorStride)}, over ${structures} bottom level${structures === 1 ? "" : "s"}`
      + (descriptor.indirect === true ? " (indirect: the count comes from a buffer)" : "");
    return [{ label: `Instances (${fmt(descriptor.instanceDescriptorType)})`, detail }];
  }
  const raw = Array.isArray(descriptor.geometries) ? descriptor.geometries.filter(isObject) : [];
  return raw.map((g, i) => {
    const kind = str(g.kind);
    let detail: string;
    if (kind === "triangles" || kind === "motionTriangles") {
      const indexed = str(g.indexType).endsWith("UInt16") || str(g.indexType).endsWith("UInt32");
      detail = `${num(g.triangleCount).toLocaleString()} triangles, ${fmt(g.vertexFormat)} vertices `
             + `(stride ${num(g.vertexStride)})`
             + (indexed ? `, ${fmt(g.indexType)} indices` : ", no indices")
             + (g.transformationMatrixBuffer ? ", with a transform" : "");
    } else if (kind === "boundingBoxes" || kind === "motionBoundingBoxes") {
      detail = `${num(g.boundingBoxCount).toLocaleString()} boxes, stride ${num(g.boundingBoxStride)}`;
    } else if (kind === "curves" || kind === "motionCurves") {
      detail = `${num(g.segmentCount).toLocaleString()} segments of ${num(g.segmentControlPointCount)} control points, `
             + `${fmt(g.curveType)} ${fmt(g.curveBasis)}`;
    } else {
      detail = `${num(g.primitiveCount).toLocaleString()} primitives`;
    }
    // Which entry of the bound intersection function table this geometry's primitives reach, which
    // Metal has in place of a hit group offset.
    const reach = g.opaque === true ? "opaque" : `intersection function +${num(g.intersectionFunctionTableOffset)}`;
    const label = `Geometry ${i} (${kind}${str(g.label) ? `: ${str(g.label)}` : ""})`;
    return { label, detail: `${detail} — ${reach}` };
  });
}

/** The view of whichever kind of structure was selected, or null when the object is not one. */
export function structureViewOf(object: VulkanObject, db: ObjectLookup): StructureView | null {
  if (object.type === "VkAccelerationStructureKHR") return vulkanStructureView(object, db);
  if (object.type === "ID3D12RaytracingAccelerationStructure") return d3d12StructureView(object, db);
  if (object.type === "MTLAccelerationStructure") return metalStructureView(object, db);
  return null;
}

/**
 * Opening a structure in its tab, as the Inspect panel offers it. `note` is non-empty when there is
 * nothing to show, and is then the reason — which is what the panel says instead of offering a view
 * that would open empty.
 */
export interface StructureOpener {
  note: string;
  open(): void;
}

/** An acceleration structure: what it is, where it lives, and the geometries its last build held. */
export function renderAccelerationStructure(parent: Widget, view: StructureView, db: ObjectLookup, onLink: LinkHandler,
                                            scene?: AccelerationScene | null, opener?: StructureOpener | null): void {
  const grp = new collapsible(parent, { label: "Acceleration Structure", collapsed: false });
  if (opener) {
    const bar = new Div(grp.body, { class: "accel-open-row" });
    new Button(bar, {
      label: "View in a Tab", class: "btn btn-sm", disabled: !!opener.note,
      tooltip: opener.note || "What it was built from, in a tab of its own with the mesh view's camera and shading",
      callback: () => opener.open(),
    });
    // Said beside the button rather than only in its tooltip: which structures can be looked at, and
    // why the others cannot, is the question this answers.
    if (opener.note) new Span(bar, { text: `  ${opener.note}`, class: "text-muted font-sm" });
  }
  for (const [label, value] of view.facts) row(grp.body, label, value);
  if (view.storage) {
    const r = row(grp.body, "Buffer", "");
    objectLink(r, view.storage.object, onLink);
    new Span(r, { text: `  ${view.storage.note}`, class: "text-muted" });
  }
  const build = view.build;
  if (!build) {
    new Div(grp.body, { text: view.notBuiltNote, class: "text-muted" });
    return;
  }
  row(grp.body, "Last build", build.title);
  if (build.flags) row(grp.body, "Build flags", build.flags);
  row(grp.body, "Primitives", build.primitives);
  for (const g of build.geometries) row(grp.body, g.label, g.detail);
  // The instances are what a top level actually holds, and the only view of an otherwise opaque
  // object; a bottom level has geometry instead, drawn from the buffers its build read.
  if (scene && scene.instances.length) renderInstances(parent, scene, db, onLink);
}

// ---------------------------------------------------------------------------------------------
// The shader binding table

/** One region of a trace's table, as the view draws it. */
export interface TableRegion {
  region: string;
  records: number;
  stride: number;
  size: number;
}

/** The regions of a trace command, whichever API recorded it. */
export function tableRegionsOf(method: string, args: ArgObject | null): TableRegion[] {
  return method === "DispatchRays" ? d3d12BindingTableRegions(args) : bindingTableRegions(args);
}

/**
 * What a record runs, as the record itself resolved it: an export name where the API gives one
 * (D3D12), else the group it matched named through `groupName`.
 */
function recordDetail(r: BindingTableRecord, groupName: (group: number) => string): string {
  if (r.name) return `${r.name}${r.dataBytes ? `, ${r.dataBytes} bytes of record data` : ""}`;
  if (r.group === null) return `no shader of this pipeline has this handle (${r.handle.slice(0, 16)}...)`;
  return `${groupName(r.group)}${r.dataBytes ? `, ${r.dataBytes} bytes of record data` : ""}`;
}

/** "closest hit Closest Hit #2 (main)": a Vulkan pipeline's shader group by its index. */
export function vulkanGroupName(pipeline: VulkanObject | null | undefined): (group: number) => string {
  return (group: number): string => {
    if (!pipeline) return `group ${group}`;
    const g = shaderGroups(pipeline)[group];
    if (!g) return `group ${group}`;
    const stage = stageName(pipeline, g.general ?? g.closestHit ?? g.anyHit ?? g.intersection);
    return `group ${group}: ${g.type}${stage ? ` (${stage})` : ""}`;
  };
}

/** A trace command's shader binding table regions, as records of the table. */
export function renderBindingTable(parent: Widget, regions: TableRegion[], records: BindingTableRecord[] | undefined,
                                   groupName: (group: number) => string, unresolvedNote: string): void {
  if (!regions.length) return;
  const grp = new collapsible(parent, { label: "Shader Binding Table", collapsed: false });
  for (const r of regions) {
    row(grp.body, r.region, r.size ? `${r.records} record${r.records === 1 ? "" : "s"} (stride ${r.stride}, ${r.size} bytes)` : "none");
  }
  if (!records || !records.length) {
    new Div(grp.body, {
      text: "The table's contents are not in this capture, so which shader each record holds is not known. "
        + "Capture again to read it back.",
      class: "text-muted font-sm",
    });
    return;
  }
  // What each record actually runs. A handle matching nothing is the interesting case: those rays
  // run the wrong shader or none, and nothing else in a capture would show it.
  for (const r of records) row(grp.body, `${r.region} record ${r.index}`, recordDetail(r, groupName));
  const unresolved = unresolvedRecords(records);
  if (unresolved.length) {
    new Div(grp.body, {
      text: `${unresolved.length} record${unresolved.length === 1 ? " holds a handle" : "s hold handles"} `
        + unresolvedNote,
      class: "text-muted font-sm",
    });
  }
}

/** The note under a table whose records did not all resolve, per API. */
export const VULKAN_UNRESOLVED_NOTE =
  "this pipeline never gave out — a table filled from another pipeline, or from handles fetched before this one was "
  + "rebuilt. Rays reaching those records run the wrong shader or none.";
export const D3D12_UNRESOLVED_NOTE =
  "this state object never gave out — a table filled from another state object, or from identifiers fetched before "
  + "this one was rebuilt. Rays reaching those records run the wrong shader or none. A state object whose library "
  + "exports everything can also have exports the capture never saw an identifier for.";

// ---------------------------------------------------------------------------------------------
// Intersection function tables (Metal)
//
// Metal's place in the binding table's stead, and a different kind of thing. There is no table in
// GPU memory and no opaque handle: a pipeline hands out an MTLIntersectionFunctionTable, the
// application sets one entry per index through the API, and a traversal calls entry N when it
// reaches a primitive whose `intersectionFunctionTableOffset` is N. So the capture knows the table
// exactly — there is nothing to read back and nothing that can fail to resolve, which is why this
// draws no "unresolved records" note the way the other two do.
//
// What *can* go wrong is the offset: a geometry or instance declaring one past the table's end
// reaches no function at all, and neither Metal nor its validation layer says so.

/** A function table as the panel draws it, with the pipeline's linked functions for context. */
export function renderFunctionTable(parent: Widget, object: VulkanObject, db: ObjectLookup,
                                    onLink: LinkHandler): void {
  const view = functionTableView(object);
  if (!view) return;
  const intersection = object.type === "MTLIntersectionFunctionTable";
  const label = intersection ? "Intersection Function Table" : "Visible Function Table";
  const grp = new collapsible(parent, { label: `${label} (${view.entries.length})`, collapsed: false });

  const pipeline = db.getObject(object.parentId);
  if (pipeline) {
    const r = row(grp.body, "Pipeline", "");
    objectLink(r, pipeline, onLink);
  }
  row(grp.body, "Entries", String(view.functionCount));

  // The functions the pipeline was linked with: what an entry is allowed to hold. An empty list on
  // a table with entries means the pipeline was made without linkedFunctions, which cannot work —
  // worth showing rather than leaving the section looking complete.
  const linked = linkedFunctionNames(pipeline);
  if (linked.length) row(grp.body, "Linked functions", linked.join(", "));

  for (const e of view.entries) {
    const detail = e.empty ? "nothing set — a ray reaching this entry calls no function"
                 : e.opaque ? `built-in ${e.opaque} intersection${e.signature && e.signature !== "None" ? ` (${e.signature})` : ""}`
                 : e.function || "(unnamed function)";
    row(grp.body, `Entry ${e.index}`, detail);
  }
  if (!view.entries.length) {
    new Div(grp.body, {
      text: "Nothing was set in this table while the inspector was watching. A table filled before it attached "
        + "is not recorded, so which function each entry holds is not known.",
      class: "text-muted font-sm",
    });
  }

  // The table's own bindings, which its intersection functions read through — a second set of
  // arguments that nothing else in a capture would show, since they are not on any encoder.
  for (const b of view.buffers) {
    const buffer = db.getObject(b.buffer);
    const r = row(grp.body, `Buffer ${b.index}`, "");
    if (buffer) objectLink(r, buffer, onLink);
    else new Span(r, { text: `buffer ${b.buffer}`, class: "text-muted" });
    if (b.offset) new Span(r, { text: `  +${b.offset}`, class: "text-muted" });
  }
  for (const v of view.visibleFunctionTables) {
    const table = db.getObject(v.table);
    const r = row(grp.body, `Visible function table ${v.index}`, "");
    if (table) objectLink(r, table, onLink);
  }
}

/**
 * Whether a geometry's or instance's table offset lands on an entry of `table`.
 *
 * The one correctness check this section is really for: an offset past the end reaches no function,
 * the traversal treats the primitive as it would an unhandled one, and nothing else in a capture —
 * nor Metal's own validation — reports it.
 */
export function tableOffsetIsInRange(table: VulkanObject | null, offset: number): boolean | null {
  const view = table ? functionTableView(table) : null;
  if (!view || !view.functionCount) return null;
  return offset < view.functionCount;
}
