// Descriptor heap contents, root signature layouts and the buffer behind a GPU virtual address.
// See descriptors.h for what each of the three trackers is for.
#include "descriptors.h"

#include "formats.h"
#include "json.h"
#include "resources.h"
#include "serialize.h"
#include "tracker.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>

namespace dxinsp {

// ---------------------------------------------------------------------------------------------
// Descriptor heaps

struct DescriptorTracker::Impl {
    struct Heap {
        HeapInfo info;
        // The records have their own lock: a Locate under the shared list lock finds the heap,
        // and the write that follows contends only with writes into the same heap.
        std::mutex mutex;
        std::vector<DescriptorRecord> records;
        uint32_t written = 0;   // the highest slot ever written + 1
    };

    // The list of heaps is read on every descriptor write and every handle serialized, and
    // changes only when the application creates or destroys a heap: a shared_mutex, and a
    // linear scan (an application has a handful of heaps).
    std::shared_mutex mutex;
    std::vector<std::unique_ptr<Heap>> heaps;

    // Callers hold `mutex` (shared at least).
    Heap* FindByCpu(SIZE_T ptr, uint32_t& index) {
        for (auto& h : heaps) {
            const HeapInfo& i = h->info;
            if (!i.increment || !i.desc.NumDescriptors) continue;
            SIZE_T start = i.cpuStart.ptr;
            SIZE_T end = start + (SIZE_T)i.increment * i.desc.NumDescriptors;
            if (ptr >= start && ptr < end) {
                index = (uint32_t)((ptr - start) / i.increment);
                return h.get();
            }
        }
        return nullptr;
    }
    Heap* FindByGpu(UINT64 ptr, uint32_t& index) {
        if (!ptr) return nullptr;
        for (auto& h : heaps) {
            const HeapInfo& i = h->info;
            if (!i.gpuStart.ptr || !i.increment || !i.desc.NumDescriptors) continue;
            UINT64 start = i.gpuStart.ptr;
            UINT64 end = start + (UINT64)i.increment * i.desc.NumDescriptors;
            if (ptr >= start && ptr < end) {
                index = (uint32_t)((ptr - start) / i.increment);
                return h.get();
            }
        }
        return nullptr;
    }
    Heap* FindByObject(ID3D12DescriptorHeap* heap) {
        for (auto& h : heaps)
            if (h->info.heap == heap) return h.get();
        return nullptr;
    }
};

DescriptorTracker& DescriptorTracker::Get() {
    static DescriptorTracker* instance = new DescriptorTracker();
    return *instance;
}

DescriptorTracker::Impl& DescriptorTracker::impl() {
    if (!_impl) _impl = new Impl();
    return *_impl;
}

void DescriptorTracker::OnHeapCreated(ID3D12Device* device, ID3D12DescriptorHeap* heap, const D3D12_DESCRIPTOR_HEAP_DESC& desc) {
    if (!heap || !device) return;
    auto h = std::make_unique<Impl::Heap>();
    h->info.heap = heap;
    h->info.desc = desc;
    {
        // The heap's own methods, and the device's, are hooked like everything else: these are ours.
        ScopedInternal internal;
        h->info.cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
        if (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
            h->info.gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
        h->info.increment = device->GetDescriptorHandleIncrementSize(desc.Type);
    }
    h->records.resize(desc.NumDescriptors);
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    // The address of a destroyed heap can come back for a new one before its Release hook ran
    // (the debug layer's wrapper releases the real object first): replace, never duplicate.
    for (auto& existing : i.heaps) {
        if (existing->info.heap == heap) {
            existing = std::move(h);
            return;
        }
    }
    i.heaps.push_back(std::move(h));
}

void DescriptorTracker::OnHeapReleased(ID3D12DescriptorHeap* heap) {
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    i.heaps.erase(std::remove_if(i.heaps.begin(), i.heaps.end(),
                                 [heap](const std::unique_ptr<Impl::Heap>& h) { return h->info.heap == heap; }),
                  i.heaps.end());
}

bool DescriptorTracker::GetHeap(ID3D12DescriptorHeap* heap, HeapInfo& out) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    Impl::Heap* h = i.FindByObject(heap);
    if (!h) return false;
    out = h->info;
    return true;
}

void DescriptorTracker::Write(D3D12_CPU_DESCRIPTOR_HANDLE handle, const DescriptorRecord& record) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    uint32_t index = 0;
    Impl::Heap* h = i.FindByCpu(handle.ptr, index);
    if (!h) return;
    std::lock_guard<std::mutex> records(h->mutex);
    if (index >= h->records.size()) return;
    h->records[index] = record;
    h->written = std::max(h->written, index + 1);
}

void DescriptorTracker::Copy(UINT numDestRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* destStarts, const UINT* destSizes,
                             UINT numSrcRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts, const UINT* srcSizes) {
    if (!destStarts || !srcStarts) return;
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    // Both sides are sequences of slots (a null size array means one slot per range); the
    // sources are read out first, so a copy within one heap never holds two locks at once.
    std::vector<DescriptorRecord> staged;
    for (UINT r = 0; r < numSrcRanges; ++r) {
        UINT count = srcSizes ? srcSizes[r] : 1;
        uint32_t index = 0;
        Impl::Heap* h = i.FindByCpu(srcStarts[r].ptr, index);
        if (h) {
            std::lock_guard<std::mutex> records(h->mutex);
            for (UINT k = 0; k < count; ++k)
                staged.push_back(index + k < h->records.size() ? h->records[index + k] : DescriptorRecord{});
        } else {
            staged.insert(staged.end(), count, DescriptorRecord{});
        }
    }
    size_t cursor = 0;
    for (UINT r = 0; r < numDestRanges && cursor < staged.size(); ++r) {
        UINT count = destSizes ? destSizes[r] : 1;
        uint32_t index = 0;
        Impl::Heap* h = i.FindByCpu(destStarts[r].ptr, index);
        if (!h) {
            cursor += count;
            continue;
        }
        std::lock_guard<std::mutex> records(h->mutex);
        for (UINT k = 0; k < count && cursor < staged.size(); ++k, ++cursor) {
            if (index + k >= h->records.size()) continue;
            h->records[index + k] = staged[cursor];
            h->written = std::max(h->written, index + k + 1);
        }
    }
}

void DescriptorTracker::CopySimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src) {
    Copy(1, &dest, &count, 1, &src, &count);
}

bool DescriptorTracker::Locate(D3D12_CPU_DESCRIPTOR_HANDLE handle, HeapInfo& heap, uint32_t& index) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    Impl::Heap* h = i.FindByCpu(handle.ptr, index);
    if (!h) return false;
    heap = h->info;
    return true;
}

bool DescriptorTracker::Locate(D3D12_GPU_DESCRIPTOR_HANDLE handle, HeapInfo& heap, uint32_t& index) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    Impl::Heap* h = i.FindByGpu(handle.ptr, index);
    if (!h) return false;
    heap = h->info;
    return true;
}

DescriptorRecord DescriptorTracker::Get(ID3D12DescriptorHeap* heap, uint32_t index) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    Impl::Heap* h = i.FindByObject(heap);
    if (!h) return DescriptorRecord{};
    std::lock_guard<std::mutex> records(h->mutex);
    if (index >= h->records.size()) return DescriptorRecord{};
    return h->records[index];
}

std::vector<DescriptorRecord> DescriptorTracker::Slots(ID3D12DescriptorHeap* heap, uint32_t first, uint32_t count) {
    std::vector<DescriptorRecord> out;
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    Impl::Heap* h = i.FindByObject(heap);
    if (!h) return out;
    std::lock_guard<std::mutex> records(h->mutex);
    if (first >= h->records.size()) return out;
    uint32_t end = (uint32_t)std::min<uint64_t>((uint64_t)first + count, h->records.size());
    out.assign(h->records.begin() + first, h->records.begin() + end);
    return out;
}

uint32_t DescriptorTracker::WrittenCount(ID3D12DescriptorHeap* heap) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    Impl::Heap* h = i.FindByObject(heap);
    if (!h) return 0;
    std::lock_guard<std::mutex> records(h->mutex);
    return h->written;
}

// ---------------------------------------------------------------------------------------------
// Handle and address writers (json.h)

void WriteCpuHandle(JsonWriter& w, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    HeapInfo heap;
    uint32_t index = 0;
    w.BeginObject();
    if (DescriptorTracker::Get().Locate(handle, heap, index)) {
        w.Key("heap");
        WriteRef(w, heap.heap, "ID3D12DescriptorHeap");
        w.Key("index");
        w.Uint(index);
    } else {
        w.Key("ptr");
        w.String(Hex(handle.ptr));
    }
    w.EndObject();
}

void WriteGpuHandle(JsonWriter& w, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    HeapInfo heap;
    uint32_t index = 0;
    w.BeginObject();
    if (DescriptorTracker::Get().Locate(handle, heap, index)) {
        w.Key("heap");
        WriteRef(w, heap.heap, "ID3D12DescriptorHeap");
        w.Key("index");
        w.Uint(index);
    } else {
        w.Key("ptr");
        w.String(Hex(handle.ptr));
    }
    w.EndObject();
}

void WriteGpuAddress(JsonWriter& w, D3D12_GPU_VIRTUAL_ADDRESS address) {
    if (!address) {
        w.Null();
        return;
    }
    w.BeginObject();
    w.Key("address");
    w.String(Hex(address));
    ID3D12Resource* buffer = nullptr;
    UINT64 offset = 0, remaining = 0;
    if (AddressMap::Get().Resolve(address, buffer, offset, remaining)) {
        w.Key("buffer");
        WriteRef(w, buffer, "ID3D12Resource");
        w.Key("offset");
        w.Uint(offset);
    }
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Snapshot entries

namespace {

bool IsBufferResource(ID3D12Resource* resource, UINT64* width) {
    ResourceInfo info;
    if (!resource || !ResourceTracker::Get().Get(resource, info)) return false;
    if (info.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return false;
    if (width) *width = info.desc.Width;
    return true;
}

// Bytes per element of a buffer view: structured, raw (32-bit words) or typed.
uint32_t BufferStride(uint32_t structureByteStride, bool raw, DXGI_FORMAT format) {
    if (structureByteStride) return structureByteStride;
    if (raw) return 4;
    return FormatOf(format).bytes;
}

void WriteDataId(JsonWriter& w, uint32_t dataId) {
    if (!dataId) return;
    w.Key("data");
    w.Uint(dataId);
}

// A buffer SRV/UAV: {buffer, offset, range, view, data}; a texture one: {resource, view, data}.
template <typename ViewDesc>
void WriteResourceView(JsonWriter& w, const DescriptorRecord& r, const ViewDesc* view, bool isBufferView,
                       UINT64 firstElement, uint32_t numElements, uint32_t stride, uint32_t dataId) {
    w.BeginObject();
    if (isBufferView) {
        w.Key("buffer");
        WriteRef(w, r.resource, "ID3D12Resource");
        w.Key("offset");
        w.Uint(firstElement * stride);
        w.Key("range");
        w.Uint((UINT64)numElements * stride);
    } else {
        w.Key("resource");
        WriteRef(w, r.resource, "ID3D12Resource");
    }
    if (r.counter) {
        w.Key("counter");
        WriteRef(w, r.counter, "ID3D12Resource");
    }
    w.Key("view");
    if (r.hasDesc) Write(w, view); else w.Null();
    WriteDataId(w, dataId);
    w.EndObject();
}

}  // namespace

void WriteDescriptorRecord(JsonWriter& w, const DescriptorRecord& r, uint32_t dataId) {
    switch (r.kind) {
        case DescriptorKind::None:
            w.Null();
            return;

        case DescriptorKind::CBV: {
            w.BeginObject();
            ID3D12Resource* buffer = nullptr;
            UINT64 offset = 0, remaining = 0;
            if (r.address && AddressMap::Get().Resolve(r.address, buffer, offset, remaining)) {
                w.Key("buffer");
                WriteRef(w, buffer, "ID3D12Resource");
                w.Key("offset");
                w.Uint(offset);
            } else {
                w.Key("buffer");
                w.Null();
                w.Key("address");
                w.String(Hex(r.address));
            }
            w.Key("range");
            w.Uint(r.size);
            WriteDataId(w, dataId);
            w.EndObject();
            return;
        }

        case DescriptorKind::SRV: {
            if (r.accelerationStructure) {
                w.BeginObject();
                w.Key("accelerationStructure");
                w.BeginObject();
                w.Key("address");
                w.String(Hex(r.address));
                w.EndObject();
                w.EndObject();
                return;
            }
            UINT64 width = 0;
            bool isBuffer = r.hasDesc ? r.srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER : IsBufferResource(r.resource, &width);
            UINT64 first = 0;
            uint32_t count = 0, stride = 1;
            if (isBuffer && r.hasDesc) {
                first = r.srv.Buffer.FirstElement;
                count = r.srv.Buffer.NumElements;
                stride = BufferStride(r.srv.Buffer.StructureByteStride, (r.srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0, r.srv.Format);
            } else if (isBuffer) {
                // A default view covers the whole buffer.
                count = (uint32_t)std::min<UINT64>(width, UINT32_MAX);
            }
            WriteResourceView(w, r, &r.srv, isBuffer, first, count, stride, dataId);
            return;
        }

        case DescriptorKind::UAV: {
            UINT64 width = 0;
            bool isBuffer = r.hasDesc ? r.uav.ViewDimension == D3D12_UAV_DIMENSION_BUFFER : IsBufferResource(r.resource, &width);
            UINT64 first = 0;
            uint32_t count = 0, stride = 1;
            if (isBuffer && r.hasDesc) {
                first = r.uav.Buffer.FirstElement;
                count = r.uav.Buffer.NumElements;
                stride = BufferStride(r.uav.Buffer.StructureByteStride, (r.uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0, r.uav.Format);
            } else if (isBuffer) {
                count = (uint32_t)std::min<UINT64>(width, UINT32_MAX);
            }
            WriteResourceView(w, r, &r.uav, isBuffer, first, count, stride, dataId);
            return;
        }

        case DescriptorKind::Sampler:
            w.BeginObject();
            w.Key("sampler");
            w.Null();
            w.Key("samplerDesc");
            Write(w, r.sampler);
            w.EndObject();
            return;

        case DescriptorKind::RTV:
            w.BeginObject();
            w.Key("resource");
            WriteRef(w, r.resource, "ID3D12Resource");
            w.Key("view");
            if (r.hasDesc) Write(w, &r.rtv); else w.Null();
            w.EndObject();
            return;

        case DescriptorKind::DSV:
            w.BeginObject();
            w.Key("resource");
            WriteRef(w, r.resource, "ID3D12Resource");
            w.Key("view");
            if (r.hasDesc) Write(w, &r.dsv); else w.Null();
            w.EndObject();
            return;
    }
    w.Null();
}

// ---------------------------------------------------------------------------------------------
// Root signatures

namespace {

typedef HRESULT(WINAPI* PFN_VersionedDeserializer)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI* PFN_LegacyDeserializer)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI* PFN_SubobjectDeserializer)(LPCVOID, SIZE_T, LPCWSTR, REFIID, void**);

// Taken from d3d12.dll by name rather than imported: the library must not pull d3d12.dll in
// before the application does (the Agility SDK path depends on who loads it first), and the
// subobject deserializer is missing from older runtimes.
FARPROC D3D12Export(const char* name) {
    HMODULE module = GetModuleHandleW(L"d3d12.dll");
    if (!module) module = LoadLibraryW(L"d3d12.dll");
    return module ? GetProcAddress(module, name) : nullptr;
}

template <typename Range>
void FillRanges(const Range* ranges, uint32_t count, RootParameterInfo& p) {
    uint32_t running = 0;
    uint32_t slots = 0;
    bool unbounded = false;
    for (uint32_t i = 0; i < count && ranges; ++i) {
        const Range& src = ranges[i];
        RootRange r;
        r.type = src.RangeType;
        r.numDescriptors = src.NumDescriptors;
        r.baseRegister = src.BaseShaderRegister;
        r.space = src.RegisterSpace;
        if constexpr (std::is_same_v<Range, D3D12_DESCRIPTOR_RANGE1>) r.flags = (uint32_t)src.Flags;
        r.offsetInTable = src.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                              ? running
                              : src.OffsetInDescriptorsFromTableStart;
        if (r.numDescriptors == UINT_MAX) {
            unbounded = true;
            running = UINT_MAX;
        } else if (!unbounded) {
            uint64_t end = (uint64_t)r.offsetInTable + r.numDescriptors;
            running = (uint32_t)std::min<uint64_t>(end, UINT_MAX);
            slots = std::max(slots, running);
        }
        p.ranges.push_back(r);
    }
    p.tableSlots = unbounded ? UINT_MAX : slots;
}

template <typename Param>
void FillParameters(const Param* params, uint32_t count, RootSignatureInfo& info) {
    for (uint32_t i = 0; i < count && params; ++i) {
        const Param& src = params[i];
        RootParameterInfo p;
        p.type = src.ParameterType;
        p.visibility = src.ShaderVisibility;
        switch (src.ParameterType) {
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
                FillRanges(src.DescriptorTable.pDescriptorRanges, src.DescriptorTable.NumDescriptorRanges, p);
                break;
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
                p.shaderRegister = src.Constants.ShaderRegister;
                p.space = src.Constants.RegisterSpace;
                p.num32BitValues = src.Constants.Num32BitValues;
                break;
            default:
                p.shaderRegister = src.Descriptor.ShaderRegister;
                p.space = src.Descriptor.RegisterSpace;
                break;
        }
        info.parameters.push_back(std::move(p));
    }
}

bool FillInfo(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& v, RootSignatureInfo& info) {
    switch (v.Version) {
        case D3D_ROOT_SIGNATURE_VERSION_1_0:
            FillParameters(v.Desc_1_0.pParameters, v.Desc_1_0.NumParameters, info);
            info.staticSamplers = v.Desc_1_0.NumStaticSamplers;
            info.flags = v.Desc_1_0.Flags;
            break;
        case D3D_ROOT_SIGNATURE_VERSION_1_1:
            FillParameters(v.Desc_1_1.pParameters, v.Desc_1_1.NumParameters, info);
            info.staticSamplers = v.Desc_1_1.NumStaticSamplers;
            info.flags = v.Desc_1_1.Flags;
            break;
        case D3D_ROOT_SIGNATURE_VERSION_1_2:
            FillParameters(v.Desc_1_2.pParameters, v.Desc_1_2.NumParameters, info);
            info.staticSamplers = v.Desc_1_2.NumStaticSamplers;
            info.flags = v.Desc_1_2.Flags;
            break;
        default:
            return false;
    }
    JsonWriter w(&Tracker::Get());
    Write(w, v);
    info.json = std::move(w.str());
    return true;
}

}  // namespace

struct RootSignatures::Impl {
    std::shared_mutex mutex;
    std::unordered_map<ID3D12RootSignature*, std::shared_ptr<const RootSignatureInfo>> signatures;
};

RootSignatures& RootSignatures::Get() {
    static RootSignatures* instance = new RootSignatures();
    return *instance;
}

RootSignatures::Impl& RootSignatures::impl() {
    if (!_impl) _impl = new Impl();
    return *_impl;
}

std::shared_ptr<RootSignatureInfo> RootSignatures::Parse(const void* blob, size_t size) {
    if (!blob || !size) return nullptr;
    ScopedInternal internal;
    auto info = std::make_shared<RootSignatureInfo>();
    if (auto fn = (PFN_VersionedDeserializer)D3D12Export("D3D12CreateVersionedRootSignatureDeserializer")) {
        ComPtr<ID3D12VersionedRootSignatureDeserializer> d;
        if (SUCCEEDED(fn(blob, size, IID_PPV_ARGS(d.put()))) && d) {
            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = d->GetUnconvertedRootSignatureDesc();
            if (desc && FillInfo(*desc, *info)) return info;
        }
    }
    // A blob the versioned deserializer rejects may still be a 1.0 one an old runtime made.
    if (auto fn = (PFN_LegacyDeserializer)D3D12Export("D3D12CreateRootSignatureDeserializer")) {
        ComPtr<ID3D12RootSignatureDeserializer> d;
        if (SUCCEEDED(fn(blob, size, IID_PPV_ARGS(d.put()))) && d) {
            const D3D12_ROOT_SIGNATURE_DESC* desc = d->GetRootSignatureDesc();
            if (desc) {
                D3D12_VERSIONED_ROOT_SIGNATURE_DESC v{};
                v.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
                v.Desc_1_0 = *desc;
                if (FillInfo(v, *info)) return info;
            }
        }
    }
    return nullptr;
}

std::shared_ptr<RootSignatureInfo> RootSignatures::ParseSubobject(const void* blob, size_t size, const wchar_t* subobjectName) {
    if (!blob || !size) return nullptr;
    ScopedInternal internal;
    auto fn = (PFN_SubobjectDeserializer)D3D12Export("D3D12CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary");
    if (!fn) return nullptr;
    ComPtr<ID3D12VersionedRootSignatureDeserializer> d;
    if (FAILED(fn(blob, size, subobjectName, IID_PPV_ARGS(d.put()))) || !d) return nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = d->GetUnconvertedRootSignatureDesc();
    if (!desc) return nullptr;
    auto info = std::make_shared<RootSignatureInfo>();
    if (!FillInfo(*desc, *info)) return nullptr;
    return info;
}

std::shared_ptr<const RootSignatureInfo> RootSignatures::Register(ID3D12RootSignature* signature, const void* blob, size_t size) {
    std::shared_ptr<RootSignatureInfo> info = Parse(blob, size);
    // The bytes as well as the layout: a mesh output measurement needs this signature again with
    // the stream-output flag on it, which means serializing it afresh (mesh_output.cpp).
    if (info && blob && size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(blob);
        info->blob.assign(bytes, bytes + size);
    }
    if (!signature) return info;
    if (info) Register(signature, info);
    return info;
}

void RootSignatures::Register(ID3D12RootSignature* signature, std::shared_ptr<const RootSignatureInfo> info) {
    if (!signature || !info) return;
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    i.signatures[signature] = std::move(info);
}

std::shared_ptr<const RootSignatureInfo> RootSignatures::Find(ID3D12RootSignature* signature) {
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto it = i.signatures.find(signature);
    return it == i.signatures.end() ? nullptr : it->second;
}

void RootSignatures::Forget(ID3D12RootSignature* signature) {
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    i.signatures.erase(signature);
}

// ---------------------------------------------------------------------------------------------
// GPU virtual addresses

struct AddressMap::Impl {
    struct Range {
        ID3D12Resource* buffer;
        UINT64 size;
    };
    std::shared_mutex mutex;
    // Keyed by start address so a lookup is one upper_bound; buffers never overlap, so the
    // range whose start is the greatest not above the address is the only candidate.
    std::map<D3D12_GPU_VIRTUAL_ADDRESS, Range> ranges;
    std::unordered_map<ID3D12Resource*, D3D12_GPU_VIRTUAL_ADDRESS> starts;
};

AddressMap& AddressMap::Get() {
    static AddressMap* instance = new AddressMap();
    return *instance;
}

AddressMap::Impl& AddressMap::impl() {
    if (!_impl) _impl = new Impl();
    return *_impl;
}

void AddressMap::Add(ID3D12Resource* buffer, D3D12_GPU_VIRTUAL_ADDRESS start, UINT64 size) {
    if (!buffer || !start || !size) return;
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    auto old = i.starts.find(buffer);
    if (old != i.starts.end()) {
        i.ranges.erase(old->second);
        i.starts.erase(old);
    }
    // A placed buffer can alias one released without its Release hook seen: the newer wins.
    i.ranges[start] = {buffer, size};
    i.starts[buffer] = start;
}

void AddressMap::Remove(ID3D12Resource* buffer) {
    Impl& i = impl();
    std::unique_lock lock(i.mutex);
    auto it = i.starts.find(buffer);
    if (it == i.starts.end()) return;
    auto range = i.ranges.find(it->second);
    if (range != i.ranges.end() && range->second.buffer == buffer) i.ranges.erase(range);
    i.starts.erase(it);
}

bool AddressMap::Resolve(D3D12_GPU_VIRTUAL_ADDRESS address, ID3D12Resource*& buffer, UINT64& offset, UINT64& remaining) {
    if (!address) return false;
    Impl& i = impl();
    std::shared_lock lock(i.mutex);
    auto it = i.ranges.upper_bound(address);
    if (it == i.ranges.begin()) return false;
    --it;
    if (address >= it->first + it->second.size) return false;
    buffer = it->second.buffer;
    offset = address - it->first;
    remaining = it->second.size - offset;
    return true;
}

}  // namespace dxinsp
