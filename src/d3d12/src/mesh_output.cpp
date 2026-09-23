// Mesh output measured while capturing: what one draw's vertex shader wrote, for the mesh view's
// VS Out (`vkinsp_replay --mesh` measures the same by replaying a Vulkan capture,
// src/replay/src/mesh.cpp).
//
// Vulkan gets it by patching the SPIR-V for transform feedback; Direct3D 12 needs no edit of the
// shader at all. A pipeline copy declares stream output over the vertex shader's own output
// signature (D3D12_SO_DECLARATION_ENTRY, built from the reflection of the unmodified bytecode),
// rasterizes nothing, and the draw is issued again with a buffer of ours bound through
// SOSetTargets. RenderDoc does the same for D3D12 (driver/d3d12/d3d12_postvs.cpp).
//
// Two things follow from that and are handled here:
//   * a pipeline's root signature almost never carries
//     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT, which the copy needs, so the signature is
//     deserialized from the blob it was created with (descriptors.h keeps it), the flag is added,
//     and it is serialized and created again. The copy is layout-compatible with the original, so
//     the root arguments the application bound still apply;
//   * the buffer needs a size before the draw runs, so it is sized from what the draw could write
//     (its vertex or index count times the record) up to the request's limit, and the stream-output
//     counter says how much was really written.
//
// A draw is named by its pass and its ordinal within it, as a draw overlay is (draw_overlay.cpp),
// and for the same reason: the measurement happens while the next frame records.
#include "pass_record.h"

#include "common.h"
#include "descriptors.h"
#include "hooks.h"
#include "shader_edit.h"
#include "shader_reflect.h"
#include "tracker.h"
#include "transport.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dxinsp
{

namespace
{

/** The pipeline copy this file asks for (VariantKind, OverlayVariant). */
constexpr uint64_t kMeshVariant = 32;

/** What a measured draw wrote, held until the capture's lists have run. */
struct PendingMesh
{
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;   // not AddRef'd: only a key for the frame it ran in
    uint64_t listId = 0;
    uint32_t passIndex = 0;
    uint32_t drawIndex = 0;
    uint32_t command = 0;
    std::string method;
    std::string topology;
    bool measured = false;
    bool truncated = false;
    uint32_t stride = 0;
    uint32_t maxVertices = 0;
    std::string note;
    /** The outputs the record holds, in the order the stream-output entries declare them. */
    std::vector<ShaderOutputParam> outputs;
    std::vector<uint32_t> offsets;
    ComPtr<ID3D12Resource> staging;      // the vertex records
    ComPtr<ID3D12Resource> counter;      // the stream-output counter: bytes actually written
    KeepList keep;
};

static_assert(std::is_nothrow_move_constructible_v<PendingMesh>);

struct MeshState
{
    std::mutex mutex;
    MeshOutputRequest request;
    std::vector<PendingMesh> pending;
    /** The stream-output root signatures made so far, by the application's signature. */
    std::map<ID3D12RootSignature*, ComPtr<ID3D12RootSignature>> signatures;
};

MeshState& State()
{
    static MeshState* s = new MeshState();
    return *s;
}

typedef HRESULT(WINAPI* PFN_D3D12CreateVersionedRootSignatureDeserializer)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI* PFN_D3D12SerializeVersionedRootSignature)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);

/**
 * The application's root signature again, with ALLOW_STREAM_OUTPUT on it. Null with `error` when
 * the blob it was made from was not kept (a signature created before the library attached) or the
 * runtime refuses the copy.
 */
ID3D12RootSignature* StreamOutputSignature(ID3D12Device* device, ID3D12RootSignature* original, std::string& error)
{
    if (!original)
    {
        error = "the draw had no root signature bound";
        return nullptr;
    }
    MeshState& s = State();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.signatures.find(original);
        if (it != s.signatures.end())
            return it->second.get();
    }
    std::shared_ptr<const RootSignatureInfo> info = RootSignatures::Get().Find(original);
    if (!info || info->blob.empty())
    {
        error = "the root signature's blob was not recorded (it was created before the library attached)";
        return nullptr;
    }
    if (info->flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT)
    {
        // Already allows it: the application's own signature will do.
        std::lock_guard<std::mutex> lock(s.mutex);
        original->AddRef();
        s.signatures[original] = ComPtr<ID3D12RootSignature>(original);
        return original;
    }
    static HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    static auto deserialize = d3d12 ? (PFN_D3D12CreateVersionedRootSignatureDeserializer)GetProcAddress(
                                          d3d12, "D3D12CreateVersionedRootSignatureDeserializer")
                                    : nullptr;
    static auto serialize = d3d12 ? (PFN_D3D12SerializeVersionedRootSignature)GetProcAddress(
                                        d3d12, "D3D12SerializeVersionedRootSignature")
                                  : nullptr;
    if (!deserialize || !serialize)
    {
        error = "d3d12.dll has no versioned root signature serializer";
        return nullptr;
    }
    ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
    HRESULT hr = deserialize(info->blob.data(), info->blob.size(), IID_PPV_ARGS(reader.put()));
    if (FAILED(hr) || !reader)
    {
        error = "the root signature blob did not deserialize (" + HrText(hr) + ")";
        return nullptr;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = reader->GetUnconvertedRootSignatureDesc();
    if (!desc)
    {
        error = "the root signature blob has no description";
        return nullptr;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *desc;
    switch (copy.Version)
    {
        case D3D_ROOT_SIGNATURE_VERSION_1_0: copy.Desc_1_0.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
        case D3D_ROOT_SIGNATURE_VERSION_1_1: copy.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
        default: copy.Desc_1_2.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT; break;
    }
    ComPtr<ID3DBlob> blob, errors;
    hr = serialize(&copy, blob.put(), errors.put());
    if (FAILED(hr) || !blob)
    {
        error = "the root signature could not be serialized with stream output (" + HrText(hr) + ")";
        return nullptr;
    }
    ComPtr<ID3D12RootSignature> made;
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(made.put()));
    if (FAILED(hr) || !made)
    {
        error = "a root signature allowing stream output could not be created (" + HrText(hr) + ")";
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    ID3D12RootSignature* raw = made.get();
    s.signatures[original] = std::move(made);
    return raw;
}

/** A buffer of our own the draw streams into, in `state`. */
ComPtr<ID3D12Resource> NewStreamBuffer(ID3D12Device* device, uint64_t size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> out;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(out.put()))))
        out.reset();
    return out;
}

/**
 * The stream-output declaration for a vertex shader's outputs, and the record's layout. Every
 * component the shader wrote is taken, in signature order, packed four bytes at a time.
 */
void BuildDeclaration(const std::vector<ShaderOutputParam>& outputs, std::vector<D3D12_SO_DECLARATION_ENTRY>& entries,
    std::vector<uint32_t>& offsets, uint32_t& stride)
{
    stride = 0;
    for (const ShaderOutputParam& o : outputs)
    {
        D3D12_SO_DECLARATION_ENTRY e{};
        e.Stream = 0;
        e.SemanticName = o.semantic.c_str();   // the caller keeps `outputs` alive across the call
        e.SemanticIndex = o.semanticIndex;
        e.StartComponent = (BYTE)o.startComponent;
        e.ComponentCount = (BYTE)o.componentCount;
        e.OutputSlot = 0;
        entries.push_back(e);
        offsets.push_back(stride);
        stride += o.componentCount * 4;
    }
}

/** Issues the measured draw alone, with the pipeline copy that streams its outputs out. */
class MeshReplay final : public PassReplay
{
public:
    MeshReplay(uint32_t drawIndex, const std::vector<D3D12_SO_DECLARATION_ENTRY>& entries, const UINT* strides,
        ID3D12Device* device, PendingMesh& out)
        : _target(drawIndex), _entries(entries), _strides(strides), _device(device), _out(out) {}

    void SetPipeline(ID3D12GraphicsCommandList*, ID3D12PipelineState* pipeline) override { _pipeline = pipeline; }

    void SetGraphicsRootSignature(ID3D12GraphicsCommandList* list, ID3D12RootSignature* signature) override
    {
        // The copy is built against a signature of ours, so that is what the list binds: the two
        // are layout-compatible, and the root arguments the application set still apply.
        _appSignature = signature;
        std::string error;
        ID3D12RootSignature* ours = StreamOutputSignature(_device, signature, error);
        if (!ours)
        {
            Note(error);
            list->SetGraphicsRootSignature(signature);
            return;
        }
        list->SetGraphicsRootSignature(ours);
        _signature = ours;
    }

    void IssueDraw(ID3D12GraphicsCommandList* list, const std::function<void(ID3D12GraphicsCommandList*)>& draw) override
    {
        const uint32_t index = _drawIndex++;
        if (index != _target)
            return;
        if (!_pipeline)
        {
            Note("the draw had no pipeline bound");
            return;
        }
        if (!_signature)
        {
            if (_out.note.empty())
                Note("the draw's root signature could not be copied with stream output");
            return;
        }
        std::string error;
        PipelineVariant v;
        v.soEntries = _entries.data();
        v.soEntryCount = (uint32_t)_entries.size();
        v.soStrides = _strides;
        v.soStrideCount = 1;
        v.rootSignature = _signature;
        ID3D12PipelineState* variant = VariantOf(_pipeline, kMeshVariant, v, error);
        if (!variant)
        {
            Note("the draw's pipeline could not be copied with stream output: " + error);
            return;
        }
        list->SetPipelineState(variant);
        KeepObject(_out.keep, variant);
        draw(list);
        _issued = true;
    }

    void Skip() override
    {
        if (_drawIndex++ == _target)
            Note("an indirect execution's draws cannot be issued one at a time");
    }

    bool issued() const { return _issued; }

private:
    void Note(const std::string& text)
    {
        if (_noted)
            return;
        _noted = true;
        _out.note += (_out.note.empty() ? "" : "; ") + text;
    }

    uint32_t _target;
    const std::vector<D3D12_SO_DECLARATION_ENTRY>& _entries;
    const UINT* _strides;
    ID3D12Device* _device;
    PendingMesh& _out;
    ID3D12PipelineState* _pipeline = nullptr;
    ID3D12RootSignature* _appSignature = nullptr;
    ID3D12RootSignature* _signature = nullptr;
    uint32_t _drawIndex = 0;
    bool _issued = false;
    bool _noted = false;
};

/** The command index and method of the pass's `drawIndex`-th draw, and the pipeline bound at it. */
ID3D12PipelineState* FindDraw(const ListOps& ops, CommandRecorder* rec, uint32_t drawIndex, uint32_t& command, std::string& method);

}  // namespace

void StartMeshOutput(const MeshOutputRequest& request)
{
    MeshState& s = State();
    std::vector<PendingMesh> pending;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        pending.swap(s.pending);
        s.request = request;
    }
    if (request.enabled)
        Log("mesh output: following draw %u of pass %u", request.drawIndex, request.passIndex);
}

bool MeshOutputRequested()
{
    MeshState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.request.enabled;
}

bool MatchMeshOutputPass(const MeasuredPass& pass)
{
    MeshState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.request.enabled && s.request.passIndex == pass.passIndex;
}

void MeasureMeshOutput(MeasuredPass& pass, CommandRecorder* rec, const ListOps& ops)
{
    MeshOutputRequest request;
    {
        MeshState& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        request = s.request;
    }
    if (!request.enabled)
        return;

    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = pass.list;
    ID3D12Device* device = pass.device;
    PendingMesh m;
    m.list = list;
    m.listId = pass.listId;
    m.passIndex = pass.passIndex;
    m.drawIndex = request.drawIndex;
    m.maxVertices = std::max<uint32_t>(1, request.maxVertices);

    // The pipeline the measured draw was drawn with, and the command it is.
    ID3D12PipelineState* pipeline = nullptr;
    {
        // Which pipeline the measured draw was drawn with, from the pipeline calls in effect at it.
        // A pipeline op asks the replay to bind, and does nothing else, so it is safe to run with no
        // list; nothing else here is run at all. The calls in effect when the pass began count too:
        // an engine that binds its pipeline before OMSetRenderTargets is the usual case.
        struct Probe final : PassReplay
        {
            ID3D12PipelineState* pipeline = nullptr;
            void SetPipeline(ID3D12GraphicsCommandList*, ID3D12PipelineState* p) override { pipeline = p; }
            void IssueDraw(ID3D12GraphicsCommandList*, const std::function<void(ID3D12GraphicsCommandList*)>&) override {}
            void Skip() override {}
        } probe;
        auto probeOp = [&](const LoggedOp& op) {
            if (op.key.policy == OpPolicy::Replace && op.key.name == std::string(ops::kPipeline))
                op.op(nullptr, probe);
        };
        const std::vector<const LoggedOp*> state = EffectiveOps(ops.ops, ops.passFirst);
        for (const LoggedOp* op : state)
            probeOp(*op);
        uint32_t index = 0;
        for (size_t k = ops.passFirst; k < ops.ops.size(); ++k)
        {
            const LoggedOp& op = ops.ops[k];
            probeOp(op);
            if (op.key.policy != OpPolicy::Draw)
                continue;
            if (index++ != request.drawIndex)
                continue;
            pipeline = probe.pipeline;
            m.command = op.command;
            m.method = RecordedMethod(rec, op.command);
            break;
        }
    }

    std::vector<D3D12_SO_DECLARATION_ENTRY> entries;
    UINT strides[1] = {0};
    if (!pass.note.empty())
    {
        m.note = pass.note;
    }
    else if (!pipeline)
    {
        m.note = "the pass holds no draw " + std::to_string(request.drawIndex) +
            " with a pipeline: the frame captured now is not the frame the draw was chosen in";
    }
    else
    {
        const void* code = nullptr;
        size_t size = 0;
        if (!ShaderEditor::Get().StageBytecode(pipeline, "vertex", code, size))
        {
            m.note = "the draw has no vertex shader to stream out of (a mesh shader pipeline writes no vertex outputs)";
        }
        else
        {
            m.outputs = ShaderOutputSignature(code, size);
            if (m.outputs.empty())
            {
                m.note = "the vertex shader's output signature could not be read (dxcompiler.dll is needed for DXIL)";
            }
            else
            {
                BuildDeclaration(m.outputs, entries, m.offsets, m.stride);
                strides[0] = m.stride;
            }
        }
    }
    // The preview draws points, lines or triangles by the topology's name (primitiveKind in
    // mesh_output.ts), so the pipeline's primitive kind answers it. Which list or strip of that kind
    // the draw set is IASetPrimitiveTopology's, and does not change what is drawn here.
    if (pipeline)
    {
        D3D12_PRIMITIVE_TOPOLOGY_TYPE kind = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
        if (ShaderEditor::Get().PrimitiveTopologyTypeOf(pipeline, kind))
        {
            switch (kind)
            {
                case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: m.topology = "D3D_PRIMITIVE_TOPOLOGY_POINTLIST"; break;
                case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: m.topology = "D3D_PRIMITIVE_TOPOLOGY_LINELIST"; break;
                case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH: m.topology = "D3D_PRIMITIVE_TOPOLOGY_PATCHLIST"; break;
                default: m.topology = "D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST"; break;
            }
        }
    }
    if (!m.stride)
    {
        MeshState& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pending.push_back(std::move(m));
        return;
    }

    // The buffer the draw streams into, and the counter saying how much of it was written.
    const uint64_t bufferSize = (uint64_t)m.stride * m.maxVertices;
    ComPtr<ID3D12Resource> buffer = NewStreamBuffer(device, bufferSize, D3D12_RESOURCE_STATE_STREAM_OUT);
    ComPtr<ID3D12Resource> counter = NewStreamBuffer(device, 8, D3D12_RESOURCE_STATE_STREAM_OUT);
    if (!buffer || !counter)
    {
        m.note = "no memory for the stream output buffer";
        MeshState& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pending.push_back(std::move(m));
        return;
    }
    KeepObject(m.keep, buffer.get());
    KeepObject(m.keep, counter.get());

    D3D12_STREAM_OUTPUT_BUFFER_VIEW view{};
    view.BufferLocation = buffer->GetGPUVirtualAddress();
    view.SizeInBytes = bufferSize;
    view.BufferFilledSizeLocation = counter->GetGPUVirtualAddress();

    // Nothing is rasterized (D3D12_SO_NO_RASTERIZED_STREAM), so the pass's own targets are left
    // bound: the draw writes into the stream buffer and nowhere else.
    const std::vector<const LoggedOp*> before = EffectiveOps(ops.ops, ops.passFirst);
    list->SOSetTargets(0, 1, &view);
    MeshReplay replay(request.drawIndex, entries, strides, device, m);
    for (const LoggedOp* op : before)
        op->op(list, replay);
    for (size_t k = ops.passFirst; k < ops.ops.size(); ++k)
        ops.ops[k].op(list, replay);
    list->SOSetTargets(0, 0, nullptr);

    if (replay.issued())
    {
        ComPtr<ID3D12Resource> staging = NewMeasurementReadback(device, bufferSize);
        ComPtr<ID3D12Resource> countStaging = NewMeasurementReadback(device, 8);
        if (staging && countStaging)
        {
            Transition(list, buffer.get(), 0, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(list, counter.get(), 0, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(staging.get(), 0, buffer.get(), 0, bufferSize);
            list->CopyBufferRegion(countStaging.get(), 0, counter.get(), 0, 8);
            m.staging = std::move(staging);
            m.counter = std::move(countStaging);
            m.measured = true;
        }
        else
        {
            m.note += std::string(m.note.empty() ? "" : "; ") + "no staging memory for the vertex records";
        }
    }
    else if (m.note.empty())
    {
        m.note = "the draw was not issued again: it is not one this pass kept";
    }

    MeshState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pending.push_back(std::move(m));
}

void AssignMeshOutputFrame(ID3D12GraphicsCommandList* list, uint32_t frame)
{
    MeshState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (PendingMesh& m : s.pending)
        if (m.list == list && m.frame == UINT32_MAX)
            m.frame = frame;
}

void SendMeshOutput()
{
    MeshState& s = State();
    std::vector<PendingMesh> pending;
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        pending.swap(s.pending);
        enabled = s.request.enabled;
        s.request = MeshOutputRequest{};
        s.signatures.clear();
    }
    if (!enabled)
        return;

    for (PendingMesh& m : pending)
    {
        std::vector<uint8_t> records;
        uint32_t vertices = 0;
        if (m.frame == UINT32_MAX)
        {
            m.measured = false;
            m.note = "the command list was not executed during the capture";
        }
        if (m.measured && m.counter && m.staging)
        {
            uint64_t written = 0;
            {
                ScopedInternal internal;
                void* p = nullptr;
                if (SUCCEEDED(m.counter->Map(0, nullptr, &p)) && p)
                {
                    memcpy(&written, p, sizeof(written));
                    m.counter->Unmap(0, nullptr);
                }
            }
            const uint64_t capacity = (uint64_t)m.stride * m.maxVertices;
            if (written > capacity)
            {
                written = capacity;
                m.truncated = true;
            }
            vertices = m.stride ? (uint32_t)(written / m.stride) : 0;
            ScopedInternal internal;
            void* p = nullptr;
            if (SUCCEEDED(m.staging->Map(0, nullptr, &p)) && p)
            {
                const uint8_t* bytes = static_cast<const uint8_t*>(p);
                records.assign(bytes, bytes + (size_t)written);
                m.staging->Unmap(0, nullptr);
            }
            else
            {
                m.measured = false;
                m.note += std::string(m.note.empty() ? "" : "; ") + "the vertex records could not be read back";
            }
        }

        JsonWriter w;
        w.BeginObject();
        w.Key("action");
        w.String("CaptureMeshOutput");
        w.Key("command");
        w.Uint(m.command);
        w.Key("commandBuffer");
        w.Uint(m.listId);
        w.Key("method");
        w.String(m.method);
        w.Key("frame");
        w.Uint(m.frame == UINT32_MAX ? 0 : m.frame);
        w.Key("passIndex");
        w.Uint(m.passIndex);
        w.Key("drawIndex");
        w.Uint(m.drawIndex);
        w.Key("measured");
        w.Boolean(m.measured);
        w.Key("topology");
        w.String(m.topology);
        w.Key("stride");
        w.Uint(m.stride);
        w.Key("vertices");
        w.Uint(vertices);
        w.Key("truncated");
        w.Boolean(m.truncated);
        w.Key("outputs");
        w.BeginArray();
        for (size_t k = 0; k < m.outputs.size(); ++k)
        {
            const ShaderOutputParam& o = m.outputs[k];
            w.BeginObject();
            w.Key("name");
            w.String(o.semanticIndex ? o.semantic + std::to_string(o.semanticIndex) : o.semantic);
            w.Key("offset");
            w.Uint(k < m.offsets.size() ? m.offsets[k] : 0);
            w.Key("components");
            w.Uint(o.componentCount);
            w.Key("base");
            w.String(o.base);
            // The mesh view draws the output it knows is the clip-space position (positionOutput in
            // mesh_output.ts looks for "Position", the name Vulkan's gl_Position carries). D3D12
            // says so through the system value, whose spelling is the reflection's: match the word
            // rather than one spelling of it, and fall back to the semantic for a shader whose
            // system value did not survive (SV_POSITION as an ordinary semantic).
            auto isPosition = [](const std::string& text) {
                std::string upper;
                for (char c : text)
                    upper += (char)toupper((unsigned char)c);
                return upper.find("POSITION") != std::string::npos;
            };
            const bool position = isPosition(o.systemValue) || (o.systemValue.empty() && isPosition(o.semantic));
            if (position)
            {
                w.Key("builtin");
                w.String("Position");
            }
            else if (!o.systemValue.empty())
            {
                w.Key("builtin");
                w.String(o.systemValue);
            }
            w.EndObject();
        }
        w.EndArray();
        w.Key("size");
        w.Uint(records.size());
        if (!m.note.empty())
        {
            w.Key("note");
            w.String(m.note);
        }
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));

        if (!records.empty())
        {
            JsonWriter h;
            h.BeginObject();
            h.Key("action");
            h.String("CaptureMeshOutputData");
            h.Key("command");
            h.Uint(m.command);
            h.Key("commandBuffer");
            h.Uint(m.listId);
            h.Key("size");
            h.Uint(records.size());
            h.EndObject();
            Transport::Get().SendBinary(std::move(h.str()), records.data(), records.size());
        }
        Log("mesh output: draw %u of pass %u (%s): %u vertices of %u bytes%s", m.drawIndex, m.passIndex, m.method.c_str(),
            vertices, m.stride, m.note.empty() ? "" : (" -- " + m.note).c_str());
    }
}

}  // namespace dxinsp
