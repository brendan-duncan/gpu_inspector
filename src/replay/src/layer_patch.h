// A pre-rasterization shader edited to draw into one layer of a layered framebuffer only, for the
// pixel history of a pass layered through gl_Layer (history.cpp): the capture's own SPIR-V, with
// gl_Position moved outside the clip volume wherever the vertex it finishes is for another layer.
// Where it finishes a vertex is the end of the entry point for a vertex or tessellation evaluation
// shader, and each EmitVertex for a geometry shader. A primitive's vertices name one layer, so a
// primitive is kept whole or clipped whole, and the queries and the one-pixel scissor of the
// history meet the followed layer's fragments only. A shader that writes no layer draws into
// layer 0: it is left alone following layer 0, and clipped everywhere following another.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkreplay
{

struct LayerPatch
{
    /** The shader decides a layer, or the followed layer is not 0: the code needs the edit. */
    bool needed = false;
    std::vector<uint32_t> words;
    /** Why a needed edit was not made; empty on success. */
    std::string error;
};

/** Edits a vertex, tessellation evaluation or geometry shader to draw into `layer` only; `entryPoint` picks the entry point. */
LayerPatch PatchForLayer(const uint32_t* words, size_t count, const std::string& entryPoint, uint32_t layer);

} // namespace vkreplay
