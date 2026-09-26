// The last stage before rasterization (a vertex, tessellation evaluation or geometry shader) edited to
// write its outputs to a transform feedback buffer, for the mesh output view (mesh.cpp): the capture's
// own SPIR-V with the TransformFeedback capability, the Xfb execution mode on the entry point, and
// XfbBuffer / XfbStride / Offset on the outputs it can capture. Transform feedback records whatever
// that stage emits, as lists: a geometry shader's strips and a tessellator's patches come out as
// separate primitives.
//
// RenderDoc gets the same data by turning the vertex shader into a compute shader (vk_postvs.cpp),
// which works without the extension; transform feedback is a far smaller edit of the module.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkreplay
{

/** One output the edited shader writes to the buffer, at `offset` in each vertex's record. */
struct XfbOutput
{
    std::string name;
    uint32_t offset = 0;
    /** Scalars in it (a vec4 is 4, a mat3 is 9), each four bytes. */
    uint32_t components = 0;
    /** "float", "int" or "uint". */
    std::string base;
    /** "Position" for gl_Position, "Layer" for gl_Layer, "ViewportIndex"; empty for a located output. */
    std::string builtin;
    int32_t location = -1;
};

struct XfbPatch
{
    std::vector<uint32_t> words;
    /** Bytes per vertex in the buffer. */
    uint32_t stride = 0;
    std::vector<XfbOutput> outputs;
    /** Why the module could not be edited; empty on success. */
    std::string error;
    /**
     * A geometry shader's vertices at most per input primitive (its OutputVertices times its
     * Invocations); 0 for the other stages.
     */
    uint32_t maxVerticesOut = 0;
    /** Set by the replay: the stage edited ("vertex", "tessellation evaluation", "geometry"), and the list topology it records past the vertex stage. */
    std::string stage;
    std::string topology;
};

/**
 * Edits a vertex, tessellation evaluation or geometry shader module; `entryPoint` picks the entry
 * point (the first one of those stages when not found).
 */
XfbPatch PatchForTransformFeedback(const uint32_t* words, size_t count, const std::string& entryPoint);

/**
 * The list topology a geometry or tessellation shader's primitives are recorded as, from its execution
 * modes (VK_PRIMITIVE_TOPOLOGY_POINT_LIST, _LINE_LIST or _TRIANGLE_LIST); empty when it names none,
 * as a tessellation evaluation shader may leave them to the control shader.
 */
std::string OutputTopology(const uint32_t* words, size_t count);

/**
 * gl_ViewIndex replaced by the constant `view`, for one view of a multiview draw captured outside
 * the multiview pass (transform feedback cannot be active in one). The input variable becomes a
 * private one initialized to the view; the module is returned unchanged when it reads no view index.
 */
std::vector<uint32_t> PatchViewIndex(const uint32_t* words, size_t count, uint32_t view);

} // namespace vkreplay
