// Export to C++ for Metal: a capture's frame written as a standalone, compilable Objective-C++
// project (docs/REPLAY.md, "Export to C++"), the counterpart of the Vulkan exporter
// (src/replay/src/exporter.h) and the Direct3D 12 one (src/d3d12/replay/src/dx_exporter.h), and
// built the same way: the replay (mtl_replayer.h) does its work and says each step as it does it,
// with the descriptions it actually handed Metal, so the exported program does what the replay did.
//
// The exporter owns what is not the frame's: the variable each object is spelled as, the data file
// (shader bytes, buffer and texture contents, the captured targets), the three functions the
// statements go into (CreateObjects, UploadContents, Frame) cut into parts and files a compiler can
// take, and the project's other files. The project it writes:
//
//   CMakeLists.txt, README.md       builds with CMake and clang; links Metal and Foundation
//   main.mm, mtl_support.h/.mm      the device, uploads, read-backs, PNG, the data file
//   frame_objects.h/.mm             one global per captured object, named by kind and capture id (texture_12)
//   frame_create*.mm                CreateObjects
//   frame_contents*.mm              UploadContents: texture and buffer contents the frame starts from
//   frame_commands*.mm              Frame: each command buffer's encoders, commands, commit and read-backs
//   shaders/*.metal                 the Metal Shading Language of every library the capture has source for
//   frame_data.bin
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#import <Foundation/Foundation.h>

#include "gpucap.h"

#include "mtl_source.h"

namespace mtlreplay {

struct MtlReplayReport;
struct MtlExportReport;

class MtlExporter {
public:
    enum Section { Create, Contents, Frame };

    MtlExporter(std::string directory, const vkreplay::CaptureFile& capture);
    ~MtlExporter();
    MtlExporter(const MtlExporter&) = delete;
    MtlExporter& operator=(const MtlExporter&) = delete;

    bool Open(std::string& error);

    // ---- names
    /** Declares a global for a captured object and returns its name (texture_12); `type` is its
     *  Objective-C type, `stem` the kind the name is built from. */
    std::string Declare(const std::string& type, const std::string& stem, uint64_t captureId, id object);
    /** A name for an object that is not the capture's (the device, the replay's own queue). */
    void Alias(id object, const std::string& name);
    std::string NameOf(id object) const;

    // ---- statements
    /** One statement, or a block of them when `body` declares locals, with a trailing label. */
    void Block(Section section, const std::string& label, const std::function<void(Source&)>& body);
    void Comment(Section section, const std::string& text);
    void Blank(Section section);
    /** The end of a command buffer: a good place for a part to end. */
    void EndSubmission();
    /** Writes bytes into the data file and returns the expression that reads them back. */
    std::string Data(const void* data, size_t size);
    /** A library's Metal Shading Language as a file of its own; returns its project-relative path. */
    std::string Shader(const std::string& stem, std::string_view source);

    void Device(const std::string& capturedDevice, const std::string& replayDevice);
    void LeftOut(uint32_t index, const std::string& method, const std::string& why);
    void Note(const std::string& note);
    void CountObject() { ++_objects; }
    void CountCommand() { ++_commands; }
    void CountSubmission() { ++_submissions; }
    void CountTarget() { ++_targets; }

    bool Finish(const MtlReplayReport& report, MtlExportReport& out);

private:
    struct Part {
        std::string file;
        std::string function;
        Source writer;
        std::vector<std::string> parts;
    };

    void Configure(Source& w);
    Part& PartOf(Section section);
    void MaybeSplit(Part& p);
    void SplitNow(Part& p);
    bool WriteText(const std::string& name, const std::string& text, std::string& error);
    bool WritePart(Part& p, std::vector<std::string>& files, std::string& error);
    std::string Readme(const MtlReplayReport& report, const MtlExportReport& summary) const;
    std::string CMakeLists() const;

    std::string _dir;
    const vkreplay::CaptureFile& _capture;
    FILE* _data = nullptr;
    uint64_t _dataSize = 0;
    /** Objective-C objects are pointers, so the name map is keyed by the pointer, as the D3D12
     *  exporter's is by IUnknown*. The replay holds every object alive for its whole run. */
    std::unordered_map<const void*, std::string> _names;
    std::vector<std::pair<std::string, std::string>> _globals;   // (type, name), in creation order
    std::vector<std::string> _shaderFiles;
    std::vector<std::string> _notes;
    std::string _capturedDevice;
    std::string _replayDevice;
    size_t _objects = 0, _commands = 0, _submissions = 0, _targets = 0, _leftOut = 0;
    Part _create{"frame_create", "CreateObjects", {}, {}};
    Part _contents{"frame_contents", "UploadContents", {}, {}};
    Part _frame{"frame_commands", "Frame", {}, {}};
};

} // namespace mtlreplay
