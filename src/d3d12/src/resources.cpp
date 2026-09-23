// Resource descriptions and the state every subresource is in. See resources.h for the model:
// a global state per subresource that executed lists have produced, and per recording list a
// log of transitions applied when the list executes.
#include "resources.h"

#include "hooks.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace dxinsp
{

namespace
{

// The counts a subresource index ranges over, from a description as the runtime reports it.
uint32_t MipCount(const D3D12_RESOURCE_DESC& desc)
{
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return 1;
    if (desc.MipLevels)
        return desc.MipLevels;
    // 0 asks for the full chain: what the runtime will make of it.
    UINT64 extent = std::max<UINT64>(desc.Width, desc.Height);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        extent = std::max<UINT64>(extent, desc.DepthOrArraySize);
    uint32_t mips = 1;
    while (extent > 1)
    {
        extent >>= 1;
        ++mips;
    }
    return mips;
}

uint32_t ArrayCount(const D3D12_RESOURCE_DESC& desc)
{
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        return 1;
    return std::max<uint32_t>(desc.DepthOrArraySize, 1);
}

}  // namespace

uint32_t SubresourceCount(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, uint32_t* planes)
{
    uint32_t planeCount = 1;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        if (planes)
            *planes = 1;
        return 1;
    }
    if (device)
    {
        // The plane count is a property of the format on this device (depth-stencil formats
        // have two, some video formats more); the device's methods are hooked, so this is ours.
        ScopedInternal internal;
        D3D12_FEATURE_DATA_FORMAT_INFO info{};
        info.Format = desc.Format;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) && info.PlaneCount)
            planeCount = info.PlaneCount;
    }
    if (planes)
        *planes = planeCount;
    return MipCount(desc) * ArrayCount(desc) * planeCount;
}

void SubresourceOf(const D3D12_RESOURCE_DESC& desc, uint32_t subresource, uint32_t& mip, uint32_t& slice, uint32_t& plane)
{
    uint32_t mips = MipCount(desc);
    uint32_t slices = ArrayCount(desc);
    mip = subresource % mips;
    slice = (subresource / mips) % slices;
    plane = subresource / (mips * slices);
}

D3D12_RESOURCE_STATES StateOfLayout(D3D12_BARRIER_LAYOUT layout)
{
    switch (layout)
    {
        // COMMON and PRESENT share a value, in the layouts as in the states.
        case D3D12_BARRIER_LAYOUT_COMMON:
        case D3D12_BARRIER_LAYOUT_UNDEFINED:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON:
        case D3D12_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON:
            return D3D12_RESOURCE_STATE_COMMON;
        case D3D12_BARRIER_LAYOUT_GENERIC_READ:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case D3D12_BARRIER_LAYOUT_COPY_DEST:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE:
            return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        case D3D12_BARRIER_LAYOUT_RESOLVE_DEST:
            return D3D12_RESOURCE_STATE_RESOLVE_DEST;
        case D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE:
            return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
        case D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ:
            return D3D12_RESOURCE_STATE_VIDEO_DECODE_READ;
        case D3D12_BARRIER_LAYOUT_VIDEO_DECODE_WRITE:
            return D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
        case D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_READ:
            return D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
        case D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_WRITE:
            return D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
        case D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_READ:
            return D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
        case D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE:
            return D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

// ---------------------------------------------------------------------------------------------

struct ResourceTracker::Impl
{
    struct Entry
    {
        ResourceInfo info;
        std::vector<D3D12_RESOURCE_STATES> states;   // one per subresource
    };
    struct Transition
    {
        ID3D12Resource* resource;
        uint32_t subresource;   // kAll for every subresource
        D3D12_RESOURCE_STATES after;
    };
    // One lock for both tables: a transition names a resource, and a resource going away
    // has to leave every log at once.
    std::shared_mutex mutex;
    std::unordered_map<ID3D12Resource*, Entry> resources;
    std::unordered_map<ID3D12CommandList*, std::vector<Transition>> logs;

    // Caller holds `mutex` exclusively.
    void Log(ID3D12CommandList* list, ID3D12Resource* resource, uint32_t subresource, D3D12_RESOURCE_STATES after)
    {
        logs[list].push_back({resource, subresource, after});
    }
};

ResourceTracker& ResourceTracker::Get()
{
    static ResourceTracker* instance = new ResourceTracker();
    return *instance;
}

ResourceTracker::Impl& ResourceTracker::impl()
{
    if (!_impl)
        _impl = new Impl();
    return *_impl;
}

void ResourceTracker::OnCreated(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState, ID3D12Heap* heap, UINT64 heapOffset, bool swapChainBuffer)
{
    if (!resource)
        return;
    Impl::Entry e;
    e.info.resource = resource;
    e.info.desc = desc;
    e.info.heapType = heapType;
    // A back buffer is handed out in PRESENT whatever the caller says.
    e.info.initialState = swapChainBuffer ? D3D12_RESOURCE_STATE_PRESENT : initialState;
    e.info.swapChainBuffer = swapChainBuffer;
    e.info.heap = heap;
    e.info.heapOffset = heapOffset;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        ScopedInternal internal;
        e.info.address = resource->GetGPUVirtualAddress();
    }
    e.info.subresources = SubresourceCount(DeviceOf(resource), desc, &e.info.planes);
    e.states.assign(e.info.subresources, e.info.initialState);
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    i.resources[resource] = std::move(e);
}

void ResourceTracker::OnReleased(ID3D12Resource* resource)
{
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    i.resources.erase(resource);
    // A list still recording may name it; the address will be reused, so the entries must go.
    for (auto& log : i.logs)
    {
        auto& v = log.second;
        v.erase(std::remove_if(v.begin(), v.end(), [resource](const Impl::Transition& t) { return t.resource == resource; }), v.end());
    }
}

bool ResourceTracker::Get(ID3D12Resource* resource, ResourceInfo& out)
{
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto it = i.resources.find(resource);
    if (it == i.resources.end())
        return false;
    out = it->second.info;
    return true;
}

D3D12_RESOURCE_STATES ResourceTracker::GlobalState(ID3D12Resource* resource, uint32_t subresource)
{
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto it = i.resources.find(resource);
    if (it == i.resources.end() || it->second.states.empty())
        return D3D12_RESOURCE_STATE_COMMON;
    const auto& states = it->second.states;
    if (subresource == kAll || subresource >= states.size())
        return states[0];
    return states[subresource];
}

D3D12_RESOURCE_STATES ResourceTracker::StateIn(ID3D12CommandList* list, ID3D12Resource* resource, uint32_t subresource, bool* known)
{
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto log = i.logs.find(list);
    if (log != i.logs.end())
    {
        // The last transition the list recorded for this subresource, or for all of them.
        const auto& v = log->second;
        for (auto it = v.rbegin(); it != v.rend(); ++it)
        {
            if (it->resource != resource)
                continue;
            if (it->subresource == kAll || subresource == kAll || it->subresource == subresource)
            {
                if (known)
                    *known = true;
                return it->after;
            }
        }
    }
    auto res = i.resources.find(resource);
    if (res == i.resources.end() || res->second.states.empty())
    {
        if (known)
            *known = false;
        return D3D12_RESOURCE_STATE_COMMON;
    }
    if (known)
        *known = true;
    const auto& states = res->second.states;
    if (subresource == kAll || subresource >= states.size())
        return states[0];
    return states[subresource];
}

void ResourceTracker::OnBarriers(ID3D12CommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (!list || !count || !barriers)
        return;
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    for (UINT k = 0; k < count; ++k)
    {
        const D3D12_RESOURCE_BARRIER& b = barriers[k];
        // Aliasing and UAV barriers change no state.
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource)
            continue;
        i.Log(list, b.Transition.pResource, b.Transition.Subresource, b.Transition.StateAfter);
    }
}

void ResourceTracker::OnBarrierGroups(ID3D12CommandList* list, UINT count, const D3D12_BARRIER_GROUP* groups)
{
    if (!list || !count || !groups)
        return;
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    for (UINT g = 0; g < count; ++g)
    {
        const D3D12_BARRIER_GROUP& group = groups[g];
        // Only texture barriers carry a layout; buffer and global barriers are about access.
        if (group.Type != D3D12_BARRIER_TYPE_TEXTURE || !group.pTextureBarriers)
            continue;
        for (UINT k = 0; k < group.NumBarriers; ++k)
        {
            const D3D12_TEXTURE_BARRIER& t = group.pTextureBarriers[k];
            if (!t.pResource)
                continue;
            D3D12_RESOURCE_STATES after = StateOfLayout(t.LayoutAfter);
            const D3D12_BARRIER_SUBRESOURCE_RANGE& r = t.Subresources;
            if (r.NumMipLevels == 0)
            {
                // IndexOrFirstMipLevel is a subresource index, 0xffffffff for all of them.
                i.Log(list, t.pResource, r.IndexOrFirstMipLevel == 0xffffffffu ? kAll : r.IndexOrFirstMipLevel, after);
                continue;
            }
            auto res = i.resources.find(t.pResource);
            if (res == i.resources.end())
            {
                // Nothing to index into: the best that can be said is that the whole thing moved.
                i.Log(list, t.pResource, kAll, after);
                continue;
            }
            const D3D12_RESOURCE_DESC& desc = res->second.info.desc;
            uint32_t mips = MipCount(desc);
            uint32_t slices = ArrayCount(desc);
            uint32_t planes = std::max<uint32_t>(res->second.info.planes, 1);
            uint32_t planeCount = r.NumPlanes ? r.NumPlanes : 1;
            uint32_t sliceCount = r.NumArraySlices ? r.NumArraySlices : 1;
            for (uint32_t p = r.FirstPlane; p < r.FirstPlane + planeCount && p < planes; ++p)
                for (uint32_t s = r.FirstArraySlice; s < r.FirstArraySlice + sliceCount && s < slices; ++s)
                    for (uint32_t m = r.IndexOrFirstMipLevel; m < r.IndexOrFirstMipLevel + r.NumMipLevels && m < mips; ++m)
                        i.Log(list, t.pResource, m + mips * (s + slices * p), after);
        }
    }
}

void ResourceTracker::OnListReset(ID3D12CommandList* list)
{
    if (!list)
        return;
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    auto it = i.logs.find(list);
    if (it != i.logs.end())
        it->second.clear();
}

void ResourceTracker::OnListExecuted(ID3D12CommandList* list)
{
    if (!list)
        return;
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    auto it = i.logs.find(list);
    if (it == i.logs.end())
        return;
    for (const Impl::Transition& t : it->second)
    {
        auto res = i.resources.find(t.resource);
        if (res == i.resources.end())
            continue;
        auto& states = res->second.states;
        if (t.subresource == kAll)
            std::fill(states.begin(), states.end(), t.after);
        else if (t.subresource < states.size())
            states[t.subresource] = t.after;
    }
    // The transitions are now part of the global state; a list executed twice without a
    // Reset would apply them again, which for transitions changes nothing.
    it->second.clear();
}

void ResourceTracker::OnListReleased(ID3D12CommandList* list)
{
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    i.logs.erase(list);
}

}  // namespace dxinsp
