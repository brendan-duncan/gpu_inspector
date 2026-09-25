// A fragment shader edited to count its own fragments, for overdraw and the draw overlays
// (overdraw.cpp): the capture's own SPIR-V, whatever it computes, with its color outputs made private
// and one output of its own at location 0 that writes 1.0. What decides whether a fragment is kept
// stays the application's: a discard (OpKill, OpTerminateInvocation, a demote to a helper) still
// throws the fragment away, and a written gl_FragDepth or gl_SampleMask still decides the depth test
// and the coverage. So alpha-tested geometry counts where it is drawn, not over its whole quad.
//
// Only needed where the shader does one of those; any other shader counts the same with the replay's
// constant one (kCountFragmentSpirv), which is cheaper to run. A shader that writes memory (a storage
// buffer or image, an atomic) is not edited: drawing it again would repeat what it wrote.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkreplay
{

struct CountPatch
{
    /** The shader discards, or writes depth, the sample mask or the stencil reference. */
    bool needed = false;
    std::vector<uint32_t> words;
    /** Why a needed edit was not made; empty on success. */
    std::string error;
};

/** Edits a fragment shader module; `entryPoint` picks the entry point (the first fragment one when not found). */
CountPatch PatchForCounting(const uint32_t* words, size_t count, const std::string& entryPoint);

} // namespace vkreplay
