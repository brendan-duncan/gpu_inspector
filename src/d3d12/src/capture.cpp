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
#include "frame_pause.h"
#include "hooks.h"
#include "json.h"
#include "overdraw.h"
#include "raytracing.h"
#include "resources.h"
#include "tracker.h"
#include "transport.h"
#include "validation.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>

namespace dxinsp
{

namespace
{

constexpr uint32_t kTimestampQueries = 32768;
constexpr uint32_t kStatsQueries = 8192;
constexpr uint32_t kOcclusionQueries = 8192;
constexpr uint32_t kPassSlots = 8192;
// One pass's resolved queries in the readback buffer: two timestamps, the statistics, the occlusion count.
constexpr uint64_t kStatsOffset = 16;
constexpr uint64_t kOcclusionOffset = kStatsOffset + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
constexpr uint64_t kSlotBytes = kOcclusionOffset + 8;

/**
 * Draws and dispatches measured one by one (CaptureOptions::drawTimings, **Measure draws**), per
 * capture and per device. Their queries sit in heaps of their own, so a pass's slots and a draw's
 * cannot run into each other; a frame with more draws than this is measured up to here, and the
 * message it is sent in says so.
 *
 * Each kind of result has a region of the readback buffer to itself, tightly packed, so a run of
 * consecutive slots resolves in one call: ResolveQueryData writes its results consecutively, which
 * a per-draw slot stride could not hold.
 */
constexpr uint32_t kDrawSlots = 16384;
constexpr uint64_t kDrawTimestampBytes = 16;   // the pair around the draw
constexpr uint64_t kDrawStatsBytes = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
constexpr uint64_t kDrawOcclusionBytes = 8;
constexpr uint64_t kDrawTimestampBase = 0;
constexpr uint64_t kDrawStatsBase = kDrawTimestampBase + kDrawTimestampBytes * kDrawSlots;
constexpr uint64_t kDrawOcclusionBase = kDrawStatsBase + kDrawStatsBytes * kDrawSlots;
constexpr uint64_t kDrawReadbackBytes = kDrawOcclusionBase + kDrawOcclusionBytes * kDrawSlots;

/** What BeginDrawQueries returns: the slot, and which of the two optional queries it began. */
constexpr uint32_t kDrawSlotMask = 0x3fffffffu;
constexpr uint32_t kDrawStatsBit = 0x80000000u;
constexpr uint32_t kDrawOcclusionBit = 0x40000000u;
constexpr uint64_t kStagingChunkBytes = 64ull << 20;
constexpr uint64_t kStagingAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;   // 512: what a placed footprint needs
constexpr uint32_t kMaxAttachmentSlices = 16;
constexpr size_t kCommandBatch = 500;

inline uint64_t Align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

/** Whether a copy from a resource in `state` needs a transition: COMMON promotes, and any state with the COPY_SOURCE bit will do. */
inline bool NeedsCopyBarrier(D3D12_RESOURCE_STATES state)
{
    return state != D3D12_RESOURCE_STATE_COMMON && !(state & D3D12_RESOURCE_STATE_COPY_SOURCE);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t subresource,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

/**
 * The description of a resource the tracker still holds, false for any other pointer.
 *
 * Only the tracker may say a resource pointer is worth dereferencing. A descriptor keeps no
 * reference to what it names, and an engine that recycles resources (Unity does, constantly)
 * leaves heap slots and bindings naming released ones, which is legal and which the application
 * never reads. Asking such a pointer for its description crashes the application in our frame.
 * Every creation path registers with the tracker and Release removes it (hooks_device.cpp,
 * resources.h), so a resource it does not have is one that is gone.
 */
bool DescOf(ID3D12Resource* resource, D3D12_RESOURCE_DESC& desc, ResourceInfo* info = nullptr)
{
    ResourceInfo local;
    if (resource && ResourceTracker::Get().Get(resource, local))
    {
        desc = local.desc;
        // The tracker keeps the creation description, where MipLevels 0 asked for a full chain;
        // the read-back needs the count the runtime chose.
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && desc.MipLevels == 0 && resource)
        {
            ScopedInternal internal;
            desc = resource->GetDesc();
            local.desc = desc;
        }
        if (info)
            *info = local;
        return true;
    }
    return false;
}

/** The protocol format a target or texture of `format` is sent under, and whether it travels as the depth aspect. */
struct ProtocolFormat
{
    const char* name = nullptr;
    bool depth = false;
    uint32_t texelBytes = 0;     // bytes of one texel (or block) in the tightly packed data
    uint32_t blockWidth = 1;
    uint32_t blockHeight = 1;
};

ProtocolFormat ProtocolFormatOf(DXGI_FORMAT format, bool asDepth)
{
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
    if (p.depth)
        p.texelBytes = typed == DXGI_FORMAT_D16_UNORM ? 2 : 4;
    else
        p.texelBytes = info.bytes;
    return p;
}

uint64_t TightRowBytes(const ProtocolFormat& p, uint32_t width)
{
    return (uint64_t)((width + p.blockWidth - 1) / p.blockWidth) * p.texelBytes;
}
uint32_t TightRows(const ProtocolFormat& p, uint32_t height)
{
    return (height + p.blockHeight - 1) / p.blockHeight;
}

uint32_t MipDim(uint64_t dim, uint32_t mip)
{
    const uint64_t v = dim >> mip;
    return v ? (uint32_t)v : 1u;
}

// ---------------------------------------------------------------------------------------------
// The command in flight on this thread (validation.h attaches messages to it)

struct ScopeState
{
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

CommandScope::CommandScope(CommandRecorder* rec)
{
    if (t_scopeDepth < kMaxScopeDepth)
        t_scopes[t_scopeDepth] = {rec, rec ? (uint32_t)rec->commandCount() : 0};
    t_scopeDepth++;
}

CommandScope::~CommandScope()
{
    if (t_scopeDepth)
        t_scopeDepth--;
}

bool CommandScope::Current(uint64_t& listId, uint32_t& slot)
{
    if (!t_scopeDepth || t_scopeDepth > kMaxScopeDepth)
        return false;
    const ScopeState& s = t_scopes[t_scopeDepth - 1];
    if (!s.rec)
        return false;
    listId = Tracker::Get().IdOf(s.rec->list());
    slot = s.slot;
    return listId != 0;
}

// ---------------------------------------------------------------------------------------------
// What a capture holds

namespace
{

struct StagingChunk
{
    ComPtr<ID3D12Resource> buffer;
    uint64_t size = 0;
    uint64_t used = 0;
    void* mapped = nullptr;
    /**
     * The kind of list whose copies write it. A chunk is only ever written by one kind of queue: the
     * debug layer tracks writes per resource, not per range, and a staging buffer written from a
     * direct and a compute queue at once -- an application with async compute -- is reported as
     * written on two queues in flight, disjoint ranges or not. Two queues of one kind can still
     * share a chunk.
     */
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

/** A staged copy of one subresource: where its rows are in the chunk and how to pack them tightly. */
struct StagedRegion
{
    uint64_t offset = 0;      // from the chunk's start
    uint32_t rowPitch = 0;    // as copied (256-aligned)
    uint32_t rows = 0;        // rows (block rows) per slice
    uint64_t rowBytes = 0;    // tight bytes per row
    uint32_t slices = 1;      // depth slices of a 3D subresource, each rows x rowPitch
};

struct TextureEntry
{
    uint64_t resourceId = 0;
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;   // whose execution gives the frame
    /**
     * The other lists that asked for the same contents and were given this entry. Any of them
     * running in the capture gives it its frame: the list that queued it may have run the frame
     * before (the frame of recording ahead of the capture), and its copy is as good for the list
     * that runs now -- what two lists share is a mesh or a texture, not a frame's constants.
     */
    std::vector<ID3D12GraphicsCommandList*> sharedBy;
    /** Queued in the frame of recording before the capture: not part of the capture unless a list of the captured frame runs it, and not sent otherwise. */
    bool warmup = false;
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
    /** What the frame found in it, taken before the first submission that reads it (kind `initial`). */
    bool initial = false;
    uint32_t captureId = 0;
    bool failed = false;
    std::string note;
    ID3D12Device* device = nullptr;
    uint32_t chunk = 0;
    std::vector<StagedRegion> regions;   // in the order of the tight data
};

struct BufferEntry
{
    uint32_t id = 0;
    uint64_t bufferId = 0;
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;
    /**
     * The other lists that asked for the same contents and were given this entry. Any of them
     * running in the capture gives it its frame: the list that queued it may have run the frame
     * before (the frame of recording ahead of the capture), and its copy is as good for the list
     * that runs now -- what two lists share is a mesh or a texture, not a frame's constants.
     */
    std::vector<ID3D12GraphicsCommandList*> sharedBy;
    /** Queued in the frame of recording before the capture: not part of the capture unless a list of the captured frame runs it, and not sent otherwise. */
    bool warmup = false;
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

/**
 * A list is recorded again: what its last recording queued and never ran is no longer its own. The
 * frame before the capture leaves such entries behind, and a list taken from a pool would
 * otherwise give them the frame of a recording they do not belong to.
 */
template <typename Entry>
void OrphanEntries(std::vector<Entry>& entries, ID3D12GraphicsCommandList* list)
{
    for (Entry& e : entries)
    {
        if (e.frame != UINT32_MAX)
            continue;
        if (e.list == list)
            e.list = nullptr;
        e.sharedBy.erase(std::remove(e.sharedBy.begin(), e.sharedBy.end(), list), e.sharedBy.end());
    }
}

/**
 * A list is recorded again, so its last recording's measurements are gone with it: the queries were
 * in the commands the Reset dropped, and that recording can never run in the capture now. An entry
 * that already has a frame ran before the Reset and stays.
 */
template <typename Entry>
void DropUnrunMeasurements(std::vector<Entry>& entries, ID3D12GraphicsCommandList* list)
{
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                      [&](const Entry& e) { return e.list == list && e.frame == UINT32_MAX; }),
        entries.end());
}

/** An entry answered to another list than the one that queued it (Entry::sharedBy). */
template <typename Entry>
void ShareEntry(Entry& e, ID3D12GraphicsCommandList* list)
{
    if (e.list != list && e.frame == UINT32_MAX && (e.sharedBy.empty() || e.sharedBy.back() != list))
        e.sharedBy.push_back(list);
}

struct TimingEntry
{
    ID3D12Device* device = nullptr;
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;
    /**
     * Queried in the frame of recording before the capture (the queries go into the list as it is
     * recorded, and an engine that records ahead built the captured frame's lists then). Not part
     * of the capture unless that list runs in it, which is what gives the entry its frame; one
     * that never does keeps UINT32_MAX and is not sent (SendPassTimings skips it).
     */
    bool warmup = false;
    uint64_t listId = 0;
    uint32_t passIndex = 0;
    bool compute = false;
    uint32_t slot = 0;
    bool hasStats = false;
    bool hasOcclusion = false;
    /** The queries to resolve for this pass at the finish (UINT32_MAX: none of that kind). */
    uint32_t statsQuery = UINT32_MAX;
    uint32_t occlusionQuery = UINT32_MAX;
};

/**
 * One draw or dispatch measured by queries of its own (CaptureOptions::drawTimings). The command
 * index is the draw's in the capture's own command list, which is what the measurements are keyed
 * by in the UI (draw_stats.ts).
 */
struct DrawEntry
{
    ID3D12Device* device = nullptr;
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;
    /** Measured in the warm-up frame, and the capture's only if that list runs in it (TimingEntry::warmup). */
    bool warmup = false;
    uint64_t listId = 0;
    uint32_t command = 0;
    uint32_t passIndex = UINT32_MAX;   // a dispatch outside a render pass has none
    bool dispatch = false;
    uint32_t slot = 0;
    bool hasStats = false;
    bool hasOcclusion = false;
};

struct SubmittedList
{
    uint64_t listId = 0;
    std::shared_ptr<const CommandList> commands;   // null: recorded before the capture
};

/** An ExecuteCommandLists, or the Present that ended a frame, in the order they happened. */
struct Submission
{
    bool present = false;
    uint64_t objectId = 0;      // the queue, or the swap chain
    uint32_t frame = 0;
    std::string args;
    /** ExecuteCommandLists: the heaps its lists index directly (CaptureManager::IndexedHeapContents). */
    std::string extra;
    std::vector<SubmittedList> lists;
};

struct ResolveKey
{
    ID3D12GraphicsCommandList* list;
    DXGI_FORMAT format;
    uint32_t width, height, layers;
    bool operator<(const ResolveKey& o) const
    {
        return std::tie(list, format, width, height, layers) < std::tie(o.list, o.format, o.width, o.height, o.layers);
    }
};

/** A capture's objects on one device: query heaps, the slots their results resolve into, staging, resolves, the fence. */
struct DeviceCapture
{
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
    /**
     * The per-draw queries (drawTimings), made on the first draw of the first capture that asks
     * for them rather than with the device: most captures never measure draws, and these heaps and
     * their readback are larger than the pass ones.
     */
    ComPtr<ID3D12QueryHeap> drawTimestampHeap;
    ComPtr<ID3D12QueryHeap> drawStatsHeap;
    ComPtr<ID3D12QueryHeap> drawOcclusionHeap;
    std::atomic<uint32_t> drawSlotsUsed{0};
    ComPtr<ID3D12Resource> drawReadback;
    void* drawMapped = nullptr;
    std::atomic<bool> drawQueriesMade{false};
    std::vector<StagingChunk> staging;
    std::map<ResolveKey, ComPtr<ID3D12Resource>> resolves;
    /**
     * The staging buffers and resolve textures of captures already finished, held until the next
     * capture starts. A command list does not keep a resource it names alive, and an engine that
     * records several frames ahead (Unity does) always has lists open that the capture recorded
     * copies into; freeing the destination while such a list is still being recorded makes its
     * Close return E_FAIL, which the application takes for a lost device. By the time another
     * capture begins those lists have been closed, executed and reset many times over.
     */
    std::vector<StagingChunk> retiredStaging;
    std::vector<ComPtr<ID3D12Resource>> retiredResolves;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t fenceValue = 0;
    std::vector<ID3D12CommandQueue*> queues;   // executed lists during the capture (not AddRef'd)
    /** The capture's own lists (RecorderSlot::afterSubmit), alive until the capture's GPU work is waited for. */
    std::vector<ComPtr<ID3D12CommandAllocator>> ownAllocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> ownLists;
    uint64_t frequency = 0;                    // ticks per second of the first direct queue seen

    ~DeviceCapture()
    {
        if (queryMapped && queryReadback)
        {
            ScopedInternal internal;
            queryReadback->Unmap(0, nullptr);
        }
        if (event)
            CloseHandle(event);
    }
};

/** A read-back copy held back, recorded later into the list it is given (the primary, for a bundle's). */
using DeferredCopy = std::function<void(ID3D12GraphicsCommandList*)>;

struct RecorderSlot
{
    std::unique_ptr<CommandRecorder> rec;
    /** Copies queued inside a BeginRenderPass region, recorded when it ends (a copy may not interrupt a render pass). */
    std::vector<DeferredCopy> deferred;
    /**
     * Copies queued inside a suspended pass (ActivePass::split), which this list has no place for
     * at all: they go into a list of the capture's own, executed after the submission this list is
     * in (OnExecuteCommandLists). What they read is what a pass reads and does not write -- vertex,
     * index and constant buffers, sampled textures -- so it still holds what the draws saw. An
     * engine that records a pass across the lists of its jobs draws its whole scene this way
     * (Unity's URP does), and without these the capture had none of its meshes or textures.
     */
    std::vector<DeferredCopy> afterSubmit;
};

}  // namespace

// How a device's frames are delimited, decided per device over its lifetime (a process can hold
// several D3D12 devices: a game and a background copy device, or, in Chrome's GPU process, Dawn's
// WebGPU device beside the compositor's). A device that presents ends its frames at the present;
// one that goes a long run of submissions without ever presenting ends them at every
// ExecuteCommandLists, the way the Vulkan layer falls back for an OpenXR application that never
// presents. DXINSP_FRAME_BOUNDARY forces one or the other.
struct DeviceFrame
{
    enum class Boundary
    {
        Auto,
        Present,
        Submit
    };
    Boundary boundary = Boundary::Auto;
    bool presentSeen = false;
    uint64_t frameIndex = 0;             // this device's own frame count (its presents, or its submit boundaries)
    uint32_t submitsWithoutPresent = 0;
    ID3D12CommandQueue* lastQueue = nullptr;   // not AddRef'd; the queue a substitute boundary ran on
};

// A device is taken to have no swap chain after this many submissions without a present, the same
// threshold the Vulkan layer uses (layer.cpp, OnSubmitForFrames).
constexpr uint32_t kSubmitsWithoutPresent = 60;

enum class BoundaryOverride
{
    Auto,
    Present,
    Submit
};

struct CaptureManager::Impl
{
    enum class State
    {
        Idle,
        Armed,
        Capturing
    };

    std::mutex mutex;   // the state machine, the options and the capture's entries
    State state = State::Idle;
    CaptureOptions options;
    uint64_t frameIndex = 0;
    uint32_t frameCount = 1;
    /**
     * Frame boundaries an armed capture lets pass before it starts, so that the frame it captures
     * has had its command lists recorded from the beginning. An engine records a frame's lists
     * during the frame before it — Unity's renderer does, on worker threads — and recording starts
     * when the capture is asked for, part way through a frame; without the wait, the lists of the
     * frame being captured were half recorded before anything was watching.
     */
    uint32_t warmupBoundaries = 0;
    /**
     * Whether a list recording now has the contents it reads taken (buffers and sampled textures;
     * under `mutex`). During the capture, and during the frame of recording before it: the lists an
     * engine records a frame ahead are the captured frame's, and with their commands alone the
     * capture had every draw of a Unity scene and none of its meshes. What such a list queued is
     * kept when the capture starts; what the lists of the frame before queued never gets a frame,
     * and is sent as not executed.
     */
    bool warmingUp = false;
    bool TakesContents() const { return state == State::Capturing || (state == State::Armed && warmingUp); }
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
    BoundaryOverride Override()
    {
        std::lock_guard lock(frameMutex);
        if (!boundaryOverrideRead)
        {
            boundaryOverrideRead = true;
            const std::string v = ConfigValue("DXINSP_FRAME_BOUNDARY");
            if (v == "submit")
                boundaryOverride = BoundaryOverride::Submit;
            else if (v == "present")
                boundaryOverride = BoundaryOverride::Present;
        }
        return boundaryOverride;
    }
    std::vector<Submission> submissions;
    std::vector<TextureEntry> textures;
    std::vector<BufferEntry> buffers;
    std::vector<TimingEntry> timings;
    std::vector<DrawEntry> draws;
    /** The textures read back as the frame found them (kind `initial`), and those a submission of the capture wrote: BeforeExecuteCommandLists. */
    std::unordered_map<ID3D12Resource*, uint32_t> initialIds;
    std::unordered_set<ID3D12Resource*> frameWritten;
    /** Per directly indexed heap, the write of each slot the capture last sent (IndexedHeapContents). */
    std::mutex heapSeenMutex;
    std::unordered_map<ID3D12DescriptorHeap*, std::vector<uint64_t>> heapSeen;
    /**
     * The draw slots each open list has taken and not resolved yet, packed as BeginDrawQueries
     * returned them. A resolve is only allowed outside a BeginRenderPass region, so it happens at
     * the end of a pass and before Close rather than at the draw.
     */
    std::unordered_map<ID3D12GraphicsCommandList*, std::vector<uint32_t>> pendingDrawSlots;
    uint64_t bufferBytes = 0;
    uint64_t imageBytes = 0;
    /** Render-target bytes read back in this capture, against CaptureOptions::maxTargetTotal. */
    uint64_t targetBytes = 0;
    /** Pass segments the capture added nothing to because they were suspended (ActivePass::split). */
    uint32_t splitPasses = 0;
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
    /** Room in a staging chunk for a copy recorded into a list of `type` (StagingChunk::type). */
    bool AllocateStaging(DeviceCapture& dc, D3D12_COMMAND_LIST_TYPE type, uint64_t size, uint32_t& chunk, uint64_t& offset,
        ID3D12Resource** buffer);
    ID3D12Resource* ResolveTextureFor(DeviceCapture& dc, const ResolveKey& key);
    std::vector<DeferredCopy>* DeferredOf(CommandRecorder* rec);
    /** Where a copy queued now goes when the list cannot take it here: the pass's end, or after the submission (null: into the list). */
    std::vector<DeferredCopy>* HeldCopiesOf(CommandRecorder* rec);
    /** Records and executes the copies the submitted lists held for after it (RecorderSlot::afterSubmit). */
    void RunAfterSubmitCopies(ID3D12Device* device, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    /**
     * Resolves the captured passes' queries into the readback buffer, from a list of the capture's
     * own, once the frame's work has been waited for. Nothing of the capture's resolves inside the
     * application's lists: a `ResolveQueryData` there is what a suspended render pass forbids, and
     * doing it here is what lets such a pass be timed at all (EndPassQueries).
     */
    void ResolveQueries(const std::vector<TimingEntry>& timings);
    /**
     * The traces' local root arguments (raytracing.h, ResolveLocalRootArguments), once the binding
     * tables' read-backs are mapped: the extras their commands carry, and the root views' buffer
     * ranges read back now, in a list of the capture's own, into buffer entries of their own.
     */
    void ResolveLocalRoots(std::vector<BufferEntry>& buffers, std::unordered_map<std::string, std::string>& lateExtras);
    uint32_t CurrentFrame();   // frame ordinal of what is being recorded now (under mutex)
    void ReleaseCaptureObjects(DeviceCapture& dc);
    /** The mapped staging chunk an entry's data is in, or null. */
    const uint8_t* MappedChunk(ID3D12Device* device, uint32_t chunk);
    void SendTextures(std::vector<TextureEntry>& textures);
    void SendBuffers(std::vector<BufferEntry>& buffers);
    void SendPassTimings(const std::vector<TimingEntry>& timings, ID3D12Device* home);
    void SendDrawStats(const std::vector<DrawEntry>& draws, ID3D12Device* home);
    ID3D12CommandQueue* CalibrationQueue(ID3D12Device* device);
    /** The last captured frame ended: waits for the GPU, streams everything, releases the capture's objects. */
    void Finish(CaptureManager& cm, ID3D12Device* device);
};

namespace
{

bool CreateReadbackBuffer(ID3D12Device* device, uint64_t size, ID3D12Resource** out)
{
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
    if (FAILED(hr))
    {
        LogAlways("capture: readback buffer of %llu bytes failed (%s)", (unsigned long long)size, HrText(hr).c_str());
        return false;
    }
    return true;
}

bool CreateQueryHeap(ID3D12Device* device, D3D12_QUERY_HEAP_TYPE type, uint32_t count, ID3D12QueryHeap** out)
{
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = type;
    desc.Count = count;
    ScopedInternal internal;
    HRESULT hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(out));
    if (FAILED(hr))
        Log("capture: query heap type %d failed (%s)", (int)type, HrText(hr).c_str());
    return SUCCEEDED(hr);
}

}  // namespace

DeviceCapture* CaptureManager::Impl::FindCapture(ID3D12Device* device)
{
    std::lock_guard lock(deviceMutex);
    auto it = devices.find(device);
    return it == devices.end() ? nullptr : it->second.get();
}

DeviceCapture* CaptureManager::Impl::CaptureFor(ID3D12Device* device)
{
    if (!device)
        return nullptr;
    std::lock_guard lock(deviceMutex);
    auto it = devices.find(device);
    if (it != devices.end())
        return it->second.get();
    auto dc = std::make_unique<DeviceCapture>();
    dc->device = device;
    // The query heaps and their readback buffer live as long as the device: a capture's passes
    // reserve slots from them, and the counters start over with every capture.
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimestampQueries, dc->timestampHeap.put());
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, kStatsQueries, dc->statsHeap.put());
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_OCCLUSION, kOcclusionQueries, dc->occlusionHeap.put());
    if (CreateReadbackBuffer(device, kSlotBytes * kPassSlots, dc->queryReadback.put()))
    {
        // Kept mapped: a readback buffer may stay mapped while the GPU writes it, and the results
        // are only read once the fence says the last list has run.
        ScopedInternal internal;
        D3D12_RANGE none{0, 0};
        if (FAILED(dc->queryReadback->Map(0, &none, &dc->queryMapped)))
            dc->queryMapped = nullptr;
    }
    {
        ScopedInternal internal;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(dc->fence.put()))))
            dc->fence.reset();
    }
    dc->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DeviceCapture* raw = dc.get();
    devices[device] = std::move(dc);
    Log("capture: device %p takes part", (void*)device);
    return raw;
}

bool CaptureManager::Impl::AllocateStaging(DeviceCapture& dc, D3D12_COMMAND_LIST_TYPE type, uint64_t size, uint32_t& chunk,
    uint64_t& offset, ID3D12Resource** buffer)
{
    // A bundle's copies are recorded into the list that executes it, which is a direct one.
    if (type == D3D12_COMMAND_LIST_TYPE_BUNDLE)
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    std::lock_guard lock(dc.mutex);
    size = Align(std::max<uint64_t>(size, 1), kStagingAlignment);
    // The newest chunk of this kind, which is the only one of its kind with room left.
    uint32_t index = UINT32_MAX;
    for (size_t k = dc.staging.size(); k-- > 0;)
    {
        if (dc.staging[k].type == type)
        {
            index = (uint32_t)k;
            break;
        }
    }
    if (index == UINT32_MAX || dc.staging[index].used + size > dc.staging[index].size)
    {
        StagingChunk c;
        c.size = std::max(size, kStagingChunkBytes);
        c.type = type;
        if (!CreateReadbackBuffer(dc.device, c.size, c.buffer.put()))
            return false;
        {
            // Named so that what the debug layer says about it is told apart from the application's.
            ScopedInternal internal;
            c.buffer->SetName(L"GPU Inspector: read-back staging");
        }
        dc.staging.push_back(std::move(c));
        index = (uint32_t)dc.staging.size() - 1;
    }
    StagingChunk& c = dc.staging[index];
    chunk = index;
    offset = c.used;
    c.used += size;
    if (buffer)
        *buffer = c.buffer.get();
    return true;
}

ID3D12Resource* CaptureManager::Impl::ResolveTextureFor(DeviceCapture& dc, const ResolveKey& key)
{
    std::lock_guard lock(dc.mutex);
    auto it = dc.resolves.find(key);
    if (it != dc.resolves.end())
        return it->second.get();
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
        if (FAILED(hr))
        {
            Log("capture: resolve texture %ux%u %s failed (%s)", key.width, key.height, FormatName(key.format), HrText(hr).c_str());
            return nullptr;
        }
    }
    ID3D12Resource* raw = texture.get();
    dc.resolves[key] = std::move(texture);
    return raw;
}

std::vector<DeferredCopy>* CaptureManager::Impl::DeferredOf(CommandRecorder* rec)
{
    std::shared_lock lock(recorderMutex);
    auto it = recorders.find(rec->list());
    if (it == recorders.end() || it->second.rec.get() != rec)
        return nullptr;
    // The vector stays where it is until the list's entry goes, which only happens when the
    // application releases the list (nobody records into it then) or at Finish.
    return &it->second.deferred;
}

uint32_t CaptureManager::Impl::CurrentFrame()
{
    return framesDone;
}

void CaptureManager::Impl::ReleaseCaptureObjects(DeviceCapture& dc)
{
    std::lock_guard lock(dc.mutex);
    ScopedInternal internal;
    for (StagingChunk& c : dc.staging)
    {
        if (c.mapped && c.buffer)
            c.buffer->Unmap(0, nullptr);
        c.mapped = nullptr;
    }
    // Unmapped and out of use here, but not freed: see DeviceCapture::retiredStaging.
    for (StagingChunk& c : dc.staging)
        dc.retiredStaging.push_back(std::move(c));
    for (auto& [key, texture] : dc.resolves)
        dc.retiredResolves.push_back(std::move(texture));
    dc.staging.clear();
    dc.resolves.clear();
    dc.queues.clear();
    dc.ownLists.clear();
    dc.ownAllocators.clear();
}

// ---------------------------------------------------------------------------------------------
// The manager

CaptureManager& CaptureManager::Get()
{
    static CaptureManager* instance = new CaptureManager();
    return *instance;
}

CaptureManager::Impl& CaptureManager::impl()
{
    if (!_impl)
        _impl = new Impl();
    return *_impl;
}

void CaptureManager::RequestCapture(const CaptureOptions& options)
{
    ForgetPendingTraces();
    Impl& i = impl();
    {
        // A new capture sends each directly indexed heap whole again, on its first submission.
        std::lock_guard seen(i.heapSeenMutex);
        i.heapSeen.clear();
    }
    std::lock_guard lock(i.mutex);
    if (i.state == Impl::State::Capturing)
    {
        Log("capture: request ignored, a capture is in progress");
        return;
    }
    i.options = options;
    i.frameCount = std::max(1u, options.frameCount);
    i.state = Impl::State::Armed;
    // Recording starts with the request, not with the frame boundary that starts the capture, and
    // the capture then lets one boundary pass before it begins. An engine that records a frame's
    // command lists during the frame before it — Unity's renderer does, on worker threads — would
    // otherwise submit lists whose commands were never recorded, and the whole capture would read
    // "<unrecorded command list>"; waiting a frame means the captured frame's lists were recorded
    // from the moment they were reset. It costs the application one frame of recording, and the
    // capture arrives a frame later than it otherwise would.
    //
    // A capture queued at a later frame does not wait, which would miss the frame it was asked for:
    // it starts recording a frame ahead of its target instead (OnFrameBoundary).
    i.warmupBoundaries = 0;
    i.warmingUp = false;
    i.textures.clear();
    i.buffers.clear();
    i.timings.clear();
    i.draws.clear();
    i.pendingDrawSlots.clear();
    i.bufferIds.clear();
    i.textureIds.clear();
    i.initialIds.clear();
    i.frameWritten.clear();
    i.bufferBytes = i.imageBytes = 0;
    // The query counters start over here rather than when the capture starts: the warm-up frame
    // records passes of the frame to be captured and reserves their slots (BeginPass), and starting
    // over after that would hand the same slots out twice and overwrite what it measured.
    {
        std::lock_guard devices(i.deviceMutex);
        for (auto& [d, dc] : i.devices)
        {
            dc->timestampsUsed.store(0, std::memory_order_relaxed);
            dc->statsUsed.store(0, std::memory_order_relaxed);
            dc->occlusionUsed.store(0, std::memory_order_relaxed);
            dc->slotsUsed.store(0, std::memory_order_relaxed);
            dc->drawSlotsUsed.store(0, std::memory_order_relaxed);
        }
    }
    if (options.atFrame == UINT64_MAX)
    {
        _recordActive.store(true, std::memory_order_relaxed);
        i.warmupBoundaries = 1;
        i.warmingUp = true;
    }
    Log("capture armed: %u frame(s)%s", i.frameCount, options.atFrame == UINT64_MAX ? "" : " at a given frame");
}

bool CaptureManager::RecordAlways() const
{
    Impl& i = const_cast<CaptureManager*>(this)->impl();
    std::lock_guard lock(i.mutex);
    if (!i.recordAlwaysRead)
    {
        i.recordAlwaysRead = true;
        i.recordAlways = ConfigFlag("DXINSP_RECORD_ALWAYS");
        if (i.recordAlways)
            const_cast<CaptureManager*>(this)->_recordActive.store(true, std::memory_order_relaxed);
    }
    return i.recordAlways;
}

void CaptureManager::SetRecordAlways(bool on)
{
    Impl& i = impl();
    RecordAlways();   // the environment's setting is read before the UI's replaces it
    std::lock_guard lock(i.mutex);
    i.recordAlways = on;
    _recordActive.store(on || _capturing.load(std::memory_order_acquire), std::memory_order_relaxed);
    Log("record always: %s", on ? "on" : "off");
}

CommandRecorder* CaptureManager::LookupRecorder(ID3D12GraphicsCommandList* list)
{
    if (!_recorderCount.load(std::memory_order_relaxed))
        return nullptr;   // the common case: nothing recorded
    Impl& i = impl();
    std::shared_lock lock(i.recorderMutex);
    auto it = i.recorders.find(list);
    return it == i.recorders.end() ? nullptr : it->second.rec.get();
}

CommandRecorder* CaptureManager::Adopt(ID3D12GraphicsCommandList* list)
{
    if (!list)
        return nullptr;
    Impl& i = impl();
    bool stacks;
    {
        std::lock_guard lock(i.mutex);
        stacks = i.options.stacktraces && i.state != Impl::State::Idle;
    }
    ID3D12Device* device = DeviceOf(list);
    if (!device)
        return nullptr;
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    std::unique_lock lock(i.recorderMutex);
    RecorderSlot& slot = i.recorders[list];
    if (!slot.rec)
    {
        slot.rec = std::make_unique<CommandRecorder>(device, list, type, type == D3D12_COMMAND_LIST_TYPE_BUNDLE);
        slot.rec->MarkAdopted();
        slot.rec->SetCaptureStacks(stacks);
        slot.rec->Record("Reset", "{\"pAllocator\":null,\"pInitialState\":null,\"adopted\":true}");
        _recorderCount.store(i.recorders.size(), std::memory_order_relaxed);
    }
    return slot.rec.get();
}

void CaptureManager::OnListReset(ID3D12Device* device, ID3D12GraphicsCommandList* list, D3D12_COMMAND_LIST_TYPE type, bool bundle,
    ID3D12PipelineState* initialState)
{
    // DXINSP_RECORD_ALWAYS is read here rather than at the first capture: a bundle an engine
    // records at start-up needs its recorder before anything asks for a capture.
    RecordAlways();
    if (!list)
        return;
    OnMeasuredListReset(list);   // nothing kept of the list is still in effect
    if (!ShouldRecord())
        return;
    Impl& i = impl();
    {
        std::lock_guard lock(i.mutex);
        if (i.TakesContents())
        {
            OrphanEntries(i.textures, list);
            OrphanEntries(i.buffers, list);
            // The timings and draw measurements of the recording this Reset replaced: unlike
            // contents, which another list may still be given, a measurement belongs to the
            // commands it was recorded among, and those are gone.
            DropUnrunMeasurements(i.timings, list);
            DropUnrunMeasurements(i.draws, list);
        }
    }
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
        if (!slot.rec)
            slot.rec = std::make_unique<CommandRecorder>(device, list, type, bundle);
        else
            slot.rec->Reset();
        slot.deferred.clear();
        slot.afterSubmit.clear();
        rec = slot.rec.get();
        _recorderCount.store(i.recorders.size(), std::memory_order_relaxed);
    }

    rec->SetCaptureStacks(stacks);
    if (initialState)
        rec->state().pipeline = initialState;
}

void CaptureManager::OnListReleased(ID3D12GraphicsCommandList* list)
{
    OnMeasuredListReleased(list);
    if (!_impl)
        return;
    Impl& i = impl();
    std::unique_lock lock(i.recorderMutex);
    i.recorders.erase(list);
    _recorderCount.store(i.recorders.size(), std::memory_order_relaxed);
}

ID3D12CommandQueue* CaptureManager::PresentQueue(IDXGISwapChain* swapChain)
{
    Impl& i = impl();
    std::lock_guard lock(i.swapChainMutex);
    auto it = i.swapChains.find(swapChain);
    return it == i.swapChains.end() ? nullptr : it->second;
}

void CaptureManager::OnSwapChainCreated(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    if (!swapChain)
        return;
    Impl& i = impl();
    std::lock_guard lock(i.swapChainMutex);
    i.swapChains[swapChain] = queue;
}

void CaptureManager::OnSwapChainReleased(IDXGISwapChain* swapChain)
{
    if (!_impl)
        return;
    Impl& i = impl();
    {
        std::lock_guard lock(i.swapChainMutex);
        i.swapChains.erase(swapChain);
    }
    ID3D12Device* finishOn = nullptr;
    {
        std::lock_guard lock(i.mutex);
        if (i.homeSwapChain == swapChain)
        {
            // The swap chain whose presents delimit the capture is going away. Mid-capture that
            // would leave nothing to end the remaining frames on (the home device presents, so its
            // submits are not boundaries), so send what was captured; when only armed, drop it and
            // let the next boundary re-arm.
            if (i.state == Impl::State::Capturing)
                finishOn = i.homeDevice;
            i.homeSwapChain = nullptr;
        }
    }
    if (finishOn)
        i.Finish(*this, finishOn);
}

void CaptureManager::OnDeviceReleased(ID3D12Device* device)
{
    if (!_impl)
        return;
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
        if (it == i.devices.end())
            return;
        dc = std::move(it->second);
        i.devices.erase(it);
    }
    // The device is going: its recorders' lists go with it, and the entries staged on it cannot
    // be read any more.
    {
        std::unique_lock lock(i.recorderMutex);
        for (auto it = i.recorders.begin(); it != i.recorders.end();)
        {
            if (it->second.rec && it->second.rec->device() == device)
                it = i.recorders.erase(it);
            else
                ++it;
        }
        _recorderCount.store(i.recorders.size(), std::memory_order_relaxed);
    }
    {
        std::lock_guard lock(i.mutex);
        for (TextureEntry& t : i.textures)
        {
            if (t.device == device && !t.failed)
            {
                t.failed = true;
                t.note = "device was released during the capture";
            }
        }
        for (BufferEntry& b : i.buffers)
        {
            if (b.device == device && !b.failed)
            {
                b.failed = true;
                b.note = "device was released during the capture";
            }
        }
        for (TimingEntry& t : i.timings)
            if (t.device == device)
                t.frame = UINT32_MAX;
    }
    i.ReleaseCaptureObjects(*dc);
    OnMeasurementDeviceReleased(device);
    dc.reset();   // the query heaps, the fence and the readback buffer, before the device's last Release runs
    Log("capture: device %p released", (void*)device);
}

// ---------------------------------------------------------------------------------------------
// Passes

namespace
{

/** `count` consecutive queries from a heap's counter, or UINT32_MAX when the heap is full. */
uint32_t ReserveQueries(std::atomic<uint32_t>& used, uint32_t count, uint32_t limit)
{
    const uint32_t first = used.fetch_add(count, std::memory_order_relaxed);
    if (first + count > limit)
        return UINT32_MAX;
    return first;
}

/**
 * The per-draw query heaps and their readback buffer, made once per device on the first draw that
 * is measured. False when the device would not make them, which turns the measurement off rather
 * than failing the capture.
 */
bool EnsureDrawQueries(DeviceCapture& dc, ID3D12Device* device)
{
    if (dc.drawQueriesMade.load(std::memory_order_acquire))
        return dc.drawTimestampHeap && dc.drawMapped;
    std::lock_guard lock(dc.mutex);
    if (dc.drawQueriesMade.load(std::memory_order_relaxed))
        return dc.drawTimestampHeap && dc.drawMapped;
    ScopedInternal internal;
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kDrawSlots * 2, dc.drawTimestampHeap.put());
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, kDrawSlots, dc.drawStatsHeap.put());
    CreateQueryHeap(device, D3D12_QUERY_HEAP_TYPE_OCCLUSION, kDrawSlots, dc.drawOcclusionHeap.put());
    if (CreateReadbackBuffer(device, kDrawReadbackBytes, dc.drawReadback.put()))
    {
        D3D12_RANGE none{0, 0};
        if (FAILED(dc.drawReadback->Map(0, &none, &dc.drawMapped)))
            dc.drawMapped = nullptr;
    }
    dc.drawQueriesMade.store(true, std::memory_order_release);
    return dc.drawTimestampHeap && dc.drawMapped;
}

/** Whether a list of this type can carry the capture's timestamp queries (a copy list needs a heap of another type). */
bool TimestampsAllowed(D3D12_COMMAND_LIST_TYPE type)
{
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
}

}  // namespace

uint32_t CaptureManager::BeginPass(CommandRecorder* rec, std::vector<BoundTarget> targets, bool renderPassApi, bool split)
{
    if (!rec)
        return 0;
    Impl& i = impl();
    ActivePass& pass = rec->pass();
    pass = ActivePass{};
    pass.active = true;
    pass.renderPassApi = renderPassApi;
    // Not an adopted list's pass (CommandRecorder::adopted): the capture saw it begin, so it is not
    // inside a region the capture missed, and it ends where the capture sees it end.
    pass.split = split;
    pass.targets = std::move(targets);
    pass.passIndex = rec->NextPassIndex();
    pass.beginCommand = rec->commandCount() ? (uint32_t)rec->commandCount() - 1 : 0;
    if (!pass.targets.empty())
    {
        D3D12_RESOURCE_DESC desc;
        const BoundTarget& t = pass.targets.front();
        if (t.resource && DescOf(t.resource, desc))
        {
            pass.width = MipDim(desc.Width, t.mip);
            pass.height = MipDim(desc.Height, t.mip);
        }
    }
    // A run of dispatches ends where a render pass begins.
    OnComputePassEnd(rec);

    // TakesContents, not Capturing: a pass's queries go into the list as it is recorded, and an
    // engine that records its lists a frame or more ahead (Unity does) records the captured frame's
    // lists during the warm-up frame. Timing only what is recorded once the capture has started
    // measures nothing at all of such a frame -- 58 passes and no timings on a Unity player. The
    // entries queries made now carry `warmup` and only become part of the capture if their list
    // runs in it (OnExecuteCommandLists assigns the frame), as read-back entries already do.
    bool profile;
    {
        std::lock_guard lock(i.mutex);
        profile = i.TakesContents() && i.options.profilePasses;
    }
    // A pass suspended across command lists is timed like any other now: its timestamps are single
    // EndQuery calls inside the pass region, and nothing of the capture's is resolved in the
    // application's list any more (EndPassQueries). An adopted list is timed the same way: what
    // state it was in when the capture found it is unknown, but a timestamp is allowed in any. It
    // matters, because an engine that pools its lists resets them frames before recording into
    // them (CaptureManager::Adopt), and whichever were reset before the capture was asked for come
    // back adopted: 40 to 60 of a Unity frame's lists in some captures, whose passes went untimed.
    if (!profile || rec->bundle() || !TimestampsAllowed(rec->type()))
        return pass.passIndex;
    DeviceCapture* dc = i.CaptureFor(rec->device());
    if (!dc || !dc->timestampHeap || !dc->queryMapped)
        return pass.passIndex;
    const uint32_t slot = dc->slotsUsed.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kPassSlots)
        return pass.passIndex;   // the counter keeps climbing; only the first kPassSlots passes are timed
    pass.timestampQuery = slot * 2;
    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = rec->list();
    list->EndQuery(dc->timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, pass.timestampQuery);
    // Statistics and occlusion need graphics; neither is begun inside a BeginRenderPass region
    // (the resolve at pass end would have to wait for EndRenderPass, and BeginQuery there is not
    // something every driver accepts), nor while the application has a query of its own open,
    // which an adopted list may have from before the capture found it.
    if (rec->type() != D3D12_COMMAND_LIST_TYPE_DIRECT || renderPassApi || pass.split || rec->adopted())
        return pass.passIndex;
    if (dc->statsHeap)
    {
        pass.statsQuery = ReserveQueries(dc->statsUsed, 1, kStatsQueries);
        if (pass.statsQuery != UINT32_MAX)
            list->BeginQuery(dc->statsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, pass.statsQuery);
    }
    if (dc->occlusionHeap && rec->state().appQueryDepth == 0)
    {
        pass.occlusionQuery = ReserveQueries(dc->occlusionUsed, 1, kOcclusionQueries);
        if (pass.occlusionQuery != UINT32_MAX)
            list->BeginQuery(dc->occlusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, pass.occlusionQuery);
    }
    return pass.passIndex;
}

void CaptureManager::EndOpenPass(ID3D12GraphicsCommandList* list, bool synthetic)
{
    CommandRecorder* rec = LookupRecorder(list);
    if (!rec)
        return;
    EndPass(rec, synthetic);
    OnComputePassEnd(rec);
}

void CaptureManager::EndSplitPassTimestamp(CommandRecorder* rec)
{
    if (!rec)
        return;
    ActivePass& pass = rec->pass();
    // Only a split pass needs its end timestamp this early. Every other pass writes it in EndPass,
    // after EndRenderPass has been forwarded, where it has always been written.
    if (!pass.active || !pass.split || pass.timestampEnded || pass.timestampQuery == UINT32_MAX)
        return;
    DeviceCapture* dc = impl().FindCapture(rec->device());
    if (!dc || !dc->timestampHeap)
        return;
    ScopedInternal internal;
    rec->list()->EndQuery(dc->timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, pass.timestampQuery + 1);
    pass.timestampEnded = true;
}

void CaptureManager::OnDraw(CommandRecorder* rec)
{
    NoteIndexedHeaps(rec, false);
    // The pass's attachments: a draw loads what it does not clear, and writes it.
    if (rec)
    {
        for (const BoundTarget& t : rec->pass().targets)
        {
            const bool cleared = t.beginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR ||
                t.beginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
            if (!cleared)
                rec->NoteRead(t.resource);
            if (!t.readOnlyDepth)
                rec->NoteWritten(t.resource);
        }
    }
    if (rec && rec->pass().active)
        rec->pass().drawCount++;
}

void CaptureManager::NoteIndexedHeaps(CommandRecorder* rec, bool compute)
{
    if (!rec)
        return;
    ListState& s = rec->state();
    const std::shared_ptr<const RootSignatureInfo>& layout = compute ? s.computeLayout : s.graphicsLayout;
    if (!layout)
        return;
    if ((layout->flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) && s.heaps[0])
        s.indexed[0] = s.heaps[0];
    if ((layout->flags & D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED) && s.heaps[1])
        s.indexed[1] = s.heaps[1];
}

uint32_t CaptureManager::BeginDrawQueries(CommandRecorder* rec)
{
    if (!rec || rec->bundle() || !TimestampsAllowed(rec->type()))
        return UINT32_MAX;
    Impl& i = impl();
    {
        std::lock_guard lock(i.mutex);
        // The warm-up frame as well, for the reason BeginPass takes its queries then: the draws of
        // the captured frame belong to lists an engine that records ahead recorded before it.
        if (!i.TakesContents() || !i.options.drawTimings)
            return UINT32_MAX;
    }
    // A pass suspended across command lists takes nothing, here as in BeginPass: a query begun in
    // one part and ended in another closes the list with E_FAIL.
    if (rec->pass().active && rec->pass().split)
        return UINT32_MAX;
    DeviceCapture* dc = i.CaptureFor(rec->device());
    if (!dc || !EnsureDrawQueries(*dc, rec->device()))
        return UINT32_MAX;
    const uint32_t slot = dc->drawSlotsUsed.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kDrawSlots)
        return UINT32_MAX;   // the counter keeps climbing; only the first kDrawSlots draws are measured
    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = rec->list();
    list->EndQuery(dc->drawTimestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    // Statistics and occlusion need a direct list, and are not begun while the application has a
    // query of its own open, nor inside a BeginRenderPass region: their resolve would have to wait
    // for EndRenderPass, which is where a pass's own queries stop for the same reason (BeginPass).
    uint32_t packed = slot;
    // Nor in an adopted list, whose own queries may have been begun before the capture found it.
    const bool graphics = rec->type() == D3D12_COMMAND_LIST_TYPE_DIRECT && !rec->adopted() && !(rec->pass().active && rec->pass().renderPassApi);
    if (graphics && dc->drawStatsHeap)
    {
        list->BeginQuery(dc->drawStatsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, slot);
        packed |= kDrawStatsBit;
    }
    if (graphics && dc->drawOcclusionHeap && rec->state().appQueryDepth == 0)
    {
        list->BeginQuery(dc->drawOcclusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, slot);
        packed |= kDrawOcclusionBit;
    }
    return packed;
}

void CaptureManager::EndDrawQueries(CommandRecorder* rec, uint32_t packed, bool dispatch)
{
    if (!rec || packed == UINT32_MAX)
        return;
    Impl& i = impl();
    DeviceCapture* dc = i.FindCapture(rec->device());
    if (!dc || !dc->drawTimestampHeap)
        return;
    const uint32_t slot = packed & kDrawSlotMask;
    DrawEntry de;
    de.hasStats = (packed & kDrawStatsBit) != 0;
    de.hasOcclusion = (packed & kDrawOcclusionBit) != 0;
    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = rec->list();
    if (de.hasStats)
        list->EndQuery(dc->drawStatsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, slot);
    if (de.hasOcclusion)
        list->EndQuery(dc->drawOcclusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, slot);
    list->EndQuery(dc->drawTimestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    de.device = rec->device();
    de.list = list;
    de.listId = Tracker::Get().IdOf(list);
    // The draw has been recorded by now, so it is the command the recorder last took.
    de.command = rec->commandCount() ? (uint32_t)rec->commandCount() - 1 : 0;
    de.passIndex = rec->pass().active ? rec->pass().passIndex : UINT32_MAX;
    de.dispatch = dispatch;
    de.slot = slot;
    std::lock_guard lock(i.mutex);
    de.warmup = i.state != Impl::State::Capturing;
    i.draws.push_back(de);
    i.pendingDrawSlots[list].push_back(packed);
}

void CaptureManager::ResolveDrawQueries(CommandRecorder* rec)
{
    if (!rec)
        return;
    Impl& i = impl();
    ID3D12GraphicsCommandList* list = rec->list();
    std::vector<uint32_t> slots;
    {
        std::lock_guard lock(i.mutex);
        auto it = i.pendingDrawSlots.find(list);
        if (it == i.pendingDrawSlots.end())
            return;
        slots.swap(it->second);
        i.pendingDrawSlots.erase(it);
    }
    if (slots.empty())
        return;
    DeviceCapture* dc = i.FindCapture(rec->device());
    if (!dc || !dc->drawReadback)
        return;
    std::sort(slots.begin(), slots.end(), [](uint32_t a, uint32_t b) { return (a & kDrawSlotMask) < (b & kDrawSlotMask); });
    ScopedInternal internal;
    /**
     * One resolve per run of consecutive slots that all carry the query: a list recording its draws
     * in order takes a contiguous block, so a pass of thousands of draws resolves in a call or two.
     * A slot whose query was never begun is left out rather than resolved, since what a resolve
     * reads from one is undefined and the debug layer says so.
     */
    auto resolve = [&](ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, uint32_t bit, uint32_t perSlot, uint64_t base,
                       uint64_t bytes) {
        if (!heap)
            return;
        for (size_t first = 0; first < slots.size();)
        {
            if (bit && !(slots[first] & bit))
            {
                first++;
                continue;
            }
            size_t last = first;
            while (last + 1 < slots.size() && (slots[last + 1] & kDrawSlotMask) == (slots[last] & kDrawSlotMask) + 1 &&
                (!bit || (slots[last + 1] & bit)))
                last++;
            const uint32_t start = slots[first] & kDrawSlotMask;
            const uint32_t count = (uint32_t)(last - first + 1);
            list->ResolveQueryData(heap, type, start * perSlot, count * perSlot, dc->drawReadback.get(),
                base + (uint64_t)start * bytes);
            first = last + 1;
        }
    };
    resolve(dc->drawTimestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, kDrawTimestampBase, kDrawTimestampBytes);
    resolve(dc->drawStatsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, kDrawStatsBit, 1, kDrawStatsBase, kDrawStatsBytes);
    resolve(dc->drawOcclusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, kDrawOcclusionBit, 1, kDrawOcclusionBase,
        kDrawOcclusionBytes);
}

namespace
{

/** The queries of a pass or compute pass ended and resolved into the pass's slot of the readback buffer. */
/**
 * Ends a pass's queries in the application's list, and resolves none of them: a `ResolveQueryData`
 * is exactly what a list may not hold while a render pass is suspended (it closes with E_FAIL and
 * the application reads that as a lost device), and it is the reason a suspended pass used to go
 * unmeasured. An `EndQuery` is allowed there, so the queries are ended here and the capture
 * resolves all of them itself, from a list of its own, once the frame's work is done
 * (`Impl::ResolveQueries`).
 *
 * `timestampEnded` says the end timestamp was already written before EndRenderPass was forwarded,
 * which is where a split pass writes it.
 */
void EndPassQueries(DeviceCapture& dc, ID3D12GraphicsCommandList* list, uint32_t timestampQuery, uint32_t statsQuery,
    uint32_t occlusionQuery, bool timestampEnded = false)
{
    ScopedInternal internal;
    if (statsQuery != UINT32_MAX)
        list->EndQuery(dc.statsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, statsQuery);
    if (occlusionQuery != UINT32_MAX)
        list->EndQuery(dc.occlusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, occlusionQuery);
    if (!timestampEnded)
        list->EndQuery(dc.timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, timestampQuery + 1);
}

/** Records the copy of one subresource into a placed footprint of the staging buffer, with the barriers its state needs. */
void CopySubresource(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t subresource, D3D12_RESOURCE_STATES state,
    ID3D12Resource* staging, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint)
{
    ScopedInternal internal;
    const bool barrier = NeedsCopyBarrier(state);
    if (barrier)
        Transition(list, resource, subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = subresource;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (barrier)
        Transition(list, resource, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

}  // namespace

void CaptureManager::EndPass(CommandRecorder* rec, bool synthetic)
{
    if (!rec)
        return;
    Impl& i = impl();
    ActivePass& pass = rec->pass();
    if (!pass.active)
        return;
    // A pass suspended across command lists takes no copies and no read-back (ActivePass::split):
    // a copy or a barrier here would close the list with E_FAIL. Its timestamps are already in the
    // list, though -- written inside the pass region by BeginPass and EndSplitPassTimestamp, and
    // resolved by the capture itself at the finish -- so it is timed like any other pass.
    if (pass.split)
    {
        if (synthetic)
            rec->Record("EndRenderTargets", std::string());
        pass.active = false;
        TimingEntry te;
        const bool timed = pass.timestampQuery != UINT32_MAX && pass.timestampEnded;
        if (timed)
        {
            te.device = rec->device();
            te.list = rec->list();
            te.listId = Tracker::Get().IdOf(rec->list());
            te.passIndex = pass.passIndex;
            te.compute = false;
            te.slot = pass.timestampQuery / 2;
        }
        std::lock_guard lock(i.mutex);
        ++i.splitPasses;
        if (timed && i.TakesContents())
        {
            te.warmup = i.state != Impl::State::Capturing;
            i.timings.push_back(te);
        }
        return;
    }
    ID3D12GraphicsCommandList* list = rec->list();
    bool capturing, takesContents, captureTextures;
    uint64_t maxTextureSize, maxTargetTotal;
    {
        std::lock_guard lock(i.mutex);
        capturing = i.state == Impl::State::Capturing;
        takesContents = i.TakesContents();
        captureTextures = i.options.captureTextures;
        maxTextureSize = i.options.maxTextureSize;
        maxTargetTotal = i.options.maxTargetTotal;
    }
    // Not CaptureFor: the queries of a pass that began them have to be ended whichever state the
    // capture is in now, and the device's capture outlives the capture itself.
    DeviceCapture* dc = i.FindCapture(rec->device());
    const uint64_t listId = Tracker::Get().IdOf(list);

    // The queries first: what the pass drew is what they count, not the copies that follow.
    //
    // A pass begun while the capture ran must end its queries even if the capture has finished
    // since -- which happens on every engine that records its lists on worker threads, since the
    // capture ends at a frame boundary and those lists are in the middle of a pass. A command list
    // closed with a query still open fails with E_FAIL, and the application reads that as a lost
    // device. The timing is only worth keeping while the capture is still collecting them.
    if (dc && pass.timestampQuery != UINT32_MAX)
    {
        EndPassQueries(*dc, list, pass.timestampQuery, pass.statsQuery, pass.occlusionQuery, pass.timestampEnded);
        // A split pass whose end timestamp was never written has half a pair and no timing: its
        // EndRenderPass was not the one this capture saw (a pass open when the capture began, or a
        // list closed inside one).
        const bool paired = !pass.split || pass.timestampEnded;
        // A pass timed during the warm-up frame is kept the same way, marked: the list it is in may
        // be one the captured frame runs, and then the timing is the captured frame's.
        if (takesContents && paired)
        {
            TimingEntry te;
            te.device = rec->device();
            te.list = list;
            te.listId = listId;
            te.warmup = !capturing;
            te.passIndex = pass.passIndex;
            te.compute = false;
            te.slot = pass.timestampQuery / 2;
            te.hasStats = pass.statsQuery != UINT32_MAX;
            te.hasOcclusion = pass.occlusionQuery != UINT32_MAX;
            te.statsQuery = pass.statsQuery;
            te.occlusionQuery = pass.occlusionQuery;
            std::lock_guard lock(i.mutex);
            i.timings.push_back(te);
        }
    }
    // The draws measured inside the pass, now that the pass is over and a resolve is allowed.
    ResolveDrawQueries(rec);

    // Then every target, slice by slice, into staging. A depth-stencil target is read back twice:
    // its depth plane, then its stencil plane (`asStencil`: plane 1, one byte per texel), each an
    // entry of its own under the same attachment index.
    if (dc && capturing && captureTextures && !rec->bundle())
    {
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
            if (!DescOf(t.resource, desc, &info))
                return fail("resource is not tracked");
            const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            ProtocolFormat pf = ProtocolFormatOf(t.format, t.depth);
            if (asStencil)
                pf.texelBytes = 1;
            e.width = MipDim(desc.Width, t.mip);
            e.height = MipDim(desc.Height, t.mip);
            e.samples = desc.SampleDesc.Count;
            e.depthAspect = pf.depth && !asStencil;
            e.stencilAspect = asStencil;
            // A 3D render target: its slices are the depth slices of the mip, copied whole.
            const uint32_t slices = volume ? MipDim(desc.DepthOrArraySize, t.mip) : std::min(std::max(1u, t.sliceCount), kMaxAttachmentSlices);
            e.layers = slices;
            if (!pf.name)
                return fail(std::string("format ") + FormatName(t.format) + " cannot be decoded");
            e.format = pf.name;
            const uint64_t rowBytes = TightRowBytes(pf, e.width);
            const uint32_t rows = TightRows(pf, e.height);
            e.size = rowBytes * rows * slices;
            if (e.size > maxTextureSize)
                return fail("exceeds max texture size");
            // The budget for the frame's targets together. Past it the copies stop: they are work
            // the capture adds to the application's own lists, and a frame with thousands of passes
            // asks for more of it than the frame itself does.
            {
                std::lock_guard lock(i.mutex);
                if (i.targetBytes + e.size > maxTargetTotal)
                {
                    e.failed = true;
                    e.note = "render target capture budget exceeded";
                    i.textures.push_back(e);
                    return;
                }
                i.targetBytes += e.size;
            }
            if (t.depth && e.samples > 1)
                return fail(asStencil ? "multisampled stencil is not read back" : "multisampled depth is not read back");
            if (t.mip >= desc.MipLevels)
                return fail("mip level out of range");
            // The stencil plane's subresources follow every mip of every slice of the depth plane.
            const uint32_t planeOffset = asStencil ? desc.MipLevels * (uint32_t)desc.DepthOrArraySize : 0;

            // A multisampled color target resolves into a single-sampled texture of the capture's
            // first; the copies then read that.
            ID3D12Resource* source = t.resource;
            D3D12_RESOURCE_DESC sourceDesc = desc;
            ID3D12Resource* resolve = nullptr;
            const DXGI_FORMAT typed = TypedFormat(t.format, t.depth);
            if (e.samples > 1)
            {
                ResolveKey key{list, typed, e.width, e.height, slices};
                resolve = i.ResolveTextureFor(*dc, key);
                if (!resolve)
                    return fail("resolve texture could not be created");
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
                for (uint32_t s = 0; s < slices; ++s)
                {
                    const uint32_t slice = volume ? 0 : t.firstSlice + s;
                    const uint32_t sub = resolve ? s : (volume ? t.mip : t.mip + slice * desc.MipLevels) + planeOffset;
                    sourceSubresources[s] = sub;
                    UINT numRows = 0;
                    UINT64 rowSize = 0, bytes = 0;
                    rec->device()->GetCopyableFootprints(&sourceDesc, sub, 1, total, &footprints[s], &numRows, &rowSize, &bytes);
                    footprints[s].Offset = total;   // relative to the span; rebased once it is allocated
                    total += Align(bytes, kStagingAlignment);
                    if (volume)
                        break;   // one subresource holds every depth slice
                }
            }
            uint64_t spanOffset = 0;
            ID3D12Resource* staging = nullptr;
            if (!i.AllocateStaging(*dc, rec->type(), total, e.chunk, spanOffset, &staging))
                return fail("staging allocation failed");

            const uint32_t copies = volume ? 1 : slices;
            for (uint32_t s = 0; s < copies; ++s)
            {
                const uint32_t slice = volume ? 0 : t.firstSlice + s;
                const uint32_t origSub = (volume ? t.mip : t.mip + slice * desc.MipLevels) + planeOffset;
                // The list's own last transition of the target, else the state the pass needs it in:
                // a target being written is in RENDER_TARGET or DEPTH_WRITE for as long as the pass
                // lasts, whatever the tracker's global state says. That is only as recent as the
                // last submission, and a list a job records -- every list of a Unity frame, and
                // every adopted one -- does not see what the lists before it in its submission did
                // to the target. Read-only depth may be in a combined read state, which the global
                // state is the better guess at.
                bool known = false, inList = false;
                D3D12_RESOURCE_STATES state = ResourceTracker::Get().StateIn(list, t.resource, origSub, &known, &inList);
                if (!inList && !(t.depth && t.readOnlyDepth && known))
                {
                    state = t.depth ? (t.readOnlyDepth ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE)
                                    : D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = footprints[s];
                fp.Offset += spanOffset;
                if (resolve)
                {
                    ScopedInternal internal;
                    Transition(list, t.resource, origSub, state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                    list->ResolveSubresource(resolve, s, t.resource, origSub, typed);
                    Transition(list, t.resource, origSub, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, state);
                    Transition(list, resolve, s, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    CopySubresource(list, resolve, s, D3D12_RESOURCE_STATE_COPY_SOURCE, staging, fp);
                    Transition(list, resolve, s, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                }
                else
                {
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
        for (const BoundTarget& t : pass.targets)
        {
            if (!t.resource)
                continue;
            readBack(t, false);
            if (t.depth && FormatOf(TypedFormat(t.format, true)).stencil)
                readBack(t, true);
        }
    }

    // Copies queued inside a BeginRenderPass region were held for its end.
    if (std::vector<DeferredCopy>* deferred = i.DeferredOf(rec))
    {
        std::vector<DeferredCopy> pending;
        pending.swap(*deferred);
        for (auto& fn : pending)
            fn(rec->list());
    }
    // The measurements the capture asked for, drawn into the same list now that the pass's own
    // work and its read-back are in it (overdraw.h). A real render pass the application never
    // ended leaves the list inside its region, where a measurement can bind nothing.
    EndMeasuredPass(rec, pass.renderPassApi && synthetic);
    if (synthetic)
        rec->Record("EndRenderTargets", std::string());
    pass.active = false;
}

void CaptureManager::OnBeforeDispatch(CommandRecorder* rec)
{
    if (!rec)
        return;
    NoteIndexedHeaps(rec, true);
    ActiveComputePass& compute = rec->compute();
    if (rec->pass().active || compute.active)
        return;   // inside a render pass the dispatch stays there
    compute = ActiveComputePass{};
    compute.active = true;
    compute.index = rec->NextComputeIndex();
    Impl& i = impl();
    bool profile;
    {
        std::lock_guard lock(i.mutex);
        profile = i.TakesContents() && i.options.profilePasses;   // the warm-up frame too (BeginPass)
    }
    // An adopted list too: a timestamp is allowed whatever state it is in (BeginPass).
    if (!profile || rec->bundle() || !TimestampsAllowed(rec->type()))
        return;
    DeviceCapture* dc = i.CaptureFor(rec->device());
    if (!dc || !dc->timestampHeap || !dc->queryMapped)
        return;
    const uint32_t slot = dc->slotsUsed.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kPassSlots)
        return;
    compute.timestampQuery = slot * 2;
    ScopedInternal internal;
    rec->list()->EndQuery(dc->timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, compute.timestampQuery);
}

void CaptureManager::OnComputePassEnd(CommandRecorder* rec)
{
    if (!rec)
        return;
    ActiveComputePass& compute = rec->compute();
    if (!compute.active)
        return;
    compute.active = false;
    if (compute.timestampQuery == UINT32_MAX)
        return;
    Impl& i = impl();
    DeviceCapture* dc = i.FindCapture(rec->device());
    if (!dc)
        return;
    EndPassQueries(*dc, rec->list(), compute.timestampQuery, UINT32_MAX, UINT32_MAX);
    TimingEntry te;
    te.device = rec->device();
    te.list = rec->list();
    te.listId = Tracker::Get().IdOf(rec->list());
    te.passIndex = compute.index;
    te.compute = true;
    te.slot = compute.timestampQuery / 2;
    std::lock_guard lock(i.mutex);
    te.warmup = i.state != Impl::State::Capturing;
    i.timings.push_back(te);
}

void CaptureManager::OnBeforeClose(ID3D12GraphicsCommandList* list)
{
    // LookupRecorder, not RecorderFor: a list left open when the capture finished keeps its
    // recorder exactly so that this runs, whether or not anything is recording now (EndOpenPass).
    CommandRecorder* rec = LookupRecorder(list);
    if (!rec)
        return;
    EndPass(rec, true);
    OnComputePassEnd(rec);
    // Whatever was measured outside a pass (a dispatch between them) is resolved here instead.
    ResolveDrawQueries(rec);
    // A BeginRenderPass region left open at Close is the application's error; its deferred copies
    // still have to land somewhere, and the list is about to close.
    if (std::vector<DeferredCopy>* deferred = impl().DeferredOf(rec))
    {
        std::vector<DeferredCopy> pending;
        pending.swap(*deferred);
        for (auto& fn : pending)
            fn(rec->list());
    }
    rec->MarkClosed();
    // The list is closed and nothing records any more: this recorder was only kept for the pass
    // just ended (Impl::Finish), so it goes now rather than waiting for the list to be released.
    if (!ShouldRecord())
    {
        Impl& i = impl();
        std::unique_lock lock(i.recorderMutex);
        i.recorders.erase(list);
        _recorderCount.store(i.recorders.size(), std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------------------------
// Bindings: the descriptor snapshot on a root table or root view, and the read-back it queues

namespace
{

/** The bytes of one element of a buffer view: its structure stride, 4 for a raw view, else the format's texel. */
uint32_t ElementStride(uint32_t structureByteStride, bool raw, DXGI_FORMAT format)
{
    if (structureByteStride)
        return structureByteStride;
    if (raw)
        return 4;
    const uint32_t bytes = FormatOf(format).bytes;
    return bytes ? bytes : 4;
}

}  // namespace

uint32_t CaptureManager::QueueBufferCapture(CommandRecorder* rec, ID3D12Resource* buffer, UINT64 offset, UINT64 size, bool whole,
    bool afterSubmit)
{
    if (!rec || !buffer)
        return 0;
    Impl& i = impl();
    ResourceInfo info;
    if (!ResourceTracker::Get().Get(buffer, info) || info.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        return 0;
    if (offset >= info.desc.Width)
        return 0;
    size = std::min<UINT64>(size, info.desc.Width - offset);
    if (!size)
        return 0;
    DeviceCapture* dc = nullptr;
    {
        std::lock_guard lock(i.mutex);
        if (!i.TakesContents() || !i.options.captureBuffers)
            return 0;
        // The same range read for a list recorded meanwhile is the same read-back, until the list
        // that took it has run: after that it holds a moment the frame has moved past -- the next
        // page frame of a WebGPU simulation reads the buffer the last one wrote -- and the range is
        // read again.
        auto it = i.bufferIds.find({buffer, offset, size});
        if (it != i.bufferIds.end() && it->second && it->second <= i.buffers.size() && i.buffers[it->second - 1].frame == UINT32_MAX)
        {
            ShareEntry(i.buffers[it->second - 1], rec->list());
            return it->second;
        }
    }
    dc = i.CaptureFor(rec->device());
    if (!dc)
        return 0;
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
        if (!i.TakesContents())
            return 0;
        e.warmup = i.state != Impl::State::Capturing;
        e.id = (uint32_t)i.buffers.size() + 1;
        i.bufferIds[{buffer, offset, size}] = e.id;
        if (info.heapType == D3D12_HEAP_TYPE_READBACK)
        {
            e.failed = true;
            e.note = "a buffer in a readback heap cannot be a copy source";
        }
        else if (HoldsAccelerationStructure(buffer))
        {
            // Not a size or a budget: it cannot be copied at all (raytracing.h).
            e.failed = true;
            e.note =
                "a buffer holding a ray tracing acceleration structure cannot be read back: its layout is the "
                "driver's, and a resource in RAYTRACING_ACCELERATION_STRUCTURE may not be transitioned to be copied";
        }
        else
        {
            if (e.size > i.options.maxBufferSize && !whole)
            {
                e.originalSize = e.size;
                e.size = i.options.maxBufferSize;
            }
            if (i.bufferBytes + e.size > i.options.maxBufferTotal)
            {
                e.failed = true;
                e.note = "buffer capture budget exceeded";
            }
            else if (!i.AllocateStaging(*dc, rec->type(), e.size, e.chunk, e.stagingOffset, &staging))
            {
                e.failed = true;
                e.note = "staging allocation failed";
            }
            else
            {
                i.bufferBytes += e.size;
            }
        }
        i.buffers.push_back(e);
    }
    if (e.failed)
        return e.id;

    // The copy, with the barriers the buffer's state needs. An upload-heap buffer is always
    // GENERIC_READ; one nothing transitioned is COMMON, which a copy promotes.
    const uint64_t copyOffset = e.offset, copySize = e.size, stagingOffset = e.stagingOffset;
    const D3D12_HEAP_TYPE heapType = info.heapType;
    auto copy = [buffer, staging, copyOffset, copySize, stagingOffset, heapType](ID3D12GraphicsCommandList* list) {
        ScopedInternal internal;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_GENERIC_READ;
        if (heapType != D3D12_HEAP_TYPE_UPLOAD)
        {
            bool known = false;
            state = ResourceTracker::Get().StateIn(list, buffer, 0, &known);
            if (!known)
                state = D3D12_RESOURCE_STATE_COMMON;
        }
        // The backstop for a structure the library never saw built -- an application attached to
        // after it had built them. A barrier out of this state is rejected and closes the list.
        if (state == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
            return;
        const bool barrier = NeedsCopyBarrier(state);
        if (barrier)
            Transition(list, buffer, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(staging, stagingOffset, buffer, copyOffset, copySize);
        if (barrier)
            Transition(list, buffer, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    };
    // Asked for after the submission: the list is closed, so the copy goes in the library's own list
    // behind it (RunAfterSubmitCopies).
    if (afterSubmit)
    {
        {
            std::shared_lock lock(i.recorderMutex);
            auto it = i.recorders.find(list);
            if (it != i.recorders.end() && it->second.rec.get() == rec)
            {
                it->second.afterSubmit.push_back(std::move(copy));
                return e.id;
            }
        }
        // Nothing will make the copy, so nothing may pretend it was made.
        std::lock_guard lock(i.mutex);
        if (e.id && e.id <= i.buffers.size())
        {
            i.buffers[e.id - 1].failed = true;
            i.buffers[e.id - 1].note = "the list it was to follow has no recorder";
        }
        return e.id;
    }
    // A bundle cannot copy: its copies go into the list that executes it. Inside a BeginRenderPass
    // region they wait for its end.
    // A suspended pass takes nothing at all, so there they wait for the submission (HeldCopiesOf).
    if (std::vector<DeferredCopy>* held = i.HeldCopiesOf(rec))
        held->push_back(std::move(copy));
    else if (!rec->bundle())
        copy(list);
    return e.id;
}

uint32_t CaptureManager::QueueAddressCaptureAfterSubmit(CommandRecorder* rec, D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 size)
{
    if (!rec || !address || rec->bundle())
        return 0;
    ID3D12Resource* buffer = nullptr;
    UINT64 offset = 0, remaining = 0;
    if (!AddressMap::Get().Resolve(address, buffer, offset, remaining))
        return 0;
    return QueueBufferCapture(rec, buffer, offset, size ? std::min<UINT64>(size, remaining) : remaining, true, true);
}

uint32_t CaptureManager::QueueAddressCapture(CommandRecorder* rec, D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 size, bool whole)
{
    if (!rec || !address)
        return 0;
    ID3D12Resource* buffer = nullptr;
    UINT64 offset = 0, remaining = 0;
    if (!AddressMap::Get().Resolve(address, buffer, offset, remaining))
        return 0;
    return QueueBufferCapture(rec, buffer, offset, size ? std::min<UINT64>(size, remaining) : remaining, whole && size);
}

uint32_t CaptureManager::QueueTextureCapture(CommandRecorder* rec, ID3D12Resource* texture, std::vector<DeferredCopy>* initialInto)
{
    if (!rec || !texture)
        return 0;
    const bool initial = initialInto != nullptr;
    // A shader reading it: what the frame found there may be what the replay has to start from.
    if (!initial)
        rec->NoteRead(texture);
    Impl& i = impl();
    {
        std::lock_guard lock(i.mutex);
        if (!i.TakesContents() || !i.options.captureImages)
            return 0;
        std::unordered_map<ID3D12Resource*, uint32_t>& ids = initial ? i.initialIds : i.textureIds;
        auto it = ids.find(texture);
        // As for a buffer: shared until the list that read it has run, then read again. What the
        // frame found is taken once whatever ran.
        if (it != ids.end() && (initial || (it->second && it->second <= i.textures.size() && i.textures[it->second - 1].frame == UINT32_MAX)))
        {
            if (!initial)
                ShareEntry(i.textures[it->second - 1], rec->list());
            return it->second;
        }
    }
    D3D12_RESOURCE_DESC desc{};
    ResourceInfo info;
    if (!DescOf(texture, desc, &info) || desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return 0;
    DeviceCapture* dc = i.CaptureFor(rec->device());
    if (!dc)
        return 0;
    ID3D12GraphicsCommandList* list = rec->list();
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;

    TextureEntry e;
    e.sampled = !initial;
    e.initial = initial;
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
    if (pf.name)
        e.format = pf.name;

    // Every mip with all its layers (a volume's depth slices), tightly packed, back to back.
    uint64_t tight = 0;
    for (uint32_t m = 0; m < e.mips; ++m)
    {
        const uint32_t slices = volume ? MipDim(desc.DepthOrArraySize, m) : e.layers;
        tight += TightRowBytes(pf, MipDim(desc.Width, m)) * TightRows(pf, MipDim(desc.Height, m)) * slices;
    }
    e.size = tight;

    auto finish = [&](const char* why) {
        if (why)
        {
            e.failed = true;
            e.note = why;
        }
        std::lock_guard lock(i.mutex);
        if (!i.TakesContents())
            return 0u;
        e.warmup = i.state != Impl::State::Capturing;
        e.captureId = (uint32_t)i.textures.size() + 1;
        (initial ? i.initialIds : i.textureIds)[texture] = e.captureId;
        if (!e.failed)
            i.imageBytes += e.size;
        i.textures.push_back(e);
        return e.captureId;
    };
    if (e.samples > 1)
        return finish("multisampled textures bound as shader resources are not read back");
    if (!pf.name)
        return finish("format cannot be decoded");
    bool tooLarge, overBudget;
    {
        std::lock_guard lock(i.mutex);
        tooLarge = e.size > i.options.maxTextureSize;
        overBudget = i.imageBytes + e.size > i.options.maxImageTotal;
    }
    if (tooLarge)
        return finish("exceeds max texture size");
    if (overBudget)
        return finish("image capture budget exceeded");

    // Footprints of every subresource copied, in the order of the data, then one staging span.
    struct Copy
    {
        uint32_t subresource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    };
    std::vector<Copy> copies;
    uint64_t total = 0;
    {
        ScopedInternal internal;
        for (uint32_t m = 0; m < e.mips; ++m)
        {
            const uint32_t perMip = volume ? 1 : e.layers;
            for (uint32_t l = 0; l < perMip; ++l)
            {
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
    if (!i.AllocateStaging(*dc, rec->type(), total, e.chunk, spanOffset, &staging))
        return finish("staging allocation failed");
    for (Copy& c : copies)
    {
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
    if (!id)
        return 0;

    auto copy = [texture, staging, copies](ID3D12GraphicsCommandList* list) {
        for (const Copy& c : copies)
        {
            bool known = false;
            D3D12_RESOURCE_STATES state = ResourceTracker::Get().StateIn(list, texture, c.subresource, &known);
            if (!known)
                state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            CopySubresource(list, texture, c.subresource, state, staging, c.footprint);
        }
    };
    if (initial)
        initialInto->push_back(std::move(copy));
    // A suspended pass takes nothing at all, so there they wait for the submission (HeldCopiesOf).
    else if (std::vector<DeferredCopy>* held = i.HeldCopiesOf(rec))
        held->push_back(std::move(copy));
    else if (!rec->bundle())
        copy(list);
    return id;
}

void CaptureManager::BeforeExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!queue || !lists || !IsCapturing())
        return;
    Impl& i = impl();
    // Copies move a resource to COPY_SOURCE and back, which only a direct queue may do from every
    // state a draw leaves it in; the read-backs taken later cover the other queues.
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    // The lists' reads in submission order, less what an earlier list of this submission or an
    // earlier submission of the capture wrote: a replay does those writes itself.
    std::vector<std::pair<ID3D12Resource*, CommandRecorder*>> reads;
    std::unordered_set<ID3D12Resource*> written;
    {
        std::lock_guard lock(i.mutex);
        written = i.frameWritten;
    }
    std::unordered_set<ID3D12Resource*> taken;
    std::unordered_set<ID3D12Resource*> writes;   // by this submission
    for (UINT k = 0; k < count; ++k)
    {
        CommandRecorder* rec = lists[k] ? LookupRecorder(static_cast<ID3D12GraphicsCommandList*>(lists[k])) : nullptr;
        if (!rec || rec->bundle())
            continue;
        for (ID3D12Resource* r : rec->readsBeforeWrites())
            if (!written.count(r) && taken.insert(r).second)
                reads.push_back({r, rec});
        written.insert(rec->writes().begin(), rec->writes().end());
        writes.insert(rec->writes().begin(), rec->writes().end());
    }
    {
        std::lock_guard lock(i.mutex);
        i.frameWritten = std::move(written);
        // A texture the submission only reads is the same after it as before, and one already read
        // back as sampled (at a pass's end, or right after the submission) has what it held: a
        // second copy would only double what a frame's static textures cost. What the submission
        // writes, and what nothing else reads back (a depth buffer a pass only tests against), is
        // taken here.
        reads.erase(std::remove_if(reads.begin(), reads.end(),
                        [&](const auto& r) { return !writes.count(r.first) && i.textureIds.count(r.first); }),
            reads.end());
    }
    if (reads.empty())
        return;
    std::vector<DeferredCopy> copies;
    ID3D12Device* device = reads.front().second->device();
    for (const auto& [resource, rec] : reads)
    {
        D3D12_RESOURCE_DESC desc{};
        if (!DescOf(resource, desc) || desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            continue;
        QueueTextureCapture(rec, resource, &copies);
    }
    if (copies.empty())
        return;
    DeviceCapture* dc = i.CaptureFor(device);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ScopedInternal internal;
    if (!dc || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put()))))
    {
        Log("capture: no list for what the frame found in %zu texture(s)", copies.size());
        return;
    }
    list->SetName(L"GPU Inspector: what the frame found, before a submission");
    // Not one of the application's lists, so a resource's state in it is the global state, which
    // the submission's own transitions have not reached yet (Hook_ExecuteCommandLists).
    for (DeferredCopy& copy : copies)
        copy(list.get());
    if (SUCCEEDED(list->Close()))
    {
        ID3D12CommandList* const submit[] = {list.get()};
        queue->ExecuteCommandLists(1, submit);
    }
    else
    {
        Log("capture: the list of what the frame found did not close");
    }
    std::lock_guard lock(dc->mutex);
    dc->ownAllocators.push_back(std::move(allocator));
    dc->ownLists.push_back(std::move(list));
}

namespace
{

/** The read-back a descriptor record names, queued; the capture id its `data` carries (0 for none). */
uint32_t QueueRecordData(CaptureManager& cm, CommandRecorder* rec, const DescriptorRecord& r)
{
    switch (r.kind)
    {
        case DescriptorKind::CBV:
            return r.address ? cm.QueueAddressCapture(rec, r.address, r.size) : 0;
        case DescriptorKind::SRV:
        case DescriptorKind::UAV:
        {
            if (r.accelerationStructure || !r.resource)
                return 0;
            // The pointer a descriptor holds is not known to name anything: DescOf refuses the
            // ones the tracker has let go, which is what keeps this off a released resource.
            D3D12_RESOURCE_DESC desc{};
            if (!DescOf(r.resource, desc))
                return 0;
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
                return cm.QueueTextureCapture(rec, r.resource);
            if (!r.hasDesc)
                return cm.QueueBufferCapture(rec, r.resource, 0, desc.Width);
            if (r.kind == DescriptorKind::SRV)
            {
                if (r.srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
                    return 0;
                const uint32_t stride = ElementStride(r.srv.Buffer.StructureByteStride,
                    (r.srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0, r.srv.Format);
                return cm.QueueBufferCapture(rec, r.resource, r.srv.Buffer.FirstElement * stride, (UINT64)r.srv.Buffer.NumElements * stride);
            }
            if (r.uav.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
                return 0;
            const uint32_t stride = ElementStride(r.uav.Buffer.StructureByteStride,
                (r.uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0, r.uav.Format);
            return cm.QueueBufferCapture(rec, r.resource, r.uav.Buffer.FirstElement * stride, (UINT64)r.uav.Buffer.NumElements * stride);
        }
        default:
            return 0;
    }
}

void BeginSnapshot(JsonWriter& w, bool compute, uint32_t parameterIndex, ID3D12DescriptorHeap* heap, ID3D12RootSignature* signature)
{
    w.BeginObject();
    w.Key("bindPoint");
    w.String(compute ? "compute" : "graphics");
    w.Key("sets");
    w.BeginArray();
    w.BeginObject();
    w.Key("set");
    w.Uint(parameterIndex);
    w.Key("descriptorSet");
    WriteRef(w, heap, "ID3D12DescriptorHeap");
    w.Key("layout");
    WriteRef(w, signature, "ID3D12RootSignature");
    w.Key("bindings");
    w.BeginArray();
}

void EndSnapshot(JsonWriter& w)
{
    w.EndArray();    // bindings
    w.EndObject();   // set
    w.EndArray();    // sets
    w.EndObject();
}

}  // namespace

void CaptureManager::SnapshotRootTable(CommandRecorder* rec, bool compute, uint32_t parameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    if (!rec)
        return;
    // A bundle that sets no root signature inherits the executing list's.
    const std::shared_ptr<const RootSignatureInfo> ownLayout = compute ? rec->state().computeLayout : rec->state().graphicsLayout;
    ID3D12RootSignature* ownSignature = compute ? rec->state().computeRootSignature : rec->state().graphicsRootSignature;
    rec->DeferSnapshot(compute, [this, compute, parameterIndex, base, ownLayout, ownSignature](CommandRecorder* rec) {
        ListState& state = rec->state();
        const std::shared_ptr<const RootSignatureInfo> layout = ownLayout ? ownLayout : compute ? state.computeLayout
                                                                                                : state.graphicsLayout;
        ID3D12RootSignature* signature = ownLayout ? ownSignature : compute ? state.computeRootSignature
                                                                            : state.graphicsRootSignature;
        HeapInfo heap;
        uint32_t index = 0;
        const bool located = DescriptorTracker::Get().Locate(base, heap, index);

        JsonWriter w(&Tracker::Get());
        BeginSnapshot(w, compute, parameterIndex, located ? heap.heap : nullptr, signature);
        const RootParameterInfo* param = layout && parameterIndex < layout->parameters.size() ? &layout->parameters[parameterIndex] : nullptr;
        if (param && param->type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && located)
        {
            for (size_t k = 0; k < param->ranges.size(); ++k)
            {
                const RootRange& range = param->ranges[k];
                const uint32_t first = index + range.offsetInTable;
                uint32_t count = range.numDescriptors;
                if (count == UINT_MAX)
                {
                // Unbounded: the rest of the heap, within reason.
                    count = first < heap.desc.NumDescriptors ? std::min(heap.desc.NumDescriptors - first, 1024u) : 0;
                }
                std::vector<uint64_t> writes;
                std::vector<DescriptorRecord> records = DescriptorTracker::Get().Slots(heap.heap, first, count, &writes);
                // Which writes the snapshot saw, for the submission to tell a later rewrite by.
                rec->tableSlots().push_back({heap.heap, first, std::move(writes)});
                w.BeginObject();
                w.Key("binding");
                w.Uint(k);
                w.Key("type");
                w.Enum(ToString_D3D12_DESCRIPTOR_RANGE_TYPE((int64_t)range.type), (int64_t)range.type);
                w.Key("register");
                w.Uint(range.baseRegister);
                w.Key("space");
                w.Uint(range.space);
                w.Key("stages");
                w.Enum(ToString_D3D12_SHADER_VISIBILITY((int64_t)param->visibility), (int64_t)param->visibility);
                w.Key("descriptors");
                w.BeginArray();
                for (const DescriptorRecord& r : records)
                {
                    const uint32_t dataId = range.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? 0 : QueueRecordData(*this, rec, r);
                    WriteDescriptorRecord(w, r, dataId);
                }
                w.EndArray();
                w.EndObject();
            }
        }
        EndSnapshot(w);
        return ",\"descriptors\":" + w.str();
    });
}

void CaptureManager::SnapshotRootView(CommandRecorder* rec, bool compute, uint32_t parameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    if (!rec)
        return;
    const std::shared_ptr<const RootSignatureInfo> ownLayout = compute ? rec->state().computeLayout : rec->state().graphicsLayout;
    ID3D12RootSignature* ownSignature = compute ? rec->state().computeRootSignature : rec->state().graphicsRootSignature;
    rec->SetSnapshotOnLast([this, compute, parameterIndex, address, ownLayout, ownSignature](CommandRecorder* rec) {
        ListState& state = rec->state();
        const std::shared_ptr<const RootSignatureInfo> layout = ownLayout ? ownLayout : compute ? state.computeLayout
                                                                                                : state.graphicsLayout;
        ID3D12RootSignature* signature = ownLayout ? ownSignature : compute ? state.computeRootSignature
                                                                            : state.graphicsRootSignature;
        const RootParameterInfo* param = layout && parameterIndex < layout->parameters.size() ? &layout->parameters[parameterIndex] : nullptr;

        JsonWriter w(&Tracker::Get());
        BeginSnapshot(w, compute, parameterIndex, nullptr, signature);
        w.BeginObject();
        w.Key("binding");
        w.Uint(0);
        w.Key("type");
        if (param)
            w.Enum(ToString_D3D12_ROOT_PARAMETER_TYPE((int64_t)param->type), (int64_t)param->type);
        else
            w.Null();
        w.Key("register");
        w.Uint(param ? param->shaderRegister : 0);
        w.Key("space");
        w.Uint(param ? param->space : 0);
        w.Key("stages");
        if (param)
            w.Enum(ToString_D3D12_SHADER_VISIBILITY((int64_t)param->visibility), (int64_t)param->visibility);
        else
            w.String("D3D12_SHADER_VISIBILITY_ALL");
        w.Key("descriptors");
        w.BeginArray();
        ID3D12Resource* buffer = nullptr;
        UINT64 offset = 0, remaining = 0;
        if (address && AddressMap::Get().Resolve(address, buffer, offset, remaining))
        {
            const uint32_t dataId = QueueBufferCapture(rec, buffer, offset, remaining);
            w.BeginObject();
            w.Key("buffer");
            WriteRef(w, buffer, "ID3D12Resource");
            w.Key("offset");
            w.Uint(offset);
            w.Key("range");
            w.Uint(remaining);
            w.Key("data");
            w.Uint(dataId);
            w.EndObject();
        }
        else
        {
            w.BeginObject();
            w.Key("buffer");
            w.Null();
            w.Key("address");
            w.String(Hex(address));
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        EndSnapshot(w);
        return ",\"descriptors\":" + w.str();
    });
}

// ---------------------------------------------------------------------------------------------
// Queues and frames

namespace
{

/** Whether an entry was also given to `list` (Entry::sharedBy); false for the entries that are never shared (a pass's timings). */
template <typename Entry>
auto SharedWith(const Entry& e, ID3D12GraphicsCommandList* list, int) -> decltype(e.sharedBy, bool())
{
    return std::find(e.sharedBy.begin(), e.sharedBy.end(), list) != e.sharedBy.end();
}
template <typename Entry>
bool SharedWith(const Entry&, ID3D12GraphicsCommandList*, long)
{
    return false;
}

/** Every capture entry recorded into `list` that has no frame yet ran in `frame`. */
template <typename Entry>
void AssignFrame(std::vector<Entry>& entries, ID3D12GraphicsCommandList* list, uint32_t frame)
{
    for (Entry& e : entries)
        if (e.frame == UINT32_MAX && (e.list == list || SharedWith(e, list, 0)))
            e.frame = frame;
}

template <typename Entry>
void RekeyList(std::vector<Entry>& entries, ID3D12GraphicsCommandList* from, ID3D12GraphicsCommandList* to)
{
    for (Entry& e : entries)
        if (e.list == from && e.frame == UINT32_MAX)
            e.list = to;
}

/** The entries a list queued but whose copies were never recorded, marked so the data is not read as if it were there. */
template <typename Entry>
void FailList(std::vector<Entry>& entries, ID3D12GraphicsCommandList* list, const char* why)
{
    for (Entry& e : entries)
        if (e.list == list && e.frame == UINT32_MAX && !e.failed)
        {
            e.failed = true;
            e.note = why;
        }
}

void NoteQueue(DeviceCapture& dc, ID3D12CommandQueue* queue)
{
    if (!queue)
        return;
    std::lock_guard lock(dc.mutex);
    if (std::find(dc.queues.begin(), dc.queues.end(), queue) != dc.queues.end())
        return;
    dc.queues.push_back(queue);
    if (!dc.frequency)
    {
        ScopedInternal internal;
        D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        UINT64 frequency = 0;
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && SUCCEEDED(queue->GetTimestampFrequency(&frequency)))
            dc.frequency = frequency;
    }
}

}  // namespace

std::vector<DeferredCopy>* CaptureManager::Impl::HeldCopiesOf(CommandRecorder* rec)
{
    const ActivePass& pass = rec->pass();
    // Closed to any work of the capture's: inside a suspended or resumed pass, and after one that
    // ended suspended (the pass is kept as it was once it ends). A pass that resumes and ends for
    // good does have room after it, but its list is one a job recorded, which does not know the
    // state the lists before it in the submission leave a resource in: a copy there was tried, and
    // its barriers were wrong. After the submission the state is the tracker's own. A texture the
    // frame reads and then overwrites (temporal anti-aliasing's history) is read back here as it
    // was written, which the draw's details show; what the frame found in it, which a replay
    // starts from, is taken before the submission (BeforeExecuteCommandLists).
    // A list already closed (a submission's own read-backs, IndexedHeapContents) can only be followed.
    const bool closed = rec->adopted() || (!rec->bundle() && rec->closed()) || (pass.split && (pass.active || pass.suspending));
    if (!rec->bundle() && !closed && !(pass.active && pass.renderPassApi))
        return nullptr;
    std::shared_lock lock(recorderMutex);
    auto it = recorders.find(rec->list());
    if (it == recorders.end() || it->second.rec.get() != rec)
        return nullptr;
    // A bundle's copies go to the list that executes it, which decides then (OnExecuteBundle).
    return !rec->bundle() && closed ? &it->second.afterSubmit : &it->second.deferred;
}

void CaptureManager::Impl::ResolveQueries(const std::vector<TimingEntry>& timings)
{
    if (timings.empty())
        return;
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard lock(deviceMutex);
        for (auto& [d, dc] : devices)
            captures.push_back(dc.get());
    }
    for (DeviceCapture* dc : captures)
    {
        if (!dc->queryReadback || !dc->timestampHeap)
            continue;
        // Only the passes a list of the captured frame ran: resolving a query that was never
        // written gives undefined data, where an unresolved slot reads as the zeros the readback
        // buffer holds, which is how "the list never ran" is told apart (SendPassTimings).
        std::vector<const TimingEntry*> mine;
        for (const TimingEntry& te : timings)
            if (te.device == dc->device && te.frame != UINT32_MAX && te.slot < kPassSlots)
                mine.push_back(&te);
        if (mine.empty())
            continue;
        // A direct queue of the device's own: the queries were made on its lists, and occlusion and
        // pipeline statistics resolve on a direct queue. The frame's work has been waited for by
        // now (Finish), so what the heaps hold is complete.
        ID3D12CommandQueue* queue = nullptr;
        {
            std::lock_guard lock(dc->mutex);
            for (ID3D12CommandQueue* q : dc->queues)
            {
                if (q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
                {
                    queue = q;
                    break;
                }
            }
        }
        ScopedInternal internal;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (!queue || FAILED(dc->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put()))) ||
            FAILED(dc->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put()))))
        {
            LogAlways("capture: the passes' queries could not be resolved, so the frame has no timings");
            continue;
        }
        // Named so that what the debug layer says about it is told apart from the application's.
        list->SetName(L"GPU Inspector: pass query resolve");
        ID3D12Resource* readback = dc->queryReadback.get();
        for (const TimingEntry* te : mine)
        {
            const uint64_t base = (uint64_t)te->slot * kSlotBytes;
            list->ResolveQueryData(dc->timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, te->slot * 2, 2, readback, base);
            if (te->statsQuery != UINT32_MAX && dc->statsHeap)
                list->ResolveQueryData(dc->statsHeap.get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, te->statsQuery, 1, readback,
                    base + kStatsOffset);
            if (te->occlusionQuery != UINT32_MAX && dc->occlusionHeap)
                list->ResolveQueryData(dc->occlusionHeap.get(), D3D12_QUERY_TYPE_OCCLUSION, te->occlusionQuery, 1, readback,
                    base + kOcclusionOffset);
        }
        if (FAILED(list->Close()))
        {
            LogAlways("capture: the list resolving the passes' queries did not close");
            continue;
        }
        ID3D12CommandList* submit[] = {list.get()};
        queue->ExecuteCommandLists(1, submit);
        // Waited for here rather than left to the caller: the results are read straight after.
        if (dc->fence && dc->event)
        {
            const uint64_t value = ++dc->fenceValue;
            if (SUCCEEDED(queue->Signal(dc->fence.get(), value)) && dc->fence->GetCompletedValue() < value &&
                SUCCEEDED(dc->fence->SetEventOnCompletion(value, dc->event)))
            {
                if (WaitForSingleObject(dc->event, 10000) != WAIT_OBJECT_0)
                    LogAlways("capture: the query resolve did not finish within 10 s; the timings may be incomplete");
            }
        }
        Log("capture: resolved the queries of %zu pass(es)", mine.size());
    }
}

void CaptureManager::Impl::ResolveLocalRoots(std::vector<BufferEntry>& buffers, std::unordered_map<std::string, std::string>& lateExtras)
{
    auto bytesOf = [&](uint32_t id, const uint8_t*& bytes, size_t& size) {
        if (!id || id > buffers.size())
            return false;
        const BufferEntry& e = buffers[id - 1];
        if (e.failed || e.frame == UINT32_MAX)
            return false;
        const uint8_t* chunk = MappedChunk(e.device, e.chunk);
        if (!chunk)
            return false;
        bytes = chunk + e.stagingOffset;
        size = (size_t)e.size;
        return true;
    };
    struct LateRead
    {
        DeviceCapture* dc;
        ID3D12Resource* buffer;
        UINT64 offset;
        UINT64 size;
        ID3D12Resource* staging;
        UINT64 stagingOffset;
    };
    std::vector<LateRead> reads;
    // A root view's range, read at the end of the frame: what the trace read, for data that does not
    // change within it, which is what a local root argument points at (materials, per-object constants).
    auto readBack = [&](ID3D12Resource* buffer, UINT64 offset, UINT64 size) -> uint32_t {
        ID3D12Device* device = DeviceOf(buffer);
        DeviceCapture* dc = device ? FindCapture(device) : nullptr;
        ResourceInfo info;
        if (!dc || !size || (ResourceTracker::Get().Get(buffer, info) && info.heapType == D3D12_HEAP_TYPE_READBACK))
            return 0;
        BufferEntry e;
        e.id = (uint32_t)buffers.size() + 1;
        e.bufferId = Tracker::Get().IdOf(buffer);
        e.frame = 0;
        e.offset = offset;
        e.size = size;
        e.device = device;
        ID3D12Resource* staging = nullptr;
        if (!AllocateStaging(*dc, D3D12_COMMAND_LIST_TYPE_DIRECT, size, e.chunk, e.stagingOffset, &staging))
            return 0;
        buffers.push_back(e);
        reads.push_back({dc, buffer, offset, size, staging, e.stagingOffset});
        return e.id;
    };
    lateExtras = ResolveLocalRootArguments(bytesOf, readBack);
    if (reads.empty())
        return;

    // Every queue has finished (Finish waited): a buffer has decayed to COMMON, from which a copy
    // promotes it, and an upload heap's is GENERIC_READ for good. Neither needs a barrier.
    std::vector<DeviceCapture*> devices;
    for (const LateRead& r : reads)
        if (std::find(devices.begin(), devices.end(), r.dc) == devices.end())
            devices.push_back(r.dc);
    for (DeviceCapture* dc : devices)
    {
        ID3D12CommandQueue* queue = nullptr;
        {
            std::lock_guard lock(dc->mutex);
            for (ID3D12CommandQueue* q : dc->queues)
            {
                if (q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
                {
                    queue = q;
                    break;
                }
            }
        }
        ScopedInternal internal;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (!queue || FAILED(dc->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put()))) ||
            FAILED(dc->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put()))))
        {
            LogAlways("capture: the buffers the traces' local root arguments name could not be read back");
            continue;
        }
        list->SetName(L"GPU Inspector: local root argument read-backs");
        for (const LateRead& r : reads)
            if (r.dc == dc)
                list->CopyBufferRegion(r.staging, r.stagingOffset, r.buffer, r.offset, r.size);
        if (FAILED(list->Close()))
            continue;
        ID3D12CommandList* submit[] = {list.get()};
        queue->ExecuteCommandLists(1, submit);
        if (dc->fence && dc->event)
        {
            const uint64_t value = ++dc->fenceValue;
            if (SUCCEEDED(queue->Signal(dc->fence.get(), value)) && dc->fence->GetCompletedValue() < value &&
                SUCCEEDED(dc->fence->SetEventOnCompletion(value, dc->event)))
                WaitForSingleObject(dc->event, 10000);
        }
        // Chunks made for these are mapped now, as the others were at the finish.
        std::lock_guard lock(dc->mutex);
        for (StagingChunk& c : dc->staging)
        {
            if (c.buffer && !c.mapped && FAILED(c.buffer->Map(0, nullptr, &c.mapped)))
                c.mapped = nullptr;
        }
    }
}

void CaptureManager::Impl::RunAfterSubmitCopies(ID3D12Device* device, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    std::vector<DeferredCopy> copies;
    {
        std::shared_lock lock(recorderMutex);
        for (UINT k = 0; k < count; ++k)
        {
            auto it = lists && lists[k] ? recorders.find(static_cast<ID3D12GraphicsCommandList*>(lists[k])) : recorders.end();
            if (it == recorders.end() || it->second.afterSubmit.empty())
                continue;
            copies.insert(copies.end(), std::make_move_iterator(it->second.afterSubmit.begin()), std::make_move_iterator(it->second.afterSubmit.end()));
            it->second.afterSubmit.clear();
        }
    }
    if (copies.empty() || !device)
        return;
    DeviceCapture* dc = CaptureFor(device);
    // The copies move a resource to COPY_SOURCE and back, which only a direct queue may do from
    // the states a draw leaves it in.
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ScopedInternal internal;
    if (!dc || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put()))))
    {
        std::lock_guard lock(mutex);
        for (UINT k = 0; k < count; ++k)
        {
            auto* l = lists ? static_cast<ID3D12GraphicsCommandList*>(lists[k]) : nullptr;
            if (!l)
                continue;
            FailList(textures, l, "inside a suspended render pass");
            FailList(buffers, l, "inside a suspended render pass");
        }
        return;
    }
    list->SetName(L"GPU Inspector: copies after a submission");
    // The list is not one of the application's, so a resource's state in it is the state the
    // submission left it in (ResourceTracker::StateIn falls back to the global state).
    for (DeferredCopy& copy : copies)
        copy(list.get());
    if (SUCCEEDED(list->Close()))
    {
        ID3D12CommandList* const submit[] = {list.get()};
        queue->ExecuteCommandLists(1, submit);
    }
    else
    {
        Log("capture: the list of copies taken after a submission did not close");
    }
    std::lock_guard lock(dc->mutex);
    dc->ownAllocators.push_back(std::move(allocator));
    dc->ownLists.push_back(std::move(list));
}

bool CaptureManager::OnExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, double cpuMs)
{
    (void)cpuMs;
    if (!queue)
        return false;
    Impl& i = impl();
    // The device this submission belongs to: every list on one queue shares it. Found from a
    // recorded list, else from the queue itself.
    ID3D12Device* device = nullptr;
    for (UINT k = 0; k < count && !device; ++k)
    {
        auto* list = lists ? static_cast<ID3D12GraphicsCommandList*>(lists[k]) : nullptr;
        if (!list)
            continue;
        CommandRecorder* rec = LookupRecorder(list);
        device = rec ? rec->device() : DeviceOf(list);
    }
    if (!device)
        device = DeviceOf(queue);

    // Record the submission (only while capturing), into the frame the home boundary is on.
    if (IsCapturing())
    {
        // The acceleration structures built before the capture began: their inputs are read back
        // behind the first submission of it, so a structure an engine built at load can still be
        // drawn (raytracing.h). Before the submission is noted below, so the reads count as its.
        if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            for (UINT k = 0; k < count; ++k)
            {
                auto* list = lists ? static_cast<ID3D12GraphicsCommandList*>(lists[k]) : nullptr;
                CommandRecorder* rec = list ? LookupRecorder(list) : nullptr;
                if (!rec || rec->bundle())
                    continue;
                ReadBackEarlierStructures(rec, CaptureSerial());
                break;
            }
        }
        Submission s;
        s.objectId = Tracker::Get().IdOf(queue);
        {
            Args a;
            a.u("NumCommandLists", count);
            JsonWriter& w = a.key("ppCommandLists");
            w.BeginArray();
            for (UINT k = 0; k < count; ++k)
                WriteRef(w, lists ? lists[k] : nullptr, "ID3D12GraphicsCommandList");
            w.EndArray();
            s.args = a.str();
        }
        std::vector<ID3D12GraphicsCommandList*> executed;
        uint64_t commands = 0;
        for (UINT k = 0; k < count; ++k)
        {
            ID3D12GraphicsCommandList* list = lists ? static_cast<ID3D12GraphicsCommandList*>(lists[k]) : nullptr;
            if (!list)
                continue;
            CommandRecorder* rec = LookupRecorder(list);
            SubmittedList sl;
            sl.listId = Tracker::Get().IdOf(list);
            if (rec)
                sl.commands = rec->Snapshot();
            commands += sl.commands ? sl.commands->size() : 1;
            s.lists.push_back(std::move(sl));
            executed.push_back(list);
            // The queue is waited for at the finish, through the fence of the device its lists belong to.
            ID3D12Device* listDevice = rec ? rec->device() : DeviceOf(list);
            if (DeviceCapture* dc = i.CaptureFor(listDevice))
                NoteQueue(*dc, queue);
        }
        // What bindless shaders read, as the heaps hold it now (before the entries below get the frame).
        s.extra = IndexedHeapContents(count, lists);
        std::lock_guard lock(i.mutex);
        if (i.state == Impl::State::Capturing)
        {
            s.frame = i.CurrentFrame();
            i.commandTotal += 1 + commands;
            for (ID3D12GraphicsCommandList* list : executed)
            {
                AssignFrame(i.textures, list, s.frame);
                AssignFrame(i.buffers, list, s.frame);
                AssignFrame(i.timings, list, s.frame);
                AssignFrame(i.draws, list, s.frame);
                AssignMeasurementFrame(list, s.frame);
            }
            i.submissions.push_back(std::move(s));
        }
    }
    // The copies the submitted lists held for now, in the frame before the capture as well: a list
    // of the captured frame may be given what one of these queued (Entry::sharedBy).
    bool takes;
    {
        std::lock_guard lock(i.mutex);
        takes = i.TakesContents();
    }
    if (takes)
        i.RunAfterSubmitCopies(device, queue, count, lists);

    // The frame boundary for a device that never presents: settled per device below, and this
    // submission ends its frame once it has. Runs whether or not a capture is active, so the
    // decision is made from the application's normal submissions and a queued "capture frame N"
    // lands on the right one.
    if (!device)
        return false;
    const BoundaryOverride override = i.Override();
    if (override == BoundaryOverride::Present)
        return false;
    bool boundary = false;
    {
        std::lock_guard lock(i.frameMutex);
        DeviceFrame& df = i.FrameFor(device);
        if (df.boundary == DeviceFrame::Boundary::Present)
            return false;   // this device presents; presents delimit it
        df.lastQueue = queue;
        const uint32_t n = ++df.submitsWithoutPresent;
        if (df.boundary == DeviceFrame::Boundary::Auto)
        {
            if (override == BoundaryOverride::Submit || n >= kSubmitsWithoutPresent)
            {
                df.boundary = DeviceFrame::Boundary::Submit;
                Log("no present after %u submissions on device %p: its frames end at every ExecuteCommandLists", n, (void*)device);
            }
        }
        boundary = df.boundary == DeviceFrame::Boundary::Submit;
    }
    if (boundary)
        EndFrame(device, queue, nullptr, false);
    return boundary;
}

std::string CaptureManager::IndexedHeapContents(UINT count, ID3D12CommandList* const* lists)
{
    Impl& i = impl();
    // Per heap, the slots to send and a list of the submission whose read-backs follow them: every
    // slot of a directly indexed heap written since the capture last sent it, and every slot a table
    // snapshot read that was rewritten after it.
    struct HeapSlots
    {
        ID3D12DescriptorHeap* heap;
        CommandRecorder* rec;
        std::map<uint32_t, DescriptorRecord> slots;
    };
    std::vector<HeapSlots> heaps;
    auto entry = [&](ID3D12DescriptorHeap* heap, CommandRecorder* rec) -> HeapSlots& {
        for (HeapSlots& h : heaps)
            if (h.heap == heap)
                return h;
        heaps.push_back({heap, rec, {}});
        return heaps.back();
    };
    std::vector<ID3D12DescriptorHeap*> indexed;
    for (UINT k = 0; k < count && lists; ++k)
    {
        CommandRecorder* rec = lists[k] ? LookupRecorder(static_cast<ID3D12GraphicsCommandList*>(lists[k])) : nullptr;
        if (!rec || rec->bundle())
            continue;
        for (ID3D12DescriptorHeap* heap : rec->state().indexed)
        {
            if (!heap || std::find(indexed.begin(), indexed.end(), heap) != indexed.end())
                continue;
            indexed.push_back(heap);
            std::vector<std::pair<uint32_t, DescriptorRecord>> changed;
            {
                std::lock_guard seen(i.heapSeenMutex);
                changed = DescriptorTracker::Get().ChangedSince(heap, i.heapSeen[heap]);
            }
            HeapSlots& h = entry(heap, rec);
            for (auto& [slot, r] : changed)
                h.slots[slot] = r;
        }
        for (const CommandRecorder::TableSlots& t : rec->tableSlots())
        {
            for (auto& [slot, r] : DescriptorTracker::Get().ChangedFrom(t.heap, t.first, t.writes))
                entry(t.heap, rec).slots[slot] = r;
        }
    }
    if (std::all_of(heaps.begin(), heaps.end(), [](const HeapSlots& h) { return h.slots.empty(); }))
        return {};
    JsonWriter w(&Tracker::Get());
    w.BeginArray();
    for (const HeapSlots& h : heaps)
    {
        ID3D12DescriptorHeap* heap = h.heap;
        CommandRecorder* rec = h.rec;
        HeapInfo info;
        if (h.slots.empty() || !DescriptorTracker::Get().GetHeap(heap, info))
            continue;
        const auto& changed = h.slots;
        const bool samplers = info.desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        w.BeginObject();
        w.Key("heap");
        WriteRef(w, heap, "ID3D12DescriptorHeap");
        w.Key("slots");
        w.BeginArray();
        for (const auto& [slot, r] : changed)
        {
            D3D12_DESCRIPTOR_RANGE_TYPE type;
            switch (r.kind)
            {
                case DescriptorKind::CBV: type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; break;
                case DescriptorKind::SRV: type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; break;
                case DescriptorKind::UAV: type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; break;
                case DescriptorKind::Sampler: type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; break;
                default: continue;
            }
            // A bindless heap keeps views of resources the application has since released, in slots
            // it no longer indexes: those name nothing, and are left out.
            D3D12_RESOURCE_DESC desc{};
            if (r.resource && !DescOf(r.resource, desc))
                continue;
            w.BeginObject();
            w.Key("slot");
            w.Uint(slot);
            w.Key("type");
            w.Uint((uint32_t)type);
            w.Key("descriptor");
            WriteDescriptorRecord(w, r, samplers ? 0 : QueueRecordData(*this, rec, r));
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    return ",\"heapDescriptors\":" + w.str();
}

void CaptureManager::OnExecuteBundle(CommandRecorder* rec, ID3D12GraphicsCommandList* bundle)
{
    if (!rec || !bundle)
        return;
    Impl& i = impl();
    CommandRecorder* bundleRec = LookupRecorder(bundle);
    if (!bundleRec)
        return;
    // A bundle's draws index the heaps of the list that executes it, under its own root
    // signature or the list's (a bundle that sets none inherits it).
    for (int k = 0; k < 2; ++k)
        if (bundleRec->state().indexed[k] && rec->state().heaps[k])
            rec->state().indexed[k] = rec->state().heaps[k];
    NoteIndexedHeaps(rec, false);
    NoteIndexedHeaps(rec, true);
    // The bundle's read-back copies (a bundle records none itself) go into the executing list, and
    // its entries count as this list's for the frame they run in.
    if (std::vector<DeferredCopy>* deferred = i.DeferredOf(bundleRec))
    {
        std::vector<DeferredCopy> pending;
        pending.swap(*deferred);
        if (!pending.empty())
        {
            {
                std::lock_guard lock(i.mutex);
                RekeyList(i.textures, bundle, rec->list());
                RekeyList(i.buffers, bundle, rec->list());
            }
            // Inside a pass they are held as this list's own are: for the pass's end, or for after
            // the submission when the pass is suspended (HeldCopiesOf).
            if (std::vector<DeferredCopy>* mine = i.HeldCopiesOf(rec))
            {
                mine->insert(mine->end(), std::make_move_iterator(pending.begin()), std::make_move_iterator(pending.end()));
            }
            else
            {
                for (auto& fn : pending)
                    fn(rec->list());
            }
        }
    }
    std::shared_ptr<const CommandList> commands = bundleRec->Snapshot();
    if (!commands || commands->empty())
        return;
    // What the bundle binds is read when it runs, which is now: during a capture its snapshots are
    // taken again, their copies recorded into this list (or held for its pass's end).
    bool capturing = false;
    {
        std::lock_guard lock(i.mutex);
        capturing = i.state == Impl::State::Capturing;
    }
    JsonWriter w;
    w.BeginArray();
    w.BeginObject();
    w.Key("commandBuffer");
    w.Uint(Tracker::Get().IdOf(bundle));
    w.Key("commands");
    w.BeginArray();
    uint32_t slot = 0;
    for (const RecordedCommand& c : *commands)
    {
        w.BeginObject();
        w.Key("method");
        w.String(c.method);
        w.Key("args");
        if (c.args.empty())
            w.Null();
        else
            w.Raw(c.args);
        w.Key("slot");
        w.Uint(slot++);
        // A pre-separated member list: ,"descriptors":{...}
        if (capturing && c.refresh)
            w.str() += c.extra.substr(0, c.refreshFrom) + (*c.refresh)(rec);
        else if (!c.extra.empty())
            w.str() += c.extra;
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndArray();
    rec->SetExtraOnLast(",\"children\":" + w.str());
}

void CaptureManager::OnPresent(ID3D12Device* device, IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    Impl& i = impl();
    // DXINSP_FRAME_BOUNDARY=submit delimits every device by its submissions and ignores presents
    // for framing (the Chrome case: the compositor may present on a hooked device, but the work to
    // capture is Dawn's, which never presents). The present still updated the frame timing.
    if (i.Override() == BoundaryOverride::Submit)
        return;
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

void CaptureManager::EndFrame(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* swapChain, bool present)
{
    if (!device)
        return;
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
    DrawOverlayRequest drawOverlay;
    MeshOutputRequest meshOutput;
    uint64_t maxTextureSize = 0;
    {
        std::lock_guard lock(i.mutex);
        if (i.state == Impl::State::Armed)
        {
            // A process that presents somewhere has its frames ended by those presents: a
            // background device's substitute submit boundary does not start the capture, unless
            // DXINSP_FRAME_BOUNDARY=submit forces it (the Chrome case, where the compositor may
            // present on a hooked device but the WebGPU work to capture is Dawn's, which does not).
            if (!present && i.presentSeenAny && override != BoundaryOverride::Submit)
                return;
            // A capture queued at a later frame starts recording one frame before it, so that the
            // lists the target frame executes are recorded wherever the engine records them (see
            // RequestCapture). Nothing is kept: the recorders are reset as the lists are.
            if (i.options.atFrame != UINT64_MAX && deviceFrameIndex + 1 >= i.options.atFrame)
            {
                _recordActive.store(true, std::memory_order_relaxed);
                i.warmingUp = true;
            }
            // One frame of recording before the captured one, so that its lists were recorded whole.
            if (i.warmupBoundaries)
            {
                --i.warmupBoundaries;
                return;
            }
            if (i.options.atFrame == UINT64_MAX || deviceFrameIndex >= i.options.atFrame)
            {
                i.state = Impl::State::Capturing;
                i.frameIndex = deviceFrame;
                i.framesDone = 0;
                i.homeDevice = device;
                i.homeSwapChain = present ? swapChain : nullptr;   // null: the home is delimited by submits
                i.submissions.clear();
                // The contents the frame before queued stay (TakesContents), but nothing queued from
                // here on is answered with one of them: a range read again is read again, since
                // what it held a frame ago is not what this frame's draws see. The budgets start
                // over with it, so the frame before does not spend the captured frame's.
                i.warmingUp = false;
                // The passes the warm-up frame timed stay for the same reason its read-backs do:
                // an engine that records ahead recorded the captured frame's lists then, and their
                // queries are in those lists. An entry no list of this frame runs keeps its
                // UINT32_MAX frame and is not sent (SendPassTimings). Cleared when the capture is
                // armed instead (RequestCapture).
                i.bufferIds.clear();
                i.textureIds.clear();
                i.initialIds.clear();
                i.frameWritten.clear();
                i.bufferBytes = i.imageBytes = i.targetBytes = i.commandTotal = 0;
                i.splitPasses = 0;
                // The host calls the frame spends its time in, from here until Finish
                // (cpu_timeline.h).
                BeginCpuTimeline();
                _captureSerial.fetch_add(1, std::memory_order_acq_rel);
                _capturing.store(true, std::memory_order_release);
                _recordActive.store(true, std::memory_order_relaxed);
                started = true;
                overdraw = i.options.overdraw;
                pixelHistory = i.options.pixelHistory;
                drawOverlay = i.options.drawOverlay;
                meshOutput = i.options.meshOutput;
                maxTextureSize = i.options.maxTextureSize;
            }
        }
        else if (i.state == Impl::State::Capturing)
        {
            if (present)
            {
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
            if (home)
            {
                i.framesDone++;
                if (i.framesDone >= i.frameCount)
                    finish = true;
            }
        }
    }
    if (started)
    {
        // What the capture measures while it records (overdraw.h), with nothing left from the last one.
        StartMeasurements(overdraw, pixelHistory, drawOverlay, meshOutput, maxTextureSize);
        // The counters started over when the capture was armed (RequestCapture), since the warm-up
        // frame reserves the slots of the passes it records; the heaps themselves stay.
        std::lock_guard lock(i.deviceMutex);
        for (auto& [d, dc] : i.devices)
        {
            // The last capture's staging, which its lists have long since stopped naming.
            std::lock_guard dlock(dc->mutex);
            ScopedInternal internal;
            dc->retiredStaging.clear();
            dc->retiredResolves.clear();
        }
        Log("capture started at frame %llu (%u frame(s), %s)", (unsigned long long)deviceFrame, i.frameCount,
            present ? "present" : "submit boundary");
        return;
    }
    if (IsCapturing())
    {
        if (DeviceCapture* dc = i.CaptureFor(device))
            NoteQueue(*dc, queue);
    }
    if (finish)
        i.Finish(*this, device);
}

// ---------------------------------------------------------------------------------------------
// Finish: wait for the GPU, map, send, release

namespace
{

struct CaptureData
{
    uint64_t frameIndex = 0;
    uint32_t frameCount = 1;
    uint64_t commandTotal = 0;
    std::vector<Submission> submissions;
    std::vector<TextureEntry> textures;
    std::vector<BufferEntry> buffers;
    std::vector<TimingEntry> timings;
    std::vector<DrawEntry> draws;
    /** Members a trace's command carries beyond what it recorded: its local root arguments (ResolveLocalRootArguments), by its extras. */
    std::unordered_map<std::string, std::string> lateExtras;
};

void WriteCommandEntry(JsonWriter& w, uint64_t index, uint32_t frame, int64_t slot, const char* method, const char* objectClass,
    uint64_t objectId, const std::string& args, const std::string& extra)
{
    w.BeginObject();
    w.Key("index");
    w.Uint(index);
    w.Key("frame");
    w.Uint(frame);
    // Position within the list's recording: what validation messages refer to.
    if (slot >= 0)
    {
        w.Key("slot");
        w.Uint((uint64_t)slot);
    }
    w.Key("method");
    w.String(method);
    w.Key("object");
    if (objectId)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"__id\":%llu,\"__class\":\"%s\"}", (unsigned long long)objectId, objectClass);
        w.Raw(buf);
    }
    else
    {
        w.Null();
    }
    w.Key("args");
    if (args.empty())
        w.Null();
    else
        w.Raw(args);
    if (!extra.empty())
        w.str() += extra;   // a pre-separated member list: ,"descriptors":{...},"stack":[...]
    w.EndObject();
}

void SendCommands(const CaptureData& data)
{
    Transport& t = Transport::Get();
    uint64_t total = 0;
    for (const Submission& s : data.submissions)
    {
        total += 1;
        for (const SubmittedList& l : s.lists)
            total += l.commands ? l.commands->size() : 1;
    }
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("action");
        w.String("CaptureFrameResults");
        w.Key("frame");
        w.Uint(data.frameIndex);
        w.Key("frames");
        w.Uint(data.frameCount);
        w.Key("count");
        w.Uint(total);
        w.Key("batches");
        w.Uint((total + kCommandBatch - 1) / kCommandBatch);
        w.Key("api");
        w.String("d3d12");
        w.EndObject();
        t.SendJson(std::move(w.str()));
    }
    // Flatten: the submission's entry, then each of its lists' commands, in submission order; a
    // frame's Present closes it.
    uint64_t index = 0;
    JsonWriter batch;
    size_t inBatch = 0;
    auto flush = [&]() {
        if (!inBatch)
            return;
        batch.EndArray();
        batch.EndObject();
        t.SendJson(std::move(batch.str()));
        batch.Reset();
        inBatch = 0;
    };
    auto emit = [&](uint32_t frame, int64_t slot, const char* method, const char* cls, uint64_t objectId, const std::string& args,
                    const std::string& extra) {
        if (!inBatch)
        {
            batch.BeginObject();
            batch.Key("action");
            batch.String("CaptureFrameCommands");
            batch.Key("frame");
            batch.Uint(data.frameIndex);
            batch.Key("index");
            batch.Uint(index);
            batch.Key("commands");
            batch.BeginArray();
        }
        WriteCommandEntry(batch, index++, frame, slot, method, cls, objectId, args, extra);
        if (++inBatch >= kCommandBatch)
            flush();
    };
    static const std::string kNone;
    uint32_t unrecorded = 0;
    for (const Submission& s : data.submissions)
    {
        if (s.present)
        {
            emit(s.frame, -1, "Present", "IDXGISwapChain", s.objectId, s.args, kNone);
            continue;
        }
        emit(s.frame, -1, "ExecuteCommandLists", "ID3D12CommandQueue", s.objectId, s.args, s.extra);
        for (const SubmittedList& l : s.lists)
        {
            if (!l.commands)
            {
                ++unrecorded;
                emit(s.frame, -1, "<unrecorded command list>", "ID3D12GraphicsCommandList", l.listId, kNone, kNone);
                continue;
            }
            int64_t slot = 0;
            for (const RecordedCommand& c : *l.commands)
            {
                // A trace's local root arguments were resolved at the finish, after it was recorded.
                if (!data.lateExtras.empty() && c.method == "DispatchRays")
                {
                    auto late = std::find_if(data.lateExtras.begin(), data.lateExtras.end(),
                        [&](const auto& e) { return c.extra.find(e.first) != std::string::npos; });
                    if (late != data.lateExtras.end())
                    {
                        emit(s.frame, slot++, c.method.c_str(), "ID3D12GraphicsCommandList", l.listId, c.args, c.extra + late->second);
                        continue;
                    }
                }
                emit(s.frame, slot++, c.method.c_str(), "ID3D12GraphicsCommandList", l.listId, c.args, c.extra);
            }
        }
    }
    flush();
    // A list whose commands nothing recorded was recorded before the capture asked for anything —
    // earlier than the frame before it, which is where recording starts (RequestCapture). Saying so
    // is the difference between a capture that looks empty and one that says what to do about it.
    if (unrecorded)
        Log("capture: %u submitted command list(s) were recorded before the capture began and hold no commands; "
            "turn on \"Record all command buffers\" to capture them",
            unrecorded);
}

}  // namespace

/** The mapped staging chunk an entry's data is in, or null. */
const uint8_t* CaptureManager::Impl::MappedChunk(ID3D12Device* device, uint32_t chunk)
{
    DeviceCapture* dc = FindCapture(device);
    if (!dc)
        return nullptr;
    std::lock_guard lock(dc->mutex);
    if (chunk >= dc->staging.size())
        return nullptr;
    return static_cast<const uint8_t*>(dc->staging[chunk].mapped);
}

void CaptureManager::Impl::SendTextures(std::vector<TextureEntry>& textures)
{
    Transport& t = Transport::Get();
    // What no list of the capture ran is not the capture's, and nothing in it refers to it: what the
    // frame before queued in lists it ran itself, what the captured frame queued in the lists an
    // engine records ahead for the next one, and what a pooled list queued when it was recorded
    // again after running. On a Unity player that was a third of the buffers, each reported as a
    // failed read-back, "command list was not executed during the capture".
    textures.erase(std::remove_if(textures.begin(), textures.end(), [](const TextureEntry& e) { return e.frame == UINT32_MAX; }), textures.end());
    for (TextureEntry& e : textures)
    {
        if (!e.failed && !MappedChunk(e.device, e.chunk))
        {
            e.failed = true;
            e.note = "staging buffer could not be mapped";
        }
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("CaptureTextureFrames");
    w.Key("count");
    w.Uint(textures.size());
    w.Key("textures");
    w.BeginArray();
    for (const TextureEntry& e : textures)
    {
        w.BeginObject();
        w.Key("id");
        w.Uint(e.resourceId);
        w.Key("frame");
        w.Uint(e.frame);
        w.Key("commandBuffer");
        w.Uint(e.listId);
        w.Key("passIndex");
        w.Uint(e.passIndex);
        w.Key("attachment");
        w.Uint(e.attachment);
        w.Key("format");
        w.String(e.format);
        w.Key("aspect");
        w.String(e.depthAspect ? "depth" : e.stencilAspect ? "stencil"
                                                           : "color");
        w.Key("width");
        w.Uint(e.width);
        w.Key("height");
        w.Uint(e.height);
        w.Key("depth");
        w.Uint(e.depth);
        w.Key("layers");
        w.Uint(e.layers);
        w.Key("mip");
        w.Uint(e.mip);
        if (e.mips > 1)
        {
            w.Key("mips");
            w.Uint(e.mips);
        }
        w.Key("size");
        w.Uint(e.failed ? 0 : e.size);
        if (e.samples > 1)
        {
            w.Key("samples");
            w.Uint(e.samples);
        }
        if (e.sampled || e.initial)
        {
            w.Key("kind");
            w.String(e.initial ? "initial" : "sampled");
            w.Key("capture");
            w.Uint(e.captureId);
            w.Key("baseLayer");
            w.Uint(0);
        }
        if (e.failed)
        {
            w.Key("error");
            w.String(e.note);
        }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    t.SendJson(std::move(w.str()));

    std::vector<uint8_t> packed;
    for (const TextureEntry& e : textures)
    {
        if (e.failed)
            continue;
        const uint8_t* mapped = MappedChunk(e.device, e.chunk);
        if (!mapped)
            continue;
        // Rows come out of the staging buffer at the copy's 256-byte pitch; the UI wants them tight.
        packed.assign((size_t)e.size, 0);
        size_t at = 0;
        for (const StagedRegion& r : e.regions)
        {
            const uint64_t slicePitch = (uint64_t)r.rowPitch * r.rows;
            for (uint32_t s = 0; s < r.slices; ++s)
            {
                for (uint32_t row = 0; row < r.rows; ++row)
                {
                    if (at + r.rowBytes > packed.size())
                        break;
                    memcpy(packed.data() + at, mapped + r.offset + s * slicePitch + (uint64_t)row * r.rowPitch, (size_t)r.rowBytes);
                    at += (size_t)r.rowBytes;
                }
            }
        }
        JsonWriter h;
        h.BeginObject();
        h.Key("action");
        h.String("CaptureTextureData");
        h.Key("id");
        h.Uint(e.resourceId);
        h.Key("frame");
        h.Uint(e.frame);
        h.Key("commandBuffer");
        h.Uint(e.listId);
        h.Key("passIndex");
        h.Uint(e.passIndex);
        h.Key("attachment");
        h.Uint(e.attachment);
        // A depth-stencil target has an entry per aspect under the same attachment index.
        h.Key("aspect");
        h.String(e.depthAspect ? "depth" : e.stencilAspect ? "stencil"
                                                           : "color");
        // What names the data's entry: a sampled or initial one shares its key with a target's read-back.
        if (e.sampled || e.initial)
        {
            h.Key("capture");
            h.Uint(e.captureId);
        }
        h.Key("size");
        h.Uint(e.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), packed.data(), packed.size());
    }
}

void CaptureManager::Impl::SendBuffers(std::vector<BufferEntry>& buffers)
{
    Transport& t = Transport::Get();
    // What no list of the capture ran is not the capture's, and nothing in it refers to it: what the
    // frame before queued in lists it ran itself, what the captured frame queued in the lists an
    // engine records ahead for the next one, and what a pooled list queued when it was recorded
    // again after running. On a Unity player that was a third of the buffers, each reported as a
    // failed read-back, "command list was not executed during the capture".
    buffers.erase(std::remove_if(buffers.begin(), buffers.end(), [](const BufferEntry& e) { return e.frame == UINT32_MAX; }), buffers.end());
    for (BufferEntry& e : buffers)
    {
        if (!e.failed && !MappedChunk(e.device, e.chunk))
        {
            e.failed = true;
            e.note = "staging buffer could not be mapped";
        }
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("CaptureBuffers");
    w.Key("count");
    w.Uint(buffers.size());
    w.Key("buffers");
    w.BeginArray();
    for (const BufferEntry& e : buffers)
    {
        w.BeginObject();
        w.Key("id");
        w.Uint(e.id);
        w.Key("buffer");
        w.Uint(e.bufferId);
        w.Key("frame");
        w.Uint(e.frame);
        w.Key("commandBuffer");
        w.Uint(e.listId);
        w.Key("offset");
        w.Uint(e.offset);
        w.Key("size");
        w.Uint(e.failed ? 0 : e.size);
        if (e.originalSize)
        {
            w.Key("originalSize");
            w.Uint(e.originalSize);
        }
        if (e.failed)
        {
            w.Key("error");
            w.String(e.note);
        }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    t.SendJson(std::move(w.str()));
    for (const BufferEntry& e : buffers)
    {
        if (e.failed)
            continue;
        const uint8_t* mapped = MappedChunk(e.device, e.chunk);
        if (!mapped)
            continue;
        JsonWriter h;
        h.BeginObject();
        h.Key("action");
        h.String("CaptureBufferData");
        h.Key("id");
        h.Uint(e.id);
        h.Key("size");
        h.Uint(e.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), mapped + e.stagingOffset, (size_t)e.size);
    }
}

/**
 * A direct queue of the device, for GetClockCalibration. Any queue can calibrate, but the pass
 * timestamps were resolved on a direct one and a copy queue may run on a different clock.
 */
ID3D12CommandQueue* CaptureManager::Impl::CalibrationQueue(ID3D12Device* device)
{
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard lock(deviceMutex);
        for (auto& [d, dc] : devices)
            captures.push_back(dc.get());
    }
    ID3D12CommandQueue* fallback = nullptr;
    for (DeviceCapture* dc : captures)
    {
        std::lock_guard lock(dc->mutex);
        for (ID3D12CommandQueue* q : dc->queues)
        {
            if (!q)
                continue;
            if (dc->device == device)
                return q;
            if (!fallback)
                fallback = q;
        }
    }
    return fallback;
}

void CaptureManager::Impl::SendPassTimings(const std::vector<TimingEntry>& timings, ID3D12Device* home)
{
    if (timings.empty())
        return;
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard lock(deviceMutex);
        for (auto& [d, dc] : devices)
            if (dc->queryMapped)
                captures.push_back(dc.get());
    }
    if (captures.empty())
        return;
    // The period of the home device's clock; each device's passes are measured on its own clock,
    // from the earliest pass it timed.
    uint64_t frequency = 0;
    for (DeviceCapture* dc : captures)
        if (dc->device == home && dc->frequency)
            frequency = dc->frequency;
    if (!frequency)
        for (DeviceCapture* dc : captures)
            if (dc->frequency)
            {
                frequency = dc->frequency;
                break;
            }
    JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("CapturePassTimings");
    w.Key("timestampPeriodNs");
    w.Double(frequency ? 1e9 / (double)frequency : 0.0);
    w.Key("passes");
    w.BeginArray();
    uint32_t sent = 0, counted = 0;
    // The tick every pass start is measured from, on the home device: with the clock
    // calibration (cpu_timeline.h) this is what places a pass beside the CPU events that
    // submitted it. Only the home device has a calibrated clock, so only its origin is sent.
    uint64_t originTicks = 0;
    for (DeviceCapture* dc : captures)
    {
        const double freq = (double)(dc->frequency ? dc->frequency : frequency);
        if (freq <= 0)
            continue;
        const uint8_t* results = static_cast<const uint8_t*>(dc->queryMapped);
        auto stamps = [&](const TimingEntry& te, uint64_t& begin, uint64_t& end) {
            if (te.device != dc->device || te.frame == UINT32_MAX || te.slot >= kPassSlots)
                return false;
            memcpy(&begin, results + te.slot * kSlotBytes, 8);
            memcpy(&end, results + te.slot * kSlotBytes + 8, 8);
            // Both zero: the list never ran (a readback heap starts zeroed and nothing resolved into the slot).
            return !(begin == 0 && end == 0) && end >= begin;
        };
        uint64_t earliest = UINT64_MAX;
        for (const TimingEntry& te : timings)
        {
            uint64_t b, e;
            if (stamps(te, b, e))
                earliest = std::min(earliest, b);
        }
        if (dc->device == home && earliest != UINT64_MAX)
            originTicks = earliest;
        for (const TimingEntry& te : timings)
        {
            uint64_t begin, end;
            if (!stamps(te, begin, end))
                continue;
            w.BeginObject();
            w.Key("frame");
            w.Uint(te.frame);
            w.Key("commandBuffer");
            w.Uint(te.listId);
            w.Key("passIndex");
            w.Uint(te.passIndex);
            if (te.compute)
            {
                w.Key("kind");
                w.String("compute");
            }
            w.Key("startMs");
            w.Double((double)(begin - earliest) / freq * 1e3);
            w.Key("durationMs");
            w.Double((double)(end - begin) / freq * 1e3);
            if (te.hasStats || te.hasOcclusion)
            {
                w.Key("counters");
                w.BeginObject();
                if (te.hasStats)
                {
                    // The Vulkan layer's names for the same quantities (pipeline_stats.cpp), which the
                    // bottleneck rules read.
                    D3D12_QUERY_DATA_PIPELINE_STATISTICS stats;
                    memcpy(&stats, results + te.slot * kSlotBytes + kStatsOffset, sizeof(stats));
                    w.Key("inputAssemblyVertices");
                    w.Uint(stats.IAVertices);
                    w.Key("inputAssemblyPrimitives");
                    w.Uint(stats.IAPrimitives);
                    w.Key("vertexInvocations");
                    w.Uint(stats.VSInvocations);
                    w.Key("clipperInvocations");
                    w.Uint(stats.CInvocations);
                    w.Key("clipperPrimitivesOut");
                    w.Uint(stats.CPrimitives);
                    w.Key("fragmentInvocations");
                    w.Uint(stats.PSInvocations);
                }
                if (te.hasOcclusion)
                {
                    uint64_t passed = 0;
                    memcpy(&passed, results + te.slot * kSlotBytes + kOcclusionOffset, 8);
                    w.Key("fragmentsPassed");
                    w.Uint(passed);
                }
                w.EndObject();
                counted++;
            }
            w.EndObject();
            sent++;
        }
    }
    w.EndArray();
    w.Key("count");
    w.Uint(sent);
    if (originTicks)
    {
        w.Key("originTicks");
        w.Uint(originTicks);
    }
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    Log("pass profiling: %u of %zu passes timed, %u with counters", sent, timings.size(), counted);
}

/**
 * Every draw and dispatch the capture measured, in the shape `vkinsp_replay --draws` writes for a
 * Vulkan capture (src/replay/src/draw_stats.cpp, draw_stats.ts): a time and the counters, keyed by
 * the command the draw is in the capture's own list.
 *
 * As there, a draw's time is not what that draw costs on its own -- the GPU pipelines consecutive
 * draws, so their spans overlap and add up to more than the pass takes. It says what share of a
 * pass a draw accounts for, which is what the Shader Flame Graph splits a pass's duration by. The
 * counters are exact.
 */
void CaptureManager::Impl::SendDrawStats(const std::vector<DrawEntry>& draws, ID3D12Device* home)
{
    if (draws.empty())
        return;
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard lock(deviceMutex);
        for (auto& [d, dc] : devices)
            captures.push_back(dc.get());
    }
    uint64_t frequency = 0;
    for (DeviceCapture* dc : captures)
        if (dc->device == home && dc->frequency)
            frequency = dc->frequency;
    if (!frequency)
        for (DeviceCapture* dc : captures)
            if (dc->frequency)
            {
                frequency = dc->frequency;
                break;
            }

    JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("CaptureDrawStats");
    w.Key("draws");
    w.BeginArray();
    uint32_t sent = 0, timed = 0, counted = 0;
    for (DeviceCapture* dc : captures)
    {
        const double freq = (double)(dc->frequency ? dc->frequency : frequency);
        const uint8_t* results = static_cast<const uint8_t*>(dc->drawMapped);
        if (!results || freq <= 0)
            continue;
        for (const DrawEntry& de : draws)
        {
            if (de.device != dc->device || de.frame == UINT32_MAX || de.slot >= kDrawSlots)
                continue;
            uint64_t begin = 0, end = 0;
            memcpy(&begin, results + kDrawTimestampBase + (uint64_t)de.slot * kDrawTimestampBytes, 8);
            memcpy(&end, results + kDrawTimestampBase + (uint64_t)de.slot * kDrawTimestampBytes + 8, 8);
            // Both zero: the list never ran, so nothing resolved into the slot (the buffer starts zeroed).
            const bool hasTime = !(begin == 0 && end == 0) && end >= begin;
            D3D12_QUERY_DATA_PIPELINE_STATISTICS stats{};
            if (de.hasStats)
                memcpy(&stats, results + kDrawStatsBase + (uint64_t)de.slot * kDrawStatsBytes, sizeof(stats));
            uint64_t passed = 0;
            if (de.hasOcclusion)
                memcpy(&passed, results + kDrawOcclusionBase + (uint64_t)de.slot * kDrawOcclusionBytes, 8);
            w.BeginObject();
            w.Key("command");
            w.Uint(de.command);
            w.Key("frame");
            w.Uint(de.frame);
            w.Key("commandBuffer");
            w.Uint(de.listId);
            // The sentinel draw_stats.ts reads as "in no render pass".
            w.Key("passIndex");
            w.Uint(de.passIndex);
            w.Key("timed");
            w.Boolean(hasTime);
            w.Key("ms");
            w.Double(hasTime ? (double)(end - begin) / freq * 1e3 : 0.0);
            w.Key("counted");
            w.Boolean(de.hasStats);
            w.Key("vertexInvocations");
            w.Uint(stats.VSInvocations);
            w.Key("primitives");
            w.Uint(stats.IAPrimitives);
            w.Key("fragmentInvocations");
            w.Uint(stats.PSInvocations);
            w.Key("computeInvocations");
            w.Uint(stats.CSInvocations);
            w.Key("sampled");
            w.Boolean(de.hasOcclusion);
            w.Key("samplesPassed");
            w.Uint(passed);
            w.EndObject();
            sent++;
            if (hasTime)
                timed++;
            if (de.hasStats)
                counted++;
        }
    }
    w.EndArray();
    w.Key("count");
    w.Uint(sent);
    // What the measurement could not reach, in the words the UI shows above the numbers.
    std::string note;
    uint32_t overflowed = 0;
    for (DeviceCapture* dc : captures)
    {
        const uint32_t used = dc->drawSlotsUsed.load(std::memory_order_relaxed);
        if (used > kDrawSlots)
            overflowed = std::max(overflowed, used - kDrawSlots);
    }
    if (overflowed)
        note = "the first " + std::to_string(kDrawSlots) + " draws of the frame were measured; " +
            std::to_string(overflowed) + " more were not";
    if (sent && !counted)
        note += std::string(note.empty() ? "" : "; ") +
            "the draws were timed but not counted: statistics queries are not taken inside a BeginRenderPass region";
    if (!note.empty())
    {
        w.Key("note");
        w.String(note);
    }
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    Log("draw profiling: %u of %zu draws sent, %u timed, %u with counters", sent, draws.size(), timed, counted);
}

void CaptureManager::Impl::Finish(CaptureManager& cm, ID3D12Device* device)
{
    // Finish runs on the thread that presented, so the application is stopped for as long as it
    // takes: how long that was is the first thing to know when a capture of a large frame seems to
    // hang, and it says whether the wait is here or in the client reading it (docs/ARCHITECTURE.md).
    const auto finishBegan = std::chrono::steady_clock::now();
    cm._capturing.store(false, std::memory_order_release);
    // A capture asked for while the application was paused was let through the frames it needed
    // rather than resuming it (frame_pause.h). This is the frame boundary it ends on -- the
    // present that ended the last captured frame waits straight after this -- so releasing the
    // hold here leaves the application frozen on the frame the capture holds.
    gpuinsp::FramePause::Get().ReleaseCaptureHold();
    cm._recordActive.store(cm.RecordAlways(), std::memory_order_relaxed);
    CaptureData data;
    uint32_t splitPassCount = 0;
    {
        std::lock_guard lock(mutex);
        data.frameIndex = frameIndex;
        data.frameCount = frameCount;
        data.commandTotal = commandTotal;
        data.submissions.swap(submissions);
        data.textures.swap(textures);
        data.buffers.swap(buffers);
        data.timings.swap(timings);
        data.draws.swap(draws);
        pendingDrawSlots.clear();
        bufferIds.clear();
        textureIds.clear();
        initialIds.clear();
        frameWritten.clear();
        bufferBytes = imageBytes = targetBytes = commandTotal = 0;
        splitPassCount = splitPasses;
        splitPasses = 0;
        homeSwapChain = nullptr;
        homeDevice = nullptr;   // presentSeenAny is a device-lifetime fact and is not reset here
        state = Impl::State::Idle;
    }
    Log("capture finishing: %zu submissions, %llu commands, %zu textures, %zu buffers, %zu passes", data.submissions.size(),
        (unsigned long long)data.commandTotal, data.textures.size(), data.buffers.size(), data.timings.size());
    // How much of the frame was measured through lists recorded before it: an engine that records
    // ahead (Unity) builds the captured frame's lists during the warm-up frame, and their queries
    // went in as they were recorded (BeginPass). Worth saying, because it is the difference between
    // a measured frame and an unmeasured one, and it is invisible from the capture itself.
    {
        uint32_t fromBefore = 0, unused = 0;
        for (const TimingEntry& te : data.timings)
        {
            if (!te.warmup)
                continue;
            if (te.frame != UINT32_MAX)
                ++fromBefore;
            else
                ++unused;
        }
        if (fromBefore || unused)
            Log("capture: %u pass(es) were timed in lists the application recorded before the captured frame; "
                "%u more were timed in lists it did not run", fromBefore, unused);
    }
    // A pass the application suspends across command lists is recorded whole and measured not at
    // all: between the suspension and its resume nothing may be added to the list (ActivePass::split).
    if (splitPassCount)
        LogAlways("capture: %u render pass segment(s) were suspended across command lists; they are timed, but their render targets were not read back",
            splitPassCount);

    // Everything recorded in the frames has been executed; wait for it on every queue that took
    // part, so the staging buffers and the query results are complete.
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard lock(deviceMutex);
        for (auto& [d, dc] : devices)
            captures.push_back(dc.get());
    }
    for (DeviceCapture* dc : captures)
    {
        std::vector<ID3D12CommandQueue*> queues;
        {
            std::lock_guard lock(dc->mutex);
            queues = dc->queues;
        }
        if (!dc->fence || !dc->event)
            continue;
        ScopedInternal internal;
        for (ID3D12CommandQueue* q : queues)
        {
            // A queue the application released mid-capture would be a dangling pointer here; the
            // application cannot release a queue it presented on or executed lists on this frame
            // without waiting for them, so this is not guarded against.
            const uint64_t value = ++dc->fenceValue;
            if (FAILED(q->Signal(dc->fence.get(), value)))
                continue;
            if (dc->fence->GetCompletedValue() >= value)
                continue;
            if (FAILED(dc->fence->SetEventOnCompletion(value, dc->event)))
                continue;
            if (WaitForSingleObject(dc->event, 10000) != WAIT_OBJECT_0)
                LogAlways("capture: the GPU did not finish within 10 s; read-backs may be incomplete");
        }
        // Map the staging chunks for the sends.
        std::lock_guard lock(dc->mutex);
        for (StagingChunk& c : dc->staging)
        {
            if (!c.buffer || c.mapped)
                continue;
            if (FAILED(c.buffer->Map(0, nullptr, &c.mapped)))
                c.mapped = nullptr;
        }
    }

    // The passes' queries, resolved from a list of the capture's own now that every queue has
    // finished: nothing of the capture's is resolved inside the application's lists, which is what
    // lets a render pass suspended across command lists be timed at all (EndPassQueries).
    ResolveQueries(data.timings);
    ResolveLocalRoots(data.buffers, data.lateExtras);

    SendCommands(data);
    SendTextures(data.textures);
    SendBuffers(data.buffers);
    SendPassTimings(data.timings, device);
    SendDrawStats(data.draws, device);
    // The GPU clock related to the host's, while the queue is still alive, then the CPU events.
    SampleCalibration(CalibrationQueue(device));
    SendCpuTimeline();
    // The measurements taken while the frames were recorded, between the timings and
    // CaptureComplete (docs/ARCHITECTURE.md, "Frame capture").
    SendOverdraw();
    SendPixelHistory();
    SendDrawOverlay();
    SendMeshOutput();
    // Read-backs that failed for a reason other than the capture's own limits are worth a line
    // in the validation view, where the user looks for what went wrong.
    {
        uint32_t failed = 0;
        std::string first;
        auto count = [&](bool isFailed, const std::string& note) {
            if (!isFailed || note.find("budget") != std::string::npos || note.find("max ") != std::string::npos)
                return;
            if (!failed)
                first = note;
            failed++;
        };
        for (const TextureEntry& e : data.textures)
            count(e.failed, e.note);
        for (const BufferEntry& e : data.buffers)
            count(e.failed, e.note);
        if (failed)
            ValidationLog::Get().Note("capture: " + std::to_string(failed) + " read-back(s) failed: " + first);
    }
    {
        // The end of the capture's stream, whichever sections it had.
        JsonWriter w;
        w.BeginObject();
        w.Key("action");
        w.String("CaptureComplete");
        w.Key("frame");
        w.Uint(data.frameIndex);
        w.Key("frames");
        w.Uint(data.frameCount);
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
    }

    for (DeviceCapture* dc : captures)
        ReleaseCaptureObjects(*dc);
    // Not a clear. A capture ends at a frame boundary, and an engine that builds its lists on
    // worker threads has several of them open at that moment, in the middle of a pass this capture
    // began -- and so holding the queries it began with them. Only the list's own recorder knows to
    // end those, at its Close (OnBeforeClose), and a list closed with a query still open fails with
    // E_FAIL, which the application reads as a lost device. So the recorders of lists still open
    // stay until each one closes; the rest go here.
    if (!cm.RecordAlways())
    {
        std::unique_lock lock(recorderMutex);
        for (auto it = recorders.begin(); it != recorders.end();)
        {
            if (it->second.rec && !it->second.rec->closed())
                ++it;
            else
                it = recorders.erase(it);
        }
        cm._recorderCount.store(recorders.size(), std::memory_order_relaxed);
    }
    LogAlways("capture sent: %llu commands in %.1f s", (unsigned long long)data.commandTotal,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - finishBegan).count());
}

}  // namespace dxinsp
