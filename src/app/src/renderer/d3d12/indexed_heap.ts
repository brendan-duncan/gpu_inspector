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
import { decodeBase64 } from "../utils/base64.js";
import { isObject, num, refId, str } from "../vulkan/vulkan_object.js";
import type { ConstantReader } from "./heap_indices.js";
import type { ArgObject, ArgValue, CaptureCommand, CaptureDescriptor, CaptureDescriptorSet } from "../../shared/protocol.js";

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

/** A captured buffer range: its bytes, and the buffer offset they start at. */
export type CapturedRange = (dataId: number) => { bytes: Uint8Array; offset: number } | null;

/**
 * The constant buffers a draw's shaders see, for working out an index they read from one
 * (heap_indices.ts): root constants from the Set*Root32BitConstant(s) calls before the draw, at the
 * register their root parameter declares, and constant buffer views -- a table's or a root CBV --
 * from what the capture read back of them.
 */
export function drawConstants(commands: readonly CaptureCommand[], cmd: CaptureCommand, objectOf: ObjectArgs, compute: boolean,
                              sets: readonly CaptureDescriptorSet[], bytesOf: CapturedRange): ConstantReader {
  // Root constants: the root signature's 32-bit constant parameters, filled by the calls since it was set.
  const roots = new Map<string, Uint32Array>();   // "register:space" -> values
  const rootParams = new Map<number, string>();    // parameter index -> "register:space"
  const writes: CaptureCommand[] = [];
  const prefix = compute ? "SetComputeRoot32BitConstant" : "SetGraphicsRoot32BitConstant";
  const list = cmd.object?.__id;
  let root: number | null = null;
  for (let i = cmd.index - 1; i >= 0; i--) {
    const c = commands[i];
    if (!c || c.object?.__id !== list) break;
    if (c.secondary && c.secondary !== cmd.secondary) continue;
    if (c.method === "Reset" && !c.secondary) break;
    // Setting the root signature drops every root argument set before it.
    if (c.method === (compute ? COMPUTE_ROOT : GRAPHICS_ROOT)) {
      root = refId(c.args?.pRootSignature);
      break;
    }
    if (c.method === prefix || c.method === `${prefix}s`) writes.push(c);
  }
  const params = rootParameters(root === null ? null : objectOf(root)?.args ?? null);
  params.forEach((p, index) => {
    if (!isObject(p) || !str(p.ParameterType).endsWith("_32BIT_CONSTANTS") || !isObject(p.Constants)) return;
    const key = `${num(p.Constants.ShaderRegister)}:${num(p.Constants.RegisterSpace)}`;
    rootParams.set(index, key);
    roots.set(key, new Uint32Array(num(p.Constants.Num32BitValues)));
  });
  const known = new Map<string, Set<number>>();   // which values were ever written
  for (const c of writes.reverse()) {
    const a = c.args;
    const key = a ? rootParams.get(num(a.RootParameterIndex)) : undefined;
    const values = key ? roots.get(key) : undefined;
    if (!a || !key || !values) continue;
    const dest = num(a.DestOffsetIn32BitValues);
    let words: number[] = [];
    if (c.method.endsWith("s")) {
      const src = isObject(a.pSrcData) ? a.pSrcData : isObject(a.pValues) ? a.pValues : null;
      const bytes = src && typeof src.base64 === "string" ? decodeBase64(src.base64) : null;
      if (bytes) words = Array.from({ length: bytes.length >> 2 }, (_, k) => new DataView(bytes.buffer, bytes.byteOffset).getUint32(k * 4, true));
    } else {
      words = [num(a.SrcData) >>> 0];
    }
    let seen = known.get(key);
    if (!seen) known.set(key, (seen = new Set()));
    words.forEach((w, k) => {
      if (dest + k < values.length) {
        values[dest + k] = w;
        seen!.add(dest + k);
      }
    });
  }
  // Constant buffer views, by register: their captured bytes from the view's start.
  const views = new Map<string, { data: number; offset: number }>();
  for (const set of sets) {
    for (const b of set.bindings) {
      if (!b.type.endsWith("_CBV") || b.register === undefined) continue;
      b.descriptors.forEach((d, k) => {
        if (d && d.data !== undefined) views.set(`${num(b.register) + k}:${num(b.space)}`, { data: num(d.data), offset: num(d.offset) });
      });
    }
  }
  return (register, space, byteOffset) => {
    const key = `${register}:${space}`;
    const values = roots.get(key);
    if (values) {
      const word = byteOffset >> 2;
      return (byteOffset & 3) === 0 && known.get(key)?.has(word) ? values[word] : null;
    }
    const view = views.get(key);
    const captured = view ? bytesOf(view.data) : null;
    if (!view || !captured) return null;
    const at = view.offset - captured.offset + byteOffset;
    if (at < 0 || at + 4 > captured.bytes.length) return null;
    return new DataView(captured.bytes.buffer, captured.bytes.byteOffset).getUint32(at, true);
  };
}

/** A root signature's parameters, from its description in any version. */
function rootParameters(args: ArgObject | null): ArgValue[] {
  const desc = isObject(args?.pDesc) ? args.pDesc : null;
  for (const version of ["Desc_1_2", "Desc_1_1", "Desc_1_0"]) {
    const d = desc?.[version];
    if (isObject(d)) return Array.isArray(d.pParameters) ? d.pParameters : [];
  }
  return [];
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
