#include "mtl_exporter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "mtl_replayer.h"

namespace mtlreplay {

// The exported project's hand-written files (src/metal/replay/export_template, embedded by
// tools/embed_files.py).
struct EmbeddedFile { const char* name; const char* const* pieces; size_t count; };
extern const EmbeddedFile kMtlExportTemplates[];
extern const size_t kMtlExportTemplatesCount;

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
    for (size_t i = 0; i < kMtlExportTemplatesCount; ++i) {
        if (std::strcmp(kMtlExportTemplates[i].name, name) != 0) continue;
        std::string out;
        for (size_t p = 0; p < kMtlExportTemplates[i].count; ++p) out += kMtlExportTemplates[i].pieces[p];
        return out;
    }
    return std::string();
}

std::string Hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return buf;
}

std::string Trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \n");
    const size_t b = s.find_last_not_of(" \n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

MtlExporter::MtlExporter(std::string directory, const vkreplay::CaptureFile& capture)
    : _dir(std::move(directory)), _capture(capture) {
    for (Part* p : {&_create, &_contents, &_frame}) Configure(p->writer);
}

MtlExporter::~MtlExporter() {
    if (_data) std::fclose(_data);
}

void MtlExporter::Configure(Source& w) {
    w.objectName = [this](id object) { return NameOf(object); };
    w.data = [this](const void* data, size_t size) { return Data(data, size); };
    w.indent = 1;
}

bool MtlExporter::Open(std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(_dir, ec);
    if (ec) {
        error = "could not create " + _dir + ": " + ec.message();
        return false;
    }
    std::filesystem::create_directories(_dir + "/shaders", ec);
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

std::string MtlExporter::Declare(const std::string& type, const std::string& stem, uint64_t id, ::id object) {
    const std::string name = stem + "_" + std::to_string(id);
    _globals.emplace_back(type, name);
    _names[(__bridge const void*)object] = name;
    return name;
}

void MtlExporter::Global(const std::string& type, const std::string& name) {
    _globals.emplace_back(type, name);
}

void MtlExporter::Alias(::id object, const std::string& name) { _names[(__bridge const void*)object] = name; }

std::string MtlExporter::NameOf(::id object) const {
    const auto it = _names.find((__bridge const void*)object);
    return it == _names.end() ? std::string() : it->second;
}

std::string MtlExporter::Data(const void* data, size_t size) {
    if (!data || !size || !_data) return "nullptr";
    static const char zeros[16] = {};
    const uint64_t padded = (_dataSize + 15) & ~15ull;
    if (padded != _dataSize) std::fwrite(zeros, 1, (size_t)(padded - _dataSize), _data);
    std::fwrite(data, 1, size, _data);
    _dataSize = padded + size;
    return "Data(" + Hex(padded) + ", " + std::to_string(size) + ")";
}

std::string MtlExporter::Shader(const std::string& stem, std::string_view source) {
    const std::string name = "shaders/" + stem + ".metal";
    std::string error;
    // Written as a real .metal file rather than into the data file: the shading language is the
    // first thing anyone reading a bug report wants to see, and a compiler can be pointed at it.
    if (!WriteText(name, std::string(source), error)) {
        Note("could not write " + name);
        return std::string();
    }
    _shaderFiles.push_back(name);
    return name;
}

// ---------------------------------------------------------------------------------------------
// Statements

MtlExporter::Part& MtlExporter::PartOf(Section section) {
    return section == Create ? _create : section == Contents ? _contents : _frame;
}

void MtlExporter::MaybeSplit(Part& p) {
    if (p.writer.Lines() >= kPartLines) SplitNow(p);
}

void MtlExporter::SplitNow(Part& p) {
    if (p.writer.text.empty()) return;
    p.parts.push_back(std::move(p.writer.text));
    p.writer.ResetPart();
}

void MtlExporter::Block(Section section, const std::string& label, const std::function<void(Source&)>& body) {
    Part& part = PartOf(section);
    Source& w = part.writer;
    Source scratch;
    Configure(scratch);
    scratch.indent = w.indent + 1;
    body(scratch);
    for (const std::string& n : scratch.notes) w.Note(n);
    if (scratch.Statements() == 1) {
        w.Line(Trimmed(scratch.text) + (label.empty() ? "" : "   // " + label));
    } else if (scratch.Statements()) {
        // A block, so the locals it declares do not collide with the next one's — except in the
        // frame, where an encoder declared in one statement has to stay in scope for its commands.
        w.Line("{" + (label.empty() ? std::string() : "   // " + label));
        scratch.notes.clear();
        w.Append(scratch);
        w.Line("}");
    }
    MaybeSplit(part);
}

void MtlExporter::Comment(Section section, const std::string& text) { PartOf(section).writer.Comment(text); }
void MtlExporter::Blank(Section section) { PartOf(section).writer.Blank(); }

void MtlExporter::EndSubmission() {
    if (_frame.writer.Lines() >= kPartLines / 2) SplitNow(_frame);
}

void MtlExporter::LeftOut(uint32_t index, const std::string& method, const std::string& why) {
    _frame.writer.Comment("[" + std::to_string(index) + "] " + method + ": left out: " + why);
    ++_leftOut;
}

void MtlExporter::Note(const std::string& note) { _notes.push_back(note); }

void MtlExporter::Device(const std::string& capturedDevice, const std::string& replayDevice) {
    _capturedDevice = capturedDevice;
    _replayDevice = replayDevice;
}

// ---------------------------------------------------------------------------------------------
// Files

bool MtlExporter::WriteText(const std::string& name, const std::string& text, std::string& error) {
    const std::string path = _dir + "/" + name;
    std::ofstream out(path, std::ios::binary);
    if (out) out.write(text.data(), (std::streamsize)text.size());
    if (!out) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

bool MtlExporter::WritePart(Part& p, std::vector<std::string>& files, std::string& error) {
    SplitNow(p);
    for (const std::string& n : p.writer.notes) _notes.push_back(n);
    const std::string header =
        "// Generated by GPU Inspector's Export to C++ (mtlinsp_replay --export). See README.md.\n"
        "#include \"mtl_support.h\"\n#include \"frame_objects.h\"\n\n";
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
        texts.back() += "void " + name + "(void) {\n" + p.parts[i] + "}\n\n";
        lines.back() += count + 3;
        declarations += "void " + name + "(void);\n";
        calls += "    " + name + "();\n";
    }
    texts[0].insert(header.size(), declarations + (declarations.empty() ? "" : "\n") + "void " + p.function +
                                       "(void) {\n" + calls + "}\n\n");
    for (size_t f = 0; f < texts.size(); ++f) {
        const std::string name = p.file + (f ? "_" + std::to_string(f + 1) : "") + ".mm";
        if (!WriteText(name, texts[f], error)) return false;
        files.push_back(name);
    }
    return true;
}

std::string MtlExporter::CMakeLists() const {
    return "cmake_minimum_required(VERSION 3.20)\n"
           "project(frame OBJCXX)\n\n"
           "# A frame exported from a GPU Inspector capture (README.md). Metal: macOS, with Xcode's\n"
           "# command line tools and nothing else.\n"
           "set(CMAKE_OBJCXX_STANDARD 20)\n"
           "set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)\n\n"
           "file(GLOB FRAME_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/*.mm\")\n"
           "add_executable(frame ${FRAME_SOURCES})\n"
           "target_compile_options(frame PRIVATE -fobjc-arc -Wall -Wno-unused-variable)\n"
           "target_link_libraries(frame PRIVATE \"-framework Foundation\" \"-framework Metal\")\n\n"
           "# The data file and the shaders beside the executable, where the program looks for them.\n"
           "add_custom_command(TARGET frame POST_BUILD\n"
           "    COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/frame_data.bin\" \"$<TARGET_FILE_DIR:frame>/frame_data.bin\"\n"
           "    COMMAND ${CMAKE_COMMAND} -E copy_directory \"${CMAKE_CURRENT_SOURCE_DIR}/shaders\" \"$<TARGET_FILE_DIR:frame>/shaders\")\n";
}

std::string MtlExporter::Readme(const MtlReplayReport& report, const MtlExportReport& summary) const {
    auto text = [&](const char* key) {
        const vkreplay::JValue* v = _capture.Manifest().Get(key);
        return v && v->IsString() ? std::string(v->Str()) : std::string();
    };
    std::string s = "# An exported frame\n\n";
    s += "This project re-creates one captured frame of a Metal application and runs it again: the frame's\n"
         "objects, what its textures and buffers held, and every command of its command buffers, as plain\n"
         "Objective-C++ with no dependency but the Metal framework. It was written by GPU Inspector's Export\n"
         "to C++ (`mtlinsp_replay --export`) from a capture, for reproducing a problem outside the application.\n\n";
    s += "| | |\n|---|---|\n";
    // The manifest's `application` is what wrote the file (GPU Inspector); the captured application is its `source`.
    std::string application;
    if (const auto* source = _capture.Manifest().Get("source");
        source && source->Get("name") && source->Get("name")->IsString()) {
        application = std::string(source->Get("name")->Str());
    }
    if (!application.empty()) s += "| Application | `" + application + "` |\n";
    if (!text("savedAt").empty()) s += "| Captured | " + text("savedAt") + " |\n";
    if (!_capturedDevice.empty()) s += "| Captured on | " + _capturedDevice + " |\n";
    s += "| Exported from a replay on | " + _replayDevice + " |\n";
    s += "| Objects | " + std::to_string(summary.objects) + " |\n";
    s += "| Commands | " + std::to_string(summary.commands) + " in " + std::to_string(summary.submissions) +
         " command buffer" + (summary.submissions == 1 ? "" : "s") + " |\n";
    s += "| Render targets compared | " + std::to_string(summary.targets) + " |\n\n";
    s += "## Build and run\n\n```\ncmake -B build\ncmake --build build\nbuild/frame\n```\n\n"
         "It needs CMake and Xcode's command line tools, and nothing else.\n\n"
         "- `--out <directory>` is where the render targets are written (default `out`), `--no-images` writes none.\n"
         "- `--data <file>` names `frame_data.bin` when it is not beside the executable.\n\n"
         "The program prints each render target the capture read back, compared byte for byte with the copy the\n"
         "capture holds, and writes both as PNG where the format allows. It exits with 0 when every target is\n"
         "identical, 1 when some differ or could not be compared, and 2 when a call failed (the call is printed).\n\n";
    s += "## What is in it\n\n"
         "| File | |\n|---|---|\n"
         "| `frame_create*.mm` | `CreateObjects`: every object the frame uses, in the order it was created. |\n"
         "| `frame_contents*.mm` | `UploadContents`: what the frame's textures and buffers held when it started. |\n"
         "| `frame_commands*.mm` | `Frame`: each command buffer's encoders, commands, commit and read-backs. |\n"
         "| `frame_objects.h` | One variable per object, named by kind and capture id: `texture_12` is texture 12 in GPU Inspector. |\n"
         "| `shaders/*.metal` | The Metal Shading Language of every library the capture has source for. |\n"
         "| `frame_data.bin` | Shader bytes, texture and buffer contents, and the captured render targets. |\n"
         "| `mtl_support.*`, `main.mm` | Not specific to the frame: the device, uploads, read-backs, PNG. |\n\n"
         "A number in brackets after a command (`// [17]`) is its index in the capture's command list. A\n"
         "descriptor is allocated fresh and only the properties the application set are assigned — every other\n"
         "one is at Metal's own default, as it was in the application.\n\n";
    s += "## How it differs from the application\n\n"
         "The source is what GPU Inspector's replay did with the capture, which is the application's frame with\n"
         "these differences:\n\n"
         "- There is no window: the drawable's texture is an ordinary render target, and `presentDrawable:` is\n"
         "  left out. That is what lets the program run headless.\n"
         "- A private buffer is made shared, so its contents can be written without a staging blit. Metal does\n"
         "  not let a buffer's storage mode change what a shader sees.\n"
         "- A pass that discarded a target stores it instead, so there is something to read back and compare.\n"
         "- Buffers hold the ranges the frame bound; nothing else of the application's memory is in a capture.\n"
         "- Each command buffer is waited for before the next; events and fences across frames are left out.\n"
         "- A library is compiled from its Metal Shading Language where the capture has it, and loaded from the\n"
         "  metallib bytes otherwise.\n\n";
    if (summary.leftOut || !report.problems.empty() || !summary.notes.empty()) {
        s += "## What the replay left out or reported\n\n";
        if (summary.leftOut) {
            s += "- " + std::to_string(summary.leftOut) +
                 " command(s) are left out, each with a comment where it would be in `frame_commands*.mm`.\n";
        }
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
    if (!report.comparisons.empty()) {
        s += "## The replay's own result\n\nWhat the replay this was exported from got on " + _replayDevice +
             ", for comparison with a run of this program:\n\n";
        for (const MtlTargetComparison& t : report.comparisons) {
            s += "- texture " + std::to_string(t.texture) + " (" + t.format + " " + std::to_string(t.width) + "x" +
                 std::to_string(t.height) + " " + t.aspect + "): ";
            if (!t.compared) s += "not compared: " + t.note + "\n";
            else if (!t.differingTexels) s += "identical to the capture\n";
            else s += std::to_string(t.differingTexels) + " of " + std::to_string(t.texels) + " texels differ\n";
        }
        s += "\n";
    }
    return s;
}

bool MtlExporter::Finish(const MtlReplayReport& report, MtlExportReport& out) {
    out.directory = _dir;
    if (_data) {
        std::fclose(_data);
        _data = nullptr;
    }
    std::vector<std::string> sources = {"main.mm", "mtl_support.mm", "frame_objects.mm"};
    for (Part* p : {&_create, &_contents, &_frame})
        if (!WritePart(*p, sources, out.error)) return false;

    std::string header =
        "// One variable per object of the capture, named by its kind and its id in the capture (the id GPU\n"
        "// Inspector shows), and the functions the frame is made of.\n#pragma once\n\n#include \"mtl_support.h\"\n\n";
    std::string source = "#include \"frame_objects.h\"\n\n";
    for (const auto& [type, name] : _globals) {
        header += "extern " + type + " " + name + ";\n";
        source += type + " " + name + " = nil;\n";
    }
    header += "\nvoid CreateObjects(void);\nvoid UploadContents(void);\nvoid Frame(void);\n";

    out.objects = _objects;
    out.commands = _commands;
    out.submissions = _submissions;
    out.targets = _targets;
    out.leftOut = _leftOut;
    out.dataBytes = _dataSize;
    std::sort(_notes.begin(), _notes.end());
    _notes.erase(std::unique(_notes.begin(), _notes.end()), _notes.end());
    out.notes = _notes;

    if (!WriteText("frame_objects.h", header, out.error) ||
        !WriteText("frame_objects.mm", source, out.error) ||
        !WriteText("CMakeLists.txt", CMakeLists(), out.error) ||
        !WriteText("README.md", Readme(report, out), out.error)) {
        return false;
    }
    for (const char* name : {"main.mm", "mtl_support.h", "mtl_support.mm"}) {
        const std::string text = Template(name);
        if (text.empty()) {
            out.error = std::string("the exporter was built without its template ") + name;
            return false;
        }
        if (!WriteText(name, text, out.error)) return false;
    }
    out.files = sources;
    for (const char* name : {"mtl_support.h", "frame_objects.h", "CMakeLists.txt", "README.md", "frame_data.bin"}) {
        out.files.push_back(name);
    }
    for (const std::string& shader : _shaderFiles) out.files.push_back(shader);
    return true;
}

} // namespace mtlreplay
