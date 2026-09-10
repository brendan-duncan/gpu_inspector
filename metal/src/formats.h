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
#pragma once

#include <cstdint>

#import <Metal/Metal.h>

namespace mtlinsp {

struct PixelFormatInfo {
    /** The protocol's name, or "" when this format has no equivalent yet. */
    const char *name;
    /** 1x1 for an uncompressed format, 4x4 for the BC family. */
    uint32_t blockWidth;
    uint32_t blockHeight;
    /** Bytes per block — per pixel when the block is 1x1. */
    uint32_t blockBytes;
};

/** Metal's own name for the format, e.g. "MTLPixelFormatBGRA8Unorm". "" if the SDK has none. */
const char *PixelFormatEnumName(MTLPixelFormat format);

/** The protocol name and block geometry, or a zeroed entry when the format is not mapped. */
PixelFormatInfo PixelFormatDetails(MTLPixelFormat format);

/** Metal's own name for a texture type, e.g. "MTLTextureType2D". */
const char *TextureTypeEnumName(MTLTextureType type);

/** Metal's own name for a storage mode, e.g. "MTLStorageModePrivate". */
const char *StorageModeEnumName(MTLStorageMode mode);

/** Bytes a `width` x `height` region of this format occupies, rows padded to whole blocks. */
uint64_t PixelFormatImageSize(const PixelFormatInfo &info, uint32_t width, uint32_t height,
                              uint64_t *bytesPerRow);

}  // namespace mtlinsp
