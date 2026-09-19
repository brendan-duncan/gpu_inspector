#include "dx_exporter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "dx_replayer.h"

namespace dxreplay {

// The exported project's hand-written files (src/d3d12/replay/export_template, embedded by tools/embed_files.py).
struct EmbeddedFile { const char* name; const char* const* pieces; size_t count; };
extern const EmbeddedFile kDxExportTemplates[];
extern const size_t kDxExportTemplatesCount;

namespace {

/** Lines in one generated function and in one file; the Vulkan exporter's overrides apply here too. */
size_t LimitFromEnvironment(const char* name, size_t fallback) {
    const char* text = std::getenv(name);
    const long long value = text ? std::atoll(text) : 0;
    return value > 0 ? (size_t)value : fallback;
}
const size_t kPartLines = LimitFromEnvironment("VKINSP_EXPORT_PART_LINES", 2000);
const size_t kFileLines = LimitFromEnvironment("VKINSP_EXPORT_FILE_LINES", 24000);

std::string Template(const char* name) {
    for (size_t i = 0; i < kDxExportTemplatesCount; ++i) {
        if (std::strcmp(kDxExportTemplates[i].name, name) != 0) continue;
        std::string out;
        for (size_t p = 0; p < kDxExportTemplates[i].count; ++p) out += kDxExportTemplates[i].pieces[p];
        return out;
    }
    return std::string();
}

std::string Hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return buf;
}

uint64_t HashBytes(const void* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string Trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \n");
    const size_t b = s.find_last_not_of(" \n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

DxExporter::DxExporter(std::string directory, const vkreplay::CaptureFile& capture) : _dir(std::move(directory)), _capture(capture) {
    for (Part* p : {&_create, &_contents, &_frame}) Configure(p->writer);
}

DxExporter::~DxExporter() {
    if (_data) std::fclose(_data);
}

void DxExporter::Configure(Source& w) {
    w.objectName = [this](IUnknown* object) { return NameOf(object); };
    w.data = [this](const void* data, size_t size) { return Data(data, size); };
    w.addressExpr = [this](D3D12_GPU_VIRTUAL_ADDRESS address) -> std::string {
        for (const BufferRange& b : _buffers)
            if (address >= b.address && address < b.address + std::max<uint64_t>(b.size, 1)) {
                const uint64_t offset = address - b.address;
                return b.name + "->GetGPUVirtualAddress()" + (offset ? " + " + std::to_string(offset) : std::string());
            }
        return std::string();
    };
    w.cpuHandleExpr = [this](D3D12_CPU_DESCRIPTOR_HANDLE handle) -> std::string {
        for (const HeapRange& h : _heaps)
            if (h.increment && handle.ptr >= h.cpu && handle.ptr < h.cpu + (SIZE_T)h.increment * h.count)
                return "CpuHandle(" + h.name + ", " + std::to_string((handle.ptr - h.cpu) / h.increment) + ")";
        return std::string();
    };
    w.indent = 1;
}

bool DxExporter::Open(std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(_dir, ec);
    if (ec) {
        error = "could not create " + _dir + ": " + ec.message();
        return false;
    }
    const std::string path = _dir + "/frame_data.bin";
    _data = std::fopen(path.c_str(), "wb");
    if (!_data) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Names and data

std::string DxExporter::Declare(const std::string& type, const std::string& stem, uint64_t id, IUnknown* object) {
    const std::string name = stem + "_" + std::to_string(id);
    _globals.emplace_back(type, name);
    _names[object] = name;
    return name;
}

void DxExporter::Alias(IUnknown* object, const std::string& name) { _names[object] = name; }

std::string DxExporter::NameOf(IUnknown* object) const {
    auto it = _names.find(object);
    return it == _names.end() ? std::string() : it->second;
}

void DxExporter::Heap(ID3D12DescriptorHeap* heap, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu, uint32_t increment, uint32_t count) {
    _heaps.push_back({NameOf(heap), cpu.ptr, gpu.ptr, increment, count});
}

void DxExporter::Buffer(ID3D12Resource* buffer, D3D12_GPU_VIRTUAL_ADDRESS address, uint64_t size) {
    _buffers.push_back({NameOf(buffer), address, size});
}

std::string DxExporter::GpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) const {
    for (const HeapRange& h : _heaps)
        if (h.gpu && h.increment && handle.ptr >= h.gpu && handle.ptr < h.gpu + (UINT64)h.increment * h.count)
            return "GpuHandle(" + h.name + ", " + std::to_string((handle.ptr - h.gpu) / h.increment) + ")";
    return "D3D12_GPU_DESCRIPTOR_HANDLE{}";
}

std::string DxExporter::Data(const void* data, size_t size) {
    if (!data || !size || !_data) return "nullptr";
    const uint64_t hash = HashBytes(data, size);
    auto& known = _blobs[hash];
    for (const Blob& b : known)
        if (b.size == size) return "Data(" + Hex(b.offset) + ", " + std::to_string(size) + ")";
    static const char zeros[16] = {};
    const uint64_t padded = (_dataSize + 15) & ~15ull;
    if (padded != _dataSize) std::fwrite(zeros, 1, (size_t)(padded - _dataSize), _data);
    std::fwrite(data, 1, size, _data);
    known.push_back({padded, size});
    _dataSize = padded + size;
    return "Data(" + Hex(padded) + ", " + std::to_string(size) + ")";
}

// ---------------------------------------------------------------------------------------------
// Statements

DxExporter::Part& DxExporter::PartOf(Section section) { return section == Create ? _create : section == Contents ? _contents : _frame; }

void DxExporter::MaybeSplit(Part& p) {
    if (p.writer.Lines() >= kPartLines) SplitNow(p);
}

void DxExporter::SplitNow(Part& p) {
    if (p.writer.text.empty()) return;
    p.parts.push_back(std::move(p.writer.text));
    p.writer.ResetPart();
}

void DxExporter::Block(Section section, const std::string& label, const std::function<void(Source&)>& body) {
    Part& part = PartOf(section);
    Source& w = part.writer;
    Source scratch;
    Configure(scratch);
    scratch.indent = w.indent + 1;
    body(scratch);
    for (const std::string& n : scratch.notes) w.Note(n);
    if (scratch.Statements() == 1) {
        const std::string statement = Trimmed(scratch.text);
        // A one-time list declared in one statement stays in scope for the ones after it.
        w.Line(statement + (label.empty() ? "" : "   // " + label));
    } else if (scratch.Statements()) {
        w.Line("{" + (label.empty() ? std::string() : "   // " + label));
        scratch.notes.clear();
        w.Append(scratch);
        w.Line("}");
    }
    // The contents section ends with a one-time list held open across its statements, so it is only
    // cut after an upload, which is a block of its own and comes before that list.
    if (section != Contents || (!label.empty() && scratch.Statements() > 1)) MaybeSplit(part);
}

void DxExporter::Comment(Section section, const std::string& text) { PartOf(section).writer.Comment(text); }
void DxExporter::Blank(Section section) { PartOf(section).writer.Blank(); }

void DxExporter::EndSubmission() {
    if (_frame.writer.Lines() >= kPartLines / 2) SplitNow(_frame);
}

void DxExporter::LeftOut(uint32_t index, const std::string& method, const std::string& why) {
    _frame.writer.Comment("[" + std::to_string(index) + "] " + method + ": left out: " + why);
    ++_leftOut;
}

void DxExporter::Note(const std::string& note) { _notes.push_back(note); }

void DxExporter::Device(const std::string& capturedAdapter, const std::string& replayAdapter, const std::string& featureLevel) {
    _capturedAdapter = capturedAdapter;
    _replayAdapter = replayAdapter;
    std::string s = "void CreateDevice(bool debugLayer) {\n";
    if (!capturedAdapter.empty() && capturedAdapter != replayAdapter) s += "    // Captured on " + capturedAdapter + "; this source was exported from a replay on " + replayAdapter + ".\n";
    s += "    // The adapter by name where this machine has it, else the first hardware one.\n";
    s += "    CreateDeviceOn(" + Source::String((capturedAdapter.empty() ? replayAdapter : capturedAdapter).c_str()) + ", " + featureLevel + ", debugLayer);\n";
    s += "}\n";
    _deviceSource = s;
}

// ---------------------------------------------------------------------------------------------
// Files

bool DxExporter::WriteText(const std::string& name, const std::string& text, std::string& error) {
    const std::string path = _dir + "/" + name;
    std::ofstream out(path, std::ios::binary);
    if (out) out.write(text.data(), (std::streamsize)text.size());
    if (!out) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

bool DxExporter::WritePart(Part& p, std::vector<std::string>& files, std::string& error) {
    SplitNow(p);
    for (const std::string& n : p.writer.notes) _notes.push_back(n);
    const std::string header = "// Generated by GPU Inspector's Export to C++ (dxinsp_replay --export). See README.md.\n"
                               "#include \"dx_support.h\"\n#include \"frame_objects.h\"\n\n";
    std::vector<std::string> texts(1, header);
    std::vector<size_t> lines(1, 0);
    std::string declarations, calls;
    for (size_t i = 0; i < p.parts.size(); ++i) {
        const std::string name = p.function + "_" + std::to_string(i + 1);
        const size_t count = (size_t)std::count(p.parts[i].begin(), p.parts[i].end(), '\n');
        if (lines.back() && lines.back() + count > kFileLines) {
            texts.push_back(header);
            lines.push_back(0);
        }
        texts.back() += "void " + name + "() {\n" + p.parts[i] + "}\n\n";
        lines.back() += count + 3;
        declarations += "void " + name + "();\n";
        calls += "    " + name + "();\n";
    }
    texts[0].insert(header.size(), declarations + (declarations.empty() ? "" : "\n") + "void " + p.function + "() {\n" + calls + "}\n\n");
    for (size_t f = 0; f < texts.size(); ++f) {
        const std::string name = p.file + (f ? "_" + std::to_string(f + 1) : "") + ".cpp";
        if (!WriteText(name, texts[f], error)) return false;
        files.push_back(name);
    }
    return true;
}

std::string DxExporter::Readme(const DxReplayReport& report, const DxExportReport& summary) const {
    auto text = [&](const char* key) {
        const vkreplay::JValue* v = _capture.Manifest().Get(key);
        return v && v->IsString() ? std::string(v->Str()) : std::string();
    };
    std::string s = "# An exported frame\n\n";
    s += "This project re-creates one captured frame of a Direct3D 12 application and runs it again: the\n"
         "frame's objects, what its textures and buffers held, and every command of its command lists, as\n"
         "plain C++ with no dependency but the Windows SDK. It was written by GPU Inspector's Export to C++\n"
         "(`dxinsp_replay --export`) from a capture, for reproducing a problem outside the application.\n\n";
    s += "| | |\n|---|---|\n";
    // The manifest's `application` is what wrote the file (GPU Inspector); the captured application is its `source`.
    std::string application;
    if (const auto* source = _capture.Manifest().Get("source"); source && source->Get("name") && source->Get("name")->IsString())
        application = std::string(source->Get("name")->Str());
    if (!application.empty()) s += "| Application | `" + application + "` |\n";
    if (!text("savedAt").empty()) s += "| Captured | " + text("savedAt") + " |\n";
    if (!_capturedAdapter.empty()) s += "| Captured on | " + _capturedAdapter + " |\n";
    s += "| Exported from a replay on | " + _replayAdapter + " |\n";
    s += "| Objects | " + std::to_string(summary.objects) + " |\n";
    s += "| Commands | " + std::to_string(summary.commands) + " in " + std::to_string(summary.submissions) + " submission" + (summary.submissions == 1 ? "" : "s") + " |\n";
    s += "| Render targets compared | " + std::to_string(summary.targets) + " |\n\n";
    s += "## Build and run\n\n```\ncmake -B build\ncmake --build build --config Release\nbuild\\Release\\frame.exe\n```\n\n"
         "It needs CMake and Visual Studio (or clang-cl) with the Windows SDK, and nothing else.\n\n"
         "- `--debug-layer` enables the D3D12 debug layer and prints its messages.\n"
         "- `--out <directory>` is where the render targets are written (default `out`), `--no-images` writes none.\n"
         "- `--data <file>` names `frame_data.bin` when it is not beside the executable.\n\n"
         "The program prints each render target the capture read back, compared byte for byte with the copy\n"
         "the capture holds, and writes both as PNG where the format allows. It exits with 0 when every target\n"
         "is identical, 1 when some differ or could not be compared, and 2 when a call failed (the call is printed).\n\n";
    s += "## What is in it\n\n"
         "| File | |\n|---|---|\n"
         "| `frame_create*.cpp` | `CreateObjects`: every object the frame uses, in the order it was created. |\n"
         "| `frame_contents*.cpp` | `UploadContents`: what the frame's textures held, and the state it expects each resource in. |\n"
         "| `frame_commands*.cpp` | `Frame`: each submission's buffer contents, descriptors, command lists and execution. |\n"
         "| `frame_objects.h` | One variable per object, named by kind and capture id: `texture_36` is resource 36 in GPU Inspector. |\n"
         "| `frame_data.bin` | Shader bytecode, texture and buffer contents, and the captured render targets. |\n"
         "| `dx_support.*`, `main.cpp` | Not specific to the frame: the device, uploads, descriptor handles, read-backs. |\n\n"
         "A number in brackets after a command (`// [17]`) is its index in the capture's command list. A struct is\n"
         "declared zeroed and only the members that are set are assigned.\n\n";
    s += "## How it differs from the application\n\n"
         "The source is what GPU Inspector's replay did with the capture, which is the application's frame with\n"
         "these differences:\n\n"
         "- Every resource is committed, with memory of its own, whatever heap the application placed it in.\n"
         "- Swap chain buffers are ordinary render target textures; there is no swap chain or window.\n"
         "- A capture records no resource states and no descriptor writes, which happen before the frame. Each\n"
         "  resource starts in the state the frame's first barrier says it was in, or that its uses need, and each\n"
         "  descriptor is written from what the capture says it held when it was bound.\n"
         "- Pipelines take their shaders from the bytecode the capture kept, and root signatures are serialized\n"
         "  again from their descriptions.\n"
         "- Buffers hold the ranges the frame read; nothing else of the application's memory is in the capture.\n"
         "- Each submission is waited for before the next; fences and presents are left out.\n\n";
    if (summary.leftOut || !report.problems.empty() || !summary.notes.empty()) {
        s += "## What the replay left out or reported\n\n";
        if (summary.leftOut) s += "- " + std::to_string(summary.leftOut) + " command(s) are left out, each with a comment where it would be in `frame_commands*.cpp`.\n";
        std::map<std::string, int> grouped;
        for (const std::string& p : report.problems) ++grouped[p];
        for (const std::string& n : summary.notes) ++grouped[n];
        size_t shown = 0;
        for (const auto& [p, n] : grouped) {
            if (++shown > 60) {
                s += "- ... " + std::to_string(grouped.size() - 60) + " more\n";
                break;
            }
            s += "- " + p + (n > 1 ? " (x" + std::to_string(n) + ")" : "") + "\n";
        }
        s += "\n";
    }
    if (!report.targets.empty()) {
        s += "## The replay's own result\n\nWhat the replay this was exported from got on " + _replayAdapter + ", for comparison with a run of this program:\n\n";
        for (const DxTargetComparison& t : report.targets) {
            s += "- resource " + std::to_string(t.resource) + " (" + t.format + " " + std::to_string(t.width) + "x" + std::to_string(t.height) + " " + t.aspect + "): ";
            if (!t.compared) s += "not compared: " + t.note + "\n";
            else if (!t.differingTexels) s += "identical to the capture\n";
            else s += std::to_string(t.differingTexels) + " of " + std::to_string(t.texels) + " texels differ\n";
        }
        s += "\n";
    }
    return s;
}

bool DxExporter::Finish(const DxReplayReport& report, DxExportReport& out) {
    out.requested = true;
    out.directory = _dir;
    if (_data) {
        std::fclose(_data);
        _data = nullptr;
    }
    std::vector<std::string> sources = {"main.cpp", "dx_support.cpp", "frame_objects.cpp"};
    for (Part* p : {&_create, &_contents, &_frame})
        if (!WritePart(*p, sources, out.error)) return false;

    std::string header = "// One variable per object of the capture, named by its kind and its id in the capture (the id GPU\n"
                         "// Inspector shows), and the functions the frame is made of.\n#pragma once\n\n#include \"dx_support.h\"\n\n";
    std::string source = "#include \"frame_objects.h\"\n\n";
    std::string release = "void ReleaseObjects() {\n";
    for (const auto& [type, name] : _globals) {
        header += "extern " + type + "* " + name + ";\n";
        source += type + "* " + name + " = nullptr;\n";
    }
    for (auto it = _globals.rbegin(); it != _globals.rend(); ++it) release += "    if (" + it->second + ") " + it->second + "->Release();\n";
    release += "}\n";
    header += "\nvoid CreateDevice(bool debugLayer);\nvoid CreateObjects();\nvoid UploadContents();\nvoid Frame();\nvoid ReleaseObjects();\n";
    source += "\n" + _deviceSource + "\n" + release;

    out.objects = _objects;
    out.commands = _commands;
    out.submissions = _submissions;
    out.targets = _targets;
    out.leftOut = _leftOut;
    out.dataBytes = _dataSize;
    std::sort(_notes.begin(), _notes.end());
    _notes.erase(std::unique(_notes.begin(), _notes.end()), _notes.end());
    out.notes = _notes;

    std::string list;
    for (const std::string& f : sources) list += "    " + f + "\n";
    const std::string cmake =
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(frame CXX)\n\n"
        "# A frame exported from a GPU Inspector capture (README.md). Direct3D 12: Windows, with the Windows SDK.\n"
        "set(CMAKE_CXX_STANDARD 20)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n"
        "add_executable(frame\n" + list + ")\n"
        "target_compile_definitions(frame PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)\n"
        "if(MSVC)\n    target_compile_options(frame PRIVATE /W3 /bigobj)\nendif()\n"
        "target_link_libraries(frame PRIVATE d3d12 dxgi dxguid)\n\n"
        "# The data file beside the executable, where it looks for it.\n"
        "add_custom_command(TARGET frame POST_BUILD\n"
        "    COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/frame_data.bin\" \"$<TARGET_FILE_DIR:frame>/frame_data.bin\")\n";

    if (!WriteText("frame_objects.h", header, out.error) || !WriteText("frame_objects.cpp", source, out.error) ||
        !WriteText("CMakeLists.txt", cmake, out.error) || !WriteText("README.md", Readme(report, out), out.error))
        return false;
    for (const char* name : {"main.cpp", "dx_support.h", "dx_support.cpp"}) {
        const std::string text = Template(name);
        if (text.empty()) {
            out.error = std::string("the exporter was built without its template ") + name;
            return false;
        }
        if (!WriteText(name, text, out.error)) return false;
    }
    out.files = sources;
    for (const char* name : {"dx_support.h", "frame_objects.h", "CMakeLists.txt", "README.md", "frame_data.bin"}) out.files.push_back(name);
    return true;
}

} // namespace dxreplay
