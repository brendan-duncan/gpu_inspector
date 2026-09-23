// Export to C++ for Direct3D 12: a capture's frame written as a standalone, compilable C++ project
// (docs/REPLAY.md, "Export to C++"), the counterpart of the Vulkan replay's exporter
// (src/replay/src/exporter.h) and built the same way: the replay (dx_replayer.h) does its work and
// says each step as it does it, with the descriptions it actually handed the runtime, so the
// exported program does what the replay did.
//
// The exporter owns what is not the frame's: the variable each object is spelled as, the data file
// (bytecode, buffer and texture contents, the captured targets), the three functions the statements
// go into (CreateObjects, UploadContents, Frame) cut into parts and files a compiler can take, and
// the project's other files. The project it writes:
//
//   CMakeLists.txt, README.md      builds with CMake and MSVC or clang-cl; links d3d12, dxgi, dxguid
//   main.cpp, dx_support.h/.cpp    the device, uploads, descriptor handles, read-backs, PNG, the data file
//   frame_objects.h/.cpp           one global per captured object, named by kind and capture id (texture_36)
//   frame_create*.cpp              CreateObjects
//   frame_contents*.cpp            UploadContents: texture contents and the states the frame expects
//   frame_commands*.cpp            Frame: each submission's buffer contents, descriptors, lists, execute, read-backs
//   frame_data.bin
#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpucap.h"

#include "dx_source.h"

namespace dxreplay
{

struct DxReplayReport;
struct DxExportReport;

class DxExporter
{
public:
    /** Restore: what puts the frame's resources back so that it can run again (the exported program shows it in a loop). */
    enum Section
    {
        Create,
        Contents,
        Frame,
        Restore
    };

    DxExporter(std::string directory, const vkreplay::CaptureFile& capture);
    ~DxExporter();
    DxExporter(const DxExporter&) = delete;
    DxExporter& operator=(const DxExporter&) = delete;

    bool Open(std::string& error);
    /** What the frame leaves on screen, for the window: a texture's name and the state it ends in, or none (`name` empty). */
    void FrameOutput(const std::string& name, const std::string& state, const std::string& comment);

    // ---- names
    /** Declares a global for a captured object and returns its name (texture_36); `type` is its interface. */
    std::string Declare(const std::string& type, const std::string& stem, uint64_t id, IUnknown* object);
    /** A name for an object that is not the capture's (the device, the replay's own queue). */
    void Alias(IUnknown* object, const std::string& name);
    std::string NameOf(IUnknown* object) const;
    /** A descriptor heap's range, so a handle into it is spelled CpuHandle(heap, index). */
    void Heap(ID3D12DescriptorHeap* heap, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu, uint32_t increment, uint32_t count);
    /** A buffer's address range, so an address in it is spelled buffer->GetGPUVirtualAddress() + offset. */
    void Buffer(ID3D12Resource* buffer, D3D12_GPU_VIRTUAL_ADDRESS address, uint64_t size);
    std::string GpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) const;

    // ---- statements
    /** One statement, or a block of them when `body` declares locals, with a trailing label. */
    void Block(Section section, const std::string& label, const std::function<void(Source&)>& body);
    void Comment(Section section, const std::string& text);
    void Blank(Section section);
    /** The end of a submission: a good place for a part to end. */
    void EndSubmission();
    std::string Data(const void* data, size_t size);

    void Device(const std::string& capturedAdapter, const std::string& replayAdapter, const std::string& featureLevel);
    void LeftOut(uint32_t index, const std::string& method, const std::string& why);
    void Note(const std::string& note);
    void CountObject() { ++_objects; }
    void CountCommand() { ++_commands; }
    void CountSubmission() { ++_submissions; }
    void CountTarget() { ++_targets; }

    bool Finish(const DxReplayReport& report, DxExportReport& out);

private:
    struct Part
    {
        std::string file;
        std::string function;
        Source writer;
        std::vector<std::string> parts;
    };
    struct Blob
    {
        uint64_t offset;
        uint64_t size;
    };
    struct HeapRange
    {
        std::string name;
        SIZE_T cpu;
        UINT64 gpu;
        uint32_t increment;
        uint32_t count;
    };
    struct BufferRange
    {
        std::string name;
        D3D12_GPU_VIRTUAL_ADDRESS address;
        uint64_t size;
    };

    void Configure(Source& w);
    Part& PartOf(Section section);
    void MaybeSplit(Part& p);
    void SplitNow(Part& p);
    bool WriteText(const std::string& name, const std::string& text, std::string& error);
    bool WritePart(Part& p, std::vector<std::string>& files, std::string& error);
    std::string Readme(const DxReplayReport& report, const DxExportReport& summary) const;

    std::string _dir;
    const vkreplay::CaptureFile& _capture;
    FILE* _data = nullptr;
    uint64_t _dataSize = 0;
    std::unordered_map<uint64_t, std::vector<Blob>> _blobs;
    std::unordered_map<IUnknown*, std::string> _names;
    std::vector<std::pair<std::string, std::string>> _globals;   // (type, name), in creation order
    std::vector<HeapRange> _heaps;
    std::vector<BufferRange> _buffers;
    std::vector<std::string> _notes;
    std::string _deviceSource;
    std::string _capturedAdapter;
    std::string _replayAdapter;
    size_t _objects = 0, _commands = 0, _submissions = 0, _targets = 0, _leftOut = 0;
    Part _create{"frame_create", "CreateObjects", {}, {}};
    Part _contents{"frame_contents", "UploadContents", {}, {}};
    Part _frame{"frame_commands", "Frame", {}, {}};
    Part _restore{"frame_restore", "RestoreFrame", {}, {}};
    std::string _outputSource;
};

} // namespace dxreplay
