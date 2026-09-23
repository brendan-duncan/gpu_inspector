// A memory capture: every allocation and every free while it ran, with the frame each happened
// in and the object it was (src/vulkan/src/cpu_timeline.h, src/d3d12/src/cpu_timeline.h).
//
// memory_timeline.ts says which way memory is going. It cannot say *what* is going: a total that
// climbs a megabyte a second is one allocation a frame that is never freed, or a thousand a frame
// that mostly are, and nothing about the fix for one applies to the other. The series is a sum, and
// a sum has thrown away exactly the part that tells them apart.
//
// So this keeps the events, and asks the three questions they can answer and a total cannot: what
// was made while this ran and is still held (the leak, by name), what was made and thrown away
// again within a frame or two (the churn, which costs time rather than memory), and which frames
// did the allocating (the spikes, which are where a hitch comes from).
import type { MemoryEventsMessage } from "../shared/protocol.js";

export interface MemoryEvent {
  frame: number;
  /** Milliseconds since the capture began. */
  ms: number;
  /** The object's id in the inspector, or 0 when the capture library could not name it. */
  id: number;
  bytes: number;
  /** Index into the device's heaps, the order MemorySample reports them in. */
  heap: number;
  free?: boolean;
  /** D3D12 residency, beside the allocations (protocol.ts, MemoryEventsMessage). */
  kind?: "evict" | "resident" | "budget";
  count?: number;
}

/** A residency event or a budget change, for the graph's marks and the report's rows. */
export interface MemoryMark {
  frame: number;
  kind: "evict" | "resident" | "budget";
  /** What the call named; for a budget change, the new budget. */
  bytes: number;
  count: number;
  heap: number;
}

export interface Residency {
  evictions: number;
  evictedBytes: number;
  pageIns: number;
  residentBytes: number;
  budgetChanges: number;
}

export interface MemoryCapture {
  /** What each heap held when the capture began, so totals are absolute rather than relative. */
  baseline: { allocated: number; allocations: number }[];
  events: MemoryEvent[];
  /** Events past the capture library's cap, counted rather than recorded. */
  dropped: number;
}

export function emptyMemoryCapture(): MemoryCapture {
  return { baseline: [], events: [], dropped: 0 };
}

/** Appends a message's events; the first message of a capture carries the baseline. */
export function appendMemoryEvents(capture: MemoryCapture, msg: MemoryEventsMessage): void {
  if (msg.baseline) capture.baseline = msg.baseline.map((h) => ({ allocated: h.allocated ?? 0, allocations: h.allocations ?? 0 }));
  if (typeof msg.dropped === "number") capture.dropped = msg.dropped;
  for (const e of msg.events ?? []) capture.events.push(e);
  // The capture library stops at about a million; this is the same bound, against a library that
  // does not.
  if (capture.events.length > MAX_MEMORY_EVENTS) capture.events.splice(0, capture.events.length - MAX_MEMORY_EVENTS);
}

export const MAX_MEMORY_EVENTS = 1 << 20;

/**
 * An allocation freed within this many frames of being made is transient: it never outlived the
 * frames in flight, so it was scratch space, and scratch space asked of the driver every frame is
 * the thing a ring buffer or a pool exists to replace.
 */
export const TRANSIENT_FRAMES = 3;

/**
 * Transient allocations at this rate or above are worth the verdict's sentence. Half rather than
 * one: an application making exactly one a frame measures just under it, because the ones made in
 * the capture's last frames have not been freed yet.
 */
export const CHURN_PER_FRAME = 0.5;

/** An allocation made during the capture and still held when it ended. */
export interface Survivor {
  id: number;
  bytes: number;
  heap: number;
  frame: number;
  ms: number;
}

/** A frame that allocated, for the list of the ones that allocated most. */
export interface AllocatingFrame {
  frame: number;
  allocations: number;
  bytes: number;
}

export interface HeapChange {
  heap: number;
  allocatedBytes: number;
  freedBytes: number;
  /** allocatedBytes - freedBytes: what the heap gained over the capture. */
  netBytes: number;
}

export interface MemoryCaptureSummary {
  /** Frames from the first event to the last, inclusive. */
  frames: number;
  firstFrame: number;
  lastFrame: number;
  durationMs: number;
  allocations: number;
  allocatedBytes: number;
  frees: number;
  freedBytes: number;
  netBytes: number;
  /** Held at the start (the baseline), at the end, and at the most. */
  startBytes: number;
  endBytes: number;
  peakBytes: number;
  heaps: HeapChange[];
  /** Largest first. */
  survivors: Survivor[];
  survivorBytes: number;
  /** Allocations the capture library could not name, which cannot be matched to their frees. */
  unnamed: number;
  /** Made and freed within TRANSIENT_FRAMES. */
  transient: { count: number; bytes: number; perFrame: number };
  /** Frees of allocations made before the capture began. */
  freedOlder: { count: number; bytes: number };
  /** Most bytes first. */
  busiest: AllocatingFrame[];
  /** Bytes held after each frame that had events, for the graph. */
  series: { frame: number; bytes: number }[];
  /** Evictions, page-ins and budget changes, in frame order (D3D12). */
  marks: MemoryMark[];
  residency: Residency;
  verdict: string;
}

function bytesText(n: number): string {
  const abs = Math.abs(n);
  if (abs >= 1 << 30) return `${(n / (1 << 30)).toFixed(2)} GB`;
  if (abs >= 1 << 20) return `${(n / (1 << 20)).toFixed(1)} MB`;
  if (abs >= 1 << 10) return `${(n / (1 << 10)).toFixed(0)} KB`;
  return `${n} B`;
}

/** What the capture says, or null when it has recorded nothing yet. */
export function summarizeMemoryCapture(capture: MemoryCapture): MemoryCaptureSummary | null {
  const events = capture.events;
  if (!events.length) return null;

  const startBytes = capture.baseline.reduce((sum, h) => sum + h.allocated, 0);
  const heaps = new Map<number, HeapChange>();
  const heapOf = (index: number): HeapChange => {
    let h = heaps.get(index);
    if (!h) {
      h = { heap: index, allocatedBytes: 0, freedBytes: 0, netBytes: 0 };
      heaps.set(index, h);
    }
    return h;
  };

  // Allocations made during the capture and not yet freed, by id. Ids are the inspector's own and
  // are never handed out twice, which is what makes this a match rather than a guess: the handle
  // the driver gave is reused within a frame or two of a free.
  const live = new Map<number, MemoryEvent>();
  const perFrame = new Map<number, AllocatingFrame>();
  const series: { frame: number; bytes: number }[] = [];
  let held = startBytes;
  let peak = startBytes;
  let allocations = 0, allocatedBytes = 0, frees = 0, freedBytes = 0, unnamed = 0;
  let transientCount = 0, transientBytes = 0, olderCount = 0, olderBytes = 0;
  const marks: MemoryMark[] = [];
  const residency: Residency = { evictions: 0, evictedBytes: 0, pageIns: 0, residentBytes: 0, budgetChanges: 0 };

  for (const e of events) {
    // Residency is beside the allocations, not among them: what is evicted is still held.
    if (e.kind) {
      marks.push({ frame: e.frame, kind: e.kind, bytes: e.bytes, count: e.count ?? 0, heap: e.heap });
      if (e.kind === "evict") { residency.evictions++; residency.evictedBytes += e.bytes; }
      else if (e.kind === "resident") { residency.pageIns++; residency.residentBytes += e.bytes; }
      else residency.budgetChanges++;
      continue;
    }
    const h = heapOf(e.heap);
    if (e.free) {
      frees++;
      freedBytes += e.bytes;
      h.freedBytes += e.bytes;
      held -= e.bytes;
      const made = e.id ? live.get(e.id) : undefined;
      if (made) {
        live.delete(e.id);
        if (e.frame - made.frame <= TRANSIENT_FRAMES) {
          transientCount++;
          transientBytes += made.bytes;
        }
      } else {
        olderCount++;
        olderBytes += e.bytes;
      }
    } else {
      allocations++;
      allocatedBytes += e.bytes;
      h.allocatedBytes += e.bytes;
      held += e.bytes;
      if (e.id) live.set(e.id, e); else unnamed++;
      let f = perFrame.get(e.frame);
      if (!f) {
        f = { frame: e.frame, allocations: 0, bytes: 0 };
        perFrame.set(e.frame, f);
      }
      f.allocations++;
      f.bytes += e.bytes;
    }
    if (held > peak) peak = held;
    const last = series[series.length - 1];
    if (last && last.frame === e.frame) last.bytes = held; else series.push({ frame: e.frame, bytes: held });
  }
  for (const h of heaps.values()) h.netBytes = h.allocatedBytes - h.freedBytes;

  const firstFrame = events[0].frame;
  const lastFrame = events[events.length - 1].frame;
  const frames = Math.max(1, lastFrame - firstFrame + 1);
  const survivors: Survivor[] = [...live.values()]
    .map((e) => ({ id: e.id, bytes: e.bytes, heap: e.heap, frame: e.frame, ms: e.ms }))
    .sort((a, b) => b.bytes - a.bytes || a.frame - b.frame);
  // An allocation made in the last few frames has not had the chance to be transient yet: calling
  // it held would make every capture of a churning application end in a list of false leaks.
  const settled = survivors.filter((s) => lastFrame - s.frame > TRANSIENT_FRAMES);
  const survivorBytes = settled.reduce((sum, s) => sum + s.bytes, 0);
  const busiest = [...perFrame.values()].sort((a, b) => b.bytes - a.bytes || b.allocations - a.allocations).slice(0, 10);
  const netBytes = allocatedBytes - freedBytes;
  const transient = { count: transientCount, bytes: transientBytes, perFrame: transientCount / frames };

  // The verdict names the larger problem first. Growth is memory and churn is time; an application
  // can have both, and the sentence says so rather than picking.
  const parts: string[] = [];
  if (settled.length && survivorBytes > 0) {
    parts.push(`${bytesText(survivorBytes)} made during these ${frames} frames was still held when they ended, in ${settled.length} allocation${settled.length === 1 ? "" : "s"}: if that is not something the application meant to keep, it is the leak, and the largest are named below.`);
  }
  if (transient.perFrame >= CHURN_PER_FRAME) {
    parts.push(`${transient.perFrame.toFixed(1)} allocation${transient.perFrame >= 1.05 ? "s" : ""} a frame (${bytesText(transientBytes / frames)}) ${transient.perFrame >= 1.05 ? "were" : "was"} made and freed again within ${TRANSIENT_FRAMES} frames: scratch space asked of the driver every frame, which a ring buffer or a pool would take out of the frame entirely.`);
  }
  if (!parts.length) {
    parts.push(netBytes === 0
      ? `${allocations} allocation${allocations === 1 ? "" : "s"} over ${frames} frames, and everything made was freed again: nothing is growing and nothing is churning.`
      : `${allocations} allocation${allocations === 1 ? "" : "s"} over ${frames} frames, ${netBytes > 0 ? "up" : "down"} ${bytesText(Math.abs(netBytes))} overall, with nothing made here left held for long enough to call a leak.`);
  }
  if (residency.evictions || residency.pageIns) {
    parts.push(`The application evicted ${bytesText(residency.evictedBytes)} in ${residency.evictions} call${residency.evictions === 1 ? "" : "s"} and paged ${bytesText(residency.residentBytes)} back in ${residency.pageIns} time${residency.pageIns === 1 ? "" : "s"}: memory pressure it answered, and paging back in is a stall where it happens (the marks on the graph).`);
  }
  if (residency.budgetChanges) {
    parts.push(`The driver changed this process's memory budget ${residency.budgetChanges} time${residency.budgetChanges === 1 ? "" : "s"} while this ran: something else on the GPU took or gave back memory.`);
  }

  return {
    frames, firstFrame, lastFrame,
    durationMs: events[events.length - 1].ms - events[0].ms,
    allocations, allocatedBytes, frees, freedBytes, netBytes,
    startBytes, endBytes: held, peakBytes: peak,
    heaps: [...heaps.values()].sort((a, b) => a.heap - b.heap),
    survivors: settled, survivorBytes, unnamed, transient,
    freedOlder: { count: olderCount, bytes: olderBytes },
    busiest, series, marks, residency,
    verdict: parts.join(" "),
  };
}
