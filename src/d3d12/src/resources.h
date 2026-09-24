// Resources as the library needs to know them: their descriptions, and the state each
// subresource is in, so a read-back can transition it to COPY_SOURCE and back.
//
// D3D12 makes the application declare every transition (ResourceBarrier, or Barrier with
// layouts), per command list, taking effect when the list executes. The tracker keeps two levels:
// a global state per subresource, the result of every list executed so far, and per list a log
// of the transitions it recorded, applied to the global state at ExecuteCommandLists and read
// while the list records to answer "what state is this in here", which is what a read-back
// appended to the list needs. A resource nothing has transitioned is in the state it was created
// in (a swap chain buffer: PRESENT). Promotion and decay (a COMMON resource promoted by use, a
// buffer decaying back to COMMON at the end of the list) are not modeled beyond treating COMMON
// as usable for a copy: in practice a render target bound at pass end is in RENDER_TARGET, and
// the capture asks with that fallback.
#pragma once

#include "common.h"

#include <vector>

namespace dxinsp
{

struct ResourceInfo
{
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_DESC desc{};
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    /** Buffers: the GPU virtual address (0 for textures). */
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    /** Mips x array slices x planes, what a subresource index ranges over. */
    uint32_t subresources = 1;
    uint32_t planes = 1;
    /** A swap chain's back buffer (GetBuffer), owned by the swap chain. */
    bool swapChainBuffer = false;
    /** Placed or reserved: what it sits on (not AddRef'd). */
    ID3D12Heap* heap = nullptr;
    UINT64 heapOffset = 0;
};

class ResourceTracker
{
public:
    static ResourceTracker& Get();

    /** A resource the application created, or a swap chain buffer it retrieved. */
    void OnCreated(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_STATES initialState, ID3D12Heap* heap = nullptr, UINT64 heapOffset = 0, bool swapChainBuffer = false);
    void OnReleased(ID3D12Resource* resource);
    bool Get(ID3D12Resource* resource, ResourceInfo& out);

    /** The state a subresource is in once every executed list has run (D3D12_RESOURCE_STATE_COMMON when never transitioned). */
    D3D12_RESOURCE_STATES GlobalState(ID3D12Resource* resource, uint32_t subresource);
    /**
     * The state a subresource is in at this point of a list's recording: the last transition the
     * list recorded for it, else the global state. `known` says whether either exists, `inList`
     * whether it was the list's own: the global state is only as recent as the last submission, and
     * a list recorded beside others does not see what the lists before it in its submission do.
     */
    D3D12_RESOURCE_STATES StateIn(ID3D12CommandList* list, ID3D12Resource* resource, uint32_t subresource, bool* known = nullptr,
        bool* inList = nullptr);

    /** The list's transitions: ResourceBarrier (transition barriers) and Barrier (texture barriers, by layout). */
    void OnBarriers(ID3D12CommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers);
    void OnBarrierGroups(ID3D12CommandList* list, UINT count, const D3D12_BARRIER_GROUP* groups);
    /** The list was reset: its log starts over. */
    void OnListReset(ID3D12CommandList* list);
    /** The list was executed: its log is applied to the global state, in order. */
    void OnListExecuted(ID3D12CommandList* list);
    void OnListReleased(ID3D12CommandList* list);
    /** A discarded resource (Unity's swap chain resize): forget its states. */

    /** All subresources, as D3D12 spells it. */
    static constexpr uint32_t kAll = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

private:
    ResourceTracker() = default;
    struct Impl;
    Impl* _impl = nullptr;
    Impl& impl();
};

/** The number of subresources of a description (mips x array size x planes for the format on this device). */
uint32_t SubresourceCount(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, uint32_t* planes = nullptr);

/** The mip and array slice of a subresource index, given the description. */
void SubresourceOf(const D3D12_RESOURCE_DESC& desc, uint32_t subresource, uint32_t& mip, uint32_t& slice, uint32_t& plane);

/** The legacy state an enhanced-barrier layout stands for (README.md, "Not done"); COMMON for one without a counterpart. */
D3D12_RESOURCE_STATES StateOfLayout(D3D12_BARRIER_LAYOUT layout);

}  // namespace dxinsp
