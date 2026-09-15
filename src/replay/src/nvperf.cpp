// NVIDIA's Nsight Perf SDK behind hw_counters.h: the SDK's range profiler, which collects the
// GPU's own counters between ranges pushed into command buffers, replaying the frame once per
// collection pass, and its metrics evaluator, which turns raw counters into named metrics. RenderDoc
// drives the same SDK the same way (driver/ihv/nv/nv_vk_counters.cpp).
//
// The SDK's headers (third_party/nvperf, the redistributable part of the SDK) declare the API and
// a header-only utility layer; the host library itself is loaded at run time by nvperf_host_impl.h,
// which this file compiles once for the whole program.
#include "hw_counters.h"

#include "replayer.h"

#ifdef VKINSP_NVPERF

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include "windows-desktop-x64/nvperf_host_impl.h"
#else
#include "linux-desktop-x64/nvperf_host_impl.h"
#endif

#include "NvPerfCounterConfiguration.h"
#include "NvPerfCounterData.h"
#include "NvPerfInit.h"
#include "NvPerfMetricsConfigBuilder.h"
#include "NvPerfMetricsEvaluator.h"
#include "NvPerfRangeProfilerVulkan.h"
#include "NvPerfVulkan.h"

namespace vkreplay {
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

#ifdef _WIN32
constexpr const char* kLibraryName = "nvperf_grfx_host.dll";
#else
constexpr const char* kLibraryName = "libnvperf_grfx_host.so";
#endif

/** Directories that hold the host library, in the order to try them. */
std::vector<std::string> SearchDirectories() {
    namespace fs = std::filesystem;
    std::vector<std::string> dirs;
    auto add = [&](const fs::path& dir) {
        std::error_code ec;
        if (!fs::exists(dir / kLibraryName, ec)) return;
        const std::string s = dir.string();
        if (std::find(dirs.begin(), dirs.end(), s) == dirs.end()) dirs.push_back(s);
    };
    // Beside the tool, and a plugins/nv directory next to it (where RenderDoc keeps its copy).
    std::error_code ec;
    fs::path exe;
#ifdef _WIN32
    wchar_t buffer[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, 4096);
    if (n > 0 && n < 4096) exe = fs::path(buffer);
#else
    exe = fs::read_symlink("/proc/self/exe", ec);
#endif
    if (!exe.empty()) {
        add(exe.parent_path());
        add(exe.parent_path() / "plugins" / "nv");
    }
    if (const char* env = std::getenv("VKINSP_NVPERF_DIR"); env && *env) add(fs::path(env));
    // An Nsight install: Graphics, Systems and Compute each carry the library a few levels down.
    std::vector<fs::path> roots;
#ifdef _WIN32
    const char* programFiles = std::getenv("ProgramFiles");
    roots.push_back(fs::path(programFiles && *programFiles ? programFiles : "C:\\Program Files") / "NVIDIA Corporation");
#else
    roots.push_back("/opt/nvidia");
    roots.push_back("/usr/local/NVIDIA-Nsight-Graphics");
#endif
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    for (const fs::path& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            if (it.depth() > 4) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->path().filename() == kLibraryName) found.push_back({fs::last_write_time(it->path(), ec), it->path().parent_path()});
        }
    }
    // The newest copy first: the SDK's library must be at least as new as the headers.
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& f : found) add(f.second);
    return dirs;
}

/** The unit a metric's dimensions come to: what the app formats the value by. */
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
        g_loadNote = std::string("NVIDIA's Nsight Perf SDK library (") + kLibraryName + ") was not found: put it beside vkinsp_replay, "
                     "name its directory in VKINSP_NVPERF_DIR, or install Nsight Graphics (https://developer.nvidia.com/nsight-perf-sdk)";
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
        g_loadNote = "NVIDIA's Nsight Perf SDK library in " + dirs.front() + " could not be initialised: " + TakeLog("no details");
        note = g_loadNote;
        return false;
    }
    if (!NVPA_GetProcAddress("NVPW_VK_RawCounterConfig_Create")) {
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

void InstanceExtensions(uint32_t apiVersion, std::vector<const char*>& out) {
    if (!g_loaded) return;
    std::vector<const char*> names;
    if (nv::perf::VulkanAppendInstanceRequiredExtensions(names, apiVersion)) out.insert(out.end(), names.begin(), names.end());
    g_log.clear();
}

void DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr gipa, std::vector<const char*>& out) {
    if (!g_loaded) return;
    std::vector<const char*> names;
    if (nv::perf::VulkanAppendDeviceRequiredExtensions(instance, physical, (void*)gipa, names)) out.insert(out.end(), names.begin(), names.end());
    g_log.clear();
}

struct Session::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    std::string chip;
    nv::perf::MetricsEvaluator evaluator;
    nv::perf::profiler::RangeProfilerVulkan profiler;
    bool inSession = false;
    NVPW_RawCounterConfig* rawConfig = nullptr;
    nv::perf::CounterConfiguration configuration;
    std::vector<NVPW_MetricEvalRequest> requests;
    std::vector<uint8_t> counterData;

    ~Impl() {
        if (rawConfig) {
            NVPW_RawCounterConfig_Destroy_Params destroy{NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE};
            destroy.pRawCounterConfig = rawConfig;
            NVPW_RawCounterConfig_Destroy(&destroy);
        }
    }

    /** A metric's description, hardware unit and unit, by its evaluation request. */
    HwCounterInfo Describe(const std::string& name, const NVPW_MetricEvalRequest& request) {
        HwCounterInfo info;
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

bool Session::Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t queueFamily,
                   PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, std::string& note) {
    if (!g_loaded) {
        note = g_loadNote.empty() ? "NVIDIA's Nsight Perf SDK is not loaded" : g_loadNote;
        return false;
    }
    Impl& s = *_impl;
    s.instance = instance;
    s.physical = physical;
    s.device = device;
    s.queue = queue;
    s.queueFamily = queueFamily;
    s.gipa = gipa;
    s.gdpa = gdpa;
    g_log.clear();
    if (!nv::perf::VulkanLoadDriver(instance)) {
        note = "the Nsight Perf SDK could not load the Vulkan driver: " + TakeLog("no details");
        return false;
    }
    if (!nv::perf::profiler::VulkanIsGpuSupported(instance, physical, device, gipa, gdpa)) {
        note = "the Nsight Perf SDK does not profile this GPU: " + TakeLog("no details");
        return false;
    }
    const nv::perf::DeviceIdentifiers ids = nv::perf::VulkanGetDeviceIdentifiers(instance, physical, device, gipa, gdpa);
    if (!ids.pChipName) {
        note = "the Nsight Perf SDK could not name this GPU's chip: " + TakeLog("no details");
        return false;
    }
    s.chip = ids.pChipName;
    const size_t scratch = nv::perf::VulkanCalculateMetricsEvaluatorScratchBufferSize(ids.pChipName);
    if (!scratch) {
        note = "the Nsight Perf SDK has no metrics for chip " + s.chip + ": " + TakeLog("no details");
        return false;
    }
    std::vector<uint8_t> buffer(scratch);
    NVPW_MetricsEvaluator* evaluator = nv::perf::VulkanCreateMetricsEvaluator(buffer.data(), buffer.size(), ids.pChipName);
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
        note = "the Nsight Perf SDK session was not initialised";
        return false;
    }
    g_log.clear();
    nv::perf::profiler::SessionOptions options;
    options.maxNumRanges = std::max<size_t>(maxRanges, 16);
    options.avgRangeNameLength = 16;
    options.numTraceBuffers = 1;
    if (!s.profiler.BeginSession(s.instance, s.physical, s.device, s.queue, s.queueFamily, options, s.gipa, s.gdpa)) {
        note = "the Nsight Perf SDK could not start a profiling session: " + TakeLog("no details")
             + " (on Windows, allow GPU performance counters for all users in the NVIDIA Control Panel, Developer > Manage GPU Performance Counters, or run as administrator)";
        return false;
    }
    s.inSession = true;
    return true;
}

void Session::ListMetrics(std::vector<HwCounterInfo>& out) {
    Impl& s = *_impl;
    if (!s.evaluator.Get()) return;
    // One spelling per metric: a counter's sum, a ratio as a percentage, a throughput as its share of the
    // unit's sustained peak over the range, which is how Nsight's own reports show them.
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

bool Session::Configure(const std::vector<std::string>& names, std::vector<HwCounterInfo>& chosen, std::vector<std::string>& notes) {
    Impl& s = *_impl;
    if (!s.inSession) return false;
    g_log.clear();
    s.rawConfig = nv::perf::profiler::VulkanCreateRawCounterConfig(s.chip.c_str());
    if (!s.rawConfig) {
        notes.push_back("the Nsight Perf SDK could not create a counter configuration for chip " + s.chip + ": " + TakeLog("no details"));
        return false;
    }
    // Which raw counters this GPU can actually collect, so a metric that needs one it cannot is left out.
    {
        NVPW_VK_Profiler_Queue_GetCounterAvailability_Params params{NVPW_VK_Profiler_Queue_GetCounterAvailability_Params_STRUCT_SIZE};
        params.instance = s.instance;
        params.physicalDevice = s.physical;
        params.device = s.device;
        params.queue = s.queue;
        params.pfnGetInstanceProcAddr = (void*)s.gipa;
        params.pfnGetDeviceProcAddr = (void*)s.gdpa;
        std::vector<uint8_t> image;
        if (NVPW_VK_Profiler_Queue_GetCounterAvailability(&params) == NVPA_STATUS_SUCCESS && params.counterAvailabilityImageSize) {
            image.resize(params.counterAvailabilityImageSize);
            params.pCounterAvailabilityImage = image.data();
            if (NVPW_VK_Profiler_Queue_GetCounterAvailability(&params) == NVPA_STATUS_SUCCESS) {
                NVPW_RawCounterConfig_SetCounterAvailability_Params set{NVPW_RawCounterConfig_SetCounterAvailability_Params_STRUCT_SIZE};
                set.pRawCounterConfig = s.rawConfig;
                set.pCounterAvailabilityImage = image.data();
                NVPW_RawCounterConfig_SetCounterAvailability(&set);
            }
        }
        g_log.clear();
    }
    nv::perf::MetricsConfigBuilder builder;
    if (!builder.Initialize(s.evaluator, s.rawConfig, s.chip.c_str())) {
        notes.push_back("the Nsight Perf SDK could not initialise its configuration builder: " + TakeLog("no details"));
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
    // Two nesting levels: a pass's range with its draws' ranges inside.
    if (!s.profiler.EnqueueCounterCollection(s.configuration, 2, 1)) {
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

void Session::PushRange(VkCommandBuffer cb, const char* name) { nv::perf::profiler::VulkanPushRange(cb, name); }

void Session::PopRange(VkCommandBuffer cb) { nv::perf::profiler::VulkanPopRange(cb); }

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
        nv::perf::EvaluateToGpuValues(s.evaluator, s.counterData.data(), s.counterData.size(), r, s.requests.size(), s.requests.data(), values.data());
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
} // namespace vkreplay

#else  // VKINSP_NVPERF

namespace vkreplay {
namespace nvperf {

bool Load(std::string& note) {
    note = "the replay was built without NVIDIA's Nsight Perf SDK (VKINSP_NVPERF)";
    return false;
}
std::string LibraryPath() { return {}; }
void InstanceExtensions(uint32_t, std::vector<const char*>&) {}
void DeviceExtensions(VkInstance, VkPhysicalDevice, PFN_vkGetInstanceProcAddr, std::vector<const char*>&) {}

struct Session::Impl {};
Session::Session() = default;
Session::~Session() = default;
const std::string& Session::Chip() const {
    static const std::string none;
    return none;
}
bool Session::Init(VkInstance, VkPhysicalDevice, VkDevice, VkQueue, uint32_t, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, std::string& note) {
    return Load(note);
}
bool Session::Begin(uint32_t, std::string& note) {
    return Load(note);
}
void Session::ListMetrics(std::vector<HwCounterInfo>&) {}
bool Session::Configure(const std::vector<std::string>&, std::vector<HwCounterInfo>&, std::vector<std::string>&) { return false; }
size_t Session::Passes() const { return 0; }
bool Session::BeginPass() { return false; }
bool Session::EndPass() { return false; }
void Session::PushRange(VkCommandBuffer, const char*) {}
void Session::PopRange(VkCommandBuffer) {}
bool Session::Decode(bool& done, std::string& error) {
    done = false;
    error = "not built with the Nsight Perf SDK";
    return false;
}
bool Session::Results(std::vector<std::pair<std::string, std::vector<double>>>&, std::string& error) {
    error = "not built with the Nsight Perf SDK";
    return false;
}
void Session::End() {}

} // namespace nvperf
} // namespace vkreplay

#endif
