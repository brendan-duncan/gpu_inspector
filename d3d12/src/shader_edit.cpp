// Live shader editing (shader_edit.h): a deep copy of every pipeline's description, kept so a
// stage can be swapped and the pipeline made again; the replacement is tracked as an object of
// its own and bound in place of the original by SetPipelineState (Substitute).
//
// A pipeline's description is only borrowed at creation — the bytecode, the input layout's
// semantic names and the stream-output declarations are the application's memory, freed as soon
// as the call returns — so the copy owns every buffer the description points at and the pointers
// in the copy point into those. A stream (CreatePipelineState) is copied as bytes and walked
// subobject by subobject to fix up the same pointers inside it.
//
// A replacement that stops being bound (a newer edit, a restore, the original's release) is
// retired and destroyed three presents later, once no command list still in flight names it.
#include "shader_edit.h"

#include "hooks.h"
#include "shader_reflect.h"
#include "tracker.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace dxinsp {

namespace {

// The UI's stage names, in the order of the description's members.
constexpr const char* kStageNames[] = {"vertex", "fragment", "tess_control", "tess_eval", "geometry", "compute", "task", "mesh"};

bool StageKnown(const std::string& stage) {
    for (const char* s : kStageNames)
        if (stage == s) return true;
    return false;
}

// One pipeline's description, deep-copied, and where each stage's bytecode sits in it.
struct PipelineRecord {
    enum class Kind { Graphics, Compute, Stream } kind = Kind::Graphics;
    ID3D12Device* device = nullptr;   // not AddRef'd: a pipeline never outlives its device
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    std::vector<uint8_t> stream;
    // Owned storage the description points into. A deque never moves its elements, so the
    // pointers stay valid as more are added.
    std::deque<std::vector<uint8_t>> buffers;
    std::deque<std::string> strings;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    std::vector<D3D12_SO_DECLARATION_ENTRY> soEntries;
    std::vector<UINT> soStrides;
    std::vector<D3D12_VIEW_INSTANCE_LOCATION> viewInstances;
    /** The root signature as given (null for one embedded in the shaders); not AddRef'd. */
    ID3D12RootSignature* rootSignature = nullptr;
    /** UI stage name -> offset of its D3D12_SHADER_BYTECODE from Base(). */
    std::map<std::string, size_t> stageOffsets;
    /** What the copy could not keep (a cached PSO), told to the UI with the result. */
    std::string note;
    /** A stream with a subobject this library does not know cannot be copied faithfully. */
    bool unsupported = false;
    /** Stage -> the bytecode currently replacing it. */
    std::map<std::string, std::vector<uint8_t>> edits;
    ID3D12PipelineState* replacement = nullptr;

    PipelineRecord() = default;
    PipelineRecord(const PipelineRecord&) = delete;
    PipelineRecord& operator=(const PipelineRecord&) = delete;

    const uint8_t* Base() const {
        switch (kind) {
            case Kind::Graphics: return reinterpret_cast<const uint8_t*>(&graphics);
            case Kind::Compute: return reinterpret_cast<const uint8_t*>(&compute);
            default: return stream.data();
        }
    }
    /** The stage's bytecode in the copy, or null when the pipeline has no such stage. */
    const D3D12_SHADER_BYTECODE* Stage(const std::string& stage) const {
        auto it = stageOffsets.find(stage);
        if (it == stageOffsets.end()) return nullptr;
        auto* bc = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(Base() + it->second);
        return bc->pShaderBytecode && bc->BytecodeLength ? bc : nullptr;
    }
};

// --- Owning the description's pointers ----------------------------------------------------

void OwnBytecode(PipelineRecord& rec, D3D12_SHADER_BYTECODE& bc) {
    if (!bc.pShaderBytecode || !bc.BytecodeLength) {
        bc = {nullptr, 0};
        return;
    }
    const uint8_t* p = static_cast<const uint8_t*>(bc.pShaderBytecode);
    rec.buffers.emplace_back(p, p + bc.BytecodeLength);
    bc.pShaderBytecode = rec.buffers.back().data();
}

const char* OwnString(PipelineRecord& rec, const char* s) {
    if (!s) return nullptr;
    rec.strings.emplace_back(s);
    return rec.strings.back().c_str();
}

void OwnInputLayout(PipelineRecord& rec, D3D12_INPUT_LAYOUT_DESC& il) {
    if (!il.pInputElementDescs || !il.NumElements) {
        il = {nullptr, 0};
        return;
    }
    rec.inputElements.assign(il.pInputElementDescs, il.pInputElementDescs + il.NumElements);
    for (D3D12_INPUT_ELEMENT_DESC& e : rec.inputElements) e.SemanticName = OwnString(rec, e.SemanticName);
    il.pInputElementDescs = rec.inputElements.data();
}

void OwnStreamOutput(PipelineRecord& rec, D3D12_STREAM_OUTPUT_DESC& so) {
    if (so.pSODeclaration && so.NumEntries) {
        rec.soEntries.assign(so.pSODeclaration, so.pSODeclaration + so.NumEntries);
        for (D3D12_SO_DECLARATION_ENTRY& e : rec.soEntries) e.SemanticName = OwnString(rec, e.SemanticName);
        so.pSODeclaration = rec.soEntries.data();
    } else {
        so.pSODeclaration = nullptr;
        so.NumEntries = 0;
    }
    if (so.pBufferStrides && so.NumStrides) {
        rec.soStrides.assign(so.pBufferStrides, so.pBufferStrides + so.NumStrides);
        so.pBufferStrides = rec.soStrides.data();
    } else {
        so.pBufferStrides = nullptr;
        so.NumStrides = 0;
    }
}

void OwnViewInstancing(PipelineRecord& rec, D3D12_VIEW_INSTANCING_DESC& vi) {
    if (vi.pViewInstanceLocations && vi.ViewInstanceCount) {
        rec.viewInstances.assign(vi.pViewInstanceLocations, vi.pViewInstanceLocations + vi.ViewInstanceCount);
        vi.pViewInstanceLocations = rec.viewInstances.data();
    } else {
        vi.pViewInstanceLocations = nullptr;
        vi.ViewInstanceCount = 0;
    }
}

// A cached PSO blob describes the original shaders; the runtime rejects it with different ones,
// so the rebuild goes without and says so.
void DropCachedPso(PipelineRecord& rec, D3D12_CACHED_PIPELINE_STATE& cached) {
    if (cached.pCachedBlob && cached.CachedBlobSizeInBytes) rec.note += "the cached PSO blob was dropped; ";
    cached = {nullptr, 0};
}

// --- Streams --------------------------------------------------------------------------------

// The layout d3dx12.h's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT gives every subobject: the type
// enum at a pointer-aligned offset, the payload at its own alignment, the whole padded to
// pointer alignment. That is what the runtime parses, so it is what any stream looks like.
template <typename T>
struct StreamSubobject {
    alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T inner;
};

struct SubobjectLayout {
    size_t size = 0;
    size_t payload = 0;
};

template <typename T>
constexpr SubobjectLayout LayoutOf() {
    return {sizeof(StreamSubobject<T>), offsetof(StreamSubobject<T>, inner)};
}

// A pointer-sized payload follows the type after padding; a 4-byte one follows it at once.
static_assert(LayoutOf<D3D12_SHADER_BYTECODE>().payload == sizeof(void*) && LayoutOf<D3D12_SHADER_BYTECODE>().size == 3 * sizeof(void*));
static_assert(LayoutOf<UINT>().payload == 4 && LayoutOf<UINT>().size == sizeof(void*));

bool SubobjectLayoutFor(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, SubobjectLayout& out) {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: out = LayoutOf<ID3D12RootSignature*>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: out = LayoutOf<D3D12_SHADER_BYTECODE>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: out = LayoutOf<D3D12_STREAM_OUTPUT_DESC>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: out = LayoutOf<D3D12_BLEND_DESC>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: out = LayoutOf<UINT>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: out = LayoutOf<D3D12_RASTERIZER_DESC>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: out = LayoutOf<D3D12_DEPTH_STENCIL_DESC>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: out = LayoutOf<D3D12_INPUT_LAYOUT_DESC>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: out = LayoutOf<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: out = LayoutOf<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: out = LayoutOf<D3D12_RT_FORMAT_ARRAY>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: out = LayoutOf<DXGI_FORMAT>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: out = LayoutOf<DXGI_SAMPLE_DESC>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: out = LayoutOf<UINT>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: out = LayoutOf<D3D12_CACHED_PIPELINE_STATE>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: out = LayoutOf<D3D12_PIPELINE_STATE_FLAGS>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: out = LayoutOf<D3D12_DEPTH_STENCIL_DESC1>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: out = LayoutOf<D3D12_VIEW_INSTANCING_DESC>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: out = LayoutOf<D3D12_DEPTH_STENCIL_DESC2>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: out = LayoutOf<D3D12_RASTERIZER_DESC1>(); return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: out = LayoutOf<D3D12_RASTERIZER_DESC2>(); return true;
        default: return false;
    }
}

const char* StreamStageName(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: return "vertex";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: return "fragment";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: return "tess_control";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: return "tess_eval";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: return "geometry";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return "compute";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: return "task";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return "mesh";
        default: return nullptr;
    }
}

void CopyStream(PipelineRecord& rec, const D3D12_PIPELINE_STATE_STREAM_DESC& desc) {
    if (!desc.pPipelineStateSubobjectStream || !desc.SizeInBytes) {
        rec.unsupported = true;
        rec.note += "empty pipeline stream; ";
        return;
    }
    const uint8_t* src = static_cast<const uint8_t*>(desc.pPipelineStateSubobjectStream);
    rec.stream.assign(src, src + desc.SizeInBytes);
    size_t pos = 0;
    while (pos < rec.stream.size()) {
        if (rec.stream.size() - pos < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
            rec.unsupported = true;
            rec.note += "truncated pipeline stream; ";
            return;
        }
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        memcpy(&type, rec.stream.data() + pos, sizeof(type));
        SubobjectLayout layout;
        if (!SubobjectLayoutFor(type, layout) || layout.size > rec.stream.size() - pos) {
            rec.unsupported = true;
            rec.note += "unknown pipeline stream subobject type " + std::to_string((int)type) + "; ";
            return;
        }
        uint8_t* payload = rec.stream.data() + pos + layout.payload;
        if (const char* stage = StreamStageName(type)) {
            rec.stageOffsets[stage] = pos + layout.payload;
            OwnBytecode(rec, *reinterpret_cast<D3D12_SHADER_BYTECODE*>(payload));
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            memcpy(&rec.rootSignature, payload, sizeof(rec.rootSignature));
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) {
            OwnStreamOutput(rec, *reinterpret_cast<D3D12_STREAM_OUTPUT_DESC*>(payload));
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
            OwnInputLayout(rec, *reinterpret_cast<D3D12_INPUT_LAYOUT_DESC*>(payload));
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING) {
            OwnViewInstancing(rec, *reinterpret_cast<D3D12_VIEW_INSTANCING_DESC*>(payload));
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO) {
            DropCachedPso(rec, *reinterpret_cast<D3D12_CACHED_PIPELINE_STATE*>(payload));
        }
        pos += layout.size;
    }
}

// --- Rebuilding -----------------------------------------------------------------------------

// The description with the current edits in place of the stages they replace. `base` is a
// working copy of the record's description (struct or stream bytes).
void ApplyEdits(const PipelineRecord& rec, uint8_t* base) {
    for (auto& [stage, bytes] : rec.edits) {
        auto it = rec.stageOffsets.find(stage);
        if (it == rec.stageOffsets.end()) continue;
        D3D12_SHADER_BYTECODE bc{bytes.data(), bytes.size()};
        memcpy(base + it->second, &bc, sizeof(bc));
    }
}

bool CreateWithEdits(PipelineRecord& rec, ID3D12PipelineState*& out, std::string& error) {
    ScopedInternal internal;
    out = nullptr;
    HRESULT hr = E_FAIL;
    const char* call = "";
    switch (rec.kind) {
        case PipelineRecord::Kind::Graphics: {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = rec.graphics;
            ApplyEdits(rec, reinterpret_cast<uint8_t*>(&d));
            call = "CreateGraphicsPipelineState";
            hr = rec.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out));
            break;
        }
        case PipelineRecord::Kind::Compute: {
            D3D12_COMPUTE_PIPELINE_STATE_DESC d = rec.compute;
            ApplyEdits(rec, reinterpret_cast<uint8_t*>(&d));
            call = "CreateComputePipelineState";
            hr = rec.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out));
            break;
        }
        case PipelineRecord::Kind::Stream: {
            std::vector<uint8_t> s = rec.stream;
            ApplyEdits(rec, s.data());
            ComPtr<ID3D12Device2> device2;
            if (FAILED(rec.device->QueryInterface(IID_PPV_ARGS(device2.put()))) || !device2) {
                error = "ID3D12Device2 is not available: a pipeline stream cannot be rebuilt";
                return false;
            }
            D3D12_PIPELINE_STATE_STREAM_DESC sd{s.size(), s.data()};
            call = "CreatePipelineState";
            hr = device2->CreatePipelineState(&sd, IID_PPV_ARGS(&out));
            break;
        }
    }
    if (FAILED(hr) || !out) {
        error = std::string(call) + " failed: " + HrText(hr) +
                (ConfigFlag("DXINSP_DEBUG_LAYER") ? " (see the validation messages)" : " (run with the debug layer for the reason)");
        out = nullptr;
        return false;
    }
    return true;
}

// The blob name the original carries for a stage ("<stage>:<entry>"), so the replacement's
// shaders are found under the same names; a stage the original has no blob for is named from
// its own bytecode.
std::string BlobName(const TrackedObject& original, const std::string& stage, const void* data, size_t size) {
    const std::string prefix = stage + ":";
    for (auto& [name, blob] : original.blobs)
        if (name.compare(0, prefix.size(), prefix) == 0) return name;
    ShaderInfo info = ReflectShader(data, size);
    return prefix + (info.entryPoint.empty() ? "main" : info.entryPoint);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ShaderEditor

struct ShaderEditor::Impl {
    /**
     * Records, edits and rebuilds: held for the whole of an edit (a rebuild is rare and slow
     * anyway). Recursive so a creation hook that reported our own rebuild would not deadlock.
     */
    std::recursive_mutex mutex;
    std::unordered_map<ID3D12PipelineState*, std::unique_ptr<PipelineRecord>> records;
    /** Original -> replacement, read by every SetPipelineState. */
    std::shared_mutex substitutionMutex;
    std::unordered_map<ID3D12PipelineState*, ID3D12PipelineState*> substitutions;
    std::atomic<size_t> editCount{0};
    /** Replacements no longer bound, destroyed a few presents after they were retired. */
    std::mutex retiredMutex;
    struct Retired {
        ID3D12PipelineState* pipeline;
        uint64_t destroyAt;
    };
    std::vector<Retired> retired;
    uint64_t presents = 0;

    void Retire(ID3D12PipelineState* pipeline) {
        if (!pipeline) return;
        std::lock_guard<std::mutex> lock(retiredMutex);
        // Three presents: a list recorded with the replacement and executed this frame has run
        // by then, whatever the swap chain's buffering.
        retired.push_back({pipeline, presents + 3});
    }

    /** Registers `pipeline` as the record's replacement and retires the previous one. */
    void Install(PipelineRecord& rec, ID3D12PipelineState* original, ID3D12PipelineState* pipeline) {
        ID3D12PipelineState* previous;
        {
            std::unique_lock lock(substitutionMutex);
            previous = rec.replacement;
            rec.replacement = pipeline;
            if (pipeline) substitutions[original] = pipeline;
            else substitutions.erase(original);
            editCount.store(substitutions.size(), std::memory_order_release);
        }
        Retire(previous);
    }

    /** Makes the pipeline again with the record's edits, tracks it and installs it. */
    bool Rebuild(PipelineRecord& rec, ID3D12PipelineState* original, std::string& error, uint64_t& replacementId) {
        ID3D12PipelineState* pipeline = nullptr;
        if (!CreateWithEdits(rec, pipeline, error)) return false;
        TrackedObject orig;
        bool tracked = Tracker::Get().Find(original, orig);
        // The replacement is an object of its own for the UI: same class, same creating call and
        // arguments, its name marked. Track ignores calls made under Internal(), so none of this
        // is under a ScopedInternal.
        HookPipelineState(pipeline);
        const char* cmd = tracked && !orig.cmd.empty() ? orig.cmd.c_str()
                          : rec.kind == PipelineRecord::Kind::Stream ? "CreatePipelineState"
                          : rec.kind == PipelineRecord::Kind::Compute ? "CreateComputePipelineState" : "CreateGraphicsPipelineState";
        replacementId = Tracker::Get().Track(pipeline, "ID3D12PipelineState", cmd, rec.device, orig.args);
        std::string label = (orig.label.empty() ? "PipelineState " + std::to_string(orig.id) : orig.label) + " (edited)";
        Tracker::Get().SetLabel(pipeline, label);
        for (auto& [stage, offset] : rec.stageOffsets) {
            const uint8_t* data = nullptr;
            size_t size = 0;
            auto edit = rec.edits.find(stage);
            if (edit != rec.edits.end()) {
                data = edit->second.data();
                size = edit->second.size();
            } else if (const D3D12_SHADER_BYTECODE* bc = rec.Stage(stage)) {
                data = static_cast<const uint8_t*>(bc->pShaderBytecode);
                size = bc->BytecodeLength;
            }
            if (!data || !size) continue;
            Tracker::Get().AddBlob(pipeline, BlobName(orig, stage, data, size), std::make_shared<std::vector<uint8_t>>(data, data + size));
        }
        Install(rec, original, pipeline);
        Log("shader edit: pipeline %llu rebuilt as %llu (%zu edited stage(s))", (unsigned long long)orig.id,
            (unsigned long long)replacementId, rec.edits.size());
        return true;
    }

    /** The record of the tracked pipeline `id`, or null with `error`. */
    PipelineRecord* Lookup(uint64_t id, ID3D12PipelineState*& pipeline, std::string& error) {
        TrackedObject obj;
        if (!Tracker::Get().FindById(id, obj) || obj.type != "ID3D12PipelineState") {
            error = "object " + std::to_string(id) + " is not a pipeline state";
            return nullptr;
        }
        pipeline = reinterpret_cast<ID3D12PipelineState*>(static_cast<uintptr_t>(obj.handle));
        auto it = records.find(pipeline);
        if (it == records.end()) {
            error = "the pipeline's description was not recorded (loaded from a pipeline library?): it cannot be rebuilt";
            return nullptr;
        }
        return it->second.get();
    }

    void AddRecord(ID3D12PipelineState* pipeline, std::unique_ptr<PipelineRecord> rec) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        // The address of a released pipeline can come back for a new one; a stale record's
        // replacement must not follow it.
        auto it = records.find(pipeline);
        if (it != records.end()) Install(*it->second, pipeline, nullptr);
        records[pipeline] = std::move(rec);
    }
};

ShaderEditor& ShaderEditor::Get() {
    static ShaderEditor* instance = new ShaderEditor();
    return *instance;
}

ShaderEditor::Impl& ShaderEditor::impl() {
    if (!_impl) _impl = new Impl();
    return *_impl;
}

void ShaderEditor::OnGraphicsPipelineCreated(ID3D12Device* device, ID3D12PipelineState* pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    if (!pipeline || !device) return;
    auto rec = std::make_unique<PipelineRecord>();
    rec->kind = PipelineRecord::Kind::Graphics;
    rec->device = device;
    rec->graphics = desc;
    rec->rootSignature = desc.pRootSignature;
    OwnBytecode(*rec, rec->graphics.VS);
    OwnBytecode(*rec, rec->graphics.PS);
    OwnBytecode(*rec, rec->graphics.DS);
    OwnBytecode(*rec, rec->graphics.HS);
    OwnBytecode(*rec, rec->graphics.GS);
    OwnStreamOutput(*rec, rec->graphics.StreamOutput);
    OwnInputLayout(*rec, rec->graphics.InputLayout);
    DropCachedPso(*rec, rec->graphics.CachedPSO);
    rec->stageOffsets = {
        {"vertex", offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, VS)},
        {"fragment", offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, PS)},
        {"tess_eval", offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, DS)},
        {"tess_control", offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, HS)},
        {"geometry", offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, GS)},
    };
    impl().AddRecord(pipeline, std::move(rec));
}

void ShaderEditor::OnComputePipelineCreated(ID3D12Device* device, ID3D12PipelineState* pipeline, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) {
    if (!pipeline || !device) return;
    auto rec = std::make_unique<PipelineRecord>();
    rec->kind = PipelineRecord::Kind::Compute;
    rec->device = device;
    rec->compute = desc;
    rec->rootSignature = desc.pRootSignature;
    OwnBytecode(*rec, rec->compute.CS);
    DropCachedPso(*rec, rec->compute.CachedPSO);
    rec->stageOffsets = {{"compute", offsetof(D3D12_COMPUTE_PIPELINE_STATE_DESC, CS)}};
    impl().AddRecord(pipeline, std::move(rec));
}

void ShaderEditor::OnStreamPipelineCreated(ID3D12Device* device, ID3D12PipelineState* pipeline, const D3D12_PIPELINE_STATE_STREAM_DESC& desc) {
    if (!pipeline || !device) return;
    auto rec = std::make_unique<PipelineRecord>();
    rec->kind = PipelineRecord::Kind::Stream;
    rec->device = device;
    CopyStream(*rec, desc);
    impl().AddRecord(pipeline, std::move(rec));
}

void ShaderEditor::OnPipelineReleased(ID3D12PipelineState* pipeline) {
    if (!pipeline || !_impl) return;
    Impl& i = impl();
    std::lock_guard<std::recursive_mutex> lock(i.mutex);
    auto it = i.records.find(pipeline);
    if (it == i.records.end()) return;
    i.Install(*it->second, pipeline, nullptr);
    i.records.erase(it);
}

bool ShaderEditor::Replace(uint64_t pipelineId, const std::string& stage, const std::vector<uint8_t>& bytecode,
                           std::string& error, uint64_t& replacementId, std::string& note) {
    Impl& i = impl();
    replacementId = 0;
    if (!StageKnown(stage)) {
        error = "unknown shader stage '" + stage + "'";
        return false;
    }
    if (!IsShaderContainer(bytecode.data(), bytecode.size())) {
        error = "the payload is not a DXBC/DXIL container";
        return false;
    }
    // The bytecode's own stage must be the one it replaces: the runtime would refuse a pixel
    // shader in the vertex slot with a message the UI cannot see.
    ShaderInfo info = ReflectShader(bytecode.data(), bytecode.size());
    if (!info.stage.empty() && info.stage != stage) {
        error = "the bytecode is a " + info.stage + " shader (" + info.target + "), not " + stage;
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(i.mutex);
    ID3D12PipelineState* pipeline = nullptr;
    PipelineRecord* rec = i.Lookup(pipelineId, pipeline, error);
    if (!rec) return false;
    if (rec->unsupported) {
        error = "the pipeline stream cannot be copied faithfully: " + rec->note;
        return false;
    }
    if (!rec->Stage(stage)) {
        error = "the pipeline has no " + stage + " stage";
        return false;
    }
    // The root signature is the application's object, not held by this library; one released
    // after the pipeline was made (unusual: it is bound alongside the pipeline) cannot be reused.
    if (rec->rootSignature && !Tracker::Get().IdOf(rec->rootSignature)) {
        error = "the pipeline's root signature has been released: the pipeline cannot be rebuilt";
        return false;
    }
    auto previous = rec->edits.find(stage);
    std::vector<uint8_t> previousBytes;
    bool hadPrevious = previous != rec->edits.end();
    if (hadPrevious) previousBytes = std::move(previous->second);
    rec->edits[stage] = bytecode;
    if (!i.Rebuild(*rec, pipeline, error, replacementId)) {
        // The edit that failed leaves what was bound before it in place.
        if (hadPrevious) rec->edits[stage] = std::move(previousBytes);
        else rec->edits.erase(stage);
        return false;
    }
    note = rec->note;
    while (!note.empty() && (note.back() == ' ' || note.back() == ';')) note.pop_back();
    return true;
}

bool ShaderEditor::Restore(uint64_t pipelineId, const std::string& stage, std::string& error) {
    Impl& i = impl();
    std::lock_guard<std::recursive_mutex> lock(i.mutex);
    ID3D12PipelineState* pipeline = nullptr;
    PipelineRecord* rec = i.Lookup(pipelineId, pipeline, error);
    if (!rec) return false;
    if (stage.empty()) {
        rec->edits.clear();
    } else if (!rec->edits.erase(stage)) {
        error = "the " + stage + " stage of pipeline " + std::to_string(pipelineId) + " is not edited";
        return false;
    }
    if (rec->edits.empty()) {
        i.Install(*rec, pipeline, nullptr);
        Log("shader edit: pipeline %llu restored", (unsigned long long)pipelineId);
        return true;
    }
    // Other stages stay edited: the pipeline is made again without this one.
    uint64_t replacementId = 0;
    return i.Rebuild(*rec, pipeline, error, replacementId);
}

ID3D12PipelineState* ShaderEditor::Substitute(ID3D12PipelineState* pipeline) {
    if (!pipeline || !_impl) return pipeline;
    Impl& i = *_impl;
    if (i.editCount.load(std::memory_order_acquire) == 0) return pipeline;
    std::shared_lock lock(i.substitutionMutex);
    auto it = i.substitutions.find(pipeline);
    return it == i.substitutions.end() ? pipeline : it->second;
}

void ShaderEditor::OnPresent() {
    if (!_impl) return;
    Impl& i = *_impl;
    std::vector<ID3D12PipelineState*> due;
    {
        std::lock_guard<std::mutex> lock(i.retiredMutex);
        ++i.presents;
        for (size_t k = 0; k < i.retired.size();) {
            if (i.retired[k].destroyAt <= i.presents) {
                due.push_back(i.retired[k].pipeline);
                i.retired[k] = i.retired.back();
                i.retired.pop_back();
            } else {
                ++k;
            }
        }
    }
    for (ID3D12PipelineState* pipeline : due) {
        // Untracked first: once released the address may be handed out again.
        Tracker::Get().Untrack(pipeline);
        ScopedInternal internal;
        pipeline->Release();
    }
}

}  // namespace dxinsp
