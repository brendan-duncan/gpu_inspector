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

#include <array>
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

namespace dxreplay
{

/** What the replay was doing last ("command 412 DrawIndexedInstanced"), for a crash handler to name: empty before it starts. */
const char* CurrentStep();

class DxExporter;
class Source;
using vkreplay::CaptureFile;

/** What the hardware counters run asks for (dx_counters.h, --counters). */
struct DxCounterOptions
{
    bool enabled = false;
    /** Only list what this GPU offers, which needs no profiling session. */
    bool list = false;
    /** The metrics to collect; empty takes the default set (the limiters docs/PROFILING.md names). */
    std::vector<std::string> names;
};

/** Draws timed again with variants of one of their shader stages (--ablate, dx_measure.cpp). */
struct DxAblationOptions
{
    struct Variant
    {
        std::string name;
        /** The variant's DXIL container. */
        std::vector<uint8_t> code;
    };
    struct Target
    {
        uint32_t command = 0;
        /** "fragment", "compute", "vertex": the names the capture keeps a pipeline's code under. */
        std::string stage;
        /** Draws issued between one pair of timestamps. */
        uint32_t repeat = 1;
        std::vector<Variant> variants;
    };
    bool enabled = false;
    /** Timed rounds; one more runs first, untimed. */
    uint32_t rounds = 5;
    std::vector<Target> targets;
};

/**
 * A pipeline's stage given other code for the whole replay (--replace): a shader edited in GPU
 * Inspector and run in the captured frame instead of in the application. What the frame's targets
 * hold with it is compared with what the capture read back, which is the edit's effect.
 */
struct DxShaderReplacement
{
    uint64_t pipeline = 0;
    /** "vertex", "fragment", "compute", ...: the names the capture keeps a pipeline's code under. */
    std::string stage;
    /** The DXBC or DXIL container. */
    std::vector<uint8_t> code;
};

struct DxReplayOptions
{
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
    /** Every draw and dispatch timed and counted as the frame replays (--draws, dx_measure.cpp). */
    bool drawStats = false;
    DxAblationOptions ablation;
    std::vector<DxShaderReplacement> replacements;
};

/** One draw or dispatch as the replay measured it (the Vulkan replay's DrawResult). */
struct DxDrawResult
{
    uint32_t command = 0;
    uint32_t frame = 0;
    uint64_t commandList = 0;
    /** The pass it is in, counted per command list as the counters count them; UINT32_MAX outside one. */
    uint32_t passIndex = UINT32_MAX;
    bool timed = false;
    double durationMs = 0;
    bool counted = false;
    uint64_t vertexInvocations = 0;
    uint64_t primitives = 0;
    uint64_t fragmentInvocations = 0;
    uint64_t computeInvocations = 0;
    bool sampled = false;
    uint64_t samplesPassed = 0;
};

struct DxAblationTiming
{
    std::string name;
    bool measured = false;
    /** Per draw: the median of the rounds. */
    double ms = 0;
    std::vector<double> samples;
    std::string note;
};

struct DxAblationResult
{
    uint32_t command = 0;
    std::string stage;
    uint64_t pipeline = 0;
    uint32_t frame = 0;
    uint64_t commandList = 0;
    uint32_t passIndex = UINT32_MAX;
    uint32_t rounds = 0;
    uint32_t repeat = 1;
    DxAblationTiming baseline;
    std::vector<DxAblationTiming> variants;
    std::string note;
};

/** One measured range's counter values, in the order of DxCounterReport::counters. */
struct DxCounterRange
{
    uint32_t command = 0;
    uint32_t frame = 0;
    uint64_t commandBuffer = 0;
    uint32_t passIndex = 0;
    std::vector<double> values;
};

struct DxCounterReport
{
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

struct DxTargetComparison
{
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

struct DxExportReport
{
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

struct DxReplayReport
{
    std::string device;
    size_t objectsCreated = 0;
    size_t objectsSkipped = 0;
    size_t commandsRecorded = 0;
    size_t submissions = 0;
    size_t texturesUploaded = 0;
    size_t bufferUploads = 0;
    size_t descriptorsWritten = 0;
    /** Acceleration structures built before the capture began, built again before the frame from what was read back. */
    size_t earlierStructuresBuilt = 0;
    std::vector<std::string> problems;
    /** The debug layer's errors and warnings. */
    std::vector<std::string> messages;
    std::vector<DxTargetComparison> targets;
    DxExportReport exported;
    DxCounterReport counters;
    std::vector<DxDrawResult> draws;
    /** Why some of the draws' measurements are missing. */
    std::string drawStatsNote;
    std::vector<DxAblationResult> ablations;
};

/** A command that draws, dispatches or runs a bundle: what --draws puts queries around. */
bool IsActionMethod(const std::string& method);

/**
 * What an exported program needs to make a top level's instances again: the captured bytes, and the
 * address here of the bottom level each one names (0 where none), which the program spells from its
 * own buffers (UploadInstances in dx_support).
 */
struct InstanceSource
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> bottoms;
};

class DxReplayer
{
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

    // --- Ray tracing (dx_raytracing.cpp) -------------------------------------------------------
    /** Where a captured acceleration structure lives: the buffer a build wrote it into, and where in it. */
    struct StructurePlace
    {
        uint64_t buffer = 0;
        uint64_t offset = 0;
    };
    using Identifier = std::array<uint8_t, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES>;

    /** Reads the capture's acceleration structures, so an instance's address can be remapped. */
    void PrepareRaytracing();
    /** This machine's address for a structure the captured process had at `captured`; 0 for one it has none of. */
    D3D12_GPU_VIRTUAL_ADDRESS RemapStructureAddress(uint64_t captured) const;
    ID3D12StateObject* CreateStateObject(uint64_t id, const vkreplay::JValue& object, const vkreplay::JValue& args);
    /** This runtime's identifier per captured one, which is what makes a captured binding table replayable. */
    void NoteStateObjectIdentifiers(uint64_t id, const vkreplay::JValue& object, ID3D12StateObject* stateObject);
    /** A top level build's instances with their bottom level addresses remapped, in a buffer of the replay's own. */
    D3D12_GPU_VIRTUAL_ADDRESS RemapInstances(const vkreplay::JValue& command, UINT count, InstanceSource* source = nullptr);
    /** The same, from a list of read-backs ({field, capture}): a command's buildData or a structure's captureInputs. */
    D3D12_GPU_VIRTUAL_ADDRESS RemapInstancesFrom(const vkreplay::JValue* list, UINT count, InstanceSource* source = nullptr);
    /**
     * A local root argument's value in this process (ResolveLocalRootArguments in the capture
     * library resolved it): a descriptor table's handle into the replay's heap, or a root view's
     * address in the replay's buffer. 0 when the capture could not resolve it or the replay lacks it.
     */
    uint64_t LocalRootValue(const vkreplay::JValue& argument);
    /** A trace's local descriptor tables' descriptors, written into the replay's heaps before it runs. */
    void WriteLocalRootDescriptors(const vkreplay::JValue& command);
    /** Export to C++: a state object's description as the replay made it, into CreateObjects. */
    void ExportStateObject(uint64_t id, const std::string& name, const D3D12_STATE_OBJECT_DESC& desc);
    /** Export to C++: DispatchRays with its binding table rebuilt by the program (BindingTable in dx_support). */
    void ExportDispatchRays(uint32_t index, const vkreplay::JValue& command, const D3D12_DISPATCH_RAYS_DESC& issued, uint64_t stateObjectId,
        const std::string& listName);
    /** Builds, before the frame, the structures built before the capture began, from what was read back of them. */
    void BuildEarlierStructures();
    /** One binding table region rebuilt with this runtime's identifiers, in a buffer of the replay's own. */
    D3D12_GPU_VIRTUAL_ADDRESS RemapBindingTable(const vkreplay::JValue& command, const char* region, UINT64 stride,
        UINT64 size, uint64_t stateObjectId);
    /** An upload buffer holding these bytes, released when the submission that read it has finished. */
    D3D12_GPU_VIRTUAL_ADDRESS UploadTransient(const void* data, size_t size, const char* what);
    /** Whether this GPU does ray tracing at all, fetching ID3D12Device5 the first time it is asked. */
    bool RaytracingDevice();
    ID3D12GraphicsCommandList4* RaytracingList(ID3D12GraphicsCommandList* list);
    /** One of the five ray tracing commands; false with a reason when it was left out. */
    bool IssueRaytracingCommand(const std::string& method, const vkreplay::JValue& command, const vkreplay::JValue* args,
        ID3D12GraphicsCommandList* list, std::string& leftOut, uint32_t index = UINT32_MAX);
    ID3D12Device5* _device5 = nullptr;
    bool _noRaytracing = false;
    std::unordered_map<uint64_t, StructurePlace> _structureAddresses;   // captured address -> where it lives
    /**
     * The buffers a captured acceleration structure lives in. A resource cannot be transitioned
     * into D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE — it is created in that state
     * and stays there — so these are made in it and left out of the state inference entirely.
     */
    std::unordered_set<uint64_t> _structureBuffers;
    /** The structures the captured frame builds; one it does not is a structure the replay cannot fill. */
    std::unordered_set<uint64_t> _structuresBuiltInFrame;
    /** Structures already reported as unbuilt, so a frame of many instances says it once. */
    std::unordered_set<uint64_t> _reportedUnbuilt;
    /** Per state object id: this runtime's identifier for each identifier the capture recorded. */
    std::unordered_map<uint64_t, std::unordered_map<std::string, Identifier>> _shaderIdentifiers;
    /** The last SetPipelineState1, for a capture taken before a trace named its own state object. */
    uint64_t _boundStateObject = 0;

    // --- Per-draw measurements and ablation (dx_measure.cpp) -----------------------------------
    struct MeasureState;
    bool PrepareMeasurements();
    void DestroyMeasurements();
    /** A list is about to be recorded: the query slots it takes start here. */
    void BeginListMeasurements();
    /** Queries around one action; the index to end them with, or -1 when it is not measured. */
    int BeginDrawQuery(ID3D12GraphicsCommandList* list, uint32_t command, uint32_t frame, uint64_t listId, uint32_t passIndex);
    void EndDrawQuery(ID3D12GraphicsCommandList* list, int pending);
    /** Resolves what the list's queries wrote, before it closes. */
    void ResolveListMeasurements(ID3D12GraphicsCommandList* list);
    /** Reads the submission's results once it has been waited for; `ran` false when it never ran. */
    void CompleteMeasurements(ID3D12CommandQueue* queue, bool ran);
    /** A copy of a pipeline with a variant's code for one stage (-1: the capture's own), writing no depth. */
    ID3D12PipelineState* AblationPipeline(uint64_t pipelineId, size_t target, int variant);
    /** Times the command with its ablation target's variants, right before it is issued as captured. */
    void IssueAblation(uint32_t index, const std::string& method, const vkreplay::JValue& command, const vkreplay::JValue* args,
        ID3D12GraphicsCommandList* list, uint64_t listId, uint32_t frame, uint32_t passIndex);
    MeasureState* _measure = nullptr;
    /** Per list being recorded: the pipeline set, and how many of the capture's own queries are open. */
    uint64_t _boundPipeline = 0;
    uint32_t _appQueryDepth = 0;
    /** While a pipeline is decoded for an ablation: the stage whose code is the variant's. */
    std::string _overrideStage;
    const std::vector<uint8_t>* _overrideCode = nullptr;

private:
    struct Resource
    {
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
    struct Heap
    {
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
    struct InitialState
    {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool fixed = false;   // a barrier or a writing use decided it
        bool any = false;
    };
    struct Group
    {
        uint64_t list = 0;
        uint32_t first = 0;   // its Reset
        uint32_t last = 0;    // its Close
        bool used = false;
    };
    struct PassTarget
    {
        uint64_t resource = 0;
        uint32_t mip = 0;
        uint32_t firstSlice = 0;
        bool depth = false;
        // The render pass ends by discarding it (the stencil separately): what it holds afterwards is undefined.
        bool discarded = false;
        bool stencilDiscarded = false;
    };
    struct Pass
    {
        bool active = false;
        bool realPass = false;   // BeginRenderPass: nothing may be copied until EndRenderPass
        uint32_t index = 0;
        uint32_t frame = 0;
        uint64_t list = 0;
        std::vector<PassTarget> targets;   // by attachment index
    };
    struct Readback
    {
        ID3D12Resource* buffer = nullptr;
        size_t target = 0;
        const vkreplay::JValue* texture = nullptr;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        uint64_t rowBytes = 0;
        uint32_t rows = 0;
    };
    enum class DescriptorKind
    {
        RenderTarget,
        DepthStencil,
        Table
    };

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
    // What the exported program shows in its window: the swap chain buffer the frame wrote last, else its last color target.
    std::unordered_set<uint64_t> _swapBuffers;
    uint64_t _lastSwapWrite = 0;
    uint64_t _lastColorTarget = 0;
    std::vector<ID3D12CommandAllocator*> _allocators;
    /** Where Transition's source goes: the contents before the frame (false), the restore after it. */
    bool _restoring = false;
    void NoteWrite(uint64_t resource, bool colorTarget);
    void EmitFrameEnd();
    struct BundleInfo
    {
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
