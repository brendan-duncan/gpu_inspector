// Which shader group each record of a shader binding table holds.
//
// A trace does not name the shaders it runs. It names four regions of memory, and each record in
// them begins with an opaque handle the driver gave for one of the pipeline's shader groups — so a
// table read on its own is bytes, and the only way to say what a record runs is to match its handle
// against the handles the driver handed out (`vkGetRayTracingShaderGroupHandlesKHR`, or D3D12's
// `ID3D12StateObjectProperties::GetShaderIdentifier`).
//
// The capture libraries keep both halves: the handles (a blob on a Vulkan pipeline, a named list on
// a D3D12 state object) and the regions' contents read back from the addresses the trace pointed at
// (src/vulkan/src/hooks.cpp, src/d3d12/src/raytracing.h). This matches them.
//
// Walking the records is the same work either way; what differs is only what a handle is matched
// against, which is why that is a callback. Vulkan gets back a group index and D3D12 an export
// name, and a record carries whichever its API gave.
//
// Whatever follows the handle in a record is the application's own — the shader record data a group
// reads through `shaderRecordEXT`, or a D3D12 local root signature's arguments — so its size is
// reported rather than its meaning guessed at.

import { isObject, num, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { ArgObject } from "../shared/protocol.js";

/** One record of one region of the table. */
export interface BindingTableRecord {
  /** "raygen", "miss", "hit" or "callable". */
  region: string;
  /** Index of the record within its region, which is what a trace's offsets count in. */
  index: number;
  /** The pipeline shader group this record runs, or null when its handle matches none. */
  group: number | null;
  /** D3D12: the export the identifier names, or null when it matches none of the state object's. */
  name?: string | null;
  /** The handle as hex, which is the only thing to show when it matches no group. */
  handle: string;
  /** Bytes of the record after the handle: the application's own shader record data. */
  dataBytes: number;
}

function hex(bytes: Uint8Array): string {
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
}

function sameBytes(a: Uint8Array, b: Uint8Array, length: number): boolean {
  for (let i = 0; i < length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

/**
 * The records of one region, matched against the pipeline's group handles.
 *
 * `handles` is the blob the driver filled, one handle of `handleSize` per group in group order.
 * Returns nothing when the region has no stride to walk by, or no handles to match against — a
 * record whose group cannot be named is still listed, with its handle, because a handle matching
 * nothing is itself worth seeing: it means the table holds something the pipeline did not give it.
 */
export function bindingTableRecords(region: string, contents: Uint8Array | null, stride: number,
                                    handles: Uint8Array | null, handleSize: number): BindingTableRecord[] {
  const groups = handles ? Math.floor(handles.byteLength / handleSize) : 0;
  return walkRecords(region, contents, stride, handleSize, (handle) => {
    for (let g = 0; g < groups; g++) {
      if (handles && sameBytes(handle, handles.subarray(g * handleSize, (g + 1) * handleSize), handleSize)) {
        return { group: g };
      }
    }
    return null;
  });
}

/** What a handle turned out to name: a group index, an export name, or both. */
export interface HandleMatch {
  group?: number;
  name?: string;
}

/**
 * The records of one region, `match` deciding what each record's handle names. A record whose
 * handle matches nothing is still listed, with the handle, because a handle matching nothing is
 * itself worth seeing: it means the table holds something the pipeline did not give it.
 */
export function walkRecords(region: string, contents: Uint8Array | null, stride: number, handleSize: number,
                            match: (handle: Uint8Array) => HandleMatch | null): BindingTableRecord[] {
  if (!contents || stride <= 0 || handleSize <= 0) return [];
  const out: BindingTableRecord[] = [];
  for (let at = 0, index = 0; at + handleSize <= contents.byteLength; at += stride, index++) {
    const handle = contents.subarray(at, at + handleSize);
    const hit = match(handle);
    out.push({
      region, index,
      group: hit?.group ?? null,
      ...(hit?.name !== undefined ? { name: hit.name } : {}),
      handle: hex(handle),
      // The last record can be short when the read-back was cut off; only count what is there.
      dataBytes: Math.max(0, Math.min(stride, contents.byteLength - at) - handleSize),
    });
  }
  return out;
}

/**
 * Whether a table's records all resolved. A record that matched nothing is the interesting case —
 * an application filling its table from the wrong pipeline, or from handles fetched before a
 * pipeline was rebuilt, gets rays that run the wrong shader or none.
 */
export function unresolvedRecords(records: BindingTableRecord[]): BindingTableRecord[] {
  return records.filter((r) => r.group === null && !r.name);
}

/** What building a table's records needs from a capture and its database. */
export interface BindingTableSource {
  /** The `bindingTableData` the layer put on the trace command: a capture id per region. */
  captures: { region: string; capture: number }[];
  /** The trace's own arguments, which give each region's stride. */
  args: ArgObject | null;
  /** Contents of a capture's buffer read-back. */
  bytesOf(captureId: number): Uint8Array | null;
  /** The pipeline bound at the trace, for its group handles. */
  pipeline: VulkanObject | null;
  /** The blob a pipeline carries, by name. */
  blobOf(object: VulkanObject, name: string): Uint8Array | null;
}

const REGION_ARG: Record<string, string> = {
  raygen: "pRaygenShaderBindingTable",
  miss: "pMissShaderBindingTable",
  hit: "pHitShaderBindingTable",
  callable: "pCallableShaderBindingTable",
};

/**
 * Every record of a trace's binding table, across its regions, matched to the pipeline's groups.
 * Empty when the capture did not read the table back — which is every capture taken before the
 * layer resolved the trace's addresses.
 */
export function tableRecords(source: BindingTableSource): BindingTableRecord[] {
  const handles = source.pipeline ? source.blobOf(source.pipeline, "group handles") : null;
  const declared = source.pipeline?.updates.shaderGroupHandles;
  const handleSize = isObject(declared) ? num(declared.handleSize) : 0;
  const out: BindingTableRecord[] = [];
  for (const c of source.captures) {
    const region = isObject(source.args) ? source.args[REGION_ARG[c.region] ?? ""] : undefined;
    const stride = isObject(region) ? num(region.stride) : 0;
    out.push(...bindingTableRecords(c.region, source.bytesOf(c.capture), stride, handles, handleSize));
  }
  return out;
}
