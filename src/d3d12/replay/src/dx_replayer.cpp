#include "dx_replayer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

#include <cstdlib>
#include <fstream>
#include <set>

#include "decode.h"

#include "dx_exporter.h"
#include "dx_source.h"

namespace dxreplay
{

namespace
{
const std::string* g_currentStep = nullptr;
}

const char* CurrentStep() { return g_currentStep ? g_currentStep->c_str() : ""; }

using vkreplay::JValue;

namespace
{
/** What a copy of a depth-stencil format's depth plane holds: 32 bits of D32_FLOAT_S8X24's 64, which is how the capture stores it. */
dxinsp::FormatInfo DepthPlaneFormat(DXGI_FORMAT format)
{
    const DXGI_FORMAT plane = dxinsp::DepthCopyFormat(format);
    return dxinsp::FormatOf(plane == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT : plane);
}
} // namespace

namespace
{

std::string Str(const JValue* v) { return v && v->IsString() ? std::string(v->Str()) : std::string(); }

std::string Narrow(const wchar_t* wide)
{
    std::string out;
    for (; wide && *wide; ++wide)
        out += *wide < 0x80 ? (char)*wide : '?';
    return out;
}

std::string HrText(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)hr);
    return buf;
}

template <typename T>
void SafeRelease(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

uint32_t MipDim(uint64_t size, uint32_t mip) { return (uint32_t)std::max<uint64_t>(1, size >> mip); }

const char* StateName(D3D12_RESOURCE_STATES) { return "D3D12_RESOURCE_STATES"; }

std::string StateText(D3D12_RESOURCE_STATES s)
{
    if (s == D3D12_RESOURCE_STATE_COMMON)
        return "D3D12_RESOURCE_STATE_COMMON";
    return Source::Flags(dxinsp::kEnum_D3D12_RESOURCE_STATES, std::size(dxinsp::kEnum_D3D12_RESOURCE_STATES), (uint64_t)s, StateName(s));
}

std::string FormatText(DXGI_FORMAT f)
{
    if (f == DXGI_FORMAT_UNKNOWN)
        return "DXGI_FORMAT_UNKNOWN";
    return Source::Enum(dxinsp::kEnum_DXGI_FORMAT, std::size(dxinsp::kEnum_DXGI_FORMAT), f, "DXGI_FORMAT");
}

bool IsWriteState(D3D12_RESOURCE_STATES s)
{
    return (s & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_STREAM_OUT)) != 0;
}

} // namespace

DxReplayer::DxReplayer()
{
    _env.object = [this](uint64_t id) { return Object(id); };
    _env.address = [this](uint64_t bufferId, uint64_t offset) -> D3D12_GPU_VIRTUAL_ADDRESS {
        Resource* r = ResourceOf(bufferId);
        return r && r->IsBuffer() ? r->resource->GetGPUVirtualAddress() + offset : 0;
    };
    _env.cpuHandle = [this](uint64_t heapId, uint32_t index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{};
        auto it = _heaps.find(heapId);
        if (it != _heaps.end() && index < it->second.desc.NumDescriptors)
            h.ptr = it->second.cpu.ptr + (SIZE_T)index * it->second.increment;
        return h;
    };
    _env.bytecode = [this](const char* stage, const uint8_t*& data, size_t& size) {
        // An ablation's copy of a pipeline takes one stage's code from the request (dx_measure.cpp).
        if (_overrideCode && _overrideStage == stage)
        {
            data = _overrideCode->data();
            size = _overrideCode->size();
            return size > 0;
        }
        // A stage replaced for the whole replay (--replace): every pipeline made from this object,
        // an ablation's baseline included, runs the edited code.
        const uint64_t id = _currentObject && _currentObject->Get("id") ? _currentObject->Get("id")->Uint() : 0;
        for (const DxShaderReplacement& r : _options.replacements)
        {
            if (r.pipeline != id || r.stage != stage || r.code.empty())
                continue;
            data = r.code.data();
            size = r.code.size();
            return true;
        }
        // The blob is named "<stage>:<entry point>", and the description does not say the entry point.
        const JValue* blobs = _currentObject ? _currentObject->Get("blobs") : nullptr;
        const std::string prefix = std::string(stage) + ":";
        for (uint32_t i = 0; blobs && blobs->IsArray() && i < blobs->count; ++i)
        {
            const JValue& b = blobs->items[i];
            if (Str(b.Get("name")).rfind(prefix, 0) == 0)
                return _capture->Payload(b.Get("payload"), data, size);
        }
        return false;
    };
}

DxReplayer::~DxReplayer()
{
    if (_queue && _fence)
        WaitForQueue(_queue);
    for (ID3D12Resource* r : _transients)
        r->Release();
    for (auto it = _created.rbegin(); it != _created.rend(); ++it)
        (*it)->Release();
    SafeRelease(_utility);
    SafeRelease(_allocator);
    SafeRelease(_fence);
    SafeRelease(_queue);
    SafeRelease(_infoQueue);
    SafeRelease(_device);
    SafeRelease(_adapter);
    SafeRelease(_factory);
    if (_fenceEvent)
        CloseHandle(_fenceEvent);
}

void DxReplayer::Problem(const std::string& message) { _report->problems.push_back(message); }

IUnknown* DxReplayer::Object(uint64_t id) const
{
    auto it = _objects.find(id);
    return it == _objects.end() ? nullptr : it->second;
}

DxReplayer::Resource* DxReplayer::ResourceOf(uint64_t id)
{
    auto it = _resources.find(id);
    return it == _resources.end() ? nullptr : &it->second;
}

uint64_t DxReplayer::IdOfResource(ID3D12Resource* resource) const
{
    auto it = _resourceIds.find(resource);
    return it == _resourceIds.end() ? 0 : it->second;
}

std::string DxReplayer::ListName(ID3D12GraphicsCommandList* list) const { return _x ? _x->NameOf(list) : std::string(); }

void DxReplayer::CollectMessages()
{
    if (!_infoQueue)
        return;
    const UINT64 count = _infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;
        _infoQueue->GetMessage(i, nullptr, &size);
        std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (FAILED(_infoQueue->GetMessage(i, message, &size)))
            continue;
        if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING)
            continue;
        _report->messages.push_back(std::string(message->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? "warning: " : "error: ") + message->pDescription);
    }
    _infoQueue->ClearStoredMessages();
}

// ---------------------------------------------------------------------------------------------
// The device

bool DxReplayer::CreateDevice()
{
    // The captured adapter's name and the feature level the application asked for.
    std::string capturedAdapter;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (const JValue* objects = _capture->Objects(); objects && objects->IsArray())
    {
        for (uint32_t i = 0; i < objects->count; ++i)
        {
            const JValue& o = objects->items[i];
            const JValue* args = o.Get("args");
            if (Str(o.Get("type")) == "IDXGIAdapter" && args && capturedAdapter.empty())
            {
                if (const JValue* desc = args->Get("Desc"))
                    capturedAdapter = Str(desc->Get("Description"));
            }
            if (Str(o.Get("type")) == "ID3D12Device" && args)
                level = (D3D_FEATURE_LEVEL)ParseEnum(args->Get("MinimumFeatureLevel"), dxinsp::kEnum_D3D_FEATURE_LEVEL, std::size(dxinsp::kEnum_D3D_FEATURE_LEVEL));
        }
    }
    if (level < D3D_FEATURE_LEVEL_11_0)
        level = D3D_FEATURE_LEVEL_11_0;

    if (_options.debugLayer)
    {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            debug->Release();
        }
        else
        {
            Problem("the D3D12 debug layer is not installed (Windows' Graphics Tools)");
        }
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&_factory))))
    {
        Problem("CreateDXGIFactory2 failed");
        return false;
    }
    // The adapter the capture was taken on, by name; else the first hardware adapter.
    IDXGIAdapter1* first = nullptr;
    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (_factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!capturedAdapter.empty() && Narrow(desc.Description) == capturedAdapter)
        {
            SafeRelease(first);
            _adapter = adapter;
            break;
        }
        if (!first && !software)
            first = adapter;
        else
            adapter->Release();
    }
    if (!_adapter)
        _adapter = first;
    DXGI_ADAPTER_DESC1 desc{};
    if (_adapter)
        _adapter->GetDesc1(&desc);
    _report->device = Narrow(desc.Description);
    if (!capturedAdapter.empty() && capturedAdapter != _report->device)
        Problem("captured on " + capturedAdapter + ", replayed on " + _report->device);

    HRESULT hr = D3D12CreateDevice(_adapter, level, IID_PPV_ARGS(&_device));
    if (FAILED(hr))
    {
        Problem("D3D12CreateDevice failed (" + HrText(hr) + ")");
        return false;
    }
    if (_options.debugLayer)
        _device->QueryInterface(IID_PPV_ARGS(&_infoQueue));

    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(_device->CreateCommandQueue(&queue, IID_PPV_ARGS(&_queue))) ||
        FAILED(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_allocator))) ||
        FAILED(_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocator, nullptr, IID_PPV_ARGS(&_utility))) ||
        FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
    {
        Problem("the replay's own queue and command list could not be created");
        return false;
    }
    _utility->Close();
    _fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (_x)
    {
        _x->Alias(_device, "device");
        _x->Device(capturedAdapter, _report->device,
            Source::Enum(dxinsp::kEnum_D3D_FEATURE_LEVEL, std::size(dxinsp::kEnum_D3D_FEATURE_LEVEL), level, "D3D_FEATURE_LEVEL"));
    }
    return true;
}

void DxReplayer::WaitForQueue(ID3D12CommandQueue* queue)
{
    const uint64_t value = ++_fenceValue;
    if (SUCCEEDED(queue->Signal(_fence, value)) && _fence->GetCompletedValue() < value)
    {
        _fence->SetEventOnCompletion(value, _fenceEvent);
        WaitForSingleObject(_fenceEvent, 60 * 1000);
    }
    // A frame the driver cannot run takes the device with it, and every call after that fails or
    // worse. It is said once, with the reason, and the replay stops issuing work (DeviceLost).
    const HRESULT reason = _device->GetDeviceRemovedReason();
    if (FAILED(reason) && !_deviceLost)
    {
        _deviceLost = true;
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08X", (unsigned)reason);
        Problem(std::string("the device was removed (") + code + ") after " + (_env.where.empty() ? "the last submission" : _env.where) +
            ": the frame cannot be run to its end on this GPU, so what follows was not replayed and no target after it is compared");
    }
}

bool DxReplayer::RunOneTime(const std::function<void(ID3D12GraphicsCommandList*)>& record)
{
    if (FAILED(_allocator->Reset()) || FAILED(_utility->Reset(_allocator, nullptr)))
        return false;
    record(_utility);
    if (FAILED(_utility->Close()))
        return false;
    ID3D12CommandList* lists[] = {_utility};
    _queue->ExecuteCommandLists(1, lists);
    WaitForQueue(_queue);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Objects

void DxReplayer::CreateObjects()
{
    const JValue* objects = _capture->Objects();
    if (!objects || !objects->IsArray())
        return;
    std::vector<const JValue*> ordered;
    for (uint32_t i = 0; i < objects->count; ++i)
        ordered.push_back(&objects->items[i]);
    std::sort(ordered.begin(), ordered.end(), [](const JValue* a, const JValue* b) { return a->Get("id")->Uint() < b->Get("id")->Uint(); });
    for (const JValue* o : ordered)
        CreateObject(*o);
    // The application's names, so what the debug layer says about an object names the one the
    // capture shows rather than "Unnamed ID3D12Resource Object".
    for (const JValue* o : ordered)
    {
        const std::string label = Str(o->Get("label"));
        IUnknown* made = label.empty() ? nullptr : Object(o->Get("id")->Uint());
        ID3D12Object* named = nullptr;
        if (!made || FAILED(made->QueryInterface(IID_PPV_ARGS(&named))))
            continue;
        const int wide = MultiByteToWideChar(CP_UTF8, 0, label.c_str(), (int)label.size(), nullptr, 0);
        std::wstring name((size_t)wide, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, label.c_str(), (int)label.size(), name.data(), wide);
        named->SetName(name.c_str());
        named->Release();
    }
}

ID3D12Resource* DxReplayer::CreateResource(uint64_t id, const D3D12_HEAP_PROPERTIES& capturedHeap, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC& capturedDesc,
    const D3D12_CLEAR_VALUE* clear, const std::string& comment)
{
    // Memory of its own in a heap of the captured kind, whatever heap it was placed in: a capture then
    // replays on a GPU with another heap tier. Only the heap's type matters to what the frame does.
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = capturedHeap.Type == D3D12_HEAP_TYPE_UPLOAD || capturedHeap.Type == D3D12_HEAP_TYPE_READBACK ? capturedHeap.Type : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = capturedDesc;
    desc.Alignment = 0;
    // A buffer in an upload heap is born readable and one in a read-back heap a copy's destination;
    // everything else starts in COMMON and is moved to where the frame expects it (MoveToInitialStates).
    const D3D12_RESOURCE_STATES state = heap.Type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
        : heap.Type == D3D12_HEAP_TYPE_READBACK                             ? D3D12_RESOURCE_STATE_COPY_DEST
                                      // A buffer a build wrote an acceleration structure into: that state
                                      // cannot be reached by a barrier, only by creation (dx_raytracing.cpp).
        : _structureBuffers.count(id) ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                      : D3D12_RESOURCE_STATE_COMMON;
    // A clear value is only legal on a render target or a depth stencil.
    const bool clearable = (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0 &&
        desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
    const D3D12_CLEAR_VALUE* usedClear = clearable ? clear : nullptr;
    ID3D12Resource* resource = nullptr;
    const HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, usedClear, IID_PPV_ARGS(&resource));
    if (FAILED(hr) || !resource)
    {
        Problem("resource " + std::to_string(id) + ": CreateCommittedResource failed (" + HrText(hr) + ")");
        return nullptr;
    }
    Resource rec;
    rec.resource = resource;
    rec.desc = desc;
    rec.heapType = heap.Type;
    const bool buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    const dxinsp::FormatInfo format = dxinsp::FormatOf(desc.Format);
    rec.mips = buffer ? 1 : std::max<uint32_t>(1, desc.MipLevels);
    rec.slices = buffer || desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : std::max<uint32_t>(1, desc.DepthOrArraySize);
    rec.planes = format.depth && format.stencil ? 2 : 1;
    rec.states.assign((size_t)rec.mips * rec.slices * rec.planes, state);
    _resources[id] = rec;
    _resourceIds[resource] = id;
    if (_x)
    {
        const std::string name = _x->Declare("ID3D12Resource", buffer ? "buffer" : "texture", id, resource);
        if (buffer)
            _x->Buffer(resource, resource->GetGPUVirtualAddress(), desc.Width);
        _x->Block(DxExporter::Create, "ID3D12Resource " + std::to_string(id) + (comment.empty() ? "" : ": " + comment), [&](Source& s) {
            const std::string h = EmitStruct(s, "heap", heap);
            const std::string d = EmitStruct(s, "desc", desc);
            const std::string c = usedClear ? "&" + EmitStruct(s, "clearValue", *usedClear) : std::string("nullptr");
            s.Line("DX_CHECK(device->CreateCommittedResource(&" + h + ", D3D12_HEAP_FLAG_NONE, &" + d + ", " + StateText(state) + ", " + c +
                ", IID_PPV_ARGS(&" + name + ")));");
        });
        _x->CountObject();
    }
    return resource;
}

ID3D12RootSignature* DxReplayer::CreateRootSignature(uint64_t id, const JValue& args)
{
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    if (!DecodeStruct(_env, args, "pDesc", desc))
    {
        Problem("root signature " + std::to_string(id) + ": the capture has no description of it");
        return nullptr;
    }
    if (desc.Version != D3D_ROOT_SIGNATURE_VERSION_1_0 && desc.Version != D3D_ROOT_SIGNATURE_VERSION_1_1)
    {
        Problem("root signature " + std::to_string(id) + ": version 1.2 root signatures are not replayed yet");
        return nullptr;
    }
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &errors);
    if (FAILED(hr) || !blob)
    {
        Problem("root signature " + std::to_string(id) + ": it could not be serialized" +
            (errors ? std::string(": ") + static_cast<const char*>(errors->GetBufferPointer()) : std::string()));
        SafeRelease(errors);
        return nullptr;
    }
    SafeRelease(errors);
    ID3D12RootSignature* root = nullptr;
    hr = _device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    blob->Release();
    if (FAILED(hr) || !root)
    {
        Problem("root signature " + std::to_string(id) + ": CreateRootSignature failed (" + HrText(hr) + ")");
        return nullptr;
    }
    _rootSignatures[id] = args.Get("pDesc");
    if (_x)
    {
        const std::string name = _x->Declare("ID3D12RootSignature", "rootSignature", id, root);
        _x->Block(DxExporter::Create, "ID3D12RootSignature " + std::to_string(id) + ", serialized again from its description", [&](Source& s) {
            s.Line(name + " = CreateRootSignatureFrom(&" + EmitStruct(s, "desc", desc) + ");");
        });
        _x->CountObject();
    }
    return root;
}

ID3D12PipelineState* DxReplayer::CreatePipeline(const JValue& object, const std::string& cmd, const JValue& args)
{
    const uint64_t id = object.Get("id")->Uint();
    const JValue* json = args.Get("pDesc");
    if (!json || json->IsNull())
    {
        Problem("pipeline " + std::to_string(id) + ": the capture has no description of it");
        return nullptr;
    }
    // A pipeline made from a stream is written under the members of the two descriptions, so it is
    // made from one of them again, which covers every subobject they can express.
    const bool stream = cmd == "CreatePipelineState" || cmd == "LoadPipeline";
    if (stream)
    {
        for (const char* key : {"AS", "MS", "ViewInstancing"})
            if (const JValue* v = json->Get(key); v && !v->IsNull())
            {
                Problem("pipeline " + std::to_string(id) + ": its " + key + " subobject is not replayed yet");
                return nullptr;
            }
    }
    const bool compute = cmd == "CreateComputePipelineState" || cmd == "LoadComputePipeline" || (stream && json->Get("CS") && !json->Get("CS")->IsNull());
    _currentObject = &object;
    const size_t unresolved = _env.unresolved;
    ID3D12PipelineState* pipeline = nullptr;
    HRESULT hr = E_FAIL;
    std::string name;
    if (compute)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        DecodeStruct(_env, args, "pDesc", desc);
        if (_env.unresolved == unresolved)
        {
            hr = _device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
            if (SUCCEEDED(hr) && _x)
            {
                name = _x->Declare("ID3D12PipelineState", "pipelineState", id, pipeline);
                _x->Block(DxExporter::Create, "ID3D12PipelineState " + std::to_string(id) + " (" + cmd + ")", [&](Source& s) {
                    s.Line("DX_CHECK(device->CreateComputePipelineState(&" + EmitStruct(s, "desc", desc) + ", IID_PPV_ARGS(&" + name + ")));");
                });
            }
        }
    }
    else
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        DecodeStruct(_env, args, "pDesc", desc);
        if (desc.SampleDesc.Count == 0)
            desc.SampleDesc.Count = 1;   // a stream without SAMPLE_DESC means one sample
        if (stream && !json->Get("SampleMask"))
            desc.SampleMask = UINT_MAX;
        if (_env.unresolved == unresolved)
        {
            hr = _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
            if (SUCCEEDED(hr) && _x)
            {
                name = _x->Declare("ID3D12PipelineState", "pipelineState", id, pipeline);
                _x->Block(DxExporter::Create, "ID3D12PipelineState " + std::to_string(id) + " (" + cmd + ")", [&](Source& s) {
                    s.Line("DX_CHECK(device->CreateGraphicsPipelineState(&" + EmitStruct(s, "desc", desc) + ", IID_PPV_ARGS(&" + name + ")));");
                });
            }
        }
    }
    _currentObject = nullptr;
    if (_env.unresolved != unresolved)
        return nullptr;
    if (FAILED(hr) || !pipeline)
    {
        Problem("pipeline " + std::to_string(id) + ": " + (compute ? "CreateComputePipelineState" : "CreateGraphicsPipelineState") + " failed (" + HrText(hr) + ")");
        return nullptr;
    }
    if (_x)
        _x->CountObject();
    return pipeline;
}

void DxReplayer::CreateObject(const JValue& o)
{
    const uint64_t id = o.Get("id") ? o.Get("id")->Uint() : 0;
    const std::string type = Str(o.Get("type"));
    const std::string cmd = Str(o.Get("cmd"));
    const JValue* args = o.Get("args");
    _env.where = type + " " + std::to_string(id);
    if (_options.trace)
    {
        std::fprintf(stderr, "object %llu %s (%s)\n", (unsigned long long)id, type.c_str(), cmd.c_str());
        std::fflush(stderr);
    }
    const size_t unresolved = _env.unresolved;
    auto resolved = [&] { return _env.unresolved == unresolved; };
    auto skip = [&](const char* why) {
        _skipped.insert(id);
        _report->objectsSkipped++;
        if (_x)
            _x->Comment(DxExporter::Create, type + " " + std::to_string(id) + ": left out on purpose: " + why);
    };
    IUnknown* made = nullptr;
    bool owned = true;
    static const JValue kNoArgs;
    const JValue& a = args && !args->IsNull() ? *args : kNoArgs;
    Decoder d(&a, _env);

    if (type == "ID3D12Device")
    {
        made = _device;
        owned = false;
    }
    else if (type == "IDXGIAdapter" || type == "IDXGIFactory" || type == "IDXGIOutput" || type == "IDXGISwapChain" || type == "ID3D12Fence" ||
        type == "ID3D12PipelineLibrary")
    {
        return skip("the frame does not need it to run again");
    }
    else if (type == "ID3D12Heap")
    {
        return skip("a resource placed in it gets memory of its own");
    }
    else if (type == "ID3D12RaytracingAccelerationStructure")
    {
        // Not a D3D12 object at all: the capture library mints one per address a build wrote to, so
        // the UI has something to hang a build on (src/d3d12/src/raytracing.h). What the replay
        // needs of it is the address, which PrepareRaytracing has already read.
        return skip("an acceleration structure is a range in a buffer, which the replay makes with that buffer");
    }
    else if (type == "ID3D12CommandQueue")
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        d.Struct("pDesc", desc);
        desc.NodeMask = 0;
        ID3D12CommandQueue* queue = nullptr;
        if (SUCCEEDED(_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
            made = queue;
        if (made && _x)
        {
            const std::string name = _x->Declare("ID3D12CommandQueue", "commandQueue", id, made);
            _x->Block(DxExporter::Create, "ID3D12CommandQueue " + std::to_string(id), [&](Source& s) {
                s.Line("DX_CHECK(device->CreateCommandQueue(&" + EmitStruct(s, "desc", desc) + ", IID_PPV_ARGS(&" + name + ")));");
            });
        }
    }
    else if (type == "ID3D12CommandAllocator")
    {
        D3D12_COMMAND_LIST_TYPE listType = D3D12_COMMAND_LIST_TYPE_DIRECT;
        d.Enum("type", listType, DX_TABLE(D3D12_COMMAND_LIST_TYPE));
        ID3D12CommandAllocator* allocator = nullptr;
        if (SUCCEEDED(_device->CreateCommandAllocator(listType, IID_PPV_ARGS(&allocator))))
            made = allocator;
        if (allocator)
            _allocators.push_back(allocator);
        if (made && _x)
        {
            const std::string name = _x->Declare("ID3D12CommandAllocator", "commandAllocator", id, made);
            _x->Block(DxExporter::Create, "", [&](Source& s) {
                s.Line("DX_CHECK(device->CreateCommandAllocator(" + Source::Enum(DX_TABLE(D3D12_COMMAND_LIST_TYPE), listType) + ", IID_PPV_ARGS(&" + name + ")));");
            });
        }
    }
    else if (type == "ID3D12GraphicsCommandList")
    {
        D3D12_COMMAND_LIST_TYPE listType = D3D12_COMMAND_LIST_TYPE_DIRECT;
        d.Enum("type", listType, DX_TABLE(D3D12_COMMAND_LIST_TYPE));
        // An allocator of its own: the one it was created with may be gone, and every list is reset
        // with the allocator its recording names before it records anything.
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(_device->CreateCommandAllocator(listType, IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(_device->CreateCommandList(0, listType, allocator, nullptr, IID_PPV_ARGS(&list))))
        {
            list->Close();
            made = list;
            _created.push_back(allocator);
            if (listType == D3D12_COMMAND_LIST_TYPE_BUNDLE)
                _bundles[id] = {IdOf(d.Get("pCommandAllocator")), IdOf(d.Get("pInitialState")), false};
        }
        else
        {
            SafeRelease(allocator);
        }
        if (made && _x)
        {
            const std::string name = _x->Declare("ID3D12GraphicsCommandList", "commandList", id, made);
            _x->Block(DxExporter::Create, "", [&](Source& s) {
                s.Line(name + " = CreateClosedCommandList(" + Source::Enum(DX_TABLE(D3D12_COMMAND_LIST_TYPE), listType) + ");   // ID3D12GraphicsCommandList " + std::to_string(id));
            });
        }
    }
    else if (type == "ID3D12DescriptorHeap")
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        d.Struct("pDesc", desc);
        desc.NodeMask = 0;
        ID3D12DescriptorHeap* heap = nullptr;
        if (SUCCEEDED(_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
        {
            made = heap;
            Heap rec;
            rec.heap = heap;
            rec.desc = desc;
            rec.cpu = heap->GetCPUDescriptorHandleForHeapStart();
            if (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                rec.gpu = heap->GetGPUDescriptorHandleForHeapStart();
            rec.increment = _device->GetDescriptorHandleIncrementSize(desc.Type);
            _heaps[id] = rec;
            if (_x)
            {
                const std::string name = _x->Declare("ID3D12DescriptorHeap", "descriptorHeap", id, made);
                _x->Heap(heap, rec.cpu, rec.gpu, rec.increment, desc.NumDescriptors);
                _x->Block(DxExporter::Create, "ID3D12DescriptorHeap " + std::to_string(id), [&](Source& s) {
                    s.Line("DX_CHECK(device->CreateDescriptorHeap(&" + EmitStruct(s, "desc", desc) + ", IID_PPV_ARGS(&" + name + ")));");
                });
            }
        }
    }
    else if (type == "ID3D12Resource")
    {
        D3D12_RESOURCE_DESC desc{};
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};
        const bool hasDesc = DecodeStruct(_env, a, "pDesc", desc);
        const bool hasClear = DecodeStruct(_env, a, "pOptimizedClearValue", clear);
        std::string comment = cmd;
        if (cmd == "GetBuffer")
        {
            _swapBuffers.insert(id);
            comment = "a swap chain's buffer, as an ordinary render target of its format and size";
        }
        else if (cmd == "OpenSharedHandle")
        {
            comment = "a resource another device or process shared, as one of the replay's own of its description";
        }
        else if (cmd == "CreatePlacedResource")
        {
            // The heap it was placed in says what kind of memory it had.
            const JValue* heapObject = _capture->Object(IdOf(a.Get("pHeap")));
            const JValue* heapArgs = heapObject ? heapObject->Get("args") : nullptr;
            const JValue* heapDesc = heapArgs ? heapArgs->Get("pDesc") : nullptr;
            if (heapDesc)
                DecodeStruct(_env, *heapDesc, "Properties", heap);
            comment = "placed in heap " + std::to_string(IdOf(a.Get("pHeap"))) + " by the application; memory of its own here";
        }
        else if (cmd == "CreateReservedResource" || cmd == "CreateReservedResource1" || cmd == "CreateReservedResource2")
        {
            comment = "a reserved (tiled) resource in the application; fully committed here";
        }
        else
        {
            DecodeStruct(_env, a, "pHeapProperties", heap);
        }
        if (hasDesc)
            made = CreateResource(id, heap, D3D12_HEAP_FLAG_NONE, desc, hasClear ? &clear : nullptr, comment);
        if (!made)
        {
            if (!hasDesc)
                Problem("resource " + std::to_string(id) + " (" + cmd + "): the capture has no description of it");
            _report->objectsSkipped++;
            return;
        }
        _objects[id] = made;
        _created.push_back(made);
        _report->objectsCreated++;
        return;
    }
    else if (type == "ID3D12RootSignature")
    {
        made = CreateRootSignature(id, a);
        if (made)
        {
            _objects[id] = made;
            _created.push_back(made);
            _report->objectsCreated++;
        }
        else
        {
            _report->objectsSkipped++;
        }
        return;
    }
    else if (type == "ID3D12PipelineState")
    {
        made = CreatePipeline(o, cmd, a);
        if (made)
        {
            _objects[id] = made;
            _created.push_back(made);
            _report->objectsCreated++;
        }
        else
        {
            if (!resolved())
                Problem("pipeline " + std::to_string(id) + " was not created: it names objects the replay does not have");
            _report->objectsSkipped++;
        }
        return;
    }
    else if (type == "ID3D12StateObject")
    {
        made = CreateStateObject(id, o, a);
        if (made)
        {
            _objects[id] = made;
            _created.push_back(made);
            _report->objectsCreated++;
        }
        else
        {
            _report->objectsSkipped++;
        }
        return;
    }
    else if (type == "ID3D12CommandSignature")
    {
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        d.Struct("pDesc", desc);
        desc.NodeMask = 0;
        ID3D12RootSignature* root = nullptr;
        if (d.Get("pRootSignature"))
            d.Object("pRootSignature", root, "ID3D12RootSignature");
        ID3D12CommandSignature* signature = nullptr;
        if (resolved() && SUCCEEDED(_device->CreateCommandSignature(&desc, root, IID_PPV_ARGS(&signature))))
            made = signature;
        if (made && _x)
        {
            const std::string name = _x->Declare("ID3D12CommandSignature", "commandSignature", id, made);
            _x->Block(DxExporter::Create, "ID3D12CommandSignature " + std::to_string(id), [&](Source& s) {
                s.Line("DX_CHECK(device->CreateCommandSignature(&" + EmitStruct(s, "desc", desc) + ", " + s.Object(root) + ", IID_PPV_ARGS(&" + name + ")));");
            });
        }
    }
    else if (type == "ID3D12QueryHeap")
    {
        D3D12_QUERY_HEAP_DESC desc{};
        d.Struct("pDesc", desc);
        desc.NodeMask = 0;
        ID3D12QueryHeap* heap = nullptr;
        if (SUCCEEDED(_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap))))
            made = heap;
        if (made && _x)
        {
            const std::string name = _x->Declare("ID3D12QueryHeap", "queryHeap", id, made);
            _x->Block(DxExporter::Create, "ID3D12QueryHeap " + std::to_string(id), [&](Source& s) {
                s.Line("DX_CHECK(device->CreateQueryHeap(&" + EmitStruct(s, "desc", desc) + ", IID_PPV_ARGS(&" + name + ")));");
            });
        }
    }
    else
    {
        Problem(type + " " + std::to_string(id) + " (" + cmd + ") is not replayed yet");
        _report->objectsSkipped++;
        if (_x)
            _x->Comment(DxExporter::Create, type + " " + std::to_string(id) + ": left out: the replay does not make objects of this type yet");
        return;
    }

    if (!made)
    {
        Problem(type + " " + std::to_string(id) + " (" + cmd + ") " + (resolved() ? "could not be created" : "was not created: it names objects the replay does not have"));
        _report->objectsSkipped++;
        if (_x)
            _x->Comment(DxExporter::Create, type + " " + std::to_string(id) + ": left out: the replay could not create it");
        return;
    }
    _objects[id] = made;
    if (owned)
    {
        _created.push_back(made);
        if (_x)
            _x->CountObject();
    }
    _report->objectsCreated++;
}

// ---------------------------------------------------------------------------------------------
// The states the frame expects. A capture records no resource states: they are set by barriers in
// frames long gone. What the frame's own commands say is enough, though. A subresource with a
// transition in the frame starts in that transition's StateBefore, which the application's earlier
// uses of it were legal in; one with none starts in the states its uses need, which is any state
// they are legal in, since nothing in the frame says otherwise.

void DxReplayer::NoteUse(uint64_t resourceId, uint32_t subresource, D3D12_RESOURCE_STATES state, bool writes)
{
    Resource* r = ResourceOf(resourceId);
    if (!r || r->heapType != D3D12_HEAP_TYPE_DEFAULT)
        return;
    // An acceleration structure's buffer is born in its one state and may never leave it.
    if (_structureBuffers.count(resourceId))
        return;
    auto& initial = _initial[resourceId];
    initial.resize(r->states.size());
    for (uint32_t i = 0; i < initial.size(); ++i)
    {
        if (subresource != UINT_MAX && i != subresource)
            continue;
        InitialState& s = initial[i];
        if (s.fixed)
            continue;
        if (writes)
        {
            // A write state stands alone; reads seen before it were in another state the frame
            // never named, which cannot be known, so the write wins only when nothing was read.
            if (!s.any)
                s.state = state;
            s.fixed = true;
        }
        else
        {
            s.state |= state;
        }
        s.any = true;
    }
}

void DxReplayer::ComputeInitialStates()
{
    const JValue* commands = _capture->Commands();
    if (!commands || !commands->IsArray())
        return;
    auto flags = [](const JValue* v) { return (D3D12_RESOURCE_STATES)ParseFlags(v, dxinsp::kEnum_D3D12_RESOURCE_STATES, std::size(dxinsp::kEnum_D3D12_RESOURCE_STATES)); };
    const D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    auto address = [&](const JValue* v, D3D12_RESOURCE_STATES state, bool writes) {
        if (v && !v->IsNull())
            NoteUse(IdOf(v->Get("buffer")), UINT_MAX, state, writes);
    };
    auto target = [&](const JValue* entry, bool depth) {
        if (!entry || entry->IsNull())
            return;
        D3D12_RESOURCE_STATES state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (depth)
        {
            const JValue* view = entry->Get("view");
            const std::string viewFlags = view ? Str(view->Get("Flags")) : std::string();
            if (viewFlags.find("READ_ONLY_DEPTH") != std::string::npos)
                state = D3D12_RESOURCE_STATE_DEPTH_READ;
        }
        NoteUse(IdOf(entry->Get("resource")), UINT_MAX, state, state != D3D12_RESOURCE_STATE_DEPTH_READ);
    };
    for (uint32_t i = 0; i < commands->count; ++i)
    {
        const JValue& c = commands->items[i];
        const std::string m = Str(c.Get("method"));
        const JValue* args = c.Get("args");
        if (const JValue* snapshot = c.Get("descriptors"))
        {
            const JValue* sets = snapshot->Get("sets");
            for (uint32_t s = 0; sets && sets->IsArray() && s < sets->count; ++s)
            {
                const JValue* bindings = sets->items[s].Get("bindings");
                for (uint32_t b = 0; bindings && bindings->IsArray() && b < bindings->count; ++b)
                {
                    const std::string type = Str(bindings->items[b].Get("type"));
                    const JValue* list = bindings->items[b].Get("descriptors");
                    const bool uav = type.find("_UAV") != std::string::npos;
                    const bool cbv = type.find("_CBV") != std::string::npos;
                    for (uint32_t k = 0; list && list->IsArray() && k < list->count; ++k)
                    {
                        const JValue& record = list->items[k];
                        const uint64_t id = IdOf(record.Get("buffer")) ? IdOf(record.Get("buffer")) : IdOf(record.Get("resource"));
                        if (!id)
                            continue;
                        NoteUse(id, UINT_MAX, uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : cbv ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                                                                                                : kShaderRead,
                            uav);
                    }
                }
            }
        }
        if (!args || args->IsNull())
            continue;
        if (m == "ResourceBarrier")
        {
            const JValue* barriers = args->Get("pBarriers");
            for (uint32_t b = 0; barriers && barriers->IsArray() && b < barriers->count; ++b)
            {
                const JValue* t = barriers->items[b].Get("Transition");
                if (!t)
                    continue;
                // An END_ONLY half completes a transition begun in another list: only a whole one or its beginning says where a subresource was.
                if (Str(barriers->items[b].Get("Flags")).find("END_ONLY") != std::string::npos)
                    continue;
                const uint64_t id = IdOf(t->Get("pResource"));
                Resource* r = ResourceOf(id);
                if (!r || r->heapType != D3D12_HEAP_TYPE_DEFAULT)
                    continue;
                const uint32_t sub = t->Get("Subresource") ? (uint32_t)t->Get("Subresource")->Uint() : UINT_MAX;
                auto& initial = _initial[id];
                initial.resize(r->states.size());
                for (uint32_t k = 0; k < initial.size(); ++k)
                {
                    if ((sub != UINT_MAX && k != sub) || initial[k].fixed)
                        continue;
                    initial[k].state = flags(t->Get("StateBefore"));
                    initial[k].fixed = initial[k].any = true;
                }
            }
        }
        else if (m == "IASetVertexBuffers")
        {
            const JValue* views = args->Get("pViews");
            for (uint32_t k = 0; views && views->IsArray() && k < views->count; ++k)
                address(views->items[k].Get("BufferLocation"), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
        }
        else if (m == "IASetIndexBuffer")
        {
            if (const JValue* view = args->Get("pView"); view && !view->IsNull())
                address(view->Get("BufferLocation"), D3D12_RESOURCE_STATE_INDEX_BUFFER, false);
        }
        else if (m.find("RootConstantBufferView") != std::string::npos)
        {
            address(args->Get("BufferLocation"), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
        }
        else if (m.find("RootShaderResourceView") != std::string::npos)
        {
            address(args->Get("BufferLocation"), kShaderRead, false);
        }
        else if (m.find("RootUnorderedAccessView") != std::string::npos)
        {
            address(args->Get("BufferLocation"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        }
        else if (m == "OMSetRenderTargets")
        {
            const JValue* list = args->Get("pRenderTargetDescriptors");
            for (uint32_t k = 0; list && list->IsArray() && k < list->count; ++k)
                target(&list->items[k], false);
            target(args->Get("pDepthStencilDescriptor"), true);
        }
        else if (m == "BeginRenderPass")
        {
            const JValue* list = args->Get("pRenderTargets");
            for (uint32_t k = 0; list && list->IsArray() && k < list->count; ++k)
                target(&list->items[k], false);
            target(args->Get("pDepthStencil"), true);
        }
        else if (m == "ClearRenderTargetView")
        {
            NoteUse(IdOf(args->Get("resource")), UINT_MAX, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
        }
        else if (m == "ClearDepthStencilView")
        {
            NoteUse(IdOf(args->Get("resource")), UINT_MAX, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
        }
        else if (m == "ClearUnorderedAccessViewUint" || m == "ClearUnorderedAccessViewFloat")
        {
            NoteUse(IdOf(args->Get("pResource")), UINT_MAX, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        }
        else if (m == "CopyBufferRegion")
        {
            NoteUse(IdOf(args->Get("pSrcBuffer")), UINT_MAX, D3D12_RESOURCE_STATE_COPY_SOURCE, false);
            NoteUse(IdOf(args->Get("pDstBuffer")), UINT_MAX, D3D12_RESOURCE_STATE_COPY_DEST, true);
        }
        else if (m == "CopyResource")
        {
            NoteUse(IdOf(args->Get("pSrcResource")), UINT_MAX, D3D12_RESOURCE_STATE_COPY_SOURCE, false);
            NoteUse(IdOf(args->Get("pDstResource")), UINT_MAX, D3D12_RESOURCE_STATE_COPY_DEST, true);
        }
        else if (m == "CopyTextureRegion")
        {
            auto location = [&](const JValue* l, D3D12_RESOURCE_STATES state, bool writes) {
                if (!l || l->IsNull())
                    return;
                const JValue* sub = l->Get("SubresourceIndex");
                NoteUse(IdOf(l->Get("pResource")), sub ? (uint32_t)sub->Uint() : UINT_MAX, state, writes);
            };
            location(args->Get("pSrc"), D3D12_RESOURCE_STATE_COPY_SOURCE, false);
            location(args->Get("pDst"), D3D12_RESOURCE_STATE_COPY_DEST, true);
        }
        else if (m == "ResolveSubresource" || m == "ResolveSubresourceRegion")
        {
            NoteUse(IdOf(args->Get("pSrcResource")), args->Get("SrcSubresource") ? (uint32_t)args->Get("SrcSubresource")->Uint() : UINT_MAX, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, false);
            NoteUse(IdOf(args->Get("pDstResource")), args->Get("DstSubresource") ? (uint32_t)args->Get("DstSubresource")->Uint() : UINT_MAX, D3D12_RESOURCE_STATE_RESOLVE_DEST, true);
        }
        else if (m == "ExecuteIndirect")
        {
            NoteUse(IdOf(args->Get("pArgumentBuffer")), UINT_MAX, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, false);
            NoteUse(IdOf(args->Get("pCountBuffer")), UINT_MAX, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, false);
        }
        else if (m == "ResolveQueryData")
        {
            NoteUse(IdOf(args->Get("pDestinationBuffer")), UINT_MAX, D3D12_RESOURCE_STATE_COPY_DEST, true);
        }
        else if (m == "RSSetShadingRateImage")
        {
            NoteUse(IdOf(args->Get("shadingRateImage")), UINT_MAX, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, false);
        }
        else if (m == "SOSetTargets")
        {
            const JValue* views = args->Get("pViews");
            for (uint32_t k = 0; views && views->IsArray() && k < views->count; ++k)
            {
                address(views->items[k].Get("BufferLocation"), D3D12_RESOURCE_STATE_STREAM_OUT, true);
                address(views->items[k].Get("BufferFilledSizeLocation"), D3D12_RESOURCE_STATE_STREAM_OUT, true);
            }
        }
        else if (m == "SetPredication")
        {
            NoteUse(IdOf(args->Get("pBuffer")), UINT_MAX, D3D12_RESOURCE_STATE_PREDICATION, false);
        }
        else if (m == "BuildRaytracingAccelerationStructure")
        {
            // A structure's buffer lives in RAYTRACING_ACCELERATION_STRUCTURE and never leaves it,
            // and nothing in a frame transitions it: the state is set once, when it is created.
            // Without this the build's destination is still in COMMON and the runtime rejects it.
            const JValue* desc = args->Get("pDesc");
            if (desc && !desc->IsNull())
            {
                address(desc->Get("DestAccelerationStructureData"), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
                address(desc->Get("SourceAccelerationStructureData"), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, false);
                address(desc->Get("ScratchAccelerationStructureData"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
                const JValue* inputs = desc->Get("Inputs");
                const JValue* geometries = inputs ? inputs->Get("pGeometryDescs") : nullptr;
                for (uint32_t k = 0; geometries && geometries->IsArray() && k < geometries->count; ++k)
                {
                    const JValue* tri = geometries->items[k].Get("Triangles");
                    if (!tri || tri->IsNull())
                        continue;
                    const JValue* vertices = tri->Get("VertexBuffer");
                    address(vertices ? vertices->Get("StartAddress") : nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false);
                    address(tri->Get("IndexBuffer"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false);
                    address(tri->Get("Transform3x4"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false);
                }
            }
        }
        else if (m == "CopyRaytracingAccelerationStructure")
        {
            address(args->Get("DestAccelerationStructureData"), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
            address(args->Get("SourceAccelerationStructureData"), D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, false);
        }
        else if (m == "EmitRaytracingAccelerationStructurePostbuildInfo")
        {
            const JValue* desc = args->Get("pDesc");
            if (desc && !desc->IsNull())
                address(desc->Get("DestBuffer"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        }
    }
}

D3D12_RESOURCE_STATES DxReplayer::StateOf(const Resource& r, uint32_t subresource) const
{
    return r.states.empty() ? D3D12_RESOURCE_STATE_COMMON : r.states[subresource < r.states.size() ? subresource : 0];
}

void DxReplayer::Transition(ID3D12GraphicsCommandList* list, Resource& r, uint32_t subresource, D3D12_RESOURCE_STATES to, const char* listName)
{
    // An acceleration structure's buffer stays in the state it was created in; a barrier out of it
    // is rejected outright, and nothing in a frame asks for one.
    if (_structureBuffers.count(IdOfResource(r.resource)))
        return;
    // One barrier for the whole resource when every subresource is in one state, else one each.
    const bool whole = subresource == UINT_MAX && std::all_of(r.states.begin(), r.states.end(), [&](D3D12_RESOURCE_STATES s) { return s == r.states[0]; });
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (uint32_t i = 0; i < r.states.size(); ++i)
    {
        if (subresource != UINT_MAX && i != subresource)
            continue;
        if (r.states[i] == to)
            continue;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r.resource;
        b.Transition.Subresource = whole ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : i;
        b.Transition.StateBefore = r.states[i];
        b.Transition.StateAfter = to;
        barriers.push_back(b);
        if (whole)
            break;
    }
    for (uint32_t i = 0; i < r.states.size(); ++i)
        if (subresource == UINT_MAX || i == subresource)
            r.states[i] = to;
    if (barriers.empty())
        return;
    list->ResourceBarrier((UINT)barriers.size(), barriers.data());
    if (_x && listName)
    {
        for (const D3D12_RESOURCE_BARRIER& b : barriers)
            _x->Block(_restoring ? DxExporter::Restore : DxExporter::Contents, "", [&](Source& s) {
                s.Line(std::string("Transition(") + listName + ", " + s.Object(b.Transition.pResource) + ", " + Source::Uint(b.Transition.Subresource) + ", " +
                    StateText(b.Transition.StateBefore) + ", " + StateText(b.Transition.StateAfter) + ");");
            });
    }
}

void DxReplayer::NoteBarrier(const D3D12_RESOURCE_BARRIER& barrier)
{
    if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || (barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY))
        return;
    Resource* r = ResourceOf(IdOfResource(barrier.Transition.pResource));
    if (!r)
        return;
    for (uint32_t i = 0; i < r->states.size(); ++i)
        if (barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || i == barrier.Transition.Subresource)
            r->states[i] = barrier.Transition.StateAfter;
}

// ---------------------------------------------------------------------------------------------
// Contents

void DxReplayer::UploadTextures()
{
    const JValue* textures = _capture->Textures();
    if (!textures || !textures->IsArray())
        return;
    // A subresource is uploaded once. What the frame found in a texture (kind `initial`, read back
    // before the first submission that reads it) is what the frame starts from and comes first; a
    // sampled read-back is taken at a pass's end or after its submission, and may already hold what
    // the frame wrote (a history texture read and then overwritten). The capture can hold a texture
    // sampled twice (in the frame of recording before the capture, and again in the captured
    // frame), and the later reading is the one the frame saw, so those are taken last first.
    std::set<std::pair<uint64_t, UINT>> uploaded;
    std::vector<const JValue*> order;
    for (uint32_t n = 0; n < textures->count; ++n)
        if (const JValue* info = textures->items[n].Get("info"); info && Str(info->Get("kind")) == "initial")
            order.push_back(&textures->items[n]);
    for (uint32_t n = textures->count; n-- > 0;)
        if (const JValue* info = textures->items[n].Get("info"); info && Str(info->Get("kind")) == "sampled")
            order.push_back(&textures->items[n]);
    for (const JValue* entry : order)
    {
        const JValue& t = *entry;
        const JValue* info = t.Get("info");
        if (!info || info->Get("error"))
            continue;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!_capture->Payload(t.Get("payload"), data, size) || !size)
            continue;
        const uint64_t id = info->Get("id")->Uint();
        Resource* r = ResourceOf(id);
        if (!r || r->IsBuffer() || r->desc.SampleDesc.Count > 1)
            continue;
        const bool depthAspect = Str(info->Get("aspect")) == "depth";
        const bool stencilAspect = Str(info->Get("aspect")) == "stencil";
        const uint32_t baseMip = info->Get("mip") ? (uint32_t)info->Get("mip")->Uint() : 0;
        const uint32_t mips = info->Get("mips") ? (uint32_t)info->Get("mips")->Uint() : 1;
        const uint32_t layers = std::max<uint32_t>(1, info->Get("layers") ? (uint32_t)info->Get("layers")->Uint() : 1);
        const uint32_t baseLayer = info->Get("baseLayer") ? (uint32_t)info->Get("baseLayer")->Uint() : 0;
        const uint32_t plane = stencilAspect ? 1 : 0;
        dxinsp::FormatInfo format = depthAspect ? DepthPlaneFormat(r->desc.Format) : dxinsp::FormatOf(r->desc.Format);
        if (stencilAspect)
        {
            format = dxinsp::FormatInfo{};
            format.bytes = 1;
        }
        if (!format.bytes)
        {
            Problem("texture " + std::to_string(id) + ": its format cannot be uploaded");
            continue;
        }
        const bool volume = r->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        // The payload is tight rows, slice after slice, mip after mip (src/d3d12/src/capture.cpp).
        struct Region
        {
            UINT subresource;
            UINT64 rowBytes;
            UINT rows;
            UINT slices;
        };
        std::vector<Region> regions;
        uint64_t total = 0;
        for (uint32_t m = baseMip; m < baseMip + mips && m < r->mips; ++m)
        {
            for (uint32_t l = baseLayer; l < baseLayer + layers && l < r->slices; ++l)
            {
                Region region;
                region.subresource = m + l * r->mips + plane * r->mips * r->slices;
                region.rowBytes = dxinsp::RowBytes(format, MipDim(r->desc.Width, m));
                region.rows = dxinsp::RowCount(format, MipDim(r->desc.Height, m));
                region.slices = volume ? MipDim(r->desc.DepthOrArraySize, m) : 1;
                const uint64_t bytes = region.rowBytes * region.rows * region.slices;
                if (total + bytes > size)
                    break;
                regions.push_back(region);
                total += bytes;
            }
        }
        if (regions.empty())
            continue;
        // All of it or none: the payload is one run of bytes, and an entry that overlaps another in
        // part is the same texture read through another view, which the first covers.
        if (std::any_of(regions.begin(), regions.end(), [&](const Region& region) { return uploaded.count({id, region.subresource}) != 0; }))
            continue;
        for (const Region& region : regions)
            uploaded.insert({id, region.subresource});

        // A staging buffer laid out as the copies want it, rows at the footprint's pitch.
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(regions.size());
        uint64_t stagingSize = 0;
        for (size_t k = 0; k < regions.size(); ++k)
        {
            UINT64 bytes = 0;
            _device->GetCopyableFootprints(&r->desc, regions[k].subresource, 1, stagingSize, &footprints[k], nullptr, nullptr, &bytes);
            stagingSize = (footprints[k].Offset + bytes + 511) & ~511ull;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = std::max<uint64_t>(stagingSize, 1);
        bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* staging = nullptr;
        if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))))
            continue;
        void* mapped = nullptr;
        if (FAILED(staging->Map(0, nullptr, &mapped)))
        {
            staging->Release();
            continue;
        }
        const uint8_t* at = data;
        for (size_t k = 0; k < regions.size(); ++k)
        {
            uint8_t* base = static_cast<uint8_t*>(mapped) + footprints[k].Offset;
            const uint64_t slicePitch = (uint64_t)footprints[k].Footprint.RowPitch * regions[k].rows;
            for (UINT z = 0; z < regions[k].slices; ++z)
                for (UINT row = 0; row < regions[k].rows; ++row, at += regions[k].rowBytes)
                    std::memcpy(base + z * slicePitch + (uint64_t)row * footprints[k].Footprint.RowPitch, at, (size_t)regions[k].rowBytes);
        }
        staging->Unmap(0, nullptr);
        const bool ok = RunOneTime([&](ID3D12GraphicsCommandList* list) {
            for (size_t k = 0; k < regions.size(); ++k)
            {
                Transition(list, *r, regions[k].subresource, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
                D3D12_TEXTURE_COPY_LOCATION dst{r->resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                dst.SubresourceIndex = regions[k].subresource;
                D3D12_TEXTURE_COPY_LOCATION src{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                src.PlacedFootprint = footprints[k];
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
        });
        staging->Release();
        if (!ok)
            continue;
        _report->texturesUploaded++;
        if (_x)
        {
            _x->Block(DxExporter::Contents, "texture " + std::to_string(id) + ": its contents as the frame sampled them", [&](Source& s) {
                std::string items;
                for (const Region& region : regions)
                    items += (items.empty() ? "" : ", ") + std::string("{") + std::to_string(region.subresource) + ", " + std::to_string(region.rowBytes) + ", " +
                        std::to_string(region.rows) + ", " + std::to_string(region.slices) + "}";
                const std::string name = s.Local("regions");
                s.Line("const TextureRegion " + name + "[] = {" + items + "};   // subresource, bytes per row, rows, slices");
                s.Line("UploadTexture(" + s.Object(r->resource) + ", " + name + ", " + std::to_string(regions.size()) + ", " + _x->Data(data, (size_t)total) + ", " +
                    std::to_string(total) + ");   // from COMMON, left in COPY_DEST");
            });
        }
    }
}

void DxReplayer::NoteWrite(uint64_t resource, bool colorTarget)
{
    if (!resource)
        return;
    if (_swapBuffers.count(resource))
        _lastSwapWrite = resource;
    if (colorTarget)
        _lastColorTarget = resource;
}

void DxReplayer::EmitFrameEnd()
{
    if (!_x)
        return;
    // What the window shows: the swap chain buffer the frame wrote last, which is what it presented;
    // a frame without one (a renderer that never presents) shows its last color target.
    const uint64_t output = _lastSwapWrite ? _lastSwapWrite : _lastColorTarget;
    Resource* r = ResourceOf(output);
    if (r)
    {
        _x->FrameOutput(_x->NameOf(r->resource), StateText(StateOf(*r, 0)),
            std::string(_lastSwapWrite ? "the swap chain buffer the frame wrote last" : "the frame's last color target (it writes no swap chain buffer)") +
                ", resource " + std::to_string(output) + ", in the state the frame leaves it in.");
    }
    // The restore. An allocator is reset before the lists recorded from it are recorded again (every
    // submission was waited for, so none is in use), and each subresource goes back from where the
    // frame's barriers left it to where they expect to find it.
    _x->Comment(DxExporter::Restore, "The allocators the frame's lists record from.");
    for (ID3D12CommandAllocator* allocator : _allocators)
    {
        const std::string name = _x->NameOf(allocator);
        if (!name.empty())
            _x->Block(DxExporter::Restore, "", [&](Source& s) { s.Line("DX_CHECK(" + name + "->Reset());"); });
    }
    _restoring = true;
    MoveToInitialStates();
    _restoring = false;
}

void DxReplayer::MoveToInitialStates()
{
    const DxExporter::Section section = _restoring ? DxExporter::Restore : DxExporter::Contents;
    if (_x)
    {
        _x->Comment(section, _restoring ? "Every subresource from the state the frame leaves it in back to the one it expects." : "Every subresource into the state the frame expects it in: its first barrier's StateBefore, else what its uses need.");
        _x->Block(section, "", [&](Source& s) { s.Line("ID3D12GraphicsCommandList* list = BeginOneTime();"); });
    }
    RunOneTime([&](ID3D12GraphicsCommandList* list) {
        for (auto& [id, r] : _resources)
        {
            if (r.heapType != D3D12_HEAP_TYPE_DEFAULT)
                continue;
            auto it = _initial.find(id);
            for (uint32_t i = 0; i < r.states.size(); ++i)
            {
                const bool known = it != _initial.end() && i < it->second.size() && it->second[i].any;
                // A subresource the frame never touches still has to leave the upload's COPY_DEST.
                const D3D12_RESOURCE_STATES want = known ? it->second[i].state : D3D12_RESOURCE_STATE_COMMON;
                if (r.states[i] != want)
                    Transition(list, r, i, want, "list");
            }
        }
    });
    if (_x)
        _x->Block(section, "", [&](Source& s) { s.Line("EndOneTime(list);"); });
}

void DxReplayer::ApplyBufferData(const Group& group)
{
    const JValue* commands = _capture->Commands();
    std::unordered_set<uint64_t> applied;
    // The uploads into default-heap buffers, gathered into one staging buffer and one command list
    // rather than a staging resource, a list and a wait each: a frame can carry hundreds of thousands
    // of buffer read-backs, and one at a time they took minutes.
    struct Pending
    {
        Resource* resource;
        uint64_t bufferId;
        uint64_t offset;
        size_t at;
        size_t size;
    };
    std::vector<Pending> pending;
    std::vector<uint8_t> bytes;
    auto flush = [&]() {
        if (pending.empty())
            return;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes.size();
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* staging = nullptr;
        void* mapped = nullptr;
        bool ok = false;
        if (SUCCEEDED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging))) &&
            SUCCEEDED(staging->Map(0, nullptr, &mapped)))
        {
            std::memcpy(mapped, bytes.data(), bytes.size());
            staging->Unmap(0, nullptr);
            ok = RunOneTime([&](ID3D12GraphicsCommandList* list) {
                // Each copy between its own transitions, which also orders two copies into the same bytes.
                for (const Pending& u : pending)
                {
                    const D3D12_RESOURCE_STATES state = StateOf(*u.resource, 0);
                    Transition(list, *u.resource, UINT_MAX, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
                    list->CopyBufferRegion(u.resource->resource, u.offset, staging, u.at, u.size);
                    Transition(list, *u.resource, UINT_MAX, state, nullptr);
                }
            });
        }
        SafeRelease(staging);
        for (const Pending& u : pending)
        {
            if (ok)
                _report->bufferUploads++;
            else
                Problem("buffer " + std::to_string(u.bufferId) + ": its captured contents could not be uploaded");
        }
        pending.clear();
        bytes.clear();
    };
    auto apply = [&](uint64_t dataId) {
        if (!dataId || !applied.insert(dataId).second)
            return;
        auto it = _bufferData.find(dataId);
        if (it == _bufferData.end())
            return;
        const JValue* info = it->second->Get("info");
        if (!info || info->Get("error"))
            return;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!_capture->Payload(it->second->Get("payload"), data, size) || !size)
            return;
        const uint64_t bufferId = info->Get("buffer")->Uint();
        Resource* r = ResourceOf(bufferId);
        if (!r || !r->IsBuffer() || r->heapType == D3D12_HEAP_TYPE_READBACK)
            return;
        const uint64_t offset = info->Get("offset") ? info->Get("offset")->Uint() : 0;
        if (offset >= r->desc.Width)
            return;
        size = (size_t)std::min<uint64_t>(size, r->desc.Width - offset);
        // A read-back cut at the capture's Max KB: the rest of the range is not in the capture,
        // and a shader that reads it reads whatever the replay's buffer holds -- a simulation over
        // a large particle buffer then differs everywhere, with nothing else to say why.
        if (const JValue* originalSize = info->Get("originalSize"); originalSize && originalSize->Uint() > size && _truncatedBuffers.insert(bufferId).second)
        {
            const uint64_t original = originalSize->Uint();
            Problem("buffer " + std::to_string(bufferId) + ": the capture read back " + std::to_string(size) + " of the " + std::to_string(original) + " bytes the frame binds (the capture's Max KB); the rest starts as the replay's own buffer holds it, so what reads it may differ -- capture with a larger Max KB");
        }
        const D3D12_RESOURCE_STATES state = StateOf(*r, 0);
        bool ok = false;
        if (r->heapType == D3D12_HEAP_TYPE_UPLOAD)
        {
            void* mapped = nullptr;
            const D3D12_RANGE none{0, 0};
            if (SUCCEEDED(r->resource->Map(0, &none, &mapped)))
            {
                std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
                r->resource->Unmap(0, nullptr);
                ok = true;
            }
        }
        else
        {
            // A batch is sent at this size, so its staging buffer stays a reasonable allocation.
            constexpr size_t kBatchBytes = 64u << 20;
            if (!pending.empty() && bytes.size() + size > kBatchBytes)
                flush();
            pending.push_back({r, bufferId, offset, bytes.size(), size});
            bytes.insert(bytes.end(), data, data + size);
        }
        if (r->heapType == D3D12_HEAP_TYPE_UPLOAD)
        {
            if (!ok)
            {
                Problem("buffer " + std::to_string(bufferId) + ": its captured contents could not be uploaded");
                return;
            }
            _report->bufferUploads++;
        }
        if (_x)
        {
            _x->Block(DxExporter::Frame, "buffer " + std::to_string(bufferId) + ": what the frame reads from it, as captured", [&](Source& s) {
                s.Line("UploadBuffer(" + s.Object(r->resource) + ", " + std::to_string(offset) + ", " + _x->Data(data, size) + ", " + std::to_string(size) + ", " +
                    StateText(state) + ", " + (r->heapType == D3D12_HEAP_TYPE_UPLOAD ? "true" : "false") + ");");
            });
        }
    };
    for (uint32_t i = group.first; i <= group.last && i < commands->count; ++i)
    {
        const JValue& c = commands->items[i];
        if (const JValue* list = c.Get("bufferData"); list && list->IsArray())
            for (uint32_t k = 0; k < list->count; ++k)
                apply(list->items[k].Uint());
        // What an acceleration structure build read: the ids are under their field names rather
        // than in a flat list, because the UI tells a vertex buffer from an index buffer by them
        // (src/d3d12/src/raytracing.cpp). A build reading uninitialized vertices makes a structure
        // nothing ever hits, which is indistinguishable from a correct replay of an empty scene.
        if (const JValue* list = c.Get("buildData"); list && list->IsArray())
            for (uint32_t k = 0; k < list->count; ++k)
                if (const JValue* id = list->items[k].Get("capture"))
                    apply(id->Uint());
        // What a trace's local root arguments read: a root view's range, and a local table's buffer
        // descriptors' (raytracing.h in the capture library, ResolveLocalRootArguments).
        if (const JValue* list = c.Get("localRootArguments"); list && list->IsArray())
        {
            for (uint32_t k = 0; k < list->count; ++k)
            {
                const JValue& a = list->items[k];
                if (const JValue* id = a.Get("capture"))
                    apply(id->Uint());
                const JValue* descriptors = a.Get("descriptors");
                for (uint32_t d = 0; descriptors && descriptors->IsArray() && d < descriptors->count; ++d)
                    if (descriptors->items[d].Get("buffer") && descriptors->items[d].Get("data"))
                        apply(descriptors->items[d].Get("data")->Uint());
            }
        }
        const JValue* snapshot = c.Get("descriptors");
        const JValue* sets = snapshot ? snapshot->Get("sets") : nullptr;
        for (uint32_t s = 0; sets && sets->IsArray() && s < sets->count; ++s)
        {
            const JValue* bindings = sets->items[s].Get("bindings");
            for (uint32_t b = 0; bindings && bindings->IsArray() && b < bindings->count; ++b)
            {
                const JValue* list = bindings->items[b].Get("descriptors");
                for (uint32_t k = 0; list && list->IsArray() && k < list->count; ++k)
                {
                    const JValue& record = list->items[k];
                    // A texture's data id is a texture capture's, which UploadTextures has dealt with.
                    if (record.Get("buffer") && record.Get("data"))
                        apply(record.Get("data")->Uint());
                }
            }
        }
        // A submission's directly indexed heaps: the buffers their views name (WriteHeapDescriptors).
        const JValue* heaps = c.Get("heapDescriptors");
        for (uint32_t h = 0; heaps && heaps->IsArray() && h < heaps->count; ++h)
        {
            const JValue* slots = heaps->items[h].Get("slots");
            for (uint32_t k = 0; slots && slots->IsArray() && k < slots->count; ++k)
            {
                const JValue* record = slots->items[k].Get("descriptor");
                if (record && record->Get("buffer") && record->Get("data"))
                    apply(record->Get("data")->Uint());
            }
        }
    }
    flush();
}

// ---------------------------------------------------------------------------------------------
// Descriptors

bool DxReplayer::WriteDescriptor(uint64_t heapId, uint32_t index, D3D12_DESCRIPTOR_RANGE_TYPE type, const JValue& record)
{
    auto hit = _heaps.find(heapId);
    if (hit == _heaps.end() || index >= hit->second.desc.NumDescriptors || record.IsNull())
        return false;
    Heap& heap = hit->second;
    const D3D12_CPU_DESCRIPTOR_HANDLE handle{heap.cpu.ptr + (SIZE_T)index * heap.increment};
    const std::string handleText = _x ? "CpuHandle(" + _x->NameOf(heap.heap) + ", " + std::to_string(index) + ")" : std::string();
    const size_t unresolved = _env.unresolved;
    Decoder d(&record, _env);

    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
    {
        D3D12_SAMPLER_DESC desc{};
        if (!DecodeStruct(_env, record, "samplerDesc", desc))
            return false;
        _device->CreateSampler(&desc, handle);
        if (_x)
            _x->Block(DxExporter::Frame, "", [&](Source& s) { s.Line("device->CreateSampler(&" + EmitStruct(s, "sampler", desc) + ", " + handleText + ");"); });
    }
    else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
    {
        const uint64_t bufferId = IdOf(record.Get("buffer"));
        Resource* r = ResourceOf(bufferId);
        if (!r)
        {
            Problem(_env.where + ": a constant buffer view of a buffer the replay does not have");
            return false;
        }
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        desc.BufferLocation = r->resource->GetGPUVirtualAddress() + (record.Get("offset") ? record.Get("offset")->Uint() : 0);
        desc.SizeInBytes = (UINT)(((record.Get("range") ? record.Get("range")->Uint() : 0) + 255) & ~255ull);
        _device->CreateConstantBufferView(&desc, handle);
        if (_x)
            _x->Block(DxExporter::Frame, "", [&](Source& s) { s.Line("device->CreateConstantBufferView(&" + EmitStruct(s, "cbv", desc) + ", " + handleText + ");"); });
    }
    else
    {
        if (const JValue* scene = record.Get("accelerationStructure"))
        {
            // The one view with no resource behind it: it names a top level by the address a build
            // wrote it to, which this machine has at another address (dx_raytracing.cpp).
            const JValue* number = scene->Get("address");
            const uint64_t captured = number && number->IsString() ? strtoull(std::string(number->Str()).c_str(), nullptr, 0) : 0;
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.RaytracingAccelerationStructure.Location = RemapStructureAddress(captured);
            if (!desc.RaytracingAccelerationStructure.Location)
            {
                Problem(_env.where + ": it binds an acceleration structure the capture holds no build of");
                return false;
            }
            _device->CreateShaderResourceView(nullptr, &desc, handle);
            if (_x)
                _x->Block(DxExporter::Frame, "", [&](Source& s) {
                    s.Line("device->CreateShaderResourceView(nullptr, &" + EmitStruct(s, "srv", desc) + ", " + handleText + ");");
                });
            return true;
        }
        const uint64_t resourceId = IdOf(record.Get("buffer")) ? IdOf(record.Get("buffer")) : IdOf(record.Get("resource"));
        Resource* r = ResourceOf(resourceId);
        if (!r)
        {
            Problem(_env.where + ": a view of resource " + std::to_string(resourceId) + ", which the replay does not have");
            return false;
        }
        const bool hasView = d.Get("view") != nullptr;
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            if (hasView)
                d.Struct("view", desc);
            if (_env.unresolved != unresolved)
                return false;
            _device->CreateShaderResourceView(r->resource, hasView ? &desc : nullptr, handle);
            if (_x)
                _x->Block(DxExporter::Frame, "", [&](Source& s) {
                    s.Line("device->CreateShaderResourceView(" + s.Object(r->resource) + ", " + (hasView ? "&" + EmitStruct(s, "srv", desc) : std::string("nullptr")) + ", " + handleText + ");");
                });
        }
        else
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            if (hasView)
                d.Struct("view", desc);
            ID3D12Resource* counter = nullptr;
            if (d.Get("counter"))
                d.Object("counter", counter, "ID3D12Resource");
            if (_env.unresolved != unresolved)
                return false;
            _device->CreateUnorderedAccessView(r->resource, counter, hasView ? &desc : nullptr, handle);
            if (_x)
                _x->Block(DxExporter::Frame, "", [&](Source& s) {
                    s.Line("device->CreateUnorderedAccessView(" + s.Object(r->resource) + ", " + s.Object(counter) + ", " +
                        (hasView ? "&" + EmitStruct(s, "uav", desc) : std::string("nullptr")) + ", " + handleText + ");");
                });
        }
    }
    _report->descriptorsWritten++;
    return true;
}

bool DxReplayer::WriteTargetDescriptor(const JValue* entry, bool depth, PassTarget* out)
{
    if (!entry || entry->IsNull())
        return false;
    const uint64_t heapId = IdOf(entry->Get("heap"));
    const uint32_t index = entry->Get("index") ? (uint32_t)entry->Get("index")->Uint() : 0;
    const uint64_t resourceId = IdOf(entry->Get("resource"));
    auto hit = _heaps.find(heapId);
    Resource* r = ResourceOf(resourceId);
    if (hit == _heaps.end() || !r)
        return false;
    if (!depth)
        NoteWrite(resourceId, true);
    Heap& heap = hit->second;
    const D3D12_CPU_DESCRIPTOR_HANDLE handle{heap.cpu.ptr + (SIZE_T)index * heap.increment};
    const JValue* view = entry->Get("view");
    const bool hasView = view && !view->IsNull();
    PassTarget target;
    target.resource = resourceId;
    target.depth = depth;
    // Written once per content: a target bound again in a later pass is the same view of the same resource.
    const std::string key = std::to_string(resourceId) + (hasView ? std::string(":") + std::to_string((uintptr_t)view->members) : std::string(":default"));
    const bool fresh = heap.written[index] != key;
    heap.written[index] = key;
    const std::string handleText = _x ? "CpuHandle(" + _x->NameOf(heap.heap) + ", " + std::to_string(index) + ")" : std::string();
    Decoder d(entry, _env);
    if (depth)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
        if (hasView)
            d.Struct("view", desc);
        if (desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D)
            target.mip = desc.Texture2D.MipSlice;
        if (desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY)
        {
            target.mip = desc.Texture2DArray.MipSlice;
            target.firstSlice = desc.Texture2DArray.FirstArraySlice;
        }
        if (fresh)
        {
            _device->CreateDepthStencilView(r->resource, hasView ? &desc : nullptr, handle);
            if (_x)
                _x->Block(DxExporter::Frame, "", [&](Source& s) {
                    s.Line("device->CreateDepthStencilView(" + s.Object(r->resource) + ", " + (hasView ? "&" + EmitStruct(s, "dsv", desc) : std::string("nullptr")) + ", " + handleText + ");");
                });
        }
    }
    else
    {
        D3D12_RENDER_TARGET_VIEW_DESC desc{};
        if (hasView)
            d.Struct("view", desc);
        if (desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D)
            target.mip = desc.Texture2D.MipSlice;
        if (desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY)
        {
            target.mip = desc.Texture2DArray.MipSlice;
            target.firstSlice = desc.Texture2DArray.FirstArraySlice;
        }
        if (fresh)
        {
            _device->CreateRenderTargetView(r->resource, hasView ? &desc : nullptr, handle);
            if (_x)
                _x->Block(DxExporter::Frame, "", [&](Source& s) {
                    s.Line("device->CreateRenderTargetView(" + s.Object(r->resource) + ", " + (hasView ? "&" + EmitStruct(s, "rtv", desc) : std::string("nullptr")) + ", " + handleText + ");");
                });
        }
    }
    if (fresh)
        _report->descriptorsWritten++;
    if (out)
        *out = target;
    return true;
}

namespace
{
/** A JSON value as text, members in the capture's order: what a descriptor record holds, to tell a slot's content from another's. */
void Canonical(const JValue& v, std::string& out)
{
    if (v.IsObject())
    {
        out += '{';
        for (uint32_t i = 0; i < v.count; ++i)
        {
            if (v.members[i].key == "data")
                continue;   // which read-back holds the contents: not what the descriptor is
            out += v.members[i].key;
            out += ':';
            Canonical(v.members[i].value, out);
            out += ',';
        }
        out += '}';
    }
    else if (v.IsArray())
    {
        out += '[';
        for (uint32_t i = 0; i < v.count; ++i)
        {
            Canonical(v.items[i], out);
            out += ',';
        }
        out += ']';
    }
    else if (v.IsBool())
    {
        out += v.boolean ? "true" : "false";
    }
    else if (v.text)
    {
        out.append(v.text, v.length);
    }
    else
    {
        out += "null";
    }
}
} // namespace

void DxReplayer::WriteTableDescriptors(const JValue& command, const JValue& args)
{
    // The snapshot lists the table's ranges in the root signature's order, each with what its
    // descriptors held; where a range lies in the heap comes from the root signature.
    const JValue* snapshot = command.Get("descriptors");
    const JValue* sets = snapshot ? snapshot->Get("sets") : nullptr;
    if (!sets || !sets->IsArray() || !sets->count)
        return;
    const JValue& set = sets->items[0];
    const JValue* base = args.Get("BaseDescriptor");
    const uint64_t heapId = base ? IdOf(base->Get("heap")) : 0;
    const uint32_t baseIndex = base && base->Get("index") ? (uint32_t)base->Get("index")->Uint() : 0;
    const uint32_t parameter = args.Get("RootParameterIndex") ? (uint32_t)args.Get("RootParameterIndex")->Uint() : 0;
    const uint64_t rootId = IdOf(set.Get("layout"));
    auto rit = _rootSignatures.find(rootId);
    if (!heapId || rit == _rootSignatures.end() || !rit->second)
        return;
    const JValue* versioned = rit->second;
    const JValue* desc = versioned->Get("Desc_1_1") ? versioned->Get("Desc_1_1") : versioned->Get("Desc_1_0") ? versioned->Get("Desc_1_0")
                                                                                                              : versioned->Get("Desc_1_2");
    const JValue* parameters = desc ? desc->Get("pParameters") : nullptr;
    if (!parameters || !parameters->IsArray() || parameter >= parameters->count)
        return;
    const JValue* table = parameters->items[parameter].Get("DescriptorTable");
    const JValue* ranges = table ? table->Get("pDescriptorRanges") : nullptr;
    const JValue* bindings = set.Get("bindings");
    if (!ranges || !ranges->IsArray() || !bindings || !bindings->IsArray())
        return;
    uint32_t append = 0;
    for (uint32_t k = 0; k < ranges->count && k < bindings->count; ++k)
    {
        const JValue& range = ranges->items[k];
        const auto type = (D3D12_DESCRIPTOR_RANGE_TYPE)ParseEnum(range.Get("RangeType"), dxinsp::kEnum_D3D12_DESCRIPTOR_RANGE_TYPE, std::size(dxinsp::kEnum_D3D12_DESCRIPTOR_RANGE_TYPE));
        const uint32_t count = range.Get("NumDescriptors") ? (uint32_t)range.Get("NumDescriptors")->Uint() : 0;
        const uint32_t offset = range.Get("OffsetInDescriptorsFromTableStart") ? (uint32_t)range.Get("OffsetInDescriptorsFromTableStart")->Uint() : UINT_MAX;
        const uint32_t start = offset == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND ? append : offset;
        const JValue* list = bindings->items[k].Get("descriptors");
        for (uint32_t i = 0; list && list->IsArray() && i < list->count; ++i)
        {
            const JValue& record = list->items[i];
            if (record.IsNull())
                continue;
            auto hit = _heaps.find(heapId);
            if (hit == _heaps.end())
                break;
            // A slot keeps what it was given until the frame gives it something else: what a record
            // holds is its members' text, and its view's.
            //
            // Two draws of one submission can have been recorded with different contents in a slot,
            // and the later write standing for both is right: the GPU reads a descriptor when the
            // submission runs, so every draw of it reads the one slot's one contents (a static range
            // may not change at all once set, a volatile one may until the list runs). What the slot
            // held then comes with the submission when it differs from the snapshots
            // (`heapDescriptors`, WriteHeapDescriptors), and is written last.
            const uint32_t slot = baseIndex + start + i;
            std::string content = std::to_string((int)type) + ";";
            Canonical(record, content);
            if (hit->second.written[slot] == content)
                continue;
            if (WriteDescriptor(heapId, slot, type, record))
                hit->second.written[slot] = content;
        }
        append = start + (count == UINT_MAX ? (list && list->IsArray() ? list->count : 0) : count);
    }
}

void DxReplayer::WriteHeapDescriptors(const JValue& submission, uint32_t index)
{
    const JValue* heaps = submission.Get("heapDescriptors");
    if (!heaps || !heaps->IsArray())
        return;
    // A bindless heap keeps views of resources the frame never touches, and some of them the replay
    // does not have: those are counted into one line rather than one problem each.
    const size_t problems = _report->problems.size();
    uint32_t failed = 0;
    for (uint32_t h = 0; h < heaps->count; ++h)
    {
        const uint64_t heapId = IdOf(heaps->items[h].Get("heap"));
        auto hit = _heaps.find(heapId);
        const JValue* slots = heaps->items[h].Get("slots");
        if (hit == _heaps.end() || !slots || !slots->IsArray())
            continue;
        for (uint32_t k = 0; k < slots->count; ++k)
        {
            const JValue& e = slots->items[k];
            const JValue* record = e.Get("descriptor");
            if (!record || record->IsNull() || !e.Get("slot"))
                continue;
            const uint32_t slot = (uint32_t)e.Get("slot")->Uint();
            const auto type = (D3D12_DESCRIPTOR_RANGE_TYPE)(e.Get("type") ? e.Get("type")->Uint() : 0);
            // What the slot holds as a root table's snapshot would have it, so neither writes it twice.
            std::string content = std::to_string((int)type) + ";";
            Canonical(*record, content);
            if (hit->second.written[slot] == content)
                continue;
            if (WriteDescriptor(heapId, slot, type, *record))
                hit->second.written[slot] = content;
            else
                ++failed;
        }
    }
    if (failed)
    {
        _report->problems.resize(problems);
        Problem("submission " + std::to_string(index) + ": " + std::to_string(failed) +
            " descriptor(s) of a heap its shaders index directly name what the replay does not have; a shader that reads one reads nothing");
    }
}

// ---------------------------------------------------------------------------------------------
// Commands

void DxReplayer::BuildGroups()
{
    const JValue* commands = _capture->Commands();
    if (!commands || !commands->IsArray())
        return;
    std::unordered_map<uint64_t, size_t> open;
    for (uint32_t i = 0; i < commands->count; ++i)
    {
        const JValue& c = commands->items[i];
        if (c.Get("secondary"))
            continue;   // a bundle's, inlined in the list that executes it: its Close is not the list's
        const std::string m = Str(c.Get("method"));
        const uint64_t list = IdOf(c.Get("object"));
        if (m == "Reset" && Str(c.Get("object") ? c.Get("object")->Get("__class") : nullptr) == "ID3D12GraphicsCommandList")
        {
            open[list] = _groups.size();
            _groups.push_back({list, i, i, false});
        }
        else if (m == "Close")
        {
            auto it = open.find(list);
            if (it != open.end())
            {
                _groups[it->second].last = i;
                open.erase(it);
            }
        }
    }
    for (auto& [list, index] : open)
    {
        Problem("command list " + std::to_string(list) + " is not closed in the capture");
        _groups[index].last = commands->count - 1;
    }
}

bool DxReplayer::IssueCommand(uint32_t index, const std::string& m, const JValue& command, const JValue* argsOrNull, ID3D12GraphicsCommandList* list, uint64_t listId)
{
    static const JValue kNoArgs;
    const JValue& args = argsOrNull && !argsOrNull->IsNull() ? *argsOrNull : kNoArgs;
    Decoder d(&args, _env);
    const size_t unresolved = _env.unresolved;
    const std::string listName = ListName(list);
    // DXINSP_REPLAY_CRASH_AT=<command index>: an access violation there, to test what a driver crash leaves behind (main.cpp, OnCrash).
    static const long crashAt = std::getenv("DXINSP_REPLAY_CRASH_AT") ? std::atol(std::getenv("DXINSP_REPLAY_CRASH_AT")) : -1;
    if (crashAt >= 0 && (long)index == crashAt)
        *static_cast<volatile int*>(nullptr) = 0;
    auto leftOut = [&](const std::string& why) {
        Problem("command " + std::to_string(index) + " " + m + ": left out: " + why);
        if (_x)
            _x->LeftOut(index, m, why);
        return false;
    };
    auto resolved = [&] { return _env.unresolved == unresolved; };
    /** Emits `list->Method(args)`, the arguments built by `build` (which may declare locals first). */
    auto emit = [&](const std::string& call, const std::function<std::string(Source&)>& build) {
        if (!_x)
            return;
        _x->Block(DxExporter::Frame, "[" + std::to_string(index) + "]", [&](Source& s) { s.Line(call + "(" + (build ? build(s) : std::string()) + ");"); });
        _x->CountCommand();
    };
    auto method = [&](const char* name) { return listName + "->" + name; };
    /** A method of a later command list interface: the list asked for it, and left out where this runtime lacks it. */
    auto as = [&]<typename T>(T*& out) { return SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&out))) && out; };
    auto U = [&](const char* n) { UINT v = 0; d.Int(n, v); return v; };
    auto U64 = [&](const char* n) { UINT64 v = 0; d.Int(n, v); return v; };
    auto F = [&](const char* n) { float v = 0; d.Float(n, v); return v; };

    // A copy or a resolve into a swap chain buffer is how a frame that renders elsewhere reaches the screen (EmitFrameEnd).
    if (m == "CopyResource" || m == "ResolveSubresource" || m == "ResolveSubresourceRegion")
        NoteWrite(IdOf(args.Get("pDstResource")), false);
    else if (m == "CopyTextureRegion")
        NoteWrite(IdOf(args.Get("pDst") ? args.Get("pDst")->Get("pResource") : nullptr), false);

    if (m == "DrawInstanced")
    {
        const UINT a = U("VertexCountPerInstance"), b = U("InstanceCount"), c = U("StartVertexLocation"), e = U("StartInstanceLocation");
        list->DrawInstanced(a, b, c, e);
        emit(method("DrawInstanced"), [&](Source&) { return Source::Uint(a) + ", " + Source::Uint(b) + ", " + Source::Uint(c) + ", " + Source::Uint(e); });
    }
    else if (m == "DrawIndexedInstanced")
    {
        const UINT a = U("IndexCountPerInstance"), b = U("InstanceCount"), c = U("StartIndexLocation"), e = U("StartInstanceLocation");
        INT base = 0;
        d.Int("BaseVertexLocation", base);
        list->DrawIndexedInstanced(a, b, c, base, e);
        emit(method("DrawIndexedInstanced"), [&](Source&) { return Source::Uint(a) + ", " + Source::Uint(b) + ", " + Source::Uint(c) + ", " + Source::Int(base) + ", " + Source::Uint(e); });
    }
    else if (m == "Dispatch")
    {
        const UINT x = U("ThreadGroupCountX"), y = U("ThreadGroupCountY"), z = U("ThreadGroupCountZ");
        list->Dispatch(x, y, z);
        emit(method("Dispatch"), [&](Source&) { return Source::Uint(x) + ", " + Source::Uint(y) + ", " + Source::Uint(z); });
    }
    else if (m == "ExecuteIndirect")
    {
        ID3D12CommandSignature* signature = nullptr;
        ID3D12Resource* arguments = nullptr;
        ID3D12Resource* count = nullptr;
        d.Object("pCommandSignature", signature, "ID3D12CommandSignature");
        d.Object("pArgumentBuffer", arguments, "ID3D12Resource");
        if (d.Get("pCountBuffer"))
            d.Object("pCountBuffer", count, "ID3D12Resource");
        if (!resolved())
            return leftOut("it names objects the replay does not have");
        const UINT max = U("MaxCommandCount");
        const UINT64 argumentOffset = U64("ArgumentBufferOffset"), countOffset = U64("CountBufferOffset");
        list->ExecuteIndirect(signature, max, arguments, argumentOffset, count, countOffset);
        emit(method("ExecuteIndirect"), [&](Source& s) {
            return s.Object(signature) + ", " + Source::Uint(max) + ", " + s.Object(arguments) + ", " + Source::Uint(argumentOffset) + ", " + s.Object(count) + ", " + Source::Uint(countOffset);
        });
    }
    else if (m == "CopyBufferRegion")
    {
        ID3D12Resource* dst = nullptr;
        ID3D12Resource* src = nullptr;
        d.Object("pDstBuffer", dst, "ID3D12Resource");
        d.Object("pSrcBuffer", src, "ID3D12Resource");
        if (!resolved())
            return leftOut("it names objects the replay does not have");
        const UINT64 dstOffset = U64("DstOffset"), srcOffset = U64("SrcOffset"), bytes = U64("NumBytes");
        list->CopyBufferRegion(dst, dstOffset, src, srcOffset, bytes);
        emit(method("CopyBufferRegion"), [&](Source& s) { return s.Object(dst) + ", " + Source::Uint(dstOffset) + ", " + s.Object(src) + ", " + Source::Uint(srcOffset) + ", " + Source::Uint(bytes); });
    }
    else if (m == "CopyResource")
    {
        ID3D12Resource* dst = nullptr;
        ID3D12Resource* src = nullptr;
        d.Object("pDstResource", dst, "ID3D12Resource");
        d.Object("pSrcResource", src, "ID3D12Resource");
        if (!resolved())
            return leftOut("it names objects the replay does not have");
        list->CopyResource(dst, src);
        emit(method("CopyResource"), [&](Source& s) { return s.Object(dst) + ", " + s.Object(src); });
    }
    else if (m == "CopyTextureRegion")
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        D3D12_BOX box{};
        d.Struct("pDst", dst);
        d.Struct("pSrc", src);
        const bool hasBox = d.Get("pSrcBox") != nullptr;
        if (hasBox)
            d.Struct("pSrcBox", box);
        if (!resolved() || !dst.pResource || !src.pResource)
            return leftOut("it names objects the replay does not have");
        const UINT x = U("DstX"), y = U("DstY"), z = U("DstZ");
        list->CopyTextureRegion(&dst, x, y, z, &src, hasBox ? &box : nullptr);
        emit(method("CopyTextureRegion"), [&](Source& s) {
            const std::string a = EmitStruct(s, "dst", dst), b = EmitStruct(s, "src", src);
            return "&" + a + ", " + Source::Uint(x) + ", " + Source::Uint(y) + ", " + Source::Uint(z) + ", &" + b + ", " + (hasBox ? "&" + EmitStruct(s, "box", box) : std::string("nullptr"));
        });
    }
    else if (m == "ResolveSubresource")
    {
        ID3D12Resource* dst = nullptr;
        ID3D12Resource* src = nullptr;
        d.Object("pDstResource", dst, "ID3D12Resource");
        d.Object("pSrcResource", src, "ID3D12Resource");
        if (!resolved())
            return leftOut("it names objects the replay does not have");
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        d.Enum("Format", format, DX_TABLE(DXGI_FORMAT));
        const UINT dstSub = U("DstSubresource"), srcSub = U("SrcSubresource");
        list->ResolveSubresource(dst, dstSub, src, srcSub, format);
        emit(method("ResolveSubresource"), [&](Source& s) { return s.Object(dst) + ", " + Source::Uint(dstSub) + ", " + s.Object(src) + ", " + Source::Uint(srcSub) + ", " + FormatText(format); });
    }
    else if (m == "ClearRenderTargetView" || m == "ClearDepthStencilView")
    {
        const bool depth = m == "ClearDepthStencilView";
        // The view may not have been bound yet in this frame: it is written from the resource the capture names.
        const JValue* handleJson = d.Get(depth ? "DepthStencilView" : "RenderTargetView");
        const uint64_t heapId = handleJson ? IdOf(handleJson->Get("heap")) : 0;
        const uint32_t slot = handleJson && handleJson->Get("index") ? (uint32_t)handleJson->Get("index")->Uint() : 0;
        auto hit = _heaps.find(heapId);
        if (hit == _heaps.end())
            return leftOut("its view is in a descriptor heap the replay does not have");
        Resource* r = ResourceOf(IdOf(d.Get("resource")));
        if (!r)
            return leftOut("it clears a resource the replay does not have");
        const D3D12_CPU_DESCRIPTOR_HANDLE handle{hit->second.cpu.ptr + (SIZE_T)slot * hit->second.increment};
        if (hit->second.written.find(slot) == hit->second.written.end())
        {
            hit->second.written[slot] = std::to_string(IdOf(d.Get("resource"))) + ":default";
            if (depth)
                _device->CreateDepthStencilView(r->resource, nullptr, handle);
            else
                _device->CreateRenderTargetView(r->resource, nullptr, handle);
            _report->descriptorsWritten++;
            if (_x)
                _x->Block(DxExporter::Frame, "", [&](Source& s) {
                    s.Line(std::string("device->") + (depth ? "CreateDepthStencilView(" : "CreateRenderTargetView(") + s.Object(r->resource) + ", nullptr, CpuHandle(" +
                        _x->NameOf(hit->second.heap) + ", " + std::to_string(slot) + "));");
                });
        }
        std::vector<D3D12_RECT> rects;
        if (const JValue* list2 = d.Get("pRects"); list2 && list2->IsArray())
        {
            rects.resize(list2->count);
            for (uint32_t k = 0; k < list2->count; ++k)
            {
                Decoder rd(&list2->items[k], _env);
                Reflect(rd, rects[k]);
            }
        }
        if (depth)
        {
            D3D12_CLEAR_FLAGS flags{};
            d.Flags("ClearFlags", flags, DX_TABLE(D3D12_CLEAR_FLAGS));
            const float depthValue = F("Depth");
            const UINT8 stencil = (UINT8)U("Stencil");
            list->ClearDepthStencilView(handle, flags, depthValue, stencil, (UINT)rects.size(), rects.empty() ? nullptr : rects.data());
            emit(method("ClearDepthStencilView"), [&](Source& s) {
                return s.CpuHandle(handle) + ", " + Source::Flags(DX_TABLE(D3D12_CLEAR_FLAGS), flags) + ", " + Source::Float(depthValue) + ", " + Source::Uint(stencil) + ", " +
                    Source::Uint(rects.size()) + ", " + EmitStructs(s, "rects", rects.data(), rects.size());
            });
        }
        else
        {
            float color[4] = {};
            d.FixedFloats("ColorRGBA", color, 4);
            list->ClearRenderTargetView(handle, color, (UINT)rects.size(), rects.empty() ? nullptr : rects.data());
            emit(method("ClearRenderTargetView"), [&](Source& s) {
                const std::string c = s.Local("color");
                s.Line("const float " + c + "[4] = {" + Source::Float(color[0]) + ", " + Source::Float(color[1]) + ", " + Source::Float(color[2]) + ", " + Source::Float(color[3]) + "};");
                return s.CpuHandle(handle) + ", " + c + ", " + Source::Uint(rects.size()) + ", " + EmitStructs(s, "rects", rects.data(), rects.size());
            });
        }
    }
    else if (m == "IASetPrimitiveTopology")
    {
        D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        d.Enum("PrimitiveTopology", topology, DX_TABLE(D3D_PRIMITIVE_TOPOLOGY));
        list->IASetPrimitiveTopology(topology);
        emit(method("IASetPrimitiveTopology"), [&](Source&) { return Source::Enum(DX_TABLE(D3D_PRIMITIVE_TOPOLOGY), topology); });
    }
    else if (m == "RSSetViewports" || m == "RSSetScissorRects")
    {
        const bool viewports = m == "RSSetViewports";
        const JValue* items = d.Get(viewports ? "pViewports" : "pRects");
        std::vector<D3D12_VIEWPORT> vps;
        std::vector<D3D12_RECT> rects;
        for (uint32_t k = 0; items && items->IsArray() && k < items->count; ++k)
        {
            Decoder id(&items->items[k], _env);
            if (viewports)
            {
                vps.emplace_back();
                Reflect(id, vps.back());
            }
            else
            {
                rects.emplace_back();
                Reflect(id, rects.back());
            }
        }
        if (viewports)
            list->RSSetViewports((UINT)vps.size(), vps.data());
        else
            list->RSSetScissorRects((UINT)rects.size(), rects.data());
        emit(method(viewports ? "RSSetViewports" : "RSSetScissorRects"), [&](Source& s) {
            return viewports ? Source::Uint(vps.size()) + ", " + EmitStructs(s, "viewports", vps.data(), vps.size())
                             : Source::Uint(rects.size()) + ", " + EmitStructs(s, "scissors", rects.data(), rects.size());
        });
    }
    else if (m == "OMSetBlendFactor")
    {
        float factor[4] = {};
        d.FixedFloats("BlendFactor", factor, 4);
        list->OMSetBlendFactor(factor);
        emit(method("OMSetBlendFactor"), [&](Source& s) {
            const std::string c = s.Local("blendFactor");
            s.Line("const float " + c + "[4] = {" + Source::Float(factor[0]) + ", " + Source::Float(factor[1]) + ", " + Source::Float(factor[2]) + ", " + Source::Float(factor[3]) + "};");
            return c;
        });
    }
    else if (m == "OMSetStencilRef")
    {
        const UINT ref = U("StencilRef");
        list->OMSetStencilRef(ref);
        emit(method("OMSetStencilRef"), [&](Source&) { return Source::Uint(ref); });
    }
    else if (m == "OMSetDepthBounds")
    {
        ID3D12GraphicsCommandList1* list1 = nullptr;
        if (!as(list1))
            return leftOut("this runtime has no ID3D12GraphicsCommandList1");
        const float lo = F("Min"), hi = F("Max");
        list1->OMSetDepthBounds(lo, hi);
        list1->Release();
        emit("As<ID3D12GraphicsCommandList1>(" + listName + ")->OMSetDepthBounds", [&](Source&) { return Source::Float(lo) + ", " + Source::Float(hi); });
    }
    else if (m == "SetPipelineState" || m == "SetGraphicsRootSignature" || m == "SetComputeRootSignature")
    {
        const bool pipeline = m == "SetPipelineState";
        IUnknown* object = nullptr;
        d.Object(pipeline ? "pPipelineState" : "pRootSignature", object, pipeline ? "ID3D12PipelineState" : "ID3D12RootSignature");
        if (!resolved())
            return leftOut("it names objects the replay does not have");
        if (pipeline)
            list->SetPipelineState(static_cast<ID3D12PipelineState*>(object));
        else if (m == "SetGraphicsRootSignature")
        {
            list->SetGraphicsRootSignature(static_cast<ID3D12RootSignature*>(object));
            _graphicsRoot = IdOf(d.Get("pRootSignature"));
        }
        else
        {
            list->SetComputeRootSignature(static_cast<ID3D12RootSignature*>(object));
            _computeRoot = IdOf(d.Get("pRootSignature"));
        }
        emit(method(m.c_str()), [&](Source& s) { return s.Object(object); });
    }
    else if (m == "SetDescriptorHeaps")
    {
        std::vector<ID3D12DescriptorHeap*> heaps;
        const JValue* items = d.Get("ppDescriptorHeaps");
        for (uint32_t k = 0; items && items->IsArray() && k < items->count; ++k)
        {
            IUnknown* heap = Object(IdOf(&items->items[k]));
            if (!heap)
                return leftOut("it names a descriptor heap the replay does not have");
            heaps.push_back(static_cast<ID3D12DescriptorHeap*>(heap));
        }
        list->SetDescriptorHeaps((UINT)heaps.size(), heaps.data());
        emit(method("SetDescriptorHeaps"), [&](Source& s) {
            std::string names;
            for (ID3D12DescriptorHeap* h : heaps)
                names += (names.empty() ? "" : ", ") + s.Object(h);
            const std::string local = s.Local("heaps");
            s.Line("ID3D12DescriptorHeap* const " + local + "[] = {" + names + "};");
            return Source::Uint(heaps.size()) + ", " + local;
        });
    }
    else if (m == "SetGraphicsRootDescriptorTable" || m == "SetComputeRootDescriptorTable")
    {
        WriteTableDescriptors(command, args);
        const JValue* base = d.Get("BaseDescriptor");
        auto hit = _heaps.find(base ? IdOf(base->Get("heap")) : 0);
        if (hit == _heaps.end() || !hit->second.gpu.ptr)
            return leftOut("its table is in no shader-visible heap the replay has");
        const uint32_t slot = base->Get("index") ? (uint32_t)base->Get("index")->Uint() : 0;
        const D3D12_GPU_DESCRIPTOR_HANDLE handle{hit->second.gpu.ptr + (UINT64)slot * hit->second.increment};
        const UINT parameter = U("RootParameterIndex");
        if (m == "SetGraphicsRootDescriptorTable")
            list->SetGraphicsRootDescriptorTable(parameter, handle);
        else
            list->SetComputeRootDescriptorTable(parameter, handle);
        emit(method(m.c_str()), [&](Source&) { return Source::Uint(parameter) + ", GpuHandle(" + _x->NameOf(hit->second.heap) + ", " + std::to_string(slot) + ")"; });
    }
    else if (m.find("Root32BitConstant") != std::string::npos)
    {
        const bool compute = m.find("Compute") != std::string::npos;
        const UINT parameter = U("RootParameterIndex"), count = U("Num32BitValuesToSet"), offset = U("DestOffsetIn32BitValues");
        const JValue* bytes = d.Get("pSrcData");
        std::vector<uint8_t> data;
        if (bytes && bytes->Get("base64"))
            vkreplay::DecodeBase64(bytes->Get("base64")->Str(), data);
        data.resize(std::max<size_t>(data.size(), (size_t)count * 4), 0);
        if (compute)
            list->SetComputeRoot32BitConstants(parameter, count, data.data(), offset);
        else
            list->SetGraphicsRoot32BitConstants(parameter, count, data.data(), offset);
        emit(method(compute ? "SetComputeRoot32BitConstants" : "SetGraphicsRoot32BitConstants"), [&](Source& s) {
            std::string words;
            for (UINT k = 0; k < count; ++k)
            {
                uint32_t word = 0;
                std::memcpy(&word, &data[(size_t)k * 4], 4);
                char hex[16];
                std::snprintf(hex, sizeof(hex), "0x%08x", word);
                words += (k ? ", " : "") + std::string(hex);
            }
            const std::string local = s.Local("constants");
            s.Line("const UINT " + local + "[] = {" + words + "};");
            return Source::Uint(parameter) + ", " + Source::Uint(count) + ", " + local + ", " + Source::Uint(offset);
        });
    }
    else if (m.find("RootConstantBufferView") != std::string::npos || m.find("RootShaderResourceView") != std::string::npos ||
        m.find("RootUnorderedAccessView") != std::string::npos)
    {
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        d.Address("BufferLocation", address);
        if (!resolved())
            return leftOut("its buffer is not one the replay has");
        const UINT parameter = U("RootParameterIndex");
        if (m == "SetGraphicsRootConstantBufferView")
            list->SetGraphicsRootConstantBufferView(parameter, address);
        else if (m == "SetComputeRootConstantBufferView")
            list->SetComputeRootConstantBufferView(parameter, address);
        else if (m == "SetGraphicsRootShaderResourceView")
            list->SetGraphicsRootShaderResourceView(parameter, address);
        else if (m == "SetComputeRootShaderResourceView")
            list->SetComputeRootShaderResourceView(parameter, address);
        else if (m == "SetGraphicsRootUnorderedAccessView")
            list->SetGraphicsRootUnorderedAccessView(parameter, address);
        else
            list->SetComputeRootUnorderedAccessView(parameter, address);
        emit(method(m.c_str()), [&](Source& s) { return Source::Uint(parameter) + ", " + s.Address(address); });
    }
    else if (m == "IASetIndexBuffer")
    {
        D3D12_INDEX_BUFFER_VIEW view{};
        const bool has = d.Get("pView") != nullptr;
        if (has)
            d.Struct("pView", view);
        if (!resolved())
            return leftOut("its buffer is not one the replay has");
        list->IASetIndexBuffer(has ? &view : nullptr);
        emit(method("IASetIndexBuffer"), [&](Source& s) { return has ? "&" + EmitStruct(s, "indexBuffer", view) : std::string("nullptr"); });
    }
    else if (m == "IASetVertexBuffers")
    {
        std::vector<D3D12_VERTEX_BUFFER_VIEW> views;
        const JValue* items = d.Get("pViews");
        for (uint32_t k = 0; items && items->IsArray() && k < items->count; ++k)
        {
            views.emplace_back();
            // An empty slot has no address, which is no fault of the capture's.
            if (items->items[k].Get("BufferLocation") && !items->items[k].Get("BufferLocation")->IsNull())
            {
                Decoder vd(&items->items[k], _env);
                Reflect(vd, views.back());
            }
        }
        if (!resolved())
            return leftOut("one of its buffers is not one the replay has");
        const UINT start = U("StartSlot");
        list->IASetVertexBuffers(start, (UINT)views.size(), views.empty() ? nullptr : views.data());
        emit(method("IASetVertexBuffers"), [&](Source& s) { return Source::Uint(start) + ", " + Source::Uint(views.size()) + ", " + EmitStructs(s, "vertexBuffers", views.data(), views.size()); });
    }
    else if (m == "OMSetRenderTargets")
    {
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> handles;
        const JValue* items = d.Get("pRenderTargetDescriptors");
        for (uint32_t k = 0; items && items->IsArray() && k < items->count; ++k)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE h = d.HandleOf(&items->items[k]);
            handles.push_back(h);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE depth{};
        const bool hasDepth = d.Get("pDepthStencilDescriptor") != nullptr;
        if (hasDepth)
            depth = d.HandleOf(d.Get("pDepthStencilDescriptor"));
        if (!resolved())
            return leftOut("a target's descriptor heap is not one the replay has");
        // Every handle is passed on its own: the capture wrote a single range out slot by slot.
        list->OMSetRenderTargets((UINT)handles.size(), handles.empty() ? nullptr : handles.data(), FALSE, hasDepth ? &depth : nullptr);
        emit(method("OMSetRenderTargets"), [&](Source& s) {
            std::string names;
            for (const D3D12_CPU_DESCRIPTOR_HANDLE& h : handles)
                names += (names.empty() ? "" : ", ") + s.CpuHandle(h);
            std::string targets = "nullptr";
            if (!handles.empty())
            {
                targets = s.Local("renderTargets");
                s.Line("const D3D12_CPU_DESCRIPTOR_HANDLE " + targets + "[] = {" + names + "};");
            }
            std::string depthName = "nullptr";
            if (hasDepth)
            {
                depthName = s.Local("depthStencil");
                s.Line("const D3D12_CPU_DESCRIPTOR_HANDLE " + depthName + " = " + s.CpuHandle(depth) + ";");
                depthName = "&" + depthName;
            }
            return Source::Uint(handles.size()) + ", " + targets + ", FALSE, " + depthName;
        });
    }
    else if (m == "BeginRenderPass")
    {
        ID3D12GraphicsCommandList4* list4 = nullptr;
        if (!as(list4))
            return leftOut("this runtime has no ID3D12GraphicsCommandList4");
        std::vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> targets;
        const JValue* items = d.Get("pRenderTargets");
        for (uint32_t k = 0; items && items->IsArray() && k < items->count; ++k)
        {
            targets.emplace_back();
            Decoder td(&items->items[k], _env);
            Reflect(td, targets.back());
        }
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth{};
        const bool hasDepth = d.Get("pDepthStencil") != nullptr;
        if (hasDepth)
            d.Struct("pDepthStencil", depth);
        D3D12_RENDER_PASS_FLAGS flags{};
        d.Flags("Flags", flags, DX_TABLE(D3D12_RENDER_PASS_FLAGS));
        if (!resolved())
        {
            list4->Release();
            return leftOut("it names objects the replay does not have");
        }
        list4->BeginRenderPass((UINT)targets.size(), targets.empty() ? nullptr : targets.data(), hasDepth ? &depth : nullptr, flags);
        list4->Release();
        emit("As<ID3D12GraphicsCommandList4>(" + listName + ")->BeginRenderPass", [&](Source& s) {
            const std::string t = EmitStructs(s, "renderTargets", targets.data(), targets.size());
            const std::string ds = hasDepth ? "&" + EmitStruct(s, "depthStencil", depth) : std::string("nullptr");
            return Source::Uint(targets.size()) + ", " + t + ", " + ds + ", " + (flags ? Source::Flags(DX_TABLE(D3D12_RENDER_PASS_FLAGS), flags) : std::string("D3D12_RENDER_PASS_FLAG_NONE"));
        });
    }
    else if (m == "EndRenderPass")
    {
        ID3D12GraphicsCommandList4* list4 = nullptr;
        if (!as(list4))
            return leftOut("this runtime has no ID3D12GraphicsCommandList4");
        list4->EndRenderPass();
        list4->Release();
        emit("As<ID3D12GraphicsCommandList4>(" + listName + ")->EndRenderPass", nullptr);
    }
    else if (m == "ResourceBarrier")
    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        const JValue* items = d.Get("pBarriers");
        for (uint32_t k = 0; items && items->IsArray() && k < items->count; ++k)
        {
            barriers.emplace_back();
            Decoder bd(&items->items[k], _env);
            Reflect(bd, barriers.back());
        }
        if (!resolved())
            return leftOut("it names resources the replay does not have");
        if (barriers.empty())
            return true;
        list->ResourceBarrier((UINT)barriers.size(), barriers.data());
        for (const D3D12_RESOURCE_BARRIER& b : barriers)
            NoteBarrier(b);
        emit(method("ResourceBarrier"), [&](Source& s) { return Source::Uint(barriers.size()) + ", " + EmitStructs(s, "barriers", barriers.data(), barriers.size()); });
    }
    else if (m == "BeginQuery" || m == "EndQuery")
    {
        ID3D12QueryHeap* heap = nullptr;
        d.Object("pQueryHeap", heap, "ID3D12QueryHeap");
        if (!resolved())
            return leftOut("it names a query heap the replay does not have");
        D3D12_QUERY_TYPE type{};
        d.Enum("Type", type, DX_TABLE(D3D12_QUERY_TYPE));
        const UINT slot = U("Index");
        if (m == "BeginQuery")
            list->BeginQuery(heap, type, slot);
        else
            list->EndQuery(heap, type, slot);
        emit(method(m.c_str()), [&](Source& s) { return s.Object(heap) + ", " + Source::Enum(DX_TABLE(D3D12_QUERY_TYPE), type) + ", " + Source::Uint(slot); });
    }
    else if (m == "ResolveQueryData")
    {
        ID3D12QueryHeap* heap = nullptr;
        ID3D12Resource* destination = nullptr;
        d.Object("pQueryHeap", heap, "ID3D12QueryHeap");
        d.Object("pDestinationBuffer", destination, "ID3D12Resource");
        if (!resolved())
            return leftOut("it names objects the replay does not have");
        D3D12_QUERY_TYPE type{};
        d.Enum("Type", type, DX_TABLE(D3D12_QUERY_TYPE));
        const UINT start = U("StartIndex"), count = U("NumQueries");
        const UINT64 offset = U64("AlignedDestinationBufferOffset");
        list->ResolveQueryData(heap, type, start, count, destination, offset);
        emit(method("ResolveQueryData"), [&](Source& s) {
            return s.Object(heap) + ", " + Source::Enum(DX_TABLE(D3D12_QUERY_TYPE), type) + ", " + Source::Uint(start) + ", " + Source::Uint(count) + ", " + s.Object(destination) + ", " + Source::Uint(offset);
        });
    }
    else if (m == "DispatchMesh")
    {
        ID3D12GraphicsCommandList6* list6 = nullptr;
        if (!as(list6))
            return leftOut("this runtime has no ID3D12GraphicsCommandList6");
        const UINT x = U("ThreadGroupCountX"), y = U("ThreadGroupCountY"), z = U("ThreadGroupCountZ");
        list6->DispatchMesh(x, y, z);
        list6->Release();
        emit("As<ID3D12GraphicsCommandList6>(" + listName + ")->DispatchMesh", [&](Source&) { return Source::Uint(x) + ", " + Source::Uint(y) + ", " + Source::Uint(z); });
    }
    else if (m == "DiscardResource")
    {
        ID3D12Resource* resource = nullptr;
        d.Object("pResource", resource, "ID3D12Resource");
        if (!resolved() || !resource)
            return leftOut("it names a resource the replay does not have");
        D3D12_DISCARD_REGION region{};
        const bool hasRegion = d.Get("pRegion") && !d.Get("pRegion")->IsNull() && DecodeStruct(_env, args, "pRegion", region);
        list->DiscardResource(resource, hasRegion ? &region : nullptr);
        emit(method("DiscardResource"), [&](Source& s) { return s.Object(resource) + ", " + (hasRegion ? "&" + EmitStruct(s, "region", region) : std::string("nullptr")); });
    }
    else if (m == "ClearState")
    {
        ID3D12PipelineState* pipeline = nullptr;
        if (d.Get("pPipelineState") && !d.Get("pPipelineState")->IsNull())
            d.Object("pPipelineState", pipeline, "ID3D12PipelineState");
        if (!resolved())
            return leftOut("it names a pipeline the replay does not have");
        list->ClearState(pipeline);
        _graphicsRoot = _computeRoot = 0;
        emit(method("ClearState"), [&](Source& s) { return s.Object(pipeline); });
    }
    else if (m == "SetViewInstanceMask")
    {
        ID3D12GraphicsCommandList2* list2 = nullptr;
        if (!as(list2))
            return leftOut("this runtime has no ID3D12GraphicsCommandList2");
        const UINT mask = U("Mask");
        list2->SetViewInstanceMask(mask);
        list2->Release();
        emit("As<ID3D12GraphicsCommandList2>(" + listName + ")->SetViewInstanceMask", [&](Source&) { return Source::Uint(mask); });
    }
    else if (m == "SetPredication")
    {
        ID3D12Resource* buffer = nullptr;
        if (d.Get("pBuffer") && !d.Get("pBuffer")->IsNull())
            d.Object("pBuffer", buffer, "ID3D12Resource");
        if (!resolved())
            return leftOut("it names a buffer the replay does not have");
        D3D12_PREDICATION_OP op{};
        d.Enum("Operation", op, DX_TABLE(D3D12_PREDICATION_OP));
        const UINT64 offset = U64("AlignedBufferOffset");
        list->SetPredication(buffer, offset, op);
        emit(method("SetPredication"), [&](Source& s) { return s.Object(buffer) + ", " + Source::Uint(offset) + ", " + Source::Enum(DX_TABLE(D3D12_PREDICATION_OP), op); });
    }
    else if (m == "SOSetTargets")
    {
        std::vector<D3D12_STREAM_OUTPUT_BUFFER_VIEW> views;
        const JValue* items = d.Get("pViews");
        for (uint32_t k = 0; items && items->IsArray() && k < items->count; ++k)
        {
            views.emplace_back();
            if (items->items[k].Get("BufferLocation") && !items->items[k].Get("BufferLocation")->IsNull())
            {
                Decoder vd(&items->items[k], _env);
                Reflect(vd, views.back());
            }
        }
        if (!resolved())
            return leftOut("one of its buffers is not one the replay has");
        const UINT start = U("StartSlot");
        list->SOSetTargets(start, (UINT)views.size(), views.empty() ? nullptr : views.data());
        emit(method("SOSetTargets"), [&](Source& s) { return Source::Uint(start) + ", " + Source::Uint(views.size()) + ", " + EmitStructs(s, "streamOutput", views.data(), views.size()); });
    }
    else if (m == "RSSetShadingRateImage")
    {
        ID3D12GraphicsCommandList5* list5 = nullptr;
        if (!as(list5))
            return leftOut("this runtime has no ID3D12GraphicsCommandList5");
        ID3D12Resource* image = nullptr;
        if (d.Get("shadingRateImage"))
            d.Object("shadingRateImage", image, "ID3D12Resource");
        if (!resolved())
        {
            list5->Release();
            return leftOut("it names a resource the replay does not have");
        }
        list5->RSSetShadingRateImage(image);
        list5->Release();
        emit("As<ID3D12GraphicsCommandList5>(" + listName + ")->RSSetShadingRateImage", [&](Source& s) { return s.Object(image); });
    }
    else if (m == "BeginEvent" || m == "EndEvent" || m == "SetMarker")
    {
        // A debug marker's payload is the application's own encoding of a label, which does nothing to the frame.
        if (_x)
            _x->Comment(DxExporter::Frame, "[" + std::to_string(index) + "] " + m + (d.Get("label") ? ": " + Str(d.Get("label")) : std::string()));
        return true;
    }
    else if (m == "ExecuteBundle")
    {
        const uint64_t bundleId = IdOf(d.Get("pCommandList"));
        auto bundle = static_cast<ID3D12GraphicsCommandList*>(Object(bundleId));
        if (!bundle)
            return leftOut("the bundle was not replayed");
        if (!RecordBundle(index, bundleId, bundle))
            return leftOut("the bundle's recording is not in the capture: it was recorded before the capture began (Record always keeps it)");
        list->ExecuteBundle(bundle);
        emit(method("ExecuteBundle"), [&](Source& s) { return s.Object(bundle); });
    }
    else if (m == "DispatchRays" || m == "BuildRaytracingAccelerationStructure" || m == "CopyRaytracingAccelerationStructure" ||
        m == "EmitRaytracingAccelerationStructurePostbuildInfo" || m == "SetPipelineState1")
    {
        // dx_raytracing.cpp: the addresses decode like any other, but the bytes inside an instance
        // buffer and inside a binding table are the captured process's and have to be rewritten.
        std::string why;
        if (!IssueRaytracingCommand(m, command, &args, list, why, index))
            return leftOut(why);
    }
    else
    {
        return leftOut("the replay does not issue it yet");
    }
    (void)listId;
    return true;
}

ID3D12CommandAllocator* DxReplayer::MissingAllocator(uint64_t id, D3D12_COMMAND_LIST_TYPE type)
{
    ID3D12CommandAllocator* allocator = nullptr;
    if (FAILED(_device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))))
        return nullptr;
    _allocators.push_back(allocator);
    _objects[id] = allocator;
    _report->objectsCreated++;
    if (_x)
    {
        const std::string name = _x->Declare("ID3D12CommandAllocator", "commandAllocator", id, allocator);
        _x->Block(DxExporter::Create, "ID3D12CommandAllocator " + std::to_string(id) + ": not among the capture's objects, made from its type", [&](Source& s) {
            s.Line("DX_CHECK(device->CreateCommandAllocator(" + Source::Enum(DX_TABLE(D3D12_COMMAND_LIST_TYPE), type) + ", IID_PPV_ARGS(&" + name + ")));");
        });
    }
    return allocator;
}

ID3D12GraphicsCommandList* DxReplayer::MissingList(uint64_t id, D3D12_COMMAND_LIST_TYPE type)
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(_device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))))
        return nullptr;
    if (FAILED(_device->CreateCommandList(0, type, allocator, nullptr, IID_PPV_ARGS(&list))))
    {
        allocator->Release();
        return nullptr;
    }
    list->Close();
    _created.push_back(allocator);
    _objects[id] = list;
    _report->objectsCreated++;
    if (_x)
    {
        const std::string name = _x->Declare("ID3D12GraphicsCommandList", "commandList", id, list);
        _x->Block(DxExporter::Create, "ID3D12GraphicsCommandList " + std::to_string(id) + ": not among the capture's objects, made from its queue's type", [&](Source& s) {
            s.Line(name + " = CreateClosedCommandList(" + Source::Enum(DX_TABLE(D3D12_COMMAND_LIST_TYPE), type) + ");");
        });
    }
    return list;
}

bool DxReplayer::RecordBundle(uint32_t executeIndex, uint64_t bundleId, ID3D12GraphicsCommandList* bundle)
{
    const JValue* commands = _capture->Commands();
    auto info = _bundles.find(bundleId);
    if (info == _bundles.end())
        return false;
    if (info->second.recorded)
        return true;
    // The capture inlines what the bundle recorded after the command that executes it.
    uint32_t end = executeIndex + 1;
    while (end < commands->count && commands->items[end].Get("secondary") && commands->items[end].Get("secondary")->Uint() == bundleId)
        ++end;
    if (end == executeIndex + 1)
        return false;
    // A bundle has no Reset in the capture: it is reset as it was created. The allocator is the
    // replay's own for it when the application's is gone.
    auto allocator = static_cast<ID3D12CommandAllocator*>(Object(info->second.allocator));
    auto initial = static_cast<ID3D12PipelineState*>(Object(info->second.initialState));
    if (!allocator || FAILED(bundle->Reset(allocator, initial)))
    {
        Problem("bundle " + std::to_string(bundleId) + ": Reset failed");
        return false;
    }
    info->second.recorded = true;
    const std::string name = ListName(bundle);
    if (_x)
    {
        _x->Comment(DxExporter::Frame, "bundle " + std::to_string(bundleId) + ": commands " + std::to_string(executeIndex + 1) + " to " + std::to_string(end - 1));
        _x->Block(DxExporter::Frame, "", [&](Source& s) { s.Line("DX_CHECK(" + name + "->Reset(" + s.Object(allocator) + ", " + s.Object(initial) + "));"); });
    }
    const std::string where = _env.where;
    for (uint32_t i = executeIndex + 1; i < end; ++i)
    {
        const JValue& c = commands->items[i];
        const std::string m = Str(c.Get("method"));
        if (m == "Close")
            continue;
        _env.where = "command " + std::to_string(i) + " " + m;
        if (_options.trace)
        {
            std::fprintf(stderr, "command %u %s (bundle %llu)\n", i, m.c_str(), (unsigned long long)bundleId);
            std::fflush(stderr);
        }
        if (IssueCommand(i, m, c, c.Get("args"), bundle, bundleId))
            _report->commandsRecorded++;
    }
    _env.where = where;
    if (FAILED(bundle->Close()))
        Problem("bundle " + std::to_string(bundleId) + ": Close failed, so what it recorded is not valid");
    if (_x)
        _x->Block(DxExporter::Frame, "", [&](Source& s) { s.Line("DX_CHECK(" + name + "->Close());"); });
    return true;
}

void DxReplayer::RecordGroup(Group& group, ID3D12GraphicsCommandList* list, std::vector<Readback>& readbacks)
{
    const JValue* commands = _capture->Commands();
    group.used = true;
    const JValue& reset = commands->items[group.first];
    const JValue* resetArgs = reset.Get("args");
    const uint32_t frame = reset.Get("frame") ? (uint32_t)reset.Get("frame")->Uint() : 0;
    _env.where = "command " + std::to_string(group.first) + " Reset";
    auto allocator = static_cast<ID3D12CommandAllocator*>(Object(resetArgs ? IdOf(resetArgs->Get("pAllocator")) : 0));
    auto initial = static_cast<ID3D12PipelineState*>(Object(resetArgs ? IdOf(resetArgs->Get("pInitialState")) : 0));
    if (!allocator)
    {
        // An allocator the capture has no object for, or none named at all: a list the capture began
        // to record at its first call seen rather than at its Reset (the capture library's Adopt).
        // The id of one the replay makes for such a list is out of the capture's range.
        const uint64_t named = resetArgs ? IdOf(resetArgs->Get("pAllocator")) : 0;
        allocator = MissingAllocator(named ? named : (1ull << 40) + group.list, list->GetType());
    }
    if (!allocator)
    {
        Problem("command list " + std::to_string(group.list) + ": its allocator was not replayed");
        return;
    }
    // Allocators are fresh and the frame is recorded once, so none is reset: a list's recording
    // shares its allocator with the others the application recorded from it.
    if (FAILED(list->Reset(allocator, initial)))
    {
        Problem("command list " + std::to_string(group.list) + ": Reset failed");
        return;
    }
    const std::string listName = ListName(list);
    if (_x)
    {
        _x->Comment(DxExporter::Frame, "command list " + std::to_string(group.list) + ": commands " + std::to_string(group.first) + " to " + std::to_string(group.last));
        _x->Block(DxExporter::Frame, "[" + std::to_string(group.first) + "]", [&](Source& s) {
            s.Line("DX_CHECK(" + listName + "->Reset(" + s.Object(allocator) + ", " + s.Object(initial) + "));");
        });
    }
    _graphicsRoot = _computeRoot = 0;
    _boundPipeline = resetArgs ? IdOf(resetArgs->Get("pInitialState")) : 0;
    _appQueryDepth = 0;
    BeginListMeasurements();
    Pass pass;
    uint32_t passCount = 0;
    for (uint32_t i = group.first + 1; i < group.last; ++i)
    {
        const JValue& c = commands->items[i];
        if (IdOf(c.Get("object")) != group.list)
            continue;   // another list's recording interleaved
        if (c.Get("secondary"))
            continue;                    // a bundle's, recorded by the ExecuteBundle before it
        const std::string m = Str(c.Get("method"));
        const JValue* args = c.Get("args");
        _env.where = "command " + std::to_string(i) + " " + m;
        if (_options.trace)
        {
            std::fprintf(stderr, "command %u %s\n", i, m.c_str());
            std::fflush(stderr);
        }
        if (m == "EndRenderTargets")
        {
            // The capture's own marker: the pass OMSetRenderTargets began ends here, and here it read the targets back.
            if (pass.active && _options.compareTargets)
                InjectReadbacks(list, pass, readbacks);
            if (pass.active && _counters)
                PopCounterRange(list);
            pass.active = false;
            continue;
        }
        if (m == "OMSetRenderTargets" || m == "BeginRenderPass")
        {
            pass = Pass{};
            pass.active = true;
            pass.realPass = m == "BeginRenderPass";
            pass.index = passCount++;
            pass.frame = frame;
            pass.list = group.list;
            // A counter range around the pass, named so its values can be matched back to it
            // (dx_counters.cpp). It goes in before the pass's own commands are recorded.
            if (_counters)
                PushCounterRange(list, pass.index, i, frame, group.list);
            // The views the pass renders to, written before the command that names them.
            const JValue* targets = args ? args->Get(pass.realPass ? "pRenderTargets" : "pRenderTargetDescriptors") : nullptr;
            const uint32_t colorCount = targets && targets->IsArray() ? targets->count : 0;
            pass.targets.assign(colorCount + 1, PassTarget{});
            auto writeTarget = [&](const JValue* entry, bool depth, uint32_t slot) {
                if (!entry || entry->IsNull())
                    return;
                if (pass.realPass)
                {
                    // BeginRenderPass keeps the handle under cpuDescriptor, beside the resource and the view:
                    // as OMSetRenderTargets' one entry {heap, index, resource, view}, which WriteTargetDescriptor reads.
                    const JValue* handle = entry->Get("cpuDescriptor");
                    if (!handle)
                        return;
                    vkreplay::JMember* members = _arena.Make<vkreplay::JMember>(4);
                    uint32_t n = 0;
                    for (const char* key : {"heap", "index"})
                        if (const JValue* v = handle->Get(key))
                        {
                            members[n].key = key;
                            members[n].value = *v;
                            ++n;
                        }
                    for (const char* key : {"resource", "view"})
                        if (const JValue* v = entry->Get(key))
                        {
                            members[n].key = key;
                            members[n].value = *v;
                            ++n;
                        }
                    JValue merged;
                    merged.type = vkreplay::JType::Object;
                    merged.members = members;
                    merged.count = n;
                    WriteTargetDescriptor(&merged, depth, &pass.targets[slot]);
                    auto discards = [&](const char* access) {
                        const JValue* ending = entry->Get(access);
                        return ending && Str(ending->Get("Type")) == "D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD";
                    };
                    pass.targets[slot].discarded = discards(depth ? "DepthEndingAccess" : "EndingAccess");
                    pass.targets[slot].stencilDiscarded = depth && discards("StencilEndingAccess");
                }
                else
                {
                    WriteTargetDescriptor(entry, depth, &pass.targets[slot]);
                }
            };
            for (uint32_t k = 0; k < colorCount; ++k)
                writeTarget(&targets->items[k], false, k);
            writeTarget(args ? args->Get(pass.realPass ? "pDepthStencil" : "pDepthStencilDescriptor") : nullptr, true, colorCount);
        }
        // What the measurements need to know of the list's state (dx_measure.cpp).
        if (m == "SetPipelineState")
            _boundPipeline = args ? IdOf(args->Get("pPipelineState")) : 0;
        else if (m == "BeginQuery")
            ++_appQueryDepth;
        else if (m == "EndQuery" && _appQueryDepth)
            --_appQueryDepth;
        int drawQuery = -1;
        if (_measure && IsActionMethod(m))
        {
            const uint32_t passIndex = pass.active ? pass.index : UINT32_MAX;
            IssueAblation(i, m, c, args, list, group.list, frame, passIndex);
            // A bundle's draws cannot hold queries, so the bundle is measured whole and the time
            // goes to the first draw it holds, which is a command the capture lists after it.
            uint32_t measured = i;
            if (m == "ExecuteBundle")
            {
                for (uint32_t k = i + 1; k < commands->count && commands->items[k].Get("secondary"); ++k)
                {
                    if (!IsActionMethod(Str(commands->items[k].Get("method"))))
                        continue;
                    measured = k;
                    break;
                }
            }
            drawQuery = BeginDrawQuery(list, measured, frame, group.list, passIndex);
        }
        const bool issued = IssueCommand(i, m, c, args, list, group.list);
        if (drawQuery >= 0)
            EndDrawQuery(list, drawQuery);
        if (issued)
            _report->commandsRecorded++;
        if (m == "EndRenderPass")
        {
            if (pass.active && _options.compareTargets)
                InjectReadbacks(list, pass, readbacks);
            pass.active = false;
        }
        _arena.Reset();
    }
    ResolveListMeasurements(list);
    if (FAILED(list->Close()))
        Problem("command list " + std::to_string(group.list) + ": Close failed, so what it recorded is not valid");
    if (_x)
        _x->Block(DxExporter::Frame, "[" + std::to_string(group.last) + "]", [&](Source& s) { s.Line("DX_CHECK(" + listName + "->Close());"); });
}

void DxReplayer::InjectReadbacks(ID3D12GraphicsCommandList* list, const Pass& pass, std::vector<Readback>& readbacks)
{
    const JValue* textures = _capture->Textures();
    if (!textures || !textures->IsArray())
        return;
    for (uint32_t i = 0; i < textures->count; ++i)
    {
        const JValue& t = textures->items[i];
        const JValue* info = t.Get("info");
        if (!info || info->Get("kind"))
            continue;   // sampled textures are uploaded, not compared
        if (info->Get("commandBuffer")->Uint() != pass.list || info->Get("passIndex")->Uint() != pass.index || info->Get("frame")->Uint() != pass.frame)
            continue;
        DxTargetComparison cmp;
        cmp.resource = info->Get("id")->Uint();
        cmp.commandList = pass.list;
        cmp.frame = pass.frame;
        cmp.passIndex = pass.index;
        cmp.attachment = (uint32_t)info->Get("attachment")->Uint();
        cmp.format = Str(info->Get("format"));
        cmp.aspect = Str(info->Get("aspect"));
        cmp.width = (uint32_t)info->Get("width")->Uint();
        cmp.height = (uint32_t)info->Get("height")->Uint();
        const size_t target = _report->targets.size();
        auto skip = [&](const std::string& why) {
            cmp.note = why;
            _report->targets.push_back(cmp);
        };
        if (info->Get("error"))
        {
            cmp.undefined = true;
            skip("the capture's read-back failed: " + Str(info->Get("error")));
            continue;
        }
        Resource* r = ResourceOf(cmp.resource);
        if (!r)
        {
            skip("the target was not replayed");
            continue;
        }
        const uint8_t* captured = nullptr;
        size_t capturedSize = 0;
        if (!_capture->Payload(t.Get("payload"), captured, capturedSize) || !capturedSize)
        {
            skip("the capture has no pixels for this target");
            continue;
        }
        const PassTarget* bound = cmp.attachment < pass.targets.size() && pass.targets[cmp.attachment].resource == cmp.resource ? &pass.targets[cmp.attachment] : nullptr;
        if (bound && (cmp.aspect == "stencil" ? bound->stencilDiscarded : bound->discarded))
        {
            cmp.undefined = true;
            skip("the render pass ends by discarding it, so what it holds afterwards is undefined");
            continue;
        }
        const uint32_t mip = info->Get("mip") ? (uint32_t)info->Get("mip")->Uint() : bound ? bound->mip
                                                                                           : 0;
        const uint32_t slice = bound ? bound->firstSlice : 0;
        const uint32_t layers = std::max<uint32_t>(1, info->Get("layers") ? (uint32_t)info->Get("layers")->Uint() : 1);
        const uint32_t plane = cmp.aspect == "stencil" ? 1 : 0;
        const bool multisampled = r->desc.SampleDesc.Count > 1;
        if (multisampled && cmp.aspect != "color")
        {
            skip("a multisampled depth target is not read back");
            continue;
        }
        dxinsp::FormatInfo format = cmp.aspect == "depth" ? DepthPlaneFormat(r->desc.Format) : dxinsp::FormatOf(dxinsp::TypedFormat(r->desc.Format, false));
        if (cmp.aspect == "stencil")
        {
            format = dxinsp::FormatInfo{};
            format.bytes = 1;
        }
        if (!format.bytes)
        {
            skip("its format cannot be read back");
            continue;
        }

        // A multisampled target goes through a resolve into a texture of the replay's own, as the capture read it.
        Resource resolved;
        Resource* source = r;
        const DXGI_FORMAT resolveFormat = dxinsp::TypedFormat(r->desc.Format, false);
        if (multisampled)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = r->desc;
            desc.SampleDesc = {1, 0};
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            desc.MipLevels = 1;
            desc.DepthOrArraySize = 1;
            desc.Width = MipDim(r->desc.Width, mip);
            desc.Height = MipDim(r->desc.Height, mip);
            if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved.resource))))
            {
                skip("no memory to resolve the multisampled target into");
                continue;
            }
            _transients.push_back(resolved.resource);
            resolved.desc = desc;
            resolved.states.assign(1, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        }
        Readback rb;
        rb.target = target;
        rb.texture = &t;
        rb.rowBytes = dxinsp::RowBytes(format, MipDim(r->desc.Width, mip));
        rb.rows = dxinsp::RowCount(format, MipDim(r->desc.Height, mip));
        uint64_t total = 0;
        std::vector<uint32_t> subresources;
        for (uint32_t l = 0; l < layers; ++l)
        {
            const uint32_t sub = multisampled ? 0 : mip + (slice + l) * r->mips + plane * r->mips * r->slices;
            subresources.push_back(sub);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            UINT64 bytes = 0;
            _device->GetCopyableFootprints(&source->desc, multisampled ? 0 : sub, 1, total, &fp, nullptr, nullptr, &bytes);
            if (multisampled)
                _device->GetCopyableFootprints(&resolved.desc, 0, 1, total, &fp, nullptr, nullptr, &bytes);
            rb.footprints.push_back(fp);
            total = (fp.Offset + bytes + 511) & ~511ull;
            if (multisampled)
                break;
        }
        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = std::max<uint64_t>(total, 1);
        bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb.buffer))))
        {
            skip("no read-back memory");
            continue;
        }
        const uint32_t firstSub = multisampled ? mip + slice * r->mips : subresources[0];
        const D3D12_RESOURCE_STATES before = StateOf(*r, firstSub);
        if (multisampled)
        {
            Transition(list, *r, firstSub, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, nullptr);
            list->ResolveSubresource(resolved.resource, 0, r->resource, firstSub, resolveFormat);
            Transition(list, *r, firstSub, before, nullptr);
            Transition(list, resolved, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr);
            D3D12_TEXTURE_COPY_LOCATION src{resolved.resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            D3D12_TEXTURE_COPY_LOCATION dst{rb.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = rb.footprints[0];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        else
        {
            for (size_t k = 0; k < subresources.size(); ++k)
            {
                const D3D12_RESOURCE_STATES state = StateOf(*r, subresources[k]);
                Transition(list, *r, subresources[k], D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr);
                D3D12_TEXTURE_COPY_LOCATION src{r->resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                src.SubresourceIndex = subresources[k];
                D3D12_TEXTURE_COPY_LOCATION dst{rb.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint = rb.footprints[k];
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                Transition(list, *r, subresources[k], state, nullptr);
            }
        }
        if (_x)
        {
            const std::string name = "texture" + std::to_string(cmp.resource) + "_list" + std::to_string(pass.list) + "_pass" + std::to_string(pass.index) + "_att" +
                std::to_string(cmp.attachment) + (cmp.aspect == "color" ? "" : "_" + cmp.aspect);
            _x->Comment(DxExporter::Frame, "Read back here to compare with the capture's copy, taken at the same point of the frame.");
            _x->Block(DxExporter::Frame, "", [&](Source& s) {
                s.Line("ReadbackTexture(" + ListName(list) + ", " + s.Object(r->resource) + ", " + Source::String(name.c_str()) + ", " + std::to_string(firstSub) + ", " +
                    std::to_string(multisampled ? 1 : (uint32_t)subresources.size()) + ", " + StateText(before) + ", " + (multisampled ? FormatText(resolveFormat) : std::string("DXGI_FORMAT_UNKNOWN")) +
                    ", " + FormatText(cmp.aspect == "color" ? dxinsp::TypedFormat(r->desc.Format, false) : r->desc.Format) + ", " + (cmp.aspect == "color" ? "0" : cmp.aspect == "depth" ? "1"
                                                                                                                                                                                         : "2") +
                    ", " +
                    std::to_string(cmp.width) + ", " + std::to_string(cmp.height) + ", " + std::to_string(rb.rowBytes) + ", " + std::to_string(rb.rows) + ", " +
                    _x->Data(captured, capturedSize) + ", " + std::to_string(capturedSize) + ");");
            });
            _x->CountTarget();
        }
        _report->targets.push_back(cmp);
        readbacks.push_back(std::move(rb));
    }
}

void DxReplayer::CompareReadbacks(std::vector<Readback>& readbacks)
{
    for (Readback& rb : readbacks)
    {
        DxTargetComparison& cmp = _report->targets[rb.target];
        const uint8_t* captured = nullptr;
        size_t capturedSize = 0;
        _capture->Payload(rb.texture->Get("payload"), captured, capturedSize);
        void* mapped = nullptr;
        if (FAILED(rb.buffer->Map(0, nullptr, &mapped)) || !mapped)
        {
            cmp.note = "the read-back could not be mapped";
            rb.buffer->Release();
            continue;
        }
        // Rows out of the copy's pitch into the capture's tight layout.
        std::vector<uint8_t> replayed;
        for (const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp : rb.footprints)
            for (uint32_t row = 0; row < rb.rows; ++row)
            {
                const uint8_t* at = static_cast<const uint8_t*>(mapped) + fp.Offset + (uint64_t)row * fp.Footprint.RowPitch;
                replayed.insert(replayed.end(), at, at + rb.rowBytes);
            }
        const D3D12_RANGE none{0, 0};
        rb.buffer->Unmap(0, &none);
        rb.buffer->Release();
        const size_t size = std::min(capturedSize, replayed.size());
        const uint64_t texels = (uint64_t)cmp.width * cmp.height * rb.footprints.size();
        const uint32_t texel = texels ? std::max<uint32_t>(1, (uint32_t)(capturedSize / texels)) : 1;
        // A 24-bit depth plane is copied as 32 bits whose top byte is undefined.
        const bool d24 = cmp.aspect == "depth" && cmp.format.find("D24") != std::string::npos;
        const uint32_t comparedBytes = d24 ? 3 : texel;
        cmp.compared = true;
        cmp.texels = size / texel;
        for (size_t at = 0; at + texel <= size; at += texel)
        {
            bool differs = false;
            for (uint32_t k = 0; k < comparedBytes; ++k)
            {
                const uint32_t delta = (uint32_t)std::abs((int)captured[at + k] - (int)replayed[at + k]);
                if (delta)
                {
                    differs = true;
                    cmp.maxByteDelta = std::max(cmp.maxByteDelta, delta);
                }
            }
            if (differs)
                cmp.differingTexels++;
        }
        if (capturedSize != replayed.size())
            cmp.note = "sizes differ: captured " + std::to_string(capturedSize) + ", replayed " + std::to_string(replayed.size());
        // DXINSP_REPLAY_DUMP=<directory>: both sides of a target that differs, as raw bytes.
        if (const char* dump = std::getenv("DXINSP_REPLAY_DUMP"); dump && *dump && cmp.differingTexels)
        {
            const std::string stem = std::string(dump) + "/pass" + std::to_string(cmp.passIndex) + "_" + std::to_string(cmp.resource) + "_" + cmp.aspect;
            std::ofstream(stem + ".captured.bin", std::ios::binary).write(reinterpret_cast<const char*>(captured), (std::streamsize)size);
            std::ofstream(stem + ".replayed.bin", std::ios::binary).write(reinterpret_cast<const char*>(replayed.data()), (std::streamsize)size);
        }
        if (_options.keepPixels)
        {
            cmp.captured.assign(captured, captured + size);
            cmp.replayed = std::move(replayed);
        }
    }
    readbacks.clear();
}

void DxReplayer::ReplayCommands()
{
    const JValue* commands = _capture->Commands();
    if (!commands || !commands->IsArray())
        return;
    BuildGroups();
    for (uint32_t i = 0; i < commands->count; ++i)
    {
        if (_deviceLost)
            break;
        const JValue& c = commands->items[i];
        if (Str(c.Get("method")) != "ExecuteCommandLists")
            continue;
        const JValue* args = c.Get("args");
        auto queue = static_cast<ID3D12CommandQueue*>(Object(IdOf(c.Get("object"))));
        if (!queue)
        {
            Problem("submission " + std::to_string(i) + ": its queue was not replayed");
            continue;
        }
        if (_x)
        {
            _x->Blank(DxExporter::Frame);
            _x->Comment(DxExporter::Frame, "---- The submission at command " + std::to_string(i) + " (ExecuteCommandLists) ----");
        }
        std::vector<Readback> readbacks;
        std::vector<ID3D12CommandList*> lists;
        if (c.Get("heapDescriptors"))
        {
            Group submission;
            submission.first = submission.last = i;
            ApplyBufferData(submission);
        }
        const JValue* ids = args ? args->Get("ppCommandLists") : nullptr;
        for (uint32_t k = 0; ids && ids->IsArray() && k < ids->count; ++k)
        {
            const uint64_t id = IdOf(&ids->items[k]);
            auto group = std::find_if(_groups.begin(), _groups.end(), [&](const Group& g) { return g.list == id && !g.used && g.first > i; });
            auto list = static_cast<ID3D12GraphicsCommandList*>(Object(id));
            if (group != _groups.end() && !list)
                list = MissingList(id, queue->GetDesc().Type);
            if (group == _groups.end() || !list)
            {
                Problem("submission " + std::to_string(i) + ": command list " + std::to_string(id) + " has no recording in the capture (recorded before it started?)");
                if (_x)
                    _x->Comment(DxExporter::Frame, "command list " + std::to_string(id) + " is left out: the capture holds no recording of it");
                continue;
            }
            ApplyBufferData(*group);
            RecordGroup(*group, list, readbacks);
            lists.push_back(list);
        }
        if (lists.empty())
            continue;
        WriteHeapDescriptors(c, i);
        queue->ExecuteCommandLists((UINT)lists.size(), lists.data());
        // Inside a counter collection pass the wait belongs after the pass, not here (_inCounterRound).
        if (!_inCounterRound)
            WaitForQueue(queue);
        CompleteMeasurements(queue, !_deviceLost);
        _report->submissions++;
        if (_x)
        {
            _x->Block(DxExporter::Frame, "executed, and waited for", [&](Source& s) {
                std::string names;
                for (ID3D12CommandList* l : lists)
                    names += (names.empty() ? "" : ", ") + s.Object(l);
                const std::string local = s.Local("lists");
                s.Line("ID3D12CommandList* const " + local + "[] = {" + names + "};");
                s.Line("ExecuteAndWait(" + s.Object(queue) + ", " + local + ", " + std::to_string(lists.size()) + ");");
            });
            _x->Block(DxExporter::Frame, "", [&](Source& s) { s.Line("CompleteReadbacks();"); });
            _x->CountSubmission();
            _x->EndSubmission();
        }
        CompareReadbacks(readbacks);
        CollectMessages();
        // A resource still named by work in flight must outlive it: during a counter round the
        // frame's submissions have not been waited for yet, so its transients go at the end of it.
        if (!_inCounterRound)
        {
            for (ID3D12Resource* r : _transients)
                r->Release();
            _transients.clear();
        }
    }
    for (const Group& g : _groups)
        if (!g.used)
            Problem("command list " + std::to_string(g.list) + " was recorded but its submission is not in the capture");
}

bool DxReplayer::Run(const CaptureFile& capture, const DxReplayOptions& options, DxReplayReport& report)
{
    _capture = &capture;
    _options = options;
    _report = &report;
    if (Str(capture.Manifest().Get("api")) != "d3d12")
    {
        Problem("this is not a Direct3D 12 capture (api \"" + Str(capture.Manifest().Get("api")) + "\"): vkinsp_replay replays Vulkan ones");
        return false;
    }
    if (!options.exportDir.empty())
    {
        report.exported.requested = true;
        report.exported.directory = options.exportDir;
        _x = std::make_unique<DxExporter>(options.exportDir, capture);
        if (!_x->Open(report.exported.error))
        {
            Problem("export to C++: " + report.exported.error);
            _x.reset();
        }
    }
    g_currentStep = &_env.where;
    if (options.counters.enabled)
    {
        // Before the device: the SDK puts the driver into profiling mode, and a device created
        // before that has no profiling support (dx_counters.h, LoadDriver).
        std::string note;
        if (!nvperf::LoadDriver(note))
            report.counters.notes.push_back(note);
    }
    if (!CreateDevice())
        return false;
    if (const JValue* buffers = capture.Buffers(); buffers && buffers->IsArray())
        for (uint32_t i = 0; i < buffers->count; ++i)
            if (const JValue* info = buffers->items[i].Get("info"))
                _bufferData[info->Get("id")->Uint()] = &buffers->items[i];
    PrepareRaytracing();
    CreateObjects();
    ComputeInitialStates();
    UploadTextures();
    MoveToInitialStates();
    // Before anything traces: the structures an engine built at load (dx_raytracing.cpp).
    BuildEarlierStructures();
    CollectMessages();
    if (options.counters.enabled)
    {
        // Hardware counters are their own analysis: the frame is replayed once per collection pass
        // the counters need, and nothing else runs (dx_counters.cpp).
        _options.compareTargets = false;
        if (PrepareCounters())
        {
            if (options.counters.list)
            {
                ListCounters();
            }
            else
            {
                const uint32_t rounds = std::max(2u, CounterRounds());
                std::fprintf(stderr, "hardware counters: up to %u replays of the frame\n", rounds);
                for (uint32_t round = 0; round < rounds; ++round)
                {
                    if (round > 0)
                    {
                        // Each round runs the frame from where it started: the uploads again, and
                        // the resources back in the states the frame's first commands expect.
                        UploadTextures();
                        MoveToInitialStates();
                    }
                    if (!BeginCounterRound())
                        break;
                    std::fprintf(stderr, "  replay %u of at most %u\n", round + 1, rounds);
                    std::fflush(stderr);
                    ReplayCommands();
                    if (_deviceLost)
                        break;
                    if (!EndCounterRound())
                        break;
                }
                CompleteCounters();
            }
        }
        DestroyCounters();
        for (auto& p : _env.problems)
            report.problems.push_back(p);
        _env.problems.clear();
        CollectMessages();
        return true;
    }
    if (options.drawStats || options.ablation.enabled)
    {
        // An ablation issues a draw many times over, so what the targets hold afterwards is not the
        // capture's and is not compared. Queries around the draws change nothing they draw.
        if (options.ablation.enabled)
            _options.compareTargets = false;
        PrepareMeasurements();
    }
    ReplayCommands();
    DestroyMeasurements();
    if (!_deviceLost)
        EmitFrameEnd();
    for (auto& p : _env.problems)
        report.problems.push_back(p);
    _env.problems.clear();
    CollectMessages();
    if (_x)
    {
        if (!_x->Finish(report, report.exported))
            Problem("export to C++: " + report.exported.error);
        _x.reset();
    }
    return true;
}

} // namespace dxreplay
