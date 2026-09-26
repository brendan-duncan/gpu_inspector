// A geometry or tessellation evaluation shader edited to also write which invocation made each
// vertex, for the mesh output (mesh.cpp) and through it the shader debugger: a geometry shader's
// gl_PrimitiveIDIn and gl_InvocationID at every EmitVertex, a tessellation evaluation shader's
// gl_PrimitiveID (its patch) and gl_TessCoord at its end. They are written to output variables of
// their own at locations after the shader's, which the transform feedback edit (xfb_patch.h) then
// captures like any other output. The debugger finds the invocation behind a record from them,
// and the tessellation coordinate is what no other part of the capture has: which point of the
// patch the tessellator handed that invocation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkreplay
{

struct IdentityOutput
{
    uint32_t location = 0;
    /** The name the mesh output gives it ("gl_PrimitiveIDIn"), and the built-in it copies ("PrimitiveId"). */
    std::string name;
    std::string builtin;
};

struct IdentityPatch
{
    /** The edited module; empty when the stage has nothing to add (a vertex shader) or on an error. */
    std::vector<uint32_t> words;
    std::vector<IdentityOutput> outputs;
    std::string error;
};

/** Adds the identity outputs to a geometry or tessellation evaluation entry point; `entryPoint` picks it. */
IdentityPatch AddIdentityOutputs(const uint32_t* words, size_t count, const std::string& entryPoint);

} // namespace vkreplay
