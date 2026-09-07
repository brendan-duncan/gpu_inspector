// CPU decoding of read-back render targets to RGBA8 for display. Covers the common attachment
// formats; block-compressed and exotic formats will use a GPU path later (WebGPU Inspector's
// texture_utils.js ported to WebGPU in the renderer).
import type { CaptureTextureInfo } from "../../shared/protocol.js";

function halfToFloat(h: number): number {
  const s = (h & 0x8000) ? -1 : 1;
  const e = (h >> 10) & 0x1f;
  const f = h & 0x3ff;
  if (e === 0) return s * Math.pow(2, -14) * (f / 1024);
  if (e === 31) return f ? NaN : s * Infinity;
  return s * Math.pow(2, e - 15) * (1 + f / 1024);
}

function unorm(v: number): number {
  return v < 0 ? 0 : v > 1 ? 255 : Math.round(v * 255);
}

function srgbEncode(v: number): number {
  // Linear float -> sRGB byte, used for float / linear formats so they look right on screen.
  v = v < 0 ? 0 : v > 1 ? 1 : v;
  return Math.round((v <= 0.0031308 ? v * 12.92 : 1.055 * Math.pow(v, 1 / 2.4) - 0.055) * 255);
}

type Decoder = (src: DataView, texel: number, out: Uint8ClampedArray, o: number) => void;

const DECODERS: Record<string, { bytes: number; decode: Decoder }> = {
  VK_FORMAT_R8G8B8A8_UNORM: { bytes: 4, decode: (s, t, out, o) => { out[o] = s.getUint8(t); out[o + 1] = s.getUint8(t + 1); out[o + 2] = s.getUint8(t + 2); out[o + 3] = s.getUint8(t + 3); } },
  VK_FORMAT_R8G8B8A8_SRGB: { bytes: 4, decode: (s, t, out, o) => { out[o] = s.getUint8(t); out[o + 1] = s.getUint8(t + 1); out[o + 2] = s.getUint8(t + 2); out[o + 3] = s.getUint8(t + 3); } },
  VK_FORMAT_B8G8R8A8_UNORM: { bytes: 4, decode: (s, t, out, o) => { out[o] = s.getUint8(t + 2); out[o + 1] = s.getUint8(t + 1); out[o + 2] = s.getUint8(t); out[o + 3] = s.getUint8(t + 3); } },
  VK_FORMAT_B8G8R8A8_SRGB: { bytes: 4, decode: (s, t, out, o) => { out[o] = s.getUint8(t + 2); out[o + 1] = s.getUint8(t + 1); out[o + 2] = s.getUint8(t); out[o + 3] = s.getUint8(t + 3); } },
  VK_FORMAT_A8B8G8R8_UNORM_PACK32: { bytes: 4, decode: (s, t, out, o) => { out[o] = s.getUint8(t); out[o + 1] = s.getUint8(t + 1); out[o + 2] = s.getUint8(t + 2); out[o + 3] = s.getUint8(t + 3); } },
  VK_FORMAT_A8B8G8R8_SRGB_PACK32: { bytes: 4, decode: (s, t, out, o) => { out[o] = s.getUint8(t); out[o + 1] = s.getUint8(t + 1); out[o + 2] = s.getUint8(t + 2); out[o + 3] = s.getUint8(t + 3); } },
  VK_FORMAT_R8_UNORM: { bytes: 1, decode: (s, t, out, o) => { const v = s.getUint8(t); out[o] = v; out[o + 1] = v; out[o + 2] = v; out[o + 3] = 255; } },
  VK_FORMAT_R8G8_UNORM: { bytes: 2, decode: (s, t, out, o) => { out[o] = s.getUint8(t); out[o + 1] = s.getUint8(t + 1); out[o + 2] = 0; out[o + 3] = 255; } },
  VK_FORMAT_R16G16B16A16_SFLOAT: { bytes: 8, decode: (s, t, out, o) => {
    out[o] = srgbEncode(halfToFloat(s.getUint16(t, true)));
    out[o + 1] = srgbEncode(halfToFloat(s.getUint16(t + 2, true)));
    out[o + 2] = srgbEncode(halfToFloat(s.getUint16(t + 4, true)));
    out[o + 3] = unorm(halfToFloat(s.getUint16(t + 6, true)));
  } },
  VK_FORMAT_R16G16_SFLOAT: { bytes: 4, decode: (s, t, out, o) => {
    out[o] = srgbEncode(halfToFloat(s.getUint16(t, true)));
    out[o + 1] = srgbEncode(halfToFloat(s.getUint16(t + 2, true)));
    out[o + 2] = 0;
    out[o + 3] = 255;
  } },
  VK_FORMAT_R16_SFLOAT: { bytes: 2, decode: (s, t, out, o) => { const v = srgbEncode(halfToFloat(s.getUint16(t, true))); out[o] = v; out[o + 1] = v; out[o + 2] = v; out[o + 3] = 255; } },
  VK_FORMAT_R32G32B32A32_SFLOAT: { bytes: 16, decode: (s, t, out, o) => {
    out[o] = srgbEncode(s.getFloat32(t, true));
    out[o + 1] = srgbEncode(s.getFloat32(t + 4, true));
    out[o + 2] = srgbEncode(s.getFloat32(t + 8, true));
    out[o + 3] = unorm(s.getFloat32(t + 12, true));
  } },
  VK_FORMAT_R32_SFLOAT: { bytes: 4, decode: (s, t, out, o) => { const v = srgbEncode(s.getFloat32(t, true)); out[o] = v; out[o + 1] = v; out[o + 2] = v; out[o + 3] = 255; } },
  VK_FORMAT_B10G11R11_UFLOAT_PACK32: { bytes: 4, decode: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    const f11 = (bits: number): number => { const e = (bits >> 6) & 0x1f; const m = bits & 0x3f; return e === 0 ? Math.pow(2, -14) * (m / 64) : Math.pow(2, e - 15) * (1 + m / 64); };
    const f10 = (bits: number): number => { const e = (bits >> 5) & 0x1f; const m = bits & 0x1f; return e === 0 ? Math.pow(2, -14) * (m / 32) : Math.pow(2, e - 15) * (1 + m / 32); };
    out[o] = srgbEncode(f11(v & 0x7ff));
    out[o + 1] = srgbEncode(f11((v >> 11) & 0x7ff));
    out[o + 2] = srgbEncode(f10((v >> 22) & 0x3ff));
    out[o + 3] = 255;
  } },
  VK_FORMAT_A2B10G10R10_UNORM_PACK32: { bytes: 4, decode: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o] = Math.round((v & 0x3ff) / 1023 * 255);
    out[o + 1] = Math.round(((v >> 10) & 0x3ff) / 1023 * 255);
    out[o + 2] = Math.round(((v >> 20) & 0x3ff) / 1023 * 255);
    out[o + 3] = Math.round(((v >>> 30) & 0x3) / 3 * 255);
  } },
  VK_FORMAT_A2R10G10B10_UNORM_PACK32: { bytes: 4, decode: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o + 2] = Math.round((v & 0x3ff) / 1023 * 255);
    out[o + 1] = Math.round(((v >> 10) & 0x3ff) / 1023 * 255);
    out[o] = Math.round(((v >> 20) & 0x3ff) / 1023 * 255);
    out[o + 3] = Math.round(((v >>> 30) & 0x3) / 3 * 255);
  } },
  VK_FORMAT_R5G6B5_UNORM_PACK16: { bytes: 2, decode: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = Math.round(((v >> 11) & 0x1f) / 31 * 255);
    out[o + 1] = Math.round(((v >> 5) & 0x3f) / 63 * 255);
    out[o + 2] = Math.round((v & 0x1f) / 31 * 255);
    out[o + 3] = 255;
  } },
};

const DEPTH_DECODERS: Record<string, { bytes: number; read: (s: DataView, t: number) => number }> = {
  VK_FORMAT_D16_UNORM: { bytes: 2, read: (s, t) => s.getUint16(t, true) / 65535 },
  VK_FORMAT_D16_UNORM_S8_UINT: { bytes: 2, read: (s, t) => s.getUint16(t, true) / 65535 },
  VK_FORMAT_X8_D24_UNORM_PACK32: { bytes: 4, read: (s, t) => (s.getUint32(t, true) & 0xffffff) / 16777215 },
  VK_FORMAT_D24_UNORM_S8_UINT: { bytes: 4, read: (s, t) => (s.getUint32(t, true) & 0xffffff) / 16777215 },
  VK_FORMAT_D32_SFLOAT: { bytes: 4, read: (s, t) => s.getFloat32(t, true) },
  VK_FORMAT_D32_SFLOAT_S8_UINT: { bytes: 4, read: (s, t) => s.getFloat32(t, true) },
};

/** Decodes the first layer of a captured texture to RGBA8, or null if the format is unsupported. */
export function decodeTexture(info: CaptureTextureInfo, data: Uint8Array): Uint8ClampedArray<ArrayBuffer> | null {
  const w = info.width;
  const h = info.height;
  const out = new Uint8ClampedArray(w * h * 4);
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);

  if (info.aspect === "depth") {
    const d = DEPTH_DECODERS[info.format];
    if (!d || data.byteLength < w * h * d.bytes) return null;
    // Auto-range so the depth buffer is visible (typical values cluster near 1.0).
    let min = Infinity;
    let max = -Infinity;
    const n = w * h;
    const values = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      const v = d.read(view, i * d.bytes);
      values[i] = v;
      if (v < min) min = v;
      if (v > max) max = v;
    }
    const range = max > min ? max - min : 1;
    for (let i = 0; i < n; i++) {
      const g = Math.round((values[i] - min) / range * 255);
      out[i * 4] = g;
      out[i * 4 + 1] = g;
      out[i * 4 + 2] = g;
      out[i * 4 + 3] = 255;
    }
    return out;
  }

  const dec = DECODERS[info.format];
  if (!dec || data.byteLength < w * h * dec.bytes) return null;
  const n = w * h;
  for (let i = 0; i < n; i++) dec.decode(view, i * dec.bytes, out, i * 4);
  return out;
}
