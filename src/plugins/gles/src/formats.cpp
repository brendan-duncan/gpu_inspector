#include "formats.h"

#include "../gen/gles_constants.gen.h"

namespace glesinsp {

namespace {

FormatInfo Color(const char* vk, ReadClass read, bool srgb = false) {
    FormatInfo f;
    f.vk = vk;
    f.read = read;
    f.srgb = srgb;
    return f;
}

FormatInfo DepthStencil(const char* vk, bool depth, bool stencil) {
    FormatInfo f;
    f.vk = vk;
    f.depth = depth;
    f.stencil = stencil;
    return f;
}

FormatInfo Compressed(const char* vk, int bw, int bh, int bytes, bool srgb = false) {
    FormatInfo f;
    f.vk = vk;
    f.compressed = true;
    f.blockWidth = bw;
    f.blockHeight = bh;
    f.blockBytes = bytes;
    f.srgb = srgb;
    return f;
}

// GL_COMPRESSED_RGBA_ASTC_4x4_KHR (0x93B0) onwards, and the sRGB ones from 0x93D0, in this order.
const char* const kAstc[][2] = {
    {"VK_FORMAT_ASTC_4x4_UNORM_BLOCK", "VK_FORMAT_ASTC_4x4_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_5x4_UNORM_BLOCK", "VK_FORMAT_ASTC_5x4_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_5x5_UNORM_BLOCK", "VK_FORMAT_ASTC_5x5_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_6x5_UNORM_BLOCK", "VK_FORMAT_ASTC_6x5_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_6x6_UNORM_BLOCK", "VK_FORMAT_ASTC_6x6_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_8x5_UNORM_BLOCK", "VK_FORMAT_ASTC_8x5_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_8x6_UNORM_BLOCK", "VK_FORMAT_ASTC_8x6_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_8x8_UNORM_BLOCK", "VK_FORMAT_ASTC_8x8_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_10x5_UNORM_BLOCK", "VK_FORMAT_ASTC_10x5_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_10x6_UNORM_BLOCK", "VK_FORMAT_ASTC_10x6_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_10x8_UNORM_BLOCK", "VK_FORMAT_ASTC_10x8_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_10x10_UNORM_BLOCK", "VK_FORMAT_ASTC_10x10_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_12x10_UNORM_BLOCK", "VK_FORMAT_ASTC_12x10_SRGB_BLOCK"},
    {"VK_FORMAT_ASTC_12x12_UNORM_BLOCK", "VK_FORMAT_ASTC_12x12_SRGB_BLOCK"},
};
const int kAstcBlocks[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

}  // namespace

FormatInfo FormatOf(GLenum f) {
    switch (f) {
        // Normalized color.
        case GL_RGBA8: return Color("VK_FORMAT_R8G8B8A8_UNORM", ReadClass::Unorm);
        case GL_RGB8: return Color("VK_FORMAT_R8G8B8_UNORM", ReadClass::Unorm);
        case GL_RG8: return Color("VK_FORMAT_R8G8_UNORM", ReadClass::Unorm);
        case GL_R8: return Color("VK_FORMAT_R8_UNORM", ReadClass::Unorm);
        case GL_SRGB8_ALPHA8: return Color("VK_FORMAT_R8G8B8A8_SRGB", ReadClass::Unorm, true);
        case GL_SRGB8: return Color("VK_FORMAT_R8G8B8_SRGB", ReadClass::Unorm, true);
        case GL_RGB565: return Color("VK_FORMAT_R5G6B5_UNORM_PACK16", ReadClass::Unorm);
        case GL_RGBA4: return Color("VK_FORMAT_R4G4B4A4_UNORM_PACK16", ReadClass::Unorm);
        case GL_RGB5_A1: return Color("VK_FORMAT_R5G5B5A1_UNORM_PACK16", ReadClass::Unorm);
        case GL_RGB10_A2: return Color("VK_FORMAT_A2B10G10R10_UNORM_PACK32", ReadClass::Unorm);
        case GL_BGRA8_EXT: return Color("VK_FORMAT_B8G8R8A8_UNORM", ReadClass::Unorm);
        case GL_R8_SNORM: return Color("VK_FORMAT_R8_SNORM", ReadClass::None);
        case GL_RG8_SNORM: return Color("VK_FORMAT_R8G8_SNORM", ReadClass::None);
        case GL_RGB8_SNORM: return Color("VK_FORMAT_R8G8B8_SNORM", ReadClass::None);
        case GL_RGBA8_SNORM: return Color("VK_FORMAT_R8G8B8A8_SNORM", ReadClass::None);
        // Float.
        case GL_R16F: return Color("VK_FORMAT_R16_SFLOAT", ReadClass::Float);
        case GL_RG16F: return Color("VK_FORMAT_R16G16_SFLOAT", ReadClass::Float);
        case GL_RGB16F: return Color("VK_FORMAT_R16G16B16_SFLOAT", ReadClass::Float);
        case GL_RGBA16F: return Color("VK_FORMAT_R16G16B16A16_SFLOAT", ReadClass::Float);
        case GL_R32F: return Color("VK_FORMAT_R32_SFLOAT", ReadClass::Float);
        case GL_RG32F: return Color("VK_FORMAT_R32G32_SFLOAT", ReadClass::Float);
        case GL_RGB32F: return Color("VK_FORMAT_R32G32B32_SFLOAT", ReadClass::Float);
        case GL_RGBA32F: return Color("VK_FORMAT_R32G32B32A32_SFLOAT", ReadClass::Float);
        case GL_R11F_G11F_B10F: return Color("VK_FORMAT_B10G11R11_UFLOAT_PACK32", ReadClass::Float);
        case GL_RGB9_E5: return Color("VK_FORMAT_E5B9G9R9_UFLOAT_PACK32", ReadClass::None);
        // Integer.
        case GL_R8I: return Color("VK_FORMAT_R8_SINT", ReadClass::Int);
        case GL_RG8I: return Color("VK_FORMAT_R8G8_SINT", ReadClass::Int);
        case GL_RGBA8I: return Color("VK_FORMAT_R8G8B8A8_SINT", ReadClass::Int);
        case GL_R16I: return Color("VK_FORMAT_R16_SINT", ReadClass::Int);
        case GL_RG16I: return Color("VK_FORMAT_R16G16_SINT", ReadClass::Int);
        case GL_RGBA16I: return Color("VK_FORMAT_R16G16B16A16_SINT", ReadClass::Int);
        case GL_R32I: return Color("VK_FORMAT_R32_SINT", ReadClass::Int);
        case GL_RG32I: return Color("VK_FORMAT_R32G32_SINT", ReadClass::Int);
        case GL_RGBA32I: return Color("VK_FORMAT_R32G32B32A32_SINT", ReadClass::Int);
        case GL_R8UI: return Color("VK_FORMAT_R8_UINT", ReadClass::Uint);
        case GL_RG8UI: return Color("VK_FORMAT_R8G8_UINT", ReadClass::Uint);
        case GL_RGBA8UI: return Color("VK_FORMAT_R8G8B8A8_UINT", ReadClass::Uint);
        case GL_R16UI: return Color("VK_FORMAT_R16_UINT", ReadClass::Uint);
        case GL_RG16UI: return Color("VK_FORMAT_R16G16_UINT", ReadClass::Uint);
        case GL_RGBA16UI: return Color("VK_FORMAT_R16G16B16A16_UINT", ReadClass::Uint);
        case GL_R32UI: return Color("VK_FORMAT_R32_UINT", ReadClass::Uint);
        case GL_RG32UI: return Color("VK_FORMAT_R32G32_UINT", ReadClass::Uint);
        case GL_RGBA32UI: return Color("VK_FORMAT_R32G32B32A32_UINT", ReadClass::Uint);
        case GL_RGB10_A2UI: return Color("VK_FORMAT_A2B10G10R10_UINT_PACK32", ReadClass::Uint);
        // Depth and stencil: OpenGL ES cannot glReadPixels them.
        case GL_DEPTH_COMPONENT16: return DepthStencil("VK_FORMAT_D16_UNORM", true, false);
        case GL_DEPTH_COMPONENT24: return DepthStencil("VK_FORMAT_X8_D24_UNORM_PACK32", true, false);
        case GL_DEPTH_COMPONENT32F: return DepthStencil("VK_FORMAT_D32_SFLOAT", true, false);
        case GL_DEPTH24_STENCIL8: return DepthStencil("VK_FORMAT_D24_UNORM_S8_UINT", true, true);
        case GL_DEPTH32F_STENCIL8: return DepthStencil("VK_FORMAT_D32_SFLOAT_S8_UINT", true, true);
        case GL_STENCIL_INDEX8: return DepthStencil("VK_FORMAT_S8_UINT", false, true);
        // ETC and EAC.
        case GL_ETC1_RGB8_OES: return Compressed("VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK", 4, 4, 8);
        case GL_COMPRESSED_RGB8_ETC2: return Compressed("VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK", 4, 4, 8);
        case GL_COMPRESSED_SRGB8_ETC2: return Compressed("VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK", 4, 4, 8, true);
        case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2: return Compressed("VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK", 4, 4, 8);
        case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2: return Compressed("VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK", 4, 4, 8, true);
        case GL_COMPRESSED_RGBA8_ETC2_EAC: return Compressed("VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK", 4, 4, 16);
        case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC: return Compressed("VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK", 4, 4, 16, true);
        case GL_COMPRESSED_R11_EAC: return Compressed("VK_FORMAT_EAC_R11_UNORM_BLOCK", 4, 4, 8);
        case GL_COMPRESSED_SIGNED_R11_EAC: return Compressed("VK_FORMAT_EAC_R11_SNORM_BLOCK", 4, 4, 8);
        case GL_COMPRESSED_RG11_EAC: return Compressed("VK_FORMAT_EAC_R11G11_UNORM_BLOCK", 4, 4, 16);
        case GL_COMPRESSED_SIGNED_RG11_EAC: return Compressed("VK_FORMAT_EAC_R11G11_SNORM_BLOCK", 4, 4, 16);
        // S3TC / BPTC / RGTC (desktop GPUs, and ANGLE on Windows).
        case 0x83F0: return Compressed("VK_FORMAT_BC1_RGB_UNORM_BLOCK", 4, 4, 8);    // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
        case 0x83F1: return Compressed("VK_FORMAT_BC1_RGBA_UNORM_BLOCK", 4, 4, 8);   // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
        case 0x83F2: return Compressed("VK_FORMAT_BC2_UNORM_BLOCK", 4, 4, 16);       // DXT3
        case 0x83F3: return Compressed("VK_FORMAT_BC3_UNORM_BLOCK", 4, 4, 16);       // DXT5
        case 0x8C4C: return Compressed("VK_FORMAT_BC1_RGB_SRGB_BLOCK", 4, 4, 8, true);
        case 0x8C4D: return Compressed("VK_FORMAT_BC1_RGBA_SRGB_BLOCK", 4, 4, 8, true);
        case 0x8C4E: return Compressed("VK_FORMAT_BC2_SRGB_BLOCK", 4, 4, 16, true);
        case 0x8C4F: return Compressed("VK_FORMAT_BC3_SRGB_BLOCK", 4, 4, 16, true);
        case 0x8E8C: return Compressed("VK_FORMAT_BC7_UNORM_BLOCK", 4, 4, 16);       // GL_COMPRESSED_RGBA_BPTC_UNORM_EXT
        case 0x8E8D: return Compressed("VK_FORMAT_BC7_SRGB_BLOCK", 4, 4, 16, true);
        case 0x8E8E: return Compressed("VK_FORMAT_BC6H_SFLOAT_BLOCK", 4, 4, 16);
        case 0x8E8F: return Compressed("VK_FORMAT_BC6H_UFLOAT_BLOCK", 4, 4, 16);
        case 0x8DBB: return Compressed("VK_FORMAT_BC4_UNORM_BLOCK", 4, 4, 8);        // GL_COMPRESSED_RED_RGTC1_EXT
        case 0x8DBC: return Compressed("VK_FORMAT_BC4_SNORM_BLOCK", 4, 4, 8);
        case 0x8DBD: return Compressed("VK_FORMAT_BC5_UNORM_BLOCK", 4, 4, 16);
        case 0x8DBE: return Compressed("VK_FORMAT_BC5_SNORM_BLOCK", 4, 4, 16);
        default: break;
    }
    if (f >= 0x93B0 && f <= 0x93BD) {
        const int i = (int)(f - 0x93B0);
        return Compressed(kAstc[i][0], kAstcBlocks[i][0], kAstcBlocks[i][1], 16);
    }
    if (f >= 0x93D0 && f <= 0x93DD) {
        const int i = (int)(f - 0x93D0);
        return Compressed(kAstc[i][1], kAstcBlocks[i][0], kAstcBlocks[i][1], 16, true);
    }
    return FormatInfo();
}

GLenum SizedFormat(GLenum internalFormat, GLenum format, GLenum type) {
    switch (internalFormat) {
        case GL_RGBA:
            switch (type) {
                case GL_UNSIGNED_SHORT_4_4_4_4: return GL_RGBA4;
                case GL_UNSIGNED_SHORT_5_5_5_1: return GL_RGB5_A1;
                case GL_FLOAT: return GL_RGBA32F;
                case GL_HALF_FLOAT:
                case GL_HALF_FLOAT_OES: return GL_RGBA16F;
                default: return GL_RGBA8;
            }
        case GL_RGB:
            switch (type) {
                case GL_UNSIGNED_SHORT_5_6_5: return GL_RGB565;
                case GL_FLOAT: return GL_RGB32F;
                case GL_HALF_FLOAT:
                case GL_HALF_FLOAT_OES: return GL_RGB16F;
                default: return GL_RGB8;
            }
        // Luminance and alpha read as the one or two channels they are; the viewer shows red (and green).
        case GL_LUMINANCE:
        case GL_ALPHA:
            return type == GL_FLOAT ? GL_R32F : (type == GL_HALF_FLOAT || type == GL_HALF_FLOAT_OES) ? GL_R16F : GL_R8;
        case GL_LUMINANCE_ALPHA:
            return type == GL_FLOAT ? GL_RG32F : (type == GL_HALF_FLOAT || type == GL_HALF_FLOAT_OES) ? GL_RG16F : GL_RG8;
        case GL_BGRA_EXT: return GL_BGRA8_EXT;
        case GL_RED: return type == GL_FLOAT ? GL_R32F : GL_R8;
        case GL_RG: return type == GL_FLOAT ? GL_RG32F : GL_RG8;
        case GL_DEPTH_COMPONENT: return type == GL_UNSIGNED_INT ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16;
        case GL_DEPTH_STENCIL: return GL_DEPTH24_STENCIL8;
        default: (void)format; return internalFormat;
    }
}

ReadFormat ReadFormatOf(const FormatInfo& f) {
    switch (f.read) {
        case ReadClass::Unorm: return {GL_RGBA, GL_UNSIGNED_BYTE, 4, f.srgb ? "VK_FORMAT_R8G8B8A8_SRGB" : "VK_FORMAT_R8G8B8A8_UNORM"};
        case ReadClass::Float: return {GL_RGBA, GL_FLOAT, 16, "VK_FORMAT_R32G32B32A32_SFLOAT"};
        case ReadClass::Int: return {GL_RGBA_INTEGER, GL_INT, 16, "VK_FORMAT_R32G32B32A32_SINT"};
        case ReadClass::Uint: return {GL_RGBA_INTEGER, GL_UNSIGNED_INT, 16, "VK_FORMAT_R32G32B32A32_UINT"};
        default: return {};
    }
}

size_t CompressedBytes(const FormatInfo& f, int width, int height) {
    if (!f.compressed) return 0;
    const size_t bx = (size_t)((width + f.blockWidth - 1) / f.blockWidth);
    const size_t by = (size_t)((height + f.blockHeight - 1) / f.blockHeight);
    return bx * by * (size_t)f.blockBytes;
}

}  // namespace glesinsp
