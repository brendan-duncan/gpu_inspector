// Pixel formats and the other Metal enums the descriptors carry.
//
// Two different jobs, deliberately separate:
//
//   * `PixelFormatEnumName` is Metal's own spelling, `MTLPixelFormatBGRA8Unorm`. It is what a
//     descriptor shows in the Inspect panel, because that is what the application wrote and what
//     its documentation calls it. A bare `80` is not something anyone can act on.
//   * `PixelFormatDetails` is the protocol's name for the same memory layout, which is Vulkan's,
//     plus the block geometry needed to size a read-back. The UI's decoder is built around those
//     names, so emitting the canonical one for an identical layout reuses all of it.
//
// The enum names are generated from the SDK header (see the note at the top of formats.mm) and
// cover every format Metal has. The canonical mapping is hand-written and covers what has a real
// equivalent: a format with no mapping still displays its name, and read-back reports that it
// cannot be read rather than showing nothing.
//
// Vertex formats get the same pair, for the same reason: the pipeline descriptor shows
// `MTLVertexFormatFloat3`, and the UI's vertex decoder is handed `VK_FORMAT_R32G32B32_SFLOAT`.
#pragma once

#include <cstdint>
#include <string>

#import <Metal/Metal.h>

namespace mtlinsp
{

struct PixelFormatInfo
{
    /** The protocol's name, or "" when this format has no equivalent yet. */
    const char* name;
    /** 1x1 for an uncompressed format, 4x4 for the BC family. */
    uint32_t blockWidth;
    uint32_t blockHeight;
    /** Bytes per block — per pixel when the block is 1x1. */
    uint32_t blockBytes;
};

/** Metal's own name for the format, e.g. "MTLPixelFormatBGRA8Unorm". "" if the SDK has none. */
const char* PixelFormatEnumName(MTLPixelFormat format);

/** The protocol name and block geometry, or a zeroed entry when the format is not mapped. */
PixelFormatInfo PixelFormatDetails(MTLPixelFormat format);

/**
 * What a blit of the depth aspect of this format produces, and the blit option that selects it.
 *
 * A combined depth-stencil texture cannot be copied to a buffer whole: the blit has to pick one
 * aspect with MTLBlitOptionDepthFromDepthStencil or MTLBlitOptionStencilFromDepthStencil, and
 * what lands in the buffer is that aspect alone — four bytes of depth for
 * MTLPixelFormatDepth32Float_Stencil8, not eight. A plain depth format needs no option. Returns
 * a zeroed entry for a format with no depth.
 */
PixelFormatInfo DepthReadbackDetails(MTLPixelFormat format, MTLBlitOption* option);

/**
 * The same for the stencil aspect: one byte a texel (`VK_FORMAT_S8_UINT`), and the blit option that
 * picks stencil out of a combined format.
 *
 * A combined depth/stencil texture is read twice, once per aspect, because a blit takes one of them
 * at a time — `MTLBlitOptionDepthFromDepthStencil` and `MTLBlitOptionStencilFromDepthStencil` may
 * not both be set on one copy. `MTLPixelFormatStencil8` and the `X*_Stencil8` views are stencil
 * already and need no option.
 */
PixelFormatInfo StencilReadbackDetails(MTLPixelFormat format, MTLBlitOption* option);

bool PixelFormatHasDepth(MTLPixelFormat format);
bool PixelFormatHasStencil(MTLPixelFormat format);

/** Metal's own name for a texture type, e.g. "MTLTextureType2D". */
const char* TextureTypeEnumName(MTLTextureType type);

/** Metal's own name for a storage mode, e.g. "MTLStorageModePrivate". */
const char* StorageModeEnumName(MTLStorageMode mode);
const char* PurgeableStateEnumName(MTLPurgeableState state);

/** Metal's own name for a vertex format, e.g. "MTLVertexFormatFloat3". "" if unknown. */
const char* VertexFormatEnumName(MTLVertexFormat format);

/** The protocol's name for a vertex format's layout, e.g. "VK_FORMAT_R32G32B32_SFLOAT". */
const char* VertexFormatCanonicalName(MTLVertexFormat format);

const char* LoadActionEnumName(MTLLoadAction action);
const char* StoreActionEnumName(MTLStoreAction action);
const char* PrimitiveTypeEnumName(MTLPrimitiveType type);
const char* IndexTypeEnumName(MTLIndexType type);

// The ray tracing enums (raytracing.h). A geometry's vertex format is an MTLAttributeFormat, whose
// values are identical to MTLVertexFormat's — the two enums list the same 42 entries with the same
// numbers — so both vertex format namers above serve it, and an acceleration structure's vertices
// reach the UI's decoder under the same `VK_FORMAT_*` name a draw's do.

/** Metal's own name for an instance descriptor type, e.g. "MTLAccelerationStructureInstanceDescriptorTypeUserID". */
const char* InstanceDescriptorTypeEnumName(uint64_t type);
/** Metal's own name for a transform layout, e.g. "MTLMatrixLayoutColumnMajor". */
const char* MatrixLayoutEnumName(uint64_t layout);
/** Metal's own name for a motion border mode, e.g. "MTLMotionBorderModeClamp". */
const char* MotionBorderModeEnumName(uint64_t mode);
const char* CurveTypeEnumName(uint64_t type);
const char* CurveBasisEnumName(uint64_t basis);
const char* CurveEndCapsEnumName(uint64_t caps);

/**
 * An option set as "Refit|PreferFastBuild": the short names joined by `|`, which is how the Vulkan
 * layer and the rest of this library spell a flag set (UsageFlags in hooks_descriptors.mm).
 * "None" for zero.
 */
std::string AccelerationStructureUsageFlags(uint64_t usage);
std::string AccelerationStructureRefitOptionFlags(uint64_t options);
std::string IntersectionFunctionSignatureFlags(uint64_t signature);

/** Bytes a `width` x `height` region of this format occupies, rows padded to whole blocks. */
uint64_t PixelFormatImageSize(const PixelFormatInfo& info, uint32_t width, uint32_t height,
    uint64_t* bytesPerRow);

}  // namespace mtlinsp
