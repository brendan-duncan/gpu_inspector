// CPU decoding of read-back image data, in two stages like WebGPU Inspector's texture display:
//   decodeTexels():  raw bytes -> one float per channel per texel (the values the tooltip shows),
//   displayTexels(): floats -> RGBA8 for the canvas, applying the display settings (channel
//                    selection, exposure, auto range, sRGB encoding of linear data).
// Covers the color and depth formats, the BC1-BC7 block formats, ETC2 / EAC, ASTC (LDR
// endpoints; HDR blocks show the error colour), PVRTC1 and Metal's packed 422 and extended
// range formats.
import type { ImageDataInfo } from "../../shared/protocol.js";
import { decodeAstcBlock } from "./astc_decode.js";
import { decodeBc6hBlock, decodeBc7Block } from "./bc67_decode.js";
import { decodeEacR11, decodeEacRg11, decodeEtc2Rgb, decodeEtc2Rgba1, decodeEtc2Rgba8 } from "./etc_decode.js";
import { decodePvrtc } from "./pvrtc_decode.js";

/** Decoded texel values of one image slice. `values` holds 4 floats per texel (unused = 0). */
export interface TexelData {
  width: number;
  height: number;
  /** Channels the format actually stores (1..4). */
  channels: number;
  /** True when values are linear (float formats, depth) and should be sRGB-encoded for display. */
  linear: boolean;
  /** True when values are integers (UINT/SINT formats). */
  integer: boolean;
  /** Channel names for the tooltip ("R", "G", "B", "A" or "D" / "S"). */
  names: string[];
  values: Float32Array;
  min: number[];
  max: number[];
}

export type ChannelMode = "rgb" | "r" | "g" | "b" | "a" | "luminance";

export interface DisplaySettings {
  channels: ChannelMode;
  exposure: number;
  /** Stretch the values so the smallest maps to black and the largest to white. */
  autoRange: boolean;
}

export const DEFAULT_DISPLAY: DisplaySettings = { channels: "rgb", exposure: 1, autoRange: false };

function halfToFloat(h: number): number {
  const s = (h & 0x8000) ? -1 : 1;
  const e = (h >> 10) & 0x1f;
  const f = h & 0x3ff;
  if (e === 0) return s * Math.pow(2, -14) * (f / 1024);
  if (e === 31) return f ? NaN : s * Infinity;
  return s * Math.pow(2, e - 15) * (1 + f / 1024);
}

function srgbEncode(v: number): number {
  v = v < 0 ? 0 : v > 1 ? 1 : v;
  return v <= 0.0031308 ? v * 12.92 : 1.055 * Math.pow(v, 1 / 2.4) - 0.055;
}

// ---------------------------------------------------------------------------------------------
// Uncompressed formats: bytes per texel and a reader producing floats into out[o..o+3].

type Reader = (s: DataView, t: number, out: Float32Array, o: number) => void;

interface Format {
  bytes: number;
  channels: number;
  linear?: boolean;
  integer?: boolean;
  /** Channel names when they are not R, G, B, A (an alpha-only format, YCbCr). */
  names?: string[];
  read: Reader;
}

const u8 = (s: DataView, t: number): number => s.getUint8(t) / 255;
const s8 = (s: DataView, t: number): number => Math.max(-1, s.getInt8(t) / 127);
const u16n = (s: DataView, t: number): number => s.getUint16(t, true) / 65535;
const s16n = (s: DataView, t: number): number => Math.max(-1, s.getInt16(t, true) / 32767);
const f16 = (s: DataView, t: number): number => halfToFloat(s.getUint16(t, true));
const f32 = (s: DataView, t: number): number => s.getFloat32(t, true);
const u16 = (s: DataView, t: number): number => s.getUint16(t, true);
const s16 = (s: DataView, t: number): number => s.getInt16(t, true);
const u32 = (s: DataView, t: number): number => s.getUint32(t, true);
const s32 = (s: DataView, t: number): number => s.getInt32(t, true);
const u8i = (s: DataView, t: number): number => s.getUint8(t);
const s8i = (s: DataView, t: number): number => s.getInt8(t);

/** Metal's extended-range 10-bit encoding: 0 at 384, 1 at 894, so -0.75..1.25 fits. */
const xr10 = (v: number): number => (v - 384) / 510;

/** A 5-bit unsigned float with `mantissaBits` mantissa bits (the shared-exponent and 11/10-bit packed formats). */
function smallFloat(bits: number, mantissaBits: number): number {
  const e = (bits >> mantissaBits) & 0x1f;
  const m = bits & ((1 << mantissaBits) - 1);
  const scale = 1 << mantissaBits;
  return e === 0 ? Math.pow(2, -14) * (m / scale) : Math.pow(2, e - 15) * (1 + m / scale);
}

/** An integer format: `count` channels of `size` bytes read by `fn`. */
function ints(fn: (s: DataView, t: number) => number, size: number, count: number): Format {
  const order: number[] = [];
  for (let i = 0; i < count; i++) order.push(i);
  return { bytes: size * count, channels: count, integer: true, read: ch(fn, size, order) };
}

/** Builds a reader from per-channel readers and byte offsets. */
function ch(fn: (s: DataView, t: number) => number, size: number, order: number[]): Reader {
  return (s, t, out, o) => {
    for (let i = 0; i < order.length; i++) out[o + i] = fn(s, t + order[i] * size);
  };
}

const FORMATS: Record<string, Format> = {
  VK_FORMAT_A8_UNORM_KHR: { bytes: 1, channels: 1, names: ["A"], read: ch(u8, 1, [0]) },
  VK_FORMAT_R8G8_SINT: ints(s8i, 1, 2),
  VK_FORMAT_R8G8B8A8_SINT: ints(s8i, 1, 4),
  VK_FORMAT_R16_SINT: ints(s16, 2, 1),
  VK_FORMAT_R16G16_UINT: ints(u16, 2, 2),
  VK_FORMAT_R16G16_SINT: ints(s16, 2, 2),
  VK_FORMAT_R16G16B16A16_UINT: ints(u16, 2, 4),
  VK_FORMAT_R16G16B16A16_SINT: ints(s16, 2, 4),
  VK_FORMAT_R32G32_UINT: ints(u32, 4, 2),
  VK_FORMAT_R32G32_SINT: ints(s32, 4, 2),
  VK_FORMAT_R32G32B32A32_UINT: ints(u32, 4, 4),
  VK_FORMAT_R32G32B32A32_SINT: ints(s32, 4, 4),
  VK_FORMAT_A2B10G10R10_UINT_PACK32: { bytes: 4, channels: 4, integer: true, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o] = v & 0x3ff; out[o + 1] = (v >> 10) & 0x3ff; out[o + 2] = (v >> 20) & 0x3ff; out[o + 3] = (v >>> 30) & 3;
  } },
  VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: { bytes: 4, channels: 3, linear: true, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    const scale = Math.pow(2, ((v >>> 27) & 0x1f) - 15 - 9);
    out[o] = (v & 0x1ff) * scale; out[o + 1] = ((v >> 9) & 0x1ff) * scale; out[o + 2] = ((v >> 18) & 0x1ff) * scale;
  } },
  VK_FORMAT_R5G5B5A1_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = ((v >> 11) & 0x1f) / 31; out[o + 1] = ((v >> 6) & 0x1f) / 31; out[o + 2] = ((v >> 1) & 0x1f) / 31; out[o + 3] = v & 1;
  } },
  VK_FORMAT_A1R5G5B5_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = ((v >> 10) & 0x1f) / 31; out[o + 1] = ((v >> 5) & 0x1f) / 31; out[o + 2] = (v & 0x1f) / 31; out[o + 3] = (v >> 15) & 1;
  } },
  VK_FORMAT_B5G5R5A1_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o + 2] = ((v >> 11) & 0x1f) / 31; out[o + 1] = ((v >> 6) & 0x1f) / 31; out[o] = ((v >> 1) & 0x1f) / 31; out[o + 3] = v & 1;
  } },
  VK_FORMAT_R4G4B4A4_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = ((v >> 12) & 0xf) / 15; out[o + 1] = ((v >> 8) & 0xf) / 15; out[o + 2] = ((v >> 4) & 0xf) / 15; out[o + 3] = (v & 0xf) / 15;
  } },
  VK_FORMAT_B4G4R4A4_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o + 2] = ((v >> 12) & 0xf) / 15; out[o + 1] = ((v >> 8) & 0xf) / 15; out[o] = ((v >> 4) & 0xf) / 15; out[o + 3] = (v & 0xf) / 15;
  } },
  VK_FORMAT_B5G6R5_UNORM_PACK16: { bytes: 2, channels: 3, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o + 2] = ((v >> 11) & 0x1f) / 31; out[o + 1] = ((v >> 5) & 0x3f) / 63; out[o] = (v & 0x1f) / 31;
  } },
  // Metal's extended-range formats (no Vulkan spelling): 10 bits per channel, B lowest. The
  // 64-bit form keeps each channel's 10 bits in the low bits of a 16-bit word.
  MTLPixelFormatBGR10_XR: { bytes: 4, channels: 3, linear: true, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o + 2] = xr10(v & 0x3ff); out[o + 1] = xr10((v >> 10) & 0x3ff); out[o] = xr10((v >> 20) & 0x3ff);
  } },
  MTLPixelFormatBGR10_XR_sRGB: { bytes: 4, channels: 3, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o + 2] = xr10(v & 0x3ff); out[o + 1] = xr10((v >> 10) & 0x3ff); out[o] = xr10((v >> 20) & 0x3ff);
  } },
  MTLPixelFormatBGRA10_XR: { bytes: 8, channels: 4, linear: true, read: (s, t, out, o) => {
    out[o + 2] = xr10(s.getUint16(t, true) & 0x3ff); out[o + 1] = xr10(s.getUint16(t + 2, true) & 0x3ff);
    out[o] = xr10(s.getUint16(t + 4, true) & 0x3ff); out[o + 3] = xr10(s.getUint16(t + 6, true) & 0x3ff);
  } },
  MTLPixelFormatBGRA10_XR_sRGB: { bytes: 8, channels: 4, read: (s, t, out, o) => {
    out[o + 2] = xr10(s.getUint16(t, true) & 0x3ff); out[o + 1] = xr10(s.getUint16(t + 2, true) & 0x3ff);
    out[o] = xr10(s.getUint16(t + 4, true) & 0x3ff); out[o + 3] = xr10(s.getUint16(t + 6, true) & 0x3ff);
  } },
  VK_FORMAT_R8_UNORM: { bytes: 1, channels: 1, read: ch(u8, 1, [0]) },
  VK_FORMAT_R8_SRGB: { bytes: 1, channels: 1, read: ch(u8, 1, [0]) },
  VK_FORMAT_R8_SNORM: { bytes: 1, channels: 1, read: ch(s8, 1, [0]) },
  VK_FORMAT_R8_UINT: { bytes: 1, channels: 1, integer: true, read: (s, t, out, o) => { out[o] = s.getUint8(t); } },
  VK_FORMAT_R8_SINT: { bytes: 1, channels: 1, integer: true, read: (s, t, out, o) => { out[o] = s.getInt8(t); } },
  VK_FORMAT_R8G8_UNORM: { bytes: 2, channels: 2, read: ch(u8, 1, [0, 1]) },
  VK_FORMAT_R8G8_SRGB: { bytes: 2, channels: 2, read: ch(u8, 1, [0, 1]) },
  VK_FORMAT_R8G8_SNORM: { bytes: 2, channels: 2, read: ch(s8, 1, [0, 1]) },
  VK_FORMAT_R8G8_UINT: { bytes: 2, channels: 2, integer: true, read: (s, t, out, o) => { out[o] = s.getUint8(t); out[o + 1] = s.getUint8(t + 1); } },
  VK_FORMAT_R8G8B8_UNORM: { bytes: 3, channels: 3, read: ch(u8, 1, [0, 1, 2]) },
  VK_FORMAT_R8G8B8_SRGB: { bytes: 3, channels: 3, read: ch(u8, 1, [0, 1, 2]) },
  VK_FORMAT_B8G8R8_UNORM: { bytes: 3, channels: 3, read: ch(u8, 1, [2, 1, 0]) },
  VK_FORMAT_B8G8R8_SRGB: { bytes: 3, channels: 3, read: ch(u8, 1, [2, 1, 0]) },
  VK_FORMAT_R8G8B8A8_UNORM: { bytes: 4, channels: 4, read: ch(u8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_R8G8B8A8_SRGB: { bytes: 4, channels: 4, read: ch(u8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_R8G8B8A8_SNORM: { bytes: 4, channels: 4, read: ch(s8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_R8G8B8A8_UINT: { bytes: 4, channels: 4, integer: true, read: (s, t, out, o) => { for (let i = 0; i < 4; i++) out[o + i] = s.getUint8(t + i); } },
  VK_FORMAT_B8G8R8A8_UNORM: { bytes: 4, channels: 4, read: ch(u8, 1, [2, 1, 0, 3]) },
  VK_FORMAT_B8G8R8A8_SRGB: { bytes: 4, channels: 4, read: ch(u8, 1, [2, 1, 0, 3]) },
  VK_FORMAT_A8B8G8R8_UNORM_PACK32: { bytes: 4, channels: 4, read: ch(u8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_A8B8G8R8_SRGB_PACK32: { bytes: 4, channels: 4, read: ch(u8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_R16_UNORM: { bytes: 2, channels: 1, read: ch(u16n, 2, [0]) },
  VK_FORMAT_R16_SNORM: { bytes: 2, channels: 1, read: ch(s16n, 2, [0]) },
  VK_FORMAT_R16_UINT: { bytes: 2, channels: 1, integer: true, read: (s, t, out, o) => { out[o] = s.getUint16(t, true); } },
  VK_FORMAT_R16_SFLOAT: { bytes: 2, channels: 1, linear: true, read: ch(f16, 2, [0]) },
  VK_FORMAT_R16G16_UNORM: { bytes: 4, channels: 2, read: ch(u16n, 2, [0, 1]) },
  VK_FORMAT_R16G16_SNORM: { bytes: 4, channels: 2, read: ch(s16n, 2, [0, 1]) },
  VK_FORMAT_R16G16_SFLOAT: { bytes: 4, channels: 2, linear: true, read: ch(f16, 2, [0, 1]) },
  VK_FORMAT_R16G16B16A16_UNORM: { bytes: 8, channels: 4, read: ch(u16n, 2, [0, 1, 2, 3]) },
  VK_FORMAT_R16G16B16A16_SNORM: { bytes: 8, channels: 4, read: ch(s16n, 2, [0, 1, 2, 3]) },
  VK_FORMAT_R16G16B16A16_SFLOAT: { bytes: 8, channels: 4, linear: true, read: ch(f16, 2, [0, 1, 2, 3]) },
  VK_FORMAT_R32_UINT: { bytes: 4, channels: 1, integer: true, read: (s, t, out, o) => { out[o] = s.getUint32(t, true); } },
  VK_FORMAT_R32_SINT: { bytes: 4, channels: 1, integer: true, read: (s, t, out, o) => { out[o] = s.getInt32(t, true); } },
  VK_FORMAT_R32_SFLOAT: { bytes: 4, channels: 1, linear: true, read: ch(f32, 4, [0]) },
  VK_FORMAT_R32G32_SFLOAT: { bytes: 8, channels: 2, linear: true, read: ch(f32, 4, [0, 1]) },
  VK_FORMAT_R32G32B32_SFLOAT: { bytes: 12, channels: 3, linear: true, read: ch(f32, 4, [0, 1, 2]) },
  VK_FORMAT_R32G32B32A32_SFLOAT: { bytes: 16, channels: 4, linear: true, read: ch(f32, 4, [0, 1, 2, 3]) },
  VK_FORMAT_B10G11R11_UFLOAT_PACK32: { bytes: 4, channels: 3, linear: true, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o] = smallFloat(v & 0x7ff, 6);
    out[o + 1] = smallFloat((v >> 11) & 0x7ff, 6);
    out[o + 2] = smallFloat((v >> 22) & 0x3ff, 5);
  } },
  VK_FORMAT_A2B10G10R10_UNORM_PACK32: { bytes: 4, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o] = (v & 0x3ff) / 1023;
    out[o + 1] = ((v >> 10) & 0x3ff) / 1023;
    out[o + 2] = ((v >> 20) & 0x3ff) / 1023;
    out[o + 3] = ((v >>> 30) & 0x3) / 3;
  } },
  VK_FORMAT_A2R10G10B10_UNORM_PACK32: { bytes: 4, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o + 2] = (v & 0x3ff) / 1023;
    out[o + 1] = ((v >> 10) & 0x3ff) / 1023;
    out[o] = ((v >> 20) & 0x3ff) / 1023;
    out[o + 3] = ((v >>> 30) & 0x3) / 3;
  } },
  VK_FORMAT_R5G6B5_UNORM_PACK16: { bytes: 2, channels: 3, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = ((v >> 11) & 0x1f) / 31;
    out[o + 1] = ((v >> 5) & 0x3f) / 63;
    out[o + 2] = (v & 0x1f) / 31;
  } },
};

const DEPTH_FORMATS: Record<string, { bytes: number; read: (s: DataView, t: number) => number }> = {
  VK_FORMAT_D16_UNORM: { bytes: 2, read: (s, t) => s.getUint16(t, true) / 65535 },
  VK_FORMAT_D16_UNORM_S8_UINT: { bytes: 2, read: (s, t) => s.getUint16(t, true) / 65535 },
  VK_FORMAT_X8_D24_UNORM_PACK32: { bytes: 4, read: (s, t) => (s.getUint32(t, true) & 0xffffff) / 16777215 },
  VK_FORMAT_D24_UNORM_S8_UINT: { bytes: 4, read: (s, t) => (s.getUint32(t, true) & 0xffffff) / 16777215 },
  VK_FORMAT_D32_SFLOAT: { bytes: 4, read: (s, t) => s.getFloat32(t, true) },
  VK_FORMAT_D32_SFLOAT_S8_UINT: { bytes: 4, read: (s, t) => s.getFloat32(t, true) },
};

// ---------------------------------------------------------------------------------------------
// Block compression. Blocks of `width` x `height` texels are stored row-major over the padded
// image; a decoder writes the block's texels as RGBA floats (0..1) into px, row-major. A
// format whose texels depend on neighbouring blocks (PVRTC) decodes the whole image instead.

interface BlockFormat {
  bytes: number;
  width: number;
  height: number;
  channels: number;
  linear?: boolean;
  names?: string[];
  decode?: (s: DataView, block: number, px: Float32Array) => void;
  decodeImage?: (s: DataView, width: number, height: number, values: Float32Array) => void;
}

function rgb565(v: number): [number, number, number] {
  return [((v >> 11) & 0x1f) / 31, ((v >> 5) & 0x3f) / 63, (v & 0x1f) / 31];
}

function decodeBc1(s: DataView, t: number, px: Float32Array, alpha1Bit: boolean): void {
  const c0 = s.getUint16(t, true);
  const c1 = s.getUint16(t + 2, true);
  const idx = s.getUint32(t + 4, true);
  const [r0, g0, b0] = rgb565(c0);
  const [r1, g1, b1] = rgb565(c1);
  const palette: number[][] = [[r0, g0, b0, 1], [r1, g1, b1, 1]];
  if (c0 > c1 || !alpha1Bit) {
    palette.push([(2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 1]);
    palette.push([(r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 1]);
  } else {
    palette.push([(r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 1]);
    palette.push([0, 0, 0, 0]);
  }
  for (let i = 0; i < 16; i++) {
    const c = palette[(idx >>> (i * 2)) & 3];
    px[i * 4] = c[0]; px[i * 4 + 1] = c[1]; px[i * 4 + 2] = c[2]; px[i * 4 + 3] = c[3];
  }
}

/** BC4 channel block: 2 endpoints + 3-bit indices. Writes one channel of each texel. */
function decodeBc4Channel(s: DataView, t: number, px: Float32Array, channel: number, signed: boolean): void {
  const raw0 = s.getUint8(t);
  const raw1 = s.getUint8(t + 1);
  const toValue = signed
    ? (v: number): number => Math.max(-127, (v << 24) >> 24) / 127
    : (v: number): number => v / 255;
  const a0 = toValue(raw0);
  const a1 = toValue(raw1);
  const palette = [a0, a1];
  const gt = signed ? (raw0 << 24) >> 24 > (raw1 << 24) >> 24 : raw0 > raw1;
  if (gt) {
    for (let i = 1; i <= 6; i++) palette.push(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (let i = 1; i <= 4; i++) palette.push(((5 - i) * a0 + i * a1) / 5);
    palette.push(signed ? -1 : 0, 1);
  }
  const lo = s.getUint32(t + 2, true);
  const hi = s.getUint16(t + 6, true);
  for (let i = 0; i < 16; i++) {
    const bit = i * 3;
    let v: number;
    if (bit + 3 <= 32) v = (lo >>> bit) & 7;
    else if (bit >= 32) v = (hi >>> (bit - 32)) & 7;
    else v = ((lo >>> bit) | (hi << (32 - bit))) & 7;
    px[i * 4 + channel] = palette[v];
  }
}

const BC1: BlockFormat = { bytes: 8, width: 4, height: 4, channels: 4, decode: (s, b, px) => decodeBc1(s, b, px, true) };
const BC2: BlockFormat = { bytes: 16, width: 4, height: 4, channels: 4, decode: (s, b, px) => {
  decodeBc1(s, b + 8, px, false);
  for (let i = 0; i < 16; i++) px[i * 4 + 3] = ((s.getUint16(b + (i >> 2) * 2, true) >> ((i & 3) * 4)) & 0xf) / 15;
} };
const BC3: BlockFormat = { bytes: 16, width: 4, height: 4, channels: 4, decode: (s, b, px) => {
  decodeBc1(s, b + 8, px, false);
  decodeBc4Channel(s, b, px, 3, false);
} };
const bc4 = (signed: boolean): BlockFormat => ({ bytes: 8, width: 4, height: 4, channels: 1, decode: (s, b, px) => {
  px.fill(0);
  decodeBc4Channel(s, b, px, 0, signed);
} });
const bc5 = (signed: boolean): BlockFormat => ({ bytes: 16, width: 4, height: 4, channels: 2, decode: (s, b, px) => {
  px.fill(0);
  decodeBc4Channel(s, b, px, 0, signed);
  decodeBc4Channel(s, b + 8, px, 1, signed);
} });

const BLOCK_FORMATS: Record<string, BlockFormat> = {
  VK_FORMAT_BC1_RGB_UNORM_BLOCK: { ...BC1, channels: 3 }, VK_FORMAT_BC1_RGB_SRGB_BLOCK: { ...BC1, channels: 3 },
  VK_FORMAT_BC1_RGBA_UNORM_BLOCK: BC1, VK_FORMAT_BC1_RGBA_SRGB_BLOCK: BC1,
  VK_FORMAT_BC2_UNORM_BLOCK: BC2, VK_FORMAT_BC2_SRGB_BLOCK: BC2,
  VK_FORMAT_BC3_UNORM_BLOCK: BC3, VK_FORMAT_BC3_SRGB_BLOCK: BC3,
  VK_FORMAT_BC4_UNORM_BLOCK: bc4(false), VK_FORMAT_BC4_SNORM_BLOCK: bc4(true),
  VK_FORMAT_BC5_UNORM_BLOCK: bc5(false), VK_FORMAT_BC5_SNORM_BLOCK: bc5(true),
  VK_FORMAT_BC6H_UFLOAT_BLOCK: { bytes: 16, width: 4, height: 4, channels: 3, linear: true, decode: (s, b, px) => decodeBc6hBlock(s, b, px, false) },
  VK_FORMAT_BC6H_SFLOAT_BLOCK: { bytes: 16, width: 4, height: 4, channels: 3, linear: true, decode: (s, b, px) => decodeBc6hBlock(s, b, px, true) },
  VK_FORMAT_BC7_UNORM_BLOCK: { bytes: 16, width: 4, height: 4, channels: 4, decode: decodeBc7Block },
  VK_FORMAT_BC7_SRGB_BLOCK: { bytes: 16, width: 4, height: 4, channels: 4, decode: decodeBc7Block },
  VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: { bytes: 8, width: 4, height: 4, channels: 3, decode: decodeEtc2Rgb },
  VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: { bytes: 8, width: 4, height: 4, channels: 3, decode: decodeEtc2Rgb },
  VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: { bytes: 8, width: 4, height: 4, channels: 4, decode: decodeEtc2Rgba1 },
  VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: { bytes: 8, width: 4, height: 4, channels: 4, decode: decodeEtc2Rgba1 },
  VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: { bytes: 16, width: 4, height: 4, channels: 4, decode: decodeEtc2Rgba8 },
  VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: { bytes: 16, width: 4, height: 4, channels: 4, decode: decodeEtc2Rgba8 },
  VK_FORMAT_EAC_R11_UNORM_BLOCK: { bytes: 8, width: 4, height: 4, channels: 1, decode: (s, b, px) => decodeEacR11(s, b, px, false) },
  VK_FORMAT_EAC_R11_SNORM_BLOCK: { bytes: 8, width: 4, height: 4, channels: 1, decode: (s, b, px) => decodeEacR11(s, b, px, true) },
  VK_FORMAT_EAC_R11G11_UNORM_BLOCK: { bytes: 16, width: 4, height: 4, channels: 2, decode: (s, b, px) => decodeEacRg11(s, b, px, false) },
  VK_FORMAT_EAC_R11G11_SNORM_BLOCK: { bytes: 16, width: 4, height: 4, channels: 2, decode: (s, b, px) => decodeEacRg11(s, b, px, true) },
  VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG: { bytes: 8, width: 8, height: 4, channels: 4, decodeImage: (s, w, h, v) => decodePvrtc(s, w, h, true, v) },
  VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG: { bytes: 8, width: 8, height: 4, channels: 4, decodeImage: (s, w, h, v) => decodePvrtc(s, w, h, true, v) },
  VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG: { bytes: 8, width: 4, height: 4, channels: 4, decodeImage: (s, w, h, v) => decodePvrtc(s, w, h, false, v) },
  VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG: { bytes: 8, width: 4, height: 4, channels: 4, decodeImage: (s, w, h, v) => decodePvrtc(s, w, h, false, v) },
  // Packed 4:2:2: two texels share their chroma. Shown as the stored Y, Cb and Cr values in
  // the G, B and R channels, the way Metal's sampler returns them, without a colour conversion.
  VK_FORMAT_G8B8G8R8_422_UNORM: { bytes: 4, width: 2, height: 1, channels: 3, names: ["R (Cr)", "G (Y)", "B (Cb)"], decode: (s, b, px) => {
    const g0 = s.getUint8(b) / 255, cb = s.getUint8(b + 1) / 255, g1 = s.getUint8(b + 2) / 255, cr = s.getUint8(b + 3) / 255;
    px[0] = cr; px[1] = g0; px[2] = cb; px[3] = 1; px[4] = cr; px[5] = g1; px[6] = cb; px[7] = 1;
  } },
  VK_FORMAT_B8G8R8G8_422_UNORM: { bytes: 4, width: 2, height: 1, channels: 3, names: ["R (Cr)", "G (Y)", "B (Cb)"], decode: (s, b, px) => {
    const cb = s.getUint8(b) / 255, g0 = s.getUint8(b + 1) / 255, cr = s.getUint8(b + 2) / 255, g1 = s.getUint8(b + 3) / 255;
    px[0] = cr; px[1] = g0; px[2] = cb; px[3] = 1; px[4] = cr; px[5] = g1; px[6] = cb; px[7] = 1;
  } },
};

// ASTC: every footprint, in UNORM, sRGB and (HDR profile) float flavours. The float flavour
// decodes the LDR blocks of an HDR image; its HDR blocks show the error colour.
for (const [w, h] of [[4, 4], [5, 4], [5, 5], [6, 5], [6, 6], [8, 5], [8, 6], [8, 8], [10, 5], [10, 6], [10, 8], [10, 10], [12, 10], [12, 12]]) {
  const ldr = (srgb: boolean): BlockFormat => ({ bytes: 16, width: w, height: h, channels: 4, decode: (s, b, px) => decodeAstcBlock(s, b, w, h, px, srgb) });
  BLOCK_FORMATS[`VK_FORMAT_ASTC_${w}x${h}_UNORM_BLOCK`] = ldr(false);
  BLOCK_FORMATS[`VK_FORMAT_ASTC_${w}x${h}_SRGB_BLOCK`] = ldr(true);
  BLOCK_FORMATS[`VK_FORMAT_ASTC_${w}x${h}_SFLOAT_BLOCK`] = { ...ldr(false), linear: true };
}

// ---------------------------------------------------------------------------------------------

/** Bytes per (w x h) slice of image data for the format, or 0 if unknown. */
export function sliceBytes(info: ImageDataInfo): number {
  const w = info.width;
  const h = info.height;
  if (info.aspect === "depth") return (DEPTH_FORMATS[info.format]?.bytes ?? 0) * w * h;
  if (info.aspect === "stencil") return w * h;
  const block = BLOCK_FORMATS[info.format];
  if (block) return Math.ceil(w / block.width) * Math.ceil(h / block.height) * block.bytes;
  return (FORMATS[info.format]?.bytes ?? 0) * w * h;
}

export function isFormatSupported(info: ImageDataInfo): boolean {
  return sliceBytes(info) > 0;
}

function finish(tex: TexelData): TexelData {
  const n = tex.width * tex.height;
  for (let c = 0; c < tex.channels; c++) {
    let min = Infinity;
    let max = -Infinity;
    for (let i = 0; i < n; i++) {
      const v = tex.values[i * 4 + c];
      if (v < min) min = v;
      if (v > max) max = v;
    }
    tex.min[c] = min;
    tex.max[c] = max;
  }
  return tex;
}

/**
 * Decodes one slice (array layer or depth slice) of image data into per-channel floats, or null
 * if the format is unsupported. `slice` indexes consecutive slices in `data`.
 */
export function decodeTexels(info: ImageDataInfo, data: Uint8Array, slice = 0): TexelData | null {
  const w = info.width;
  const h = info.height;
  const bytes = sliceBytes(info);
  if (!bytes) return null;
  const offset = slice * bytes;
  if (data.byteLength < offset + bytes) return null;
  const view = new DataView(data.buffer, data.byteOffset + offset, bytes);
  const n = w * h;
  const values = new Float32Array(n * 4);
  const tex: TexelData = { width: w, height: h, channels: 4, linear: false, integer: false, names: ["R", "G", "B", "A"], values, min: [], max: [] };

  if (info.aspect === "depth") {
    const d = DEPTH_FORMATS[info.format];
    if (!d) return null;
    for (let i = 0; i < n; i++) values[i * 4] = d.read(view, i * d.bytes);
    tex.channels = 1;
    tex.linear = true;
    tex.names = ["D"];
    return finish(tex);
  }
  if (info.aspect === "stencil") {
    for (let i = 0; i < n; i++) values[i * 4] = view.getUint8(i);
    tex.channels = 1;
    tex.integer = true;
    tex.names = ["S"];
    return finish(tex);
  }
  const block = BLOCK_FORMATS[info.format];
  if (block) {
    tex.channels = block.channels;
    tex.linear = !!block.linear;
    if (block.names) tex.names = block.names;
    if (block.decodeImage) {
      block.decodeImage(view, w, h, values);
      return finish(tex);
    }
    const bw = Math.ceil(w / block.width);
    const bh = Math.ceil(h / block.height);
    const texels = block.width * block.height;
    const px = new Float32Array(texels * 4);
    for (let by = 0; by < bh; by++) {
      for (let bx = 0; bx < bw; bx++) {
        block.decode?.(view, (by * bw + bx) * block.bytes, px);
        for (let i = 0; i < texels; i++) {
          const x = bx * block.width + (i % block.width);
          const y = by * block.height + Math.floor(i / block.width);
          if (x >= w || y >= h) continue;
          const o = (y * w + x) * 4;
          values[o] = px[i * 4]; values[o + 1] = px[i * 4 + 1]; values[o + 2] = px[i * 4 + 2]; values[o + 3] = px[i * 4 + 3];
        }
      }
    }
    return finish(tex);
  }
  const f = FORMATS[info.format];
  if (!f) return null;
  for (let i = 0; i < n; i++) f.read(view, i * f.bytes, values, i * 4);
  tex.channels = f.channels;
  tex.linear = !!f.linear;
  tex.integer = !!f.integer;
  if (f.names) tex.names = f.names;
  return finish(tex);
}

/** Converts decoded texels to RGBA8 for a canvas, mirroring WebGPU Inspector's display shader. */
export function displayTexels(tex: TexelData, display: DisplaySettings = DEFAULT_DISPLAY): Uint8ClampedArray<ArrayBuffer> {
  const n = tex.width * tex.height;
  const out = new Uint8ClampedArray(n * 4);
  const v = tex.values;
  const colorChannels = Math.min(tex.channels, 3);
  let lo = Infinity;
  let hi = -Infinity;
  for (let c = 0; c < colorChannels; c++) {
    lo = Math.min(lo, tex.min[c]);
    hi = Math.max(hi, tex.max[c]);
  }
  const range = display.autoRange && hi - lo > 0.00001;
  const exposure = display.exposure;
  const encode = tex.linear ? srgbEncode : (x: number): number => x;
  const mode = display.channels;
  for (let i = 0; i < n; i++) {
    const o = i * 4;
    let r = v[o];
    let g = v[o + 1];
    let b = v[o + 2];
    let a = v[o + 3];
    if (range) {
      r = (r - lo) / (hi - lo);
      g = (g - lo) / (hi - lo);
      b = (b - lo) / (hi - lo);
    }
    if (tex.channels === 1) { g = r; b = r; a = 1; }
    else if (tex.channels === 2) { b = 0; a = 1; }
    else if (tex.channels === 3) a = 1;
    let dr: number;
    let dg: number;
    let db: number;
    switch (mode) {
      case "r": dr = r * exposure; dg = 0; db = 0; break;
      case "g": dr = 0; dg = g * exposure; db = 0; break;
      case "b": dr = 0; dg = 0; db = b * exposure; break;
      case "a": dr = dg = db = a * exposure; break;
      case "luminance": dr = dg = db = (0.2126 * r + 0.7152 * g + 0.0722 * b) * exposure; break;
      default: dr = r * exposure; dg = g * exposure; db = b * exposure; break;
    }
    out[o] = encode(dr) * 255;
    out[o + 1] = encode(dg) * 255;
    out[o + 2] = encode(db) * 255;
    out[o + 3] = mode === "rgb" ? a * 255 : 255;
  }
  return out;
}

/** One-step decode to RGBA8 with default display settings (capture panel thumbnails). */
export function decodeImage(info: ImageDataInfo, data: Uint8Array, slice = 0): Uint8ClampedArray<ArrayBuffer> | null {
  const tex = decodeTexels(info, data, slice);
  if (!tex) return null;
  return displayTexels(tex, { ...DEFAULT_DISPLAY, autoRange: info.aspect === "depth" });
}

/** Formats a texel's channel values for display (integers as-is, floats to 4 decimals). */
export function formatTexel(tex: TexelData, x: number, y: number): string[] {
  const o = (y * tex.width + x) * 4;
  const lines: string[] = [];
  for (let c = 0; c < tex.channels; c++) {
    const value = tex.values[o + c];
    lines.push(`${tex.names[c]}: ${tex.integer ? String(value) : formatFloat(value)}`);
  }
  return lines;
}

export function formatFloat(v: number): string {
  if (!Number.isFinite(v)) return String(v);
  if (v === 0) return "0";
  const a = Math.abs(v);
  if (a >= 1e5 || a < 1e-4) return v.toExponential(3);
  return v.toFixed(4).replace(/\.?0+$/, "");
}
