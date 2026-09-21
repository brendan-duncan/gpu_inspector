// Metal ray tracing: acceleration structures as objects, what a build read, and the function
// tables a traversal reaches its intersection functions through.
//
// The Metal counterpart of src/d3d12/src/raytracing.h and of the ray tracing half of
// src/vulkan/src/hooks.cpp, and much the smallest of the three, because Metal names things with
// objects where the other two name them with numbers:
//
//   The structures. A VkAccelerationStructureKHR is a handle the tracker owns; a D3D12 structure
//   is a range inside a UAV buffer, named only by the address a build wrote it to, so that library
//   has to mint an object per address and keep a registry of them. An MTLAccelerationStructure is
//   an ordinary Metal object with a `dealloc` the tracker already watches, so it needs no registry
//   at all — only a creation hook. It even carries its own `size`, which is the `resultSize` both
//   other libraries have to ask the driver for.
//
//   Build inputs. A Vulkan or D3D12 build reads its geometry from GPU virtual addresses, and an
//   address on its own says nothing: both libraries keep an address map and resolve through it,
//   which is the largest part of each. A Metal geometry descriptor holds `id<MTLBuffer>` and an
//   offset outright, so there is nothing to resolve and nothing to keep — QueueBufferCapture takes
//   exactly what the descriptor already says.
//
//   A top level's bottom levels. Vulkan and D3D12 name them by device address inside the instance
//   buffer, which is why resolving that address is what made them reachable at all. A Metal
//   instance names its bottom level by *index* into the descriptor's
//   `instancedAccelerationStructures`, an array of tracked objects — so the link a capture needs is
//   handed over rather than reconstructed. (The two `Indirect` descriptor types are the exception:
//   they hold an MTLResourceID, which is why every structure's is recorded.)
//
// What Metal has that the others do not is the other half of this file. There is no shader binding
// table and no raygen/miss/hit dispatch: a Metal kernel traverses the scene itself and reaches its
// intersection functions through an MTLIntersectionFunctionTable, whose entries are *set through
// the API*. So where the other two libraries read a table back out of GPU memory and match opaque
// handles against a pipeline's, this one simply records what the application set, exactly.
#pragma once

#include <cstdint>
#include <string>

#import <Metal/Metal.h>
#import <objc/runtime.h>

namespace mtlinsp {

// ---------------------------------------------------------------------------------------------
// Acceleration structures

/**
 * Registers a structure the application created, with the descriptor it was sized from when there
 * was one.
 *
 * `descriptor` is nil for `newAccelerationStructureWithSize:`, which is the form most applications
 * use (`test/path_tracer/metal` does): the size comes from
 * `accelerationStructureSizesWithDescriptor:` and the descriptor itself is only handed over later,
 * at the build. So the descriptor is not where a structure's contents are learned — the build is —
 * and this only records what was known at creation.
 *
 * `parent` is the device, or the heap it was sub-allocated from. Where it sits in that heap needs no
 * argument: an MTLAccelerationStructure is an MTLResource, so WriteMemoryInfo reports its
 * `allocatedSize`, `heap` and `heapOffset` the same way it does a buffer's.
 */
void NoteAccelerationStructure(id structure, id parent, const char *cmd,
                               MTLAccelerationStructureDescriptor *descriptor);

/**
 * Records a build or refit on the structure it wrote, queues the buffers it read, and returns the
 * command's arguments.
 *
 * Called whether or not a capture is recording, which is the whole point: an engine builds its
 * bottom levels once, at load, and a capture of any later frame would otherwise know what its
 * structures are but not what is in them. `encoder` nil means "not recording" — the descriptor and
 * the buffers it names are remembered, and nothing is read back until
 * ReadBackEarlierStructures.
 *
 * The returned JSON carries each input's `{buffer, offset, capture}` inside the geometry that names
 * it, rather than in a separate list beside the command the way the Vulkan layer's `buildData` and
 * the D3D12 library's do. Both of those exist because an address has to be resolved to a buffer
 * before it means anything, and the result had nowhere else to go; a Metal descriptor already says
 * `buffer` and `offset`, so the capture id belongs beside them. It also puts the ids on the command
 * rather than on the structure, which is what the other two libraries went out of their way to do:
 * a structure's update is last-write-wins, and an application that rebuilds every frame would
 * otherwise overwrite the captured build's ids with a later build's, which has none.
 *
 * The same descriptor also goes onto the structure as its `build` update, so a structure the
 * capture holds no build command for still says what it is.
 */
std::string NoteAccelerationStructureBuild(id encoder, const char *method, id destination,
                                           MTLAccelerationStructureDescriptor *descriptor,
                                           id scratch, NSUInteger scratchOffset, id source);

/**
 * A copy or compacting copy between structures, as the command's arguments.
 *
 * The destination takes the source's build, so a structure the application compacted still says
 * what is in it — the same reasoning as NoteAccelerationStructureCopy in the D3D12 library. Called
 * whether or not a capture is recording, for the reason above.
 */
std::string NoteAccelerationStructureCopy(const char *method, id source, id destination,
                                          id buffer, NSUInteger offset);

/**
 * Reads back what every known structure's last build read, once per capture, behind `encoder`.
 *
 * A bottom level an engine built at load has no build in any later frame, so without this a capture
 * knows what a structure is but not what is in it. What comes back is what those buffers hold
 * *now*: right for static geometry, and not for a buffer the application has rewritten since, which
 * is what the UI says beside it. Posted on each structure as `captureInputs`, the same key the
 * Vulkan and D3D12 libraries use.
 *
 * Called from the first acceleration structure or compute encoder of a capture, since a private
 * buffer's read-back needs an open pass to blit through.
 */
void ReadBackEarlierStructures(id encoder, uint64_t captureSerial);

/** Whether any structure has been created, so the capture only looks for earlier ones when there are. */
bool HasAccelerationStructures(void);

// ---------------------------------------------------------------------------------------------
// Function tables
//
// An MTLIntersectionFunctionTable is what a geometry's or instance's `intersectionFunctionTableOffset`
// indexes: the traversal calls entry N of the bound table when it reaches a primitive whose offset
// is N. Its entries are set through `setFunction:atIndex:` and friends, so the capture records the
// table's contents as they are set rather than reading anything back — and an MTLFunctionHandle
// carries its own `name`, so an entry names the function directly with no side table to keep.

/** Registers a table a pipeline handed out, with the entry count its descriptor asked for. */
void NoteFunctionTable(id table, id pipeline, const char *type, const char *cmd,
                       NSUInteger functionCount);

/**
 * One entry of an intersection or visible function table, as an `ObjectUpdate` on the table.
 *
 * `handle` is the MTLFunctionHandle set there, or nil to clear the entry. The whole table is sent
 * each time rather than one entry, because an ObjectUpdate is keyed and last-write-wins: a partial
 * update would lose the entries set before it.
 */
void NoteTableFunction(id table, NSUInteger index, id handle);

/** `setOpaqueTriangleIntersectionFunctionWithSignature:` and the curve form: a built-in, not a function. */
void NoteTableOpaqueFunction(id table, NSUInteger index, NSUInteger signature, const char *what);

/** A buffer bound to a table's own argument slots, which its intersection functions read through. */
void NoteTableBuffer(id table, NSUInteger index, id buffer, NSUInteger offset);

/** A visible function table bound into an intersection function table. */
void NoteTableVisibleTable(id table, NSUInteger index, id visible);

// ---------------------------------------------------------------------------------------------
// Hook installers (hooks.h holds the rest; these are the ray tracing classes)

/**
 * Hooks an acceleration structure command encoder's class: the builds, refits and copies, which
 * until now were a pass with no commands in it.
 */
void HookAccelerationStructureEncoderClass(id encoder);

/** Hooks a function table's class, so what the application sets in it is recorded. */
void HookFunctionTableClass(id table);

/**
 * Hooks a render or compute pipeline state's class: the two function table factories.
 *
 * Called from Track for every pipeline state, since a pipeline is the only thing that hands a
 * function table out and the application may ask for one at any time after creating it.
 */
void HookPipelineStateClass(id state, const char *type);

}  // namespace mtlinsp
