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

/**
 * One output of a shader's signature, as a stream-output declaration entry needs it
 * (D3D12_SO_DECLARATION_ENTRY): the mesh view's VS Out streams a vertex shader's own outputs out of
 * the unmodified bytecode, so nothing but the signature is needed to ask for them.
 */
struct ShaderOutputParam {
    std::string semantic;
    uint32_t semanticIndex = 0;
    uint32_t startComponent = 0;
    uint32_t componentCount = 0;
    /** "float", "int" or "uint", as the UI names the record's fields. */
    std::string base;
    /** The system value it carries ("SV_Position" and the rest), empty for an ordinary output. */
    std::string systemValue;
};

/** The output signature of a shader container, empty when it could not be reflected. */
std::vector<ShaderOutputParam> ShaderOutputSignature(const void* bytecode, size_t size);

/** Whether the bytes are a DXBC/DXIL container ("DXBC" magic and a plausible size). */
bool IsShaderContainer(const void* bytecode, size_t size);

/** The stage and, when available, the entry point and reflection of a container. */
ShaderInfo ReflectShader(const void* bytecode, size_t size);

/** The container's disassembly; false with `error` when no disassembler is available. */
bool DisassembleShader(const void* bytecode, size_t size, std::string& text, std::string& error);

/**
 * The other direction, for DXIL: a module's disassembly (LLVM IR as text, edited or not) assembled
 * into a container, validated and signed, which is what makes it a shader the runtime will take.
 * This is how a shader is changed with no source for it (the app's dxil_ablate.ts): dxc has no
 * editor for DXIL, but it has both halves of one. False with `error` holding the assembler's or
 * the validator's own message, which names the line.
 */
bool AssembleDxil(const std::string& text, std::vector<uint8_t>& container, std::string& error);

/**
 * How dxc was run, as the debug information records it: what a compile of the same source again
 * needs (the shader debugger compiles the HLSL to SPIR-V to step it). Empty where the container
 * or PDB does not say (DXBC from fxc, an old dxc).
 */
struct ShaderCompileInfo {
    /** The file dxc was given, as it was named on the command line. */
    std::string mainFile;
    std::string entryPoint;
    std::string target;
    /** -D defines, "NAME" or "NAME=VALUE". */
    std::vector<std::string> defines;
    /** The other arguments (-HV, -enable-16bit-types, -O3, ...), without the defines and the file options. */
    std::vector<std::string> args;
};

/** The source files embedded in a container compiled with -Zi (or a DXBC with debug info): (name, text). */
std::vector<std::pair<std::string, std::string>> EmbeddedSources(const void* bytecode, size_t size);

/** The same, with how they were compiled read out beside them; `compile` may be null. */
std::vector<std::pair<std::string, std::string>> EmbeddedSources(const void* bytecode, size_t size, ShaderCompileInfo* compile);

/** Where a container's HLSL was found, and why there is none when there is none. */
struct ShaderSourceFiles {
    std::vector<std::pair<std::string, std::string>> files;
    /** The PDB the files were read out of; "" when they were embedded in the container itself. */
    std::string pdb;
    /** Why nothing was found: the PDB dxc named, and where it was looked for. */
    std::string note;
    /** How the files were compiled, from the same debug information. */
    ShaderCompileInfo compile;
};

/**
 * The name dxc wrote into the container for its PDB, from the ILDN part ("<hash>.pdb"); "" when
 * the container has no debug name (built without -Zi/-Zs). Both -Zi and -Zs write it, so it is
 * what says which file on this machine holds a -Zs build's source.
 */
std::string ShaderDebugName(const void* bytecode, size_t size);

/**
 * The container's shader hash as lowercase hex: the HASH part's digest, which is the name dxc
 * gives the PDB, and falls back to the 16 bytes in the container header.
 */
std::string ShaderHashHex(const void* bytecode, size_t size);

/** The source files a PDB file carries; `error` says why when it gives none. */
std::vector<std::pair<std::string, std::string>> PdbSources(const std::wstring& path, std::string& error);

/**
 * The HLSL of a container: embedded in it (-Zi), else out of the PDB dxc wrote beside the build
 * (-Zs with -Fd <dir>\), either one of `pdbFiles` or one found in `pdbDirs` — by the name the
 * container carries, then by the hash of every .pdb lying in the directory.
 */
ShaderSourceFiles FindShaderSources(const void* bytecode, size_t size,
                                    const std::vector<std::wstring>& pdbFiles,
                                    const std::vector<std::wstring>& pdbDirs);

/** A stable hash of the bytecode, for naming and matching. */
uint64_t ShaderHash(const void* bytecode, size_t size);

}  // namespace dxinsp
