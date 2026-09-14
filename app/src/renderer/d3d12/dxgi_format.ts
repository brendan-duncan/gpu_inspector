// DXGI formats, as the D3D12 capture library names them in descriptors ("DXGI_FORMAT_R8G8B8A8_UNORM").
//
// Pixel data travels under the protocol's VK_FORMAT_* names (d3d12/src/formats.cpp maps each
// DXGI format to the Vulkan spelling of the same memory layout, so the UI's decoders read it
// unchanged); the DXGI name stays in the object descriptors. This is the same table on the UI
// side, for the places that read a format off a descriptor: a vertex layout's attribute formats,
// a texture's size estimate, whether a resource is a depth target.

interface Entry {
  vk: string | null;
  /** Bytes per pixel, or per block for a block-compressed format. */
  bytes: number;
  /** Block width in pixels (1 for uncompressed). */
  block: number;
  depth: boolean;
  stencil: boolean;
}

const TABLE: Record<string, Entry> = {
  R32G32B32A32_TYPELESS: { vk: "VK_FORMAT_R32G32B32A32_SFLOAT", bytes: 16, block: 1, depth: false, stencil: false },
  R32G32B32A32_FLOAT: { vk: "VK_FORMAT_R32G32B32A32_SFLOAT", bytes: 16, block: 1, depth: false, stencil: false },
  R32G32B32A32_UINT: { vk: "VK_FORMAT_R32G32B32A32_UINT", bytes: 16, block: 1, depth: false, stencil: false },
  R32G32B32A32_SINT: { vk: "VK_FORMAT_R32G32B32A32_SINT", bytes: 16, block: 1, depth: false, stencil: false },
  R32G32B32_TYPELESS: { vk: "VK_FORMAT_R32G32B32_SFLOAT", bytes: 12, block: 1, depth: false, stencil: false },
  R32G32B32_FLOAT: { vk: "VK_FORMAT_R32G32B32_SFLOAT", bytes: 12, block: 1, depth: false, stencil: false },
  R32G32B32_UINT: { vk: "VK_FORMAT_R32G32B32_UINT", bytes: 12, block: 1, depth: false, stencil: false },
  R32G32B32_SINT: { vk: "VK_FORMAT_R32G32B32_SINT", bytes: 12, block: 1, depth: false, stencil: false },
  R16G16B16A16_TYPELESS: { vk: "VK_FORMAT_R16G16B16A16_UNORM", bytes: 8, block: 1, depth: false, stencil: false },
  R16G16B16A16_FLOAT: { vk: "VK_FORMAT_R16G16B16A16_SFLOAT", bytes: 8, block: 1, depth: false, stencil: false },
  R16G16B16A16_UNORM: { vk: "VK_FORMAT_R16G16B16A16_UNORM", bytes: 8, block: 1, depth: false, stencil: false },
  R16G16B16A16_UINT: { vk: "VK_FORMAT_R16G16B16A16_UINT", bytes: 8, block: 1, depth: false, stencil: false },
  R16G16B16A16_SNORM: { vk: "VK_FORMAT_R16G16B16A16_SNORM", bytes: 8, block: 1, depth: false, stencil: false },
  R16G16B16A16_SINT: { vk: "VK_FORMAT_R16G16B16A16_SINT", bytes: 8, block: 1, depth: false, stencil: false },
  R32G32_TYPELESS: { vk: "VK_FORMAT_R32G32_SFLOAT", bytes: 8, block: 1, depth: false, stencil: false },
  R32G32_FLOAT: { vk: "VK_FORMAT_R32G32_SFLOAT", bytes: 8, block: 1, depth: false, stencil: false },
  R32G32_UINT: { vk: "VK_FORMAT_R32G32_UINT", bytes: 8, block: 1, depth: false, stencil: false },
  R32G32_SINT: { vk: "VK_FORMAT_R32G32_SINT", bytes: 8, block: 1, depth: false, stencil: false },
  R32G8X24_TYPELESS: { vk: "VK_FORMAT_D32_SFLOAT_S8_UINT", bytes: 8, block: 1, depth: true, stencil: true },
  D32_FLOAT_S8X24_UINT: { vk: "VK_FORMAT_D32_SFLOAT_S8_UINT", bytes: 8, block: 1, depth: true, stencil: true },
  R32_FLOAT_X8X24_TYPELESS: { vk: "VK_FORMAT_D32_SFLOAT_S8_UINT", bytes: 8, block: 1, depth: true, stencil: true },
  X32_TYPELESS_G8X24_UINT: { vk: null, bytes: 8, block: 1, depth: true, stencil: true },
  R10G10B10A2_TYPELESS: { vk: "VK_FORMAT_A2B10G10R10_UNORM_PACK32", bytes: 4, block: 1, depth: false, stencil: false },
  R10G10B10A2_UNORM: { vk: "VK_FORMAT_A2B10G10R10_UNORM_PACK32", bytes: 4, block: 1, depth: false, stencil: false },
  R10G10B10A2_UINT: { vk: "VK_FORMAT_A2B10G10R10_UINT_PACK32", bytes: 4, block: 1, depth: false, stencil: false },
  R11G11B10_FLOAT: { vk: "VK_FORMAT_B10G11R11_UFLOAT_PACK32", bytes: 4, block: 1, depth: false, stencil: false },
  R8G8B8A8_TYPELESS: { vk: "VK_FORMAT_R8G8B8A8_UNORM", bytes: 4, block: 1, depth: false, stencil: false },
  R8G8B8A8_UNORM: { vk: "VK_FORMAT_R8G8B8A8_UNORM", bytes: 4, block: 1, depth: false, stencil: false },
  R8G8B8A8_UNORM_SRGB: { vk: "VK_FORMAT_R8G8B8A8_SRGB", bytes: 4, block: 1, depth: false, stencil: false },
  R8G8B8A8_UINT: { vk: "VK_FORMAT_R8G8B8A8_UINT", bytes: 4, block: 1, depth: false, stencil: false },
  R8G8B8A8_SNORM: { vk: "VK_FORMAT_R8G8B8A8_SNORM", bytes: 4, block: 1, depth: false, stencil: false },
  R8G8B8A8_SINT: { vk: "VK_FORMAT_R8G8B8A8_SINT", bytes: 4, block: 1, depth: false, stencil: false },
  R16G16_TYPELESS: { vk: "VK_FORMAT_R16G16_UNORM", bytes: 4, block: 1, depth: false, stencil: false },
  R16G16_FLOAT: { vk: "VK_FORMAT_R16G16_SFLOAT", bytes: 4, block: 1, depth: false, stencil: false },
  R16G16_UNORM: { vk: "VK_FORMAT_R16G16_UNORM", bytes: 4, block: 1, depth: false, stencil: false },
  R16G16_UINT: { vk: "VK_FORMAT_R16G16_UINT", bytes: 4, block: 1, depth: false, stencil: false },
  R16G16_SNORM: { vk: "VK_FORMAT_R16G16_SNORM", bytes: 4, block: 1, depth: false, stencil: false },
  R16G16_SINT: { vk: "VK_FORMAT_R16G16_SINT", bytes: 4, block: 1, depth: false, stencil: false },
  R32_TYPELESS: { vk: "VK_FORMAT_R32_SFLOAT", bytes: 4, block: 1, depth: false, stencil: false },
  D32_FLOAT: { vk: "VK_FORMAT_D32_SFLOAT", bytes: 4, block: 1, depth: true, stencil: false },
  R32_FLOAT: { vk: "VK_FORMAT_R32_SFLOAT", bytes: 4, block: 1, depth: false, stencil: false },
  R32_UINT: { vk: "VK_FORMAT_R32_UINT", bytes: 4, block: 1, depth: false, stencil: false },
  R32_SINT: { vk: "VK_FORMAT_R32_SINT", bytes: 4, block: 1, depth: false, stencil: false },
  R24G8_TYPELESS: { vk: "VK_FORMAT_D24_UNORM_S8_UINT", bytes: 4, block: 1, depth: true, stencil: true },
  D24_UNORM_S8_UINT: { vk: "VK_FORMAT_D24_UNORM_S8_UINT", bytes: 4, block: 1, depth: true, stencil: true },
  R24_UNORM_X8_TYPELESS: { vk: "VK_FORMAT_D24_UNORM_S8_UINT", bytes: 4, block: 1, depth: true, stencil: false },
  X24_TYPELESS_G8_UINT: { vk: null, bytes: 4, block: 1, depth: false, stencil: true },
  R8G8_TYPELESS: { vk: "VK_FORMAT_R8G8_UNORM", bytes: 2, block: 1, depth: false, stencil: false },
  R8G8_UNORM: { vk: "VK_FORMAT_R8G8_UNORM", bytes: 2, block: 1, depth: false, stencil: false },
  R8G8_UINT: { vk: "VK_FORMAT_R8G8_UINT", bytes: 2, block: 1, depth: false, stencil: false },
  R8G8_SNORM: { vk: "VK_FORMAT_R8G8_SNORM", bytes: 2, block: 1, depth: false, stencil: false },
  R8G8_SINT: { vk: "VK_FORMAT_R8G8_SINT", bytes: 2, block: 1, depth: false, stencil: false },
  R16_TYPELESS: { vk: "VK_FORMAT_R16_UNORM", bytes: 2, block: 1, depth: false, stencil: false },
  R16_FLOAT: { vk: "VK_FORMAT_R16_SFLOAT", bytes: 2, block: 1, depth: false, stencil: false },
  D16_UNORM: { vk: "VK_FORMAT_D16_UNORM", bytes: 2, block: 1, depth: true, stencil: false },
  R16_UNORM: { vk: "VK_FORMAT_R16_UNORM", bytes: 2, block: 1, depth: false, stencil: false },
  R16_UINT: { vk: "VK_FORMAT_R16_UINT", bytes: 2, block: 1, depth: false, stencil: false },
  R16_SNORM: { vk: "VK_FORMAT_R16_SNORM", bytes: 2, block: 1, depth: false, stencil: false },
  R16_SINT: { vk: "VK_FORMAT_R16_SINT", bytes: 2, block: 1, depth: false, stencil: false },
  R8_TYPELESS: { vk: "VK_FORMAT_R8_UNORM", bytes: 1, block: 1, depth: false, stencil: false },
  R8_UNORM: { vk: "VK_FORMAT_R8_UNORM", bytes: 1, block: 1, depth: false, stencil: false },
  R8_UINT: { vk: "VK_FORMAT_R8_UINT", bytes: 1, block: 1, depth: false, stencil: false },
  R8_SNORM: { vk: "VK_FORMAT_R8_SNORM", bytes: 1, block: 1, depth: false, stencil: false },
  R8_SINT: { vk: "VK_FORMAT_R8_SINT", bytes: 1, block: 1, depth: false, stencil: false },
  A8_UNORM: { vk: "VK_FORMAT_A8_UNORM_KHR", bytes: 1, block: 1, depth: false, stencil: false },
  R1_UNORM: { vk: null, bytes: 1, block: 8, depth: false, stencil: false },
  R9G9B9E5_SHAREDEXP: { vk: "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32", bytes: 4, block: 1, depth: false, stencil: false },
  R8G8_B8G8_UNORM: { vk: null, bytes: 4, block: 1, depth: false, stencil: false },
  G8R8_G8B8_UNORM: { vk: null, bytes: 4, block: 1, depth: false, stencil: false },
  BC1_TYPELESS: { vk: "VK_FORMAT_BC1_RGBA_UNORM_BLOCK", bytes: 8, block: 4, depth: false, stencil: false },
  BC1_UNORM: { vk: "VK_FORMAT_BC1_RGBA_UNORM_BLOCK", bytes: 8, block: 4, depth: false, stencil: false },
  BC1_UNORM_SRGB: { vk: "VK_FORMAT_BC1_RGBA_SRGB_BLOCK", bytes: 8, block: 4, depth: false, stencil: false },
  BC2_TYPELESS: { vk: "VK_FORMAT_BC2_UNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC2_UNORM: { vk: "VK_FORMAT_BC2_UNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC2_UNORM_SRGB: { vk: "VK_FORMAT_BC2_SRGB_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC3_TYPELESS: { vk: "VK_FORMAT_BC3_UNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC3_UNORM: { vk: "VK_FORMAT_BC3_UNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC3_UNORM_SRGB: { vk: "VK_FORMAT_BC3_SRGB_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC4_TYPELESS: { vk: "VK_FORMAT_BC4_UNORM_BLOCK", bytes: 8, block: 4, depth: false, stencil: false },
  BC4_UNORM: { vk: "VK_FORMAT_BC4_UNORM_BLOCK", bytes: 8, block: 4, depth: false, stencil: false },
  BC4_SNORM: { vk: "VK_FORMAT_BC4_SNORM_BLOCK", bytes: 8, block: 4, depth: false, stencil: false },
  BC5_TYPELESS: { vk: "VK_FORMAT_BC5_UNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC5_UNORM: { vk: "VK_FORMAT_BC5_UNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC5_SNORM: { vk: "VK_FORMAT_BC5_SNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  B5G6R5_UNORM: { vk: "VK_FORMAT_B5G6R5_UNORM_PACK16", bytes: 2, block: 1, depth: false, stencil: false },
  B5G5R5A1_UNORM: { vk: "VK_FORMAT_A1R5G5B5_UNORM_PACK16", bytes: 2, block: 1, depth: false, stencil: false },
  B8G8R8A8_UNORM: { vk: "VK_FORMAT_B8G8R8A8_UNORM", bytes: 4, block: 1, depth: false, stencil: false },
  B8G8R8X8_UNORM: { vk: "VK_FORMAT_B8G8R8A8_UNORM", bytes: 4, block: 1, depth: false, stencil: false },
  R10G10B10_XR_BIAS_A2_UNORM: { vk: null, bytes: 4, block: 1, depth: false, stencil: false },
  B8G8R8A8_TYPELESS: { vk: "VK_FORMAT_B8G8R8A8_UNORM", bytes: 4, block: 1, depth: false, stencil: false },
  B8G8R8A8_UNORM_SRGB: { vk: "VK_FORMAT_B8G8R8A8_SRGB", bytes: 4, block: 1, depth: false, stencil: false },
  B8G8R8X8_TYPELESS: { vk: "VK_FORMAT_B8G8R8A8_UNORM", bytes: 4, block: 1, depth: false, stencil: false },
  B8G8R8X8_UNORM_SRGB: { vk: "VK_FORMAT_B8G8R8A8_SRGB", bytes: 4, block: 1, depth: false, stencil: false },
  BC6H_TYPELESS: { vk: "VK_FORMAT_BC6H_UFLOAT_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC6H_UF16: { vk: "VK_FORMAT_BC6H_UFLOAT_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC6H_SF16: { vk: "VK_FORMAT_BC6H_SFLOAT_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC7_TYPELESS: { vk: "VK_FORMAT_BC7_UNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC7_UNORM: { vk: "VK_FORMAT_BC7_UNORM_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  BC7_UNORM_SRGB: { vk: "VK_FORMAT_BC7_SRGB_BLOCK", bytes: 16, block: 4, depth: false, stencil: false },
  B4G4R4A4_UNORM: { vk: "VK_FORMAT_A4R4G4B4_UNORM_PACK16", bytes: 2, block: 1, depth: false, stencil: false },
};

/** "DXGI_FORMAT_R8G8B8A8_UNORM" or "R8G8B8A8_UNORM" -> the table key. */
function keyOf(name: string): string {
  return name.startsWith("DXGI_FORMAT_") ? name.substring("DXGI_FORMAT_".length) : name;
}

function entry(name: string | null | undefined): Entry | null {
  if (!name) return null;
  return TABLE[keyOf(name)] ?? null;
}

/** The VK_FORMAT_* spelling of a DXGI format's memory layout, or null for a format with none. */
export function vkFormatOfDxgi(name: string | null | undefined): string | null {
  return entry(name)?.vk ?? null;
}

/** Bytes per pixel, or per 4x4 block for a block-compressed format; 0 for an unknown format. */
export function dxgiFormatBytes(name: string | null | undefined): number {
  return entry(name)?.bytes ?? 0;
}

export function dxgiFormatIsDepth(name: string | null | undefined): boolean {
  return entry(name)?.depth ?? false;
}

export function dxgiFormatIsStencil(name: string | null | undefined): boolean {
  return entry(name)?.stencil ?? false;
}

export function dxgiFormatIsBlockCompressed(name: string | null | undefined): boolean {
  const e = entry(name);
  return !!e && e.block === 4;
}

/** Bytes of one mip level of a texture in the format: rows of blocks (or pixels) times their size. */
export function dxgiMipBytes(name: string | null | undefined, width: number, height: number): number {
  const e = entry(name);
  if (!e) return 0;
  const block = e.block === 8 ? 1 : e.block;   // R1_UNORM packs 8 pixels per byte
  const w = Math.ceil(Math.max(1, width) / block);
  const h = Math.ceil(Math.max(1, height) / block);
  return e.block === 8 ? Math.ceil(w / 8) * h : w * h * e.bytes;
}

/** "DXGI_FORMAT_R8G8B8A8_UNORM" -> "R8G8B8A8_UNORM". */
export function dxgiFormatShort(name: string | null | undefined): string {
  return name ? keyOf(name) : "";
}
