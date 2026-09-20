// DXR: what a state object holds, what an acceleration structure was built from, and what a trace
// runs. The D3D12 counterpart of the ray tracing half of src/vulkan/src/hooks.cpp.
//
// Three things make ray tracing legible, and D3D12 hides each of them behind a number:
//
//   Shader identifiers. A trace does not name the shaders it runs. It names four regions of
//   memory, and each record in them begins with the 32 opaque bytes the runtime gave for one of
//   the state object's exports (ID3D12StateObjectProperties::GetShaderIdentifier). So saying what
//   a record runs needs both halves: the identifiers, kept per export on the state object, and the
//   table's contents, read back from the addresses the trace points at. Vulkan's version of this
//   is vkGetRayTracingShaderGroupHandlesKHR, which hands out handles by group index; D3D12 hands
//   them out by export name, which is strictly more to go on.
//
//   Build inputs. A build reads its geometry and its instances from GPU virtual addresses, and an
//   address on its own says nothing. AddressMap (descriptors.h) resolves one to the buffer that
//   owns it, which is what lets the capture read back the vertices, indices and instance
//   descriptions a structure was actually built from — the only view there is of an object the
//   driver keeps opaque.
//
//   The structures themselves. This is where D3D12 differs from Vulkan in kind rather than in
//   spelling. A VkAccelerationStructureKHR is a handle the tracker owns and the UI can select; a
//   D3D12 acceleration structure is a range inside a UAV buffer, named only by the address a build
//   wrote it to. Nothing exists to attach a build to, and nothing links a top level's instances to
//   the bottom levels under them. So the library makes the object: StructureRegistry mints one
//   tracked "ID3D12RaytracingAccelerationStructure" per destination address the first time a build
//   writes there, and every later build, copy and instance reference resolves to it.
#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace dxinsp {

class CommandRecorder;

// ---------------------------------------------------------------------------------------------
// State objects

/**
 * Records what a state object holds, after the runtime has created it: the identifier of every
 * export the description names, the stack each one needs, and the DXIL libraries' bytecode as
 * blobs. Called from the CreateStateObject and AddToStateObject hooks.
 *
 * `grownFrom` is the state object an AddToStateObject grew from, whose exports the new one also
 * carries; null for a CreateStateObject.
 */
void NoteStateObject(ID3D12StateObject* stateObject, const D3D12_STATE_OBJECT_DESC* desc,
                     ID3D12StateObject* grownFrom);

/**
 * The export whose identifier is these bytes, or "". Kept for the UI to resolve a binding table
 * record with, and used by the replay through the capture's `shaderIdentifiers` update.
 *
 * `identifier` is D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES long.
 */
std::string ExportWithIdentifier(ID3D12StateObject* stateObject, const void* identifier);

// ---------------------------------------------------------------------------------------------
// Acceleration structures

/**
 * The tracked object for the structure a build wrote at `address`, minting one the first time that
 * address is written. Returns 0 for a null address.
 *
 * An acceleration structure has no COM object of its own, so the tracker is keyed by a sentinel
 * this owns rather than by an interface pointer, and the id is stable for as long as the address
 * is in use. A build to an address a structure already occupies replaces its contents, which is
 * the same object as far as the UI is concerned — an application rebuilding its top level every
 * frame writes the same address every time.
 */
uint64_t NoteStructureAt(D3D12_GPU_VIRTUAL_ADDRESS address, ID3D12Device* device);

/**
 * The structure already known at `address`, or 0. Nothing is minted: an address a descriptor names
 * or an instance points at is only a structure once something has built one there, and an address
 * with no build behind it is exactly what the UI should say nothing about.
 */
uint64_t StructureAt(D3D12_GPU_VIRTUAL_ADDRESS address);

/** The structure at `address` as a tracked reference, or null; for an instance naming its bottom level. */
void WriteStructureRef(JsonWriter& w, D3D12_GPU_VIRTUAL_ADDRESS address);

/**
 * Records a build on the structure it wrote, and queues its inputs for read-back.
 *
 * Returns the extra JSON for the command (`buildData`, the capture ids of the inputs, and
 * `destStructure`, the object the build wrote), or "" when nothing was captured. The ids go on the
 * command as well as on the structure because a structure's update is last-write-wins: an
 * application that rebuilds every frame overwrites the captured build's ids with a later build's,
 * which has none. This is the same reasoning as NoteAccelerationStructureBuilds in the Vulkan
 * layer, and the same defect it was written to avoid.
 */
std::string NoteAccelerationStructureBuild(CommandRecorder* rec,
                                           const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& desc,
                                           ID3D12Device* device);

/** A copy between structures: the destination takes the source's build, so the UI can still show it. */
void NoteAccelerationStructureCopy(D3D12_GPU_VIRTUAL_ADDRESS dest, D3D12_GPU_VIRTUAL_ADDRESS source,
                                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode, ID3D12Device* device);

// ---------------------------------------------------------------------------------------------
// Traces

/**
 * Queues a trace's four shader binding table regions for read-back. Returns the extra JSON for the
 * command (`bindingTableData`), or "" when none of them could be resolved.
 */
std::string NoteDispatchRays(CommandRecorder* rec, const D3D12_DISPATCH_RAYS_DESC& desc);

/** The state object a list last set with SetPipelineState1, so a trace knows whose identifiers to match. */
void NoteBoundStateObject(CommandRecorder* rec, ID3D12StateObject* stateObject);
ID3D12StateObject* BoundStateObject(CommandRecorder* rec);

/**
 * Drops the structures living in a buffer the application has released. Called before the buffer
 * leaves the AddressMap, which is what says which structures those are: an address outlives the
 * memory behind it, and the allocator hands the same one out again.
 */
void ForgetStructuresIn(ID3D12Resource* buffer);

/** Drops what a released state object exported; its properties interface's vtable stays patched. */
void ForgetStateObject(ID3D12StateObject* stateObject);

/** Drops everything the registries hold. Called when the device goes. */
void ResetRaytracing();

}  // namespace dxinsp
