// What the application binds through, which D3D12 does not make objects of: the contents of
// descriptor heaps, the layout of root signatures, and the resources behind GPU virtual addresses.
//
// The Vulkan layer follows vkUpdateDescriptorSets so a bind command during a capture can carry a
// snapshot of what each bound set contained (src/vulkan/src/descriptors.h). Here the same is done for
// descriptor heaps: every Create*View, CreateSampler, CopyDescriptors and CopyDescriptorsSimple
// writes a record per heap slot, found from the CPU handle as (handle - heap start) / increment.
// A root descriptor table bound with SetGraphicsRootDescriptorTable names a GPU handle; the root
// signature (deserialized at CreateRootSignature) says how many slots each range covers and which
// shader registers they are, so the snapshot on the binding command can list them.
//
// Vertex and index buffer views, root CBV/SRV/UAVs and constant buffer views hold GPU virtual
// addresses rather than resources: AddressMap resolves one to the buffer whose range holds it.
#pragma once

#include "common.h"

#include <memory>
#include <string>
#include <vector>

namespace dxinsp {

// ---------------------------------------------------------------------------------------------
// Descriptor heap contents

enum class DescriptorKind : uint8_t { None, CBV, SRV, UAV, Sampler, RTV, DSV };

/** One heap slot as the application last wrote it. */
struct DescriptorRecord {
    DescriptorKind kind = DescriptorKind::None;
    /** The resource the view describes (SRV, UAV, RTV, DSV), not AddRef'd; null for a null view. */
    ID3D12Resource* resource = nullptr;
    /** UAV: the counter resource, or null. */
    ID3D12Resource* counter = nullptr;
    /** CBV: the buffer range. */
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    uint32_t size = 0;
    /** Whether the application passed a description (else the resource's default view). */
    bool hasDesc = false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    D3D12_SAMPLER_DESC2 sampler{};
    /** The SRV was an acceleration structure (its address is in `address`). */
    bool accelerationStructure = false;
};

/** A descriptor heap as the tracker knows it: where its handles start and how far apart they are. */
struct HeapInfo {
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};   // 0 for a heap that is not shader visible
    uint32_t increment = 0;
};

class DescriptorTracker {
public:
    static DescriptorTracker& Get();

    void OnHeapCreated(ID3D12Device* device, ID3D12DescriptorHeap* heap, const D3D12_DESCRIPTOR_HEAP_DESC& desc);
    void OnHeapReleased(ID3D12DescriptorHeap* heap);
    bool GetHeap(ID3D12DescriptorHeap* heap, HeapInfo& out);

    /** A Create*View / CreateSampler: the record replaces the slot's. */
    void Write(D3D12_CPU_DESCRIPTOR_HANDLE handle, const DescriptorRecord& record);
    /** CopyDescriptors: records copied range by range (the ranges' sizes are counted in slots). */
    void Copy(UINT numDestRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* destStarts, const UINT* destSizes,
              UINT numSrcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts, const UINT* srcSizes);
    void CopySimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src);

    /** The heap and slot a handle points into; false when no tracked heap holds it. */
    bool Locate(D3D12_CPU_DESCRIPTOR_HANDLE handle, HeapInfo& heap, uint32_t& index);
    bool Locate(D3D12_GPU_DESCRIPTOR_HANDLE handle, HeapInfo& heap, uint32_t& index);
    /** The record of a slot (kind None when never written or out of range). */
    DescriptorRecord Get(ID3D12DescriptorHeap* heap, uint32_t index);
    /** `count` records from `first` (clamped to the heap), for a table snapshot or RequestDescriptorSet. */
    std::vector<DescriptorRecord> Slots(ID3D12DescriptorHeap* heap, uint32_t first, uint32_t count);
    /** The number of slots ever written in a heap. */
    uint32_t WrittenCount(ID3D12DescriptorHeap* heap);

private:
    DescriptorTracker() = default;
    struct Impl;
    Impl* _impl = nullptr;
    Impl& impl();
};

/** A record as one entry of a snapshot's `descriptors` array (README.md, "Bound buffers and textures"); `dataId` is the capture id or 0. */
void WriteDescriptorRecord(JsonWriter& w, const DescriptorRecord& r, uint32_t dataId);

// ---------------------------------------------------------------------------------------------
// Root signatures

struct RootRange {
    D3D12_DESCRIPTOR_RANGE_TYPE type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    uint32_t numDescriptors = 0;     // UINT_MAX: unbounded (the rest of the heap)
    uint32_t baseRegister = 0;
    uint32_t space = 0;
    uint32_t offsetInTable = 0;      // resolved: D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND replaced by the running offset
    uint32_t flags = 0;
};

struct RootParameterInfo {
    D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
    /** Root constants and root views: the register and space. Root constants: the count. */
    uint32_t shaderRegister = 0;
    uint32_t space = 0;
    uint32_t num32BitValues = 0;
    /** Descriptor tables: the ranges, in order. */
    std::vector<RootRange> ranges;
    /** The number of slots the table covers (UINT_MAX when a range is unbounded). */
    uint32_t tableSlots = 0;
};

struct RootSignatureInfo {
    std::vector<RootParameterInfo> parameters;
    uint32_t staticSamplers = 0;
    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    /** The versioned description serialized, for the object's args. */
    std::string json;
};

class RootSignatures {
public:
    static RootSignatures& Get();
    /**
     * Deserializes the blob a root signature was created from (D3D12CreateVersionedRootSignatureDeserializer)
     * and keeps the layout under the object. Returns the layout (with `json` filled), or null when the
     * blob does not deserialize.
     */
    std::shared_ptr<const RootSignatureInfo> Register(ID3D12RootSignature* signature, const void* blob, size_t size);
    /** A root signature the application handed the library already deserialized (CreateRootSignatureFromSubobjectInLibrary). */
    void Register(ID3D12RootSignature* signature, std::shared_ptr<const RootSignatureInfo> info);
    std::shared_ptr<const RootSignatureInfo> Find(ID3D12RootSignature* signature);
    void Forget(ID3D12RootSignature* signature);
    /** The layout of a serialized blob without registering it (a pipeline's embedded signature). */
    static std::shared_ptr<RootSignatureInfo> Parse(const void* blob, size_t size);
    /** The layout of a root signature subobject of a shader library blob (CreateRootSignatureFromSubobjectInLibrary); null when it cannot be read. */
    static std::shared_ptr<RootSignatureInfo> ParseSubobject(const void* blob, size_t size, const wchar_t* subobjectName);

private:
    RootSignatures() = default;
    struct Impl;
    Impl* _impl = nullptr;
    Impl& impl();
};

// ---------------------------------------------------------------------------------------------
// GPU virtual addresses

class AddressMap {
public:
    static AddressMap& Get();
    /** A buffer's range (ID3D12Resource::GetGPUVirtualAddress and its width), from its creation. */
    void Add(ID3D12Resource* buffer, D3D12_GPU_VIRTUAL_ADDRESS start, UINT64 size);
    void Remove(ID3D12Resource* buffer);
    /**
     * The buffer holding `address`: its resource, the offset into it and the bytes from there to
     * its end. False for an address no tracked buffer holds (a heap the application manages
     * itself, or an acceleration structure).
     */
    bool Resolve(D3D12_GPU_VIRTUAL_ADDRESS address, ID3D12Resource*& buffer, UINT64& offset, UINT64& remaining);

private:
    AddressMap() = default;
    struct Impl;
    Impl* _impl = nullptr;
    Impl& impl();
};

}  // namespace dxinsp
