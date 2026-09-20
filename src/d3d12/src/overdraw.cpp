// Overdraw measured while capturing (overdraw.h), and everything the pixel history shares with it
// (pass_record.h): the calls of a render pass kept per command list, the copies of the
// application's pipelines both measurements draw with, and the textures, descriptors and readback
// buffers they make on the application's device.
//
// Every render pass of a capture with `overdraw` is issued a second time into the application's own
// command list, right after the library's read-back of the pass's render targets, into an R16_FLOAT
// target of the pass's size with every pipeline replaced by a copy whose pixel shader returns 1.0
// and whose blending is ONE + ONE: each pixel then holds how many fragments landed on it. Twice per
// pass: once against a copy of the depth-stencil attachment taken before the pass began, with the
// pipelines' own depth-stencil state (the fragments that passed, in draw order), and once with no
// depth-stencil attachment and the tests off (every fragment rasterized).
//
// Unlike a Metal encoder, a D3D12 command list keeps its state across passes, so the measurement
// puts back what it changed: the calls still in effect at the end of the pass are issued again once
// it is drawn. The render targets are not, because a pass only ends where the application is about
// to bind others (OMSetRenderTargets), has left the render-pass region (EndRenderPass), or is
// closing the list.
#include "pass_record.h"

#include "formats.h"
#include "hooks.h"
#include "resources.h"
#include "shader_edit.h"
#include "tracker.h"
#include "transport.h"

#include <d3dcommon.h>
#include <dxcapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace dxinsp {

// Shared with draw_overlay.cpp, which reads the same count targets and takes the same barriers
// (pass_record.h); the rest of this file is its own.

float HalfToFloat(uint16_t h) {
    const int sign = (h >> 15) ? -1 : 1;
    const int exponent = (h >> 10) & 0x1F;
    const int mantissa = h & 0x3FF;
    if (exponent == 0) return sign * std::ldexp((float)mantissa, -24);
    if (exponent == 31) return mantissa ? NAN : sign * INFINITY;
    return sign * std::ldexp((float)(mantissa + 1024), exponent - 25);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t subresource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = subresource;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

namespace {

constexpr uint32_t kHistogramBuckets = 8;
/** Descriptors per heap of the measurement's own; a capture that needs more gets another heap. */
constexpr uint32_t kMeasurementHeapSize = 256;

inline uint64_t Align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// ---------------------------------------------------------------------------------------------
// One measurement drawn into a command list, waiting for it to run

struct PendingMeasurement {
    uint32_t frame = UINT32_MAX;
    ID3D12GraphicsCommandList* list = nullptr;   // not AddRef'd: only a key for the frame it ran in
    uint64_t listId = 0;
    uint32_t passIndex = 0;
    bool depthTested = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t draws = 0;
    uint32_t skipped = 0;
    std::string note;
    /** The measurement was drawn: its counts mean something, if only zeros. */
    bool measured = false;
    /** Nothing was drawn: every count is zero and there is no staging to read. */
    bool empty = true;
    uint32_t rowPitch = 0;
    ComPtr<ID3D12Resource> staging;
    /**
     * What the measurement's draws use and this library made or holds: the count target, the depth
     * copy, the pipeline copies. Kept until the capture's lists have run, since the application
     * may release its pipeline (and with it the copy) at any moment.
     */
    KeepList keep;
};

// Moved into the pending list under the lock: the vector that holds them must never let one go
// while it grows, because releasing one of the application's objects comes back through the
// Release hook into this module.
static_assert(std::is_nothrow_move_constructible_v<PendingMeasurement>);

struct MeasurementState {
    std::mutex mutex;
    std::atomic<bool> active{false};
    bool overdraw = false;
    uint64_t maxDataSize = 256ull << 20;
    /** Per command list: the calls kept, and the pass open in it. Held by pointer, so a rehash does not move them. */
    std::unordered_map<ID3D12GraphicsCommandList*, std::unique_ptr<ListOps>> lists;
    std::unordered_map<ID3D12GraphicsCommandList*, std::shared_ptr<MeasuredPass>> open;
    std::vector<PendingMeasurement> pending;
};

MeasurementState& State() {
    static MeasurementState* s = new MeasurementState();
    return *s;
}

/** What the measurements made on one device: descriptors they hand out, and what they keep. */
struct DeviceMeasurement {
    std::vector<ComPtr<ID3D12DescriptorHeap>> rtvHeaps;
    std::vector<ComPtr<ID3D12DescriptorHeap>> dsvHeaps;
    uint32_t rtvUsed = kMeasurementHeapSize;   // forces the first heap to be made
    uint32_t dsvUsed = kMeasurementHeapSize;
    uint32_t rtvIncrement = 0;
    uint32_t dsvIncrement = 0;
    std::vector<ComPtr<IUnknown>> objects;
};

std::mutex g_deviceMutex;
std::unordered_map<ID3D12Device*, std::unique_ptr<DeviceMeasurement>> g_deviceMeasurements;

DeviceMeasurement* DeviceMeasurementFor(ID3D12Device* device) {
    if (!device) return nullptr;
    auto it = g_deviceMeasurements.find(device);
    if (it != g_deviceMeasurements.end()) return it->second.get();
    auto made = std::make_unique<DeviceMeasurement>();
    {
        ScopedInternal internal;
        made->rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        made->dsvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    DeviceMeasurement* raw = made.get();
    g_deviceMeasurements[device] = std::move(made);
    return raw;
}


// ---------------------------------------------------------------------------------------------
// The pixel shaders the measurements draw with
//
// A pipeline whose other stages are DXIL needs a DXIL pixel shader: D3D12 refuses a pipeline that
// mixes a DXBC stage with a DXIL one. DXBC comes from d3dcompiler_47.dll, which every Windows since
// 8.1 has in System32; DXIL needs dxcompiler.dll, which the system does not have, so the reflection
// code's copy is used when it found one (it is loaded by then: every pipeline's shaders are
// reflected at creation) and otherwise the usual places are tried.

struct MeasurementShader {
    std::once_flag once;
    std::vector<uint8_t> bytes;
    D3D12_SHADER_BYTECODE bytecode{};
    std::string error;
};

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob**, ID3DBlob**);

bool CompileDxbc(const char* source, const char* target, std::vector<uint8_t>& out, std::string& error) {
    static PFN_D3DCompile compile = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
        if (module) compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(module, "D3DCompile"));
    });
    if (!compile) {
        error = "d3dcompiler_47.dll has no D3DCompile";
        return false;
    }
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> messages;
    const HRESULT hr = compile(source, strlen(source), "gpu_inspector", nullptr, nullptr, "main", target, 0, 0, code.put(),
                               messages.put());
    if (FAILED(hr) || !code) {
        error = std::string("the measurement's ") + target + " shader did not compile: " + HrText(hr);
        if (messages && messages->GetBufferPointer()) error += std::string(" ") + (const char*)messages->GetBufferPointer();
        return false;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
    out.assign(bytes, bytes + code->GetBufferSize());
    return true;
}

HMODULE DxcModule() {
    static HMODULE module = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        // shader_reflect.cpp looks in the module's directory, the Vulkan SDK, the Windows SDK and
        // PATH, and has reflected every DXIL shader of the application by now, so its copy is
        // already in the process.
        module = GetModuleHandleW(L"dxcompiler.dll");
        if (!module) module = LoadLibraryW(L"dxcompiler.dll");
        if (!module) {
            wchar_t sdk[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"VULKAN_SDK", sdk, MAX_PATH)) {
                std::wstring path = std::wstring(sdk) + L"\\Bin\\dxcompiler.dll";
                module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
        }
    });
    return module;
}

bool CompileDxil(const char* source, const wchar_t* target, std::vector<uint8_t>& out, std::string& error) {
    HMODULE module = DxcModule();
    DxcCreateInstanceProc create =
        module ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance")) : nullptr;
    if (!create) {
        error = "dxcompiler.dll was not found, so no DXIL pixel shader could be compiled for the pipelines that need one";
        return false;
    }
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put()))) || !compiler) {
        error = "dxcompiler.dll: DxcCreateInstance(CLSID_DxcCompiler) failed";
        return false;
    }
    DxcBuffer buffer{source, strlen(source), DXC_CP_ACP};
    LPCWSTR args[] = {L"-T", target, L"-E", L"main"};
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, args, (UINT32)(sizeof(args) / sizeof(args[0])), nullptr, IID_PPV_ARGS(result.put())))
        || !result) {
        error = "dxcompiler.dll: the measurement's pixel shader did not compile";
        return false;
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    ComPtr<IDxcBlob> object;
    if (FAILED(status) || FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.put()), nullptr)) || !object) {
        error = "dxcompiler.dll: the measurement's pixel shader did not compile (" + HrText(status) + ")";
        return false;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(object->GetBufferPointer());
    out.assign(bytes, bytes + object->GetBufferSize());
    return true;
}

const D3D12_SHADER_BYTECODE* MeasurementPixelShader(MeasurementShader& shader, const char* source, bool dxil,
                                                    std::string& error) {
    std::call_once(shader.once, [&] {
        const bool ok = dxil ? CompileDxil(source, L"ps_6_0", shader.bytes, shader.error)
                             : CompileDxbc(source, "ps_5_0", shader.bytes, shader.error);
        if (ok) shader.bytecode = {shader.bytes.data(), shader.bytes.size()};
        else LogAlways("measurement: %s", shader.error.c_str());
    });
    if (shader.bytes.empty()) {
        error = shader.error;
        return nullptr;
    }
    return &shader.bytecode;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Shared with pixel_history.cpp (pass_record.h)

const D3D12_SHADER_BYTECODE* CountingPixelShader(bool dxil, std::string& error) {
    // One into the count target's only channel; the blend adds it to what is there.
    static const char* kSource = "float main() : SV_Target { return 1.0; }\n";
    static MeasurementShader dxbcShader;
    static MeasurementShader dxilShader;
    return MeasurementPixelShader(dxil ? dxilShader : dxbcShader, kSource, dxil, error);
}

const D3D12_SHADER_BYTECODE* CoverPixelShader(bool dxil, std::string& error) {
    // Writes nothing and discards nothing: what the pixel history counts as coverage.
    static const char* kSource = "void main() { }\n";
    static MeasurementShader dxbcShader;
    static MeasurementShader dxilShader;
    return MeasurementPixelShader(dxil ? dxilShader : dxbcShader, kSource, dxil, error);
}

ComPtr<ID3D12Resource> NewMeasurementTexture(ID3D12Device* device, DXGI_FORMAT format, uint32_t width, uint32_t height,
                                             uint32_t layers, bool depthStencil, D3D12_RESOURCE_STATES state,
                                             const D3D12_CLEAR_VALUE* clear) {
    ComPtr<ID3D12Resource> texture;
    if (!device || !width || !height) return texture;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = (UINT16)std::max<uint32_t>(1, layers);
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = depthStencil ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ScopedInternal internal;
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(texture.put()));
    if (FAILED(hr)) {
        Log("measurement: a %ux%u %s texture failed (%s)", width, height, FormatName(format), HrText(hr).c_str());
        texture.reset();
    }
    return texture;
}

ComPtr<ID3D12Resource> NewMeasurementReadback(ID3D12Device* device, uint64_t size) {
    ComPtr<ID3D12Resource> buffer;
    if (!device || !size) return buffer;
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
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(buffer.put()));
    if (FAILED(hr)) {
        Log("measurement: a readback buffer of %llu bytes failed (%s)", (unsigned long long)size, HrText(hr).c_str());
        buffer.reset();
    }
    return buffer;
}

bool MeasurementDescriptor(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_CPU_DESCRIPTOR_HANDLE& out) {
    out = D3D12_CPU_DESCRIPTOR_HANDLE{};
    const bool rtv = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    DeviceMeasurement* dm = DeviceMeasurementFor(device);
    if (!dm) return false;
    std::vector<ComPtr<ID3D12DescriptorHeap>>& heaps = rtv ? dm->rtvHeaps : dm->dsvHeaps;
    uint32_t& used = rtv ? dm->rtvUsed : dm->dsvUsed;
    const uint32_t increment = rtv ? dm->rtvIncrement : dm->dsvIncrement;
    if (!increment) return false;
    if (used >= kMeasurementHeapSize) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = kMeasurementHeapSize;
        ComPtr<ID3D12DescriptorHeap> heap;
        ScopedInternal internal;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put()))) || !heap) return false;
        heaps.push_back(std::move(heap));
        used = 0;
    }
    ScopedInternal internal;
    // A descriptor is read where the list is recorded, and the list runs later, so a slot is never
    // handed out twice within a capture.
    out = heaps.back()->GetCPUDescriptorHandleForHeapStart();
    out.ptr += (SIZE_T)used * increment;
    used++;
    return true;
}

void KeepMeasurementObject(ID3D12Device* device, IUnknown* object) {
    if (!object) return;
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    DeviceMeasurement* dm = DeviceMeasurementFor(device);
    if (!dm) return;
    object->AddRef();
    dm->objects.emplace_back(ComPtr<IUnknown>(object));
}

ID3D12PipelineState* VariantOf(ID3D12PipelineState* pipeline, uint64_t key, const PipelineVariant& variant, std::string& error) {
    ID3D12PipelineState* out = nullptr;
    if (!ShaderEditor::Get().VariantPipeline(pipeline, key, variant, &out, error)) return nullptr;
    return out;
}

std::vector<const LoggedOp*> EffectiveOps(const std::vector<LoggedOp>& ops, size_t count) {
    auto same = [](const char* a, const char* b) { return a && b && strcmp(a, b) == 0; };
    // A list: dropping an undone call from the middle leaves the others in the order they were made.
    std::list<const LoggedOp*> kept;
    for (size_t i = 0; i < count && i < ops.size(); ++i) {
        const LoggedOp& op = ops[i];
        const OpKey& k = op.key;
        if (k.policy == OpPolicy::Draw || k.policy == OpPolicy::Action) continue;
        auto drop = [&](auto undone) {
            for (auto it = kept.begin(); it != kept.end();) {
                if (undone((*it)->key)) it = kept.erase(it);
                else ++it;
            }
        };
        switch (k.policy) {
            case OpPolicy::Replace:
                drop([&](const OpKey& o) {
                    return (o.policy == OpPolicy::Replace && same(o.name, k.name)) || (k.clears && same(o.name, k.clears));
                });
                break;
            case OpPolicy::Slot:
                // A range that also covers the slot stays: it still binds the others, and it was
                // made earlier, so issuing both in order leaves this slot with the later value.
                drop([&](const OpKey& o) { return o.policy == OpPolicy::Slot && same(o.name, k.name) && o.location == k.location; });
                break;
            case OpPolicy::Range:
                drop([&](const OpKey& o) {
                    if (!same(o.name, k.name)) return false;
                    if (o.policy == OpPolicy::Range) return o.location == k.location && o.length == k.length;
                    return o.policy == OpPolicy::Slot && o.location >= k.location && o.location < k.location + k.length;
                });
                break;
            default:
                break;
        }
        kept.push_back(&op);
    }
    return std::vector<const LoggedOp*>(kept.begin(), kept.end());
}

namespace {

/** Issues the calls exactly as the application made them, for putting the list's state back. */
class PassThroughReplay final : public PassReplay {
public:
    void SetPipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) override {
        // What the application had bound: its own pipeline, or the replacement a shader edit put
        // in its place (shader_edit.h), which is what SetPipelineState forwarded.
        if (pipeline) list->SetPipelineState(ShaderEditor::Get().Substitute(pipeline));
    }
    void IssueDraw(ID3D12GraphicsCommandList*, const std::function<void(ID3D12GraphicsCommandList*)>&) override {}
    void Skip() override {}
};

}  // namespace

void ReissueState(ID3D12GraphicsCommandList* list, const std::vector<const LoggedOp*>& ops) {
    PassThroughReplay replay;
    for (const LoggedOp* op : ops) op->op(list, replay);
}

std::string RecordedMethod(CommandRecorder* rec, uint32_t command) {
    if (!rec) return std::string();
    std::shared_ptr<const CommandList> commands = rec->Snapshot();
    if (!commands || command >= commands->size()) return std::string();
    return (*commands)[command].method;
}

// ---------------------------------------------------------------------------------------------
// The overdraw measurement

namespace {

/** One of a pass's two overdraw counts, as the kept calls see it. */
class CountReplay final : public PassReplay {
public:
    CountReplay(const MeasuredPass& pass, DXGI_FORMAT depthFormat, D3D12_CPU_DESCRIPTOR_HANDLE dsv, bool dsvBound,
                PendingMeasurement& out)
        : _pass(pass), _depthFormat(depthFormat), _dsv(dsv), _dsvBound(dsvBound), _out(out) {}

    void SetPipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) override {
        _bound = false;
        if (!pipeline) return;
        std::string error;
        const bool dxil = ShaderEditor::Get().PipelineIsDxil(pipeline);
        const D3D12_SHADER_BYTECODE* shader = CountingPixelShader(dxil, error);
        if (!shader) {
            Note("some draws were not counted: " + error);
            return;
        }
        PipelineVariant v;
        v.pixelShader = shader->pShaderBytecode;
        v.pixelShaderSize = shader->BytecodeLength;
        v.countFormat = DXGI_FORMAT_R16_FLOAT;
        v.setDepthFormat = true;
        v.depthFormat = _depthFormat;
        // Without an attachment to test against, the copy tests nothing; with one it keeps the
        // pipeline's own state, writes included, so the count is what passed in draw order.
        v.disableDepth = _depthFormat == DXGI_FORMAT_UNKNOWN;
        v.disableStencil = _depthFormat == DXGI_FORMAT_UNKNOWN;
        v.singleSample = true;
        ID3D12PipelineState* variant = VariantOf(pipeline, VariantKey(VariantKind::Count, _depthFormat), v, error);
        if (!variant) {
            Note("some draws were not counted: " + error);
            return;
        }
        list->SetPipelineState(variant);
        if (_kept.insert(variant).second) KeepObject(_out.keep, variant);
        _bound = true;
    }

    void ClearDepthStencil(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CLEAR_FLAGS flags,
                           FLOAT depth, UINT8 stencil, UINT numRects, const D3D12_RECT* rects) override {
        // The application clearing the pass's depth-stencil clears the measurement's copy of it.
        if (_dsvBound && handle.ptr == _pass.depth.handle.ptr) list->ClearDepthStencilView(_dsv, flags, depth, stencil, numRects, rects);
    }

    void IssueDraw(ID3D12GraphicsCommandList* list, const std::function<void(ID3D12GraphicsCommandList*)>& draw) override {
        if (!_bound) {
            _out.skipped++;
            return;
        }
        _out.draws++;
        draw(list);
    }

    void Skip() override { _out.skipped++; }

private:
    /** The first reason a draw was not counted, beside whatever the pass already said. */
    void Note(const std::string& text) {
        if (_noted) return;
        _noted = true;
        _out.note += (_out.note.empty() ? "" : "; ") + text;
    }

    const MeasuredPass& _pass;
    DXGI_FORMAT _depthFormat;
    D3D12_CPU_DESCRIPTOR_HANDLE _dsv;
    bool _dsvBound;
    PendingMeasurement& _out;
    bool _bound = false;
    bool _noted = false;
    std::unordered_set<ID3D12PipelineState*> _kept;
};

/** Draws a pass's two overdraw counts into its command list, inside the application's EndPass. */
void MeasureOverdraw(MeasuredPass& pass, const ListOps& ops) {
    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = pass.list;
    ID3D12Device* device = pass.device;
    const std::vector<const LoggedOp*> before = EffectiveOps(ops.ops, ops.passFirst);
    std::vector<PendingMeasurement> results;
    for (int mode = 0; mode < 2; ++mode) {
        PendingMeasurement m;
        m.list = list;
        m.listId = pass.listId;
        m.passIndex = pass.passIndex;
        m.depthTested = mode == 0;
        m.width = pass.width;
        m.height = pass.height;
        if (!pass.note.empty()) {
            m.note = pass.note;
            results.push_back(std::move(m));
            continue;
        }
        bool tests = m.depthTested;
        if (tests && !pass.hasDepth) {
            tests = false;
            m.note = "the pass has no depth-stencil attachment";
        } else if (tests && pass.samples > 1) {
            tests = false;
            m.note = "a multisampled depth-stencil attachment is not copied: counted without the tests";
        } else if (tests && pass.layered) {
            tests = false;
            m.note = "a layered pass's depth-stencil attachment is not copied: counted without the tests";
        } else if (tests && !pass.depthStart) {
            tests = false;
            m.note = "the depth-stencil attachment could not be copied: counted without the tests";
        }
        if (pass.samples > 1) {
            m.note += std::string(m.note.empty() ? "" : "; ") + "a multisampled pass is counted with one sample";
        }

        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R16_FLOAT;
        ComPtr<ID3D12Resource> target =
            NewMeasurementTexture(device, DXGI_FORMAT_R16_FLOAT, pass.width, pass.height, 1, false,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        if (!target || !MeasurementDescriptor(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtv)) {
            m.note = "no memory for the count target";
            results.push_back(std::move(m));
            continue;
        }
        {
            D3D12_RENDER_TARGET_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_R16_FLOAT;
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(target.get(), &d, rtv);
        }
        KeepObject(m.keep, target.get());

        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        bool dsvBound = false;
        if (tests && MeasurementDescriptor(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsv)) {
            D3D12_DEPTH_STENCIL_VIEW_DESC d{};
            d.Format = pass.depth.format;
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(pass.depthStart.get(), &d, dsv);
            dsvBound = true;
            KeepObject(m.keep, pass.depthStart.get());
        }
        const DXGI_FORMAT depthFormat = dsvBound ? pass.depth.format : DXGI_FORMAT_UNKNOWN;

        list->OMSetRenderTargets(1, &rtv, FALSE, dsvBound ? &dsv : nullptr);
        const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        list->ClearRenderTargetView(rtv, zero, 0, nullptr);
        // A real render pass that clears its depth-stencil at the start: the copy is cleared the
        // same way, since the clear is part of BeginRenderPass rather than a command of its own.
        if (dsvBound) {
            D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;
            if (pass.depth.beginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) flags |= D3D12_CLEAR_FLAG_DEPTH;
            if (pass.depth.stencilBeginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) flags |= D3D12_CLEAR_FLAG_STENCIL;
            if (flags) {
                list->ClearDepthStencilView(dsv, flags, pass.depth.clearValue.DepthStencil.Depth,
                                            pass.depth.clearValue.DepthStencil.Stencil, 0, nullptr);
            }
        }

        CountReplay replay(pass, depthFormat, dsv, dsvBound, m);
        for (const LoggedOp* op : before) op->op(list, replay);
        for (size_t k = ops.passFirst; k < ops.ops.size(); ++k) ops.ops[k].op(list, replay);
        m.measured = true;

        // The counts, read back once the list has run. Row pitches in a texture copy are 256-aligned.
        list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        const uint32_t rowPitch = (uint32_t)Align((uint64_t)pass.width * 2, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        ComPtr<ID3D12Resource> staging = NewMeasurementReadback(device, (uint64_t)rowPitch * pass.height);
        if (!staging) {
            m.note += std::string(m.note.empty() ? "" : "; ") + "no staging memory for the counts";
            results.push_back(std::move(m));
            continue;
        }
        Transition(list, target.get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = staging.get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16_FLOAT;
        dst.PlacedFootprint.Footprint.Width = pass.width;
        dst.PlacedFootprint.Footprint.Height = pass.height;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        m.rowPitch = rowPitch;
        m.staging = std::move(staging);
        m.empty = false;
        results.push_back(std::move(m));
    }

    MeasurementState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pending.reserve(s.pending.size() + results.size());
    for (PendingMeasurement& m : results) s.pending.push_back(std::move(m));
}

/** The depth-stencil the counted run tests against: a copy taken before the pass can change it. */
void CopyDepthStart(MeasuredPass& pass) {
    if (!pass.hasDepth || pass.samples > 1 || pass.layered || !pass.depth.resource) return;
    // A real render pass that clears its depth-stencil starts the copy from the clear value
    // instead; one that discards it starts from whatever the copy holds, which is what the
    // application's draws would meet.
    const bool preserve = pass.depth.beginAccess == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
    D3D12_RESOURCE_DESC desc{};
    ResourceInfo info;
    if (!ResourceTracker::Get().Get(pass.depth.resource, info)) return;
    desc = info.desc;
    uint32_t planes = 1;
    const uint32_t subresources = SubresourceCount(pass.device, desc, &planes);
    (void)subresources;
    const uint32_t mips = std::max<uint32_t>(1, desc.MipLevels);
    const uint32_t slices = std::max<uint32_t>(1, desc.DepthOrArraySize);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = TypedFormat(desc.Format, true);
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;
    ComPtr<ID3D12Resource> copy = NewMeasurementTexture(pass.device, desc.Format, pass.width, pass.height, 1, true,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, &clear);
    if (!copy) return;
    ScopedInternal internal;
    ID3D12GraphicsCommandList* list = pass.list;
    for (uint32_t plane = 0; preserve && plane < planes; ++plane) {
        const uint32_t source = pass.depth.mip + pass.depth.slice * mips + plane * mips * slices;
        bool known = false;
        D3D12_RESOURCE_STATES state = ResourceTracker::Get().StateIn(list, pass.depth.resource, source, &known);
        if (!known) state = pass.depth.readOnlyDepth ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE;
        Transition(list, pass.depth.resource, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = copy.get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = plane;   // the copy has one mip and one slice, so the plane is the index
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = pass.depth.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = source;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list, pass.depth.resource, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        Transition(list, copy.get(), plane, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    if (!preserve) {
        for (uint32_t plane = 0; plane < planes; ++plane) {
            Transition(list, copy.get(), plane, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }
    // The copy is now referenced by the application's list whether or not the measurement is
    // drawn (a pass that turns out not to be measurable still recorded it), so it is kept on the
    // device until the capture's lists have run rather than with the measurement.
    KeepMeasurementObject(pass.device, copy.get());
    pass.depthStart = std::move(copy);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Recording

bool PassRecordingActive() {
    return State().active.load(std::memory_order_relaxed);
}

void LogPassOp(CommandRecorder* rec, OpKey key, PassOp op) {
    if (!rec || !PassRecordingActive()) return;
    MeasurementState& s = State();
    ListOps* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed)) return;
        std::unique_ptr<ListOps>& slot = s.lists[rec->list()];
        if (!slot) slot = std::make_unique<ListOps>();
        entry = slot.get();
    }
    // The list is externally synchronized by the application, so only this thread records into it.
    const uint32_t command = rec->commandCount() ? (uint32_t)rec->commandCount() - 1 : 0;
    entry->ops.push_back(LoggedOp{std::move(op), key, command});
}

void OnMeasuredListReset(ID3D12GraphicsCommandList* list) {
    if (!PassRecordingActive()) return;
    MeasurementState& s = State();
    std::unique_ptr<ListOps> dropped;
    std::shared_ptr<MeasuredPass> pass;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.lists.find(list);
        if (it != s.lists.end()) {
            dropped = std::move(it->second);
            s.lists.erase(it);
        }
        auto open = s.open.find(list);
        if (open != s.open.end()) {
            pass = std::move(open->second);
            s.open.erase(open);
        }
    }
    // The closures let go of the application's objects here, outside the lock.
}

void OnMeasuredListClearState(ID3D12GraphicsCommandList* list) {
    OnMeasuredListReset(list);
}

void OnMeasuredBundle(ID3D12GraphicsCommandList* list, ID3D12GraphicsCommandList* bundle) {
    if (!PassRecordingActive() || !list) return;
    MeasurementState& s = State();
    ListOps* slot = nullptr;
    std::vector<LoggedOp> inlined;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        std::unique_ptr<ListOps>& entry = s.lists[list];
        if (!entry) entry = std::make_unique<ListOps>();
        slot = entry.get();
        auto it = bundle ? s.lists.find(bundle) : s.lists.end();
        if (it == s.lists.end() || it->second->ops.empty()) {
            // A bundle recorded before the capture has no kept calls, so the pass that executes it
            // cannot be issued again.
            slot->unknownBundle = true;
            return;
        }
        // Copied under the lock (a copy only takes references), appended outside it: growing the
        // list's own vector lets go of the references the calls it moves out were holding, and a
        // release of one of the application's objects comes back through the Release hook into
        // this module, which must not find the lock held.
        inlined = it->second->ops;
    }
    // A bundle sets state on the list that executes it and draws with it, so its calls are the
    // list's from here on. Only the thread recording the list touches its kept calls.
    for (LoggedOp& op : inlined) slot->ops.push_back(std::move(op));
}

void OnMeasuredListReleased(ID3D12GraphicsCommandList* list) {
    // Calls are only kept while a measured capture records, and everything kept is dropped when it
    // finishes, so there is nothing of a released list to forget outside that.
    if (!PassRecordingActive()) return;
    MeasurementState& s = State();
    std::unique_ptr<ListOps> dropped;
    std::shared_ptr<MeasuredPass> pass;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.lists.find(list);
        if (it != s.lists.end()) {
            dropped = std::move(it->second);
            s.lists.erase(it);
        }
        auto open = s.open.find(list);
        if (open != s.open.end()) {
            pass = std::move(open->second);
            s.open.erase(open);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Passes

void PrepareMeasuredPass(CommandRecorder* rec, const std::vector<BoundTarget>& targets) {
    if (!rec || !PassRecordingActive()) return;
    MeasurementState& s = State();
    bool overdraw = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed)) return;
        overdraw = s.overdraw;
        s.open.erase(rec->list());
    }
    const bool overlay = DrawOverlayRequested() || MeshOutputRequested();
    if (rec->bundle()) return;   // a bundle records no pass of its own

    auto pass = std::make_shared<MeasuredPass>();
    pass->device = rec->device();
    pass->list = rec->list();
    pass->listId = Tracker::Get().IdOf(rec->list());
    pass->measureOverdraw = overdraw;
    pass->measureOverlay = overlay;
    for (const BoundTarget& t : targets) {
        if (!t.resource) continue;
        PassAttachment a;
        a.resource = t.resource;
        a.format = TypedFormat(t.format, t.depth);
        a.mip = t.mip;
        a.slice = t.firstSlice;
        a.handle = t.handle;
        a.readOnlyDepth = t.readOnlyDepth;
        a.beginAccess = t.beginAccess;
        a.stencilBeginAccess = t.stencilBeginAccess;
        a.clearValue = t.clearValue;
        D3D12_RESOURCE_DESC desc{};
        ResourceInfo info;
        if (!ResourceTracker::Get().Get(t.resource, info)) continue;
        desc = info.desc;
        if (!pass->width) {
            pass->width = std::max<uint32_t>(1, (uint32_t)(desc.Width >> t.mip));
            pass->height = std::max<uint32_t>(1, desc.Height >> t.mip);
            pass->samples = std::max<uint32_t>(1, desc.SampleDesc.Count);
        }
        if (desc.SampleDesc.Count > 1) pass->samples = desc.SampleDesc.Count;
        if (t.sliceCount > 1) pass->layered = true;
        if (t.depth) {
            pass->hasDepth = true;
            pass->depth = a;
        } else {
            pass->colors.push_back(a);
        }
    }
    if (!pass->width || !pass->height) {
        pass->note = "the pass has no render target this library can measure";
    } else {
        const int attachment = MatchPixelHistoryAttachment(*pass);
        if (attachment >= 0) PreparePixelHistory(*pass, attachment);
        // The overlay's depth-test run tests against the same copy the overdraw count does.
        if ((pass->measureOverdraw || pass->measureOverlay) && pass->note.empty()) CopyDepthStart(*pass);
    }
    if (!pass->measureOverdraw && !pass->history && !pass->measureOverlay) return;   // nothing the capture measures renders here
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.active.load(std::memory_order_relaxed)) s.open[rec->list()] = std::move(pass);
}

void BeginMeasuredPass(CommandRecorder* rec) {
    if (!rec || !PassRecordingActive()) return;
    MeasurementState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.open.find(rec->list());
    if (it == s.open.end()) return;
    it->second->passIndex = rec->pass().passIndex;
    it->second->beginCommand = rec->pass().beginCommand;
    std::unique_ptr<ListOps>& slot = s.lists[rec->list()];
    if (!slot) slot = std::make_unique<ListOps>();
    slot->passFirst = slot->ops.size();
    slot->unknownBundle = false;
}

void EndMeasuredPass(CommandRecorder* rec, bool insideRenderPass) {
    if (!rec || !PassRecordingActive()) return;
    MeasurementState& s = State();
    std::shared_ptr<MeasuredPass> pass;
    ListOps* ops = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = s.open.find(rec->list());
        if (it == s.open.end()) return;
        pass = std::move(it->second);
        s.open.erase(it);
        auto entry = s.lists.find(rec->list());
        if (entry != s.lists.end()) ops = entry->second.get();
    }
    ListOps none;
    ListOps& kept = ops ? *ops : none;
    if (insideRenderPass && pass->note.empty()) {
        // A list inside a BeginRenderPass region refuses OMSetRenderTargets and the copies the
        // measurements need, and the runtime gives it a vtable that says so. That only happens
        // where the application closed the list without EndRenderPass.
        pass->note = "the render pass was still open when the command list closed, so it could not be issued again";
    }
    if (kept.unknownBundle && pass->note.empty()) {
        pass->note = "the pass executes a bundle whose calls were not kept, so it cannot be issued again"
                     " (a bundle is only kept when it is recorded while the capture records; engines record theirs at start-up)";
    }
    const bool measurable = pass->note.empty();
    if (pass->measureOverdraw) MeasureOverdraw(*pass, kept);
    if (pass->measureOverlay && MatchDrawOverlayPass(*pass)) MeasureDrawOverlay(*pass, rec, kept);
    if (pass->measureOverlay && MatchMeshOutputPass(*pass)) MeasureMeshOutput(*pass, rec, kept);
    if (pass->history) FollowPixel(*pass, rec, kept);
    if (measurable) {
        // The measurement bound its own pipelines, render targets and scissors; the application's
        // state goes back, since a D3D12 command list keeps it across passes.
        ScopedInternal internal;
        ReissueState(rec->list(), EffectiveOps(kept.ops, kept.ops.size()));
    }
    // The pass's draws are done with; the state calls stay for the passes that follow.
    if (ops) {
        std::vector<LoggedOp> remaining;
        remaining.reserve(ops->ops.size());
        for (LoggedOp& op : ops->ops) {
            if (op.key.policy != OpPolicy::Draw && op.key.policy != OpPolicy::Action) remaining.push_back(std::move(op));
        }
        ops->ops.swap(remaining);
        ops->passFirst = ops->ops.size();
        ops->unknownBundle = false;
    }
}

// ---------------------------------------------------------------------------------------------
// The capture

void StartMeasurements(bool overdraw, const PixelHistoryRequest& history, const DrawOverlayRequest& overlay,
                       const MeshOutputRequest& mesh, uint64_t maxDataSize) {
    MeasurementState& s = State();
    std::vector<PendingMeasurement> pending;
    std::unordered_map<ID3D12GraphicsCommandList*, std::unique_ptr<ListOps>> lists;
    std::unordered_map<ID3D12GraphicsCommandList*, std::shared_ptr<MeasuredPass>> open;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        pending.swap(s.pending);
        lists.swap(s.lists);
        open.swap(s.open);
        s.overdraw = overdraw;
        s.maxDataSize = maxDataSize;
        s.active.store(overdraw || history.enabled || overlay.enabled || mesh.enabled, std::memory_order_relaxed);
    }
    StartPixelHistory(history);
    StartDrawOverlay(overlay);
    StartMeshOutput(mesh);
    if (overdraw) Log("overdraw: measuring every render pass of the capture");
}

void AssignMeasurementFrame(ID3D12GraphicsCommandList* list, uint32_t frame) {
    // Measurements only exist while a measured capture records; a capture that asked for none does
    // no work here at all.
    if (!PassRecordingActive()) return;
    MeasurementState& s = State();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        for (PendingMeasurement& m : s.pending)
            if (m.list == list && m.frame == UINT32_MAX) m.frame = frame;
    }
    AssignPixelHistoryFrame(list, frame);
    AssignDrawOverlayFrame(list, frame);
    AssignMeshOutputFrame(list, frame);
}

void OnMeasurementDeviceReleased(ID3D12Device* device) {
    std::unique_ptr<DeviceMeasurement> dropped;
    {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        auto it = g_deviceMeasurements.find(device);
        if (it == g_deviceMeasurements.end()) return;
        dropped = std::move(it->second);
        g_deviceMeasurements.erase(it);
    }
}

void SendOverdraw() {
    MeasurementState& s = State();
    std::vector<PendingMeasurement> pending;
    std::unordered_map<ID3D12GraphicsCommandList*, std::unique_ptr<ListOps>> lists;
    std::unordered_map<ID3D12GraphicsCommandList*, std::shared_ptr<MeasuredPass>> open;
    bool overdraw = false;
    uint64_t maxDataSize = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        overdraw = s.overdraw;
        maxDataSize = s.maxDataSize;
        s.overdraw = false;
        s.active.store(false, std::memory_order_relaxed);
        pending.swap(s.pending);
        lists.swap(s.lists);
        open.swap(s.open);
    }
    // The textures, descriptor heaps and pipeline copies the measurements used: the capture's
    // lists have run, so nothing references them any more. Released whatever was measured.
    struct Release {
        std::vector<std::unique_ptr<DeviceMeasurement>> devices;
        ~Release() {
            std::lock_guard<std::mutex> lock(g_deviceMutex);
            for (auto& [device, dm] : g_deviceMeasurements) devices.push_back(std::move(dm));
            g_deviceMeasurements.clear();
        }
    } release;
    if (!overdraw) return;

    struct Result {
        uint64_t fragments = 0;
        uint64_t covered = 0;
        uint32_t maxCount = 0;
        uint64_t histogram[kHistogramBuckets] = {};
        std::vector<uint8_t> counts;   // u16 per pixel, little endian, row by row
    };
    std::vector<Result> results(pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
        PendingMeasurement& m = pending[i];
        Result& r = results[i];
        const size_t pixels = (size_t)m.width * m.height;
        if (m.frame == UINT32_MAX) {
            m.frame = 0;
            if (m.measured) {
                m.measured = false;
                m.note = "the command list was not executed during the capture";
            }
        }
        if (!m.measured) continue;
        if (m.empty || !m.staging) {
            r.counts.assign(pixels * 2, 0);   // a pass with nothing to draw
            continue;
        }
        const uint8_t* mapped = nullptr;
        {
            ScopedInternal internal;
            void* p = nullptr;
            if (SUCCEEDED(m.staging->Map(0, nullptr, &p))) mapped = static_cast<const uint8_t*>(p);
        }
        if (!mapped) {
            m.measured = false;
            m.note = "the count target could not be read";
            continue;
        }
        r.counts.resize(pixels * 2);
        for (uint32_t y = 0; y < m.height; ++y) {
            const uint8_t* row = mapped + (size_t)y * m.rowPitch;
            for (uint32_t x = 0; x < m.width; ++x) {
                const float value = HalfToFloat((uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8)));
                const uint32_t n = std::isfinite(value) && value > 0 ? (uint32_t)std::min(65535L, std::lround(value)) : 0;
                const size_t p = (size_t)y * m.width + x;
                r.counts[p * 2] = (uint8_t)(n & 0xFF);
                r.counts[p * 2 + 1] = (uint8_t)(n >> 8);
                if (!n) continue;
                r.fragments += n;
                r.covered++;
                r.maxCount = std::max(r.maxCount, n);
                const int bucket = n <= 4 ? (int)n - 1 : n <= 8 ? 4 : n <= 16 ? 5 : n <= 32 ? 6 : 7;
                r.histogram[bucket]++;
            }
        }
        ScopedInternal internal;
        m.staging->Unmap(0, nullptr);
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        if (results[i].counts.size() > maxDataSize) {
            results[i].counts.clear();
            pending[i].note += std::string(pending[i].note.empty() ? "" : "; ")
                               + "per-pixel counts not sent: larger than the capture's texture size limit";
        }
    }

    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureOverdraw");
    w.Key("count"); w.Uint(pending.size());
    w.Key("passes"); w.BeginArray();
    for (size_t i = 0; i < pending.size(); ++i) {
        const PendingMeasurement& m = pending[i];
        const Result& r = results[i];
        w.BeginObject();
        w.Key("frame"); w.Uint(m.frame);
        w.Key("commandBuffer"); w.Uint(m.listId);
        w.Key("passIndex"); w.Uint(m.passIndex);
        w.Key("depthTested"); w.Boolean(m.depthTested);
        w.Key("measured"); w.Boolean(m.measured);
        w.Key("width"); w.Uint(m.width);
        w.Key("height"); w.Uint(m.height);
        w.Key("fragments"); w.Uint(r.fragments);
        w.Key("coveredPixels"); w.Uint(r.covered);
        w.Key("maxCount"); w.Uint(r.maxCount);
        w.Key("draws"); w.Uint(m.draws);
        w.Key("skippedDraws"); w.Uint(m.skipped);
        w.Key("histogram"); w.BeginArray();
        for (uint64_t h : r.histogram) w.Uint(h);
        w.EndArray();
        w.Key("size"); w.Uint(r.counts.size());
        if (!m.note.empty()) { w.Key("note"); w.String(m.note); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));

    for (size_t i = 0; i < pending.size(); ++i) {
        if (results[i].counts.empty()) continue;
        const PendingMeasurement& m = pending[i];
        JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureOverdrawData");
        h.Key("frame"); h.Uint(m.frame);
        h.Key("commandBuffer"); h.Uint(m.listId);
        h.Key("passIndex"); h.Uint(m.passIndex);
        h.Key("depthTested"); h.Boolean(m.depthTested);
        h.Key("size"); h.Uint(results[i].counts.size());
        h.EndObject();
        Transport::Get().SendBinary(std::move(h.str()), results[i].counts.data(), results[i].counts.size());
    }
    Log("overdraw: %zu measurement(s) sent%s", pending.size(),
        open.empty() ? "" : " (passes still open at the end of the capture were not measured)");
}

}  // namespace dxinsp
