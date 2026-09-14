// A vertex shader edited to write its outputs to a transform feedback buffer, for the mesh output
// view (mesh.cpp): the capture's own SPIR-V with the TransformFeedback capability, the Xfb execution
// mode on the entry point, and XfbBuffer / XfbStride / Offset on the outputs it can capture.
//
// RenderDoc gets the same data by turning the vertex shader into a compute shader (vk_postvs.cpp),
// which works without the extension; transform feedback is a far smaller edit of the module.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkreplay {

/** One output the edited shader writes to the buffer, at `offset` in each vertex's record. */
struct XfbOutput {
    std::string name;
    uint32_t offset = 0;
    /** Scalars in it (a vec4 is 4, a mat3 is 9), each four bytes. */
    uint32_t components = 0;
    /** "float", "int" or "uint". */
    std::string base;
    /** "Position" for gl_Position; empty for a located output. */
    std::string builtin;
    int32_t location = -1;
};

struct XfbPatch {
    std::vector<uint32_t> words;
    /** Bytes per vertex in the buffer. */
    uint32_t stride = 0;
    std::vector<XfbOutput> outputs;
    /** Why the module could not be edited; empty on success. */
    std::string error;
};

/** Edits a vertex shader module; `entryPoint` picks the entry point (the first vertex one when not found). */
XfbPatch PatchForTransformFeedback(const uint32_t* words, size_t count, const std::string& entryPoint);

} // namespace vkreplay
