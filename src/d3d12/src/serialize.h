// JSON serializers for the D3D12 and DXGI structures the hooks record: the descriptors an object
// is created from, the arguments of a command, the views a descriptor holds. Written by hand, one
// per struct, in the shape the generated Vulkan serializers give the UI: every member under its
// own name, enums by name (gen/d3d12_enums.gen.h), flags as "A | B", nested structs as objects,
// fixed arrays as arrays, `Num*`/`p*` pairs as arrays of the pointed-to structs, and object
// pointers as tracked references. Shader bytecode is summarized ({"__bytes": N}); the bytes
// themselves go into blobs (tracker.h).
//
// Every writer takes the JsonWriter positioned at a value (after Key()) and writes one value.
#pragma once

#include "common.h"

namespace dxinsp {

/**
 * An enum as the value the generated name tables hold: every D3D12/DXGI enum is 32 bits, and the
 * tables store a 0xffffffff entry (D3D12_BARRIER_LAYOUT_UNDEFINED) as 4294967295 where MSVC's
 * int-backed enum reads -1. Use it for `w.Enum(ToString_X(EnumValue(v)), EnumValue(v))`.
 */
template <typename T>
inline int64_t EnumValue(T v) { return (int64_t)(uint32_t)(int64_t)v; }

// DXGI
void Write(JsonWriter& w, const DXGI_SAMPLE_DESC& v);
void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_DESC& v);
void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_DESC1& v);
void Write(JsonWriter& w, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC& v);
void Write(JsonWriter& w, const DXGI_ADAPTER_DESC3& v);
void Write(JsonWriter& w, const DXGI_ADAPTER_DESC1& v);
void Write(JsonWriter& w, const DXGI_PRESENT_PARAMETERS& v);

// Resources and heaps
void Write(JsonWriter& w, const D3D12_HEAP_PROPERTIES& v);
void Write(JsonWriter& w, const D3D12_HEAP_DESC& v);
void Write(JsonWriter& w, const D3D12_RESOURCE_DESC& v);
void Write(JsonWriter& w, const D3D12_RESOURCE_DESC1& v);
void Write(JsonWriter& w, const D3D12_CLEAR_VALUE* v, DXGI_FORMAT formatHint);
void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_VALUE& v);
void Write(JsonWriter& w, const D3D12_RANGE* v);
void Write(JsonWriter& w, const D3D12_BOX* v);
void Write(JsonWriter& w, const D3D12_TEXTURE_COPY_LOCATION& v);
void Write(JsonWriter& w, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& v);
void Write(JsonWriter& w, const D3D12_SUBRESOURCE_FOOTPRINT& v);
void Write(JsonWriter& w, const D3D12_TILED_RESOURCE_COORDINATE& v);
void Write(JsonWriter& w, const D3D12_TILE_REGION_SIZE& v);

// Views and samplers
void Write(JsonWriter& w, const D3D12_CONSTANT_BUFFER_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D12_SHADER_RESOURCE_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D12_UNORDERED_ACCESS_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D12_RENDER_TARGET_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_VIEW_DESC* v);
void Write(JsonWriter& w, const D3D12_SAMPLER_DESC& v);
void Write(JsonWriter& w, const D3D12_SAMPLER_DESC2& v);
void Write(JsonWriter& w, const D3D12_VERTEX_BUFFER_VIEW& v);
void Write(JsonWriter& w, const D3D12_INDEX_BUFFER_VIEW& v);
void Write(JsonWriter& w, const D3D12_STREAM_OUTPUT_BUFFER_VIEW& v);

// Pipelines
void Write(JsonWriter& w, const D3D12_SHADER_BYTECODE& v);
void Write(JsonWriter& w, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& v);
void Write(JsonWriter& w, const D3D12_COMPUTE_PIPELINE_STATE_DESC& v);
/** A pipeline state stream, subobject by subobject, under the same member names as the graphics/compute descs. */
void Write(JsonWriter& w, const D3D12_PIPELINE_STATE_STREAM_DESC& v);
void Write(JsonWriter& w, const D3D12_INPUT_LAYOUT_DESC& v);
void Write(JsonWriter& w, const D3D12_INPUT_ELEMENT_DESC& v);
void Write(JsonWriter& w, const D3D12_STREAM_OUTPUT_DESC& v);
void Write(JsonWriter& w, const D3D12_BLEND_DESC& v);
void Write(JsonWriter& w, const D3D12_RENDER_TARGET_BLEND_DESC& v);
void Write(JsonWriter& w, const D3D12_RASTERIZER_DESC& v);
void Write(JsonWriter& w, const D3D12_RASTERIZER_DESC1& v);
void Write(JsonWriter& w, const D3D12_RASTERIZER_DESC2& v);
void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_DESC& v);
void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_DESC1& v);
void Write(JsonWriter& w, const D3D12_DEPTH_STENCIL_DESC2& v);
void Write(JsonWriter& w, const D3D12_DEPTH_STENCILOP_DESC& v);
void Write(JsonWriter& w, const D3D12_DEPTH_STENCILOP_DESC1& v);
void Write(JsonWriter& w, const D3D12_RT_FORMAT_ARRAY& v);
void Write(JsonWriter& w, const D3D12_VIEW_INSTANCING_DESC& v);
void Write(JsonWriter& w, const D3D12_CACHED_PIPELINE_STATE& v);
void Write(JsonWriter& w, const D3D12_STATE_OBJECT_DESC& v);

// Root signatures and descriptor heaps
void Write(JsonWriter& w, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& v);
void Write(JsonWriter& w, const D3D12_ROOT_SIGNATURE_DESC& v);
void Write(JsonWriter& w, const D3D12_ROOT_SIGNATURE_DESC1& v);
void Write(JsonWriter& w, const D3D12_ROOT_SIGNATURE_DESC2& v);
void Write(JsonWriter& w, const D3D12_STATIC_SAMPLER_DESC& v);
void Write(JsonWriter& w, const D3D12_STATIC_SAMPLER_DESC1& v);
void Write(JsonWriter& w, const D3D12_DESCRIPTOR_HEAP_DESC& v);
void Write(JsonWriter& w, const D3D12_COMMAND_QUEUE_DESC& v);
void Write(JsonWriter& w, const D3D12_QUERY_HEAP_DESC& v);
void Write(JsonWriter& w, const D3D12_COMMAND_SIGNATURE_DESC& v);
void Write(JsonWriter& w, const D3D12_INDIRECT_ARGUMENT_DESC& v);

// Commands
void Write(JsonWriter& w, const D3D12_VIEWPORT& v);
void Write(JsonWriter& w, const D3D12_RECT& v);
void Write(JsonWriter& w, const D3D12_RESOURCE_BARRIER& v);
void Write(JsonWriter& w, const D3D12_BARRIER_GROUP& v);
void Write(JsonWriter& w, const D3D12_RENDER_PASS_RENDER_TARGET_DESC& v);
void Write(JsonWriter& w, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC& v);
void Write(JsonWriter& w, const D3D12_RENDER_PASS_BEGINNING_ACCESS& v);
void Write(JsonWriter& w, const D3D12_RENDER_PASS_ENDING_ACCESS& v);
void Write(JsonWriter& w, const D3D12_DISPATCH_RAYS_DESC& v);
void Write(JsonWriter& w, const D3D12_GPU_VIRTUAL_ADDRESS_RANGE& v);
void Write(JsonWriter& w, const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& v);
/** One geometry of a bottom level build; the build's own record of what it read (raytracing.cpp) uses it too. */
void Write(JsonWriter& w, const D3D12_RAYTRACING_GEOMETRY_DESC& v);
void Write(JsonWriter& w, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& v);
void Write(JsonWriter& w, const D3D12_DISCARD_REGION* v);
void Write(JsonWriter& w, const D3D12_SAMPLE_POSITION& v);
void Write(JsonWriter& w, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER& v);
void Write(JsonWriter& w, const D3D12_DISPATCH_GRAPH_DESC& v);
void Write(JsonWriter& w, const D3D12_SET_PROGRAM_DESC& v);

// Device features (device_info.cpp writes these as the device's "features" update)
void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS& v);
void Write(JsonWriter& w, const D3D12_FEATURE_DATA_ARCHITECTURE1& v);

/** A fixed array of enum values (RTVFormats), `count` of them. */
void WriteFormats(JsonWriter& w, const DXGI_FORMAT* formats, uint32_t count);
/** A GUID as "{xxxxxxxx-....}". */
void Write(JsonWriter& w, const GUID& v);
/** A LUID as its two words in hex. */
void Write(JsonWriter& w, const LUID& v);

}  // namespace dxinsp
