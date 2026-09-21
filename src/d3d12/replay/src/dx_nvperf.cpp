// NVIDIA's Nsight Perf SDK behind dx_counters.h: the SDK's range profiler, which collects the GPU's
// own counters between ranges pushed into command lists, replaying the frame once per collection
// pass, and its metrics evaluator, which turns raw counters into named metrics. The Vulkan replay
// does the same thing through the SDK's own `RangeProfilerVulkan` (src/replay/src/nvperf.cpp).
//
// The SDK's redistributable utility layer ships a Vulkan range profiler but no Direct3D 12 one, so
// the state machine's IProfilerApi is implemented here over the NVPW_D3D12_* entry points --
// which mirror the Vulkan family call for call (CommandList_PushRange for CommandBuffer_PushRange,
// and so on). Everything above that, the metrics evaluator and the configuration builder, is the
// SDK's own and API-neutral.
//
// The host library itself is loaded at run time by nvperf_host_impl.h, which this file compiles
// once for the whole program.
#include "dx_counters.h"

#ifdef DXINSP_NVPERF

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include "windows-desktop-x64/nvperf_host_impl.h"
#endif

#include "NvPerfCounterConfiguration.h"
#include "NvPerfCounterData.h"
#include "NvPerfDeviceProperties.h"
#include "NvPerfInit.h"
#include "NvPerfMetricsConfigBuilder.h"
#include "NvPerfMetricsEvaluator.h"
#include "NvPerfRangeProfiler.h"

#include "nvperf_d3d12_host.h"
#include "nvperf_d3d12_target.h"

namespace dxreplay {
namespace nvperf {

namespace {

bool g_loaded = false;
bool g_tried = false;
std::string g_loadNote;
std::string g_libraryPath;
/** What the SDK logged since the last call that asked, for the notes. */
std::string g_log;

void CaptureLog(const char* prefix, const char*, const char*, const char* function, const char* message, void*) {
    std::string line = std::string(prefix ? prefix : "") + (function ? function : "") + ": " + (message ? message : "");
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (g_log.size() < 4000) g_log += (g_log.empty() ? "" : "; ") + line;
}

/** The last thing the SDK logged, cleared; a fallback message when it logged nothing. */
std::string TakeLog(const char* fallback) {
    std::string out = g_log.empty() ? fallback : g_log;
    g_log.clear();
    return out;
}

constexpr const char* kLibraryName = "nvperf_grfx_host.dll";

/** Directories that hold the host library, in the order to try them (nvperf.cpp's list). */
std::vector<std::string> SearchDirectories() {
    namespace fs = std::filesystem;
    std::vector<std::string> dirs;
    auto add = [&](const fs::path& dir) {
        std::error_code ec;
        if (!fs::exists(dir / kLibraryName, ec)) return;
        const std::string s = dir.string();
        if (std::find(dirs.begin(), dirs.end(), s) == dirs.end()) dirs.push_back(s);
    };
    std::error_code ec;
    fs::path exe;
    wchar_t buffer[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, 4096);
    if (n > 0 && n < 4096) exe = fs::path(buffer);
    if (!exe.empty()) {
        // Beside the tool, and a plugins/nv directory next to it (where RenderDoc keeps its copy).
        add(exe.parent_path());
        add(exe.parent_path() / "plugins" / "nv");
    }
    if (const char* env = std::getenv("DXINSP_NVPERF_DIR"); env && *env) add(fs::path(env));
    if (const char* env = std::getenv("VKINSP_NVPERF_DIR"); env && *env) add(fs::path(env));
    const char* programFiles = std::getenv("ProgramFiles");
    const fs::path root = fs::path(programFiles && *programFiles ? programFiles : "C:\\Program Files") / "NVIDIA Corporation";
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    if (fs::is_directory(root, ec)) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->path().filename() == kLibraryName) found.push_back({fs::last_write_time(it->path(), ec), it->path().parent_path()});
            if (it.depth() > 4) it.disable_recursion_pending();
        }
    }
    // The newest copy first: the SDK's library must be at least as new as the headers.
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& f : found) add(f.second);
    return dirs;
}

/** The unit a metric's dimensions come to: what the app formats the value by (nvperf.cpp's table). */
std::string UnitOf(const std::vector<NVPW_DimUnitFactor>& dims) {
    if (dims.empty()) return "ratio";
    if (dims.size() == 1 && dims[0].exponent == 1) {
        switch (dims[0].dimUnit) {
            case NVPW_DIM_UNIT_PERCENT: return "percent";
            case NVPW_DIM_UNIT_NANOSECONDS: return "ns";
            case NVPW_DIM_UNIT_BYTES: return "bytes";
            case NVPW_DIM_UNIT_KILOBYTES: return "kbytes";
            case NVPW_DIM_UNIT_UNITLESS: return "ratio";
            case NVPW_DIM_UNIT_DRAM_CYCLES:
            case NVPW_DIM_UNIT_FBP_CYCLES:
            case NVPW_DIM_UNIT_GPC_CYCLES:
            case NVPW_DIM_UNIT_SYS_CYCLES:
            case NVPW_DIM_UNIT_PCIE_CYCLES: return "cycles";
            default: return "count";
        }
    }
    if (dims.size() == 2 && dims[0].dimUnit == NVPW_DIM_UNIT_BYTES && dims[1].exponent == -1) return "bytes/s";
    return "count";
}

// ---------------------------------------------------------------------------------------------
// The Direct3D 12 range profiler
//
// What NvPerfRangeProfilerVulkan.h is for Vulkan: the SDK's RangeProfilerStateMachine driven
// through the NVPW_D3D12_* entry points. The state machine owns the pass and decode bookkeeping;
// this supplies the seven calls it makes.

using nv::perf::profiler::RangeProfilerStateMachine;
using nv::perf::profiler::SessionOptions;
using nv::perf::profiler::SetConfigParams;

class RangeProfilerD3D12 {
public:
    RangeProfilerD3D12() : _stateMachine(_api) {}
    ~RangeProfilerD3D12() { EndSession(); }

    bool BeginSession(ID3D12Device* device, ID3D12CommandQueue* queue, const SessionOptions& options) {
        _api.queue = queue;
        _api.device = device;
        _api.sessionOptions = options;

        NVPW_D3D12_Profiler_CalcTraceBufferSize_Params traceParams{NVPW_D3D12_Profiler_CalcTraceBufferSize_Params_STRUCT_SIZE};
        traceParams.maxRangesPerPass = options.maxNumRanges;
        traceParams.avgRangeNameLength = options.avgRangeNameLength;
        if (NVPW_D3D12_Profiler_CalcTraceBufferSize(&traceParams) != NVPA_STATUS_SUCCESS) return false;

        NVPW_D3D12_Profiler_Queue_BeginSession_Params beginParams{NVPW_D3D12_Profiler_Queue_BeginSession_Params_STRUCT_SIZE};
        beginParams.pCommandQueue = queue;
        beginParams.numTraceBuffers = options.numTraceBuffers;
        beginParams.traceBufferSize = traceParams.traceBufferSize;
        beginParams.maxRangesPerPass = options.maxNumRanges;
        beginParams.maxLaunchesPerPass = options.maxNumRanges;
        if (NVPW_D3D12_Profiler_Queue_BeginSession(&beginParams) != NVPA_STATUS_SUCCESS) return false;
        _inSession = true;
        return true;
    }

    void EndSession() {
        if (!_inSession) return;
        _stateMachine.Reset();
        NVPW_D3D12_Profiler_Queue_EndSession_Params params{NVPW_D3D12_Profiler_Queue_EndSession_Params_STRUCT_SIZE};
        params.pCommandQueue = _api.queue;
        params.timeout = 0xFFFFFFFF;
        NVPW_D3D12_Profiler_Queue_EndSession(&params);
        _inSession = false;
    }

    bool EnqueueCounterCollection(const nv::perf::CounterConfiguration& configuration, uint16_t numNestingLevels,
                                  size_t numStatisticalSamples) {
        return _stateMachine.EnqueueCounterCollection(SetConfigParams(configuration, numNestingLevels, numStatisticalSamples));
    }

    bool BeginPass() { return _stateMachine.BeginPass(); }
    bool EndPass() { return _stateMachine.EndPass(); }
    bool DecodeCounters(nv::perf::profiler::DecodeResult& result) { return _stateMachine.DecodeCounters(result); }
    bool AllPassesSubmitted() const { return _stateMachine.AllPassesSubmitted(); }

    /** Counter availability for this queue, so a metric needing a counter it cannot collect is left out. */
    bool CounterAvailability(std::vector<uint8_t>& image) const {
        NVPW_D3D12_Profiler_Queue_GetCounterAvailability_Params params{NVPW_D3D12_Profiler_Queue_GetCounterAvailability_Params_STRUCT_SIZE};
        params.pCommandQueue = _api.queue;
        if (NVPW_D3D12_Profiler_Queue_GetCounterAvailability(&params) != NVPA_STATUS_SUCCESS || !params.counterAvailabilityImageSize)
            return false;
        image.resize(params.counterAvailabilityImageSize);
        params.pCounterAvailabilityImage = image.data();
        return NVPW_D3D12_Profiler_Queue_GetCounterAvailability(&params) == NVPA_STATUS_SUCCESS;
    }

private:
    struct ProfilerApi : RangeProfilerStateMachine::IProfilerApi {
        ID3D12CommandQueue* queue = nullptr;
        ID3D12Device* device = nullptr;
        SessionOptions sessionOptions;

        bool CreateCounterData(const SetConfigParams& config, std::vector<uint8_t>& counterDataImage,
                               std::vector<uint8_t>& counterDataScratch) const override {
            NVPW_D3D12_Profiler_CounterDataImageOptions options{NVPW_D3D12_Profiler_CounterDataImageOptions_STRUCT_SIZE};
            options.pCounterDataPrefix = config.pCounterDataPrefix;
            options.counterDataPrefixSize = config.counterDataPrefixSize;
            options.maxNumRanges = (uint32_t)sessionOptions.maxNumRanges;
            options.maxNumRangeTreeNodes = (uint32_t)(2 * sessionOptions.maxNumRanges);
            options.maxRangeNameLength = (uint32_t)sessionOptions.avgRangeNameLength;

            NVPW_D3D12_Profiler_CounterDataImage_CalculateSize_Params sizeParams{
                NVPW_D3D12_Profiler_CounterDataImage_CalculateSize_Params_STRUCT_SIZE};
            sizeParams.pOptions = &options;
            sizeParams.counterDataImageOptionsSize = NVPW_D3D12_Profiler_CounterDataImageOptions_STRUCT_SIZE;
            if (NVPW_D3D12_Profiler_CounterDataImage_CalculateSize(&sizeParams) != NVPA_STATUS_SUCCESS) return false;
            counterDataImage.resize(sizeParams.counterDataImageSize);

            NVPW_D3D12_Profiler_CounterDataImage_Initialize_Params initParams{
                NVPW_D3D12_Profiler_CounterDataImage_Initialize_Params_STRUCT_SIZE};
            initParams.counterDataImageOptionsSize = NVPW_D3D12_Profiler_CounterDataImageOptions_STRUCT_SIZE;
            initParams.pOptions = &options;
            initParams.counterDataImageSize = counterDataImage.size();
            initParams.pCounterDataImage = counterDataImage.data();
            if (NVPW_D3D12_Profiler_CounterDataImage_Initialize(&initParams) != NVPA_STATUS_SUCCESS) return false;

            NVPW_D3D12_Profiler_CounterDataImage_CalculateScratchBufferSize_Params scratchParams{
                NVPW_D3D12_Profiler_CounterDataImage_CalculateScratchBufferSize_Params_STRUCT_SIZE};
            scratchParams.counterDataImageSize = counterDataImage.size();
            scratchParams.pCounterDataImage = counterDataImage.data();
            if (NVPW_D3D12_Profiler_CounterDataImage_CalculateScratchBufferSize(&scratchParams) != NVPA_STATUS_SUCCESS) return false;
            counterDataScratch.resize(scratchParams.counterDataScratchBufferSize);

            NVPW_D3D12_Profiler_CounterDataImage_InitializeScratchBuffer_Params initScratch{
                NVPW_D3D12_Profiler_CounterDataImage_InitializeScratchBuffer_Params_STRUCT_SIZE};
            initScratch.counterDataImageSize = counterDataImage.size();
            initScratch.pCounterDataImage = counterDataImage.data();
            initScratch.counterDataScratchBufferSize = counterDataScratch.size();
            initScratch.pCounterDataScratchBuffer = counterDataScratch.data();
            return NVPW_D3D12_Profiler_CounterDataImage_InitializeScratchBuffer(&initScratch) == NVPA_STATUS_SUCCESS;
        }

        bool SetConfig(const SetConfigParams& config) const override {
            NVPW_D3D12_Profiler_Queue_SetConfig_Params params{NVPW_D3D12_Profiler_Queue_SetConfig_Params_STRUCT_SIZE};
            params.pCommandQueue = queue;
            params.pConfig = config.pConfigImage;
            params.configSize = config.configImageSize;
            params.minNestingLevel = 1;
            params.numNestingLevels = config.numNestingLevels;
            params.passIndex = 0;
            params.targetNestingLevel = params.minNestingLevel;
            return NVPW_D3D12_Profiler_Queue_SetConfig(&params) == NVPA_STATUS_SUCCESS;
        }

        bool BeginPass() const override {
            NVPW_D3D12_Profiler_Queue_BeginPass_Params params{NVPW_D3D12_Profiler_Queue_BeginPass_Params_STRUCT_SIZE};
            params.pCommandQueue = queue;
            return NVPW_D3D12_Profiler_Queue_BeginPass(&params) == NVPA_STATUS_SUCCESS;
        }

        bool EndPass() const override {
            NVPW_D3D12_Profiler_Queue_EndPass_Params params{NVPW_D3D12_Profiler_Queue_EndPass_Params_STRUCT_SIZE};
            params.pCommandQueue = queue;
            return NVPW_D3D12_Profiler_Queue_EndPass(&params) == NVPA_STATUS_SUCCESS;
        }

        /**
         * The ranges of this replay are pushed into command lists, not onto the queue
         * (Session::PushRange), so the state machine's own queue-level push and pop do nothing but
         * succeed -- the same arrangement the Vulkan profiler uses for command-buffer ranges.
         */
        bool PushRange(const char*) override { return true; }
        bool PopRange() override { return true; }

        bool DecodeCounters(std::vector<uint8_t>& counterDataImage, std::vector<uint8_t>& counterDataScratch,
                            bool& onePassDecoded, bool& allPassesDecoded) const override {
            NVPW_D3D12_Profiler_Queue_DecodeCounters_Params params{NVPW_D3D12_Profiler_Queue_DecodeCounters_Params_STRUCT_SIZE};
            params.pCommandQueue = queue;
            params.counterDataImageSize = counterDataImage.size();
            params.pCounterDataImage = counterDataImage.data();
            params.counterDataScratchBufferSize = counterDataScratch.size();
            params.pCounterDataScratchBuffer = counterDataScratch.data();
            if (NVPW_D3D12_Profiler_Queue_DecodeCounters(&params) != NVPA_STATUS_SUCCESS) return false;
            if (params.numRangesDropped)
                NV_PERF_LOG_WRN(50, "%llu ranges were dropped: the session's maxNumRanges is too small\n",
                                (unsigned long long)params.numRangesDropped);
            onePassDecoded = !!params.onePassCollected;
            allPassesDecoded = !!params.allPassesCollected;
            return true;
        }
    };

    ProfilerApi _api;
    RangeProfilerStateMachine _stateMachine;
    bool _inSession = false;
};

} // namespace

bool Load(std::string& note) {
    if (g_tried) {
        note = g_loadNote;
        return g_loaded;
    }
    g_tried = true;
    nv::perf::UserLogEnableStderr(false);
    nv::perf::UserLogEnableCustom(CaptureLog, nullptr);
    const std::vector<std::string> dirs = SearchDirectories();
    if (dirs.empty()) {
        g_loadNote = std::string("NVIDIA's Nsight Perf SDK library (") + kLibraryName + ") was not found: put it beside dxinsp_replay, "
                     "name its directory in DXINSP_NVPERF_DIR, or install Nsight Graphics (https://developer.nvidia.com/nsight-perf-sdk)";
        note = g_loadNote;
        return false;
    }
    std::vector<const char*> paths;
    for (const std::string& d : dirs) paths.push_back(d.c_str());
    NVPW_SetLibraryLoadPaths_Params params{NVPW_SetLibraryLoadPaths_Params_STRUCT_SIZE};
    params.numPaths = paths.size();
    params.ppPaths = paths.data();
    NVPW_SetLibraryLoadPaths(&params);
    if (!nv::perf::InitializeNvPerf()) {
        g_loadNote = "NVIDIA's Nsight Perf SDK library in " + dirs.front() + " could not be initialized: " + TakeLog("no details");
        note = g_loadNote;
        return false;
    }
    if (!NVPA_GetProcAddress("NVPW_D3D12_RawCounterConfig_Create")) {
        g_loadNote = "NVIDIA's Nsight Perf SDK library in " + dirs.front() + " is older than the replay's headers; a newer Nsight has one that works";
        note = g_loadNote;
        return false;
    }
    g_libraryPath = dirs.front();
    g_loaded = true;
    g_log.clear();
    return true;
}

std::string LibraryPath() { return g_libraryPath; }

bool ProfilingPermitted(std::string& note) {
    DWORD adminOnly = 1;   // the driver's default
    DWORD size = sizeof(adminOnly);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NVTweak", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD type = 0;
        if (RegQueryValueExW(key, L"RmProfilingAdminOnly", nullptr, &type, (LPBYTE)&adminOnly, &size) != ERROR_SUCCESS || type != REG_DWORD)
            adminOnly = 1;
        RegCloseKey(key);
    }
    if (!adminOnly) return true;

    // Admin-only: this process must be elevated.
    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD returned = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned))
            elevated = elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    if (elevated) return true;
    note = "this machine restricts GPU performance counters to administrators, and the replay is not elevated: "
           "allow them for all users in the NVIDIA Control Panel (Developer > Manage GPU Performance Counters), "
           "or run the replay as administrator. Without that the driver accepts the profiling session and then "
           "never finishes the first profiled submission";
    return false;
}

bool LoadDriver(std::string& note) {
    if (!Load(note)) return false;
    NVPW_D3D12_LoadDriver_Params params{NVPW_D3D12_LoadDriver_Params_STRUCT_SIZE};
    if (NVPW_D3D12_LoadDriver(&params) != NVPA_STATUS_SUCCESS) {
        note = "the Nsight Perf SDK could not put the Direct3D 12 driver into profiling mode: " + TakeLog("no details");
        return false;
    }
    g_log.clear();
    return true;
}

struct Session::Impl {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    size_t deviceIndex = 0;
    std::string chip;
    nv::perf::MetricsEvaluator evaluator;
    RangeProfilerD3D12 profiler;
    bool inSession = false;
    nv::perf::CounterConfiguration configuration;
    std::vector<NVPW_MetricEvalRequest> requests;
    std::vector<uint8_t> counterData;

    /** A metric's description, hardware unit and unit, by its evaluation request. */
    DxCounterInfo Describe(const std::string& name, const NVPW_MetricEvalRequest& request) {
        DxCounterInfo info;
        info.name = name;
        const auto type = (NVPW_MetricType)request.metricType;
        if (const char* d = nv::perf::GetMetricDescription(evaluator, type, request.metricIndex)) info.description = d;
        if (const char* u = nv::perf::ToCString(evaluator, nv::perf::GetMetricHwUnit(evaluator, type, request.metricIndex))) info.category = u;
        std::vector<NVPW_DimUnitFactor> dims;
        if (nv::perf::GetMetricDimUnits(evaluator, request, dims)) info.unit = UnitOf(dims);
        else info.unit = "count";
        return info;
    }
};

Session::Session() : _impl(new Impl) {}

Session::~Session() {
    End();
    delete _impl;
}

const std::string& Session::Chip() const { return _impl->chip; }

bool Session::Init(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& note) {
    if (!g_loaded) {
        note = g_loadNote.empty() ? "NVIDIA's Nsight Perf SDK is not loaded" : g_loadNote;
        return false;
    }
    Impl& s = *_impl;
    s.device = device;
    s.queue = queue;
    g_log.clear();
    {
        NVPW_D3D12_Device_GetDeviceIndex_Params params{NVPW_D3D12_Device_GetDeviceIndex_Params_STRUCT_SIZE};
        params.pDevice = device;
        params.sliIndex = 0;
        if (NVPW_D3D12_Device_GetDeviceIndex(&params) != NVPA_STATUS_SUCCESS) {
            note = "the Nsight Perf SDK does not know this device (it profiles NVIDIA GPUs): " + TakeLog("no details");
            return false;
        }
        s.deviceIndex = params.deviceIndex;
    }
    {
        NVPW_D3D12_Profiler_IsGpuSupported_Params params{NVPW_D3D12_Profiler_IsGpuSupported_Params_STRUCT_SIZE};
        params.deviceIndex = s.deviceIndex;
        if (NVPW_D3D12_Profiler_IsGpuSupported(&params) != NVPA_STATUS_SUCCESS || !params.isSupported) {
            note = "the Nsight Perf SDK does not profile this GPU: " + TakeLog("no details");
            return false;
        }
    }
    const nv::perf::DeviceIdentifiers ids = nv::perf::GetDeviceIdentifiers(s.deviceIndex);
    if (!ids.pChipName) {
        note = "the Nsight Perf SDK could not name this GPU's chip: " + TakeLog("no details");
        return false;
    }
    s.chip = ids.pChipName;
    size_t scratch = 0;
    {
        NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize_Params params{
            NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize_Params_STRUCT_SIZE};
        params.pChipName = s.chip.c_str();
        if (NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize(&params) != NVPA_STATUS_SUCCESS || !params.scratchBufferSize) {
            note = "the Nsight Perf SDK has no metrics for chip " + s.chip + ": " + TakeLog("no details");
            return false;
        }
        scratch = params.scratchBufferSize;
    }
    std::vector<uint8_t> buffer(scratch);
    NVPW_MetricsEvaluator* evaluator = nullptr;
    {
        NVPW_D3D12_MetricsEvaluator_Initialize_Params params{NVPW_D3D12_MetricsEvaluator_Initialize_Params_STRUCT_SIZE};
        params.pScratchBuffer = buffer.data();
        params.scratchBufferSize = buffer.size();
        params.pChipName = s.chip.c_str();
        if (NVPW_D3D12_MetricsEvaluator_Initialize(&params) == NVPA_STATUS_SUCCESS) evaluator = params.pMetricsEvaluator;
    }
    if (!evaluator) {
        note = "the Nsight Perf SDK could not create a metrics evaluator for chip " + s.chip + ": " + TakeLog("no details");
        return false;
    }
    s.evaluator = nv::perf::MetricsEvaluator(evaluator, std::move(buffer));
    return true;
}

bool Session::Begin(uint32_t maxRanges, std::string& note) {
    Impl& s = *_impl;
    if (!s.evaluator.Get()) {
        note = "the Nsight Perf SDK session was not initialized";
        return false;
    }
    g_log.clear();
    SessionOptions options;
    options.maxNumRanges = std::max<size_t>(maxRanges, 16);
    options.avgRangeNameLength = 16;
    options.numTraceBuffers = 1;
    if (!s.profiler.BeginSession(s.device, s.queue, options)) {
        note = "the Nsight Perf SDK could not start a profiling session: " + TakeLog("no details")
             + " (on Windows, allow GPU performance counters for all users in the NVIDIA Control Panel, Developer > Manage GPU Performance Counters, or run as administrator)";
        return false;
    }
    s.inSession = true;
    return true;
}

void Session::ListMetrics(std::vector<DxCounterInfo>& out) {
    Impl& s = *_impl;
    if (!s.evaluator.Get()) return;
    // One spelling per metric, as the Vulkan replay lists them: a counter's sum, a ratio as a
    // percentage, a throughput as its share of the unit's sustained peak over the range.
    const struct { NVPW_MetricType type; const char* suffix; } kinds[] = {
        {NVPW_METRIC_TYPE_COUNTER, ".sum"},
        {NVPW_METRIC_TYPE_RATIO, ".pct"},
        {NVPW_METRIC_TYPE_THROUGHPUT, ".avg.pct_of_peak_sustained_elapsed"},
    };
    for (const auto& kind : kinds) {
        for (const char* name : nv::perf::EnumerateMetrics(s.evaluator, kind.type)) {
            if (!name || std::strstr(name, "Triage")) continue;
            const std::string full = std::string(name) + kind.suffix;
            NVPW_MetricEvalRequest request{};
            if (!nv::perf::ToMetricEvalRequest(s.evaluator, full.c_str(), request)) continue;
            out.push_back(s.Describe(full, request));
        }
    }
    g_log.clear();
}

bool Session::Configure(const std::vector<std::string>& names, uint16_t nestingLevels, std::vector<DxCounterInfo>& chosen,
                        std::vector<std::string>& notes) {
    Impl& s = *_impl;
    if (!s.inSession) return false;
    g_log.clear();
    // The config builder below takes ownership of this and destroys it with itself, so it must not
    // be destroyed here as well once Initialize has succeeded.
    NVPW_RawCounterConfig* rawConfig = nullptr;
    {
        NVPW_D3D12_RawCounterConfig_Create_Params params{NVPW_D3D12_RawCounterConfig_Create_Params_STRUCT_SIZE};
        params.activityKind = NVPA_ACTIVITY_KIND_PROFILER;
        params.pChipName = s.chip.c_str();
        if (NVPW_D3D12_RawCounterConfig_Create(&params) == NVPA_STATUS_SUCCESS) rawConfig = params.pRawCounterConfig;
    }
    if (!rawConfig) {
        notes.push_back("the Nsight Perf SDK could not create a counter configuration for chip " + s.chip + ": " + TakeLog("no details"));
        return false;
    }
    auto destroyRawConfig = [&] {
        NVPW_RawCounterConfig_Destroy_Params destroy{NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE};
        destroy.pRawCounterConfig = rawConfig;
        NVPW_RawCounterConfig_Destroy(&destroy);
    };
    // Which raw counters this GPU can actually collect, so a metric that needs one it cannot is left out.
    {
        std::vector<uint8_t> image;
        if (s.profiler.CounterAvailability(image)) {
            NVPW_RawCounterConfig_SetCounterAvailability_Params set{NVPW_RawCounterConfig_SetCounterAvailability_Params_STRUCT_SIZE};
            set.pRawCounterConfig = rawConfig;
            set.pCounterAvailabilityImage = image.data();
            NVPW_RawCounterConfig_SetCounterAvailability(&set);
        }
        g_log.clear();
    }
    nv::perf::MetricsConfigBuilder builder;
    if (!builder.Initialize(s.evaluator, rawConfig, s.chip.c_str())) {
        // Ownership only transfers once Initialize succeeds, so an early failure frees it here.
        destroyRawConfig();
        notes.push_back("the Nsight Perf SDK could not initialize its configuration builder: " + TakeLog("no details"));
        return false;
    }
    for (const std::string& name : names) {
        NVPW_MetricEvalRequest request{};
        if (!nv::perf::ToMetricEvalRequest(s.evaluator, name.c_str(), request)) {
            notes.push_back("counter \"" + name + "\" is not known for chip " + s.chip + " (--list-counters names the ones that are)");
            g_log.clear();
            continue;
        }
        if (!builder.AddMetrics(&request, 1)) {
            notes.push_back("counter \"" + name + "\" cannot be collected on this GPU: " + TakeLog("no details"));
            continue;
        }
        s.requests.push_back(request);
        chosen.push_back(s.Describe(name, request));
    }
    if (s.requests.empty()) {
        notes.push_back("none of the counters asked for can be collected");
        return false;
    }
    if (!nv::perf::CreateConfiguration(builder, s.configuration)) {
        notes.push_back("the Nsight Perf SDK could not build the counter configuration: " + TakeLog("no details"));
        return false;
    }
    // One nesting level for a range per pass; two when each pass's draws are ranges inside it.
    if (!s.profiler.EnqueueCounterCollection(s.configuration, nestingLevels, 1)) {
        notes.push_back("the Nsight Perf SDK refused the counter configuration: " + TakeLog("no details"));
        return false;
    }
    g_log.clear();
    return true;
}

size_t Session::Passes() const { return _impl->configuration.numPasses; }

bool Session::BeginPass() {
    const bool ok = _impl->profiler.BeginPass();
    if (!ok) g_log.clear();
    return ok;
}

bool Session::EndPass() {
    const bool ok = _impl->profiler.EndPass();
    if (!ok) g_log.clear();
    return ok;
}

void Session::PushRange(ID3D12GraphicsCommandList* list, const char* name) {
    NVPW_D3D12_Profiler_CommandList_PushRange_Params params{NVPW_D3D12_Profiler_CommandList_PushRange_Params_STRUCT_SIZE};
    params.pCommandList = list;
    params.pRangeName = name;
    params.rangeNameLength = name ? std::strlen(name) : 0;
    NVPW_D3D12_Profiler_CommandList_PushRange(&params);
}

void Session::PopRange(ID3D12GraphicsCommandList* list) {
    NVPW_D3D12_Profiler_CommandList_PopRange_Params params{NVPW_D3D12_Profiler_CommandList_PopRange_Params_STRUCT_SIZE};
    params.pCommandList = list;
    NVPW_D3D12_Profiler_CommandList_PopRange(&params);
}

bool Session::Decode(bool& done, std::string& error) {
    done = false;
    nv::perf::profiler::DecodeResult result;
    if (!_impl->profiler.DecodeCounters(result)) {
        error = "the Nsight Perf SDK could not decode a collection pass: " + TakeLog("no details");
        return false;
    }
    if (result.allPassesDecoded) {
        _impl->counterData = std::move(result.counterDataImage);
        done = true;
    }
    return true;
}

bool Session::Results(std::vector<std::pair<std::string, std::vector<double>>>& out, std::string& error) {
    Impl& s = *_impl;
    if (s.counterData.empty()) {
        error = "the Nsight Perf SDK collected no counter data";
        return false;
    }
    if (!nv::perf::MetricsEvaluatorSetDeviceAttributes(s.evaluator, s.counterData.data(), s.counterData.size())) {
        error = "the Nsight Perf SDK could not read the device attributes of its counter data: " + TakeLog("no details");
        return false;
    }
    const size_t ranges = nv::perf::CounterDataGetNumRanges(s.counterData.data());
    std::vector<double> values(s.requests.size());
    for (size_t r = 0; r < ranges; ++r) {
        const char* leaf = nullptr;
        const std::string full = nv::perf::profiler::CounterDataGetRangeName(s.counterData.data(), r, '/', &leaf);
        if (!leaf) continue;
        std::fill(values.begin(), values.end(), std::nan(""));
        nv::perf::EvaluateToGpuValues(s.evaluator, s.counterData.data(), s.counterData.size(), r, s.requests.size(),
                                      s.requests.data(), values.data());
        out.push_back({leaf, values});
    }
    g_log.clear();
    return true;
}

void Session::End() {
    if (_impl->inSession) {
        _impl->profiler.EndSession();
        _impl->inSession = false;
    }
    g_log.clear();
}

} // namespace nvperf
} // namespace dxreplay

#else  // DXINSP_NVPERF

namespace dxreplay {
namespace nvperf {

bool Load(std::string& note) {
    note = "dxinsp_replay was built without NVIDIA's Nsight Perf SDK (DXINSP_NVPERF)";
    return false;
}
std::string LibraryPath() { return {}; }
bool LoadDriver(std::string& note) { return Load(note); }
bool ProfilingPermitted(std::string& note) { return Load(note); }

struct Session::Impl {};

Session::Session() : _impl(nullptr) {}
Session::~Session() {}

const std::string& Session::Chip() const {
    static const std::string none;
    return none;
}
bool Session::Init(ID3D12Device*, ID3D12CommandQueue*, std::string& note) {
    return Load(note);
}
bool Session::Begin(uint32_t, std::string& note) {
    return Load(note);
}
void Session::ListMetrics(std::vector<DxCounterInfo>&) {}
bool Session::Configure(const std::vector<std::string>&, uint16_t, std::vector<DxCounterInfo>&, std::vector<std::string>&) { return false; }
size_t Session::Passes() const { return 0; }
bool Session::BeginPass() { return false; }
bool Session::EndPass() { return false; }
void Session::PushRange(ID3D12GraphicsCommandList*, const char*) {}
void Session::PopRange(ID3D12GraphicsCommandList*) {}
bool Session::Decode(bool&, std::string& error) {
    return Load(error);
}
bool Session::Results(std::vector<std::pair<std::string, std::vector<double>>>&, std::string& error) {
    return Load(error);
}
void Session::End() {}

} // namespace nvperf
} // namespace dxreplay

#endif // DXINSP_NVPERF
