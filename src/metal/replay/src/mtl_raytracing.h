// Rebuilding a Metal acceleration structure descriptor from what the capture recorded, so the
// replay can build the structure again on this machine's GPU.
//
// This is the piece the Vulkan and Direct3D 12 replays need far more machinery for, and the reason
// is in the API rather than in the code. A `VkAccelerationStructureGeometryKHR` names its vertices
// by *device address*, so the Vulkan replay has to have recorded every buffer's address range and
// map an address back to the buffer it fell in (`RemapAddress` in src/replay/src/replayer.h), with
// all the ways that can go wrong — an address into a buffer the replay made at a different address,
// an address into no buffer at all. A `D3D12_RAYTRACING_GEOMETRY_DESC` is the same.
//
// A Metal geometry descriptor holds `id<MTLBuffer> vertexBuffer` and an offset. The capture writes
// that as an object reference, and the replay resolves it the way it resolves every other
// reference. There is nothing to remap and nothing to guess.
//
// What is left is transcription: Metal has four geometry kinds and a motion variant of each, and
// the descriptor classes do not share a base with the fields on it, so each kind is filled on its
// own. The capture writes every property it read (src/metal/src/raytracing.mm), so a property
// absent from the JSON is one the SDK that took the capture did not have, and is left at the
// freshly-allocated descriptor's default — the same rule FillVisitor follows for everything else.
#pragma once

#include <string>
#include <utility>
#include <vector>

#import <Metal/Metal.h>

#include "mtl_decode.h"

namespace mtlreplay {

/**
 * The descriptor a build recorded, rebuilt. Nil with `error` set when the capture's JSON names a
 * shape this cannot make: a geometry kind the SDK here does not have, a top level whose bottom
 * levels are not all in the replay.
 *
 * `d` is the `descriptor` member of a build command's arguments, or of an acceleration structure
 * object created from one.
 */
MTLAccelerationStructureDescriptor* BuildDescriptor(const Decoder& d, std::string& error);

/**
 * The C++ that builds the same descriptor, for Export to C++. `var` is the local to assign it to,
 * already declared as `MTLAccelerationStructureDescriptor *`. False when the descriptor could not
 * be written, with `error` saying why — the same cases BuildDescriptor refuses.
 */
bool WriteDescriptorSource(Source& w, const std::string& var, const Decoder& d, std::string& error);

/**
 * The `linkedFunctions` a pipeline was built with, as an `MTLLinkedFunctions`, or nil when the
 * capture recorded none.
 *
 * Without these a pipeline whose kernel traverses a scene still *compiles* — the intersector is in
 * the kernel — but `newIntersectionFunctionTableWithDescriptor:` gives a table whose entries cannot
 * be filled: a function handle only exists for a function the pipeline was linked against. So a
 * replay that skipped them would build the structures, run the trace, and have every ray miss.
 *
 * `out` collects each linked function by name, which is how a table entry names the function it
 * runs (an `MTLFunctionHandle` carries its name; there is no identifier to match, unlike DXR's
 * 32-byte export identifiers or Vulkan's group handles).
 */
id LinkedFunctions(const Decoder& d, std::vector<std::pair<std::string, id>>& out);

/** The exported statements that build the same `MTLLinkedFunctions` into `var`; "" for none. */
std::string WriteLinkedFunctionsSource(Source& w, const Decoder& d);

} // namespace mtlreplay
