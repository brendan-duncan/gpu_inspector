// dxinsp_shader.exe: the text of a DXBC/DXIL container for the Inspect panel (src/d3d12/README.md,
// "Shaders"), from the same reflection code the capture library uses. src/app/src/main/shader_tools.ts
// runs it the way it runs spirv-dis.
//
//   dxinsp_shader --disassemble <file>   the disassembly, as text
//   dxinsp_shader --reflect <file>       the reflection JSON (shader_reflect.h)
//   dxinsp_shader --sources <file> [--pdb <file>]... [--pdb-dir <dir>]...
//                                        a JSON array of {"name", "text"}: the HLSL dxc embedded
//                                        (-Zi), else the HLSL in the PDB it wrote beside the build
//                                        (-Zs with -Fd), named by --pdb or looked for under each
//                                        --pdb-dir. An entry read out of a PDB carries "from", the
//                                        file it came from. When there is no source anywhere the
//                                        array is empty and the reason goes to stderr, exit 0.
//                                        A last entry {"compile": {"mainFile", "entryPoint",
//                                        "target", "defines", "args"}} says how dxc was run, for
//                                        compiling the same source again (the shader debugger).
//   dxinsp_shader --info <file>          {"stage", "entryPoint", "target", "dxil", "debugName"}
//   dxinsp_shader --assemble <file.ll> --out <file>
//                                        a DXIL module's disassembly (as --disassemble prints it,
//                                        edited or not) assembled into a container, validated and
//                                        signed: a shader changed with no source for it. The
//                                        assembler's or the validator's message goes to stderr.
//
// Output goes to stdout as UTF-8, errors to stderr with exit code 1.
#include "common.h"
#include "shader_reflect.h"

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dxinsp;

namespace {

int Usage() {
    fputs("usage: dxinsp_shader --disassemble|--reflect|--sources|--info <bytecode file>\n"
          "       dxinsp_shader --assemble <module.ll> --out <container file>\n"
          "       --sources also takes --pdb <file> and --pdb-dir <dir>, repeatable, for a shader\n"
          "       built with -Zs whose source dxc wrote to a PDB instead of into the container\n", stderr);
    return 1;
}

bool ReadFile(const wchar_t* path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

void WriteOut(const std::string& s) {
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}

int Fail(const std::string& message) {
    fprintf(stderr, "dxinsp_shader: %s\n", message.c_str());
    return 1;
}

struct Options {
    std::wstring mode;
    std::wstring file;
    std::vector<std::wstring> pdbFiles;
    std::vector<std::wstring> pdbDirs;
    std::wstring out;
};

/** The command line: the mode, one bytecode file, and repeatable --pdb / --pdb-dir. */
bool ParseArgs(int argc, wchar_t** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        const bool wantsValue = arg == L"--pdb" || arg == L"--pdb-dir" || arg == L"--out";
        if (wantsValue && i + 1 >= argc) return false;
        if (arg == L"--pdb") out.pdbFiles.push_back(argv[++i]);
        else if (arg == L"--out") out.out = argv[++i];
        else if (arg == L"--pdb-dir") out.pdbDirs.push_back(argv[++i]);
        else if (arg.rfind(L"--", 0) == 0) {
            if (!out.mode.empty()) return false;
            out.mode = arg;
        } else {
            if (!out.file.empty()) return false;
            out.file = arg;
        }
    }
    return !out.mode.empty() && !out.file.empty();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!ParseArgs(argc, argv, options)) return Usage();
    std::vector<uint8_t> bytes;
    if (!ReadFile(options.file.c_str(), bytes)) return Fail("cannot read " + Narrow(options.file.c_str()));
    if (options.mode == L"--assemble") {
        if (options.out.empty()) return Usage();
        std::vector<uint8_t> container;
        std::string error;
        if (!AssembleDxil(std::string(bytes.begin(), bytes.end()), container, error)) return Fail(error);
        FILE* f = _wfopen(options.out.c_str(), L"wb");
        if (!f) return Fail("cannot write " + Narrow(options.out.c_str()));
        const bool wrote = fwrite(container.data(), 1, container.size(), f) == container.size();
        fclose(f);
        return wrote ? 0 : Fail("cannot write " + Narrow(options.out.c_str()));
    }
    if (!IsShaderContainer(bytes.data(), bytes.size())) return Fail(Narrow(options.file.c_str()) + " is not a DXBC/DXIL container");
    // The disassembly and the sources may hold any UTF-8; a text-mode stdout would translate it.
    _setmode(_fileno(stdout), _O_BINARY);

    const std::wstring& mode = options.mode;
    if (mode == L"--disassemble") {
        std::string text, error;
        if (!DisassembleShader(bytes.data(), bytes.size(), text, error)) return Fail(error);
        WriteOut(text);
        return 0;
    }
    if (mode == L"--reflect") {
        ShaderInfo info = ReflectShader(bytes.data(), bytes.size());
        if (info.reflectionJson.empty()) return Fail(info.error.empty() ? "no reflection" : info.error);
        WriteOut(info.reflectionJson + "\n");
        return 0;
    }
    if (mode == L"--sources") {
        ShaderSourceFiles sources = FindShaderSources(bytes.data(), bytes.size(), options.pdbFiles, options.pdbDirs);
        JsonWriter w;
        w.BeginArray();
        for (auto& [name, text] : sources.files) {
            w.BeginObject();
            w.Key("name"); w.String(name);
            w.Key("text"); w.String(text);
            // Where the text came from, for a source the container itself does not carry.
            if (!sources.pdb.empty()) { w.Key("from"); w.String(sources.pdb); }
            w.EndObject();
        }
        // How the files were compiled, after them so a reader wanting only the files can stop.
        const ShaderCompileInfo& c = sources.compile;
        if (!sources.files.empty() && (!c.mainFile.empty() || !c.defines.empty() || !c.args.empty())) {
            w.BeginObject();
            w.Key("compile"); w.BeginObject();
            w.Key("mainFile"); w.String(c.mainFile);
            w.Key("entryPoint"); w.String(c.entryPoint);
            w.Key("target"); w.String(c.target);
            w.Key("defines"); w.BeginArray();
            for (const std::string& d : c.defines) w.String(d);
            w.EndArray();
            w.Key("args"); w.BeginArray();
            for (const std::string& a : c.args) w.String(a);
            w.EndArray();
            w.EndObject();
            w.EndObject();
        }
        w.EndArray();
        WriteOut(w.str() + "\n");
        // An empty array is not a failure: the reason belongs with it, and the caller shows it.
        if (sources.files.empty() && !sources.note.empty()) fprintf(stderr, "%s\n", sources.note.c_str());
        return 0;
    }
    if (mode == L"--info") {
        ShaderInfo info = ReflectShader(bytes.data(), bytes.size());
        if (info.stage.empty()) return Fail(info.error.empty() ? "unrecognized container" : info.error);
        JsonWriter w;
        w.BeginObject();
        w.Key("stage"); w.String(info.stage);
        w.Key("entryPoint"); w.String(info.entryPoint);
        w.Key("target"); w.String(info.target);
        w.Key("dxil"); w.Boolean(info.dxil);
        // The PDB dxc wrote for this container, for finding it on this machine.
        std::string debugName = ShaderDebugName(bytes.data(), bytes.size());
        if (!debugName.empty()) { w.Key("debugName"); w.String(debugName); }
        w.Key("hash"); w.String(ShaderHashHex(bytes.data(), bytes.size()));
        if (!info.error.empty()) { w.Key("error"); w.String(info.error); }
        w.EndObject();
        WriteOut(w.str() + "\n");
        return 0;
    }
    return Usage();
}
