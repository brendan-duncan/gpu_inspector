// dxinsp_shader.exe: the text of a DXBC/DXIL container for the Inspect panel (d3d12/README.md,
// "Shaders"), from the same reflection code the capture library uses. app/src/main/shader_tools.ts
// runs it the way it runs spirv-dis.
//
//   dxinsp_shader --disassemble <file>   the disassembly, as text
//   dxinsp_shader --reflect <file>       the reflection JSON (shader_reflect.h)
//   dxinsp_shader --sources <file>       a JSON array of {"name", "text"}: the embedded HLSL (-Zi -Qembed_debug)
//   dxinsp_shader --info <file>          {"stage", "entryPoint", "target", "dxil"}
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
    fputs("usage: dxinsp_shader --disassemble|--reflect|--sources|--info <bytecode file>\n", stderr);
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

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return Usage();
    std::wstring mode = argv[1];
    std::vector<uint8_t> bytes;
    if (!ReadFile(argv[2], bytes)) return Fail("cannot read " + Narrow(argv[2]));
    if (!IsShaderContainer(bytes.data(), bytes.size())) return Fail(Narrow(argv[2]) + " is not a DXBC/DXIL container");
    // The disassembly and the sources may hold any UTF-8; a text-mode stdout would translate it.
    _setmode(_fileno(stdout), _O_BINARY);

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
        auto sources = EmbeddedSources(bytes.data(), bytes.size());
        JsonWriter w;
        w.BeginArray();
        for (auto& [name, text] : sources) {
            w.BeginObject();
            w.Key("name"); w.String(name);
            w.Key("text"); w.String(text);
            w.EndObject();
        }
        w.EndArray();
        WriteOut(w.str() + "\n");
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
        if (!info.error.empty()) { w.Key("error"); w.String(info.error); }
        w.EndObject();
        WriteOut(w.str() + "\n");
        return 0;
    }
    return Usage();
}
