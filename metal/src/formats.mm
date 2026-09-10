// Generated from the SDK's MTLPixelFormat.h, then the canonical names filled in by hand.
// Regenerate when a new SDK adds formats; nothing here is derived at build time.
#include "formats.h"

namespace mtlinsp {

const char *PixelFormatEnumName(MTLPixelFormat format) {
    switch ((NSUInteger)format) {
        case 0: return "MTLPixelFormatInvalid";
        case 1: return "MTLPixelFormatA8Unorm";
        case 10: return "MTLPixelFormatR8Unorm";
        case 11: return "MTLPixelFormatR8Unorm_sRGB";
        case 12: return "MTLPixelFormatR8Snorm";
        case 13: return "MTLPixelFormatR8Uint";
        case 14: return "MTLPixelFormatR8Sint";
        case 20: return "MTLPixelFormatR16Unorm";
        case 22: return "MTLPixelFormatR16Snorm";
        case 23: return "MTLPixelFormatR16Uint";
        case 24: return "MTLPixelFormatR16Sint";
        case 25: return "MTLPixelFormatR16Float";
        case 30: return "MTLPixelFormatRG8Unorm";
        case 31: return "MTLPixelFormatRG8Unorm_sRGB";
        case 32: return "MTLPixelFormatRG8Snorm";
        case 33: return "MTLPixelFormatRG8Uint";
        case 34: return "MTLPixelFormatRG8Sint";
        case 40: return "MTLPixelFormatB5G6R5Unorm";
        case 41: return "MTLPixelFormatA1BGR5Unorm";
        case 42: return "MTLPixelFormatABGR4Unorm";
        case 43: return "MTLPixelFormatBGR5A1Unorm";
        case 53: return "MTLPixelFormatR32Uint";
        case 54: return "MTLPixelFormatR32Sint";
        case 55: return "MTLPixelFormatR32Float";
        case 60: return "MTLPixelFormatRG16Unorm";
        case 62: return "MTLPixelFormatRG16Snorm";
        case 63: return "MTLPixelFormatRG16Uint";
        case 64: return "MTLPixelFormatRG16Sint";
        case 65: return "MTLPixelFormatRG16Float";
        case 70: return "MTLPixelFormatRGBA8Unorm";
        case 71: return "MTLPixelFormatRGBA8Unorm_sRGB";
        case 72: return "MTLPixelFormatRGBA8Snorm";
        case 73: return "MTLPixelFormatRGBA8Uint";
        case 74: return "MTLPixelFormatRGBA8Sint";
        case 80: return "MTLPixelFormatBGRA8Unorm";
        case 81: return "MTLPixelFormatBGRA8Unorm_sRGB";
        case 90: return "MTLPixelFormatRGB10A2Unorm";
        case 91: return "MTLPixelFormatRGB10A2Uint";
        case 92: return "MTLPixelFormatRG11B10Float";
        case 93: return "MTLPixelFormatRGB9E5Float";
        case 94: return "MTLPixelFormatBGR10A2Unorm";
        case 103: return "MTLPixelFormatRG32Uint";
        case 104: return "MTLPixelFormatRG32Sint";
        case 105: return "MTLPixelFormatRG32Float";
        case 110: return "MTLPixelFormatRGBA16Unorm";
        case 112: return "MTLPixelFormatRGBA16Snorm";
        case 113: return "MTLPixelFormatRGBA16Uint";
        case 114: return "MTLPixelFormatRGBA16Sint";
        case 115: return "MTLPixelFormatRGBA16Float";
        case 123: return "MTLPixelFormatRGBA32Uint";
        case 124: return "MTLPixelFormatRGBA32Sint";
        case 125: return "MTLPixelFormatRGBA32Float";
        case 130: return "MTLPixelFormatBC1_RGBA";
        case 131: return "MTLPixelFormatBC1_RGBA_sRGB";
        case 132: return "MTLPixelFormatBC2_RGBA";
        case 133: return "MTLPixelFormatBC2_RGBA_sRGB";
        case 134: return "MTLPixelFormatBC3_RGBA";
        case 135: return "MTLPixelFormatBC3_RGBA_sRGB";
        case 140: return "MTLPixelFormatBC4_RUnorm";
        case 141: return "MTLPixelFormatBC4_RSnorm";
        case 142: return "MTLPixelFormatBC5_RGUnorm";
        case 143: return "MTLPixelFormatBC5_RGSnorm";
        case 150: return "MTLPixelFormatBC6H_RGBFloat";
        case 151: return "MTLPixelFormatBC6H_RGBUfloat";
        case 152: return "MTLPixelFormatBC7_RGBAUnorm";
        case 153: return "MTLPixelFormatBC7_RGBAUnorm_sRGB";
        case 160: return "MTLPixelFormatPVRTC_RGB_2BPP";
        case 161: return "MTLPixelFormatPVRTC_RGB_2BPP_sRGB";
        case 162: return "MTLPixelFormatPVRTC_RGB_4BPP";
        case 163: return "MTLPixelFormatPVRTC_RGB_4BPP_sRGB";
        case 164: return "MTLPixelFormatPVRTC_RGBA_2BPP";
        case 165: return "MTLPixelFormatPVRTC_RGBA_2BPP_sRGB";
        case 166: return "MTLPixelFormatPVRTC_RGBA_4BPP";
        case 167: return "MTLPixelFormatPVRTC_RGBA_4BPP_sRGB";
        case 170: return "MTLPixelFormatEAC_R11Unorm";
        case 172: return "MTLPixelFormatEAC_R11Snorm";
        case 174: return "MTLPixelFormatEAC_RG11Unorm";
        case 176: return "MTLPixelFormatEAC_RG11Snorm";
        case 178: return "MTLPixelFormatEAC_RGBA8";
        case 179: return "MTLPixelFormatEAC_RGBA8_sRGB";
        case 180: return "MTLPixelFormatETC2_RGB8";
        case 181: return "MTLPixelFormatETC2_RGB8_sRGB";
        case 182: return "MTLPixelFormatETC2_RGB8A1";
        case 183: return "MTLPixelFormatETC2_RGB8A1_sRGB";
        case 186: return "MTLPixelFormatASTC_4x4_sRGB";
        case 187: return "MTLPixelFormatASTC_5x4_sRGB";
        case 188: return "MTLPixelFormatASTC_5x5_sRGB";
        case 189: return "MTLPixelFormatASTC_6x5_sRGB";
        case 190: return "MTLPixelFormatASTC_6x6_sRGB";
        case 192: return "MTLPixelFormatASTC_8x5_sRGB";
        case 193: return "MTLPixelFormatASTC_8x6_sRGB";
        case 194: return "MTLPixelFormatASTC_8x8_sRGB";
        case 195: return "MTLPixelFormatASTC_10x5_sRGB";
        case 196: return "MTLPixelFormatASTC_10x6_sRGB";
        case 197: return "MTLPixelFormatASTC_10x8_sRGB";
        case 198: return "MTLPixelFormatASTC_10x10_sRGB";
        case 199: return "MTLPixelFormatASTC_12x10_sRGB";
        case 200: return "MTLPixelFormatASTC_12x12_sRGB";
        case 204: return "MTLPixelFormatASTC_4x4_LDR";
        case 205: return "MTLPixelFormatASTC_5x4_LDR";
        case 206: return "MTLPixelFormatASTC_5x5_LDR";
        case 207: return "MTLPixelFormatASTC_6x5_LDR";
        case 208: return "MTLPixelFormatASTC_6x6_LDR";
        case 210: return "MTLPixelFormatASTC_8x5_LDR";
        case 211: return "MTLPixelFormatASTC_8x6_LDR";
        case 212: return "MTLPixelFormatASTC_8x8_LDR";
        case 213: return "MTLPixelFormatASTC_10x5_LDR";
        case 214: return "MTLPixelFormatASTC_10x6_LDR";
        case 215: return "MTLPixelFormatASTC_10x8_LDR";
        case 216: return "MTLPixelFormatASTC_10x10_LDR";
        case 217: return "MTLPixelFormatASTC_12x10_LDR";
        case 218: return "MTLPixelFormatASTC_12x12_LDR";
        case 222: return "MTLPixelFormatASTC_4x4_HDR";
        case 223: return "MTLPixelFormatASTC_5x4_HDR";
        case 224: return "MTLPixelFormatASTC_5x5_HDR";
        case 225: return "MTLPixelFormatASTC_6x5_HDR";
        case 226: return "MTLPixelFormatASTC_6x6_HDR";
        case 228: return "MTLPixelFormatASTC_8x5_HDR";
        case 229: return "MTLPixelFormatASTC_8x6_HDR";
        case 230: return "MTLPixelFormatASTC_8x8_HDR";
        case 231: return "MTLPixelFormatASTC_10x5_HDR";
        case 232: return "MTLPixelFormatASTC_10x6_HDR";
        case 233: return "MTLPixelFormatASTC_10x8_HDR";
        case 234: return "MTLPixelFormatASTC_10x10_HDR";
        case 235: return "MTLPixelFormatASTC_12x10_HDR";
        case 236: return "MTLPixelFormatASTC_12x12_HDR";
        case 240: return "MTLPixelFormatGBGR422";
        case 241: return "MTLPixelFormatBGRG422";
        case 250: return "MTLPixelFormatDepth16Unorm";
        case 252: return "MTLPixelFormatDepth32Float";
        case 253: return "MTLPixelFormatStencil8";
        case 255: return "MTLPixelFormatDepth24Unorm_Stencil8";
        case 260: return "MTLPixelFormatDepth32Float_Stencil8";
        case 261: return "MTLPixelFormatX32_Stencil8";
        case 262: return "MTLPixelFormatX24_Stencil8";
        case 263: return "MTLPixelFormatUnspecialized";
        case 552: return "MTLPixelFormatBGRA10_XR";
        case 553: return "MTLPixelFormatBGRA10_XR_sRGB";
        case 554: return "MTLPixelFormatBGR10_XR";
        case 555: return "MTLPixelFormatBGR10_XR_sRGB";
        default: return "";
    }
}

PixelFormatInfo PixelFormatDetails(MTLPixelFormat format) {
    switch ((NSUInteger)format) {
        case 1: return {"VK_FORMAT_R8_UNORM", 1, 1, 1};
        case 10: return {"VK_FORMAT_R8_UNORM", 1, 1, 1};
        case 11: return {"VK_FORMAT_R8_SRGB", 1, 1, 1};
        case 12: return {"VK_FORMAT_R8_SNORM", 1, 1, 1};
        case 13: return {"VK_FORMAT_R8_UINT", 1, 1, 1};
        case 14: return {"VK_FORMAT_R8_SINT", 1, 1, 1};
        case 20: return {"VK_FORMAT_R16_UNORM", 1, 1, 2};
        case 22: return {"VK_FORMAT_R16_SNORM", 1, 1, 2};
        case 23: return {"VK_FORMAT_R16_UINT", 1, 1, 2};
        case 24: return {"VK_FORMAT_R16_SINT", 1, 1, 2};
        case 25: return {"VK_FORMAT_R16_SFLOAT", 1, 1, 2};
        case 30: return {"VK_FORMAT_R8G8_UNORM", 1, 1, 2};
        case 32: return {"VK_FORMAT_R8G8_SNORM", 1, 1, 2};
        case 33: return {"VK_FORMAT_R8G8_UINT", 1, 1, 2};
        case 34: return {"VK_FORMAT_R8G8_SINT", 1, 1, 2};
        case 40: return {"VK_FORMAT_R5G6B5_UNORM_PACK16", 1, 1, 2};
        case 53: return {"VK_FORMAT_R32_UINT", 1, 1, 4};
        case 54: return {"VK_FORMAT_R32_SINT", 1, 1, 4};
        case 55: return {"VK_FORMAT_R32_SFLOAT", 1, 1, 4};
        case 60: return {"VK_FORMAT_R16G16_UNORM", 1, 1, 4};
        case 62: return {"VK_FORMAT_R16G16_SNORM", 1, 1, 4};
        case 63: return {"VK_FORMAT_R16G16_UINT", 1, 1, 4};
        case 64: return {"VK_FORMAT_R16G16_SINT", 1, 1, 4};
        case 65: return {"VK_FORMAT_R16G16_SFLOAT", 1, 1, 4};
        case 70: return {"VK_FORMAT_R8G8B8A8_UNORM", 1, 1, 4};
        case 71: return {"VK_FORMAT_R8G8B8A8_SRGB", 1, 1, 4};
        case 72: return {"VK_FORMAT_R8G8B8A8_SNORM", 1, 1, 4};
        case 73: return {"VK_FORMAT_R8G8B8A8_UINT", 1, 1, 4};
        case 74: return {"VK_FORMAT_R8G8B8A8_SINT", 1, 1, 4};
        case 80: return {"VK_FORMAT_B8G8R8A8_UNORM", 1, 1, 4};
        case 81: return {"VK_FORMAT_B8G8R8A8_SRGB", 1, 1, 4};
        case 90: return {"VK_FORMAT_A2B10G10R10_UNORM_PACK32", 1, 1, 4};
        case 91: return {"VK_FORMAT_A2B10G10R10_UINT_PACK32", 1, 1, 4};
        case 92: return {"VK_FORMAT_B10G11R11_UFLOAT_PACK32", 1, 1, 4};
        case 93: return {"VK_FORMAT_E5B9G9R9_UFLOAT_PACK32", 1, 1, 4};
        case 94: return {"VK_FORMAT_A2R10G10B10_UNORM_PACK32", 1, 1, 4};
        case 103: return {"VK_FORMAT_R32G32_UINT", 1, 1, 8};
        case 104: return {"VK_FORMAT_R32G32_SINT", 1, 1, 8};
        case 105: return {"VK_FORMAT_R32G32_SFLOAT", 1, 1, 8};
        case 110: return {"VK_FORMAT_R16G16B16A16_UNORM", 1, 1, 8};
        case 112: return {"VK_FORMAT_R16G16B16A16_SNORM", 1, 1, 8};
        case 113: return {"VK_FORMAT_R16G16B16A16_UINT", 1, 1, 8};
        case 114: return {"VK_FORMAT_R16G16B16A16_SINT", 1, 1, 8};
        case 115: return {"VK_FORMAT_R16G16B16A16_SFLOAT", 1, 1, 8};
        case 123: return {"VK_FORMAT_R32G32B32A32_UINT", 1, 1, 16};
        case 124: return {"VK_FORMAT_R32G32B32A32_SINT", 1, 1, 16};
        case 125: return {"VK_FORMAT_R32G32B32A32_SFLOAT", 1, 1, 16};
        case 130: return {"VK_FORMAT_BC1_RGBA_UNORM_BLOCK", 4, 4, 8};
        case 131: return {"VK_FORMAT_BC1_RGBA_SRGB_BLOCK", 4, 4, 8};
        case 132: return {"VK_FORMAT_BC2_UNORM_BLOCK", 4, 4, 16};
        case 133: return {"VK_FORMAT_BC2_SRGB_BLOCK", 4, 4, 16};
        case 134: return {"VK_FORMAT_BC3_UNORM_BLOCK", 4, 4, 16};
        case 135: return {"VK_FORMAT_BC3_SRGB_BLOCK", 4, 4, 16};
        case 140: return {"VK_FORMAT_BC4_UNORM_BLOCK", 4, 4, 8};
        case 141: return {"VK_FORMAT_BC4_SNORM_BLOCK", 4, 4, 8};
        case 142: return {"VK_FORMAT_BC5_UNORM_BLOCK", 4, 4, 16};
        case 143: return {"VK_FORMAT_BC5_SNORM_BLOCK", 4, 4, 16};
        case 150: return {"VK_FORMAT_BC6H_SFLOAT_BLOCK", 4, 4, 16};
        case 151: return {"VK_FORMAT_BC6H_UFLOAT_BLOCK", 4, 4, 16};
        case 152: return {"VK_FORMAT_BC7_UNORM_BLOCK", 4, 4, 16};
        case 153: return {"VK_FORMAT_BC7_SRGB_BLOCK", 4, 4, 16};
        case 250: return {"VK_FORMAT_D16_UNORM", 1, 1, 2};
        case 252: return {"VK_FORMAT_D32_SFLOAT", 1, 1, 4};
        case 253: return {"VK_FORMAT_S8_UINT", 1, 1, 1};
        case 255: return {"VK_FORMAT_D24_UNORM_S8_UINT", 1, 1, 4};
        case 260: return {"VK_FORMAT_D32_SFLOAT_S8_UINT", 1, 1, 8};
        default: return {"", 0, 0, 0};
    }
}

const char *TextureTypeEnumName(MTLTextureType type) {
    switch (type) {
        case MTLTextureType1D:                 return "MTLTextureType1D";
        case MTLTextureType1DArray:            return "MTLTextureType1DArray";
        case MTLTextureType2D:                 return "MTLTextureType2D";
        case MTLTextureType2DArray:            return "MTLTextureType2DArray";
        case MTLTextureType2DMultisample:      return "MTLTextureType2DMultisample";
        case MTLTextureType2DMultisampleArray: return "MTLTextureType2DMultisampleArray";
        case MTLTextureTypeCube:               return "MTLTextureTypeCube";
        case MTLTextureTypeCubeArray:          return "MTLTextureTypeCubeArray";
        case MTLTextureType3D:                 return "MTLTextureType3D";
        case MTLTextureTypeTextureBuffer:      return "MTLTextureTypeTextureBuffer";
        default:                               return "";
    }
}

const char *StorageModeEnumName(MTLStorageMode mode) {
    switch (mode) {
        case MTLStorageModeShared:     return "MTLStorageModeShared";
        case MTLStorageModeManaged:    return "MTLStorageModeManaged";
        case MTLStorageModePrivate:    return "MTLStorageModePrivate";
        case MTLStorageModeMemoryless: return "MTLStorageModeMemoryless";
        default:                       return "";
    }
}

PixelFormatInfo DepthReadbackDetails(MTLPixelFormat format, MTLBlitOption *option) {
    if (option != nullptr) *option = MTLBlitOptionNone;
    switch ((NSUInteger)format) {
        case 250: return {"VK_FORMAT_D16_UNORM", 1, 1, 2};   // Depth16Unorm
        case 252: return {"VK_FORMAT_D32_SFLOAT", 1, 1, 4};  // Depth32Float
        case 255:                                            // Depth24Unorm_Stencil8
            if (option != nullptr) *option = MTLBlitOptionDepthFromDepthStencil;
            // The depth aspect alone lands in a 32-bit word per pixel; the decoder for the
            // combined format reads the low 24 bits, which is where the depth is.
            return {"VK_FORMAT_D24_UNORM_S8_UINT", 1, 1, 4};
        case 260:                                            // Depth32Float_Stencil8
            if (option != nullptr) *option = MTLBlitOptionDepthFromDepthStencil;
            return {"VK_FORMAT_D32_SFLOAT", 1, 1, 4};
        default: return {"", 0, 0, 0};
    }
}

bool PixelFormatHasDepth(MTLPixelFormat format) {
    switch ((NSUInteger)format) {
        case 250: case 252: case 255: case 260: return true;
        default: return false;
    }
}

bool PixelFormatHasStencil(MTLPixelFormat format) {
    switch ((NSUInteger)format) {
        case 253: case 255: case 260: case 261: case 262: return true;
        default: return false;
    }
}

// MTLVertexFormat, from MTLVertexDescriptor.h. Numeric so the table does not depend on which
// SDK it is compiled against.
const char *VertexFormatEnumName(MTLVertexFormat format) {
    switch ((NSUInteger)format) {
        case 0: return "MTLVertexFormatInvalid";
        case 1: return "MTLVertexFormatUChar2";
        case 2: return "MTLVertexFormatUChar3";
        case 3: return "MTLVertexFormatUChar4";
        case 4: return "MTLVertexFormatChar2";
        case 5: return "MTLVertexFormatChar3";
        case 6: return "MTLVertexFormatChar4";
        case 7: return "MTLVertexFormatUChar2Normalized";
        case 8: return "MTLVertexFormatUChar3Normalized";
        case 9: return "MTLVertexFormatUChar4Normalized";
        case 10: return "MTLVertexFormatChar2Normalized";
        case 11: return "MTLVertexFormatChar3Normalized";
        case 12: return "MTLVertexFormatChar4Normalized";
        case 13: return "MTLVertexFormatUShort2";
        case 14: return "MTLVertexFormatUShort3";
        case 15: return "MTLVertexFormatUShort4";
        case 16: return "MTLVertexFormatShort2";
        case 17: return "MTLVertexFormatShort3";
        case 18: return "MTLVertexFormatShort4";
        case 19: return "MTLVertexFormatUShort2Normalized";
        case 20: return "MTLVertexFormatUShort3Normalized";
        case 21: return "MTLVertexFormatUShort4Normalized";
        case 22: return "MTLVertexFormatShort2Normalized";
        case 23: return "MTLVertexFormatShort3Normalized";
        case 24: return "MTLVertexFormatShort4Normalized";
        case 25: return "MTLVertexFormatHalf2";
        case 26: return "MTLVertexFormatHalf3";
        case 27: return "MTLVertexFormatHalf4";
        case 28: return "MTLVertexFormatFloat";
        case 29: return "MTLVertexFormatFloat2";
        case 30: return "MTLVertexFormatFloat3";
        case 31: return "MTLVertexFormatFloat4";
        case 32: return "MTLVertexFormatInt";
        case 33: return "MTLVertexFormatInt2";
        case 34: return "MTLVertexFormatInt3";
        case 35: return "MTLVertexFormatInt4";
        case 36: return "MTLVertexFormatUInt";
        case 37: return "MTLVertexFormatUInt2";
        case 38: return "MTLVertexFormatUInt3";
        case 39: return "MTLVertexFormatUInt4";
        case 40: return "MTLVertexFormatInt1010102Normalized";
        case 41: return "MTLVertexFormatUInt1010102Normalized";
        case 42: return "MTLVertexFormatUChar4Normalized_BGRA";
        case 45: return "MTLVertexFormatUChar";
        case 46: return "MTLVertexFormatChar";
        case 47: return "MTLVertexFormatUCharNormalized";
        case 48: return "MTLVertexFormatCharNormalized";
        case 49: return "MTLVertexFormatUShort";
        case 50: return "MTLVertexFormatShort";
        case 51: return "MTLVertexFormatUShortNormalized";
        case 52: return "MTLVertexFormatShortNormalized";
        case 53: return "MTLVertexFormatHalf";
        case 54: return "MTLVertexFormatFloatRG11B10";
        case 55: return "MTLVertexFormatFloatRGB9E5";
        default: return "";
    }
}

const char *VertexFormatCanonicalName(MTLVertexFormat format) {
    switch ((NSUInteger)format) {
        case 1: return "VK_FORMAT_R8G8_UINT";
        case 2: return "VK_FORMAT_R8G8B8_UINT";
        case 3: return "VK_FORMAT_R8G8B8A8_UINT";
        case 4: return "VK_FORMAT_R8G8_SINT";
        case 5: return "VK_FORMAT_R8G8B8_SINT";
        case 6: return "VK_FORMAT_R8G8B8A8_SINT";
        case 7: return "VK_FORMAT_R8G8_UNORM";
        case 8: return "VK_FORMAT_R8G8B8_UNORM";
        case 9: return "VK_FORMAT_R8G8B8A8_UNORM";
        case 10: return "VK_FORMAT_R8G8_SNORM";
        case 11: return "VK_FORMAT_R8G8B8_SNORM";
        case 12: return "VK_FORMAT_R8G8B8A8_SNORM";
        case 13: return "VK_FORMAT_R16G16_UINT";
        case 14: return "VK_FORMAT_R16G16B16_UINT";
        case 15: return "VK_FORMAT_R16G16B16A16_UINT";
        case 16: return "VK_FORMAT_R16G16_SINT";
        case 17: return "VK_FORMAT_R16G16B16_SINT";
        case 18: return "VK_FORMAT_R16G16B16A16_SINT";
        case 19: return "VK_FORMAT_R16G16_UNORM";
        case 20: return "VK_FORMAT_R16G16B16_UNORM";
        case 21: return "VK_FORMAT_R16G16B16A16_UNORM";
        case 22: return "VK_FORMAT_R16G16_SNORM";
        case 23: return "VK_FORMAT_R16G16B16_SNORM";
        case 24: return "VK_FORMAT_R16G16B16A16_SNORM";
        case 25: return "VK_FORMAT_R16G16_SFLOAT";
        case 26: return "VK_FORMAT_R16G16B16_SFLOAT";
        case 27: return "VK_FORMAT_R16G16B16A16_SFLOAT";
        case 28: return "VK_FORMAT_R32_SFLOAT";
        case 29: return "VK_FORMAT_R32G32_SFLOAT";
        case 30: return "VK_FORMAT_R32G32B32_SFLOAT";
        case 31: return "VK_FORMAT_R32G32B32A32_SFLOAT";
        case 32: return "VK_FORMAT_R32_SINT";
        case 33: return "VK_FORMAT_R32G32_SINT";
        case 34: return "VK_FORMAT_R32G32B32_SINT";
        case 35: return "VK_FORMAT_R32G32B32A32_SINT";
        case 36: return "VK_FORMAT_R32_UINT";
        case 37: return "VK_FORMAT_R32G32_UINT";
        case 38: return "VK_FORMAT_R32G32B32_UINT";
        case 39: return "VK_FORMAT_R32G32B32A32_UINT";
        case 40: return "VK_FORMAT_A2B10G10R10_SNORM_PACK32";
        case 41: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
        case 42: return "VK_FORMAT_B8G8R8A8_UNORM";
        case 45: return "VK_FORMAT_R8_UINT";
        case 46: return "VK_FORMAT_R8_SINT";
        case 47: return "VK_FORMAT_R8_UNORM";
        case 48: return "VK_FORMAT_R8_SNORM";
        case 49: return "VK_FORMAT_R16_UINT";
        case 50: return "VK_FORMAT_R16_SINT";
        case 51: return "VK_FORMAT_R16_UNORM";
        case 52: return "VK_FORMAT_R16_SNORM";
        case 53: return "VK_FORMAT_R16_SFLOAT";
        case 54: return "VK_FORMAT_B10G11R11_UFLOAT_PACK32";
        case 55: return "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32";
        default: return "";
    }
}

const char *LoadActionEnumName(MTLLoadAction action) {
    switch (action) {
        case MTLLoadActionDontCare: return "MTLLoadActionDontCare";
        case MTLLoadActionLoad:     return "MTLLoadActionLoad";
        case MTLLoadActionClear:    return "MTLLoadActionClear";
        default:                    return "";
    }
}

const char *StoreActionEnumName(MTLStoreAction action) {
    switch (action) {
        case MTLStoreActionDontCare:                   return "MTLStoreActionDontCare";
        case MTLStoreActionStore:                      return "MTLStoreActionStore";
        case MTLStoreActionMultisampleResolve:         return "MTLStoreActionMultisampleResolve";
        case MTLStoreActionStoreAndMultisampleResolve: return "MTLStoreActionStoreAndMultisampleResolve";
        case MTLStoreActionUnknown:                    return "MTLStoreActionUnknown";
        case MTLStoreActionCustomSampleDepthStore:     return "MTLStoreActionCustomSampleDepthStore";
        default:                                       return "";
    }
}

const char *PrimitiveTypeEnumName(MTLPrimitiveType type) {
    switch (type) {
        case MTLPrimitiveTypePoint:         return "MTLPrimitiveTypePoint";
        case MTLPrimitiveTypeLine:          return "MTLPrimitiveTypeLine";
        case MTLPrimitiveTypeLineStrip:     return "MTLPrimitiveTypeLineStrip";
        case MTLPrimitiveTypeTriangle:      return "MTLPrimitiveTypeTriangle";
        case MTLPrimitiveTypeTriangleStrip: return "MTLPrimitiveTypeTriangleStrip";
        default:                            return "";
    }
}

uint64_t PixelFormatImageSize(const PixelFormatInfo &info, uint32_t width, uint32_t height,
                              uint64_t *bytesPerRow) {
    if (info.blockBytes == 0 || info.blockWidth == 0 || info.blockHeight == 0) {
        if (bytesPerRow != nullptr) *bytesPerRow = 0;
        return 0;
    }
    // Rounded up to whole blocks: a 5x5 BC1 image is two blocks by two, not one and a quarter.
    const uint64_t blocksWide = (width + info.blockWidth - 1) / info.blockWidth;
    const uint64_t blocksHigh = (height + info.blockHeight - 1) / info.blockHeight;
    const uint64_t rowBytes = blocksWide * info.blockBytes;
    if (bytesPerRow != nullptr) *bytesPerRow = rowBytes;
    return rowBytes * blocksHigh;
}

}  // namespace mtlinsp
