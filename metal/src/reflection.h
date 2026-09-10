// Pipeline reflection: what each stage of a pipeline binds, and the memory layout of every
// buffer it reads, as the UI's own type model.
//
// The counterpart of the UI's SPIR-V reflection (app/src/renderer/vulkan/spirv_reflect.ts),
// which is what turns a captured buffer's bytes into named fields. Metal has no SPIR-V to parse,
// but a pipeline created with the argument-info and buffer-type-info options comes back with an
// MTLRenderPipelineReflection that says the same things: per stage, the buffers, textures and
// samplers by index, and for each buffer the struct it points at, member by member. The hooks
// ask for it on every pipeline the application creates, whether or not the application did,
// and it rides along in the pipeline's descriptor as `reflection`.
//
// The layout types are written in the shape spirv_reflect.ts's ReflType has — scalar, vector,
// matrix, array, struct, opaque, with sizes and offsets — so the buffer views, the Format editor
// and the Reflection section need no Metal-specific reader.
#pragma once

#include <string>

#import <Metal/Metal.h>

namespace mtlinsp {

/**
 * JSON of a render pipeline's reflection: an object keyed by stage ("vertex", "fragment",
 * "tile", "object", "mesh"), each with `buffers`, `textures`, `samplers` and `threadgroup`
 * arrays. "" for nil.
 */
std::string RenderReflectionJson(MTLRenderPipelineReflection *reflection);

/** As above for a compute pipeline, keyed by "compute". */
std::string ComputeReflectionJson(MTLComputePipelineReflection *reflection);

}  // namespace mtlinsp
