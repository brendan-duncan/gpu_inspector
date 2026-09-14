// Decoding of vertex attribute data by VkFormat name. The name says everything the decoder
// needs ("VK_FORMAT_R16G16B16A16_SNORM", "VK_FORMAT_A2B10G10R10_UNORM_PACK32"): the channel
// order and widths, the numeric interpretation, and whether the channels are packed into one
// integer (PACK formats list channels from the most significant bits down).
import { float10ToFloat32, float11ToFloat32, float16ToFloat32 } from "../utils/float.js";

export interface VertexFormat {
  /** Bytes per element. */
  size: number;
  /** Channel letters in output order (R, G, B, A). */
  channels: string[];
  /** True for UINT/SINT/USCALED/SSCALED (values shown as integers). */
  integer: boolean;
  /** Reads one element; values are in R, G, B, A order. */
  read: (view: DataView, offset: number) => number[];
}

const cache = new Map<string, VertexFormat | null>();

/** Storage size of a VkFormat: bytes per block and the block's texel dimensions (1x1 unless compressed). */
export interface FormatBlock { bytes: number; width: number; height: number }

const blockCache = new Map<string, FormatBlock | null>();

const FIXED_BLOCKS: Record<string, number> = {
  VK_FORMAT_D16_UNORM: 2, VK_FORMAT_X8_D24_UNORM_PACK32: 4, VK_FORMAT_D32_SFLOAT: 4, VK_FORMAT_S8_UINT: 1,
  VK_FORMAT_D16_UNORM_S8_UINT: 4, VK_FORMAT_D24_UNORM_S8_UINT: 4, VK_FORMAT_D32_SFLOAT_S8_UINT: 8,
  VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: 4, VK_FORMAT_B10G11R11_UFLOAT_PACK32: 4,
};

/** Estimates a format's storage; null for formats it does not know (multi-planar video formats). */
export function formatBlock(name: string): FormatBlock | null {
  const hit = blockCache.get(name);
  if (hit !== undefined) return hit;
  const b = parseBlock(name);
  blockCache.set(name, b);
  return b;
}

function parseBlock(name: string): FormatBlock | null {
  const fixed = FIXED_BLOCKS[name];
  if (fixed) return { bytes: fixed, width: 1, height: 1 };
  let m = /^VK_FORMAT_BC(\d)/.exec(name);
  if (m) return { bytes: m[1] === "1" || m[1] === "4" ? 8 : 16, width: 4, height: 4 };
  if (/^VK_FORMAT_ETC2_R8G8B8A8|^VK_FORMAT_EAC_R11G11/.test(name)) return { bytes: 16, width: 4, height: 4 };
  if (/^VK_FORMAT_ETC2_|^VK_FORMAT_EAC_/.test(name)) return { bytes: 8, width: 4, height: 4 };
  m = /^VK_FORMAT_ASTC_(\d+)x(\d+)/.exec(name);
  if (m) return { bytes: 16, width: Number(m[1]), height: Number(m[2]) };
  m = /^VK_FORMAT_PVRTC\d_(\d)BPP/.exec(name);
  if (m) return { bytes: 8, width: m[1] === "2" ? 8 : 4, height: 4 };
  m = /_PACK(8|16|32)$/.exec(name);
  if (m) return { bytes: Number(m[1]) / 8, width: 1, height: 1 };
  m = /^VK_FORMAT_((?:[RGBAEXDS]\d+)+)_[A-Z0-9_]+$/.exec(name);
  if (m) {
    let bits = 0;
    for (const c of m[1].matchAll(/[RGBAEXDS](\d+)/g)) bits += Number(c[1]);
    return bits ? { bytes: bits / 8, width: 1, height: 1 } : null;
  }
  return null;
}

/** Estimated memory of an image with the given format, size, mip and layer counts and sample count. */
export function estimateImageBytes(format: string, width: number, height: number, depth: number, mips: number, layers: number, samples: number): number {
  const block = formatBlock(format);
  if (!block) return 0;
  let total = 0;
  for (let m = 0; m < Math.max(1, mips); m++) {
    const w = Math.max(1, width >> m);
    const h = Math.max(1, height >> m);
    const d = Math.max(1, depth >> m);
    total += Math.ceil(w / block.width) * Math.ceil(h / block.height) * d * block.bytes;
  }
  return total * Math.max(1, layers) * Math.max(1, samples);
}

interface Channel { name: string; bits: number }

function convert(raw: number, bits: number, numeric: string): number {
  const maxU = Math.pow(2, bits) - 1;
  switch (numeric) {
    case "UNORM": case "SRGB": return raw / maxU;
    case "SNORM": return Math.max(-1, toSigned(raw, bits) / (Math.pow(2, bits - 1) - 1));
    case "SINT": case "SSCALED": return toSigned(raw, bits);
    case "UFLOAT": return bits === 11 ? float11ToFloat32(raw) : bits === 10 ? float10ToFloat32(raw) : raw;
    default: return raw;
  }
}

function toSigned(raw: number, bits: number): number {
  return raw >= Math.pow(2, bits - 1) ? raw - Math.pow(2, bits) : raw;
}

const ORDER: Record<string, number> = { R: 0, G: 1, B: 2, A: 3 };

/** Parses a VkFormat name into a decoder, or null for formats that are not vertex formats. */
export function vertexFormat(name: string): VertexFormat | null {
  const hit = cache.get(name);
  if (hit !== undefined) return hit;
  const f = parse(name);
  cache.set(name, f);
  return f;
}

function parse(name: string): VertexFormat | null {
  const m = /^VK_FORMAT_((?:[RGBA]\d+)+)_([A-Z]+)(?:_PACK(8|16|32))?$/.exec(name);
  if (!m) return null;
  const channels: Channel[] = [];
  for (const c of m[1].matchAll(/([RGBA])(\d+)/g)) channels.push({ name: c[1], bits: Number(c[2]) });
  const numeric = m[2];
  const pack = m[3] ? Number(m[3]) : 0;
  const integer = numeric === "UINT" || numeric === "SINT" || numeric === "USCALED" || numeric === "SSCALED";
  // Output in R, G, B, A order whatever the storage order.
  const outputOrder = channels.map((c, i) => ({ c, i })).sort((a, b) => (ORDER[a.c.name] ?? 9) - (ORDER[b.c.name] ?? 9));
  const outChannels = outputOrder.map((o) => o.c.name);

  if (pack) {
    const size = pack / 8;
    const total = channels.reduce((s, c) => s + c.bits, 0);
    if (total !== pack) return null;
    const read = (view: DataView, offset: number): number[] => {
      const v = pack === 8 ? view.getUint8(offset) : pack === 16 ? view.getUint16(offset, true) : view.getUint32(offset, true);
      // Channels are listed from the most significant bits down.
      const raw: number[] = [];
      let shift = pack;
      for (const c of channels) {
        shift -= c.bits;
        raw.push(Math.floor(v / Math.pow(2, shift)) % Math.pow(2, c.bits));
      }
      return outputOrder.map((o) => convert(raw[o.i], o.c.bits, numeric));
    };
    return { size, channels: outChannels, integer, read };
  }

  const size = channels.reduce((s, c) => s + c.bits / 8, 0);
  const readers: ((view: DataView, offset: number) => number)[] = [];
  let byteOffset = 0;
  for (const c of channels) {
    const o = byteOffset;
    const bits = c.bits;
    byteOffset += bits / 8;
    if (numeric === "SFLOAT") {
      if (bits === 16) readers.push((v, off) => float16ToFloat32(v.getUint16(off + o, true)));
      else if (bits === 32) readers.push((v, off) => v.getFloat32(off + o, true));
      else if (bits === 64) readers.push((v, off) => v.getFloat64(off + o, true));
      else return null;
    } else if (bits === 8) {
      readers.push((v, off) => convert(v.getUint8(off + o), 8, numeric));
    } else if (bits === 16) {
      readers.push((v, off) => convert(v.getUint16(off + o, true), 16, numeric));
    } else if (bits === 32) {
      readers.push((v, off) => numeric === "SINT" || numeric === "SSCALED" ? v.getInt32(off + o, true) : convert(v.getUint32(off + o, true), 32, numeric));
    } else if (bits === 64) {
      readers.push((v, off) => numeric === "SINT" || numeric === "SSCALED" ? Number(v.getBigInt64(off + o, true)) : Number(v.getBigUint64(off + o, true)));
    } else {
      return null;
    }
  }
  const read = (view: DataView, offset: number): number[] => outputOrder.map((o) => readers[o.i](view, offset));
  return { size, channels: outChannels, integer, read };
}
