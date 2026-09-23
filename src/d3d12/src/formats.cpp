#include "formats.h"

#include "d3d12_enums.gen.h"

#include <cstdio>

namespace dxinsp
{

namespace
{

struct Entry
{
    DXGI_FORMAT format;
    const char* protocolName;   // nullptr: not decodable by the UI
    uint32_t bytes;
    uint32_t block;             // 1, or 4 for BC formats
    bool depth;
    bool stencil;
};

// Every format with a memory layout the UI's decoders read (src/app/src/renderer/vulkan/texture_decode.ts),
// plus the layouts of the ones they do not, so a read-back still knows its size. Typeless formats
// take the layout of their family; TypedFormat() picks the spelling the data is decoded as.
const Entry kFormats[] = {
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, "VK_FORMAT_R32G32B32A32_SFLOAT", 16, 1, false, false},
    {DXGI_FORMAT_R32G32B32A32_FLOAT, "VK_FORMAT_R32G32B32A32_SFLOAT", 16, 1, false, false},
    {DXGI_FORMAT_R32G32B32A32_UINT, "VK_FORMAT_R32G32B32A32_UINT", 16, 1, false, false},
    {DXGI_FORMAT_R32G32B32A32_SINT, "VK_FORMAT_R32G32B32A32_SINT", 16, 1, false, false},
    {DXGI_FORMAT_R32G32B32_TYPELESS, "VK_FORMAT_R32G32B32_SFLOAT", 12, 1, false, false},
    {DXGI_FORMAT_R32G32B32_FLOAT, "VK_FORMAT_R32G32B32_SFLOAT", 12, 1, false, false},
    {DXGI_FORMAT_R32G32B32_UINT, "VK_FORMAT_R32G32B32_UINT", 12, 1, false, false},
    {DXGI_FORMAT_R32G32B32_SINT, "VK_FORMAT_R32G32B32_SINT", 12, 1, false, false},
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, "VK_FORMAT_R16G16B16A16_UNORM", 8, 1, false, false},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, "VK_FORMAT_R16G16B16A16_SFLOAT", 8, 1, false, false},
    {DXGI_FORMAT_R16G16B16A16_UNORM, "VK_FORMAT_R16G16B16A16_UNORM", 8, 1, false, false},
    {DXGI_FORMAT_R16G16B16A16_UINT, "VK_FORMAT_R16G16B16A16_UINT", 8, 1, false, false},
    {DXGI_FORMAT_R16G16B16A16_SNORM, "VK_FORMAT_R16G16B16A16_SNORM", 8, 1, false, false},
    {DXGI_FORMAT_R16G16B16A16_SINT, "VK_FORMAT_R16G16B16A16_SINT", 8, 1, false, false},
    {DXGI_FORMAT_R32G32_TYPELESS, "VK_FORMAT_R32G32_SFLOAT", 8, 1, false, false},
    {DXGI_FORMAT_R32G32_FLOAT, "VK_FORMAT_R32G32_SFLOAT", 8, 1, false, false},
    {DXGI_FORMAT_R32G32_UINT, "VK_FORMAT_R32G32_UINT", 8, 1, false, false},
    {DXGI_FORMAT_R32G32_SINT, "VK_FORMAT_R32G32_SINT", 8, 1, false, false},
    {DXGI_FORMAT_R32G8X24_TYPELESS, "VK_FORMAT_D32_SFLOAT_S8_UINT", 8, 1, true, true},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "VK_FORMAT_D32_SFLOAT_S8_UINT", 8, 1, true, true},
    {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, "VK_FORMAT_D32_SFLOAT_S8_UINT", 8, 1, true, true},
    {DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, nullptr, 8, 1, true, true},
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, "VK_FORMAT_A2B10G10R10_UNORM_PACK32", 4, 1, false, false},
    {DXGI_FORMAT_R10G10B10A2_UNORM, "VK_FORMAT_A2B10G10R10_UNORM_PACK32", 4, 1, false, false},
    {DXGI_FORMAT_R10G10B10A2_UINT, "VK_FORMAT_A2B10G10R10_UINT_PACK32", 4, 1, false, false},
    {DXGI_FORMAT_R11G11B10_FLOAT, "VK_FORMAT_B10G11R11_UFLOAT_PACK32", 4, 1, false, false},
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, "VK_FORMAT_R8G8B8A8_UNORM", 4, 1, false, false},
    {DXGI_FORMAT_R8G8B8A8_UNORM, "VK_FORMAT_R8G8B8A8_UNORM", 4, 1, false, false},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "VK_FORMAT_R8G8B8A8_SRGB", 4, 1, false, false},
    {DXGI_FORMAT_R8G8B8A8_UINT, "VK_FORMAT_R8G8B8A8_UINT", 4, 1, false, false},
    {DXGI_FORMAT_R8G8B8A8_SNORM, "VK_FORMAT_R8G8B8A8_SNORM", 4, 1, false, false},
    {DXGI_FORMAT_R8G8B8A8_SINT, "VK_FORMAT_R8G8B8A8_SINT", 4, 1, false, false},
    {DXGI_FORMAT_R16G16_TYPELESS, "VK_FORMAT_R16G16_UNORM", 4, 1, false, false},
    {DXGI_FORMAT_R16G16_FLOAT, "VK_FORMAT_R16G16_SFLOAT", 4, 1, false, false},
    {DXGI_FORMAT_R16G16_UNORM, "VK_FORMAT_R16G16_UNORM", 4, 1, false, false},
    {DXGI_FORMAT_R16G16_UINT, "VK_FORMAT_R16G16_UINT", 4, 1, false, false},
    {DXGI_FORMAT_R16G16_SNORM, "VK_FORMAT_R16G16_SNORM", 4, 1, false, false},
    {DXGI_FORMAT_R16G16_SINT, "VK_FORMAT_R16G16_SINT", 4, 1, false, false},
    {DXGI_FORMAT_R32_TYPELESS, "VK_FORMAT_R32_SFLOAT", 4, 1, false, false},
    {DXGI_FORMAT_D32_FLOAT, "VK_FORMAT_D32_SFLOAT", 4, 1, true, false},
    {DXGI_FORMAT_R32_FLOAT, "VK_FORMAT_R32_SFLOAT", 4, 1, false, false},
    {DXGI_FORMAT_R32_UINT, "VK_FORMAT_R32_UINT", 4, 1, false, false},
    {DXGI_FORMAT_R32_SINT, "VK_FORMAT_R32_SINT", 4, 1, false, false},
    {DXGI_FORMAT_R24G8_TYPELESS, "VK_FORMAT_D24_UNORM_S8_UINT", 4, 1, true, true},
    {DXGI_FORMAT_D24_UNORM_S8_UINT, "VK_FORMAT_D24_UNORM_S8_UINT", 4, 1, true, true},
    {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, "VK_FORMAT_D24_UNORM_S8_UINT", 4, 1, true, false},
    {DXGI_FORMAT_X24_TYPELESS_G8_UINT, nullptr, 4, 1, false, true},
    {DXGI_FORMAT_R8G8_TYPELESS, "VK_FORMAT_R8G8_UNORM", 2, 1, false, false},
    {DXGI_FORMAT_R8G8_UNORM, "VK_FORMAT_R8G8_UNORM", 2, 1, false, false},
    {DXGI_FORMAT_R8G8_UINT, "VK_FORMAT_R8G8_UINT", 2, 1, false, false},
    {DXGI_FORMAT_R8G8_SNORM, "VK_FORMAT_R8G8_SNORM", 2, 1, false, false},
    {DXGI_FORMAT_R8G8_SINT, "VK_FORMAT_R8G8_SINT", 2, 1, false, false},
    {DXGI_FORMAT_R16_TYPELESS, "VK_FORMAT_R16_UNORM", 2, 1, false, false},
    {DXGI_FORMAT_R16_FLOAT, "VK_FORMAT_R16_SFLOAT", 2, 1, false, false},
    {DXGI_FORMAT_D16_UNORM, "VK_FORMAT_D16_UNORM", 2, 1, true, false},
    {DXGI_FORMAT_R16_UNORM, "VK_FORMAT_R16_UNORM", 2, 1, false, false},
    {DXGI_FORMAT_R16_UINT, "VK_FORMAT_R16_UINT", 2, 1, false, false},
    {DXGI_FORMAT_R16_SNORM, "VK_FORMAT_R16_SNORM", 2, 1, false, false},
    {DXGI_FORMAT_R16_SINT, "VK_FORMAT_R16_SINT", 2, 1, false, false},
    {DXGI_FORMAT_R8_TYPELESS, "VK_FORMAT_R8_UNORM", 1, 1, false, false},
    {DXGI_FORMAT_R8_UNORM, "VK_FORMAT_R8_UNORM", 1, 1, false, false},
    {DXGI_FORMAT_R8_UINT, "VK_FORMAT_R8_UINT", 1, 1, false, false},
    {DXGI_FORMAT_R8_SNORM, "VK_FORMAT_R8_SNORM", 1, 1, false, false},
    {DXGI_FORMAT_R8_SINT, "VK_FORMAT_R8_SINT", 1, 1, false, false},
    {DXGI_FORMAT_A8_UNORM, "VK_FORMAT_A8_UNORM_KHR", 1, 1, false, false},
    {DXGI_FORMAT_R1_UNORM, nullptr, 1, 8, false, false},
    {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32", 4, 1, false, false},
    {DXGI_FORMAT_R8G8_B8G8_UNORM, nullptr, 4, 1, false, false},
    {DXGI_FORMAT_G8R8_G8B8_UNORM, nullptr, 4, 1, false, false},
    {DXGI_FORMAT_BC1_TYPELESS, "VK_FORMAT_BC1_RGBA_UNORM_BLOCK", 8, 4, false, false},
    {DXGI_FORMAT_BC1_UNORM, "VK_FORMAT_BC1_RGBA_UNORM_BLOCK", 8, 4, false, false},
    {DXGI_FORMAT_BC1_UNORM_SRGB, "VK_FORMAT_BC1_RGBA_SRGB_BLOCK", 8, 4, false, false},
    {DXGI_FORMAT_BC2_TYPELESS, "VK_FORMAT_BC2_UNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC2_UNORM, "VK_FORMAT_BC2_UNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC2_UNORM_SRGB, "VK_FORMAT_BC2_SRGB_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC3_TYPELESS, "VK_FORMAT_BC3_UNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC3_UNORM, "VK_FORMAT_BC3_UNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC3_UNORM_SRGB, "VK_FORMAT_BC3_SRGB_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC4_TYPELESS, "VK_FORMAT_BC4_UNORM_BLOCK", 8, 4, false, false},
    {DXGI_FORMAT_BC4_UNORM, "VK_FORMAT_BC4_UNORM_BLOCK", 8, 4, false, false},
    {DXGI_FORMAT_BC4_SNORM, "VK_FORMAT_BC4_SNORM_BLOCK", 8, 4, false, false},
    {DXGI_FORMAT_BC5_TYPELESS, "VK_FORMAT_BC5_UNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC5_UNORM, "VK_FORMAT_BC5_UNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC5_SNORM, "VK_FORMAT_BC5_SNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_B5G6R5_UNORM, "VK_FORMAT_B5G6R5_UNORM_PACK16", 2, 1, false, false},
    {DXGI_FORMAT_B5G5R5A1_UNORM, "VK_FORMAT_A1R5G5B5_UNORM_PACK16", 2, 1, false, false},
    {DXGI_FORMAT_B8G8R8A8_UNORM, "VK_FORMAT_B8G8R8A8_UNORM", 4, 1, false, false},
    {DXGI_FORMAT_B8G8R8X8_UNORM, "VK_FORMAT_B8G8R8A8_UNORM", 4, 1, false, false},
    {DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, nullptr, 4, 1, false, false},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, "VK_FORMAT_B8G8R8A8_UNORM", 4, 1, false, false},
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "VK_FORMAT_B8G8R8A8_SRGB", 4, 1, false, false},
    {DXGI_FORMAT_B8G8R8X8_TYPELESS, "VK_FORMAT_B8G8R8A8_UNORM", 4, 1, false, false},
    {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, "VK_FORMAT_B8G8R8A8_SRGB", 4, 1, false, false},
    {DXGI_FORMAT_BC6H_TYPELESS, "VK_FORMAT_BC6H_UFLOAT_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC6H_UF16, "VK_FORMAT_BC6H_UFLOAT_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC6H_SF16, "VK_FORMAT_BC6H_SFLOAT_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC7_TYPELESS, "VK_FORMAT_BC7_UNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC7_UNORM, "VK_FORMAT_BC7_UNORM_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_BC7_UNORM_SRGB, "VK_FORMAT_BC7_SRGB_BLOCK", 16, 4, false, false},
    {DXGI_FORMAT_B4G4R4A4_UNORM, "VK_FORMAT_A4R4G4B4_UNORM_PACK16", 2, 1, false, false},
};

const Entry* Lookup(DXGI_FORMAT format)
{
    for (const Entry& e : kFormats)
        if (e.format == format)
            return &e;
    return nullptr;
}

}  // namespace

FormatInfo FormatOf(DXGI_FORMAT format)
{
    FormatInfo info;
    const Entry* e = Lookup(format);
    if (!e)
        return info;
    info.protocolName = e->protocolName;
    info.bytes = e->bytes;
    info.blockWidth = info.blockHeight = e->block;
    info.depth = e->depth;
    info.stencil = e->stencil;
    info.compressed = e->block > 1 && format != DXGI_FORMAT_R1_UNORM;
    return info;
}

const char* FormatName(DXGI_FORMAT format)
{
    if (const char* name = ToString_DXGI_FORMAT((int64_t)format))
        return name;
    static thread_local char buf[32];
    snprintf(buf, sizeof(buf), "DXGI_FORMAT(%d)", (int)format);
    return buf;
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT format, bool asDepth)
{
    switch (format)
    {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS: return asDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16_TYPELESS: return asDepth ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
        default: return format;
    }
}

DXGI_FORMAT DepthCopyFormat(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        default: return format;
    }
}

uint64_t RowBytes(const FormatInfo& f, uint32_t width)
{
    if (!f.bytes)
        return 0;
    uint64_t blocks = (width + f.blockWidth - 1) / f.blockWidth;
    return blocks * f.bytes;
}

uint32_t RowCount(const FormatInfo& f, uint32_t height)
{
    return (height + f.blockHeight - 1) / f.blockHeight;
}

}  // namespace dxinsp
