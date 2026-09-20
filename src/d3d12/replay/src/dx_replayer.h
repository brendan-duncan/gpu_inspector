// The Direct3D 12 replay engine: re-creates a D3D12 capture's objects on this machine's GPU and
// re-executes its command lists in submission order, the counterpart of the Vulkan replay
// (src/replay/src/replayer.h). docs/REPLAY.md, "Direct3D 12".
//
// What a capture gives it:
//   * every object with the arguments it was created from, re-created in id order. Swap chain
//     buffers become ordinary render target textures; heaps are left out and a placed resource is
//     committed, so a capture replays on a GPU with other heap tiers; pipelines take their
//     bytecode from the blobs the capture keeps with them; root signatures are serialized again
//     from their descriptions.
//   * the frame's commands, each list's recording after the ExecuteCommandLists that submitted it.
//   * no resource states and no descriptor writes, which happen outside the frame. The state each
//     subresource starts in is inferred (the first barrier's StateBefore, else what the frame's use
//     of it needs), and each descriptor is written from what the capture says it held: a render
//     target's view when it is bound, a table's descriptors from the snapshot taken when it was set.
//   * the contents it read back: sampled textures are uploaded before the frame, and each buffer
//     range a list binds before that list is executed.
//
// Every render target the capture read back at the end of a pass is read back here at the same
// point and compared byte for byte.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "dx_counters.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "arena.h"
#include "gpucap.h"
#include "json.h"

#include "dx_decode.h"

namespace dxreplay {

/** What the replay was doing last ("command 412 DrawIndexedInstanced"), for a crash handler to name: empty before it starts. */
const char* CurrentStep();

class DxExporter;
class Source;
using vkreplay::CaptureFile;

/** What the hardware counters run asks for (dx_counters.h, --counters). */
struct DxCounterOptions {
    bool enabled = false;
    /** Only list what this GPU offers, which needs no profiling session. */
    bool list = false;
    /** The metrics to collect; empty takes the default set (the limiters docs/PROFILING.md names). */
    std::vector<std::string> names;
};

struct DxReplayOptions {
    /** Enable the D3D12 debug layer and report its messages. */
    bool debugLayer = false;
    bool compareTargets = true;
    /** Keep both copies of every compared target in the report (--dump). */
    bool keepPixels = false;
    /** Print every object and command to stderr before it is replayed. */
    bool trace = false;
    /** Export to C++: write the frame as a standalone project into this directory while it replays. */
    std::string exportDir;
    /** Hardware counters: the frame is replayed once per collection pass and nothing else runs. */
    DxCounterOptions counters;
};

/** One measured range's counter values, in the order of DxCounterReport::counters. */
struct DxCounterRange {
    uint32_t command = 0;
    uint32_t frame = 0;
    uint64_t commandBuffer = 0;
    uint32_t passIndex = 0;
    std::vector<double> values;
};

struct DxCounterReport {
    bool requested = false;
    /** "nvperf"; D3D12 has no portable counter API, so there is no second backend. */
    std::string backend;
    std::string chip;
    /** Collection passes the counters needed, which is how often the frame was replayed. */
    uint32_t rounds = 0;
    std::vector<DxCounterInfo> counters;
    std::vector<DxCounterRange> passes;
    /** Every counter this GPU offers (--list-counters). */
    std::vector<DxCounterInfo> available;
    /** What could not be collected, and why. */
    std::vector<std::string> notes;
};

struct DxTargetComparison {
    uint64_t resource = 0;
    uint64_t commandList = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    uint32_t attachment = 0;
    std::string format;
    std::string aspect;
    uint32_t width = 0;
    uint32_t height = 0;
    bool compared = false;
    /** Not compared because there is nothing defined to compare: the capture's own read-back failed, or the pass discards the target. Not a failure of the replay. */
    bool undefined = false;
    std::string note;
    uint64_t differingTexels = 0;
    uint64_t texels = 0;
    uint32_t maxByteDelta = 0;
    std::vector<uint8_t> captured;
    std::vector<uint8_t> replayed;
};

struct DxExportReport {
    bool requested = false;
    std::string directory;
    std::string error;
    std::vector<std::string> files;
    size_t objects = 0;
    size_t commands = 0;
    size_t submissions = 0;
    size_t targets = 0;
    size_t leftOut = 0;
    uint64_t dataBytes = 0;
    std::vector<std::string> notes;
};

struct DxReplayReport {
    std::string device;
    size_t objectsCreated = 0;
    size_t objectsSkipped = 0;
    size_t commandsRecorded = 0;
    size_t submissions = 0;
    size_t texturesUploaded = 0;
    size_t bufferUploads = 0;
    size_t descriptorsWritten = 0;
    std::vector<std::string> problems;
    /** The debug layer's errors and warnings. */
    std::vector<std::string> messages;
    std::vector<DxTargetComparison> targets;
    DxExportReport exported;
    DxCounterReport counters;
};

class DxReplayer {
public:
    DxReplayer();
    ~DxReplayer();
    DxReplayer(const DxReplayer&) = delete;
    DxReplayer& operator=(const DxReplayer&) = delete;

    /** Replays the capture; false when it could not start. The report says what happened either way. */
    bool Run(const CaptureFile& capture, const DxReplayOptions& options, DxReplayReport& report);

private:
    // --- Hardware counters (dx_counters.cpp) ---------------------------------------------------
    struct CounterState;
    /** Starts the profiling session and configures the metrics; false with a note when it cannot. */
    bool PrepareCounters();
    /** Every counter the GPU offers, for --list-counters. */
    void ListCounters();
    /** How many replays of the frame the counters may need. */
    uint32_t CounterRounds() const;
    bool BeginCounterRound();
    /** The round's submissions, waited for with a limit (a driver that will not profile never finishes them). */
    bool WaitForCounterWork(CounterState& hw);
    bool EndCounterRound();
    /** The ranges around one render pass, recorded into the list the pass is in. */
    void PushCounterRange(ID3D12GraphicsCommandList* list, uint32_t passIndex, uint32_t command, uint32_t frame, uint64_t listId);
    void PopCounterRange(ID3D12GraphicsCommandList* list);
    /** After the last round: the values, matched back to the passes they were measured around. */
    void CompleteCounters();
    void DestroyCounters();
    CounterState* _counters = nullptr;
    /**
     * Set while a collection pass is open (BeginCounterRound..EndCounterRound). The profiler holds
     * the queue's submissions until the pass ends, so a wait between them never returns: the
     * frame's submissions are made back to back and waited for once, after the pass.
     */
    bool _inCounterRound = false;

public:

private:
    struct Resource {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_DESC desc{};
        D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
        uint32_t mips = 1;
        uint32_t slices = 1;
        uint32_t planes = 1;
        /** Per subresource: the state the replay has recorded it into so far. */
        std::vector<D3D12_RESOURCE_STATES> states;
        bool IsBuffer() const { return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER; }
    };
    struct Heap {
        ID3D12DescriptorHeap* heap = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        uint32_t increment = 0;
        /** What each slot was last written with, so a slot is not written again with what it holds. */
        std::unordered_map<uint32_t, std::string> written;
        // The slots the lists recorded for the submission at hand have bound. Descriptors are written as the
        // lists are recorded, which is before any of them runs: a slot given something else after a
        // draw bound it would show that draw the later contents.
        std::unordered_set<uint32_t> bound;
    };
    /** The state a subresource has to be in when the frame starts, as far as the frame's commands say. */
    struct InitialState {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool fixed = false;   // a barrier or a writing use decided it
        bool any = false;
    };
    struct Group {
        uint64_t list = 0;
        uint32_t first = 0;   // its Reset
        uint32_t last = 0;    // its Close
        bool used = false;
    };
    struct PassTarget {
        uint64_t resource = 0;
        uint32_t mip = 0;
        uint32_t firstSlice = 0;
        bool depth = false;
        // The render pass ends by discarding it (the stencil separately): what it holds afterwards is undefined.
        bool discarded = false;
        bool stencilDiscarded = false;
    };
    struct Pass {
        bool active = false;
        bool realPass = false;   // BeginRenderPass: nothing may be copied until EndRenderPass
        uint32_t index = 0;
        uint32_t frame = 0;
        uint64_t list = 0;
        std::vector<PassTarget> targets;   // by attachment index
    };
    struct Readback {
        ID3D12Resource* buffer = nullptr;
        size_t target = 0;
        const vkreplay::JValue* texture = nullptr;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        uint64_t rowBytes = 0;
        uint32_t rows = 0;
    };
    enum class DescriptorKind { RenderTarget, DepthStencil, Table };

    bool CreateDevice();
    void CreateObjects();
    void CreateObject(const vkreplay::JValue& object);
    ID3D12Resource* CreateResource(uint64_t id, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC& desc,
                                   const D3D12_CLEAR_VALUE* clear, const std::string& comment);
    ID3D12PipelineState* CreatePipeline(const vkreplay::JValue& object, const std::string& cmd, const vkreplay::JValue& args);
    ID3D12RootSignature* CreateRootSignature(uint64_t id, const vkreplay::JValue& args);

    void ComputeInitialStates();
    void UploadTextures();
    void MoveToInitialStates();
    void BuildGroups();
    void ReplayCommands();
    void ApplyBufferData(const Group& group);
    void RecordGroup(Group& group, ID3D12GraphicsCommandList* list, std::vector<Readback>& readbacks);
    /**
     * A command list or an allocator the frame uses and the capture has no object for: an engine
     * that makes and releases them as it goes (Unity does) may have released one before the
     * capture was saved. Either is made from its type alone, so the replay makes its own.
     */
    ID3D12GraphicsCommandList* MissingList(uint64_t id, D3D12_COMMAND_LIST_TYPE type);
    ID3D12CommandAllocator* MissingAllocator(uint64_t id, D3D12_COMMAND_LIST_TYPE type);
    /** Records a bundle from the commands the capture inlines after its ExecuteBundle (their "secondary" is the bundle). */
    bool RecordBundle(uint32_t executeIndex, uint64_t bundleId, ID3D12GraphicsCommandList* bundle);
    /** One captured command issued on `list`; false when it was left out. */
    bool IssueCommand(uint32_t index, const std::string& method, const vkreplay::JValue& command, const vkreplay::JValue* args,
                      ID3D12GraphicsCommandList* list, uint64_t listId);
    void InjectReadbacks(ID3D12GraphicsCommandList* list, const Pass& pass, std::vector<Readback>& readbacks);
    void CompareReadbacks(std::vector<Readback>& readbacks);

    // Descriptors, written from what the capture says each held.
    bool WriteTargetDescriptor(const vkreplay::JValue* entry, bool depth, PassTarget* out);
    void WriteTableDescriptors(const vkreplay::JValue& command, const vkreplay::JValue& args);
    bool WriteDescriptor(uint64_t heapId, uint32_t index, D3D12_DESCRIPTOR_RANGE_TYPE type, const vkreplay::JValue& record);

    // States
    void Transition(ID3D12GraphicsCommandList* list, Resource& r, uint32_t subresource, D3D12_RESOURCE_STATES to, const char* listName);
    void NoteBarrier(const D3D12_RESOURCE_BARRIER& barrier);
    D3D12_RESOURCE_STATES StateOf(const Resource& r, uint32_t subresource) const;
    void NoteUse(uint64_t resourceId, uint32_t subresource, D3D12_RESOURCE_STATES state, bool writes);

    bool RunOneTime(const std::function<void(ID3D12GraphicsCommandList*)>& record);
    void WaitForQueue(ID3D12CommandQueue* queue);
    void Problem(const std::string& message);
    IUnknown* Object(uint64_t id) const;
    Resource* ResourceOf(uint64_t id);
    uint64_t IdOfResource(ID3D12Resource* resource) const;
    void CollectMessages();
    std::string ListName(ID3D12GraphicsCommandList* list) const;

    const CaptureFile* _capture = nullptr;
    DxReplayOptions _options;
    DxReplayReport* _report = nullptr;
    std::unique_ptr<DxExporter> _x;

    IDXGIFactory4* _factory = nullptr;
    IDXGIAdapter1* _adapter = nullptr;
    ID3D12Device* _device = nullptr;
    ID3D12InfoQueue* _infoQueue = nullptr;
    ID3D12CommandQueue* _queue = nullptr;          // the replay's own direct queue, for uploads
    ID3D12CommandAllocator* _allocator = nullptr;
    ID3D12GraphicsCommandList* _utility = nullptr;
    ID3D12Fence* _fence = nullptr;
    uint64_t _fenceValue = 0;
    HANDLE _fenceEvent = nullptr;

    vkreplay::Arena _arena;
    DecodeEnv _env{_arena};
    const vkreplay::JValue* _currentObject = nullptr;   // whose blobs a decoded pipeline takes its bytecode from
    std::unordered_map<uint64_t, IUnknown*> _objects;
    std::unordered_set<uint64_t> _skipped;
    std::vector<IUnknown*> _created;
    std::unordered_map<uint64_t, Resource> _resources;
    std::unordered_map<ID3D12Resource*, uint64_t> _resourceIds;
    std::unordered_map<uint64_t, Heap> _heaps;
    std::unordered_map<uint64_t, const vkreplay::JValue*> _rootSignatures;   // id -> its pDesc JSON
    std::unordered_map<uint64_t, std::vector<InitialState>> _initial;
    std::unordered_map<uint64_t, const vkreplay::JValue*> _bufferData;
    bool _deviceLost = false;
    // What the exported program shows in its window: the swap chain buffer the frame wrote last, else its last colour target.
    std::unordered_set<uint64_t> _swapBuffers;
    uint64_t _lastSwapWrite = 0;
    uint64_t _lastColorTarget = 0;
    std::vector<ID3D12CommandAllocator*> _allocators;
    /** Where Transition's source goes: the contents before the frame (false), the restore after it. */
    bool _restoring = false;
    void NoteWrite(uint64_t resource, bool colorTarget);
    void EmitFrameEnd();
    struct BundleInfo {
        uint64_t allocator = 0;
        uint64_t initialState = 0;
        bool recorded = false;
    };
    std::unordered_map<uint64_t, BundleInfo> _bundles;
    std::vector<Group> _groups;
    std::vector<ID3D12Resource*> _transients;
    /** Per list being recorded: the root signatures bound, which say how a table's ranges lie in its heap. */
    uint64_t _graphicsRoot = 0;
    uint64_t _computeRoot = 0;
};

} // namespace dxreplay
