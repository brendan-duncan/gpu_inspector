// Hooks on ID3D12GraphicsCommandList through ID3D12GraphicsCommandList10: every method the
// application can record. Each replacement has the same shape — look the original up, forward at
// once when the call is the library's own (Internal()), open a CommandScope so a validation
// message fired inside the forward is attributed to the command, forward, and when the list has a
// recorder (a capture is on, or DXINSP_RECORD_ALWAYS) serialize the arguments under their D3D12
// names and Record() the method. The handful of commands the capture must know about (pass
// boundaries, dispatches, bindings, bundles) also call into capture.h, before or after the
// forward as README.md ("Passes", "Bound buffers and textures") describes.
#include "hooks.h"

#include "capture.h"
#include "command_recorder.h"
#include "d3d12_enums.gen.h"
#include "d3d12_vtables.gen.h"
#include "descriptors.h"
#include "json.h"
#include "overdraw.h"
#include "raytracing.h"
#include "resources.h"
#include "serialize.h"
#include "shader_edit.h"
#include "tracker.h"

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxinsp
{
namespace
{

using List = ID3D12GraphicsCommandList10;

// The saved original of a method, for forwarding: ORIG(DrawInstanced)(This, ...).
#define ORIG(Method) Orig<PFN_ID3D12GraphicsCommandList10_##Method>(This, slot::ID3D12GraphicsCommandList10_##Method)

inline CaptureManager& Cap() { return CaptureManager::Get(); }
inline CommandRecorder* Rec(List* This) { return CaptureManager::Get().RecorderFor(This); }

// ---------------------------------------------------------------------------------------------
// Keeping a call so a measurement can issue the pass again (overdraw.h). Every call that shapes
// what a render pass rasterizes is kept beside the recorded command, as a closure holding its
// arguments with a reference on each object it names; barriers, queries and everything that does
// not change rasterization are not. Nothing is kept unless a capture with Overdraw or a pixel
// history is recording, which PassRecordingActive() answers with one atomic load.

inline void LogOp(CommandRecorder* rec, OpKey key, PassOp op)
{
    if (PassRecordingActive())
        LogPassOp(rec, key, std::move(op));
}

/** A reference on one of the application's objects, held for as long as a kept call may be issued. */
template <typename T>
inline ComPtr<T> Held(T* object)
{
    if (object)
        object->AddRef();
    return ComPtr<T>(object);
}

/** The list a kept call is issued on, as the newest interface: the object answered to it when the application called. */
inline List* AsList(ID3D12GraphicsCommandList* list) { return static_cast<List*>(list); }

/** Which root view setter a kept call is (RecordRootView serves all six). */
enum class RootView
{
    ConstantBuffer,
    ShaderResource,
    UnorderedAccess
};

// ---------------------------------------------------------------------------------------------
// Per-object facts looked up from the tracker's args, cached by pointer. The tracked id is kept
// beside the answer so an address the allocator hands out again for a different object misses.

struct CachedKind
{
    uint64_t id = 0;
    bool compute = false;
    uint32_t stride = 0;
};
std::mutex g_kindMutex;
std::unordered_map<const void*, CachedKind> g_pipelineKinds;
std::unordered_map<const void*, CachedKind> g_signatureStrides;

/** Whether the pipeline was created from a compute description (its args carry a "CS" stage). */
bool IsComputePipeline(ID3D12PipelineState* pipeline)
{
    if (!pipeline)
        return false;
    uint64_t id = Tracker::Get().IdOf(pipeline);
    std::lock_guard<std::mutex> lock(g_kindMutex);
    auto it = g_pipelineKinds.find(pipeline);
    if (it != g_pipelineKinds.end() && it->second.id == id)
        return it->second.compute;
    CachedKind kind;
    kind.id = id;
    TrackedObject obj;
    if (Tracker::Get().Find(pipeline, obj))
        kind.compute = obj.args.find("\"CS\":{") != std::string::npos;
    g_pipelineKinds[pipeline] = kind;
    return kind.compute;
}

/** The ByteStride of a command signature, from its tracked description; 64 KB when unknown. */
uint32_t SignatureStride(ID3D12CommandSignature* signature)
{
    constexpr uint32_t kFallback = 64 * 1024;
    if (!signature)
        return kFallback;
    uint64_t id = Tracker::Get().IdOf(signature);
    std::lock_guard<std::mutex> lock(g_kindMutex);
    auto it = g_signatureStrides.find(signature);
    if (it != g_signatureStrides.end() && it->second.id == id)
        return it->second.stride;
    CachedKind kind;
    kind.id = id;
    kind.stride = kFallback;
    TrackedObject obj;
    if (Tracker::Get().Find(signature, obj))
    {
        size_t at = obj.args.find("\"ByteStride\":");
        if (at != std::string::npos)
        {
            unsigned long long v = strtoull(obj.args.c_str() + at + 13, nullptr, 10);
            if (v > 0 && v < UINT32_MAX)
                kind.stride = (uint32_t)v;
        }
    }
    g_signatureStrides[signature] = kind;
    return kind.stride;
}

// ---------------------------------------------------------------------------------------------
// Serialization helpers shared by several commands

void WriteRects(JsonWriter& w, UINT count, const D3D12_RECT* rects)
{
    w.BeginArray();
    for (UINT i = 0; rects && i < count; ++i)
        Write(w, rects[i]);
    w.EndArray();
}

void WriteFloats(JsonWriter& w, const FLOAT* values, size_t count)
{
    if (!values)
    {
        w.Null();
        return;
    }
    w.BeginArray();
    for (size_t i = 0; i < count; ++i)
        w.Double(values[i]);
    w.EndArray();
}

void WriteUints(JsonWriter& w, const UINT* values, size_t count)
{
    if (!values)
    {
        w.Null();
        return;
    }
    w.BeginArray();
    for (size_t i = 0; i < count; ++i)
        w.Uint(values[i]);
    w.EndArray();
}

/** {"Subresource", "Range": {"Begin", "End"}} (AtomicCopyBuffer's dependent ranges; no serializer in serialize.h). */
void WriteSubresourceRange(JsonWriter& w, const D3D12_SUBRESOURCE_RANGE_UINT64& r)
{
    w.BeginObject();
    w.Key("Subresource");
    w.Uint(r.Subresource);
    w.Key("Range");
    w.BeginObject();
    w.Key("Begin");
    w.Uint(r.Range.Begin);
    w.Key("End");
    w.Uint(r.Range.End);
    w.EndObject();
    w.EndObject();
}

void WritePostbuildInfo(JsonWriter& w, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC& d)
{
    w.BeginObject();
    w.Key("DestBuffer");
    WriteGpuAddress(w, d.DestBuffer);
    w.Key("InfoType");
    w.Enum(ToString_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE(d.InfoType), d.InfoType);
    w.EndObject();
}

/** The ids of buffer ranges queued for read-back, as the pre-separated member the recorder merges into the command. */
std::string BufferDataExtra(const std::vector<uint32_t>& ids)
{
    std::string extra = ",\"bufferData\":[";
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i)
            extra += ',';
        extra += std::to_string(ids[i]);
    }
    extra += ']';
    return extra;
}

// ---------------------------------------------------------------------------------------------
// Event labels. BeginEvent/SetMarker take (Metadata, pData, Size) and the runtime does not care
// what they hold; PIX defines three encodings: 0 a UTF-16 string, 1 an ANSI string, and 2 the
// PIX3 blob (an 8-byte event header, an 8-byte color, then the string — after a string-info word
// in the older runtime, directly in the newer). The blob's string is found by looking for
// printable text at the offsets those layouts put it, which is what a viewer that only wants
// the label needs and survives a layout the header did not describe.

/** Printable text starting at `offset`, UTF-16 when the second byte of the first character is zero. */
bool TextAt(const uint8_t* bytes, size_t size, size_t offset, std::string& out)
{
    if (offset >= size)
        return false;
    uint8_t c = bytes[offset];
    if (c < 0x20 || c >= 0x7F)
        return false;
    size_t avail = size - offset;
    if (avail >= 2 && bytes[offset + 1] == 0)
    {
        std::wstring text(avail / 2, L'\0');
        memcpy(&text[0], bytes + offset, (avail / 2) * sizeof(wchar_t));
        size_t n = 0;
        while (n < text.size() && text[n])
            ++n;
        out = Narrow(text.c_str(), n);
    }
    else
    {
        size_t n = 0;
        while (n < avail && bytes[offset + n])
            ++n;
        out.assign((const char*)bytes + offset, n);
    }
    return true;
}

std::string EventLabel(UINT metadata, const void* data, UINT size)
{
    std::string label;
    if (!data || !size)
        return label;
    const uint8_t* bytes = (const uint8_t*)data;
    switch (metadata)
    {
        case 0:   // PIX_EVENT_UNICODE_VERSION
            TextAt(bytes, size, 0, label);
            break;
        case 1:
        {  // PIX_EVENT_ANSI_VERSION
            size_t n = 0;
            while (n < size && bytes[n])
                ++n;
            label.assign((const char*)bytes, n);
            break;
        }
        case 2:   // PIX_EVENT_PIX3BLOB_VERSION
            for (size_t offset : {16u, 24u, 8u})
                if (TextAt(bytes, size, offset, label))
                    break;
            break;
        default:
            for (size_t offset : {0u, 16u, 24u})
                if (TextAt(bytes, size, offset, label))
                    break;
            break;
    }
    return label;
}

// ---------------------------------------------------------------------------------------------
// Render targets: a CPU handle resolved through the descriptor tracker to the heap slot, its
// record and the resource, written into the command as {heap, index, resource, view} and turned
// into the BoundTarget the pass read-back works from.

struct ResolvedTarget
{
    bool located = false;
    HeapInfo heap;
    uint32_t index = 0;
    DescriptorRecord record;
};

ResolvedTarget ResolveHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    ResolvedTarget t;
    t.located = DescriptorTracker::Get().Locate(handle, t.heap, t.index);
    if (t.located)
        t.record = DescriptorTracker::Get().Get(t.heap.heap, t.index);
    return t;
}

void WriteTargetHandle(JsonWriter& w, D3D12_CPU_DESCRIPTOR_HANDLE handle, const ResolvedTarget& t)
{
    if (t.located)
    {
        w.Key("heap");
        WriteRef(w, t.heap.heap, "ID3D12DescriptorHeap");
        w.Key("index");
        w.Uint(t.index);
    }
    else
    {
        w.Key("ptr");
        w.String(Hex(handle.ptr));
    }
}

void WriteTargetResource(JsonWriter& w, const ResolvedTarget& t, bool depth)
{
    w.Key("resource");
    WriteRef(w, t.record.resource, "ID3D12Resource");
    w.Key("view");
    if (depth)
        Write(w, t.record.hasDesc ? &t.record.dsv : nullptr);
    else
        Write(w, t.record.hasDesc ? &t.record.rtv : nullptr);
}

/** OMSetRenderTargets' entry: {heap, index, resource, view}. */
void WriteTarget(JsonWriter& w, D3D12_CPU_DESCRIPTOR_HANDLE handle, const ResolvedTarget& t, bool depth)
{
    w.BeginObject();
    WriteTargetHandle(w, handle, t);
    WriteTargetResource(w, t, depth);
    w.EndObject();
}

/** Adds the target to the pass when its resource is known (nothing can be read back of one that is not). */
void AddTarget(std::vector<BoundTarget>& targets, D3D12_CPU_DESCRIPTOR_HANDLE handle, const ResolvedTarget& t,
    bool depth, uint32_t attachment)
{
    const DescriptorRecord& r = t.record;
    if (!r.resource)
        return;
    BoundTarget b;
    b.resource = r.resource;
    b.depth = depth;
    b.attachment = attachment;
    b.handle = handle;
    ResourceInfo info;
    bool haveInfo = ResourceTracker::Get().Get(r.resource, info);
    DXGI_FORMAT viewFormat = r.hasDesc ? (depth ? r.dsv.Format : r.rtv.Format) : DXGI_FORMAT_UNKNOWN;
    b.format = viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : (haveInfo ? info.desc.Format : DXGI_FORMAT_UNKNOWN);
    if (!r.hasDesc)
    {
        // A null description views mip 0 of every slice.
        if (haveInfo && info.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
            b.sliceCount = info.desc.DepthOrArraySize;
        targets.push_back(b);
        return;
    }
    if (depth)
    {
        const D3D12_DEPTH_STENCIL_VIEW_DESC& d = r.dsv;
        b.readOnlyDepth = (d.Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
        switch (d.ViewDimension)
        {
            case D3D12_DSV_DIMENSION_TEXTURE1D: b.mip = d.Texture1D.MipSlice; break;
            case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
                b.mip = d.Texture1DArray.MipSlice;
                b.firstSlice = d.Texture1DArray.FirstArraySlice;
                b.sliceCount = d.Texture1DArray.ArraySize;
                break;
            case D3D12_DSV_DIMENSION_TEXTURE2D: b.mip = d.Texture2D.MipSlice; break;
            case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
                b.mip = d.Texture2DArray.MipSlice;
                b.firstSlice = d.Texture2DArray.FirstArraySlice;
                b.sliceCount = d.Texture2DArray.ArraySize;
                break;
            case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
                b.firstSlice = d.Texture2DMSArray.FirstArraySlice;
                b.sliceCount = d.Texture2DMSArray.ArraySize;
                break;
            default: break;   // TEXTURE2DMS, UNKNOWN: mip 0, slice 0
        }
    }
    else
    {
        const D3D12_RENDER_TARGET_VIEW_DESC& d = r.rtv;
        switch (d.ViewDimension)
        {
            case D3D12_RTV_DIMENSION_TEXTURE1D: b.mip = d.Texture1D.MipSlice; break;
            case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
                b.mip = d.Texture1DArray.MipSlice;
                b.firstSlice = d.Texture1DArray.FirstArraySlice;
                b.sliceCount = d.Texture1DArray.ArraySize;
                break;
            case D3D12_RTV_DIMENSION_TEXTURE2D: b.mip = d.Texture2D.MipSlice; break;
            case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
                b.mip = d.Texture2DArray.MipSlice;
                b.firstSlice = d.Texture2DArray.FirstArraySlice;
                b.sliceCount = d.Texture2DArray.ArraySize;
                break;
            case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
                b.firstSlice = d.Texture2DMSArray.FirstArraySlice;
                b.sliceCount = d.Texture2DMSArray.ArraySize;
                break;
            case D3D12_RTV_DIMENSION_TEXTURE3D:
                b.mip = d.Texture3D.MipSlice;
                b.firstSlice = d.Texture3D.FirstWSlice;
                b.sliceCount = d.Texture3D.WSize;
                break;
            default: break;   // TEXTURE2DMS, BUFFER, UNKNOWN
        }
    }
    if (b.sliceCount == 0)
        b.sliceCount = 1;
    targets.push_back(b);
}

/** RTV increment of the device, for RTsSingleHandleToDescriptorRange when the first handle is in no tracked heap. */
uint32_t RtvIncrement(CommandRecorder* rec)
{
    if (!rec || !rec->device())
        return 0;
    ScopedInternal internal;
    return rec->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

// ---------------------------------------------------------------------------------------------
// Root parameters: the three shapes every graphics/compute pair shares.

/**
 * A root argument's key: the parameter index, and for root constants the offset within it, so two
 * SetGraphicsRoot32BitConstants that fill different parts of one parameter do not undo each other.
 */
inline uint32_t RootSlot(UINT index, UINT offsetIn32BitValues = 0)
{
    return (index << 16) | (offsetIn32BitValues & 0xFFFF);
}

void RecordRootTable(CommandRecorder* rec, const char* method, bool compute, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
    Args args;
    args.u("RootParameterIndex", index).gpuHandle("BaseDescriptor", base);
    rec->Record(method, args.str());
    Cap().SnapshotRootTable(rec, compute, index, base);
    if (compute)
        return;   // only the graphics root shapes what a render pass rasterizes
    LogOp(rec, OpKey::Slot(ops::kGraphicsRoot, RootSlot(index)),
        [index, base](ID3D12GraphicsCommandList* list, PassReplay&) { list->SetGraphicsRootDescriptorTable(index, base); });
}

void RecordRootView(CommandRecorder* rec, const char* method, bool compute, RootView kind, UINT index,
    D3D12_GPU_VIRTUAL_ADDRESS address)
{
    Args args;
    args.u("RootParameterIndex", index).address("BufferLocation", address);
    rec->Record(method, args.str());
    Cap().SnapshotRootView(rec, compute, index, address);
    if (compute)
        return;
    LogOp(rec, OpKey::Slot(ops::kGraphicsRoot, RootSlot(index)),
        [kind, index, address](ID3D12GraphicsCommandList* list, PassReplay&) {
            switch (kind)
            {
                case RootView::ConstantBuffer: list->SetGraphicsRootConstantBufferView(index, address); break;
                case RootView::ShaderResource: list->SetGraphicsRootShaderResourceView(index, address); break;
                case RootView::UnorderedAccess: list->SetGraphicsRootUnorderedAccessView(index, address); break;
            }
        });
}

/** Root constants under their D3D12 names and, beside them, the UI's push-constant shape (README.md). */
void RecordRootConstants(CommandRecorder* rec, const char* method, bool compute, UINT index, UINT num,
    const void* data, UINT destOffset)
{
    size_t bytes = (size_t)num * 4;
    Args args;
    args.u("RootParameterIndex", index).u("Num32BitValuesToSet", num).u("DestOffsetIn32BitValues", destOffset).bytes("pSrcData", data, bytes).s("stageFlags", compute ? "compute" : "graphics").u("offset", (uint64_t)destOffset * 4).u("size", bytes).bytes("pValues", data, bytes);
    const std::shared_ptr<const RootSignatureInfo>& layout = compute ? rec->state().computeLayout : rec->state().graphicsLayout;
    if (layout && index < layout->parameters.size() &&
        layout->parameters[index].type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
    {
        args.u("register", layout->parameters[index].shaderRegister).u("space", layout->parameters[index].space);
    }
    rec->Record(method, args.str());
    if (compute || !data || !num)
        return;
    std::vector<UINT> values(static_cast<const UINT*>(data), static_cast<const UINT*>(data) + num);
    LogOp(rec, OpKey::Slot(ops::kGraphicsRoot, RootSlot(index, destOffset)),
        [index, destOffset, values = std::move(values)](ID3D12GraphicsCommandList* list, PassReplay&) {
            list->SetGraphicsRoot32BitConstants(index, (UINT)values.size(), values.data(), destOffset);
        });
}

// ---------------------------------------------------------------------------------------------
// Lifetime: Reset, Close, ClearState

HRESULT STDMETHODCALLTYPE Hook_Reset(List* This, ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState)
{
    auto orig = ORIG(Reset);
    if (Internal())
        return orig(This, pAllocator, pInitialState);
    // Forwarded with the edited pipeline, recorded with the application's.
    HRESULT hr = orig(This, pAllocator, ShaderEditor::Get().Substitute(pInitialState));
    Log("list %p Reset(allocator %p, pso %p) -> %s", (void*)This, (void*)pAllocator, (void*)pInitialState, HrText(hr).c_str());
    HookCommandList(This);   // the runtime may have swapped the vtable with the list's state (see BeginRenderPass)
    if (FAILED(hr))
        return hr;
    ResourceTracker::Get().OnListReset(This);
    D3D12_COMMAND_LIST_TYPE type;
    {
        ScopedInternal internal;
        type = This->GetType();
    }
    Cap().OnListReset(DeviceOf(This), This, type, type == D3D12_COMMAND_LIST_TYPE_BUNDLE, pInitialState);
    if (CommandRecorder* rec = Rec(This))
    {
        Args args;
        args.ref("pAllocator", pAllocator, "ID3D12CommandAllocator").ref("pInitialState", pInitialState, "ID3D12PipelineState");
        rec->Record("Reset", args.str());
        rec->state().pipeline = pInitialState;
        // Reset's initial pipeline is the only bind many engines make: the test application, and
        // Unity's D3D12 player, both hand the pipeline to Reset rather than SetPipelineState.
        if (pInitialState)
        {
            LogOp(rec, OpKey::Replace(ops::kPipeline), [held = Held(pInitialState)](ID3D12GraphicsCommandList* list, PassReplay& replay) {
                replay.SetPipeline(list, held.get());
            });
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Close(List* This)
{
    auto orig = ORIG(Close);
    if (Internal())
        return orig(This);
    CommandRecorder* rec = CaptureManager::Get().RecorderIfAny(This);
    // A table nothing drew with still shows what it named.
    if (rec)
        rec->FlushSnapshots(true, true);
    // Ends the open passes and appends their read-backs, so the stream reads [..., EndRenderTargets, Close].
    Cap().OnBeforeClose(This);
    CommandScope scope(rec);
    HRESULT hr = orig(This);
    Log("list %p Close -> %s", (void*)This, HrText(hr).c_str());
    HookCommandList(This);
    if (rec)
        rec->Record("Close", "");
    return hr;
}

void STDMETHODCALLTYPE Hook_ClearState(List* This, ID3D12PipelineState* pPipelineState)
{
    auto orig = ORIG(ClearState);
    if (Internal())
        return orig(This, pPipelineState);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, ShaderEditor::Get().Substitute(pPipelineState));
    if (!rec)
        return;
    Args args;
    args.ref("pPipelineState", pPipelineState, "ID3D12PipelineState");
    rec->Record("ClearState", args.str());
    // ClearState unbinds everything except the pipeline it is given, so nothing kept of the list
    // is still in effect.
    OnMeasuredListClearState(This);
    if (pPipelineState)
    {
        LogOp(rec, OpKey::Replace(ops::kPipeline),
            [held = Held(pPipelineState)](ID3D12GraphicsCommandList* list, PassReplay& replay) { replay.SetPipeline(list, held.get()); });
    }
    ListState& s = rec->state();
    uint32_t depth = s.appQueryDepth;
    s = ListState{};
    s.pipeline = pPipelineState;
    s.appQueryDepth = depth;
}

// ---------------------------------------------------------------------------------------------
// Draws and dispatches

void STDMETHODCALLTYPE Hook_DrawInstanced(List* This, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
    auto orig = ORIG(DrawInstanced);
    if (Internal())
        return orig(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    CommandRecorder* rec = Rec(This);
    if (rec)
        rec->FlushSnapshots(true, false);
    CommandScope scope(rec);
    const uint32_t queries = Cap().BeginDrawQueries(rec);
    orig(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    if (!rec)
        return;
    Args args;
    args.u("VertexCountPerInstance", VertexCountPerInstance).u("InstanceCount", InstanceCount).u("StartVertexLocation", StartVertexLocation).u("StartInstanceLocation", StartInstanceLocation);
    rec->Record("DrawInstanced", args.str());
    Cap().OnDraw(rec);
    Cap().EndDrawQueries(rec, queries, false);
    LogOp(rec, OpKey::DrawCall(),
        [VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation](ID3D12GraphicsCommandList* list,
            PassReplay& replay) {
            replay.IssueDraw(list, [&](ID3D12GraphicsCommandList* on) {
                on->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
            });
        });
}

void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(List* This, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
    auto orig = ORIG(DrawIndexedInstanced);
    if (Internal())
        return orig(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    CommandRecorder* rec = Rec(This);
    if (rec)
        rec->FlushSnapshots(true, false);
    CommandScope scope(rec);
    const uint32_t queries = Cap().BeginDrawQueries(rec);
    orig(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    if (!rec)
        return;
    Args args;
    args.u("IndexCountPerInstance", IndexCountPerInstance).u("InstanceCount", InstanceCount).u("StartIndexLocation", StartIndexLocation).i("BaseVertexLocation", BaseVertexLocation).u("StartInstanceLocation", StartInstanceLocation);
    rec->Record("DrawIndexedInstanced", args.str());
    Cap().OnDraw(rec);
    Cap().EndDrawQueries(rec, queries, false);
    LogOp(rec, OpKey::DrawCall(),
        [IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
            StartInstanceLocation](ID3D12GraphicsCommandList* list, PassReplay& replay) {
            replay.IssueDraw(list, [&](ID3D12GraphicsCommandList* on) {
                on->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                    StartInstanceLocation);
            });
        });
}

void STDMETHODCALLTYPE Hook_Dispatch(List* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
    auto orig = ORIG(Dispatch);
    if (Internal())
        return orig(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnBeforeDispatch(rec);
    if (rec)
        rec->FlushSnapshots(false, true);
    CommandScope scope(rec);
    const uint32_t queries = Cap().BeginDrawQueries(rec);
    orig(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    if (!rec)
        return;
    Args args;
    args.u("ThreadGroupCountX", ThreadGroupCountX).u("ThreadGroupCountY", ThreadGroupCountY).u("ThreadGroupCountZ", ThreadGroupCountZ);
    rec->Record("Dispatch", args.str());
    Cap().EndDrawQueries(rec, queries, true);
}

void STDMETHODCALLTYPE Hook_DispatchMesh(List* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
    auto orig = ORIG(DispatchMesh);
    if (Internal())
        return orig(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    CommandRecorder* rec = Rec(This);
    if (rec)
        rec->FlushSnapshots(true, false);
    CommandScope scope(rec);
    const uint32_t queries = Cap().BeginDrawQueries(rec);
    orig(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    if (!rec)
        return;
    Args args;
    args.u("ThreadGroupCountX", ThreadGroupCountX).u("ThreadGroupCountY", ThreadGroupCountY).u("ThreadGroupCountZ", ThreadGroupCountZ);
    rec->Record("DispatchMesh", args.str());
    Cap().OnDraw(rec);
    Cap().EndDrawQueries(rec, queries, false);
    LogOp(rec, OpKey::DrawCall(),
        [ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ](ID3D12GraphicsCommandList* list, PassReplay& replay) {
            replay.IssueDraw(list, [&](ID3D12GraphicsCommandList* on) {
                AsList(on)->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
            });
        });
}

void STDMETHODCALLTYPE Hook_DispatchRays(List* This, const D3D12_DISPATCH_RAYS_DESC* pDesc)
{
    auto orig = ORIG(DispatchRays);
    if (Internal())
        return orig(This, pDesc);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnBeforeDispatch(rec);
    if (rec)
        rec->FlushSnapshots(false, true);
    CommandScope scope(rec);
    const uint32_t queries = Cap().BeginDrawQueries(rec);
    orig(This, pDesc);
    if (!rec)
        return;
    Args args;
    if (pDesc)
        Write(args.key("pDesc"), *pDesc);
    else
        args.null("pDesc");
    rec->Record("DispatchRays", args.str());
    // The table's contents, which is the only thing that says which shader each record runs.
    if (pDesc)
    {
        const D3D12_DISPATCH_RAYS_DESC desc = *pDesc;
        rec->SetSnapshotOnLast([desc](CommandRecorder* on) { return NoteDispatchRays(on, desc); });
    }
    Cap().EndDrawQueries(rec, queries, true);
}

void STDMETHODCALLTYPE Hook_DispatchGraph(List* This, const D3D12_DISPATCH_GRAPH_DESC* pDesc)
{
    auto orig = ORIG(DispatchGraph);
    if (Internal())
        return orig(This, pDesc);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnBeforeDispatch(rec);
    if (rec)
        rec->FlushSnapshots(false, true);
    CommandScope scope(rec);
    orig(This, pDesc);
    if (!rec)
        return;
    Args args;
    if (pDesc)
        Write(args.key("pDesc"), *pDesc);
    else
        args.null("pDesc");
    rec->Record("DispatchGraph", args.str());
}

void STDMETHODCALLTYPE Hook_ExecuteIndirect(List* This, ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset)
{
    auto orig = ORIG(ExecuteIndirect);
    if (Internal())
        return orig(This, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
    CommandRecorder* rec = Rec(This);
    // Inside a render pass an indirect execution is a batch of draws; outside one it is compute work.
    bool inPass = rec && rec->pass().active;
    if (rec && !inPass)
        Cap().OnBeforeDispatch(rec);
    if (rec)
        rec->FlushSnapshots(true, true);   // the signature may draw or dispatch
    CommandScope scope(rec);
    const uint32_t queries = Cap().BeginDrawQueries(rec);
    orig(This, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
    if (!rec)
        return;
    Args args;
    args.ref("pCommandSignature", pCommandSignature, "ID3D12CommandSignature").u("MaxCommandCount", MaxCommandCount).ref("pArgumentBuffer", pArgumentBuffer, "ID3D12Resource").u("ArgumentBufferOffset", ArgumentBufferOffset).ref("pCountBuffer", pCountBuffer, "ID3D12Resource").u("CountBufferOffset", CountBufferOffset);
    rec->Record("ExecuteIndirect", args.str());
    Cap().EndDrawQueries(rec, queries, !inPass);
    std::vector<uint32_t> ids;
    UINT64 size = (UINT64)MaxCommandCount * SignatureStride(pCommandSignature);
    ids.push_back(pArgumentBuffer ? Cap().QueueBufferCapture(rec, pArgumentBuffer, ArgumentBufferOffset, size) : 0);
    if (pCountBuffer)
        ids.push_back(Cap().QueueBufferCapture(rec, pCountBuffer, CountBufferOffset, 4));
    rec->SetExtraOnLast(BufferDataExtra(ids));
    if (inPass)
        Cap().OnDraw(rec);
    // The draws are in a buffer the GPU reads, so a measurement cannot issue them one at a time
    // or with a pipeline of its own: they are reported as not measured.
    if (inPass)
        LogOp(rec, OpKey::DrawCall(), [](ID3D12GraphicsCommandList*, PassReplay& replay) { replay.Skip(); });
}

// ---------------------------------------------------------------------------------------------
// Copies, resolves and clears

void STDMETHODCALLTYPE Hook_CopyBufferRegion(List* This, ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes)
{
    auto orig = ORIG(CopyBufferRegion);
    if (Internal())
        return orig(This, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
    if (!rec)
        return;
    Args args;
    args.ref("pDstBuffer", pDstBuffer, "ID3D12Resource").u("DstOffset", DstOffset).ref("pSrcBuffer", pSrcBuffer, "ID3D12Resource").u("SrcOffset", SrcOffset).u("NumBytes", NumBytes);
    rec->Record("CopyBufferRegion", args.str());
    // What the copy reads, whole: an engine fills its per-frame constant buffers this way (Unity
    // does, from one upload buffer), and a replay that copies from a source it has nothing for
    // writes zeros over constants it had right.
    rec->SetExtraOnLast(BufferDataExtra({Cap().QueueBufferCapture(rec, pSrcBuffer, SrcOffset, NumBytes, true)}));
}

void STDMETHODCALLTYPE Hook_CopyTextureRegion(List* This, const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox)
{
    auto orig = ORIG(CopyTextureRegion);
    if (Internal())
        return orig(This, pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
    if (!rec)
        return;
    Args args;
    if (pDst)
        Write(args.key("pDst"), *pDst);
    else
        args.null("pDst");
    args.u("DstX", DstX).u("DstY", DstY).u("DstZ", DstZ);
    if (pSrc)
        Write(args.key("pSrc"), *pSrc);
    else
        args.null("pSrc");
    Write(args.key("pSrcBox"), pSrcBox);
    rec->Record("CopyTextureRegion", args.str());
    // A texture it copies from is read. What it writes is a region, which leaves the rest as it was.
    if (pSrc && pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        rec->NoteRead(pSrc->pResource);
    // A texture filled from a buffer (an upload): the rows the copy reads, whole, as above. The
    // footprint's height in rows is an upper bound for a block-compressed format, which the
    // buffer's end clips.
    if (pSrc && pSrc->pResource && pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
    {
        const D3D12_SUBRESOURCE_FOOTPRINT& f = pSrc->PlacedFootprint.Footprint;
        const UINT64 bytes = (UINT64)f.RowPitch * std::max<UINT>(f.Height, 1) * std::max<UINT>(f.Depth, 1);
        rec->SetExtraOnLast(BufferDataExtra({Cap().QueueBufferCapture(rec, pSrc->pResource, pSrc->PlacedFootprint.Offset, bytes, true)}));
    }
}

void STDMETHODCALLTYPE Hook_CopyResource(List* This, ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource)
{
    auto orig = ORIG(CopyResource);
    if (Internal())
        return orig(This, pDstResource, pSrcResource);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDstResource, pSrcResource);
    if (!rec)
        return;
    Args args;
    args.ref("pDstResource", pDstResource, "ID3D12Resource").ref("pSrcResource", pSrcResource, "ID3D12Resource");
    rec->Record("CopyResource", args.str());
    rec->NoteRead(pSrcResource);
    rec->NoteWritten(pDstResource);
    // A buffer copied whole is read whole (QueueBufferCapture takes nothing of a texture).
    if (const uint32_t id = Cap().QueueBufferCapture(rec, pSrcResource, 0, UINT64_MAX, true))
        rec->SetExtraOnLast(BufferDataExtra({id}));
}

void STDMETHODCALLTYPE Hook_CopyTiles(List* This, ID3D12Resource* pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE* pTileRegionSize, ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags)
{
    auto orig = ORIG(CopyTiles);
    if (Internal())
        return orig(This, pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags);
    if (!rec)
        return;
    Args args;
    args.ref("pTiledResource", pTiledResource, "ID3D12Resource");
    if (pTileRegionStartCoordinate)
        Write(args.key("pTileRegionStartCoordinate"), *pTileRegionStartCoordinate);
    else
        args.null("pTileRegionStartCoordinate");
    if (pTileRegionSize)
        Write(args.key("pTileRegionSize"), *pTileRegionSize);
    else
        args.null("pTileRegionSize");
    args.ref("pBuffer", pBuffer, "ID3D12Resource").u("BufferStartOffsetInBytes", BufferStartOffsetInBytes);
    Flags_D3D12_TILE_COPY_FLAGS(args.key("Flags"), Flags);
    rec->Record("CopyTiles", args.str());
}

void STDMETHODCALLTYPE Hook_ResolveSubresource(List* This, ID3D12Resource* pDstResource, UINT DstSubresource, ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
    auto orig = ORIG(ResolveSubresource);
    if (Internal())
        return orig(This, pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
    if (!rec)
        return;
    Args args;
    args.ref("pDstResource", pDstResource, "ID3D12Resource").u("DstSubresource", DstSubresource).ref("pSrcResource", pSrcResource, "ID3D12Resource").u("SrcSubresource", SrcSubresource).e("Format", ToString_DXGI_FORMAT(Format), Format);
    rec->Record("ResolveSubresource", args.str());
    rec->NoteRead(pSrcResource);
    rec->NoteWritten(pDstResource);
}

void STDMETHODCALLTYPE Hook_ResolveSubresourceRegion(List* This, ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource* pSrcResource, UINT SrcSubresource, D3D12_RECT* pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode)
{
    auto orig = ORIG(ResolveSubresourceRegion);
    if (Internal())
        return orig(This, pDstResource, DstSubresource, DstX, DstY, pSrcResource, SrcSubresource, pSrcRect, Format, ResolveMode);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDstResource, DstSubresource, DstX, DstY, pSrcResource, SrcSubresource, pSrcRect, Format, ResolveMode);
    if (!rec)
        return;
    Args args;
    args.ref("pDstResource", pDstResource, "ID3D12Resource").u("DstSubresource", DstSubresource).u("DstX", DstX).u("DstY", DstY).ref("pSrcResource", pSrcResource, "ID3D12Resource").u("SrcSubresource", SrcSubresource);
    if (pSrcRect)
        Write(args.key("pSrcRect"), *pSrcRect);
    else
        args.null("pSrcRect");
    args.e("Format", ToString_DXGI_FORMAT(Format), Format).e("ResolveMode", ToString_D3D12_RESOLVE_MODE(ResolveMode), ResolveMode);
    rec->Record("ResolveSubresourceRegion", args.str());
}

void RecordAtomicCopy(CommandRecorder* rec, const char* method, ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource* const* ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges)
{
    Args args;
    args.ref("pDstBuffer", pDstBuffer, "ID3D12Resource").u("DstOffset", DstOffset).ref("pSrcBuffer", pSrcBuffer, "ID3D12Resource").u("SrcOffset", SrcOffset).u("Dependencies", Dependencies);
    JsonWriter& w = args.key("ppDependentResources");
    w.BeginArray();
    for (UINT i = 0; ppDependentResources && i < Dependencies; ++i)
        WriteRef(w, ppDependentResources[i], "ID3D12Resource");
    w.EndArray();
    args.key("pDependentSubresourceRanges");
    w.BeginArray();
    for (UINT i = 0; pDependentSubresourceRanges && i < Dependencies; ++i)
        WriteSubresourceRange(w, pDependentSubresourceRanges[i]);
    w.EndArray();
    rec->Record(method, args.str());
}

void STDMETHODCALLTYPE Hook_AtomicCopyBufferUINT(List* This, ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource* const* ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges)
{
    auto orig = ORIG(AtomicCopyBufferUINT);
    if (Internal())
        return orig(This, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
    if (rec)
        RecordAtomicCopy(rec, "AtomicCopyBufferUINT", pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
}

void STDMETHODCALLTYPE Hook_AtomicCopyBufferUINT64(List* This, ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource* const* ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges)
{
    auto orig = ORIG(AtomicCopyBufferUINT64);
    if (Internal())
        return orig(This, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
    if (rec)
        RecordAtomicCopy(rec, "AtomicCopyBufferUINT64", pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
}

void STDMETHODCALLTYPE Hook_WriteBufferImmediate(List* This, UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE* pModes)
{
    auto orig = ORIG(WriteBufferImmediate);
    if (Internal())
        return orig(This, Count, pParams, pModes);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, Count, pParams, pModes);
    if (!rec)
        return;
    Args args;
    args.u("Count", Count);
    JsonWriter& w = args.key("pParams");
    w.BeginArray();
    for (UINT i = 0; pParams && i < Count; ++i)
        Write(w, pParams[i]);
    w.EndArray();
    args.key("pModes");
    if (!pModes)
    {
        w.Null();
    }
    else
    {
        w.BeginArray();
        for (UINT i = 0; i < Count; ++i)
            w.Enum(ToString_D3D12_WRITEBUFFERIMMEDIATE_MODE(pModes[i]), pModes[i]);
        w.EndArray();
    }
    rec->Record("WriteBufferImmediate", args.str());
}

void STDMETHODCALLTYPE Hook_ClearDepthStencilView(List* This, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects)
{
    auto orig = ORIG(ClearDepthStencilView);
    if (Internal())
        return orig(This, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    if (!rec)
        return;
    ResolvedTarget t = ResolveHandle(DepthStencilView);
    Args args;
    args.cpuHandle("DepthStencilView", DepthStencilView).ref("resource", t.record.resource, "ID3D12Resource");
    Flags_D3D12_CLEAR_FLAGS(args.key("ClearFlags"), ClearFlags);
    args.d("Depth", Depth).u("Stencil", Stencil).u("NumRects", NumRects);
    WriteRects(args.key("pRects"), NumRects, pRects);
    rec->Record("ClearDepthStencilView", args.str());
    // A whole depth clear writes the depth the pass would otherwise load.
    if (NumRects == 0 && (ClearFlags & D3D12_CLEAR_FLAG_DEPTH))
        rec->NoteWritten(t.record.resource);
    // A D3D12 pass has no load action: a clear inside it is what a measurement's copy of the
    // attachment has to be given too.
    std::vector<D3D12_RECT> rects(pRects, pRects + (pRects ? NumRects : 0));
    LogOp(rec, OpKey::Ordered(ops::kClear),
        [DepthStencilView, ClearFlags, Depth, Stencil, rects = std::move(rects)](ID3D12GraphicsCommandList* list,
            PassReplay& replay) {
            replay.ClearDepthStencil(list, DepthStencilView, ClearFlags, Depth, Stencil, (UINT)rects.size(),
                rects.empty() ? nullptr : rects.data());
        });
}

void STDMETHODCALLTYPE Hook_ClearRenderTargetView(List* This, D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects)
{
    auto orig = ORIG(ClearRenderTargetView);
    if (Internal())
        return orig(This, RenderTargetView, ColorRGBA, NumRects, pRects);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RenderTargetView, ColorRGBA, NumRects, pRects);
    if (!rec)
        return;
    ResolvedTarget t = ResolveHandle(RenderTargetView);
    Args args;
    args.cpuHandle("RenderTargetView", RenderTargetView).ref("resource", t.record.resource, "ID3D12Resource");
    WriteFloats(args.key("ColorRGBA"), ColorRGBA, 4);
    args.u("NumRects", NumRects);
    WriteRects(args.key("pRects"), NumRects, pRects);
    rec->Record("ClearRenderTargetView", args.str());
    if (NumRects == 0)
        rec->NoteWritten(t.record.resource);
    std::vector<D3D12_RECT> rects(pRects, pRects + (pRects ? NumRects : 0));
    std::array<FLOAT, 4> color{};
    if (ColorRGBA)
        std::copy(ColorRGBA, ColorRGBA + 4, color.begin());
    LogOp(rec, OpKey::Ordered(ops::kClear),
        [RenderTargetView, color, rects = std::move(rects)](ID3D12GraphicsCommandList* list, PassReplay& replay) {
            replay.ClearTarget(list, RenderTargetView, color.data(), (UINT)rects.size(),
                rects.empty() ? nullptr : rects.data());
        });
}

void STDMETHODCALLTYPE Hook_ClearUnorderedAccessViewUint(List* This, D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects)
{
    auto orig = ORIG(ClearUnorderedAccessViewUint);
    if (Internal())
        return orig(This, ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
    if (!rec)
        return;
    Args args;
    args.gpuHandle("ViewGPUHandleInCurrentHeap", ViewGPUHandleInCurrentHeap).cpuHandle("ViewCPUHandle", ViewCPUHandle).ref("pResource", pResource, "ID3D12Resource");
    WriteUints(args.key("Values"), Values, 4);
    args.u("NumRects", NumRects);
    WriteRects(args.key("pRects"), NumRects, pRects);
    rec->Record("ClearUnorderedAccessViewUint", args.str());
}

void STDMETHODCALLTYPE Hook_ClearUnorderedAccessViewFloat(List* This, D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects)
{
    auto orig = ORIG(ClearUnorderedAccessViewFloat);
    if (Internal())
        return orig(This, ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
    if (!rec)
        return;
    Args args;
    args.gpuHandle("ViewGPUHandleInCurrentHeap", ViewGPUHandleInCurrentHeap).cpuHandle("ViewCPUHandle", ViewCPUHandle).ref("pResource", pResource, "ID3D12Resource");
    WriteFloats(args.key("Values"), Values, 4);
    args.u("NumRects", NumRects);
    WriteRects(args.key("pRects"), NumRects, pRects);
    rec->Record("ClearUnorderedAccessViewFloat", args.str());
}

void STDMETHODCALLTYPE Hook_DiscardResource(List* This, ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion)
{
    auto orig = ORIG(DiscardResource);
    if (Internal())
        return orig(This, pResource, pRegion);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pResource, pRegion);
    if (!rec)
        return;
    Args args;
    args.ref("pResource", pResource, "ID3D12Resource");
    Write(args.key("pRegion"), pRegion);
    rec->Record("DiscardResource", args.str());
}

// ---------------------------------------------------------------------------------------------
// Fixed-function state

void STDMETHODCALLTYPE Hook_IASetPrimitiveTopology(List* This, D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology)
{
    auto orig = ORIG(IASetPrimitiveTopology);
    if (Internal())
        return orig(This, PrimitiveTopology);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, PrimitiveTopology);
    if (!rec)
        return;
    Args args;
    args.e("PrimitiveTopology", ToString_D3D_PRIMITIVE_TOPOLOGY(PrimitiveTopology), PrimitiveTopology);
    rec->Record("IASetPrimitiveTopology", args.str());
    rec->state().topology = PrimitiveTopology;
    LogOp(rec, OpKey::Replace(ops::kTopology), [PrimitiveTopology](ID3D12GraphicsCommandList* list, PassReplay&) {
        list->IASetPrimitiveTopology(PrimitiveTopology);
    });
}

void STDMETHODCALLTYPE Hook_RSSetViewports(List* This, UINT NumViewports, const D3D12_VIEWPORT* pViewports)
{
    auto orig = ORIG(RSSetViewports);
    if (Internal())
        return orig(This, NumViewports, pViewports);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, NumViewports, pViewports);
    if (!rec)
        return;
    Args args;
    args.u("NumViewports", NumViewports);
    JsonWriter& w = args.key("pViewports");
    w.BeginArray();
    for (UINT i = 0; pViewports && i < NumViewports; ++i)
        Write(w, pViewports[i]);
    w.EndArray();
    rec->Record("RSSetViewports", args.str());
    std::vector<D3D12_VIEWPORT> viewports(pViewports, pViewports + (pViewports ? NumViewports : 0));
    LogOp(rec, OpKey::Replace(ops::kViewports), [viewports = std::move(viewports)](ID3D12GraphicsCommandList* list, PassReplay&) {
        list->RSSetViewports((UINT)viewports.size(), viewports.empty() ? nullptr : viewports.data());
    });
}

void STDMETHODCALLTYPE Hook_RSSetScissorRects(List* This, UINT NumRects, const D3D12_RECT* pRects)
{
    auto orig = ORIG(RSSetScissorRects);
    if (Internal())
        return orig(This, NumRects, pRects);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, NumRects, pRects);
    if (!rec)
        return;
    Args args;
    args.u("NumRects", NumRects);
    WriteRects(args.key("pRects"), NumRects, pRects);
    rec->Record("RSSetScissorRects", args.str());
    std::vector<D3D12_RECT> rects(pRects, pRects + (pRects ? NumRects : 0));
    LogOp(rec, OpKey::Replace(ops::kScissors), [rects = std::move(rects)](ID3D12GraphicsCommandList* list, PassReplay& replay) {
        replay.SetScissors(list, (UINT)rects.size(), rects.empty() ? nullptr : rects.data());
    });
}

void STDMETHODCALLTYPE Hook_OMSetBlendFactor(List* This, const FLOAT BlendFactor[4])
{
    auto orig = ORIG(OMSetBlendFactor);
    if (Internal())
        return orig(This, BlendFactor);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, BlendFactor);
    if (!rec)
        return;
    Args args;
    WriteFloats(args.key("BlendFactor"), BlendFactor, 4);
    rec->Record("OMSetBlendFactor", args.str());
    std::array<FLOAT, 4> factor{};
    if (BlendFactor)
        std::copy(BlendFactor, BlendFactor + 4, factor.begin());
    const bool given = BlendFactor != nullptr;
    LogOp(rec, OpKey::Replace(ops::kBlendFactor), [factor, given](ID3D12GraphicsCommandList* list, PassReplay&) {
        list->OMSetBlendFactor(given ? factor.data() : nullptr);
    });
}

void STDMETHODCALLTYPE Hook_OMSetStencilRef(List* This, UINT StencilRef)
{
    auto orig = ORIG(OMSetStencilRef);
    if (Internal())
        return orig(This, StencilRef);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, StencilRef);
    if (!rec)
        return;
    Args args;
    args.u("StencilRef", StencilRef);
    rec->Record("OMSetStencilRef", args.str());
    LogOp(rec, OpKey::Replace(ops::kStencilRef), [StencilRef](ID3D12GraphicsCommandList* list, PassReplay&) {
        list->OMSetStencilRef(StencilRef);
    });
}

void STDMETHODCALLTYPE Hook_OMSetDepthBounds(List* This, FLOAT Min, FLOAT Max)
{
    auto orig = ORIG(OMSetDepthBounds);
    if (Internal())
        return orig(This, Min, Max);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, Min, Max);
    if (!rec)
        return;
    Args args;
    args.d("Min", Min).d("Max", Max);
    rec->Record("OMSetDepthBounds", args.str());
    LogOp(rec, OpKey::Replace(ops::kDepthBounds), [Min, Max](ID3D12GraphicsCommandList* list, PassReplay&) {
        AsList(list)->OMSetDepthBounds(Min, Max);
    });
}

void STDMETHODCALLTYPE Hook_SetSamplePositions(List* This, UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION* pSamplePositions)
{
    auto orig = ORIG(SetSamplePositions);
    if (Internal())
        return orig(This, NumSamplesPerPixel, NumPixels, pSamplePositions);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, NumSamplesPerPixel, NumPixels, pSamplePositions);
    if (!rec)
        return;
    Args args;
    args.u("NumSamplesPerPixel", NumSamplesPerPixel).u("NumPixels", NumPixels);
    JsonWriter& w = args.key("pSamplePositions");
    w.BeginArray();
    for (UINT i = 0; pSamplePositions && i < NumSamplesPerPixel * NumPixels; ++i)
        Write(w, pSamplePositions[i]);
    w.EndArray();
    rec->Record("SetSamplePositions", args.str());
}

void STDMETHODCALLTYPE Hook_SetViewInstanceMask(List* This, UINT Mask)
{
    auto orig = ORIG(SetViewInstanceMask);
    if (Internal())
        return orig(This, Mask);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, Mask);
    if (!rec)
        return;
    Args args;
    args.u("Mask", Mask);
    rec->Record("SetViewInstanceMask", args.str());
}

void STDMETHODCALLTYPE Hook_RSSetShadingRate(List* This, D3D12_SHADING_RATE baseShadingRate, const D3D12_SHADING_RATE_COMBINER* combiners)
{
    auto orig = ORIG(RSSetShadingRate);
    if (Internal())
        return orig(This, baseShadingRate, combiners);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, baseShadingRate, combiners);
    if (!rec)
        return;
    Args args;
    args.e("baseShadingRate", ToString_D3D12_SHADING_RATE(baseShadingRate), baseShadingRate);
    JsonWriter& w = args.key("combiners");
    if (!combiners)
    {
        w.Null();
    }
    else
    {
        w.BeginArray();
        for (UINT i = 0; i < D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT; ++i)
            w.Enum(ToString_D3D12_SHADING_RATE_COMBINER(combiners[i]), combiners[i]);
        w.EndArray();
    }
    rec->Record("RSSetShadingRate", args.str());
}

void STDMETHODCALLTYPE Hook_RSSetShadingRateImage(List* This, ID3D12Resource* shadingRateImage)
{
    auto orig = ORIG(RSSetShadingRateImage);
    if (Internal())
        return orig(This, shadingRateImage);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, shadingRateImage);
    if (!rec)
        return;
    Args args;
    args.ref("shadingRateImage", shadingRateImage, "ID3D12Resource");
    rec->Record("RSSetShadingRateImage", args.str());
}

void STDMETHODCALLTYPE Hook_OMSetFrontAndBackStencilRef(List* This, UINT FrontStencilRef, UINT BackStencilRef)
{
    auto orig = ORIG(OMSetFrontAndBackStencilRef);
    if (Internal())
        return orig(This, FrontStencilRef, BackStencilRef);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, FrontStencilRef, BackStencilRef);
    if (!rec)
        return;
    Args args;
    args.u("FrontStencilRef", FrontStencilRef).u("BackStencilRef", BackStencilRef);
    rec->Record("OMSetFrontAndBackStencilRef", args.str());
    LogOp(rec, OpKey::Replace(ops::kStencilRef), [FrontStencilRef, BackStencilRef](ID3D12GraphicsCommandList* list, PassReplay&) {
        AsList(list)->OMSetFrontAndBackStencilRef(FrontStencilRef, BackStencilRef);
    });
}

void STDMETHODCALLTYPE Hook_RSSetDepthBias(List* This, FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias)
{
    auto orig = ORIG(RSSetDepthBias);
    if (Internal())
        return orig(This, DepthBias, DepthBiasClamp, SlopeScaledDepthBias);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, DepthBias, DepthBiasClamp, SlopeScaledDepthBias);
    if (!rec)
        return;
    Args args;
    args.d("DepthBias", DepthBias).d("DepthBiasClamp", DepthBiasClamp).d("SlopeScaledDepthBias", SlopeScaledDepthBias);
    rec->Record("RSSetDepthBias", args.str());
}

void STDMETHODCALLTYPE Hook_IASetIndexBufferStripCutValue(List* This, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue)
{
    auto orig = ORIG(IASetIndexBufferStripCutValue);
    if (Internal())
        return orig(This, IBStripCutValue);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, IBStripCutValue);
    if (!rec)
        return;
    Args args;
    args.e("IBStripCutValue", ToString_D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(IBStripCutValue), IBStripCutValue);
    rec->Record("IASetIndexBufferStripCutValue", args.str());
}

void STDMETHODCALLTYPE Hook_SetProtectedResourceSession(List* This, ID3D12ProtectedResourceSession* pProtectedResourceSession)
{
    auto orig = ORIG(SetProtectedResourceSession);
    if (Internal())
        return orig(This, pProtectedResourceSession);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pProtectedResourceSession);
    if (!rec)
        return;
    Args args;
    args.ptr("pProtectedResourceSession", pProtectedResourceSession);
    rec->Record("SetProtectedResourceSession", args.str());
}

// ---------------------------------------------------------------------------------------------
// Pipelines, root signatures, descriptor heaps and root parameters

void STDMETHODCALLTYPE Hook_SetPipelineState(List* This, ID3D12PipelineState* pPipelineState)
{
    auto orig = ORIG(SetPipelineState);
    if (Internal())
        return orig(This, pPipelineState);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    // The edited replacement is bound, the application's pipeline recorded (shader_edit.h).
    orig(This, ShaderEditor::Get().Substitute(pPipelineState));
    if (!rec)
        return;
    Args args;
    args.ref("pPipelineState", pPipelineState, "ID3D12PipelineState")
        .s("bindPoint", IsComputePipeline(pPipelineState) ? "compute" : "graphics");
    rec->Record("SetPipelineState", args.str());
    rec->state().pipeline = pPipelineState;
    LogOp(rec, OpKey::Replace(ops::kPipeline),
        [held = Held(pPipelineState)](ID3D12GraphicsCommandList* list, PassReplay& replay) { replay.SetPipeline(list, held.get()); });
}

void STDMETHODCALLTYPE Hook_SetPipelineState1(List* This, ID3D12StateObject* pStateObject)
{
    auto orig = ORIG(SetPipelineState1);
    if (Internal())
        return orig(This, pStateObject);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pStateObject);
    if (!rec)
        return;
    Args args;
    args.ref("pStateObject", pStateObject, "ID3D12StateObject");
    rec->Record("SetPipelineState1", args.str());
    // A trace's binding table holds this state object's shader identifiers and no other's.
    NoteBoundStateObject(rec, pStateObject);
}

void STDMETHODCALLTYPE Hook_SetProgram(List* This, const D3D12_SET_PROGRAM_DESC* pDesc)
{
    auto orig = ORIG(SetProgram);
    if (Internal())
        return orig(This, pDesc);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDesc);
    if (!rec)
        return;
    Args args;
    if (pDesc)
        Write(args.key("pDesc"), *pDesc);
    else
        args.null("pDesc");
    rec->Record("SetProgram", args.str());
}

void STDMETHODCALLTYPE Hook_SetGraphicsRootSignature(List* This, ID3D12RootSignature* pRootSignature)
{
    auto orig = ORIG(SetGraphicsRootSignature);
    if (Internal())
        return orig(This, pRootSignature);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pRootSignature);
    if (!rec)
        return;
    Args args;
    args.ref("pRootSignature", pRootSignature, "ID3D12RootSignature");
    rec->Record("SetGraphicsRootSignature", args.str());
    // Setting a root signature drops every root argument, so the calls that set them are undone too.
    LogOp(rec, OpKey::Replace(ops::kGraphicsRootSignature, ops::kGraphicsRoot),
        [held = Held(pRootSignature)](ID3D12GraphicsCommandList* list, PassReplay& replay) {
            replay.SetGraphicsRootSignature(list, held.get());
        });
    rec->state().graphicsRootSignature = pRootSignature;
    rec->state().graphicsLayout = RootSignatures::Get().Find(pRootSignature);
}

void STDMETHODCALLTYPE Hook_SetComputeRootSignature(List* This, ID3D12RootSignature* pRootSignature)
{
    auto orig = ORIG(SetComputeRootSignature);
    if (Internal())
        return orig(This, pRootSignature);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pRootSignature);
    if (!rec)
        return;
    Args args;
    args.ref("pRootSignature", pRootSignature, "ID3D12RootSignature");
    rec->Record("SetComputeRootSignature", args.str());
    rec->state().computeRootSignature = pRootSignature;
    rec->state().computeLayout = RootSignatures::Get().Find(pRootSignature);
}

void STDMETHODCALLTYPE Hook_SetDescriptorHeaps(List* This, UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps)
{
    auto orig = ORIG(SetDescriptorHeaps);
    if (Internal())
        return orig(This, NumDescriptorHeaps, ppDescriptorHeaps);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, NumDescriptorHeaps, ppDescriptorHeaps);
    if (!rec)
        return;
    Args args;
    args.u("NumDescriptorHeaps", NumDescriptorHeaps);
    JsonWriter& w = args.key("ppDescriptorHeaps");
    w.BeginArray();
    for (UINT i = 0; ppDescriptorHeaps && i < NumDescriptorHeaps; ++i)
        WriteRef(w, ppDescriptorHeaps[i], "ID3D12DescriptorHeap");
    w.EndArray();
    rec->Record("SetDescriptorHeaps", args.str());
    {
        std::vector<ComPtr<ID3D12DescriptorHeap>> held;
        for (UINT i = 0; ppDescriptorHeaps && i < NumDescriptorHeaps; ++i)
            held.push_back(Held(ppDescriptorHeaps[i]));
        LogOp(rec, OpKey::Replace(ops::kDescriptorHeaps), [held = std::move(held)](ID3D12GraphicsCommandList* list, PassReplay&) {
            std::vector<ID3D12DescriptorHeap*> raw;
            raw.reserve(held.size());
            for (const ComPtr<ID3D12DescriptorHeap>& h : held)
                raw.push_back(h.get());
            list->SetDescriptorHeaps((UINT)raw.size(), raw.empty() ? nullptr : raw.data());
        });
    }
    // The call replaces both shader-visible heaps, whichever of them it names.
    ListState& s = rec->state();
    s.heaps[0] = s.heaps[1] = nullptr;
    for (UINT i = 0; ppDescriptorHeaps && i < NumDescriptorHeaps; ++i)
    {
        ID3D12DescriptorHeap* heap = ppDescriptorHeaps[i];
        if (!heap)
            continue;
        HeapInfo info;
        D3D12_DESCRIPTOR_HEAP_TYPE type;
        if (DescriptorTracker::Get().GetHeap(heap, info))
        {
            type = info.desc.Type;
        }
        else
        {
            ScopedInternal internal;
            type = heap->GetDesc().Type;
        }
        if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
            s.heaps[0] = heap;
        else if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            s.heaps[1] = heap;
    }
}

void STDMETHODCALLTYPE Hook_SetGraphicsRootDescriptorTable(List* This, UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    auto orig = ORIG(SetGraphicsRootDescriptorTable);
    if (Internal())
        return orig(This, RootParameterIndex, BaseDescriptor);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, BaseDescriptor);
    if (rec)
        RecordRootTable(rec, "SetGraphicsRootDescriptorTable", false, RootParameterIndex, BaseDescriptor);
}

void STDMETHODCALLTYPE Hook_SetComputeRootDescriptorTable(List* This, UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    auto orig = ORIG(SetComputeRootDescriptorTable);
    if (Internal())
        return orig(This, RootParameterIndex, BaseDescriptor);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, BaseDescriptor);
    if (rec)
        RecordRootTable(rec, "SetComputeRootDescriptorTable", true, RootParameterIndex, BaseDescriptor);
}

void STDMETHODCALLTYPE Hook_SetGraphicsRoot32BitConstant(List* This, UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
    auto orig = ORIG(SetGraphicsRoot32BitConstant);
    if (Internal())
        return orig(This, RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    if (rec)
        RecordRootConstants(rec, "SetGraphicsRoot32BitConstant", false, RootParameterIndex, 1, &SrcData, DestOffsetIn32BitValues);
}

void STDMETHODCALLTYPE Hook_SetComputeRoot32BitConstant(List* This, UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
    auto orig = ORIG(SetComputeRoot32BitConstant);
    if (Internal())
        return orig(This, RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    if (rec)
        RecordRootConstants(rec, "SetComputeRoot32BitConstant", true, RootParameterIndex, 1, &SrcData, DestOffsetIn32BitValues);
}

void STDMETHODCALLTYPE Hook_SetGraphicsRoot32BitConstants(List* This, UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues)
{
    auto orig = ORIG(SetGraphicsRoot32BitConstants);
    if (Internal())
        return orig(This, RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    if (rec)
        RecordRootConstants(rec, "SetGraphicsRoot32BitConstants", false, RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
}

void STDMETHODCALLTYPE Hook_SetComputeRoot32BitConstants(List* This, UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues)
{
    auto orig = ORIG(SetComputeRoot32BitConstants);
    if (Internal())
        return orig(This, RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    if (rec)
        RecordRootConstants(rec, "SetComputeRoot32BitConstants", true, RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
}

void STDMETHODCALLTYPE Hook_SetGraphicsRootConstantBufferView(List* This, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    auto orig = ORIG(SetGraphicsRootConstantBufferView);
    if (Internal())
        return orig(This, RootParameterIndex, BufferLocation);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, BufferLocation);
    if (rec)
        RecordRootView(rec, "SetGraphicsRootConstantBufferView", false, RootView::ConstantBuffer, RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Hook_SetComputeRootConstantBufferView(List* This, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    auto orig = ORIG(SetComputeRootConstantBufferView);
    if (Internal())
        return orig(This, RootParameterIndex, BufferLocation);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, BufferLocation);
    if (rec)
        RecordRootView(rec, "SetComputeRootConstantBufferView", true, RootView::ConstantBuffer, RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Hook_SetGraphicsRootShaderResourceView(List* This, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    auto orig = ORIG(SetGraphicsRootShaderResourceView);
    if (Internal())
        return orig(This, RootParameterIndex, BufferLocation);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, BufferLocation);
    if (rec)
        RecordRootView(rec, "SetGraphicsRootShaderResourceView", false, RootView::ShaderResource, RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Hook_SetComputeRootShaderResourceView(List* This, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    auto orig = ORIG(SetComputeRootShaderResourceView);
    if (Internal())
        return orig(This, RootParameterIndex, BufferLocation);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, BufferLocation);
    if (rec)
        RecordRootView(rec, "SetComputeRootShaderResourceView", true, RootView::ShaderResource, RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Hook_SetGraphicsRootUnorderedAccessView(List* This, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    auto orig = ORIG(SetGraphicsRootUnorderedAccessView);
    if (Internal())
        return orig(This, RootParameterIndex, BufferLocation);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, BufferLocation);
    if (rec)
        RecordRootView(rec, "SetGraphicsRootUnorderedAccessView", false, RootView::UnorderedAccess, RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Hook_SetComputeRootUnorderedAccessView(List* This, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    auto orig = ORIG(SetComputeRootUnorderedAccessView);
    if (Internal())
        return orig(This, RootParameterIndex, BufferLocation);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, RootParameterIndex, BufferLocation);
    if (rec)
        RecordRootView(rec, "SetComputeRootUnorderedAccessView", true, RootView::UnorderedAccess, RootParameterIndex, BufferLocation);
}

// ---------------------------------------------------------------------------------------------
// Input assembler and stream output. The views carry GPU addresses, written resolved to their
// buffers, and the ranges they name are queued for read-back with the ids in `bufferData`.

void STDMETHODCALLTYPE Hook_IASetIndexBuffer(List* This, const D3D12_INDEX_BUFFER_VIEW* pView)
{
    auto orig = ORIG(IASetIndexBuffer);
    if (Internal())
        return orig(This, pView);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pView);
    if (!rec)
        return;
    Args args;
    JsonWriter& w = args.key("pView");
    if (!pView)
    {
        w.Null();
    }
    else
    {
        w.BeginObject();
        w.Key("BufferLocation");
        WriteGpuAddress(w, pView->BufferLocation);
        w.Key("SizeInBytes");
        w.Uint(pView->SizeInBytes);
        w.Key("Format");
        w.Enum(ToString_DXGI_FORMAT(pView->Format), pView->Format);
        w.EndObject();
    }
    rec->Record("IASetIndexBuffer", args.str());
    const bool haveView = pView != nullptr;
    const D3D12_INDEX_BUFFER_VIEW view = haveView ? *pView : D3D12_INDEX_BUFFER_VIEW{};
    LogOp(rec, OpKey::Replace(ops::kIndexBuffer), [haveView, view](ID3D12GraphicsCommandList* list, PassReplay&) {
        list->IASetIndexBuffer(haveView ? &view : nullptr);
    });
    if (pView && pView->BufferLocation)
    {
        rec->SetSnapshotOnLast([view](CommandRecorder* on) { return BufferDataExtra({Cap().QueueAddressCapture(on, view.BufferLocation, view.SizeInBytes, true)}); });
    }
}

void STDMETHODCALLTYPE Hook_IASetVertexBuffers(List* This, UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews)
{
    auto orig = ORIG(IASetVertexBuffers);
    if (Internal())
        return orig(This, StartSlot, NumViews, pViews);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, StartSlot, NumViews, pViews);
    if (!rec)
        return;
    Args args;
    args.u("StartSlot", StartSlot).u("NumViews", NumViews);
    JsonWriter& w = args.key("pViews");
    if (!pViews)
    {
        w.Null();
    }
    else
    {
        w.BeginArray();
        for (UINT i = 0; i < NumViews; ++i)
        {
            w.BeginObject();
            w.Key("BufferLocation");
            WriteGpuAddress(w, pViews[i].BufferLocation);
            w.Key("SizeInBytes");
            w.Uint(pViews[i].SizeInBytes);
            w.Key("StrideInBytes");
            w.Uint(pViews[i].StrideInBytes);
            w.EndObject();
        }
        w.EndArray();
    }
    rec->Record("IASetVertexBuffers", args.str());
    {
        std::vector<D3D12_VERTEX_BUFFER_VIEW> views(pViews, pViews + (pViews ? NumViews : 0));
        const bool given = pViews != nullptr;
        LogOp(rec, OpKey::Range(ops::kVertexBuffers, StartSlot, NumViews),
            [StartSlot, NumViews, given, views = std::move(views)](ID3D12GraphicsCommandList* list, PassReplay&) {
                list->IASetVertexBuffers(StartSlot, NumViews, given ? views.data() : nullptr);
            });
    }
    if (!pViews)
        return;
    if (!rec->bundle())
    {
        std::vector<uint32_t> ids;
        for (UINT i = 0; i < NumViews; ++i)
        {
            ids.push_back(pViews[i].BufferLocation ? Cap().QueueAddressCapture(rec, pViews[i].BufferLocation, pViews[i].SizeInBytes, true) : 0);
        }
        rec->SetExtraOnLast(BufferDataExtra(ids));
        return;
    }
    rec->SetSnapshotOnLast([bound = std::vector<D3D12_VERTEX_BUFFER_VIEW>(pViews, pViews + NumViews)](CommandRecorder* on) {
        std::vector<uint32_t> ids;
        for (const D3D12_VERTEX_BUFFER_VIEW& v : bound)
            ids.push_back(v.BufferLocation ? Cap().QueueAddressCapture(on, v.BufferLocation, v.SizeInBytes, true) : 0);
        return BufferDataExtra(ids);
    });
}

void STDMETHODCALLTYPE Hook_SOSetTargets(List* This, UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews)
{
    auto orig = ORIG(SOSetTargets);
    if (Internal())
        return orig(This, StartSlot, NumViews, pViews);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, StartSlot, NumViews, pViews);
    if (!rec)
        return;
    Args args;
    args.u("StartSlot", StartSlot).u("NumViews", NumViews);
    JsonWriter& w = args.key("pViews");
    if (!pViews)
    {
        w.Null();
    }
    else
    {
        w.BeginArray();
        for (UINT i = 0; i < NumViews; ++i)
            Write(w, pViews[i]);
        w.EndArray();
    }
    rec->Record("SOSetTargets", args.str());
}

// ---------------------------------------------------------------------------------------------
// Render passes: OMSetRenderTargets and BeginRenderPass open one (ending whatever was open, and
// any compute pass), EndRenderPass closes the real kind. README.md, "Passes".

void STDMETHODCALLTYPE Hook_OMSetRenderTargets(List* This, UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    auto orig = ORIG(OMSetRenderTargets);
    if (Internal())
        return orig(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    CommandRecorder* rec = Rec(This);
    Cap().EndOpenPass(This, true);
    CommandScope scope(rec);
    orig(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    if (!rec)
        return;
    std::vector<BoundTarget> targets;
    Args args;
    args.u("NumRenderTargetDescriptors", NumRenderTargetDescriptors);
    JsonWriter& w = args.key("pRenderTargetDescriptors");
    w.BeginArray();
    if (pRenderTargetDescriptors)
    {
        // A single handle names a contiguous run of slots of its heap.
        uint32_t increment = 0;
        if (RTsSingleHandleToDescriptorRange && NumRenderTargetDescriptors)
        {
            HeapInfo heap;
            uint32_t index = 0;
            increment = DescriptorTracker::Get().Locate(pRenderTargetDescriptors[0], heap, index) ? heap.increment : RtvIncrement(rec);
        }
        for (UINT i = 0; i < NumRenderTargetDescriptors; ++i)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = RTsSingleHandleToDescriptorRange
                ? D3D12_CPU_DESCRIPTOR_HANDLE{pRenderTargetDescriptors[0].ptr + (SIZE_T)i * increment}
                : pRenderTargetDescriptors[i];
            ResolvedTarget t = ResolveHandle(handle);
            WriteTarget(w, handle, t, false);
            AddTarget(targets, handle, t, false, i);
        }
    }
    w.EndArray();
    args.b("RTsSingleHandleToDescriptorRange", RTsSingleHandleToDescriptorRange != FALSE);
    if (pDepthStencilDescriptor)
    {
        ResolvedTarget t = ResolveHandle(*pDepthStencilDescriptor);
        WriteTarget(args.key("pDepthStencilDescriptor"), *pDepthStencilDescriptor, t, true);
        AddTarget(targets, *pDepthStencilDescriptor, t, true, NumRenderTargetDescriptors);
    }
    else
    {
        args.null("pDepthStencilDescriptor");
    }
    // The copies a measurement starts from, taken before the application's first draw of the pass
    // and while the list is outside a render-pass region (overdraw.h).
    // Not in an adopted list, whose passes take no measurements (CommandRecorder::adopted).
    if (!rec->adopted())
        PrepareMeasuredPass(rec, targets);
    rec->Record("OMSetRenderTargets", args.str());
    Cap().BeginPass(rec, std::move(targets), false);
    if (!rec->adopted())
        BeginMeasuredPass(rec);
}

std::string BeginRenderPassArgs(CommandRecorder* rec, UINT NumRenderTargets,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil,
    D3D12_RENDER_PASS_FLAGS Flags, std::vector<BoundTarget>& targets);

void STDMETHODCALLTYPE Hook_BeginRenderPass(List* This, UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags)
{
    auto orig = ORIG(BeginRenderPass);
    if (Internal())
        return orig(This, NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
    CommandRecorder* rec = Rec(This);
    std::vector<BoundTarget> targets;
    std::string argsJson;
    // A pass the application suspends here, or resumes from another list, carries no work of the
    // capture's: not the copies a measurement starts from, not its queries, not its read-back.
    // Between a suspension and its resume the runtime rejects every GPU-work-generating call and
    // closes the list with E_FAIL, which the application takes for a lost device (ActivePass::split).
    const bool split = (Flags & (D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS | D3D12_RENDER_PASS_FLAG_RESUMING_PASS)) != 0;
    // An adopted list's pass takes its timestamps and its read-back like any other, but not the
    // measurements (CommandRecorder::adopted).
    const bool measured = !split && rec && !rec->adopted();
    // The arguments and the pass's targets are resolved before the forward, because the copies a
    // measurement starts from have to be taken while the list is still outside the render-pass
    // region: a copy may not interrupt one. The command itself is recorded after the forward, so
    // the stream keeps the order the application made its calls in.
    Cap().EndOpenPass(This, true);
    if (rec)
    {
        argsJson = BeginRenderPassArgs(rec, NumRenderTargets, pRenderTargets, pDepthStencil, Flags, targets);
        if (measured)
            PrepareMeasuredPass(rec, targets);
    }
    CommandScope scope(rec);
    orig(This, NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
    // Inside a render pass the runtime gives the list a vtable of its own (one that refuses the
    // calls a pass forbids), swapped back at EndRenderPass; a list hooked at creation is unhooked
    // in between unless that vtable is patched too. Every vtable is patched once, so this is free
    // after the first pass.
    HookCommandList(This);
    if (!rec)
        return;
    rec->Record("BeginRenderPass", argsJson);
    Cap().BeginPass(rec, std::move(targets), true, split);
    rec->pass().suspending = (Flags & D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS) != 0;
    if (measured)
        BeginMeasuredPass(rec);
}

/** BeginRenderPass' arguments as the capture records them, with the pass's targets resolved beside them. */
std::string BeginRenderPassArgs(CommandRecorder* rec, UINT NumRenderTargets,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil,
    D3D12_RENDER_PASS_FLAGS Flags, std::vector<BoundTarget>& targets)
{
    (void)rec;
    Args args;
    args.u("NumRenderTargets", NumRenderTargets);
    JsonWriter& w = args.key("pRenderTargets");
    w.BeginArray();
    for (UINT i = 0; pRenderTargets && i < NumRenderTargets; ++i)
    {
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC& d = pRenderTargets[i];
        ResolvedTarget t = ResolveHandle(d.cpuDescriptor);
        w.BeginObject();
        w.Key("cpuDescriptor");
        w.BeginObject();
        WriteTargetHandle(w, d.cpuDescriptor, t);
        w.EndObject();
        WriteTargetResource(w, t, false);
        w.Key("BeginningAccess");
        Write(w, d.BeginningAccess);
        w.Key("EndingAccess");
        Write(w, d.EndingAccess);
        w.EndObject();
        const size_t before = targets.size();
        AddTarget(targets, d.cpuDescriptor, t, false, i);
        if (targets.size() > before)
        {
            targets.back().beginAccess = d.BeginningAccess.Type;
            targets.back().clearValue = d.BeginningAccess.Clear.ClearValue;
        }
    }
    w.EndArray();
    args.key("pDepthStencil");
    if (!pDepthStencil)
    {
        w.Null();
    }
    else
    {
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC& d = *pDepthStencil;
        ResolvedTarget t = ResolveHandle(d.cpuDescriptor);
        w.BeginObject();
        w.Key("cpuDescriptor");
        w.BeginObject();
        WriteTargetHandle(w, d.cpuDescriptor, t);
        w.EndObject();
        WriteTargetResource(w, t, true);
        w.Key("DepthBeginningAccess");
        Write(w, d.DepthBeginningAccess);
        w.Key("StencilBeginningAccess");
        Write(w, d.StencilBeginningAccess);
        w.Key("DepthEndingAccess");
        Write(w, d.DepthEndingAccess);
        w.Key("StencilEndingAccess");
        Write(w, d.StencilEndingAccess);
        w.EndObject();
        const size_t before = targets.size();
        AddTarget(targets, d.cpuDescriptor, t, true, NumRenderTargets);
        if (targets.size() > before)
        {
            targets.back().beginAccess = d.DepthBeginningAccess.Type;
            targets.back().stencilBeginAccess = d.StencilBeginningAccess.Type;
            targets.back().clearValue = d.DepthBeginningAccess.Clear.ClearValue;
            targets.back().clearValue.DepthStencil.Stencil = d.StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil;
        }
    }
    Flags_D3D12_RENDER_PASS_FLAGS(args.key("Flags"), Flags);
    return args.str();
}

void STDMETHODCALLTYPE Hook_EndRenderPass(List* This)
{
    auto orig = ORIG(EndRenderPass);
    if (Internal())
        return orig(This);
    CommandRecorder* rec = Rec(This);
    // A split pass's end timestamp goes in here, inside the pass region: after the forward the pass
    // is suspended and the runtime rejects every query (capture.h, EndSplitPassTimestamp).
    Cap().EndSplitPassTimestamp(rec);
    CommandScope scope(rec);
    orig(This);
    // The vtable changes with the list's state here as well: after a pass that ends suspended
    // (Unity's, continued on the lists its jobs record) the list was on one without the hooks,
    // and its Close, with anything else it recorded, was never seen.
    HookCommandList(This);
    if (!rec)
        return;
    rec->Record("EndRenderPass", "");
    Cap().EndPass(rec, false);
}

// ---------------------------------------------------------------------------------------------
// Barriers, events and bundles: what closes a compute pass (README.md, "Passes"). The resource
// tracker follows every barrier whether or not the list is recorded, since the state a render
// target is in at a later capture depends on them.

void STDMETHODCALLTYPE Hook_ResourceBarrier(List* This, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers)
{
    auto orig = ORIG(ResourceBarrier);
    if (Internal())
        return orig(This, NumBarriers, pBarriers);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnComputePassEnd(rec);
    CommandScope scope(rec);
    orig(This, NumBarriers, pBarriers);
    ResourceTracker::Get().OnBarriers(This, NumBarriers, pBarriers);
    if (!rec)
        return;
    Args args;
    args.u("NumBarriers", NumBarriers);
    JsonWriter& w = args.key("pBarriers");
    w.BeginArray();
    for (UINT i = 0; pBarriers && i < NumBarriers; ++i)
        Write(w, pBarriers[i]);
    w.EndArray();
    rec->Record("ResourceBarrier", args.str());
}

void STDMETHODCALLTYPE Hook_Barrier(List* This, UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP* pBarrierGroups)
{
    auto orig = ORIG(Barrier);
    if (Internal())
        return orig(This, NumBarrierGroups, pBarrierGroups);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnComputePassEnd(rec);
    CommandScope scope(rec);
    orig(This, NumBarrierGroups, pBarrierGroups);
    ResourceTracker::Get().OnBarrierGroups(This, NumBarrierGroups, pBarrierGroups);
    if (!rec)
        return;
    Args args;
    args.u("NumBarrierGroups", NumBarrierGroups);
    JsonWriter& w = args.key("pBarrierGroups");
    w.BeginArray();
    for (UINT32 i = 0; pBarrierGroups && i < NumBarrierGroups; ++i)
        Write(w, pBarrierGroups[i]);
    w.EndArray();
    rec->Record("Barrier", args.str());
}

void RecordEvent(CommandRecorder* rec, const char* method, UINT Metadata, const void* pData, UINT Size)
{
    Args args;
    args.u("Metadata", Metadata).s("label", EventLabel(Metadata, pData, Size)).u("Size", Size);
    rec->Record(method, args.str());
}

void STDMETHODCALLTYPE Hook_BeginEvent(List* This, UINT Metadata, const void* pData, UINT Size)
{
    auto orig = ORIG(BeginEvent);
    if (Internal())
        return orig(This, Metadata, pData, Size);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnComputePassEnd(rec);
    CommandScope scope(rec);
    orig(This, Metadata, pData, Size);
    if (rec)
        RecordEvent(rec, "BeginEvent", Metadata, pData, Size);
}

void STDMETHODCALLTYPE Hook_SetMarker(List* This, UINT Metadata, const void* pData, UINT Size)
{
    auto orig = ORIG(SetMarker);
    if (Internal())
        return orig(This, Metadata, pData, Size);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnComputePassEnd(rec);
    CommandScope scope(rec);
    orig(This, Metadata, pData, Size);
    if (rec)
        RecordEvent(rec, "SetMarker", Metadata, pData, Size);
}

void STDMETHODCALLTYPE Hook_EndEvent(List* This)
{
    auto orig = ORIG(EndEvent);
    if (Internal())
        return orig(This);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnComputePassEnd(rec);
    CommandScope scope(rec);
    orig(This);
    if (rec)
        rec->Record("EndEvent", "");
}

void STDMETHODCALLTYPE Hook_ExecuteBundle(List* This, ID3D12GraphicsCommandList* pCommandList)
{
    auto orig = ORIG(ExecuteBundle);
    if (Internal())
        return orig(This, pCommandList);
    CommandRecorder* rec = Rec(This);
    if (rec)
        Cap().OnComputePassEnd(rec);
    if (rec)
        rec->FlushSnapshots(true, true);   // a bundle draws with the tables of the list that executes it
    CommandScope scope(rec);
    orig(This, pCommandList);
    if (!rec)
        return;
    Args args;
    args.ref("pCommandList", pCommandList, "ID3D12GraphicsCommandList");
    rec->Record("ExecuteBundle", args.str());
    Cap().OnExecuteBundle(rec, pCommandList);
    // A bundle sets state on the list that executes it and draws with it, so its kept calls become
    // the list's; one recorded before the capture has none, and the pass says so.
    OnMeasuredBundle(This, pCommandList);
}

// ---------------------------------------------------------------------------------------------
// Queries and predication. The application's open queries are counted so the capture's pass
// queries do not nest inside them (command_recorder.h, appQueryDepth).

void RecordQuery(CommandRecorder* rec, const char* method, ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
    Args args;
    args.ref("pQueryHeap", pQueryHeap, "ID3D12QueryHeap").e("Type", ToString_D3D12_QUERY_TYPE(Type), Type).u("Index", Index);
    rec->Record(method, args.str());
}

void STDMETHODCALLTYPE Hook_BeginQuery(List* This, ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
    auto orig = ORIG(BeginQuery);
    if (Internal())
        return orig(This, pQueryHeap, Type, Index);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pQueryHeap, Type, Index);
    if (!rec)
        return;
    rec->state().appQueryDepth++;
    RecordQuery(rec, "BeginQuery", pQueryHeap, Type, Index);
}

void STDMETHODCALLTYPE Hook_EndQuery(List* This, ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
    auto orig = ORIG(EndQuery);
    if (Internal())
        return orig(This, pQueryHeap, Type, Index);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pQueryHeap, Type, Index);
    if (!rec)
        return;
    // A timestamp query has no BeginQuery, so only a real pair changes the depth.
    if (rec->state().appQueryDepth > 0 && Type != D3D12_QUERY_TYPE_TIMESTAMP)
        rec->state().appQueryDepth--;
    RecordQuery(rec, "EndQuery", pQueryHeap, Type, Index);
}

void STDMETHODCALLTYPE Hook_ResolveQueryData(List* This, ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset)
{
    auto orig = ORIG(ResolveQueryData);
    if (Internal())
        return orig(This, pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset);
    if (!rec)
        return;
    Args args;
    args.ref("pQueryHeap", pQueryHeap, "ID3D12QueryHeap").e("Type", ToString_D3D12_QUERY_TYPE(Type), Type).u("StartIndex", StartIndex).u("NumQueries", NumQueries).ref("pDestinationBuffer", pDestinationBuffer, "ID3D12Resource").u("AlignedDestinationBufferOffset", AlignedDestinationBufferOffset);
    rec->Record("ResolveQueryData", args.str());
}

void STDMETHODCALLTYPE Hook_SetPredication(List* This, ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation)
{
    auto orig = ORIG(SetPredication);
    if (Internal())
        return orig(This, pBuffer, AlignedBufferOffset, Operation);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pBuffer, AlignedBufferOffset, Operation);
    if (!rec)
        return;
    Args args;
    args.ref("pBuffer", pBuffer, "ID3D12Resource").u("AlignedBufferOffset", AlignedBufferOffset).e("Operation", ToString_D3D12_PREDICATION_OP(Operation), Operation);
    rec->Record("SetPredication", args.str());
}

// ---------------------------------------------------------------------------------------------
// Meta commands and raytracing: recorded as commands, their contents not read back (README.md, "Not done").

void RecordMetaCommand(CommandRecorder* rec, const char* method, ID3D12MetaCommand* pMetaCommand, const char* sizeKey, SIZE_T size)
{
    Args args;
    args.ptr("pMetaCommand", pMetaCommand).u(sizeKey, size);
    rec->Record(method, args.str());
}

void STDMETHODCALLTYPE Hook_InitializeMetaCommand(List* This, ID3D12MetaCommand* pMetaCommand, const void* pInitializationParametersData, SIZE_T InitializationParametersDataSizeInBytes)
{
    auto orig = ORIG(InitializeMetaCommand);
    if (Internal())
        return orig(This, pMetaCommand, pInitializationParametersData, InitializationParametersDataSizeInBytes);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pMetaCommand, pInitializationParametersData, InitializationParametersDataSizeInBytes);
    if (rec)
        RecordMetaCommand(rec, "InitializeMetaCommand", pMetaCommand, "InitializationParametersDataSizeInBytes", InitializationParametersDataSizeInBytes);
}

void STDMETHODCALLTYPE Hook_ExecuteMetaCommand(List* This, ID3D12MetaCommand* pMetaCommand, const void* pExecutionParametersData, SIZE_T ExecutionParametersDataSizeInBytes)
{
    auto orig = ORIG(ExecuteMetaCommand);
    if (Internal())
        return orig(This, pMetaCommand, pExecutionParametersData, ExecutionParametersDataSizeInBytes);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pMetaCommand, pExecutionParametersData, ExecutionParametersDataSizeInBytes);
    if (rec)
        RecordMetaCommand(rec, "ExecuteMetaCommand", pMetaCommand, "ExecutionParametersDataSizeInBytes", ExecutionParametersDataSizeInBytes);
}

void STDMETHODCALLTYPE Hook_BuildRaytracingAccelerationStructure(List* This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc, UINT NumPostbuildInfoDescs, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs)
{
    auto orig = ORIG(BuildRaytracingAccelerationStructure);
    if (Internal())
        return orig(This, pDesc, NumPostbuildInfoDescs, pPostbuildInfoDescs);
    CommandRecorder* rec = Rec(This);
    // A build is compute work, timed in a compute pass with the dispatches and ray traces around it.
    if (rec && !rec->pass().active)
        Cap().OnBeforeComputeWork(rec);
    CommandScope scope(rec);
    orig(This, pDesc, NumPostbuildInfoDescs, pPostbuildInfoDescs);
    // The structure this wrote and what it was built from (raytracing.h), whether or not a capture
    // is recording: a bottom level is built once, usually before any capture, and the object has to
    // exist for a later top level's instances to resolve to it. The read-back of the inputs is what
    // needs a recorder, and only happens when there is one.
    std::string built;
    if (pDesc)
        built = NoteAccelerationStructureBuild(rec, *pDesc, DeviceOf(This));
    if (!rec)
        return;
    Args args;
    if (pDesc)
        Write(args.key("pDesc"), *pDesc);
    else
        args.null("pDesc");
    args.u("NumPostbuildInfoDescs", NumPostbuildInfoDescs);
    JsonWriter& w = args.key("pPostbuildInfoDescs");
    w.BeginArray();
    for (UINT i = 0; pPostbuildInfoDescs && i < NumPostbuildInfoDescs; ++i)
        WritePostbuildInfo(w, pPostbuildInfoDescs[i]);
    w.EndArray();
    rec->Record("BuildRaytracingAccelerationStructure", args.str());
    if (!built.empty())
        rec->SetExtraOnLast(std::move(built));
}

void STDMETHODCALLTYPE Hook_EmitRaytracingAccelerationStructurePostbuildInfo(List* This, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc, UINT NumSourceAccelerationStructures, const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData)
{
    auto orig = ORIG(EmitRaytracingAccelerationStructurePostbuildInfo);
    if (Internal())
        return orig(This, pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
    if (!rec)
        return;
    Args args;
    if (pDesc)
        WritePostbuildInfo(args.key("pDesc"), *pDesc);
    else
        args.null("pDesc");
    args.u("NumSourceAccelerationStructures", NumSourceAccelerationStructures);
    JsonWriter& w = args.key("pSourceAccelerationStructureData");
    w.BeginArray();
    for (UINT i = 0; pSourceAccelerationStructureData && i < NumSourceAccelerationStructures; ++i)
        WriteGpuAddress(w, pSourceAccelerationStructureData[i]);
    w.EndArray();
    rec->Record("EmitRaytracingAccelerationStructurePostbuildInfo", args.str());
}

void STDMETHODCALLTYPE Hook_CopyRaytracingAccelerationStructure(List* This, D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData, D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode)
{
    auto orig = ORIG(CopyRaytracingAccelerationStructure);
    if (Internal())
        return orig(This, DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
    CommandRecorder* rec = Rec(This);
    CommandScope scope(rec);
    orig(This, DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
    // As for a build: the destination is a structure whether or not anything is recording.
    NoteAccelerationStructureCopy(DestAccelerationStructureData, SourceAccelerationStructureData, Mode, DeviceOf(This));
    if (!rec)
        return;
    Args args;
    args.address("DestAccelerationStructureData", DestAccelerationStructureData)
        .address("SourceAccelerationStructureData", SourceAccelerationStructureData)
        .e("Mode", ToString_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE(Mode), Mode);
    rec->Record("CopyRaytracingAccelerationStructure", args.str());
}

// clang-format off
#define HOOK(Method) SlotHook{slot::ID3D12GraphicsCommandList10_##Method, (void*)&Hook_##Method}
// clang-format on

}  // namespace

// The newest command list interface the runtime implements decides how long its vtable is: the
// slots of a newer version must not be written past a shorter vtable (the same probing as the
// device's, hooks_device.cpp). Each version's count is the slot of the next version's first method.
static uint32_t CommandListVtableCount(ID3D12GraphicsCommandList* list)
{
    struct Version
    {
        const IID* iid;
        uint32_t count;
    };
    static const Version versions[] = {
        {&__uuidof(ID3D12GraphicsCommandList10), slot::ID3D12GraphicsCommandList10_Count},
        {&__uuidof(ID3D12GraphicsCommandList9), slot::ID3D12GraphicsCommandList10_SetProgram},
        {&__uuidof(ID3D12GraphicsCommandList8), slot::ID3D12GraphicsCommandList10_RSSetDepthBias},
        {&__uuidof(ID3D12GraphicsCommandList7), slot::ID3D12GraphicsCommandList10_OMSetFrontAndBackStencilRef},
        {&__uuidof(ID3D12GraphicsCommandList6), slot::ID3D12GraphicsCommandList10_Barrier},
        {&__uuidof(ID3D12GraphicsCommandList5), slot::ID3D12GraphicsCommandList10_DispatchMesh},
        {&__uuidof(ID3D12GraphicsCommandList4), slot::ID3D12GraphicsCommandList10_RSSetShadingRate},
        {&__uuidof(ID3D12GraphicsCommandList3), slot::ID3D12GraphicsCommandList10_BeginRenderPass},
        {&__uuidof(ID3D12GraphicsCommandList2), slot::ID3D12GraphicsCommandList10_SetProtectedResourceSession},
        {&__uuidof(ID3D12GraphicsCommandList1), slot::ID3D12GraphicsCommandList10_WriteBufferImmediate},
    };
    ScopedInternal internal;
    for (const Version& v : versions)
    {
        IUnknown* p = nullptr;
        if (SUCCEEDED(list->QueryInterface(*v.iid, (void**)&p)) && p)
        {
            p->Release();
            return v.count;
        }
    }
    return slot::ID3D12GraphicsCommandList10_AtomicCopyBufferUINT;
}

void HookCommandList(ID3D12GraphicsCommandList* list)
{
    if (!list || VtableHooked(list))
        return;
    // clang-format off
    HookD3D12Object(list, "ID3D12GraphicsCommandList", CommandListVtableCount(list), {
        HOOK(Close), HOOK(Reset), HOOK(ClearState),
        HOOK(DrawInstanced), HOOK(DrawIndexedInstanced), HOOK(Dispatch),
        HOOK(CopyBufferRegion), HOOK(CopyTextureRegion), HOOK(CopyResource), HOOK(CopyTiles), HOOK(ResolveSubresource),
        HOOK(IASetPrimitiveTopology), HOOK(RSSetViewports), HOOK(RSSetScissorRects), HOOK(OMSetBlendFactor), HOOK(OMSetStencilRef),
        HOOK(SetPipelineState), HOOK(ResourceBarrier), HOOK(ExecuteBundle), HOOK(SetDescriptorHeaps),
        HOOK(SetComputeRootSignature), HOOK(SetGraphicsRootSignature),
        HOOK(SetComputeRootDescriptorTable), HOOK(SetGraphicsRootDescriptorTable),
        HOOK(SetComputeRoot32BitConstant), HOOK(SetGraphicsRoot32BitConstant),
        HOOK(SetComputeRoot32BitConstants), HOOK(SetGraphicsRoot32BitConstants),
        HOOK(SetComputeRootConstantBufferView), HOOK(SetGraphicsRootConstantBufferView),
        HOOK(SetComputeRootShaderResourceView), HOOK(SetGraphicsRootShaderResourceView),
        HOOK(SetComputeRootUnorderedAccessView), HOOK(SetGraphicsRootUnorderedAccessView),
        HOOK(IASetIndexBuffer), HOOK(IASetVertexBuffers), HOOK(SOSetTargets), HOOK(OMSetRenderTargets),
        HOOK(ClearDepthStencilView), HOOK(ClearRenderTargetView), HOOK(ClearUnorderedAccessViewUint), HOOK(ClearUnorderedAccessViewFloat),
        HOOK(DiscardResource), HOOK(BeginQuery), HOOK(EndQuery), HOOK(ResolveQueryData), HOOK(SetPredication),
        HOOK(SetMarker), HOOK(BeginEvent), HOOK(EndEvent), HOOK(ExecuteIndirect),
        HOOK(AtomicCopyBufferUINT), HOOK(AtomicCopyBufferUINT64), HOOK(OMSetDepthBounds), HOOK(SetSamplePositions),
        HOOK(ResolveSubresourceRegion), HOOK(SetViewInstanceMask), HOOK(WriteBufferImmediate), HOOK(SetProtectedResourceSession),
        HOOK(BeginRenderPass), HOOK(EndRenderPass), HOOK(InitializeMetaCommand), HOOK(ExecuteMetaCommand),
        HOOK(BuildRaytracingAccelerationStructure), HOOK(EmitRaytracingAccelerationStructurePostbuildInfo),
        HOOK(CopyRaytracingAccelerationStructure), HOOK(SetPipelineState1), HOOK(DispatchRays),
        HOOK(RSSetShadingRate), HOOK(RSSetShadingRateImage), HOOK(DispatchMesh), HOOK(Barrier),
        HOOK(OMSetFrontAndBackStencilRef), HOOK(RSSetDepthBias), HOOK(IASetIndexBufferStripCutValue),
        HOOK(SetProgram), HOOK(DispatchGraph),
    });
    // clang-format on
}

#undef HOOK
#undef ORIG

}  // namespace dxinsp
