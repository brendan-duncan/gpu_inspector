// GL internal formats: what each is called in the inspector's vocabulary (VK_FORMAT_* names, which
// its texture decoder reads for every API; src/app/src/renderer/vulkan/texture_decode.ts), and how
// its texels can be read back in OpenGL ES, where glReadPixels takes only a few format/type pairs.
#pragma once

#include "../gen/gles_api.gen.h"

namespace glesinsp
{

/** How glReadPixels reads a color format back, and what the bytes then are. */
enum class ReadClass : uint8_t
{
    None,       // depth, stencil, compressed, or unknown: not read with glReadPixels
    Unorm,      // GL_RGBA / GL_UNSIGNED_BYTE -> R8G8B8A8 (UNORM or SRGB)
    Float,      // GL_RGBA / GL_FLOAT -> R32G32B32A32_SFLOAT
    Int,        // GL_RGBA_INTEGER / GL_INT -> R32G32B32A32_SINT
    Uint,       // GL_RGBA_INTEGER / GL_UNSIGNED_INT -> R32G32B32A32_UINT
};

struct FormatInfo
{
    /** The format's own name, "VK_FORMAT_R8G8B8A8_UNORM"; "VK_FORMAT_UNDEFINED" when unknown. */
    const char* vk = "VK_FORMAT_UNDEFINED";
    ReadClass read = ReadClass::None;
    bool srgb = false;
    bool depth = false;
    bool stencil = false;
    bool compressed = false;
    /** Compressed formats: the block's size in texels and bytes. */
    int blockWidth = 1, blockHeight = 1, blockBytes = 0;
};

FormatInfo FormatOf(GLenum internalFormat);

/** An ES 2 unsized internal format (GL_RGBA with its type) as the sized format it amounts to. */
GLenum SizedFormat(GLenum internalFormat, GLenum format, GLenum type);

/** What a read-back of this class is: glReadPixels' format and type, the bytes per texel, and its VK name. */
struct ReadFormat
{
    GLenum format = 0;
    GLenum type = 0;
    int bytesPerTexel = 0;
    const char* vk = "VK_FORMAT_UNDEFINED";
};
ReadFormat ReadFormatOf(const FormatInfo& f);

/** Bytes of one compressed level of the given size. */
size_t CompressedBytes(const FormatInfo& f, int width, int height);

}  // namespace glesinsp
