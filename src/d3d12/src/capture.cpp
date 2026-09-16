// Frame capture (capture.h): recorders per command list, the read-back of render targets, bound
// buffers and textures appended to the application's lists, GPU queries around every pass, and
// the stream of it all once the last captured frame's GPU work is done.
//
// Everything here runs on the application's threads, from inside its D3D12 calls. The library's
// own calls on the application's lists and device are made under ScopedInternal so the hooks
// forward them without recording or tracking; the objects the capture makes (query heaps,
// staging buffers, resolve textures, fences) are owned in ComPtrs and released when the capture
// is sent or the device goes away. Application objects are never AddRef'd: a device or list the
// application released is forgotten through OnDeviceReleased / OnListReleased.
#include "capture.h"
#include "cpu_timeline.h"

#include "d3d12_enums.gen.h"
#include "formats.h"
#include "hooks.h"
#include "json.h"
#include "overdraw.h"
#include "resources.h"
#include "tracker.h"
#include "transport.h"
#include "validation.h"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>

namespace dxinsp {

namespace {

constexpr uint32_t kTimestampQueries = 32768;
constexpr uint32_t kStatsQueries = 8192;
constexpr uint32_t kOcclusionQueries = 8192;
constexpr uint32_t kPassSlots = 8192;
// One pass's resolved queries in the readback buffer: two timestamps, the statistics, the occlusion count.
constexpr uint64_t kStatsOffset = 16;
constexpr uint64_t kOcclusionOffset = kStatsOffset + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
constexpr uint64_t kSlotBytes = kOcclusionOffset + 8;
constexpr uint64_t kStagingChunkBytes = 64ull << 20;
constexpr uint64_t kStagingAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;   // 512: what a placed footprint needs
constexpr uint32_t kMaxAttachmentSlices = 16;
constexpr size_t kCommandBatch = 500;

inline uint64_t Align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

/** Whether a copy from a resource in `state` needs a transition: COMMON promotes, and any state with the COPY_SOURCE bit will do. */
inline bool NeedsCopyBarrier(D3D12_RESOURCE_STATES state) {
    return state != D3D12_RESOURCE_STATE_COMMON && !(state & D3D12_RESOURCE_STATE_COPY_SOURCE);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t subresource,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

/** The description of a resource: the tracker's, else asked of the resource itself. */
bool DescOf(ID3D12Resource* resource, D3D12_RESOURCE_DESC& desc, ResourceInfo* info = nullptr) {
    ResourceInfo local;
    if (ResourceTracker::Get().Get(resource, local)) {
        desc = local.desc;
        // The tracker keeps the creation description, where MipLevels 0 asked for a full chain;
        // the read-back needs the count the runtime chose.
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && desc.MipLevels == 0 && resource) {
            ScopedInternal internal;
            desc = resource->GetDesc();
            local.desc = desc;
        }
        if (info) *info = local;
        return true;
    }
    if (!resource) return false;
    ScopedInternal internal;
    desc = resource->GetDesc();
    if (info) {
        *info = ResourceInfo{};
        info->resource = resource;
        info->desc = desc;
    }
    return true;
}

/** The protocol format a target or texture of `format` is sent under, and whether it travels as the depth aspect. */
struct ProtocolFormat {
    const char* name = nullptr;
    bool depth = false;
    uint32_t texelBytes = 0;     // bytes of one texel (or block) in the tightly packed data
    uint32_t blockWidth = 1;
    uint32_t blockHeight = 1;
};

ProtocolFormat ProtocolFormatOf(DXGI_FORMAT format, bool asDepth) {
    ProtocolFormat p;
    const DXGI_FORMAT typed = TypedFormat(format, asDepth);
    const FormatInfo info = FormatOf(typed);
    p.name = info.protocolName;
    p.depth = info.depth;
    p.blockWidth = info.blockWidth;
    p.blockHeight = info.blockHeight;
    // The depth plane of a depth-stencil texture copies as its depth format alone: 32 bits for
    // D32 and D24 (with 8 bits of padding), 16 for D16, which is how the UI sizes the depth aspect
    // of VK_FORMAT_D24_UNORM_S8_UINT and VK_FORMAT_D32_SFLOAT_S8_UINT (texture_decode.ts).
    if (p.depth) p.texelBytes = typed == DXGI_FORMAT_D16_UNORM ? 2 : 4;
    else p.texelBytes = info.bytes;
    return p;
}

uint64_t TightRowBytes(const ProtocolFormat& p, uint32_t width) {
    return (uint64_t)((width + p.blockWidth - 1) / p.blockWidth) * p.texelBytes;
}
uint32_t TightRows(const ProtocolFormat& p, uint32_t height) {
    return (height + p.blockHeight - 1) / p.blockHeight;
}

uint32_t MipDim(uint64_t dim, uint32_t mip) {
    const uint64_t v = dim >> mip;
    return v ? (uint32_t)v : 1u;
}

// ---------------------------------------------------------------------------------------------
// The command in flight on this thread (validation.h attaches messages to it)

struct ScopeState {
    CommandRecorder* rec = nullptr;
    uint32_t slot = 0;
};
// Scopes nest with the hooks that open them (ExecuteBundle inside a list method, at most), so a
// small fixed stack per thread does, without an allocation in every hooked call. Every scope
// pushes, with or without a recorder, so the destructor always has one to pop.
constexpr uint32_t kMaxScopeDepth = 8;
thread_local ScopeState t_scopes[kMaxScopeDepth];
thread_local uint32_t t_scopeDepth = 0;

}  // namespace

CommandScope::CommandScope(CommandRecorder* rec) {
    if (t_scopeDepth < kMaxScopeDepth) t_scopes[t_scopeDepth] = {rec, rec ? (uint32_t)rec->commandCount() : 0};
    t_scopeDepth++;
}

CommandScope::~CommandScope() {
    if (t_scopeDepth) t_scopeDepth--;
}

bool CommandScope::Current(uint64_t& listId, uint32_t& slot) {
    if (!t_scopeDepth || t_scopeDepth > kMaxScopeDepth) return false;
    const ScopeState& s = t_scopes[t_scopeDepth - 1];
    if (!s.rec) return false;
    listId = Tracker::Get().IdOf(s.rec->list());
    slot = s.slot;
    return listId != 0;
}

// ---------------------------------------------------------------------------------------------
// What a capture holds

namespace {

struct StagingChunk {
    ComPtr<ID3D12Resource> buffer;
    uint64_t size = 0;
    uint64_t used = 0;
    void* mapped = nullptr;
};

/** A staged copy of one subresource: where its rows are in the chunk and how to pack them tightly. */
struct StagedRegion {
    uint64_t offset = 0;      // from the chunk's start
    uint32_t rowPitch = 0;    // as copied (256-aligned)
    uint32_t rows = 0;        // rows (block rows) per slice
    uint64_t rowBytes = 0;    // tight bytes per row
    uint32_t slices = 1;      // depth slices of a 3D subresource, each rows x rowPitch
};

struct TextureEntry {
    uint64_t resourceId = 0;
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;   // whose execution gives the frame
    uint64_t listId = 0;
    uint32_t passIndex = 0;
    uint32_t attachment = 0;
    const char* format = "VK_FORMAT_UNDEFINED";
    bool depthAspect = false;
    bool stencilAspect = false;   // plane 1 of a depth-stencil target, one byte per texel
    uint32_t width = 0, height = 0, depth = 1, layers = 1, mip = 0, mips = 1;
    uint32_t samples = 1;
    uint64_t size = 0;        // tight bytes
    bool sampled = false;
    uint32_t captureId = 0;
    bool failed = false;
    std::string note;
    ID3D12Device* device = nullptr;
    uint32_t chunk = 0;
    std::vector<StagedRegion> regions;   // in the order of the tight data
};

struct BufferEntry {
    uint32_t id = 0;
    uint64_t bufferId = 0;
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t listId = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t originalSize = 0;   // when truncated
    bool failed = false;
    std::string note;
    ID3D12Device* device = nullptr;
    uint32_t chunk = 0;
    uint64_t stagingOffset = 0;
};

struct TimingEntry {
    ID3D12Device* device = nullptr;
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t listId = 0;
    uint32_t passIndex = 0;
    bool compute = false;
    uint32_t slot = 0;
    bool hasStats = false;
    bool hasOcclusion = false;
};

struct SubmittedList {
    uint64_t listId = 0;
    std::shared_ptr<const CommandList> commands;   // null: recorded before the capture
};

/** An ExecuteCommandLists, or the Present that ended a frame, in the order they happened. */
struct Submission {
    bool present = false;
    uint64_t objectId = 0;      // the queue, or the swap chain
    uint32_t frame = 0;
    std::string args;
    std::vector<SubmittedList> lists;
};

struct ResolveKey {
    ID3D12GraphicsCommandList* list;
    DXGI_FORMAT format;
    uint32_t width, height, layers;
    bool operator<(const ResolveKey& o) const {
        return std::tie(list, format, width, height, layers) < std::tie(o.list, o.format, o.width, o.height, o.layers);
    }
};

/** A capture's objects on one device: query heaps, the slots their results resolve into, staging, resolves, the fence. */
struct DeviceCapture {
    ID3D12Device* device = nullptr;
    std::mutex mutex;   // staging, resolves, queues
    ComPtr<ID3D12QueryHeap> timestampHeap;
    ComPtr<ID3D12QueryHeap> statsHeap;
    ComPtr<ID3D12QueryHeap> occlusionHeap;
    std::atomic<uint32_t> timestampsUsed{0};
    std::atomic<uint32_t> statsUsed{0};
    std::atomic<uint32_t> occlusionUsed{0};
    std::atomic<uint32_t> slotsUsed{0};
    ComPtr<ID3D12Resource> queryReadback;
    void* queryMapped = nullptr;
    std::vector<StagingChunk> staging;
    std::map<ResolveKey, ComPtr<ID3D12Resource>> resolves;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t fenceValue = 0;
    std::vector<ID3D12CommandQueue*> queues;   // executed lists during the capture (not AddRef'd)
    uint64_t frequency = 0;                    // ticks per second of the first direct queue seen

    ~DeviceCapture() {
        if (queryMapped && queryReadback) { ScopedInternal internal; queryReadback->Unmap(0, nullptr); }
        if (event) CloseHandle(event);
    }
};

/** A read-back copy held back, recorded later into the list it is given (the primary, for a bundle's). */
using DeferredCopy = std::function<void(ID3D12GraphicsCommandList*)>;

struct RecorderSlot {
    std::unique_ptr<CommandRecorder> rec;
    /** Copies queued inside a BeginRenderPass region, recorded when it ends (a copy may not interrupt a render pass). */
    std::vector<DeferredCopy> deferred;
};

}  // namespace

// How a device's frames are delimited, decided per device over its lifetime (a process can hold
// several D3D12 devices: a game and a background copy device, or, in Chrome's GPU process, Dawn's
// WebGPU device beside the compositor's). A device that presents ends its frames at the present;
// one that goes a long run of submissions without ever presenting ends them at every
// ExecuteCommandLists, the way the Vulkan layer falls back for an OpenXR application that never
// presents. DXINSP_FRAME_BOUNDARY forces one or the other.
struct DeviceFrame {
    enum class Boundary { Auto, Present, Submit };
    Boundary boundary = Boundary::Auto;
    bool presentSeen = false;
    uint64_t frameIndex = 0;             // this device's own frame count (its presents, or its submit boundaries)
    uint32_t submitsWithoutPresent = 0;
    ID3D12CommandQueue* lastQueue = nullptr;   // not AddRef'd; the queue a substitute boundary ran on
};

// A device is taken to have no swap chain after this many submissions without a present, the same
// threshold the Vulkan layer uses (layer.cpp, OnSubmitForFrames).
constexpr uint32_t kSubmitsWithoutPresent = 60;

enum class BoundaryOverride { Auto, Present, Submit };

struct CaptureManager::Impl {
    enum class State { Idle, Armed, Capturing };

    std::mutex mutex;   // the state machine, the options and the capture's entries
    State state = State::Idle;
    CaptureOptions options;
    uint64_t frameIndex = 0;
    uint32_t frameCount = 1;
    uint32_t framesDone = 0;
    IDXGISwapChain* homeSwapChain = nullptr;   // whose presents count the captured frames (null: a submit-delimited home)
    ID3D12Device* homeDevice = nullptr;        // the device the capture started on, whose frames it counts
    bool presentSeenAny = false;               // any hooked device has presented: a submit boundary then does not start a capture
    bool recordAlways = false;
    bool recordAlwaysRead = false;

    // Per-device frame boundary state, device lifetime (not the capture-scoped DeviceCapture).
    std::mutex frameMutex;
    std::unordered_map<ID3D12Device*, DeviceFrame> deviceFrames;
    BoundaryOverride boundaryOverride = BoundaryOverride::Auto;
    bool boundaryOverrideRead = false;
    DeviceFrame& FrameFor(ID3D12Device* device) { return deviceFrames[device]; }   // caller holds frameMutex
    BoundaryOverride Override() {
        std::lock_guard lock(frameMutex);
        if (!boundaryOverrideRead) {
            boundaryOverrideRead = true;
            const std::string v = ConfigValue("DXINSP_FRAME_BOUNDARY");
            if (v == "submit") boundaryOverride = BoundaryOverride::Submit;
            else if (v == "present") boundaryOverride = BoundaryOverride::Present;
        }
        return boundaryOverride;
    }
    std::vector<Submission> submissions;
    std::vector<TextureEntry> textures;
    std::vector<BufferEntry> buffers;
    std::vector<TimingEntry> timings;
    uint64_t bufferBytes = 0;
    uint64_t imageBytes = 0;
    uint64_t commandTotal = 0;
    std::map<std::tuple<ID3D12Resource*, uint64_t, uint64_t>, uint32_t> bufferIds;   // (buffer, offset, size) -> id
    std::unordered_map<ID3D12Resource*, uint32_t> textureIds;                        // resource -> sampled capture id

    std::shared_mutex recorderMutex;
    std::unordered_map<ID3D12GraphicsCommandList*, RecorderSlot> recorders;

    std::mutex deviceMutex;
    std::unordered_map<ID3D12Device*, std::unique_ptr<DeviceCapture>> devices;

    std::mutex swapChainMutex;
    std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> swapChains;

    // --- helpers implemented below ---
    DeviceCapture* CaptureFor(ID3D12Device* device);
    DeviceCapture* FindCapture(ID3D12Device* device);
    bool AllocateStaging(DeviceCapture& dc, uint64_t size, uint32_t& chunk, uint64_t& offset, ID3D12Resource** buffer);
    ID3D12Resource* ResolveTextureFor(DeviceCapture& dc, const ResolveKey& key);
    std::vector<DeferredCopy>* DeferredOf(CommandRecorder* rec);
    uint32_t CurrentFrame();   // frame ordinal of what is being recorded now (under mutex)
    void ReleaseCaptureObjects(DeviceCapture& dc);
    /** The mapped staging chunk an entry's data is in, or null. */
    const uint8_t* MappedChunk(ID3D12Device* device, uint32_t chunk);
    void SendTextures(std::vector<TextureEntry>& textures);
    void SendBuffers(std::vector<BufferEntry>& buffers);
    void SendPassTimings(const std::vector<TimingEntry>& timings, ID3D12Device* home);
    ID3D12CommandQueue* CalibrationQueue(ID3D12Device* device);
    /** The last captured frame ended: waits for the GPU, streams everything, releases the capture's objects. */
    void Finish(CaptureManager& cm, ID3D12Device* device);
};

namespace {

bool CreateReadbackBuffer(ID3D12Device* device, uint64_t size, ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ScopedInternal internal;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(out));
    if (FAILED(hr)) {
        LogAlways("capture: readback buffer of %llu bytes failed (%s)", (unsigned long long)size, HrText(hr).c_str());
        return false;
    }
    return true;
}

bool CreateQueryHeap(ID3D12Device* device, D3D12_QUERY_HEAP_TYPE type, uint32_t count, ID3D12QueryHeap** out) {
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = type;
    desc.Count = count;
    ScopedInternal internal;
    HRESULT hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(out));
    if (FAILED(hr)) Log("capture: query heap type %d failed (%s)", (int)type, HrText(hr).c_str());
    return SUCCEEDED(hr);
}

}  // namespace

DeviceCapture* CaptureManager::Impl::FindCapture(ID3D12Device* device) {
    std::lock_guard lock(deviceMutex);
    auto it = devices.find(device);
    return it == devices.end() ? nullptr : it->second.get();
}

DeviceCapture* CaptureManager::Impl::CaptureFor(ID3D12Device* device) {
    if (!device) return nullptr;
    std::lock_guard lock(deviceMutex);
    auto it = devices.find(device);
    if (it != devices.end()) return it->second.get();
    auto dc = std::make_unique<DeviceCapture>();
    dc->device = device;
    // The query heaps and their readback buffer live as long as the device: a capture's passes
    // reserve slots from them, and the counters start over with every capture.
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimestampQueries, dc->timestampHeap.put());
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, kStatsQueries, dc->statsHeap.put());
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_OCCLUSION, kOcclusionQueries, dc->occlusionHeap.put());
    if (CreateReadbackBuffer(device, kSlotBytes * kPassSlots, dc->queryReadback.put())) {
        // Kept mapped: a readback buffer may stay mapped while the GPU writes it, and the results
        // are only read once the fence says the last list has run.
        ScopedInternal internal;
        D3D12_RANGE none{0, 0};
        if (FAILED(dc->queryReadback->Map(0, &none, &dc->queryMapped))) dc->queryMapped = nullptr;
    }
    {
        ScopedInternal internal;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(dc->fence.put())))) dc->fence.reset();
    }
    dc->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DeviceCapture* raw = dc.get();
    devices[device] = std::move(dc);
    Log("capture: device %p takes part", (void*)device);
    return raw;
}

bool CaptureManager::Impl::AllocateStaging(DeviceCapture& dc, uint64_t size, uint32_t& chunk, uint64_t& offset, ID3D12Resource** buffer) {
    std::lock_guard lock(dc.mutex);
    size = Align(std::max<uint64_t>(size, 1), kStagingAlignment);
    if (dc.staging.empty() || dc.staging.back().used + size > dc.staging.back().size) {
        StagingChunk c;
        c.size = std::max(size, kStagingChunkBytes);
        if (!CreateReadbackBuffer(dc.device, c.size, c.buffer.put())) return false;
        dc.staging.push_back(std::move(c));
    }
    StagingChunk& c = dc.staging.back();
    chunk = (uint32_t)dc.staging.size() - 1;
    offset = c.used;
    c.used += size;
    if (buffer) *buffer = c.buffer.get();
    return true;
}

ID3D12Resource* CaptureManager::Impl::ResolveTextureFor(DeviceCapture& dc, const ResolveKey& key) {
    std::lock_guard lock(dc.mutex);
    auto it = dc.resolves.find(key);
    if (it != dc.resolves.end()) return it->second.get();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = key.width;
    desc.Height = key.height;
    desc.DepthOrArraySize = (UINT16)key.layers;
    desc.MipLevels = 1;
    desc.Format = key.format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> texture;
    {
        ScopedInternal internal;
        HRESULT hr = dc.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                        IID_PPV_ARGS(texture.put()));
        if (FAILED(hr)) {
            Log("capture: resolve texture %ux%u %s failed (%s)", key.width, key.height, FormatName(key.format), HrText(hr).c_str());
            return nullptr;
        }
    }
    ID3D12Resource* raw = texture.get();
    dc.resolves[key] = std::move(texture);
    return raw;
}

std::vector<DeferredCopy>* CaptureManager::Impl::DeferredOf(CommandRecorder* rec) {
    std::shared_lock lock(recorderMutex);
    auto it = recorders.find(rec->list());
    if (it == recorders.end() || it->second.rec.get() != rec) return nullptr;
    // The vector stays where it is until the list's entry goes, which only happens when the
    // application releases the list (nobody records into it then) or at Finish.
    return &it->second.deferred;
}

uint32_t CaptureManager::Impl::CurrentFrame() {
    return framesDone;
}

void CaptureManager::Impl::ReleaseCaptureObjects(DeviceCapture& dc) {
    std::lock_guard lock(dc.mutex);
    ScopedInternal internal;
    for (StagingChunk& c : dc.staging) {
        if (c.mapped && c.buffer) c.buffer->Unmap(0, nullptr);
        c.mapped = nullptr;
    }
    dc.staging.clear();
    dc.resolves.clear();
    dc.queues.clear();
}

// ---------------------------------------------------------------------------------------------
// The manager

CaptureManager& CaptureManager::Get() {
    static CaptureManager* instance = new CaptureManager();
    return *instance;
}

CaptureManager::Impl& CaptureManager::impl() {
    if (!_impl) _impl = new Impl();
    return *_impl;
}

void CaptureManager::RequestCapture(const CaptureOptions& options) {
    Impl& i = impl();
    std::lock_guard lock(i.mutex);
    if (i.state == Impl::State::Capturing) {
        Log("capture: request ignored, a capture is in progress");
        return;
    }
    i.options = options;
    i.frameCount = std::max(1u, options.frameCount);
    i.state = Impl::State::Armed;
    Log("capture armed: %u frame(s)%s", i.frameCount, options.atFrame == UINT64_MAX ? "" : " at a given frame");
}

bool CaptureManager::RecordAlways() const {
    Impl& i = const_cast<CaptureManager*>(this)->impl();
    std::lock_guard lock(i.mutex);
    if (!i.recordAlwaysRead) {
        i.recordAlwaysRead = true;
        i.recordAlways = ConfigFlag("DXINSP_RECORD_ALWAYS");
        if (i.recordAlways) const_cast<CaptureManager*>(this)->_recordActive.store(true, std::memory_order_relaxed);
    }
    return i.recordAlways;
}

void CaptureManager::SetRecordAlways(bool on) {
    Impl& i = impl();
    RecordAlways();   // the environment's setting is read before the UI's replaces it
    std::lock_guard lock(i.mutex);
    i.recordAlways = on;
    _recordActive.store(on || _capturing.load(std::memory_order_acquire), std::memory_order_relaxed);
    Log("record always: %s", on ? "on" : "off");
}

CommandRecorder* CaptureManager::LookupRecorder(ID3D12GraphicsCommandList* list) {
    Impl& i = impl();
    std::shared_lock lock(i.recorderMutex);
    auto it = i.recorders.find(list);
    return it == i.recorders.end() ? nullptr : it->second.rec.get();
}

void CaptureManager::OnListReset(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_COMMAND_LIST_TYPE type, bool bundle,
                                 ID3D12PipelineState* initialState) {
    // DXINSP_RECORD_ALWAYS is read here rather than at the first capture: a bundle an engine
    // records at start-up needs its recorder before anything asks for a capture.
    RecordAlways();
    if (!list) return;
    OnMeasuredListReset(list);   // nothing kept of the list is still in effect
    if (!ShouldRecord()) return;
    Impl& i = impl();
    bool stacks;
    {
        std::lock_guard lock(i.mutex);
        // A list reset while the capture is armed may well be recorded in its first frame.
        stacks = i.options.stacktraces && i.state != Impl::State::Idle;
    }
    CommandRecorder* rec = nullptr;
    {
        std::unique_lock lock(i.recorderMutex);
        RecorderSlot& slot = i.recorders[list];
        if (!slot.rec) slot.rec = std::make_unique<CommandRecorder>(device, list, type, bundle);
        else slot.rec->Reset();
        slot.deferred.clear();
        rec = slot.rec.get();
    }
    rec->SetCaptureStacks(stacks);
    if (initialState) rec->state().pipeline = initialState;
}

void CaptureManager::OnListReleased(ID3D12GraphicsCommandList* list) {
    OnMeasuredListReleased(list);
    if (!_impl) return;
    Impl& i = impl();
    std::unique_lock lock(i.recorderMutex);
    i.recorders.erase(list);
}

ID3D12CommandQueue* CaptureManager::PresentQueue(IDXGISwapChain* swapChain) {
    Impl& i = impl();
    std::lock_guard lock(i.swapChainMutex);
    auto it = i.swapChains.find(swapChain);
    return it == i.swapChains.end() ? nullptr : it->second;
}

void CaptureManager::OnSwapChainCreated(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue) {
    if (!swapChain) return;
    Impl& i = impl();
    std::lock_guard lock(i.swapChainMutex);
    i.swapChains[swapChain] = queue;
}

void CaptureManager::OnSwapChainReleased(IDXGISwapChain* swapChain) {
    if (!_impl) return;
    Impl& i = impl();
    {
        std::lock_guard lock(i.swapChainMutex);
        i.swapChains.erase(swapChain);
    }
    ID3D12Device* finishOn = nullptr;
    {
        std::lock_guard lock(i.mutex);
        if (i.homeSwapChain == swapChain) {
            // The swap chain whose presents delimit the capture is going away. Mid-capture that
            // would leave nothing to end the remaining frames on (the home device presents, so its
            // submits are not boundaries), so send what was captured; when only armed, drop it and
            // let the next boundary re-arm.
            if (i.state == Impl::State::Capturing) finishOn = i.homeDevice;
            i.homeSwapChain = nullptr;
        }
    }
    if (finishOn) i.Finish(*this, finishOn);
}

void CaptureManager::OnDeviceReleased(ID3D12Device* device) {
    if (!_impl) return;
    Impl& i = impl();
    // Its per-device frame state goes whether or not it ever took part in a capture.
    {
        std::lock_guard lock(i.frameMutex);
        i.deviceFrames.erase(device);
    }
    std::unique_ptr<DeviceCapture> dc;
    {
        std::lock_guard lock(i.deviceMutex);
        auto it = i.devices.find(device);
        if (it == i.devices.end()) return;
        dc = std::move(it->second);
        i.devices.erase(it);
    }
    // The device is going: its recorders' lists go with it, and the entries staged on it cannot
    // be read any more.
    {
        std::unique_lock lock(i.recorderMutex);
        for (auto it = i.recorders.begin(); it != i.recorders.end();) {
            if (it->second.rec && it->second.rec->device() == device) it = i.recorders.erase(it);
            else ++it;
        }
    }
    {
        std::lock_guard lock(i.mutex);
        for (TextureEntry& t : i.textures) {
            if (t.device == device && !t.failed) { t.failed = true; t.note = "device was released during the capture"; }
        }
        for (BufferEntry& b : i.buffers) {
            if (b.device == device && !b.failed) { b.failed = true; b.note = "device was released during the capture"; }
        }
        for (TimingEntry& t : i.timings)
            if (t.device == device) t.frame = UINT32_MAX;
    }
    i.ReleaseCaptureObjects(*dc);
    OnMeasurementDeviceReleased(device);
    dc.reset();   // the query heaps, the fence and the readback buffer, before the device's last Release runs
    Log("capture: device %p released", (void*)device);
}

// ---------------------------------------------------------------------------------------------
// Passes

namespace {

/** `count` consecutive queries from a heap's counter, or UINT32_MAX when the heap is full. */
uint32_t ReserveQueries(std::atomic<uint32_t>& used, uint32_t count, uint32_t limit) {
    const uint32_t first = used.fetch_add(count, std::memory_order_relaxed);
    if (first + count > limit) return UINT32_MAX;
    return first;
}

/** Whether a list of this type can carry the capture's timestamp queries (a copy list needs a heap of another type). */
bool TimestampsAllowed(D3D12_COMMAND_LIST_TYPE type) {
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
}

}  // namespace

uint32_t CaptureManager::BeginPass(CommandRecorder* rec, std::vector<BoundTarget> targets, bool renderPassApi) {
    if (!rec) return 0;
    Impl& i = impl();
    ActivePass& pass = rec->pass();
    pass = ActivePass{};
    pass.active = true;
    pass.renderPassApi = renderPassApi;
    pass.targets = std::move(targets);
    pass.passIndex = rec->NextPassIndex();
    pass.beginCommand = rec->commandCount() ? (uint32_t)rec->commandCount() - 1 : 0;
    if (!pass.targets.empty()) {
        D3D12_RESOURCE_DESC desc;
        const BoundTarget& t = pass.targets.front();
        if (t.resource && DescOf(t.resource, desc)) {
            pass.width = MipDim(desc.Width, t.mip);
            pass.height = MipDim(desc.Height, t.mip);
        }
    }
    // A run of dispatches ends where a render pass begins.
    OnComputePassEnd(rec);

    bool profile;
    {
        std::lock_guard lock(i.mutex);
        profile = i.state == Impl::State::Capturing && i.options.profilePasses;
    }
    if (!profile || rec->bundle() || !TimestampsAllowed(rec->type())) return pass.passIndex;
    DeviceCapture* dc = i.CaptureFor(rec->device());
    if (!dc || !dc->timestampHeap || !dc->queryMapped) return pass.passIndex;
    const uint32_t slot = dc->slotsUsed.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kPassSlots) return pass.passIndex;   // the counter keeps climbing; only the first kPassSlots passes are timed
    pass.timestampQuery = slot * 2;
    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = rec->list();
    list->EndQuery(dc->timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, pass.timestampQuery);
    // Statistics and occlusion need graphics; neither is begun inside a BeginRenderPass region
    // (the resolve at pass end would have to wait for EndRenderPass, and BeginQuery there is not
    // something every driver accepts), nor while the application has a query of its own open.
    if (rec->type() != D3D12_COMMAND_LIST_TYPE_DIRECT || renderPassApi) return pass.passIndex;
    if (dc->statsHeap) {
        pass.statsQuery = ReserveQueries(dc->statsUsed, 1, kStatsQueries);
        if (pass.statsQuery != UINT32_MAX) list->BeginQuery(dc->statsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, pass.statsQuery);
    }
    if (dc->occlusionHeap && rec->state().appQueryDepth == 0) {
        pass.occlusionQuery = ReserveQueries(dc->occlusionUsed, 1, kOcclusionQueries);
        if (pass.occlusionQuery != UINT32_MAX) list->BeginQuery(dc->occlusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, pass.occlusionQuery);
    }
    return pass.passIndex;
}

void CaptureManager::OnDraw(CommandRecorder* rec) {
    if (rec && rec->pass().active) rec->pass().drawCount++;
}

namespace {

/** The queries of a pass or compute pass ended and resolved into the pass's slot of the readback buffer. */
void EndPassQueries(DeviceCapture& dc, ID3D12GraphicsCommandList* list, uint32_t timestampQuery, uint32_t statsQuery,
                    uint32_t occlusionQuery) {
    ScopedInternal internal;
    const uint32_t slot = timestampQuery / 2;
    const uint64_t base = (uint64_t)slot * kSlotBytes;
    ID3D12Resource* readback = dc.queryReadback.get();
    if (statsQuery != UINT32_MAX) {
        list->EndQuery(dc.statsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, statsQuery);
        list->ResolveQueryData(dc.statsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, statsQuery, 1, readback, base + kStatsOffset);
    }
    if (occlusionQuery != UINT32_MAX) {
        list->EndQuery(dc.occlusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, occlusionQuery);
        list->ResolveQueryData(dc.occlusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, occlusionQuery, 1, readback, base + kOcclusionOffset);
    }
    list->EndQuery(dc.timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, timestampQuery + 1);
    list->ResolveQueryData(dc.timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, timestampQuery, 2, readback, base);
}

/** Records the copy of one subresource into a placed footprint of the staging buffer, with the barriers its state needs. */
void CopySubresource(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t subresource, D3D12_RESOURCE_STATES state,
                     ID3D12Resource* staging, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
    ScopedInternal internal;
    const bool barrier = NeedsCopyBarrier(state);
    if (barrier) Transition(list, resource, subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = subresource;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (barrier) Transition(list, resource, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

}  // namespace

void CaptureManager::EndPass(CommandRecorder* rec, bool synthetic) {
    if (!rec) return;
    Impl& i = impl();
    ActivePass& pass = rec->pass();
    if (!pass.active) return;
    ID3D12GraphicsCommandList* list = rec->list();
    bool capturing, captureTextures;
    uint64_t maxTextureSize;
    {
        std::lock_guard lock(i.mutex);
        capturing = i.state == Impl::State::Capturing;
        captureTextures = i.options.captureTextures;
        maxTextureSize = i.options.maxTextureSize;
    }
    DeviceCapture* dc = capturing ? i.CaptureFor(rec->device()) : nullptr;
    const uint64_t listId = Tracker::Get().IdOf(list);

    // The queries first: what the pass drew is what they count, not the copies that follow.
    if (dc && pass.timestampQuery != UINT32_MAX) {
        EndPassQueries(*dc, list, pass.timestampQuery, pass.statsQuery, pass.occlusionQuery);
        TimingEntry te;
        te.device = rec->device();
        te.list = list;
        te.listId = listId;
        te.passIndex = pass.passIndex;
        te.compute = false;
        te.slot = pass.timestampQuery / 2;
        te.hasStats = pass.statsQuery != UINT32_MAX;
        te.hasOcclusion = pass.occlusionQuery != UINT32_MAX;
        std::lock_guard lock(i.mutex);
        i.timings.push_back(te);
    }

    // Then every target, slice by slice, into staging. A depth-stencil target is read back twice:
    // its depth plane, then its stencil plane (`asStencil`: plane 1, one byte per texel), each an
    // entry of its own under the same attachment index.
    if (dc && captureTextures && !rec->bundle()) {
        auto readBack = [&](const BoundTarget& t, bool asStencil) {
            TextureEntry e;
            e.resourceId = Tracker::Get().IdOf(t.resource);
            e.list = list;
            e.listId = listId;
            e.passIndex = pass.passIndex;
            e.attachment = t.attachment;
            e.mip = t.mip;
            e.device = rec->device();
            D3D12_RESOURCE_DESC desc{};
            ResourceInfo info;
            auto fail = [&](std::string why) {
                e.failed = true;
                e.note = std::move(why);
                std::lock_guard lock(i.mutex);
                i.textures.push_back(e);
            };
            if (!DescOf(t.resource, desc, &info)) return fail("resource is not tracked");
            const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            ProtocolFormat pf = ProtocolFormatOf(t.format, t.depth);
            if (asStencil) pf.texelBytes = 1;
            e.width = MipDim(desc.Width, t.mip);
            e.height = MipDim(desc.Height, t.mip);
            e.samples = desc.SampleDesc.Count;
            e.depthAspect = pf.depth && !asStencil;
            e.stencilAspect = asStencil;
            // A 3D render target: its slices are the depth slices of the mip, copied whole.
            const uint32_t slices = volume ? MipDim(desc.DepthOrArraySize, t.mip) : std::min(std::max(1u, t.sliceCount), kMaxAttachmentSlices);
            e.layers = slices;
            if (!pf.name) return fail(std::string("format ") + FormatName(t.format) + " cannot be decoded");
            e.format = pf.name;
            const uint64_t rowBytes = TightRowBytes(pf, e.width);
            const uint32_t rows = TightRows(pf, e.height);
            e.size = rowBytes * rows * slices;
            if (e.size > maxTextureSize) return fail("exceeds max texture size");
            if (t.depth && e.samples > 1) return fail(asStencil ? "multisampled stencil is not read back" : "multisampled depth is not read back");
            if (t.mip >= desc.MipLevels) return fail("mip level out of range");
            // The stencil plane's subresources follow every mip of every slice of the depth plane.
            const uint32_t planeOffset = asStencil ? desc.MipLevels * (uint32_t)desc.DepthOrArraySize : 0;

            // A multisampled color target resolves into a single-sampled texture of the capture's
            // first; the copies then read that.
            ID3D12Resource* source = t.resource;
            D3D12_RESOURCE_DESC sourceDesc = desc;
            ID3D12Resource* resolve = nullptr;
            const DXGI_FORMAT typed = TypedFormat(t.format, t.depth);
            if (e.samples > 1) {
                ResolveKey key{list, typed, e.width, e.height, slices};
                resolve = i.ResolveTextureFor(*dc, key);
                if (!resolve) return fail("resolve texture could not be created");
                source = resolve;
                {
                    ScopedInternal internal;
                    sourceDesc = resolve->GetDesc();
                }
            }

            // Footprints per copied subresource, back to back at 512-byte offsets; one staging span for the target.
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(slices);
            std::vector<uint32_t> sourceSubresources(slices);
            uint64_t total = 0;
            {
                ScopedInternal internal;
                for (uint32_t s = 0; s < slices; ++s) {
                    const uint32_t slice = volume ? 0 : t.firstSlice + s;
                    const uint32_t sub = resolve ? s : (volume ? t.mip : t.mip + slice * desc.MipLevels) + planeOffset;
                    sourceSubresources[s] = sub;
                    UINT numRows = 0;
                    UINT64 rowSize = 0, bytes = 0;
                    rec->device()->GetCopyableFootprints(&sourceDesc, sub, 1, total, &footprints[s], &numRows, &rowSize, &bytes);
                    footprints[s].Offset = total;   // relative to the span; rebased once it is allocated
                    total += Align(bytes, kStagingAlignment);
                    if (volume) break;   // one subresource holds every depth slice
                }
            }
            uint64_t spanOffset = 0;
            ID3D12Resource* staging = nullptr;
            if (!i.AllocateStaging(*dc, total, e.chunk, spanOffset, &staging)) return fail("staging allocation failed");

            const uint32_t copies = volume ? 1 : slices;
            for (uint32_t s = 0; s < copies; ++s) {
                const uint32_t slice = volume ? 0 : t.firstSlice + s;
                const uint32_t origSub = (volume ? t.mip : t.mip + slice * desc.MipLevels) + planeOffset;
                bool known = false;
                D3D12_RESOURCE_STATES state = ResourceTracker::Get().StateIn(list, t.resource, origSub, &known);
                if (!known) {
                    state = t.depth ? (t.readOnlyDepth ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE)
                                    : D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = footprints[s];
                fp.Offset += spanOffset;
                if (resolve) {
                    ScopedInternal internal;
                    Transition(list, t.resource, origSub, state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                    list->ResolveSubresource(resolve, s, t.resource, origSub, typed);
                    Transition(list, t.resource, origSub, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, state);
                    Transition(list, resolve, s, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    CopySubresource(list, resolve, s, D3D12_RESOURCE_STATE_COPY_SOURCE, staging, fp);
                    Transition(list, resolve, s, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                } else {
                    CopySubresource(list, t.resource, origSub, state, staging, fp);
                }
                StagedRegion r;
                r.offset = fp.Offset;
                r.rowPitch = fp.Footprint.RowPitch;
                r.rows = rows;
                r.rowBytes = std::min<uint64_t>(rowBytes, fp.Footprint.RowPitch);
                r.slices = volume ? slices : 1;
                e.regions.push_back(r);
            }
            std::lock_guard lock(i.mutex);
            i.textures.push_back(std::move(e));
        };
        for (const BoundTarget& t : pass.targets) {
            if (!t.resource) continue;
            readBack(t, false);
            if (t.depth && FormatOf(TypedFormat(t.format, true)).stencil) readBack(t, true);
        }
    }

    // Copies queued inside a BeginRenderPass region were held for its end.
    if (std::vector<DeferredCopy>* deferred = i.DeferredOf(rec)) {
        std::vector<DeferredCopy> pending;
        pending.swap(*deferred);
        for (auto& fn : pending) fn(rec->list());
    }
    // The measurements the capture asked for, drawn into the same list now that the pass's own
    // work and its read-back are in it (overdraw.h). A real render pass the application never
    // ended leaves the list inside its region, where a measurement can bind nothing.
    EndMeasuredPass(rec, pass.renderPassApi && synthetic);
    if (synthetic) rec->Record("EndRenderTargets", std::string());
    pass.active = false;
}

void CaptureManager::OnBeforeDispatch(CommandRecorder* rec) {
    if (!rec) return;
    ActiveComputePass& compute = rec->compute();
    if (rec->pass().active || compute.active) return;   // inside a render pass the dispatch stays there
    compute = ActiveComputePass{};
    compute.active = true;
    compute.index = rec->NextComputeIndex();
    Impl& i = impl();
    bool profile;
    {
        std::lock_guard lock(i.mutex);
        profile = i.state == Impl::State::Capturing && i.options.profilePasses;
    }
    if (!profile || rec->bundle() || !TimestampsAllowed(rec->type())) return;
    DeviceCapture* dc = i.CaptureFor(rec->device());
    if (!dc || !dc->timestampHeap || !dc->queryMapped) return;
    const uint32_t slot = dc->slotsUsed.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kPassSlots) return;
    compute.timestampQuery = slot * 2;
    ScopedInternal internal;
    rec->list()->EndQuery(dc->timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, compute.timestampQuery);
}

void CaptureManager::OnComputePassEnd(CommandRecorder* rec) {
    if (!rec) return;
    ActiveComputePass& compute = rec->compute();
    if (!compute.active) return;
    compute.active = false;
    if (compute.timestampQuery == UINT32_MAX) return;
    Impl& i = impl();
    DeviceCapture* dc = i.FindCapture(rec->device());
    if (!dc) return;
    EndPassQueries(*dc, rec->list(), compute.timestampQuery, UINT32_MAX, UINT32_MAX);
    TimingEntry te;
    te.device = rec->device();
    te.list = rec->list();
    te.listId = Tracker::Get().IdOf(rec->list());
    te.passIndex = compute.index;
    te.compute = true;
    te.slot = compute.timestampQuery / 2;
    std::lock_guard lock(i.mutex);
    i.timings.push_back(te);
}

void CaptureManager::OnBeforeClose(ID3D12GraphicsCommandList* list) {
    CommandRecorder* rec = RecorderFor(list);
    if (!rec) return;
    EndPass(rec, true);
    OnComputePassEnd(rec);
    // A BeginRenderPass region left open at Close is the application's error; its deferred copies
    // still have to land somewhere, and the list is about to close.
    if (std::vector<DeferredCopy>* deferred = impl().DeferredOf(rec)) {
        std::vector<DeferredCopy> pending;
        pending.swap(*deferred);
        for (auto& fn : pending) fn(rec->list());
    }
    rec->MarkClosed();
}

// ---------------------------------------------------------------------------------------------
// Bindings: the descriptor snapshot on a root table or root view, and the read-back it queues

namespace {

/** The bytes of one element of a buffer view: its structure stride, 4 for a raw view, else the format's texel. */
uint32_t ElementStride(uint32_t structureByteStride, bool raw, DXGI_FORMAT format) {
    if (structureByteStride) return structureByteStride;
    if (raw) return 4;
    const uint32_t bytes = FormatOf(format).bytes;
    return bytes ? bytes : 4;
}

}  // namespace

uint32_t CaptureManager::QueueBufferCapture(CommandRecorder* rec, ID3D12Resource* buffer, UINT64 offset, UINT64 size) {
    if (!rec || !buffer) return 0;
    Impl& i = impl();
    ResourceInfo info;
    if (!ResourceTracker::Get().Get(buffer, info) || info.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return 0;
    if (offset >= info.desc.Width) return 0;
    size = std::min<UINT64>(size, info.desc.Width - offset);
    if (!size) return 0;
    DeviceCapture* dc = nullptr;
    {
        std::lock_guard lock(i.mutex);
        if (i.state != Impl::State::Capturing || !i.options.captureBuffers) return 0;
        auto it = i.bufferIds.find({buffer, offset, size});
        if (it != i.bufferIds.end()) return it->second;
    }
    dc = i.CaptureFor(rec->device());
    if (!dc) return 0;
    ID3D12GraphicsCommandList* list = rec->list();

    BufferEntry e;
    e.bufferId = Tracker::Get().IdOf(buffer);
    e.list = list;
    e.listId = Tracker::Get().IdOf(list);
    e.offset = offset;
    e.size = size;
    e.device = rec->device();
    ID3D12Resource* staging = nullptr;
    {
        std::lock_guard lock(i.mutex);
        if (i.state != Impl::State::Capturing) return 0;
        e.id = (uint32_t)i.buffers.size() + 1;
        i.bufferIds[{buffer, offset, size}] = e.id;
        if (info.heapType == D3D12_HEAP_TYPE_READBACK) {
            e.failed = true;
            e.note = "a buffer in a readback heap cannot be a copy source";
        } else {
            if (e.size > i.options.maxBufferSize) {
                e.originalSize = e.size;
                e.size = i.options.maxBufferSize;
            }
            if (i.bufferBytes + e.size > i.options.maxBufferTotal) {
                e.failed = true;
                e.note = "buffer capture budget exceeded";
            } else if (!i.AllocateStaging(*dc, e.size, e.chunk, e.stagingOffset, &staging)) {
                e.failed = true;
                e.note = "staging allocation failed";
            } else {
                i.bufferBytes += e.size;
            }
        }
        i.buffers.push_back(e);
    }
    if (e.failed) return e.id;

    // The copy, with the barriers the buffer's state needs. An upload-heap buffer is always
    // GENERIC_READ; one nothing transitioned is COMMON, which a copy promotes.
    const uint64_t copyOffset = e.offset, copySize = e.size, stagingOffset = e.stagingOffset;
    const D3D12_HEAP_TYPE heapType = info.heapType;
    auto copy = [buffer, staging, copyOffset, copySize, stagingOffset, heapType](ID3D12GraphicsCommandList* list) {
        ScopedInternal internal;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_GENERIC_READ;
        if (heapType != D3D12_HEAP_TYPE_UPLOAD) {
            bool known = false;
            state = ResourceTracker::Get().StateIn(list, buffer, 0, &known);
            if (!known) state = D3D12_RESOURCE_STATE_COMMON;
        }
        const bool barrier = NeedsCopyBarrier(state);
        if (barrier) Transition(list, buffer, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(staging, stagingOffset, buffer, copyOffset, copySize);
        if (barrier) Transition(list, buffer, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    };
    // A bundle cannot copy: its copies go into the list that executes it. Inside a BeginRenderPass
    // region they wait for its end.
    const ActivePass& pass = rec->pass();
    if (rec->bundle() || (pass.active && pass.renderPassApi)) {
        if (std::vector<DeferredCopy>* deferred = i.DeferredOf(rec)) deferred->push_back(std::move(copy));
        else if (!rec->bundle()) copy(list);
    } else {
        copy(list);
    }
    return e.id;
}

uint32_t CaptureManager::QueueAddressCapture(CommandRecorder* rec, D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 size) {
    if (!rec || !address) return 0;
    ID3D12Resource* buffer = nullptr;
    UINT64 offset = 0, remaining = 0;
    if (!AddressMap::Get().Resolve(address, buffer, offset, remaining)) return 0;
    return QueueBufferCapture(rec, buffer, offset, size ? std::min<UINT64>(size, remaining) : remaining);
}

uint32_t CaptureManager::QueueTextureCapture(CommandRecorder* rec, ID3D12Resource* texture) {
    if (!rec || !texture) return 0;
    Impl& i = impl();
    {
        std::lock_guard lock(i.mutex);
        if (i.state != Impl::State::Capturing || !i.options.captureImages) return 0;
        auto it = i.textureIds.find(texture);
        if (it != i.textureIds.end()) return it->second;
    }
    D3D12_RESOURCE_DESC desc{};
    ResourceInfo info;
    if (!DescOf(texture, desc, &info) || desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return 0;
    DeviceCapture* dc = i.CaptureFor(rec->device());
    if (!dc) return 0;
    ID3D12GraphicsCommandList* list = rec->list();
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;

    TextureEntry e;
    e.sampled = true;
    e.resourceId = Tracker::Get().IdOf(texture);
    e.list = list;
    e.listId = Tracker::Get().IdOf(list);
    e.passIndex = rec->pass().active ? rec->pass().passIndex : 0;
    e.attachment = 0;
    e.mip = 0;
    e.mips = std::max<uint32_t>(1, desc.MipLevels);
    e.width = (uint32_t)desc.Width;
    e.height = desc.Height;
    e.depth = volume ? desc.DepthOrArraySize : 1;
    e.layers = volume ? 1 : std::max<uint32_t>(1, desc.DepthOrArraySize);
    e.samples = desc.SampleDesc.Count;
    e.device = rec->device();
    const ProtocolFormat pf = ProtocolFormatOf(desc.Format, FormatOf(desc.Format).depth);
    e.depthAspect = pf.depth;
    if (pf.name) e.format = pf.name;

    // Every mip with all its layers (a volume's depth slices), tightly packed, back to back.
    uint64_t tight = 0;
    for (uint32_t m = 0; m < e.mips; ++m) {
        const uint32_t slices = volume ? MipDim(desc.DepthOrArraySize, m) : e.layers;
        tight += TightRowBytes(pf, MipDim(desc.Width, m)) * TightRows(pf, MipDim(desc.Height, m)) * slices;
    }
    e.size = tight;

    auto finish = [&](const char* why) {
        if (why) { e.failed = true; e.note = why; }
        std::lock_guard lock(i.mutex);
        if (i.state != Impl::State::Capturing) return 0u;
        e.captureId = (uint32_t)i.textures.size() + 1;
        i.textureIds[texture] = e.captureId;
        if (!e.failed) i.imageBytes += e.size;
        i.textures.push_back(e);
        return e.captureId;
    };
    if (e.samples > 1) return finish("multisampled textures bound as shader resources are not read back");
    if (!pf.name) return finish("format cannot be decoded");
    bool tooLarge, overBudget;
    {
        std::lock_guard lock(i.mutex);
        tooLarge = e.size > i.options.maxTextureSize;
        overBudget = i.imageBytes + e.size > i.options.maxImageTotal;
    }
    if (tooLarge) return finish("exceeds max texture size");
    if (overBudget) return finish("image capture budget exceeded");

    // Footprints of every subresource copied, in the order of the data, then one staging span.
    struct Copy {
        uint32_t subresource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    };
    std::vector<Copy> copies;
    uint64_t total = 0;
    {
        ScopedInternal internal;
        for (uint32_t m = 0; m < e.mips; ++m) {
            const uint32_t perMip = volume ? 1 : e.layers;
            for (uint32_t l = 0; l < perMip; ++l) {
                Copy c;
                c.subresource = m + l * desc.MipLevels;
                UINT numRows = 0;
                UINT64 rowSize = 0, bytes = 0;
                rec->device()->GetCopyableFootprints(&desc, c.subresource, 1, total, &c.footprint, &numRows, &rowSize, &bytes);
                c.footprint.Offset = total;
                total += Align(bytes, kStagingAlignment);
                copies.push_back(c);
            }
        }
    }
    uint64_t spanOffset = 0;
    ID3D12Resource* staging = nullptr;
    if (!i.AllocateStaging(*dc, total, e.chunk, spanOffset, &staging)) return finish("staging allocation failed");
    for (Copy& c : copies) {
        c.footprint.Offset += spanOffset;
        uint32_t m = 0, slice = 0, plane = 0;
        SubresourceOf(desc, c.subresource, m, slice, plane);
        StagedRegion r;
        r.offset = c.footprint.Offset;
        r.rowPitch = c.footprint.Footprint.RowPitch;
        r.rows = TightRows(pf, MipDim(desc.Height, m));
        r.rowBytes = std::min<uint64_t>(TightRowBytes(pf, MipDim(desc.Width, m)), c.footprint.Footprint.RowPitch);
        r.slices = volume ? MipDim(desc.DepthOrArraySize, m) : 1;
        e.regions.push_back(r);
    }
    const uint32_t id = finish(nullptr);
    if (!id) return 0;

    auto copy = [texture, staging, copies](ID3D12GraphicsCommandList* list) {
        for (const Copy& c : copies) {
            bool known = false;
            D3D12_RESOURCE_STATES state = ResourceTracker::Get().StateIn(list, texture, c.subresource, &known);
            if (!known) state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            CopySubresource(list, texture, c.subresource, state, staging, c.footprint);
        }
    };
    const ActivePass& pass = rec->pass();
    if (rec->bundle() || (pass.active && pass.renderPassApi)) {
        if (std::vector<DeferredCopy>* deferred = i.DeferredOf(rec)) deferred->push_back(std::move(copy));
        else if (!rec->bundle()) copy(list);
    } else {
        copy(list);
    }
    return id;
}

namespace {

/** The read-back a descriptor record names, queued; the capture id its `data` carries (0 for none). */
uint32_t QueueRecordData(CaptureManager& cm, CommandRecorder* rec, const DescriptorRecord& r) {
    switch (r.kind) {
        case DescriptorKind::CBV:
            return r.address ? cm.QueueAddressCapture(rec, r.address, r.size) : 0;
        case DescriptorKind::SRV:
        case DescriptorKind::UAV: {
            if (r.accelerationStructure || !r.resource) return 0;
            D3D12_RESOURCE_DESC desc{};
            if (!DescOf(r.resource, desc)) return 0;
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return cm.QueueTextureCapture(rec, r.resource);
            if (!r.hasDesc) return cm.QueueBufferCapture(rec, r.resource, 0, desc.Width);
            if (r.kind == DescriptorKind::SRV) {
                if (r.srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return 0;
                const uint32_t stride = ElementStride(r.srv.Buffer.StructureByteStride,
                                                      (r.srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0, r.srv.Format);
                return cm.QueueBufferCapture(rec, r.resource, r.srv.Buffer.FirstElement * stride, (UINT64)r.srv.Buffer.NumElements * stride);
            }
            if (r.uav.ViewDimension != D3D12_UAV_DIMENSION_BUFFER) return 0;
            const uint32_t stride = ElementStride(r.uav.Buffer.StructureByteStride,
                                                  (r.uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0, r.uav.Format);
            return cm.QueueBufferCapture(rec, r.resource, r.uav.Buffer.FirstElement * stride, (UINT64)r.uav.Buffer.NumElements * stride);
        }
        default:
            return 0;
    }
}

void BeginSnapshot(JsonWriter& w, bool compute, uint32_t parameterIndex, ID3D12DescriptorHeap* heap, ID3D12RootSignature* signature) {
    w.BeginObject();
    w.Key("bindPoint"); w.String(compute ? "compute" : "graphics");
    w.Key("sets"); w.BeginArray();
    w.BeginObject();
    w.Key("set"); w.Uint(parameterIndex);
    w.Key("descriptorSet"); WriteRef(w, heap, "ID3D12DescriptorHeap");
    w.Key("layout"); WriteRef(w, signature, "ID3D12RootSignature");
    w.Key("bindings"); w.BeginArray();
}

void EndSnapshot(JsonWriter& w) {
    w.EndArray();    // bindings
    w.EndObject();   // set
    w.EndArray();    // sets
    w.EndObject();
}

}  // namespace

void CaptureManager::SnapshotRootTable(CommandRecorder* rec, bool compute, uint32_t parameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE base) {
    if (!rec) return;
    ListState& state = rec->state();
    const std::shared_ptr<const RootSignatureInfo> layout = compute ? state.computeLayout : state.graphicsLayout;
    ID3D12RootSignature* signature = compute ? state.computeRootSignature : state.graphicsRootSignature;
    HeapInfo heap;
    uint32_t index = 0;
    const bool located = DescriptorTracker::Get().Locate(base, heap, index);

    JsonWriter w(&Tracker::Get());
    BeginSnapshot(w, compute, parameterIndex, located ? heap.heap : nullptr, signature);
    const RootParameterInfo* param = layout && parameterIndex < layout->parameters.size() ? &layout->parameters[parameterIndex] : nullptr;
    if (param && param->type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && located) {
        for (size_t k = 0; k < param->ranges.size(); ++k) {
            const RootRange& range = param->ranges[k];
            const uint32_t first = index + range.offsetInTable;
            uint32_t count = range.numDescriptors;
            if (count == UINT_MAX) {
                // Unbounded: the rest of the heap, within reason.
                count = first < heap.desc.NumDescriptors ? std::min(heap.desc.NumDescriptors - first, 1024u) : 0;
            }
            std::vector<DescriptorRecord> records = DescriptorTracker::Get().Slots(heap.heap, first, count);
            w.BeginObject();
            w.Key("binding"); w.Uint(k);
            w.Key("type"); w.Enum(ToString_D3D12_DESCRIPTOR_RANGE_TYPE((int64_t)range.type), (int64_t)range.type);
            w.Key("register"); w.Uint(range.baseRegister);
            w.Key("space"); w.Uint(range.space);
            w.Key("stages"); w.Enum(ToString_D3D12_SHADER_VISIBILITY((int64_t)param->visibility), (int64_t)param->visibility);
            w.Key("descriptors"); w.BeginArray();
            for (const DescriptorRecord& r : records) {
                const uint32_t dataId = range.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? 0 : QueueRecordData(*this, rec, r);
                WriteDescriptorRecord(w, r, dataId);
            }
            w.EndArray();
            w.EndObject();
        }
    }
    EndSnapshot(w);
    rec->SetExtraOnLast(",\"descriptors\":" + w.str());
}

void CaptureManager::SnapshotRootView(CommandRecorder* rec, bool compute, uint32_t parameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address) {
    if (!rec) return;
    ListState& state = rec->state();
    const std::shared_ptr<const RootSignatureInfo> layout = compute ? state.computeLayout : state.graphicsLayout;
    ID3D12RootSignature* signature = compute ? state.computeRootSignature : state.graphicsRootSignature;
    const RootParameterInfo* param = layout && parameterIndex < layout->parameters.size() ? &layout->parameters[parameterIndex] : nullptr;

    JsonWriter w(&Tracker::Get());
    BeginSnapshot(w, compute, parameterIndex, nullptr, signature);
    w.BeginObject();
    w.Key("binding"); w.Uint(0);
    w.Key("type");
    if (param) w.Enum(ToString_D3D12_ROOT_PARAMETER_TYPE((int64_t)param->type), (int64_t)param->type);
    else w.Null();
    w.Key("register"); w.Uint(param ? param->shaderRegister : 0);
    w.Key("space"); w.Uint(param ? param->space : 0);
    w.Key("stages");
    if (param) w.Enum(ToString_D3D12_SHADER_VISIBILITY((int64_t)param->visibility), (int64_t)param->visibility);
    else w.String("D3D12_SHADER_VISIBILITY_ALL");
    w.Key("descriptors"); w.BeginArray();
    ID3D12Resource* buffer = nullptr;
    UINT64 offset = 0, remaining = 0;
    if (address && AddressMap::Get().Resolve(address, buffer, offset, remaining)) {
        const uint32_t dataId = QueueBufferCapture(rec, buffer, offset, remaining);
        w.BeginObject();
        w.Key("buffer"); WriteRef(w, buffer, "ID3D12Resource");
        w.Key("offset"); w.Uint(offset);
        w.Key("range"); w.Uint(remaining);
        w.Key("data"); w.Uint(dataId);
        w.EndObject();
    } else {
        w.BeginObject();
        w.Key("buffer"); w.Null();
        w.Key("address"); w.String(Hex(address));
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    EndSnapshot(w);
    rec->SetExtraOnLast(",\"descriptors\":" + w.str());
}

// ---------------------------------------------------------------------------------------------
// Queues and frames

namespace {

/** Every capture entry recorded into `list` that has no frame yet ran in `frame`. */
template <typename Entry>
void AssignFrame(std::vector<Entry>& entries, ID3D12GraphicsCommandList* list, uint32_t frame) {
    for (Entry& e : entries)
        if (e.list == list && e.frame == UINT32_MAX) e.frame = frame;
}

template <typename Entry>
void RekeyList(std::vector<Entry>& entries, ID3D12GraphicsCommandList* from, ID3D12GraphicsCommandList* to) {
    for (Entry& e : entries)
        if (e.list == from && e.frame == UINT32_MAX) e.list = to;
}

void NoteQueue(DeviceCapture& dc, ID3D12CommandQueue* queue) {
    if (!queue) return;
    std::lock_guard lock(dc.mutex);
    if (std::find(dc.queues.begin(), dc.queues.end(), queue) != dc.queues.end()) return;
    dc.queues.push_back(queue);
    if (!dc.frequency) {
        ScopedInternal internal;
        D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        UINT64 frequency = 0;
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && SUCCEEDED(queue->GetTimestampFrequency(&frequency))) dc.frequency = frequency;
    }
}

}  // namespace

bool CaptureManager::OnExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, double cpuMs) {
    (void)cpuMs;
    if (!queue) return false;
    Impl& i = impl();
    // The device this submission belongs to: every list on one queue shares it. Found from a
    // recorded list, else from the queue itself.
    ID3D12Device* device = nullptr;
    for (UINT k = 0; k < count && !device; ++k) {
        auto* list = lists ? static_cast<ID3D12GraphicsCommandList*>(lists[k]) : nullptr;
        if (!list) continue;
        CommandRecorder* rec = LookupRecorder(list);
        device = rec ? rec->device() : DeviceOf(list);
    }
    if (!device) device = DeviceOf(queue);

    // Record the submission (only while capturing), into the frame the home boundary is on.
    if (IsCapturing()) {
        Submission s;
        s.objectId = Tracker::Get().IdOf(queue);
        {
            Args a;
            a.u("NumCommandLists", count);
            JsonWriter& w = a.key("ppCommandLists");
            w.BeginArray();
            for (UINT k = 0; k < count; ++k) WriteRef(w, lists ? lists[k] : nullptr, "ID3D12GraphicsCommandList");
            w.EndArray();
            s.args = a.str();
        }
        std::vector<ID3D12GraphicsCommandList*> executed;
        uint64_t commands = 0;
        for (UINT k = 0; k < count; ++k) {
            ID3D12GraphicsCommandList* list = lists ? static_cast<ID3D12GraphicsCommandList*>(lists[k]) : nullptr;
            if (!list) continue;
            CommandRecorder* rec = LookupRecorder(list);
            SubmittedList sl;
            sl.listId = Tracker::Get().IdOf(list);
            if (rec) sl.commands = rec->Snapshot();
            commands += sl.commands ? sl.commands->size() : 1;
            s.lists.push_back(std::move(sl));
            executed.push_back(list);
            // The queue is waited for at the finish, through the fence of the device its lists belong to.
            ID3D12Device* listDevice = rec ? rec->device() : DeviceOf(list);
            if (DeviceCapture* dc = i.CaptureFor(listDevice)) NoteQueue(*dc, queue);
        }
        std::lock_guard lock(i.mutex);
        if (i.state == Impl::State::Capturing) {
            s.frame = i.CurrentFrame();
            i.commandTotal += 1 + commands;
            for (ID3D12GraphicsCommandList* list : executed) {
                AssignFrame(i.textures, list, s.frame);
                AssignFrame(i.buffers, list, s.frame);
                AssignFrame(i.timings, list, s.frame);
                AssignMeasurementFrame(list, s.frame);
            }
            i.submissions.push_back(std::move(s));
        }
    }

    // The frame boundary for a device that never presents: settled per device below, and this
    // submission ends its frame once it has. Runs whether or not a capture is active, so the
    // decision is made from the application's normal submissions and a queued "capture frame N"
    // lands on the right one.
    if (!device) return false;
    const BoundaryOverride override = i.Override();
    if (override == BoundaryOverride::Present) return false;
    bool boundary = false;
    {
        std::lock_guard lock(i.frameMutex);
        DeviceFrame& df = i.FrameFor(device);
        if (df.boundary == DeviceFrame::Boundary::Present) return false;   // this device presents; presents delimit it
        df.lastQueue = queue;
        const uint32_t n = ++df.submitsWithoutPresent;
        if (df.boundary == DeviceFrame::Boundary::Auto) {
            if (override == BoundaryOverride::Submit || n >= kSubmitsWithoutPresent) {
                df.boundary = DeviceFrame::Boundary::Submit;
                Log("no present after %u submissions on device %p: its frames end at every ExecuteCommandLists", n, (void*)device);
            }
        }
        boundary = df.boundary == DeviceFrame::Boundary::Submit;
    }
    if (boundary) EndFrame(device, queue, nullptr, false);
    return boundary;
}

void CaptureManager::OnExecuteBundle(CommandRecorder* rec, ID3D12GraphicsCommandList* bundle) {
    if (!rec || !bundle) return;
    Impl& i = impl();
    CommandRecorder* bundleRec = LookupRecorder(bundle);
    if (!bundleRec) return;
    // The bundle's read-back copies (a bundle records none itself) go into the executing list, and
    // its entries count as this list's for the frame they run in.
    if (std::vector<DeferredCopy>* deferred = i.DeferredOf(bundleRec)) {
        std::vector<DeferredCopy> pending;
        pending.swap(*deferred);
        if (!pending.empty()) {
            {
                std::lock_guard lock(i.mutex);
                RekeyList(i.textures, bundle, rec->list());
                RekeyList(i.buffers, bundle, rec->list());
            }
            const ActivePass& pass = rec->pass();
            std::vector<DeferredCopy>* mine = pass.active && pass.renderPassApi ? i.DeferredOf(rec) : nullptr;
            if (mine) mine->insert(mine->end(), std::make_move_iterator(pending.begin()), std::make_move_iterator(pending.end()));
            else for (auto& fn : pending) fn(rec->list());
        }
    }
    std::shared_ptr<const CommandList> commands = bundleRec->Snapshot();
    if (!commands || commands->empty()) return;
    JsonWriter w;
    w.BeginArray();
    w.BeginObject();
    w.Key("commandBuffer"); w.Uint(Tracker::Get().IdOf(bundle));
    w.Key("commands"); w.BeginArray();
    uint32_t slot = 0;
    for (const RecordedCommand& c : *commands) {
        w.BeginObject();
        w.Key("method"); w.String(c.method);
        w.Key("args"); if (c.args.empty()) w.Null(); else w.Raw(c.args);
        w.Key("slot"); w.Uint(slot++);
        if (!c.extra.empty()) w.str() += c.extra;   // a pre-separated member list: ,"descriptors":{...}
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndArray();
    rec->SetExtraOnLast(",\"children\":" + w.str());
}

void CaptureManager::OnPresent(ID3D12Device* device, IDXGISwapChain* swapChain, ID3D12CommandQueue* queue) {
    Impl& i = impl();
    // DXINSP_FRAME_BOUNDARY=submit delimits every device by its submissions and ignores presents
    // for framing (the Chrome case: the compositor may present on a hooked device, but the work to
    // capture is Dawn's, which never presents). The present still updated the frame timing.
    if (i.Override() == BoundaryOverride::Submit) return;
    {
        // A device that presents is delimited by its presents from now on, whatever it did before,
        // and any present in the process settles the guard below.
        std::lock_guard lock(i.frameMutex);
        DeviceFrame& df = i.FrameFor(device);
        df.presentSeen = true;
        df.boundary = DeviceFrame::Boundary::Present;
        i.presentSeenAny = true;
    }
    EndFrame(device, queue ? queue : PresentQueue(swapChain), swapChain, true);
}

void CaptureManager::EndFrame(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* swapChain, bool present) {
    if (!device) return;
    Impl& i = impl();
    const uint64_t deviceFrame = _frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t deviceFrameIndex;
    {
        std::lock_guard lock(i.frameMutex);
        deviceFrameIndex = ++i.FrameFor(device).frameIndex;
    }
    const BoundaryOverride override = i.Override();

    bool finish = false;
    bool started = false;
    bool overdraw = false;
    PixelHistoryRequest pixelHistory;
    uint64_t maxTextureSize = 0;
    {
        std::lock_guard lock(i.mutex);
        if (i.state == Impl::State::Armed) {
            // A process that presents somewhere has its frames ended by those presents: a
            // background device's substitute submit boundary does not start the capture, unless
            // DXINSP_FRAME_BOUNDARY=submit forces it (the Chrome case, where the compositor may
            // present on a hooked device but the WebGPU work to capture is Dawn's, which does not).
            if (!present && i.presentSeenAny && override != BoundaryOverride::Submit) return;
            if (i.options.atFrame == UINT64_MAX || deviceFrameIndex >= i.options.atFrame) {
                i.state = Impl::State::Capturing;
                i.frameIndex = deviceFrame;
                i.framesDone = 0;
                i.homeDevice = device;
                i.homeSwapChain = present ? swapChain : nullptr;   // null: the home is delimited by submits
                i.submissions.clear();
                i.textures.clear();
                i.buffers.clear();
                i.timings.clear();
                i.bufferIds.clear();
                i.textureIds.clear();
                i.bufferBytes = i.imageBytes = i.commandTotal = 0;
                // The host calls the frame spends its time in, from here until Finish
                // (cpu_timeline.h).
                BeginCpuTimeline();
                _capturing.store(true, std::memory_order_release);
                _recordActive.store(true, std::memory_order_relaxed);
                started = true;
                overdraw = i.options.overdraw;
                pixelHistory = i.options.pixelHistory;
                maxTextureSize = i.options.maxTextureSize;
            }
        } else if (i.state == Impl::State::Capturing) {
            if (present) {
                // The present itself is part of the captured frame (a submit boundary has no such
                // command; the ExecuteCommandLists that ended the frame is already recorded).
                Submission s;
                s.present = true;
                s.objectId = Tracker::Get().IdOf(swapChain);
                s.frame = i.CurrentFrame();
                i.submissions.push_back(std::move(s));
                i.commandTotal += 1;
            }
            // The captured frames are the home boundary's: the swap chain that started a
            // present-delimited capture, or the device that started a submit-delimited one. Any
            // other present or submit is recorded but lands in the frame the home is on.
            const bool home = i.homeSwapChain ? (present && swapChain == i.homeSwapChain)
                                              : (!present && device == i.homeDevice);
            if (home) {
                i.framesDone++;
                if (i.framesDone >= i.frameCount) finish = true;
            }
        }
    }
    if (started) {
        // What the capture measures while it records (overdraw.h), with nothing left from the last one.
        StartMeasurements(overdraw, pixelHistory, maxTextureSize);
        // The pass counters start over; the heaps themselves stay.
        std::lock_guard lock(i.deviceMutex);
        for (auto& [d, dc] : i.devices) {
            dc->timestampsUsed.store(0, std::memory_order_relaxed);
            dc->statsUsed.store(0, std::memory_order_relaxed);
            dc->occlusionUsed.store(0, std::memory_order_relaxed);
            dc->slotsUsed.store(0, std::memory_order_relaxed);
        }
        Log("capture started at frame %llu (%u frame(s), %s)", (unsigned long long)deviceFrame, i.frameCount,
            present ? "present" : "submit boundary");
        return;
    }
    if (IsCapturing()) {
        if (DeviceCapture* dc = i.CaptureFor(device)) NoteQueue(*dc, queue);
    }
    if (finish) i.Finish(*this, device);
}

// ---------------------------------------------------------------------------------------------
// Finish: wait for the GPU, map, send, release

namespace {

struct CaptureData {
    uint64_t frameIndex = 0;
    uint32_t frameCount = 1;
    uint64_t commandTotal = 0;
    std::vector<Submission> submissions;
    std::vector<TextureEntry> textures;
    std::vector<BufferEntry> buffers;
    std::vector<TimingEntry> timings;
};

void WriteCommandEntry(JsonWriter& w, uint64_t index, uint32_t frame, int64_t slot, const char* method, const char* objectClass,
                       uint64_t objectId, const std::string& args, const std::string& extra) {
    w.BeginObject();
    w.Key("index"); w.Uint(index);
    w.Key("frame"); w.Uint(frame);
    // Position within the list's recording: what validation messages refer to.
    if (slot >= 0) { w.Key("slot"); w.Uint((uint64_t)slot); }
    w.Key("method"); w.String(method);
    w.Key("object");
    if (objectId) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"__id\":%llu,\"__class\":\"%s\"}", (unsigned long long)objectId, objectClass);
        w.Raw(buf);
    } else {
        w.Null();
    }
    w.Key("args"); if (args.empty()) w.Null(); else w.Raw(args);
    if (!extra.empty()) w.str() += extra;   // a pre-separated member list: ,"descriptors":{...},"stack":[...]
    w.EndObject();
}

void SendCommands(const CaptureData& data) {
    Transport& t = Transport::Get();
    uint64_t total = 0;
    for (const Submission& s : data.submissions) {
        total += 1;
        for (const SubmittedList& l : s.lists) total += l.commands ? l.commands->size() : 1;
    }
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureFrameResults");
        w.Key("frame"); w.Uint(data.frameIndex);
        w.Key("frames"); w.Uint(data.frameCount);
        w.Key("count"); w.Uint(total);
        w.Key("batches"); w.Uint((total + kCommandBatch - 1) / kCommandBatch);
        w.Key("api"); w.String("d3d12");
        w.EndObject();
        t.SendJson(std::move(w.str()));
    }
    // Flatten: the submission's entry, then each of its lists' commands, in submission order; a
    // frame's Present closes it.
    uint64_t index = 0;
    JsonWriter batch;
    size_t inBatch = 0;
    auto flush = [&]() {
        if (!inBatch) return;
        batch.EndArray();
        batch.EndObject();
        t.SendJson(std::move(batch.str()));
        batch.Reset();
        inBatch = 0;
    };
    auto emit = [&](uint32_t frame, int64_t slot, const char* method, const char* cls, uint64_t objectId, const std::string& args,
                    const std::string& extra) {
        if (!inBatch) {
            batch.BeginObject();
            batch.Key("action"); batch.String("CaptureFrameCommands");
            batch.Key("frame"); batch.Uint(data.frameIndex);
            batch.Key("index"); batch.Uint(index);
            batch.Key("commands"); batch.BeginArray();
        }
        WriteCommandEntry(batch, index++, frame, slot, method, cls, objectId, args, extra);
        if (++inBatch >= kCommandBatch) flush();
    };
    static const std::string kNone;
    for (const Submission& s : data.submissions) {
        if (s.present) {
            emit(s.frame, -1, "Present", "IDXGISwapChain", s.objectId, s.args, kNone);
            continue;
        }
        emit(s.frame, -1, "ExecuteCommandLists", "ID3D12CommandQueue", s.objectId, s.args, kNone);
        for (const SubmittedList& l : s.lists) {
            if (!l.commands) {
                emit(s.frame, -1, "<unrecorded command list>", "ID3D12GraphicsCommandList", l.listId, kNone, kNone);
                continue;
            }
            int64_t slot = 0;
            for (const RecordedCommand& c : *l.commands)
                emit(s.frame, slot++, c.method.c_str(), "ID3D12GraphicsCommandList", l.listId, c.args, c.extra);
        }
    }
    flush();
}

}  // namespace

/** The mapped staging chunk an entry's data is in, or null. */
const uint8_t* CaptureManager::Impl::MappedChunk(ID3D12Device* device, uint32_t chunk) {
    DeviceCapture* dc = FindCapture(device);
    if (!dc) return nullptr;
    std::lock_guard lock(dc->mutex);
    if (chunk >= dc->staging.size()) return nullptr;
    return static_cast<const uint8_t*>(dc->staging[chunk].mapped);
}

void CaptureManager::Impl::SendTextures(std::vector<TextureEntry>& textures) {
    Transport& t = Transport::Get();
    for (TextureEntry& e : textures) {
        if (e.frame == UINT32_MAX) {
            e.frame = 0;
            if (!e.failed) { e.failed = true; e.note = "command list was not executed during the capture"; }
        }
        if (!e.failed && !MappedChunk(e.device, e.chunk)) { e.failed = true; e.note = "staging buffer could not be mapped"; }
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureTextureFrames");
    w.Key("count"); w.Uint(textures.size());
    w.Key("textures"); w.BeginArray();
    for (const TextureEntry& e : textures) {
        w.BeginObject();
        w.Key("id"); w.Uint(e.resourceId);
        w.Key("frame"); w.Uint(e.frame);
        w.Key("commandBuffer"); w.Uint(e.listId);
        w.Key("passIndex"); w.Uint(e.passIndex);
        w.Key("attachment"); w.Uint(e.attachment);
        w.Key("format"); w.String(e.format);
        w.Key("aspect"); w.String(e.depthAspect ? "depth" : e.stencilAspect ? "stencil" : "color");
        w.Key("width"); w.Uint(e.width);
        w.Key("height"); w.Uint(e.height);
        w.Key("depth"); w.Uint(e.depth);
        w.Key("layers"); w.Uint(e.layers);
        w.Key("mip"); w.Uint(e.mip);
        if (e.mips > 1) { w.Key("mips"); w.Uint(e.mips); }
        w.Key("size"); w.Uint(e.failed ? 0 : e.size);
        if (e.samples > 1) { w.Key("samples"); w.Uint(e.samples); }
        if (e.sampled) {
            w.Key("kind"); w.String("sampled");
            w.Key("capture"); w.Uint(e.captureId);
            w.Key("baseLayer"); w.Uint(0);
        }
        if (e.failed) { w.Key("error"); w.String(e.note); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    t.SendJson(std::move(w.str()));

    std::vector<uint8_t> packed;
    for (const TextureEntry& e : textures) {
        if (e.failed) continue;
        const uint8_t* mapped = MappedChunk(e.device, e.chunk);
        if (!mapped) continue;
        // Rows come out of the staging buffer at the copy's 256-byte pitch; the UI wants them tight.
        packed.assign((size_t)e.size, 0);
        size_t at = 0;
        for (const StagedRegion& r : e.regions) {
            const uint64_t slicePitch = (uint64_t)r.rowPitch * r.rows;
            for (uint32_t s = 0; s < r.slices; ++s) {
                for (uint32_t row = 0; row < r.rows; ++row) {
                    if (at + r.rowBytes > packed.size()) break;
                    memcpy(packed.data() + at, mapped + r.offset + s * slicePitch + (uint64_t)row * r.rowPitch, (size_t)r.rowBytes);
                    at += (size_t)r.rowBytes;
                }
            }
        }
        JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureTextureData");
        h.Key("id"); h.Uint(e.resourceId);
        h.Key("frame"); h.Uint(e.frame);
        h.Key("commandBuffer"); h.Uint(e.listId);
        h.Key("passIndex"); h.Uint(e.passIndex);
        h.Key("attachment"); h.Uint(e.attachment);
        // A depth-stencil target has an entry per aspect under the same attachment index.
        h.Key("aspect"); h.String(e.depthAspect ? "depth" : e.stencilAspect ? "stencil" : "color");
        if (e.sampled) { h.Key("capture"); h.Uint(e.captureId); }
        h.Key("size"); h.Uint(e.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), packed.data(), packed.size());
    }
}

void CaptureManager::Impl::SendBuffers(std::vector<BufferEntry>& buffers) {
    Transport& t = Transport::Get();
    for (BufferEntry& e : buffers) {
        if (e.frame == UINT32_MAX) {
            e.frame = 0;
            if (!e.failed) { e.failed = true; e.note = "command list was not executed during the capture"; }
        }
        if (!e.failed && !MappedChunk(e.device, e.chunk)) { e.failed = true; e.note = "staging buffer could not be mapped"; }
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureBuffers");
    w.Key("count"); w.Uint(buffers.size());
    w.Key("buffers"); w.BeginArray();
    for (const BufferEntry& e : buffers) {
        w.BeginObject();
        w.Key("id"); w.Uint(e.id);
        w.Key("buffer"); w.Uint(e.bufferId);
        w.Key("frame"); w.Uint(e.frame);
        w.Key("commandBuffer"); w.Uint(e.listId);
        w.Key("offset"); w.Uint(e.offset);
        w.Key("size"); w.Uint(e.failed ? 0 : e.size);
        if (e.originalSize) { w.Key("originalSize"); w.Uint(e.originalSize); }
        if (e.failed) { w.Key("error"); w.String(e.note); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    t.SendJson(std::move(w.str()));
    for (const BufferEntry& e : buffers) {
        if (e.failed) continue;
        const uint8_t* mapped = MappedChunk(e.device, e.chunk);
        if (!mapped) continue;
        JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureBufferData");
        h.Key("id"); h.Uint(e.id);
        h.Key("size"); h.Uint(e.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), mapped + e.stagingOffset, (size_t)e.size);
    }
}

/**
 * A direct queue of the device, for GetClockCalibration. Any queue can calibrate, but the pass
 * timestamps were resolved on a direct one and a copy queue may run on a different clock.
 */
ID3D12CommandQueue* CaptureManager::Impl::CalibrationQueue(ID3D12Device* device) {
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard lock(deviceMutex);
        for (auto& [d, dc] : devices) captures.push_back(dc.get());
    }
    ID3D12CommandQueue* fallback = nullptr;
    for (DeviceCapture* dc : captures) {
        std::lock_guard lock(dc->mutex);
        for (ID3D12CommandQueue* q : dc->queues) {
            if (!q) continue;
            if (dc->device == device) return q;
            if (!fallback) fallback = q;
        }
    }
    return fallback;
}

void CaptureManager::Impl::SendPassTimings(const std::vector<TimingEntry>& timings, ID3D12Device* home) {
    if (timings.empty()) return;
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard lock(deviceMutex);
        for (auto& [d, dc] : devices)
            if (dc->queryMapped) captures.push_back(dc.get());
    }
    if (captures.empty()) return;
    // The period of the home device's clock; each device's passes are measured on its own clock,
    // from the earliest pass it timed.
    uint64_t frequency = 0;
    for (DeviceCapture* dc : captures)
        if (dc->device == home && dc->frequency) frequency = dc->frequency;
    if (!frequency)
        for (DeviceCapture* dc : captures)
            if (dc->frequency) { frequency = dc->frequency; break; }
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CapturePassTimings");
    w.Key("timestampPeriodNs"); w.Double(frequency ? 1e9 / (double)frequency : 0.0);
    w.Key("passes"); w.BeginArray();
    uint32_t sent = 0, counted = 0;
    // The tick every pass start is measured from, on the home device: with the clock
    // calibration (cpu_timeline.h) this is what places a pass beside the CPU events that
    // submitted it. Only the home device has a calibrated clock, so only its origin is sent.
    uint64_t originTicks = 0;
    for (DeviceCapture* dc : captures) {
        const double freq = (double)(dc->frequency ? dc->frequency : frequency);
        if (freq <= 0) continue;
        const uint8_t* results = static_cast<const uint8_t*>(dc->queryMapped);
        auto stamps = [&](const TimingEntry& te, uint64_t& begin, uint64_t& end) {
            if (te.device != dc->device || te.frame == UINT32_MAX || te.slot >= kPassSlots) return false;
            memcpy(&begin, results + te.slot * kSlotBytes, 8);
            memcpy(&end, results + te.slot * kSlotBytes + 8, 8);
            // Both zero: the list never ran (a readback heap starts zeroed and nothing resolved into the slot).
            return !(begin == 0 && end == 0) && end >= begin;
        };
        uint64_t earliest = UINT64_MAX;
        for (const TimingEntry& te : timings) {
            uint64_t b, e;
            if (stamps(te, b, e)) earliest = std::min(earliest, b);
        }
        if (dc->device == home && earliest != UINT64_MAX) originTicks = earliest;
        for (const TimingEntry& te : timings) {
            uint64_t begin, end;
            if (!stamps(te, begin, end)) continue;
            w.BeginObject();
            w.Key("frame"); w.Uint(te.frame);
            w.Key("commandBuffer"); w.Uint(te.listId);
            w.Key("passIndex"); w.Uint(te.passIndex);
            if (te.compute) { w.Key("kind"); w.String("compute"); }
            w.Key("startMs"); w.Double((double)(begin - earliest) / freq * 1e3);
            w.Key("durationMs"); w.Double((double)(end - begin) / freq * 1e3);
            if (te.hasStats || te.hasOcclusion) {
                w.Key("counters"); w.BeginObject();
                if (te.hasStats) {
                    // The Vulkan layer's names for the same quantities (pipeline_stats.cpp), which the
                    // bottleneck rules read.
                    D3D12_QUERY_DATA_PIPELINE_STATISTICS stats;
                    memcpy(&stats, results + te.slot * kSlotBytes + kStatsOffset, sizeof(stats));
                    w.Key("inputAssemblyVertices"); w.Uint(stats.IAVertices);
                    w.Key("inputAssemblyPrimitives"); w.Uint(stats.IAPrimitives);
                    w.Key("vertexInvocations"); w.Uint(stats.VSInvocations);
                    w.Key("clipperInvocations"); w.Uint(stats.CInvocations);
                    w.Key("clipperPrimitivesOut"); w.Uint(stats.CPrimitives);
                    w.Key("fragmentInvocations"); w.Uint(stats.PSInvocations);
                }
                if (te.hasOcclusion) {
                    uint64_t passed = 0;
                    memcpy(&passed, results + te.slot * kSlotBytes + kOcclusionOffset, 8);
                    w.Key("fragmentsPassed"); w.Uint(passed);
                }
                w.EndObject();
                counted++;
            }
            w.EndObject();
            sent++;
        }
    }
    w.EndArray();
    w.Key("count"); w.Uint(sent);
    if (originTicks) { w.Key("originTicks"); w.Uint(originTicks); }
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    Log("pass profiling: %u of %zu passes timed, %u with counters", sent, timings.size(), counted);
}


void CaptureManager::Impl::Finish(CaptureManager& cm, ID3D12Device* device) {
    cm._capturing.store(false, std::memory_order_release);
    cm._recordActive.store(cm.RecordAlways(), std::memory_order_relaxed);
    CaptureData data;
    {
        std::lock_guard lock(mutex);
        data.frameIndex = frameIndex;
        data.frameCount = frameCount;
        data.commandTotal = commandTotal;
        data.submissions.swap(submissions);
        data.textures.swap(textures);
        data.buffers.swap(buffers);
        data.timings.swap(timings);
        bufferIds.clear();
        textureIds.clear();
        bufferBytes = imageBytes = commandTotal = 0;
        homeSwapChain = nullptr;
        homeDevice = nullptr;   // presentSeenAny is a device-lifetime fact and is not reset here
        state = Impl::State::Idle;
    }
    Log("capture finishing: %zu submissions, %llu commands, %zu textures, %zu buffers, %zu passes", data.submissions.size(),
        (unsigned long long)data.commandTotal, data.textures.size(), data.buffers.size(), data.timings.size());

    // Everything recorded in the frames has been executed; wait for it on every queue that took
    // part, so the staging buffers and the query results are complete.
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard lock(deviceMutex);
        for (auto& [d, dc] : devices) captures.push_back(dc.get());
    }
    for (DeviceCapture* dc : captures) {
        std::vector<ID3D12CommandQueue*> queues;
        {
            std::lock_guard lock(dc->mutex);
            queues = dc->queues;
        }
        if (!dc->fence || !dc->event) continue;
        ScopedInternal internal;
        for (ID3D12CommandQueue* q : queues) {
            // A queue the application released mid-capture would be a dangling pointer here; the
            // application cannot release a queue it presented on or executed lists on this frame
            // without waiting for them, so this is not guarded against.
            const uint64_t value = ++dc->fenceValue;
            if (FAILED(q->Signal(dc->fence.get(), value))) continue;
            if (dc->fence->GetCompletedValue() >= value) continue;
            if (FAILED(dc->fence->SetEventOnCompletion(value, dc->event))) continue;
            if (WaitForSingleObject(dc->event, 10000) != WAIT_OBJECT_0)
                LogAlways("capture: the GPU did not finish within 10 s; read-backs may be incomplete");
        }
        // Map the staging chunks for the sends.
        std::lock_guard lock(dc->mutex);
        for (StagingChunk& c : dc->staging) {
            if (!c.buffer || c.mapped) continue;
            if (FAILED(c.buffer->Map(0, nullptr, &c.mapped))) c.mapped = nullptr;
        }
    }

    SendCommands(data);
    SendTextures(data.textures);
    SendBuffers(data.buffers);
    SendPassTimings(data.timings, device);
    // The GPU clock related to the host's, while the queue is still alive, then the CPU events.
    SampleCalibration(CalibrationQueue(device));
    SendCpuTimeline();
    // The measurements taken while the frames were recorded, between the timings and
    // CaptureComplete (docs/ARCHITECTURE.md, "Frame capture").
    SendOverdraw();
    SendPixelHistory();
    // Read-backs that failed for a reason other than the capture's own limits are worth a line
    // in the validation view, where the user looks for what went wrong.
    {
        uint32_t failed = 0;
        std::string first;
        auto count = [&](bool isFailed, const std::string& note) {
            if (!isFailed || note.find("budget") != std::string::npos || note.find("max ") != std::string::npos) return;
            if (!failed) first = note;
            failed++;
        };
        for (const TextureEntry& e : data.textures) count(e.failed, e.note);
        for (const BufferEntry& e : data.buffers) count(e.failed, e.note);
        if (failed) ValidationLog::Get().Note("capture: " + std::to_string(failed) + " read-back(s) failed: " + first);
    }
    {
        // The end of the capture's stream, whichever sections it had.
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureComplete");
        w.Key("frame"); w.Uint(data.frameIndex);
        w.Key("frames"); w.Uint(data.frameCount);
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
    }

    for (DeviceCapture* dc : captures) ReleaseCaptureObjects(*dc);
    if (!cm.RecordAlways()) {
        std::unique_lock lock(recorderMutex);
        recorders.clear();
    }
    Log("capture sent");
}

}  // namespace dxinsp
