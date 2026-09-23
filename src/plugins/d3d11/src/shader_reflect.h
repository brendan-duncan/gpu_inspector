// Shader bytecode: what stage a DXBC container is, and its reflection in the inspector's shape.
//
// Direct3D 11 takes DXBC only (fxc, shader model 5 and below), which d3dcompiler_47.dll, always in
// System32, reflects with D3DReflect. The inspector disassembles the bytecode itself (its shader
// tool reads DXBC), so only the reflection is made here, at shader creation.
//
// Reflection JSON, one object per stage, the shape the D3D12 library attaches to a pipeline state
// (src/d3d12/src/shader_reflect.h), which the inspector reads for any object carrying it:
//   { "entryPoint"?, "target": "vs_5_0", "inputs": [{name, semantic, index, type}], "outputs": [...],
//     "threadGroupSize"?: [x, y, z],
//     "resources": [{ "kind": "cbuffer" | "srv" | "uav" | "sampler", "name", "register", "space", "count",
//                     "dimension": "buffer" | "structured" | "byteaddress" | "texture1d" | "texture2d" | ...,
//                     "stride"?, "returnType"?, "type"?: ReflType (a cbuffer's or structured buffer's layout) }] }
#pragma once

#include "common.h"

#include <string>

namespace d3d11insp
{

struct ShaderInfo
{
    /** The inspector's stage name: vertex, fragment, tess_control, tess_eval, geometry, compute. */
    std::string stage;
    std::string target;   // "ps_5_0"
    std::string reflectionJson;   // "" when reflection failed
    std::string error;
};

/** Whether the bytes are a DXBC container ("DXBC" magic and a plausible size). */
bool IsShaderContainer(const void* bytecode, size_t size);

/** The stage and the reflection of a container. */
ShaderInfo ReflectShader(const void* bytecode, size_t size);

}  // namespace d3d11insp
