// The descriptor heaps a D3D12 draw's shaders index directly (shader model 6.6,
// `ResourceDescriptorHeap[i]` and `SamplerDescriptorHeap[i]`), with what their slots held when the
// draw ran. No root table says which slots such a shader reads, so what can be shown is every slot
// the application wrote.
//
// The capture library puts a heap's contents on the ExecuteCommandLists that submits a list
// indexing it (`heapDescriptors`, src/d3d12/README.md "Directly indexed heaps"): the slots written
// since it last sent them, so the heap at a submission is every earlier submission's entries
// applied in order, the last write of a slot winning. The draw's submission is the one its list's
// commands follow in the stream.
import { isObject, num, refId } from "../vulkan/vulkan_object.js";
import type { ArgObject, ArgValue, CaptureCommand, CaptureDescriptor } from "../../shared/protocol.js";

/** D3D12_DESCRIPTOR_RANGE_TYPE, as the capture library writes a slot's `type`. */
const RANGE_TYPES = ["D3D12_DESCRIPTOR_RANGE_TYPE_SRV", "D3D12_DESCRIPTOR_RANGE_TYPE_UAV", "D3D12_DESCRIPTOR_RANGE_TYPE_CBV", "D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER"];

export interface IndexedSlot {
  slot: number;
  /** "D3D12_DESCRIPTOR_RANGE_TYPE_SRV" and the rest: what the descriptor was written as. */
  type: string;
  descriptor: CaptureDescriptor;
  /** The command index of the submission that sent this contents of the slot. */
  sentBy: number;
}

export interface IndexedHeap {
  heap: number;
  samplers: boolean;
  /** The written slots, in slot order; empty when the capture holds no contents for the heap. */
  slots: IndexedSlot[];
  /** The draw's submission, which the contents are as of (-1 when none was found). */
  submission: number;
  /** Whether the capture sent the heap's contents at all (a capture from before it did has none). */
  captured: boolean;
}

/** What an object lookup needs to give: the object's creation arguments. */
export type ObjectArgs = (id: number) => { args: ArgObject | null } | null;

const GRAPHICS_ROOT = "SetGraphicsRootSignature";
const COMPUTE_ROOT = "SetComputeRootSignature";

/**
 * The heaps `cmd`'s root signature lets its shaders index directly, with their contents at its
 * submission. Empty when the root signature has neither flag, or no heap of the kind is bound.
 * `compute` is the draw's bind point (a dispatch or a trace reads the compute root signature).
 */
export function indexedHeapsAt(commands: readonly CaptureCommand[], cmd: CaptureCommand, objectOf: ObjectArgs, compute: boolean): IndexedHeap[] {
  // The root signature and the heaps in effect: walked back through the command's list, a bundle's
  // commands first and then the list that executes it (a bundle inherits both).
  let root: number | null = null;
  let heaps: number[] | null = null;
  const list = cmd.object?.__id;
  for (let i = cmd.index - 1; i >= 0 && (root === null || heaps === null); i--) {
    const c = commands[i];
    if (!c || c.object?.__id !== list) break;
    if (c.secondary && c.secondary !== cmd.secondary) continue;
    if (c.method === "Reset" && !c.secondary) break;
    if (root === null && c.method === (compute ? COMPUTE_ROOT : GRAPHICS_ROOT)) root = refId(c.args?.pRootSignature);
    if (heaps === null && c.method === "SetDescriptorHeaps") {
      const refs = c.args?.ppDescriptorHeaps;
      heaps = (Array.isArray(refs) ? refs : []).map((r) => refId(r)).filter((id): id is number => id !== null);
    }
  }
  if (root === null || !heaps?.length) return [];
  const flags = rootSignatureFlags(objectOf(root)?.args ?? null);
  const resources = flags.includes("CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED");
  const samplers = flags.includes("SAMPLER_HEAP_DIRECTLY_INDEXED");
  if (!resources && !samplers) return [];

  const submission = submissionOf(commands, cmd);
  const contents = heapContents(commands, submission);
  const out: IndexedHeap[] = [];
  for (const heap of heaps) {
    const isSamplers = heapIsSamplers(objectOf(heap)?.args ?? null);
    if (isSamplers ? !samplers : !resources) continue;
    const slots = contents.get(heap);
    out.push({
      heap,
      samplers: isSamplers,
      slots: slots ? [...slots.values()].sort((a, b) => a.slot - b.slot) : [],
      submission,
      captured: !!slots,
    });
  }
  return out;
}

/** The ExecuteCommandLists a command's list was submitted by: the last one before it in the stream. */
export function submissionOf(commands: readonly CaptureCommand[], cmd: CaptureCommand): number {
  for (let i = cmd.index - 1; i >= 0; i--) {
    const c = commands[i];
    if (c && !c.secondary && c.method === "ExecuteCommandLists") return i;
  }
  return -1;
}

// The heaps as of each submission asked for, per command array: a frame has few submissions and
// the draws of one all ask for the same.
const cache = new WeakMap<readonly CaptureCommand[], Map<number, Map<number, Map<number, IndexedSlot>>>>();

/** Every heap's slots as of the submission at `submission` (its own entries included). */
export function heapContents(commands: readonly CaptureCommand[], submission: number): Map<number, Map<number, IndexedSlot>> {
  let bySubmission = cache.get(commands);
  if (!bySubmission) cache.set(commands, (bySubmission = new Map()));
  const known = bySubmission.get(submission);
  if (known) return known;
  const heaps = new Map<number, Map<number, IndexedSlot>>();
  for (let i = 0; i <= submission && i < commands.length; i++) {
    const c = commands[i];
    const sent = (c as { heapDescriptors?: ArgValue } | undefined)?.heapDescriptors;
    if (!c || c.method !== "ExecuteCommandLists" || !Array.isArray(sent)) continue;
    for (const entry of sent) {
      if (!isObject(entry)) continue;
      const heap = refId(entry.heap);
      if (heap === null) continue;
      let slots = heaps.get(heap);
      if (!slots) heaps.set(heap, (slots = new Map()));
      for (const s of Array.isArray(entry.slots) ? entry.slots : []) {
        if (!isObject(s) || !isObject(s.descriptor)) continue;
        const slot = num(s.slot);
        slots.set(slot, { slot, type: RANGE_TYPES[num(s.type)] ?? `type ${num(s.type)}`, descriptor: s.descriptor as unknown as CaptureDescriptor, sentBy: i });
      }
    }
  }
  bySubmission.set(submission, heaps);
  return heaps;
}

/** A root signature's flags, from its description in any version (`pDesc.Desc_1_0` .. `Desc_1_2`), as text. */
function rootSignatureFlags(args: ArgObject | null): string {
  const desc = isObject(args?.pDesc) ? args.pDesc : null;
  for (const version of ["Desc_1_2", "Desc_1_1", "Desc_1_0"]) {
    const d = desc?.[version];
    if (isObject(d)) return JSON.stringify(d.Flags ?? "");
  }
  return "";
}

/** Whether a heap was created as a sampler heap (`pDesc.Type` D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER). */
function heapIsSamplers(args: ArgObject | null): boolean {
  const desc = isObject(args?.pDesc) ? args.pDesc : null;
  return JSON.stringify(desc?.Type ?? "").includes("SAMPLER");
}
