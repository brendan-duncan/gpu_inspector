// Pixel formats: what a DXGI_FORMAT is called in the protocol, and how its bytes are laid out.
//
// Two names for one format, wanted for different reasons: the DXGI name is what a description
// shows in the Inspect panel, and the protocol's VK_FORMAT_* name travels with pixel data, because
// the inspector's decoders are built around those names and an identical memory layout reuses all
// of them. The DXGI name comes from the generated enum table; the mapping and the layouts are here.
#pragma once

#include "common.h"

namespace d3d11insp {

struct FormatInfo {
    /** The protocol's name for pixel data of this format, or nullptr when the inspector cannot decode it. */
    const char* protocolName = nullptr;
    /** Bytes per pixel, or per block for a block-compressed format. */
    uint32_t bytes = 0;
    /** Block size in pixels (1 for uncompressed). */
    uint32_t blockWidth = 1;
    uint32_t blockHeight = 1;
    bool depth = false;      // has a depth aspect
    bool stencil = false;    // has a stencil aspect
    bool compressed = false;
};

/** The layout of a format, `bytes` 0 when it is unknown (a video format, a typeless one with no width). */
FormatInfo FormatOf(DXGI_FORMAT format);

/** The DXGI name, or "DXGI_FORMAT(n)" for one the table does not know. */
const char* FormatName(DXGI_FORMAT format);

/**
 * The typed format a typeless resource is read back as, or a view's own format: the resource's
 * `DXGI_FORMAT_R8G8B8A8_TYPELESS` becomes `_UNORM`, `_R32_TYPELESS` bound as depth becomes
 * `D32_FLOAT`. What the read-back copies can be mapped to a decoder this way.
 */
DXGI_FORMAT TypedFormat(DXGI_FORMAT format, bool asDepth);

bool IsDepthFormat(DXGI_FORMAT format);
bool IsDepthCapable(DXGI_FORMAT format);

/** Bytes of one row of `width` pixels (whole blocks), and the number of rows (whole blocks) of `height` pixels. */
uint64_t RowBytes(const FormatInfo& f, uint32_t width);
uint32_t RowCount(const FormatInfo& f, uint32_t height);

}  // namespace d3d11insp
