// Argument buffers: Metal's bindless path. The buffer's bytes hold, for each member the shader
// declared, a buffer's GPU address, a texture's or sampler's resource id, or an inline value.
// The pipeline's reflection names the members and their offsets (metal/src/reflection.mm marks
// the handles with `metal: "pointer" | "texture" | "sampler" | ...`), and every buffer, texture
// and sampler the library tracked reports its address or id in its descriptor, so the two can
// be matched: this is Xcode's argument buffer view.
//
// Only Apple GPUs encode handles this way (Tier 2 argument buffers); a Tier 1 encoding is
// driver-defined and shows as unresolved values.
//
// No UI here: argument_buffer_view.ts renders the entries, the MCP server's get_command lists them.
import { num, type VulkanObject } from "../vulkan/vulkan_object.js";
import type { ReflType } from "../vulkan/spirv_reflect.js";
import type { ObjectDatabase } from "../vulkan/object_database.js";

export interface ArgumentEntry {
  /** "material.albedo", "lights[3].shadow". */
  path: string;
  offset: number;
  /** "pointer", "texture", "sampler", or another handle kind's name. */
  kind: string;
  /** The declared type's name ("device float4 *", "texture"). */
  typeName: string;
  /** The eight bytes, as the hex the descriptors use; null when past the captured range. */
  value: string | null;
  object: VulkanObject | null;
  /** For a pointer into a buffer: how far into it. */
  objectOffset: number | null;
}

interface Handle { metal: string; name: string; size?: number }

function handleOf(type: ReflType): Handle | null {
  if (type.kind !== "opaque") return null;
  const raw = type as unknown as Record<string, unknown>;
  return typeof raw.metal === "string" ? { metal: raw.metal, name: type.name } : null;
}

/** Whether a buffer's declared type holds any resource handles: an argument buffer. */
export function isArgumentBufferType(type: ReflType, depth = 0): boolean {
  if (depth > 8) return false;
  if (handleOf(type)) return true;
  if (type.kind === "struct") return type.members.some((m) => isArgumentBufferType(m.type, depth + 1));
  if (type.kind === "array") return isArgumentBufferType(type.element, depth + 1);
  return false;
}

/** The tracked objects by what an argument buffer holds for them. Built per call; a few thousand objects at most. */
class HandleIndex {
  private _buffers: { start: bigint; end: bigint; object: VulkanObject }[] = [];
  private _byResourceId = new Map<string, VulkanObject>();

  constructor(db: ObjectDatabase) {
    for (const o of db.allObjects.values()) {
      if (!o.type.startsWith("MTL") || !o.args) continue;
      const address = o.args.gpuAddress;
      if (typeof address === "string" && address.startsWith("0x")) {
        const start = BigInt(address);
        const length = BigInt(Math.max(0, num(o.args.length)));
        this._buffers.push({ start, end: start + length, object: o });
      }
      const id = o.args.gpuResourceID;
      if (typeof id === "string" && id.startsWith("0x")) this._byResourceId.set(id.toLowerCase(), o);
    }
  }

  buffer(address: bigint): { object: VulkanObject; offset: number } | null {
    for (const b of this._buffers) {
      if (address >= b.start && address < b.end) return { object: b.object, offset: Number(address - b.start) };
    }
    return null;
  }

  resource(id: bigint): VulkanObject | null {
    return this._byResourceId.get(`0x${id.toString(16)}`) ?? null;
  }
}

const MAX_ENTRIES = 512;

/** Every handle member of the buffer, in declaration order, with what it resolves to. */
export function argumentBufferEntries(type: ReflType, data: Uint8Array, db: ObjectDatabase): ArgumentEntry[] {
  const index = new HandleIndex(db);
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const out: ArgumentEntry[] = [];
  const walk = (t: ReflType, offset: number, path: string, depth: number): void => {
    if (out.length >= MAX_ENTRIES || depth > 8) return;
    const handle = handleOf(t);
    if (handle) {
      let value: bigint | null = null;
      if (offset + 8 <= data.byteLength) value = view.getBigUint64(offset, true);
      let object: VulkanObject | null = null;
      let objectOffset: number | null = null;
      if (value !== null && value !== 0n) {
        if (handle.metal === "pointer") {
          const hit = index.buffer(value);
          if (hit) { object = hit.object; objectOffset = hit.offset; }
        } else {
          object = index.resource(value);
        }
      }
      out.push({ path, offset, kind: handle.metal, typeName: handle.name, value: value === null ? null : `0x${value.toString(16)}`, object, objectOffset });
      return;
    }
    if (t.kind === "struct") {
      for (const m of t.members) walk(m.type, offset + m.offset, path ? `${path}.${m.name}` : m.name, depth + 1);
    } else if (t.kind === "array") {
      const stride = t.stride || (t.element.kind === "opaque" ? 8 : t.element.size);
      if (stride <= 0) return;
      const count = t.count > 0 ? t.count : Math.floor((data.byteLength - offset) / stride);
      for (let i = 0; i < count && out.length < MAX_ENTRIES; i++) walk(t.element, offset + i * stride, `${path}[${i}]`, depth + 1);
    }
  };
  walk(type, 0, "", 0);
  return out;
}
