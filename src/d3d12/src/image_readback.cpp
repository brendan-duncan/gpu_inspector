// Live image read-back (image_readback.h): one subresource of an ID3D12Resource copied into a
// readback buffer by a command list of the library's own, waited for, and sent as ImageData.
//
// The copy runs on the device's first direct queue, after whatever the application has queued
// there, so the pixels are those of the last frame; the wait happens on the transport's receiver
// thread, which is the one that asked. Everything D3D12 here is the library's, under
// ScopedInternal, so the hooks forward it untracked.
#include "image_readback.h"

#include "formats.h"
#include "hooks.h"
#include "resources.h"
#include "tracker.h"
#include "transport.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dxinsp
{

namespace
{

constexpr uint64_t kMaxReadback = 256ull << 20;
constexpr DWORD kWaitMs = 3000;

/** The queues of one device the application created (not AddRef'd), and what the read-back keeps on it. */
struct DeviceQueues
{
    std::vector<ID3D12CommandQueue*> queues;
    ID3D12CommandQueue* firstDirect = nullptr;
    ComPtr<ID3D12CommandQueue> own;   // made when the application has no direct queue to borrow
};

/** The read-back objects of one device, made on first use and kept. */
struct DeviceReadback
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t fenceValue = 0;
    ComPtr<ID3D12Resource> buffer;
    uint64_t bufferSize = 0;

    ~DeviceReadback()
    {
        if (event)
            CloseHandle(event);
    }
};

struct State
{
    std::mutex mutex;
    std::unordered_map<ID3D12Device*, DeviceQueues> queues;
    std::unordered_map<ID3D12Device*, DeviceReadback> readbacks;
};

State& GetState()
{
    static State* s = new State();
    return *s;
}

void WriteHeader(JsonWriter& w, uint64_t id, uint32_t mip, uint32_t layer, uint32_t width, uint32_t height, uint32_t depth,
    const char* format, bool depthAspect, uint64_t size, const char* error)
{
    w.BeginObject();
    w.Key("action");
    w.String("ImageData");
    w.Key("id");
    w.Uint(id);
    w.Key("mip");
    w.Uint(mip);
    w.Key("layer");
    w.Uint(layer);
    w.Key("width");
    w.Uint(width);
    w.Key("height");
    w.Uint(height);
    w.Key("depth");
    w.Uint(depth);
    w.Key("layers");
    w.Uint(1);
    w.Key("format");
    w.String(format ? format : "VK_FORMAT_UNDEFINED");
    w.Key("aspect");
    w.String(depthAspect ? "depth" : "color");
    w.Key("size");
    w.Uint(size);
    if (error)
    {
        w.Key("error");
        w.String(error);
    }
    w.EndObject();
}

void Fail(uint64_t id, uint32_t mip, uint32_t layer, const char* why)
{
    JsonWriter w;
    WriteHeader(w, id, mip, layer, 0, 0, 1, nullptr, false, 0, why);
    Transport::Get().SendJson(std::move(w.str()));
    Log("image readback %llu failed: %s", (unsigned long long)id, why);
}

inline bool NeedsCopyBarrier(D3D12_RESOURCE_STATES state)
{
    return state != D3D12_RESOURCE_STATE_COMMON && !(state & D3D12_RESOURCE_STATE_COPY_SOURCE);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t subresource,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

uint32_t MipDim(uint64_t dim, uint32_t mip)
{
    const uint64_t v = dim >> mip;
    return v ? (uint32_t)v : 1u;
}

/** The device's read-back objects, made on first use; null when they cannot be. */
DeviceReadback* ReadbackFor(ID3D12Device* device, uint64_t bufferSize)
{
    State& s = GetState();
    std::lock_guard lock(s.mutex);
    DeviceReadback& rb = s.readbacks[device];
    ScopedInternal internal;
    if (!rb.allocator && FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(rb.allocator.put()))))
    {
        rb.allocator.reset();
        return nullptr;
    }
    if (!rb.list)
    {
        // Created closed, so every request starts with a Reset.
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rb.allocator.get(), nullptr, IID_PPV_ARGS(rb.list.put()))))
        {
            rb.list.reset();
            return nullptr;
        }
        rb.list->Close();
    }
    if (!rb.fence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(rb.fence.put()))))
    {
        rb.fence.reset();
        return nullptr;
    }
    if (!rb.event)
        rb.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!rb.event)
        return nullptr;
    if (!rb.buffer || rb.bufferSize < bufferSize)
    {
        rb.buffer.reset();
        rb.bufferSize = 0;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bufferSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(rb.buffer.put()))))
        {
            rb.buffer.reset();
            return nullptr;
        }
        rb.bufferSize = bufferSize;
    }
    return &rb;
}

}  // namespace

void OnQueueCreated(ID3D12Device* device, ID3D12CommandQueue* queue, D3D12_COMMAND_LIST_TYPE type)
{
    if (!device || !queue)
        return;
    State& s = GetState();
    std::lock_guard lock(s.mutex);
    DeviceQueues& q = s.queues[device];
    q.queues.push_back(queue);
    if (!q.firstDirect && type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        q.firstDirect = queue;
}

void OnQueueReleased(ID3D12CommandQueue* queue)
{
    State& s = GetState();
    std::lock_guard lock(s.mutex);
    for (auto& [device, q] : s.queues)
    {
        q.queues.erase(std::remove(q.queues.begin(), q.queues.end(), queue), q.queues.end());
        if (q.firstDirect == queue)
        {
            q.firstDirect = nullptr;
            // The next direct queue the application still has, if any.
            for (ID3D12CommandQueue* other : q.queues)
            {
                ScopedInternal internal;
                if (other->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
                {
                    q.firstDirect = other;
                    break;
                }
            }
        }
    }
}

ID3D12CommandQueue* ReadbackQueue(ID3D12Device* device)
{
    if (!device)
        return nullptr;
    State& s = GetState();
    std::lock_guard lock(s.mutex);
    DeviceQueues& q = s.queues[device];
    if (q.firstDirect)
        return q.firstDirect;
    if (!q.own)
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ScopedInternal internal;
        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(q.own.put()))))
            q.own.reset();
    }
    return q.own.get();
}

void ReadBackImage(uint64_t objectId, uint32_t mip, uint32_t layer)
{
    Tracker& tracker = Tracker::Get();
    void* object = tracker.ObjectOf(objectId);
    if (!object || tracker.TypeOf(object) != "ID3D12Resource")
        return Fail(objectId, mip, layer, "image no longer exists");
    ID3D12Resource* resource = static_cast<ID3D12Resource*>(object);
    ResourceInfo info;
    if (!ResourceTracker::Get().Get(resource, info))
        return Fail(objectId, mip, layer, "resource is not tracked");
    const D3D12_RESOURCE_DESC& desc = info.desc;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return Fail(objectId, mip, layer, "not a texture");
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const uint32_t mipLevels = std::max<uint32_t>(1, desc.MipLevels);
    const uint32_t arraySize = volume ? 1 : std::max<uint32_t>(1, desc.DepthOrArraySize);
    if (mip >= mipLevels)
        return Fail(objectId, mip, layer, "mip level out of range");
    if (layer >= arraySize)
        return Fail(objectId, mip, layer, "array layer out of range");
    if (desc.SampleDesc.Count > 1)
        return Fail(objectId, mip, layer, "multisampled textures are not read back live");

    // Depth textures travel as their depth plane under the protocol's depth format (4 bytes a
    // texel for D32 and D24, 2 for D16), everything else as its typed format.
    const FormatInfo raw = FormatOf(desc.Format);
    const DXGI_FORMAT typed = TypedFormat(desc.Format, raw.depth);
    const FormatInfo fmt = FormatOf(typed);
    if (!fmt.protocolName)
        return Fail(objectId, mip, layer, "format cannot be decoded");
    const bool depthAspect = fmt.depth;
    const uint32_t texelBytes = depthAspect ? (typed == DXGI_FORMAT_D16_UNORM ? 2u : 4u) : fmt.bytes;
    const uint32_t width = MipDim(desc.Width, mip);
    const uint32_t height = MipDim(desc.Height, mip);
    const uint32_t depth = volume ? MipDim(desc.DepthOrArraySize, mip) : 1;
    const uint64_t rowBytes = (uint64_t)((width + fmt.blockWidth - 1) / fmt.blockWidth) * texelBytes;
    const uint32_t rows = (height + fmt.blockHeight - 1) / fmt.blockHeight;
    const uint64_t size = rowBytes * rows * depth;
    if (!size)
        return Fail(objectId, mip, layer, "empty image");
    if (size > kMaxReadback)
        return Fail(objectId, mip, layer, "image is larger than 256 MB");

    ID3D12Device* device = DeviceOf(resource);
    if (!device)
        return Fail(objectId, mip, layer, "device unavailable");
    ID3D12CommandQueue* queue = ReadbackQueue(device);
    if (!queue)
        return Fail(objectId, mip, layer, "no direct queue to execute on");

    // The copy: the subresource's plane 0 into a placed footprint at the start of the buffer.
    const uint32_t subresource = mip + layer * mipLevels;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0, total = 0;
    {
        ScopedInternal internal;
        device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &numRows, &rowSize, &total);
    }
    if (!total)
        return Fail(objectId, mip, layer, "the subresource has no copyable layout");
    DeviceReadback* rb = ReadbackFor(device, total);
    if (!rb)
        return Fail(objectId, mip, layer, "read-back objects could not be created");

    // The state the subresource is in once everything executed so far has run: a swap chain
    // buffer sits in PRESENT (which is COMMON), anything never transitioned in COMMON, and both
    // promote to a copy source without a barrier.
    const D3D12_RESOURCE_STATES state = ResourceTracker::Get().GlobalState(resource, subresource);
    const bool barrier = NeedsCopyBarrier(state);
    HRESULT hr;
    {
        // One request at a time per process: the objects are shared and the wait is short.
        static std::mutex requestMutex;
        std::lock_guard lock(requestMutex);
        ScopedInternal internal;
        if (FAILED(rb->allocator->Reset()) || FAILED(rb->list->Reset(rb->allocator.get(), nullptr)))
            return Fail(objectId, mip, layer, "read-back command list could not be reset");
        ID3D12GraphicsCommandList* list = rb->list.get();
        if (barrier)
            Transition(list, resource, subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb->buffer.get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = subresource;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (barrier)
            Transition(list, resource, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        if (FAILED(list->Close()))
            return Fail(objectId, mip, layer, "read-back command list could not be closed");
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        const uint64_t value = ++rb->fenceValue;
        hr = queue->Signal(rb->fence.get(), value);
        if (SUCCEEDED(hr) && rb->fence->GetCompletedValue() < value)
        {
            hr = rb->fence->SetEventOnCompletion(value, rb->event);
            if (SUCCEEDED(hr) && WaitForSingleObject(rb->event, kWaitMs) != WAIT_OBJECT_0)
                hr = E_FAIL;
        }
        if (FAILED(hr))
            return Fail(objectId, mip, layer, "read-back timed out");

        void* mapped = nullptr;
        D3D12_RANGE range{0, (SIZE_T)total};
        if (FAILED(rb->buffer->Map(0, &range, &mapped)) || !mapped)
            return Fail(objectId, mip, layer, "readback buffer could not be mapped");
        // Rows come out at the copy's 256-byte pitch; the UI wants them tight, slice after slice.
        std::vector<uint8_t> packed((size_t)size);
        const uint8_t* base = static_cast<const uint8_t*>(mapped) + footprint.Offset;
        const uint64_t tight = std::min<uint64_t>(rowBytes, footprint.Footprint.RowPitch);
        const uint64_t slicePitch = (uint64_t)footprint.Footprint.RowPitch * numRows;
        size_t at = 0;
        for (uint32_t z = 0; z < depth; ++z)
        {
            for (uint32_t row = 0; row < rows && row < numRows; ++row)
            {
                memcpy(packed.data() + at, base + z * slicePitch + (uint64_t)row * footprint.Footprint.RowPitch, (size_t)tight);
                at += (size_t)rowBytes;
            }
        }
        D3D12_RANGE none{0, 0};
        rb->buffer->Unmap(0, &none);
        JsonWriter h;
        WriteHeader(h, objectId, mip, layer, width, height, depth, fmt.protocolName, depthAspect, size, nullptr);
        Transport::Get().SendBinary(std::move(h.str()), packed.data(), packed.size());
    }
    Log("image readback %llu: mip %u layer %u %ux%u (%llu bytes)", (unsigned long long)objectId, mip, layer, width, height,
        (unsigned long long)size);
}

}  // namespace dxinsp
