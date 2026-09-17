// What a Metal process holds, broken down by what is holding it.
//
// The other two backends have a heap table: the device says how many heaps it has and how large,
// every allocation names the one it came from, and the breakdown is that table filled in
// (renderer/memory_heaps.ts). Metal has nothing of the kind. `currentAllocatedSize` is a single
// number for the whole device, and there is no residency figure separate from it, so the Memory Use
// section could only ever show the series over time.
//
// What Metal does give is the size of every resource: `MTLResource.allocatedSize` is what the
// object actually cost, which the library records on each one as it is created. So the breakdown
// here is by *object kind* rather than by heap — buffers, textures, and the heaps themselves — and
// it is a different shape from the other two on purpose, because the thing being counted is
// different.
//
// The one trap is double counting. A resource created from a heap is suballocated out of memory the
// heap already reserved, so adding its `allocatedSize` to the heap's would count those bytes twice.
// Such resources are therefore kept apart: the heap's own size is what the process holds, and the
// resources inside it are reported as what is using that reservation.
import { isObject, num, refId, type VulkanObject } from "../vulkan/vulkan_object.js";
import type { MemoryDatabase } from "../memory_heaps.js";

export interface MetalMemoryGroup {
  /** "Buffers", "Textures", "Heaps". */
  label: string;
  count: number;
  bytes: number;
  largestBytes: number;
}

export interface MetalMemory {
  /** Every group's bytes: standalone resources plus the heaps' own reservations, counted once. */
  totalBytes: number;
  /** Largest first, and empty groups left out. */
  groups: MetalMemoryGroup[];
  /** Resources suballocated from a heap. Their bytes are the heaps' and are not in `totalBytes`. */
  inHeaps: { count: number; bytes: number };
  /** What the heaps say they are using of what they reserved, where they reported it. */
  heapUsedBytes: number;
  heapReservedBytes: number;
}

/** `allocatedSize` as the library recorded it, or 0. */
function sizeOf(o: VulkanObject): number {
  const a = isObject(o.args) ? o.args : null;
  return a ? num(a.allocatedSize) : 0;
}

/** The heap a resource was suballocated from, or null for one with memory of its own. */
function heapOf(o: VulkanObject): number | null {
  const a = isObject(o.args) ? o.args : null;
  return a ? refId(a.heap) : null;
}

/**
 * The breakdown, or null for a session with no Metal resources in it — which is every non-Metal
 * capture, so the caller can ask unconditionally.
 */
export function metalMemory(db: MemoryDatabase): MetalMemory | null {
  const groups = new Map<string, MetalMemoryGroup>();
  const inHeaps = { count: 0, bytes: 0 };
  let heapUsedBytes = 0;
  let heapReservedBytes = 0;
  let sawResource = false;

  const add = (label: string, bytes: number): void => {
    let g = groups.get(label);
    if (!g) groups.set(label, (g = { label, count: 0, bytes: 0, largestBytes: 0 }));
    g.count++;
    g.bytes += bytes;
    g.largestBytes = Math.max(g.largestBytes, bytes);
  };

  for (const o of db.allObjects.values()) {
    if (o.isDeleted) continue;
    if (o.type === "MTLHeap") {
      sawResource = true;
      // A heap's usage moves as resources are made from it, and the library updates it each time
      // (`UpdateObject(self, "usage", ...)`). The creation arguments hold the figures from the
      // moment it was made, when a heap has necessarily used none of itself — reading those would
      // report every heap as entirely empty.
      const usage = isObject(o.updates.usage) ? o.updates.usage : null;
      const a = isObject(o.args) ? o.args : null;
      // What the heap holds: its current size where it reported one (a sparse heap grows), else
      // the size it was created with.
      const size = num(usage?.currentAllocatedSize) || sizeOf(o);
      heapReservedBytes += size;
      heapUsedBytes += usage ? num(usage.usedSize) : a ? num(a.usedSize) : 0;
      add("Heaps", size);
      continue;
    }
    if (o.type !== "MTLBuffer" && o.type !== "MTLTexture") continue;
    sawResource = true;
    const bytes = sizeOf(o);
    if (heapOf(o) !== null) {
      // Suballocated: these bytes are the heap's, already counted above.
      inHeaps.count++;
      inHeaps.bytes += bytes;
      continue;
    }
    add(o.type === "MTLBuffer" ? "Buffers" : "Textures", bytes);
  }
  if (!sawResource) return null;

  const list = [...groups.values()].filter((g) => g.count > 0).sort((a, b) => b.bytes - a.bytes);
  return {
    totalBytes: list.reduce((sum, g) => sum + g.bytes, 0),
    groups: list,
    inHeaps,
    heapUsedBytes,
    heapReservedBytes,
  };
}

/**
 * How much of what the heaps reserved is actually being used, 0 to 1, or null when no heap said.
 * A heap holding far less than it reserved is memory the process has taken and is not using.
 */
export function heapOccupancy(m: MetalMemory): number | null {
  return m.heapReservedBytes > 0 ? m.heapUsedBytes / m.heapReservedBytes : null;
}

/** Below this, a heap's reservation is worth mentioning as mostly empty. */
export const HEAP_OCCUPANCY_LOW = 0.5;
