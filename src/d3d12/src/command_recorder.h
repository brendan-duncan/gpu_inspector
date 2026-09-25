// Records the commands of one command list during a frame capture, and the state the list has
// bound that the capture needs at the next draw: the root signatures, the descriptor heaps, the
// pipeline, the render targets of the open pass.
//
// The command list hooks ask CaptureManager::RecorderFor(list); when it returns a recorder, they
// serialize the call's arguments and Record() it after forwarding. A command list is externally
// synchronized by the application, so a recorder needs no lock.
#pragma once

#include "common.h"
#include "descriptors.h"
#include "stacktrace.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dxinsp
{

class CommandRecorder;

/** Takes a command's snapshot (its descriptors, the contents of the buffers it binds) again, queued on the list given. */
using ExtraRefresh = std::function<std::string(CommandRecorder*)>;

struct RecordedCommand
{
    std::string method;
    std::string args;      // JSON object with the command's arguments, or empty
    std::string extra;     // pre-separated member list merged into the entry: ,"descriptors":{...},"stack":[...]
    // A bundle's command only. A bundle is recorded once and executed for many frames, so what it
    // snapshot when it was recorded is of another time, and holds no contents when no capture was
    // on: the list that executes it takes `extra` from `refreshFrom` on again (OnExecuteBundle).
    std::shared_ptr<const ExtraRefresh> refresh;
    size_t refreshFrom = 0;
};

using CommandList = std::vector<RecordedCommand>;

/** A render target bound by OMSetRenderTargets / BeginRenderPass, resolved to what the read-back needs. */
struct BoundTarget
{
    ID3D12Resource* resource = nullptr;   // not AddRef'd
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;   // the view's format (the resource's when the view had none)
    uint32_t mip = 0;
    uint32_t firstSlice = 0;
    uint32_t sliceCount = 1;
    bool depth = false;
    bool readOnlyDepth = false;
    uint32_t attachment = 0;              // RTV index, or the number of RTVs for the depth target
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    /**
     * How a real render pass starts the attachment (BeginRenderPass). OMSetRenderTargets has no
     * such thing -- the attachment keeps what it held and the application clears it with a command
     * of its own -- so it is PRESERVE there, which is what it means. A measurement that starts from
     * a copy of the attachment reads this to know whether to copy it or to clear it (overdraw.h).
     */
    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE beginAccess = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE stencilBeginAccess = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
    D3D12_CLEAR_VALUE clearValue{};
};

/** The render pass open in the list, synthesized from OMSetRenderTargets or a real BeginRenderPass. */
struct ActivePass
{
    bool active = false;
    bool renderPassApi = false;           // BeginRenderPass/EndRenderPass rather than OMSetRenderTargets
    /**
     * The pass is suspended across command lists: BeginRenderPass carried SUSPENDING or RESUMING.
     * Between a suspension and its resume no GPU work of any kind may be issued -- not a copy, not
     * a barrier, not a ResolveQueryData -- and a list that holds any closes with E_FAIL, which is
     * fatal to the application. So the capture adds no copies and no read-back to a split pass
     * (README.md, "Passes"). It does take a timestamp pair: a timestamp is a single EndQuery, which
     * is allowed inside the pass region, and nothing is resolved in the application's lists any
     * more -- the capture resolves every query itself at the finish.
     */
    bool split = false;
    /** The pass ends suspended: the list takes no GPU work after it either, until it is closed. */
    bool suspending = false;
    /**
     * The end timestamp has been written. A split pass writes it *before* EndRenderPass is
     * forwarded (Hook_EndRenderPass), since after that the pass is suspended and the runtime takes
     * nothing; every other pass writes it where the pass ends, as before.
     */
    bool timestampEnded = false;
    std::vector<BoundTarget> targets;
    uint32_t passIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t beginCommand = 0;            // the recorded index of the command that opened it
    /** Queries of the capture around the pass (UINT32_MAX when not profiled). */
    uint32_t timestampQuery = UINT32_MAX;   // pair: begin, begin + 1
    uint32_t statsQuery = UINT32_MAX;
    uint32_t occlusionQuery = UINT32_MAX;
    uint32_t drawCount = 0;
};

/** A run of dispatches outside a render pass, timed as a compute pass. */
struct ActiveComputePass
{
    bool active = false;
    uint32_t index = 0;
    uint32_t timestampQuery = UINT32_MAX;
};

/** What the list has bound, for snapshots and the pass read-back. */
struct ListState
{
    ID3D12PipelineState* pipeline = nullptr;
    /** SetPipelineState1: the state object a trace runs, which is whose shader identifiers its binding table holds. */
    ID3D12StateObject* stateObject = nullptr;
    ID3D12RootSignature* graphicsRootSignature = nullptr;
    ID3D12RootSignature* computeRootSignature = nullptr;
    std::shared_ptr<const RootSignatureInfo> graphicsLayout;
    std::shared_ptr<const RootSignatureInfo> computeLayout;
    ID3D12DescriptorHeap* heaps[2] = {nullptr, nullptr};   // CBV_SRV_UAV, sampler
    /**
     * The heaps a draw or dispatch ran with under a root signature that lets its shaders index
     * them directly (D3D12_ROOT_SIGNATURE_FLAG_*_HEAP_DIRECTLY_INDEXED, shader model 6.6): which
     * slots those read is up to the shader, so the submission carries the heaps' contents
     * (CaptureManager::IndexedHeapContents).
     */
    ID3D12DescriptorHeap* indexed[2] = {nullptr, nullptr};
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    /** Queries the application has open (BeginQuery without EndQuery), which ours must not nest inside. */
    uint32_t appQueryDepth = 0;
};

class CommandRecorder
{
public:
    CommandRecorder(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_COMMAND_LIST_TYPE type, bool bundle)
        : _device(device), _list(list), _type(type), _bundle(bundle) {}

    /** Appends a command; returns its slot (position in the recording). */
    uint32_t Record(const char* method, std::string args)
    {
        _commands->push_back({method, std::move(args), _captureStacks ? StackExtraJson(CaptureStack(1)) : std::string()});
        return (uint32_t)_commands->size() - 1;
    }
    /** Appends extra JSON (a pre-separated member list) to the most recently recorded command. */
    void SetExtraOnLast(std::string extra)
    {
        if (!_commands->empty())
            _commands->back().extra += extra;
    }
    /** The same for a snapshot, `take(recorder)`: a bundle keeps how to take it again (RecordedCommand::refresh), which costs the others nothing. */
    template <typename F>
    void SetSnapshotOnLast(F&& take)
    {
        if (!_commands->empty())
            SetSnapshotOn(_commands->size() - 1, take);
    }
    template <typename F>
    void SetSnapshotOn(size_t slot, F&& take)
    {
        if (slot >= _commands->size())
            return;
        RecordedCommand& c = (*_commands)[slot];
        if (_bundle && !c.refresh)
        {
            c.refreshFrom = c.extra.size();
            c.refresh = std::make_shared<const ExtraRefresh>(take);
        }
        c.extra += take(this);
    }
    /**
     * A descriptor table's snapshot, held until the list next draws (or dispatches, for a compute
     * table). A table names slots of a heap, and what is in them counts when the GPU reads them:
     * an engine binds the table and then writes its descriptors (Unity does, every draw), so a
     * snapshot taken at the bind holds what the slots had the frame before.
     */
    void DeferSnapshot(bool compute, ExtraRefresh take)
    {
        if (!_commands->empty())
            _deferred.push_back({_commands->size() - 1, compute, std::move(take)});
    }
    /** Takes the held snapshots: before a draw (graphics), a dispatch (compute), or what may be either. */
    void FlushSnapshots(bool graphics, bool compute)
    {
        if (_deferred.empty())
            return;
        size_t kept = 0;
        for (size_t i = 0; i < _deferred.size(); ++i)
        {
            DeferredSnapshot& d = _deferred[i];
            if (d.compute ? compute : graphics)
                SetSnapshotOn(d.slot, d.take);
            else if (kept != i)
                _deferred[kept++] = std::move(d);
            else
                ++kept;
        }
        _deferred.resize(kept);
    }
    void SetCaptureStacks(bool on) { _captureStacks = on; }

    /** Frozen snapshot of the commands recorded so far (shared with the capture; a Reset starts a new list). */
    std::shared_ptr<const CommandList> Snapshot() const { return _commands; }
    size_t commandCount() const { return _commands->size(); }

    void Reset()
    {
        _commands = std::make_shared<CommandList>();
        _pass = ActivePass{};
        _passCount = 0;
        _compute = ActiveComputePass{};
        _computeCount = 0;
        _state = ListState{};
        _closed = false;
        _adopted = false;
        _deferred.clear();
    }

    ID3D12Device* device() const { return _device; }
    ID3D12GraphicsCommandList* list() const { return _list; }
    D3D12_COMMAND_LIST_TYPE type() const { return _type; }
    bool bundle() const { return _bundle; }
    ActivePass& pass() { return _pass; }
    uint32_t NextPassIndex() { return _passCount++; }
    ActiveComputePass& compute() { return _compute; }
    uint32_t NextComputeIndex() { return _computeCount++; }
    ListState& state() { return _state; }
    bool closed() const { return _closed; }
    void MarkClosed() { _closed = true; }
    /**
     * The recorder was made at the list's first call seen, not at its Reset (CaptureManager::Adopt):
     * what state the list was in is not known -- it may be inside a render pass begun before -- so
     * what its draws read is copied after the submission, as for a suspended pass (HeldCopiesOf). A pass the capture sees begin is known, and takes its
     * timestamps and its read-back like any other; no statistics or occlusion (the application may
     * have a query of its own open) and none of the measurements (overdraw.h).
     */
    bool adopted() const { return _adopted; }
    void MarkAdopted() { _adopted = true; }
    /** The recorded commands' frame, set when the list is executed during the capture (UINT32_MAX until then). */
    uint32_t frame = UINT32_MAX;

private:
    ID3D12Device* _device;
    ID3D12GraphicsCommandList* _list;
    D3D12_COMMAND_LIST_TYPE _type;
    bool _bundle;
    std::shared_ptr<CommandList> _commands = std::make_shared<CommandList>();
    ActivePass _pass;
    uint32_t _passCount = 0;
    ActiveComputePass _compute;
    uint32_t _computeCount = 0;
    ListState _state;
    struct DeferredSnapshot
    {
        size_t slot;
        bool compute;
        ExtraRefresh take;
    };
    std::vector<DeferredSnapshot> _deferred;
    bool _adopted = false;
    bool _captureStacks = false;
    bool _closed = false;
};

/**
 * The command being recorded on this thread, for a validation message fired inside it
 * (validation.h): the hooks open one at entry when the list has a recorder, and the slot is the
 * recorder's count at that moment, which is what the command will be recorded at.
 */
struct CommandScope
{
    explicit CommandScope(CommandRecorder* rec);
    ~CommandScope();
    /** The list id and slot of the command in flight on this thread; false when none. */
    static bool Current(uint64_t& listId, uint32_t& slot);
};

}  // namespace dxinsp
