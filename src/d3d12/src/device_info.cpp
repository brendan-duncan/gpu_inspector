// The adapter and the device as objects (device_info.h): the adapter's description, the device's
// feature level and CheckFeatureSupport results as an ObjectUpdate named "features", and the
// per-device frame timing that goes out as FrameStats.
#include "device_info.h"

#include "d3d12_enums.gen.h"
#include "cpu_timeline.h"
#include "hooks.h"
#include "json.h"
#include "serialize.h"
#include "tracker.h"
#include "transport.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dxinsp {

namespace {

using Clock = std::chrono::steady_clock;

struct DeviceRecord {
    /** The tracked adapter: the application's (no reference held) or one the library enumerated. */
    IDXGIAdapter* adapter = nullptr;
    bool ownsAdapter = false;

    // Frame timing, reported every 100 ms.
    uint64_t frame = 0;
    Clock::time_point lastPresent{};
    Clock::time_point lastReport{};
    double accumMs = 0;
    double minMs = 0;
    double maxMs = 0;
    uint64_t frames = 0;
    double submitMs = 0;
    /** The monitor's refresh period (0 when unknown), re-queried every 120 presents. */
    double displayRefreshMs = 0;
};

std::mutex g_mutex;
std::unordered_map<ID3D12Device*, std::unique_ptr<DeviceRecord>> g_devices;

/** The record of a device, made on first sight (a device seen only through Present still gets stats). Caller holds g_mutex. */
DeviceRecord& RecordOf(ID3D12Device* device) {
    auto& slot = g_devices[device];
    if (!slot) slot = std::make_unique<DeviceRecord>();
    return *slot;
}

// ---------------------------------------------------------------------------------------------
// The adapter

/**
 * The adapter the device was created on: the application's argument when it passed one, else
 * the one DXGI enumerates for the device's LUID. `owned` says whether the returned pointer
 * carries a reference of the library's (the enumerated case: nobody else keeps it alive).
 */
IDXGIAdapter* FindAdapter(IUnknown* argument, const LUID& luid, bool& owned) {
    owned = false;
    IDXGIAdapter* adapter = nullptr;
    if (argument && SUCCEEDED(argument->QueryInterface(IID_PPV_ARGS(&adapter))) && adapter) {
        // The application holds this adapter; the reference the query added is dropped so the
        // library changes nothing about its lifetime, and the pointer is the object's identity.
        adapter->Release();
        return adapter;
    }
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        Log("CreateDXGIFactory1 failed (%s): the device's adapter stays unknown", HrText(hr).c_str());
        return nullptr;
    }
    hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr) || !adapter) {
        Log("EnumAdapterByLuid failed (%s): the device's adapter stays unknown", HrText(hr).c_str());
        return nullptr;
    }
    owned = true;
    return adapter;
}

/** The adapter's args: {"Desc": DXGI_ADAPTER_DESC3 (or DESC1 on an older DXGI)}. */
std::string AdapterArgs(IDXGIAdapter* adapter, std::string* name) {
    Args args;
    ComPtr<IDXGIAdapter4> adapter4;
    DXGI_ADAPTER_DESC3 desc3{};
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(adapter4.put()))) && SUCCEEDED(adapter4->GetDesc3(&desc3))) {
        Write(args.key("Desc"), desc3);
        if (name) *name = Narrow(desc3.Description, wcsnlen(desc3.Description, 128));
        return args.str();
    }
    ComPtr<IDXGIAdapter1> adapter1;
    DXGI_ADAPTER_DESC1 desc1{};
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(adapter1.put()))) && SUCCEEDED(adapter1->GetDesc1(&desc1))) {
        Write(args.key("Desc"), desc1);
        if (name) *name = Narrow(desc1.Description, wcsnlen(desc1.Description, 128));
        return args.str();
    }
    args.null("Desc");
    return args.str();
}

// ---------------------------------------------------------------------------------------------
// What the device answers

/** The highest feature level the device supports, or `fallback` when the query fails. */
D3D_FEATURE_LEVEL HighestFeatureLevel(ID3D12Device* device, D3D_FEATURE_LEVEL fallback) {
    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    // A runtime older than a level in the list rejects the whole query, so the list is retried
    // from its next entry down.
    for (UINT first = 0; first < _countof(kLevels); ++first) {
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};
        levels.NumFeatureLevels = _countof(kLevels) - first;
        levels.pFeatureLevelsRequested = kLevels + first;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)))) {
            return levels.MaxSupportedFeatureLevel;
        }
    }
    return fallback;
}

/** The highest shader model the device supports: probed downwards, since an unknown model fails the query. */
D3D_SHADER_MODEL HighestShaderModel(ID3D12Device* device) {
    static const D3D_SHADER_MODEL kModels[] = {
        D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0,
        D3D_SHADER_MODEL_5_1,
    };
    for (D3D_SHADER_MODEL model : kModels) {
        D3D12_FEATURE_DATA_SHADER_MODEL data{model};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) return data.HighestShaderModel;
    }
    return D3D_SHADER_MODEL_5_1;
}

/** The highest root signature version the device supports. */
D3D_ROOT_SIGNATURE_VERSION HighestRootSignatureVersion(ID3D12Device* device) {
    static const D3D_ROOT_SIGNATURE_VERSION kVersions[] = {
        D3D_ROOT_SIGNATURE_VERSION_1_2, D3D_ROOT_SIGNATURE_VERSION_1_1, D3D_ROOT_SIGNATURE_VERSION_1_0,
    };
    for (D3D_ROOT_SIGNATURE_VERSION version : kVersions) {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE data{version};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &data, sizeof(data)))) return data.HighestVersion;
    }
    return D3D_ROOT_SIGNATURE_VERSION_1_0;
}

// The D3D12_OPTIONS structs, member by member. The same helpers serialize.cpp uses; the two
// structs the header declares writers for (OPTIONS and ARCHITECTURE1) live there.
#define M_UINT(m)      w.Key(#m); w.Uint((uint64_t)v.m)
#define M_BOOL(m)      w.Key(#m); w.Boolean(v.m != 0)
#define M_ENUM(m, E)   w.Key(#m); w.Enum(ToString_##E(EnumValue(v.m)), EnumValue(v.m))
#define M_FLAGS(m, E)  w.Key(#m); Flags_##E(w, (uint64_t)v.m)

// The writers below would otherwise hide serialize.h's (OPTIONS, ARCHITECTURE1) from this namespace.
using dxinsp::Write;

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS1& v) {
    w.BeginObject();
    M_BOOL(WaveOps);
    M_UINT(WaveLaneCountMin);
    M_UINT(WaveLaneCountMax);
    M_UINT(TotalLaneCount);
    M_BOOL(ExpandedComputeResourceStates);
    M_BOOL(Int64ShaderOps);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS2& v) {
    w.BeginObject();
    M_BOOL(DepthBoundsTestSupported);
    M_ENUM(ProgrammableSamplePositionsTier, D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS3& v) {
    w.BeginObject();
    M_BOOL(CopyQueueTimestampQueriesSupported);
    M_BOOL(CastingFullyTypedFormatSupported);
    M_FLAGS(WriteBufferImmediateSupportFlags, D3D12_COMMAND_LIST_SUPPORT_FLAGS);
    M_ENUM(ViewInstancingTier, D3D12_VIEW_INSTANCING_TIER);
    M_BOOL(BarycentricsSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS4& v) {
    w.BeginObject();
    M_BOOL(MSAA64KBAlignedTextureSupported);
    M_ENUM(SharedResourceCompatibilityTier, D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER);
    M_BOOL(Native16BitShaderOpsSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS5& v) {
    w.BeginObject();
    M_BOOL(SRVOnlyTiledResourceTier3);
    M_ENUM(RenderPassesTier, D3D12_RENDER_PASS_TIER);
    M_ENUM(RaytracingTier, D3D12_RAYTRACING_TIER);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS6& v) {
    w.BeginObject();
    M_BOOL(AdditionalShadingRatesSupported);
    M_BOOL(PerPrimitiveShadingRateSupportedWithViewportIndexing);
    M_ENUM(VariableShadingRateTier, D3D12_VARIABLE_SHADING_RATE_TIER);
    M_UINT(ShadingRateImageTileSize);
    M_BOOL(BackgroundProcessingSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS7& v) {
    w.BeginObject();
    M_ENUM(MeshShaderTier, D3D12_MESH_SHADER_TIER);
    M_ENUM(SamplerFeedbackTier, D3D12_SAMPLER_FEEDBACK_TIER);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS8& v) {
    w.BeginObject();
    M_BOOL(UnalignedBlockTexturesSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS9& v) {
    w.BeginObject();
    M_BOOL(MeshShaderPipelineStatsSupported);
    M_BOOL(MeshShaderSupportsFullRangeRenderTargetArrayIndex);
    M_BOOL(AtomicInt64OnTypedResourceSupported);
    M_BOOL(AtomicInt64OnGroupSharedSupported);
    M_BOOL(DerivativesInMeshAndAmplificationShadersSupported);
    M_ENUM(WaveMMATier, D3D12_WAVE_MMA_TIER);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS10& v) {
    w.BeginObject();
    M_BOOL(VariableRateShadingSumCombinerSupported);
    M_BOOL(MeshShaderPerPrimitiveShadingRateSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS11& v) {
    w.BeginObject();
    M_BOOL(AtomicInt64OnDescriptorHeapResourceSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS12& v) {
    w.BeginObject();
    M_ENUM(MSPrimitivesPipelineStatisticIncludesCulledPrimitives, D3D12_TRI_STATE);
    M_BOOL(EnhancedBarriersSupported);
    M_BOOL(RelaxedFormatCastingSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS13& v) {
    w.BeginObject();
    M_BOOL(UnrestrictedBufferTextureCopyPitchSupported);
    M_BOOL(UnrestrictedVertexElementAlignmentSupported);
    M_BOOL(InvertedViewportHeightFlipsYSupported);
    M_BOOL(InvertedViewportDepthFlipsZSupported);
    M_BOOL(TextureCopyBetweenDimensionsSupported);
    M_BOOL(AlphaBlendFactorSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS14& v) {
    w.BeginObject();
    M_BOOL(AdvancedTextureOpsSupported);
    M_BOOL(WriteableMSAATexturesSupported);
    M_BOOL(IndependentFrontAndBackStencilRefMaskSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS15& v) {
    w.BeginObject();
    M_BOOL(TriangleFanSupported);
    M_BOOL(DynamicIndexBufferStripCutSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS16& v) {
    w.BeginObject();
    M_BOOL(DynamicDepthBiasSupported);
    M_BOOL(GPUUploadHeapSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS17& v) {
    w.BeginObject();
    M_BOOL(NonNormalizedCoordinateSamplersSupported);
    M_BOOL(ManualWriteTrackingResourceSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS18& v) {
    w.BeginObject();
    M_BOOL(RenderPassesValid);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS19& v) {
    w.BeginObject();
    M_BOOL(MismatchingOutputDimensionsSupported);
    M_UINT(SupportedSampleCountsWithNoOutputs);
    M_BOOL(PointSamplingAddressesNeverRoundUp);
    M_BOOL(RasterizerDesc2Supported);
    M_BOOL(NarrowQuadrilateralLinesSupported);
    M_BOOL(AnisoFilterWithPointMipSupported);
    M_UINT(MaxSamplerDescriptorHeapSize);
    M_UINT(MaxSamplerDescriptorHeapSizeWithStaticSamplers);
    M_UINT(MaxViewDescriptorHeapSize);
    M_BOOL(ComputeOnlyCustomHeapSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS20& v) {
    w.BeginObject();
    M_BOOL(ComputeOnlyWriteWatchSupported);
    M_ENUM(RecreateAtTier, D3D12_RECREATE_AT_TIER);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_D3D12_OPTIONS21& v) {
    w.BeginObject();
    M_ENUM(WorkGraphsTier, D3D12_WORK_GRAPHS_TIER);
    M_ENUM(ExecuteIndirectTier, D3D12_EXECUTE_INDIRECT_TIER);
    M_BOOL(SampleCmpGradientAndBiasSupported);
    M_BOOL(ExtendedCommandInfoSupported);
    w.EndObject();
}

void Write(JsonWriter& w, const D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT& v) {
    w.BeginObject();
    M_UINT(MaxGPUVirtualAddressBitsPerResource);
    M_UINT(MaxGPUVirtualAddressBitsPerProcess);
    w.EndObject();
}

#undef M_UINT
#undef M_BOOL
#undef M_ENUM
#undef M_FLAGS

/**
 * The device's "features" update: {"action":"ObjectUpdate","id":N,"features":{"D3D12_OPTIONS":{...},
 * "D3D12_OPTIONS1":{...}, ..., "ARCHITECTURE1":{...}, "ROOT_SIGNATURE":{...}, "SHADER_MODEL":{...},
 * "GPU_VIRTUAL_ADDRESS_SUPPORT":{...}}}. An options struct the runtime does not answer (older
 * than it) is left out, so the list ends at the highest the runtime knows.
 */
std::string FeaturesMessage(ID3D12Device* device, uint64_t id) {
    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("features"); w.BeginObject();
#define OPTIONS(Name)                                                                                   \
    {                                                                                                   \
        D3D12_FEATURE_DATA_##Name v{};                                                                  \
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_##Name, &v, sizeof(v)))) {              \
            w.Key(#Name);                                                                               \
            Write(w, v);                                                                                \
        }                                                                                               \
    }
    OPTIONS(D3D12_OPTIONS)
    OPTIONS(D3D12_OPTIONS1)
    OPTIONS(D3D12_OPTIONS2)
    OPTIONS(D3D12_OPTIONS3)
    OPTIONS(D3D12_OPTIONS4)
    OPTIONS(D3D12_OPTIONS5)
    OPTIONS(D3D12_OPTIONS6)
    OPTIONS(D3D12_OPTIONS7)
    OPTIONS(D3D12_OPTIONS8)
    OPTIONS(D3D12_OPTIONS9)
    OPTIONS(D3D12_OPTIONS10)
    OPTIONS(D3D12_OPTIONS11)
    OPTIONS(D3D12_OPTIONS12)
    OPTIONS(D3D12_OPTIONS13)
    OPTIONS(D3D12_OPTIONS14)
    OPTIONS(D3D12_OPTIONS15)
    OPTIONS(D3D12_OPTIONS16)
    OPTIONS(D3D12_OPTIONS17)
    OPTIONS(D3D12_OPTIONS18)
    OPTIONS(D3D12_OPTIONS19)
    OPTIONS(D3D12_OPTIONS20)
    OPTIONS(D3D12_OPTIONS21)
    OPTIONS(ARCHITECTURE1)
    OPTIONS(GPU_VIRTUAL_ADDRESS_SUPPORT)
#undef OPTIONS
    {
        D3D_ROOT_SIGNATURE_VERSION version = HighestRootSignatureVersion(device);
        w.Key("ROOT_SIGNATURE"); w.BeginObject();
        w.Key("HighestVersion"); w.Enum(ToString_D3D_ROOT_SIGNATURE_VERSION(EnumValue(version)), EnumValue(version));
        w.EndObject();
    }
    {
        D3D_SHADER_MODEL model = HighestShaderModel(device);
        w.Key("SHADER_MODEL"); w.BeginObject();
        w.Key("HighestShaderModel"); w.Enum(ToString_D3D_SHADER_MODEL(EnumValue(model)), EnumValue(model));
        w.EndObject();
    }
    w.EndObject();
    w.EndObject();
    return std::move(w.str());
}

// ---------------------------------------------------------------------------------------------
// The monitor

/**
 * The refresh period of the monitor showing the swap chain's window (the primary monitor when
 * the swap chain has no window: a composition or CoreWindow target), in ms; 0 when unknown.
 */
double MonitorRefreshMs(IDXGISwapChain* swapChain) {
    ScopedInternal internal;
    HWND hwnd = nullptr;
    if (swapChain) {
        ComPtr<IDXGISwapChain1> swapChain1;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(swapChain1.put())))) {
            if (FAILED(swapChain1->GetHwnd(&hwnd))) hwnd = nullptr;
        }
        if (!hwnd) {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (SUCCEEDED(swapChain->GetDesc(&desc))) hwnd = desc.OutputWindow;
        }
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    BOOL ok = FALSE;
    if (hwnd) {
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (monitor && GetMonitorInfoW(monitor, &info)) ok = EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    }
    if (!ok) ok = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode);
    // 0 and 1 mean "the hardware default": unknown to us. The mode frequency is an integer;
    // the NTSC-derived rates are reported rounded down (59 for 59.94 Hz), as the Vulkan
    // layer's refresh_rate.cpp reads them.
    if (!ok || mode.dmDisplayFrequency < 2) return 0;
    double hz = (double)mode.dmDisplayFrequency;
    switch (mode.dmDisplayFrequency) {
        case 23: hz = 23.976; break;
        case 29: hz = 29.97; break;
        case 47: hz = 47.952; break;
        case 59: hz = 59.94; break;
        case 71: hz = 71.928; break;
        case 119: hz = 119.88; break;
        case 143: hz = 143.856; break;
        case 239: hz = 239.76; break;
        default: break;
    }
    return 1000.0 / hz;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

void RecordDeviceCreated(ID3D12Device* device, IUnknown* adapterArgument, D3D_FEATURE_LEVEL minimumFeatureLevel) {
    if (!device) return;
    {
        // D3D12CreateDevice hands out the existing device of an adapter the application still
        // holds one for: it is tracked already, with its adapter.
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end() && it->second->adapter) return;
    }

    // Everything asked of D3D12 and DXGI here is the library's own: the hooks forward it without
    // tracking. The tracker calls themselves come after the scope, since Track() ignores a call
    // made while Internal().
    IDXGIAdapter* adapter = nullptr;
    bool ownsAdapter = false;
    std::string adapterArgs, adapterName;
    LUID luid{};
    D3D_FEATURE_LEVEL featureLevel = minimumFeatureLevel;
    D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
    UINT nodeCount = 1;
    {
        ScopedInternal internal;
        luid = device->GetAdapterLuid();
        adapter = FindAdapter(adapterArgument, luid, ownsAdapter);
        if (adapter) adapterArgs = AdapterArgs(adapter, &adapterName);
        featureLevel = HighestFeatureLevel(device, minimumFeatureLevel);
        shaderModel = HighestShaderModel(device);
        nodeCount = device->GetNodeCount();
    }

    if (adapter) {
        HookAdapter(adapter);
        Tracker::Get().Track(adapter, "IDXGIAdapter", "EnumAdapters", nullptr, std::move(adapterArgs));
    }

    Args args;
    args.ref("pAdapter", adapter, "IDXGIAdapter");
    args.e("MinimumFeatureLevel", ToString_D3D_FEATURE_LEVEL(EnumValue(minimumFeatureLevel)), EnumValue(minimumFeatureLevel));
    args.e("featureLevel", ToString_D3D_FEATURE_LEVEL(EnumValue(featureLevel)), EnumValue(featureLevel));
    args.u("nodeCount", nodeCount);
    args.e("highestShaderModel", ToString_D3D_SHADER_MODEL(EnumValue(shaderModel)), EnumValue(shaderModel));
    Write(args.key("adapterLuid"), luid);
    uint64_t deviceId = Tracker::Get().Track(device, "ID3D12Device", "D3D12CreateDevice", adapter, args.str());

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        DeviceRecord& record = RecordOf(device);
        if (record.ownsAdapter && record.adapter && record.adapter != adapter) {
            ScopedInternal internal;
            record.adapter->Release();
        }
        record.adapter = adapter;
        record.ownsAdapter = ownsAdapter;
    }

    if (deviceId) {
        std::string message;
        {
            ScopedInternal internal;
            message = FeaturesMessage(device, deviceId);
        }
        Tracker::Get().UpdateById(deviceId, "features", message);
        // The adapter's memory segments, for the memory view (cpu_timeline.h).
        SendMemoryProperties(device, adapter);
    }
    const char* levelName = ToString_D3D_FEATURE_LEVEL(EnumValue(featureLevel));
    const char* modelName = ToString_D3D_SHADER_MODEL(EnumValue(shaderModel));
    Log("device %llu on \"%s\": %s, %s, %u node(s)", (unsigned long long)deviceId, adapterName.c_str(),
        levelName ? levelName : "?", modelName ? modelName : "?", nodeCount);
}

IDXGIAdapter* AdapterOf(ID3D12Device* device) {
    IDXGIAdapter* adapter = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(device);
        if (it == g_devices.end()) return nullptr;
        adapter = it->second->adapter;
    }
    // The application's adapter may have been released since (its Release hook untracked it):
    // a pointer the tracker no longer knows is not handed out.
    if (adapter && !Tracker::Get().IdOf(adapter)) return nullptr;
    return adapter;
}

void OnDeviceReleased(ID3D12Device* device) {
    std::unique_ptr<DeviceRecord> record;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(device);
        if (it == g_devices.end()) return;
        record = std::move(it->second);
        g_devices.erase(it);
    }
    if (record->ownsAdapter && record->adapter) {
        ScopedInternal internal;
        record->adapter->Release();
    }
}

// ---------------------------------------------------------------------------------------------
// Frame timing

// The frame timing of one boundary, accumulated and reported every 100 ms. `boundary` is
// "present" or "submit"; `presentMode` and the refresh period are the present path's, empty and 0
// for a submit boundary (a device that never presents has no display period). Caller holds g_mutex.
bool EmitBoundary(DeviceRecord& d, Clock::time_point now, const char* boundary, double refreshMs, const std::string& presentMode) {
    d.frame++;
    if (d.lastPresent.time_since_epoch().count() == 0) {
        d.lastReport = now;
        d.lastPresent = now;
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(now - d.lastPresent).count();
    if (d.frames == 0) {
        d.minMs = ms;
        d.maxMs = ms;
    } else {
        if (ms < d.minMs) d.minMs = ms;
        if (ms > d.maxMs) d.maxMs = ms;
    }
    d.accumMs += ms;
    d.frames++;
    const double sinceReport = std::chrono::duration<double, std::milli>(now - d.lastReport).count();
    bool reported = false;
    if (sinceReport >= 100.0) {
        reported = true;
        if (Transport::Get().Connected()) {
            JsonWriter w;
            w.BeginObject();
            w.Key("action"); w.String("FrameStats");
            w.Key("frame"); w.Uint(d.frame);
            w.Key("frameTimeMs"); w.Double(d.accumMs / (double)d.frames);
            w.Key("minMs"); w.Double(d.minMs);
            w.Key("maxMs"); w.Double(d.maxMs);
            w.Key("frames"); w.Uint(d.frames);
            w.Key("submitMs"); w.Double(d.submitMs / (double)d.frames);
            w.Key("refreshMs"); w.Double(refreshMs);
            w.Key("refreshSource"); w.String(refreshMs > 0 ? "monitor" : "");
            w.Key("displayRefreshMs"); w.Double(refreshMs > 0 ? d.displayRefreshMs : 0);
            if (!presentMode.empty()) { w.Key("presentMode"); w.String(presentMode); }
            w.Key("frameBoundary"); w.String(boundary);
            // Dropped frames need a refresh count the swap chain does not give; the UI shows the
            // zeros as sent.
            w.Key("dropped"); w.Uint(0);
            w.Key("droppedTotal"); w.Uint(0);
            w.EndObject();
            Transport::Get().SendJson(std::move(w.str()));
        }
        // Reset whether or not anything was sent, so the first report after a connect covers its
        // own interval rather than everything since the last one.
        d.accumMs = 0;
        d.frames = 0;
        d.submitMs = 0;
        d.lastReport = now;
    }
    d.lastPresent = now;
    return reported;
}

void OnFramePresented(ID3D12Device* device, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, HRESULT) {
    // A failed present is still a frame: the application paced itself to it.
    const Clock::time_point now = Clock::now();
    std::unique_lock<std::mutex> lock(g_mutex);
    DeviceRecord& d = RecordOf(device);
    // The display can change (a window moved to another monitor, a mode switch): re-queried at the
    // first present and every 120 after.
    if (d.frame % 120 == 0) {
        double ms = MonitorRefreshMs(swapChain);
        if (ms > 0 && ms != d.displayRefreshMs) {
            d.displayRefreshMs = ms;
            Log("refresh rate: %.4g Hz (%.3f ms, monitor)", 1000.0 / ms, ms);
        }
    }
    // A present that syncs waits for the display: its period is the frame's floor. Tearing
    // presents and syncInterval 0 do not, so no refresh period applies.
    const bool synced = syncInterval > 0 && !(flags & DXGI_PRESENT_ALLOW_TEARING);
    std::string presentMode = "immediate";
    if (syncInterval == 1) presentMode = "vsync";
    else if (syncInterval > 1) presentMode = "vsync/" + std::to_string(syncInterval);
    bool reported = false;
    {
        // EmitBoundary needs the record under the lock; SendMemoryBudget asks for the adapter,
        // which takes the same lock, so it runs after this scope rather than inside it.
        reported = EmitBoundary(d, now, "present", synced ? d.displayRefreshMs : 0, presentMode);
    }
    lock.unlock();
    if (reported) { SendMemoryBudget(device); SendMemorySample(device); }
}

void OnFrameNoPresent(ID3D12Device* device) {
    // A device that never presents (Dawn in Chrome): its frame time is the wall-clock interval
    // between the submit boundaries, with no display period and no present mode.
    const Clock::time_point now = Clock::now();
    std::unique_lock<std::mutex> lock(g_mutex);
    const bool reported = EmitBoundary(RecordOf(device), now, "submit", 0, std::string());
    lock.unlock();
    if (reported) { SendMemoryBudget(device); SendMemorySample(device); }
}

void AddSubmitTime(ID3D12Device* device, double milliseconds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    RecordOf(device).submitMs += milliseconds;
}

}  // namespace dxinsp
