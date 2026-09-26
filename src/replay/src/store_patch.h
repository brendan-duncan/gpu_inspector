// A vertex shader edited to write its outputs to a storage buffer, for the mesh output (mesh.cpp) on a
// GPU without transform feedback but with vertexPipelineStoresAndAtomics (MoltenVK, for one). Where
// the shader finishes (each return of its entry point), the
// outputs transform feedback would have captured (xfb_patch.h decides which, and their layout) are
// written, word by word, to record `slot` of a buffer at the descriptor set and binding given:
//
//     slot = (gl_InstanceIndex - firstInstance) * perInstance + (gl_VertexIndex - base)
//
// base is the draw's firstVertex (vertexOffset for an indexed draw), so a slot is a vertex of the
// draw (an index value of an indexed one), whatever order the GPU ran them in, and a vertex that
// several indices name is written once. A slot past `capacity` is not written. The replay puts the
// records in transform feedback's order afterwards (index order, strips as lists).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xfb_patch.h"

namespace vkreplay
{

struct StoreParams
{
    uint32_t set = 0;
    uint32_t binding = 0;
    int32_t firstInstance = 0;
    uint32_t perInstance = 0;
    int32_t base = 0;
    uint32_t capacity = 0;
};

struct StorePatch
{
    std::vector<uint32_t> words;
    /** Why the module could not be edited; empty on success. */
    std::string error;
};

/**
 * Edits a vertex shader module to store `outputs` (a transform feedback layout of the same module,
 * `stride` bytes a record) into the buffer `params` names. `entryPoint` picks the entry point.
 */
StorePatch PatchForVertexStores(const uint32_t* words, size_t count, const std::string& entryPoint, const std::vector<XfbOutput>& outputs,
    uint32_t stride, const StoreParams& params);

} // namespace vkreplay
