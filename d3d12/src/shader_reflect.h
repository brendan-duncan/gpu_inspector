// Shader bytecode: what stage a DXBC/DXIL container is, its reflection in the UI's shape, its
// disassembly, and the source embedded in it. Shared by the capture library (reflection at
// pipeline creation) and dxinsp_shader.exe (text for the Inspect panel).
//
// DXBC (fxc, shader model 5 and below) is read with d3dcompiler_47.dll, always in System32:
// D3DReflect and D3DDisassemble. DXIL (dxc, shader model 6) needs dxcompiler.dll, which is looked
// for beside this module, in the Vulkan SDK's Bin, in the Windows SDK's bin\<version>\x64 and on
// PATH; without it a DXIL shader has no reflection and no text, and the error says what to install.
//
// Reflection JSON (README.md, "Shaders"), one object per stage:
//   { "entryPoint"?, "target": "vs_6_0", "inputs": [{name, semantic, index, type}], "outputs": [...],
//     "threadGroupSize"?: [x, y, z],
//     "resources": [{ "kind": "cbuffer" | "srv" | "uav" | "sampler", "name", "register", "space", "count",
//                     "dimension": "buffer" | "structured" | "byteaddress" | "texture1d" | "texture2d" | ... | "accelerationStructure",
//                     "stride"?, "returnType"?, "type"?: ReflType (a cbuffer's or structured buffer's layout) }] }
// where ReflType is spirv_reflect.ts's: {kind: "scalar", base, width, size} | {kind: "vector", element, count, size}
// | {kind: "matrix", element, columns, rows, stride, rowMajor, size} | {kind: "array", element, count, stride, size}
// | {kind: "struct", name, members: [{name, offset, type}], size} | {kind: "opaque", name}.
#pragma once

#include "common.h"

#include <string>
#include <utility>
#include <vector>

namespace dxinsp {

struct ShaderInfo {
    /** The UI's stage name: vertex, fragment, tess_control, tess_eval, geometry, compute, task, mesh, or "library". */
    std::string stage;
    std::string entryPoint;   // "" when the container does not say (DXBC)
    std::string target;       // "ps_6_0"
    bool dxil = false;
    std::string reflectionJson;   // "" when reflection failed
    std::string error;
};

/** Whether the bytes are a DXBC/DXIL container ("DXBC" magic and a plausible size). */
bool IsShaderContainer(const void* bytecode, size_t size);

/** The stage and, when available, the entry point and reflection of a container. */
ShaderInfo ReflectShader(const void* bytecode, size_t size);

/** The container's disassembly; false with `error` when no disassembler is available. */
bool DisassembleShader(const void* bytecode, size_t size, std::string& text, std::string& error);

/** The source files embedded in a container compiled with -Zi -Qembed_debug (or a DXBC with debug info): (name, text). */
std::vector<std::pair<std::string, std::string>> EmbeddedSources(const void* bytecode, size_t size);

/** A stable hash of the bytecode, for naming and matching. */
uint64_t ShaderHash(const void* bytecode, size_t size);

}  // namespace dxinsp
