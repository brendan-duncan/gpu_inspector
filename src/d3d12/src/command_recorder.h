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

#include <memory>
#include <string>
#include <vector>

namespace dxinsp {

struct RecordedCommand {
    std::string method;
    std::string args;      // JSON object with the command's arguments, or empty
    std::string extra;     // pre-separated member list merged into the entry: ,"descriptors":{...},"stack":[...]
};

using CommandList = std::vector<RecordedCommand>;

/** A render target bound by OMSetRenderTargets / BeginRenderPass, resolved to what the read-back needs. */
struct BoundTarget {
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
struct ActivePass {
    bool active = false;
    bool renderPassApi = false;           // BeginRenderPass/EndRenderPass rather than OMSetRenderTargets
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
struct ActiveComputePass {
    bool active = false;
    uint32_t index = 0;
    uint32_t timestampQuery = UINT32_MAX;
};

/** What the list has bound, for snapshots and the pass read-back. */
struct ListState {
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12RootSignature* graphicsRootSignature = nullptr;
    ID3D12RootSignature* computeRootSignature = nullptr;
    std::shared_ptr<const RootSignatureInfo> graphicsLayout;
    std::shared_ptr<const RootSignatureInfo> computeLayout;
    ID3D12DescriptorHeap* heaps[2] = {nullptr, nullptr};   // CBV_SRV_UAV, sampler
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    /** Queries the application has open (BeginQuery without EndQuery), which ours must not nest inside. */
    uint32_t appQueryDepth = 0;
};

class CommandRecorder {
public:
    CommandRecorder(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_COMMAND_LIST_TYPE type, bool bundle)
        : _device(device), _list(list), _type(type), _bundle(bundle) {}

    /** Appends a command; returns its slot (position in the recording). */
    uint32_t Record(const char* method, std::string args) {
        _commands->push_back({method, std::move(args), _captureStacks ? StackExtraJson(CaptureStack(1)) : std::string()});
        return (uint32_t)_commands->size() - 1;
    }
    /** Appends extra JSON (a pre-separated member list) to the most recently recorded command. */
    void SetExtraOnLast(std::string extra) {
        if (!_commands->empty()) _commands->back().extra += extra;
    }
    void SetCaptureStacks(bool on) { _captureStacks = on; }

    /** Frozen snapshot of the commands recorded so far (shared with the capture; a Reset starts a new list). */
    std::shared_ptr<const CommandList> Snapshot() const { return _commands; }
    size_t commandCount() const { return _commands->size(); }

    void Reset() {
        _commands = std::make_shared<CommandList>();
        _pass = ActivePass{};
        _passCount = 0;
        _compute = ActiveComputePass{};
        _computeCount = 0;
        _state = ListState{};
        _closed = false;
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
    bool _captureStacks = false;
    bool _closed = false;
};

/**
 * The command being recorded on this thread, for a validation message fired inside it
 * (validation.h): the hooks open one at entry when the list has a recorder, and the slot is the
 * recorder's count at that moment, which is what the command will be recorded at.
 */
struct CommandScope {
    explicit CommandScope(CommandRecorder* rec);
    ~CommandScope();
    /** The list id and slot of the command in flight on this thread; false when none. */
    static bool Current(uint64_t& listId, uint32_t& slot);
};

}  // namespace dxinsp
