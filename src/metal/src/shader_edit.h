// Live shader editing: the UI's ReplaceShader {pipeline, stage, spirv} carries Metal Shading
// Language *source* (the field keeps its Vulkan name, as the D3D12 library's carries DXIL); the
// library compiles it, makes the pipeline again with that stage's function swapped, registers the
// result as an object of its own ("<name> (edited)") and binds it instead of the original from
// then on. RestoreShader drops the edit. The counterpart of src/vulkan/src/shader_edit.h and
// src/d3d12/src/shader_edit.h.
//
// Metal's is the least work of the three, and for a reason worth stating: the other two libraries
// are handed bytecode the UI had to produce — SPIR-V from glslang, DXIL from dxc, either of which
// may be absent from the machine the inspector runs on — while a Metal capture holds the Shading
// Language the application itself compiled, and the device that compiled it is right there. So
// there is no compiler to find and no bytecode to get right: `newLibraryWithSource:` takes the
// edited text, and the compiler's own diagnostics go back to the editor's status line, pointing at
// lines of the text that is on screen.
//
// What does need care is everything *around* the function:
//
//   - **A pipeline state cannot be copied, but its descriptor can.** `overdraw.h` already keeps
//     every render pipeline's descriptor since its measurements' copies are made from it, and this
//     reads the same table (`CopyRememberedPipelineDescriptor`). A pipeline the library never saw
//     created — one built before it was loaded — cannot be rebuilt, and says so.
//   - **A compute pipeline is usually built from a function, not a descriptor.**
//     `newComputePipelineStateWithFunction:` has no descriptor to keep, so the function is
//     remembered instead and the rebuild goes through the same call.
//   - **Function constants have to be carried across.** A Unity shader is one library of
//     `[[function_constant(n)]]`-guarded variants; recompiling without the values the application
//     specialized with would build a different variant and draw something else entirely. The
//     values object is kept at function creation (`function_constants.h`) and used again here.
//     This is the one part with no counterpart in either other backend.
//
// Replacements are never freed while the library is attached. A command buffer holds a reference
// to the state its encoder bound, but an application may ask for one with unretained references
// (`commandBufferWithUnretainedReferences`), and then nothing but the editor would be keeping the
// replacement alive while the GPU still had work referring to it. A handful of pipeline states is
// a few kilobytes, and a session where a person is editing shaders by hand will not make many.
#pragma once

#include <cstdint>
#include <string>

#import <objc/objc.h>

namespace mtlinsp {

/**
 * Compiles `source` as Metal Shading Language and makes the pipeline again with `stage`'s function
 * replaced by the one of the same name in it. `stage` is the UI's stage name ("vertex",
 * "fragment", "compute").
 *
 * Returns false with `error`, which for a compile failure is the compiler's own diagnostics: the
 * editor shows them against the text the person just typed. On success `replacementId` is the new
 * object's id and `note` says what could not be carried across.
 */
bool ReplaceShader(uint64_t pipelineId, const std::string &stage, const std::string &source,
                   std::string &error, uint64_t &replacementId, std::string &note);

/** Drops the edit of one stage, or of every stage when `stage` is empty. False with `error`. */
bool RestoreShader(uint64_t pipelineId, const std::string &stage, std::string &error);

/**
 * The state to bind in place of `state`: its replacement when edited, else `state` itself. Called
 * from `setRenderPipelineState:` and `setComputePipelineState:`, so the common case — nothing
 * edited — is one relaxed atomic read and no lock.
 */
id SubstitutePipelineState(id state);

/** A compute pipeline was created from a function: the function is what a rebuild needs. */
void RememberComputePipeline(id state, id function);

/** An object is being deallocated: drops what was kept of it. */
void ForgetEditedPipeline(id object);

}  // namespace mtlinsp
