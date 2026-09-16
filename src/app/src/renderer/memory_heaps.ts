// What the application has allocated, per memory heap.
//
// Inspect already totals memory across every object. That answers "how much" but not the two
// questions that decide whether it is a problem: how much of *which* heap, and how close that heap
// is to full. A gigabyte in a 16 GB device-local heap and a gigabyte in a 256 MB one are different
// situations, and neither is visible from a single total.
//
// Everything here is derived from what the object graph already carries: each VkDeviceMemory records
// the size and memory type it was allocated with, and the physical device records its heaps and the
// types that draw from them. The driver's own view — how much of a heap is resident and what it will
// let this process have — is a separate thing it must be asked for (VK_EXT_memory_budget), and the
// layer attaches it to the physical device as `memoryBudget` when the device offers it.
import { isObject, num, str, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { MemorySampleMessage } from "../shared/protocol.js";

/** What this needs of an object database: every object, since allocations are found by walking them. */
export interface MemoryDatabase {
  allObjects: Map<number, VulkanObject>;
  /**
   * Memory use over the session, where the session recorded it (renderer/memory_timeline.ts).
   * Absent for a loaded capture file, which holds one instant rather than a series.
   */
  memorySamples?: MemorySampleMessage[];
}

/** One memory type drawing from a heap. */
export interface MemoryTypeUse {
  index: number;
  /** "DEVICE_LOCAL | HOST_VISIBLE", as the layer spells the flags. */
  propertyFlags: string;
  allocations: number;
  bytes: number;
}

export interface HeapUse {
  index: number;
  /** The heap's total size as the device reports it. */
  sizeBytes: number;
  /** "VK_MEMORY_HEAP_DEVICE_LOCAL_BIT", or "0" for a host heap. */
  flags: string;
  deviceLocal: boolean;
  /** What this application has allocated from it, and in how many allocations. */
  allocations: number;
  bytes: number;
  largestBytes: number;
  /** Share of the heap the application holds, or null when the heap reports no size. */
  share: number | null;
  types: MemoryTypeUse[];
  /**
   * The driver's own view, where the device has VK_EXT_memory_budget: what it will let this process
   * have, and what is resident from every process. `usageBytes` above `bytes` is other processes
   * and the driver's own overhead.
   */
  budgetBytes?: number;
  usageBytes?: number;
}

export interface MemoryHeaps {
  heaps: HeapUse[];
  /** Everything the application has allocated, across every heap. */
  totalBytes: number;
  allocations: number;
  /** The driver reported its budget, so residency and pressure are real rather than derived. */
  hasBudget: boolean;
}

/** A heap holding more than this share of itself is worth pointing at. */
export const HEAP_PRESSURE_SHARE = 0.8;

/**
 * The object a backend hangs its memory properties on: Vulkan's physical device, and on D3D12 the
 * adapter, which is the same thing — what the GPU offers, as opposed to what was made from it
 * (src/d3d12/src/cpu_timeline.h).
 */
function isMemoryDevice(type: string): boolean {
  return type === "VkPhysicalDevice" || type === "IDXGIAdapter";
}

function memoryPropertiesOf(db: MemoryDatabase): { heaps: { size: number; flags: string }[]; types: { heapIndex: number; propertyFlags: string }[] } | null {
  for (const o of db.allObjects.values()) {
    if (!isMemoryDevice(o.type)) continue;
    const mp = o.updates.memoryProperties;
    if (!isObject(mp)) continue;
    const heapCount = num(mp.memoryHeapCount);
    const typeCount = num(mp.memoryTypeCount);
    const rawHeaps = Array.isArray(mp.memoryHeaps) ? mp.memoryHeaps : [];
    const rawTypes = Array.isArray(mp.memoryTypes) ? mp.memoryTypes : [];
    // The arrays are fixed-length in Vulkan; only the first count entries mean anything.
    return {
      heaps: rawHeaps.slice(0, heapCount || rawHeaps.length).map((h) => ({
        size: isObject(h) ? num(h.size) : 0, flags: isObject(h) ? str(h.flags) : "",
      })),
      types: rawTypes.slice(0, typeCount || rawTypes.length).map((t) => ({
        heapIndex: isObject(t) ? num(t.heapIndex) : 0, propertyFlags: isObject(t) ? str(t.propertyFlags) : "",
      })),
    };
  }
  return null;
}

/** The driver's budget per heap, as the layer attached it (`memoryBudget` on the physical device). */
function budgetOf(db: MemoryDatabase): { budget: number[]; usage: number[] } | null {
  for (const o of db.allObjects.values()) {
    if (!isMemoryDevice(o.type)) continue;
    const b = o.updates.memoryBudget;
    if (!isObject(b)) continue;
    const budget = Array.isArray(b.heapBudget) ? b.heapBudget.map((v) => num(v)) : [];
    const usage = Array.isArray(b.heapUsage) ? b.heapUsage.map((v) => num(v)) : [];
    if (budget.length || usage.length) return { budget, usage };
  }
  return null;
}

/**
 * The size and memory type of one allocation, or null when the object is not an allocation we can
 * read.
 *
 * Vulkan has one kind: a VkDeviceMemory, whose creation arguments carry both. D3D12 has two — an
 * ID3D12Heap the application allocated itself, and the implicit heap behind a committed resource,
 * whose size only the runtime knows and which the library therefore attaches as an `allocation`
 * update (src/d3d12/src/cpu_timeline.h). A placed resource is deliberately not one of them: it
 * lives inside a heap already counted here.
 */
function allocationOf(o: VulkanObject): { bytes: number; typeIndex: number } | null {
  if (o.isDeleted) return null;
  if (o.type === "VkDeviceMemory") {
    const info = isObject(o.args) ? o.args.pAllocateInfo : undefined;
    if (!isObject(info)) return null;
    return { bytes: num(info.allocationSize), typeIndex: num(info.memoryTypeIndex) };
  }
  if (o.type === "ID3D12Heap") {
    const desc = isObject(o.args) ? o.args.pDesc : undefined;
    if (!isObject(desc)) return null;
    const props = isObject(desc.Properties) ? desc.Properties : undefined;
    return { bytes: num(desc.SizeInBytes), typeIndex: d3d12HeapTypeIndex(props ? str(props.Type) : "") };
  }
  if (o.type === "ID3D12Resource") {
    const a = o.updates.allocation;
    if (!isObject(a)) return null;   // a placed or reserved resource, or a capture before this existed
    return { bytes: num(a.sizeBytes), typeIndex: num(a.heapTypeIndex) };
  }
  return null;
}

/** The index of a D3D12 heap type in the order the library reports them (cpu_timeline.cpp). */
function d3d12HeapTypeIndex(type: string): number {
  if (type.includes("UPLOAD")) return 1;
  if (type.includes("READBACK")) return 2;
  return 0;   // DEFAULT, and a CUSTOM heap whose properties the library did not map
}

/**
 * Per-heap use from the object graph. Null when the capture or session has no physical device
 * memory properties, which is every capture taken before the layer reported them.
 */
export function memoryHeaps(db: MemoryDatabase): MemoryHeaps | null {
  const props = memoryPropertiesOf(db);
  if (!props || !props.heaps.length) return null;
  const budget = budgetOf(db);

  const heaps: HeapUse[] = props.heaps.map((h, index) => ({
    index, sizeBytes: h.size, flags: h.flags, deviceLocal: h.flags.includes("DEVICE_LOCAL"),
    allocations: 0, bytes: 0, largestBytes: 0, share: null, types: [],
    ...(budget && budget.budget[index] !== undefined ? { budgetBytes: budget.budget[index] } : {}),
    ...(budget && budget.usage[index] !== undefined ? { usageBytes: budget.usage[index] } : {}),
  }));
  const typeUse = new Map<number, MemoryTypeUse>();

  let totalBytes = 0;
  let allocations = 0;
  for (const o of db.allObjects.values()) {
    const a = allocationOf(o);
    if (!a) continue;
    const type = props.types[a.typeIndex];
    if (!type) continue;
    const heap = heaps[type.heapIndex];
    if (!heap) continue;
    heap.allocations++;
    heap.bytes += a.bytes;
    heap.largestBytes = Math.max(heap.largestBytes, a.bytes);
    totalBytes += a.bytes;
    allocations++;
    let t = typeUse.get(a.typeIndex);
    if (!t) typeUse.set(a.typeIndex, (t = { index: a.typeIndex, propertyFlags: type.propertyFlags, allocations: 0, bytes: 0 }));
    t.allocations++;
    t.bytes += a.bytes;
  }
  for (const t of typeUse.values()) {
    const heap = heaps[props.types[t.index]?.heapIndex ?? -1];
    if (heap) heap.types.push(t);
  }
  for (const h of heaps) {
    h.types.sort((a, b) => b.bytes - a.bytes);
    h.share = h.sizeBytes > 0 ? h.bytes / h.sizeBytes : null;
  }
  return { heaps, totalBytes, allocations, hasBudget: !!budget };
}

/** Heaps the application is filling, or that the driver says are nearly spent. Worst first. */
export function heapPressure(m: MemoryHeaps): HeapUse[] {
  return m.heaps
    .filter((h) => {
      if (!h.allocations) return false;
      if (h.share !== null && h.share >= HEAP_PRESSURE_SHARE) return true;
      // The driver's own view counts every process, so it can be tight when ours is not.
      return h.budgetBytes !== undefined && h.usageBytes !== undefined && h.budgetBytes > 0
        && h.usageBytes / h.budgetBytes >= HEAP_PRESSURE_SHARE;
    })
    .sort((a, b) => (b.share ?? 0) - (a.share ?? 0));
}

/** Only the heaps this application actually drew from, largest first: the rest are noise. */
export function usedHeaps(m: MemoryHeaps): HeapUse[] {
  return m.heaps.filter((h) => h.allocations > 0).sort((a, b) => b.bytes - a.bytes);
}
