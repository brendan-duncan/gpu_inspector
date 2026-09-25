#include "replayer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "hw_counters.h"
#include "util.h"

// Hardware counters per pass and per draw: the GPU vendor's own counters, which say which unit
// inside the shader core a pass saturates — what docs/PROFILING.md calls "the limiters" and lists
// as out of reach through the public per-pass pipeline statistics. Two backends supply them:
//
//   * NVIDIA's Nsight Perf SDK (nvperf.cpp), the same one RenderDoc uses. It profiles named ranges,
//     so the replay pushes a range around each render pass and, nested inside, each draw. The frame
//     is replayed once per collection pass the chosen metrics need (a metric set that does not fit
//     the hardware's counter slots in one go is split over several); the SDK sums a range's counters
//     across the passes.
//   * VK_KHR_performance_query, the portable path (AMD, Intel, Arm, Qualcomm). Command-scoped
//     counters only, and a pool's queries cannot nest, so this path measures draws, not passes. The
//     frame is replayed once per counterPassIndex the pool reports.
//
// Which one runs is decided in PrepareCounters: NvPerf on an NVIDIA device where the SDK loaded,
// else KHR where the device has it. The rounds are driven from RunFrame, which replays the frame
// (ReplayCommands) once per BeginCounterRound / EndCounterRound.

namespace vkreplay
{

namespace
{

/** The name a draw's range carries, so its command index survives the round trip through the SDK. */
std::string DrawRangeName(uint32_t command) { return "D" + std::to_string(command); }
std::string PassRangeName(uint32_t passIndex) { return "P" + std::to_string(passIndex); }

/** "watts", "hertz"... from a KHR counter's unit, matching the words the NvPerf path uses. */
const char* KhrUnit(VkPerformanceCounterUnitKHR unit)
{
    switch (unit)
    {
        case VK_PERFORMANCE_COUNTER_UNIT_PERCENTAGE_KHR: return "percent";
        case VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR: return "ns";
        case VK_PERFORMANCE_COUNTER_UNIT_BYTES_KHR: return "bytes";
        case VK_PERFORMANCE_COUNTER_UNIT_BYTES_PER_SECOND_KHR: return "bytes/s";
        case VK_PERFORMANCE_COUNTER_UNIT_KELVIN_KHR: return "kelvin";
        case VK_PERFORMANCE_COUNTER_UNIT_WATTS_KHR: return "watts";
        case VK_PERFORMANCE_COUNTER_UNIT_VOLTS_KHR: return "volts";
        case VK_PERFORMANCE_COUNTER_UNIT_AMPS_KHR: return "amps";
        case VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR: return "hertz";
        case VK_PERFORMANCE_COUNTER_UNIT_CYCLES_KHR: return "cycles";
        default: return "count";
    }
}

double KhrValue(const VkPerformanceCounterResultKHR& r, VkPerformanceCounterStorageKHR storage, VkPerformanceCounterUnitKHR unit)
{
    double v = 0;
    switch (storage)
    {
        case VK_PERFORMANCE_COUNTER_STORAGE_INT32_KHR: v = r.int32; break;
        case VK_PERFORMANCE_COUNTER_STORAGE_INT64_KHR: v = (double)r.int64; break;
        case VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR: v = r.uint32; break;
        case VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR: v = (double)r.uint64; break;
        case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR: v = r.float32; break;
        case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR: v = r.float64; break;
        default: break;
    }
    if (unit == VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR)
        return v;  // already nanoseconds
    return v;
}

} // namespace

/**
 * All the state one collection needs, kept off the Replayer so its header carries none of the SDK's
 * types. One range recorded in the frame: what it is, and (KHR) which query index holds it.
 */
struct HwCounterState
{
    bool nvperf = false;
    bool khr = false;
    /** Whether each draw gets a range of its own, not only each render pass. */
    bool perDraw = false;
    nvperf::Session session;

    // What each range is, filled on the first round (the frame is the same every round).
    struct Range
    {
        bool pass = false;
        uint32_t command = 0;
        uint32_t frame = 0;
        uint64_t commandBuffer = 0;
        uint32_t passIndex = 0;
        uint32_t query = 0;   // KHR: its slot in the pool
    };
    std::vector<Range> ranges;
    std::unordered_map<uint32_t, size_t> byCommand;   // command index -> ranges[] (draws), first round only
    bool rangesKnown = false;
    uint32_t nextQuery = 0;

    std::vector<HwCounterInfo> counters;   // the metrics collected, in value order

    // NvPerf: how many collection passes the configuration needs.
    size_t passes = 1;
    /**
     * Replays of the frame to allow for. A nesting level's ranges are collected in their own sweep,
     * so a configuration of `passes` needs `nesting * passes` of them, plus one to settle
     * (RenderDoc's NVCounterEnumerator::GetMaxNumReplayPasses computes the same bound). Stopping at
     * `passes` alone leaves a two-level run undecoded and collecting nothing.
     */
    size_t maxRounds = 2;

    // KHR
    VkQueryPool khrPool = VK_NULL_HANDLE;
    uint32_t khrPasses = 1;
    std::vector<uint32_t> khrCounterIndices;
    std::vector<VkPerformanceCounterKHR> khrCounters;
    std::vector<VkPerformanceCounterDescriptionKHR> khrDescs;
    bool lockHeld = false;
    VkPerformanceQuerySubmitInfoKHR submit{VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR};
};

// ---------------------------------------------------------------------------------------------

bool Replayer::PrepareCounters()
{
    _report->counters.requested = true;
    if (!_hw)
        _hw = new HwCounterState;
    HwCounterState& hw = *_hw;

    // How many ranges the frame holds: every draw, and (NvPerf) every render pass.
    uint32_t draws = 0, passes = 0;
    if (const JValue* commands = _capture->Commands(); commands && commands->IsArray())
    {
        for (uint32_t i = 0; i < commands->count; ++i)
        {
            const std::string m = Str(commands->items[i].Get("method"));
            if (IsMeasured(m))
                ++draws;
            else if (IsBeginRenderPass(m) || IsBeginRendering(m))
                ++passes;
        }
    }
    if (!draws)
    {
        _report->counters.notes.push_back("the capture has no draws or dispatches to measure");
        return false;
    }

    // Which backend to run. Naming one the device cannot use is not an error: its path still runs
    // as far as it can and says what stopped it, which is the only way to exercise the portable
    // path on a driver that does not offer VK_KHR_performance_query (NVIDIA's does not).
    const std::string& want = _options.counters.backend;
    if (!want.empty() && want != "nvperf" && want != "khr")
    {
        _report->counters.notes.push_back("unknown counter backend \"" + want + "\": it is \"nvperf\" or \"khr\"");
        return false;
    }
    const bool forceNvperf = want == "nvperf";
    const bool forceKhr = want == "khr";

    if (forceNvperf && !_nvperfReady)
    {
        _report->counters.notes.push_back("the nvperf backend was asked for but is not available here" + (_nvperfNote.empty() ? std::string() : ": " + _nvperfNote));
        return false;
    }

    if (_nvperfReady && !forceKhr)
    {
        std::string note;
        auto gdpa = (PFN_vkGetDeviceProcAddr)_fns.GetInstanceProcAddr(_instance, "vkGetDeviceProcAddr");
        if (hw.session.Init(_instance, _physical, _device, _queue, _queueFamily, _fns.GetInstanceProcAddr, gdpa, note))
        {
            hw.nvperf = true;
            _report->counters.backend = "nvperf";
            _report->counters.chip = hw.session.Chip();
            if (_options.counters.list)
                return true;   // listing needs the evaluator only, not a session
            // A range per pass, and per draw as well only when asked: profiling thousands of ranges
            // is slow, and the extra nesting level roughly doubles the collection passes.
            hw.perDraw = _options.counters.perDraw;
            const uint32_t ranges = passes + (hw.perDraw ? draws : 0) + 2;
            // The session itself needs GPU performance-counter access enabled (ERR_NVGPUCTRPERM).
            if (!hw.session.Begin(ranges, note))
            {
                _report->counters.notes.push_back(note);
                return false;
            }
            std::vector<std::string> names = _options.counters.names;
            if (names.empty())
                names = DefaultCounterNames();
            std::vector<std::string> notes;
            if (!hw.session.Configure(names, hw.perDraw ? 2 : 1, hw.counters, notes))
            {
                for (auto& n : notes)
                    _report->counters.notes.push_back(n);
                _report->counters.notes.push_back("no NVIDIA counters could be configured");
                return false;
            }
            for (auto& n : notes)
                _report->counters.notes.push_back(n);
            hw.passes = std::max<size_t>(1, hw.session.Passes());
            hw.maxRounds = (hw.perDraw ? 2 : 1) * hw.passes + 1;
            return true;
        }
        _report->counters.notes.push_back(note);
        // Fall through to KHR if the device has it.
    }

    if (!forceNvperf && (_perfQueryAvailable || forceKhr))
    {
        hw.khr = true;
        _report->counters.backend = "khr";
        return PrepareKhrCounters(draws);
    }

    if (!_nvperfReady && !_perfQueryAvailable)
    {
        _report->counters.notes.push_back(_nvperfNote.empty()
                ? "this GPU exposes no hardware counters the replay can read (no NVIDIA Nsight Perf SDK and no VK_KHR_performance_query)"
                : _nvperfNote);
    }
    return false;
}

std::vector<std::string> Replayer::DefaultCounterNames() const
{
    // The limiters docs/PROFILING.md names, in NvPerf's spelling: overall SM and memory throughput,
    // the warp occupancy against the theoretical maximum, and the L2 and DRAM traffic. These are the
    // ones a fragment- or bandwidth-bound pass shows first.
    return {
        "sm__throughput.avg.pct_of_peak_sustained_elapsed",             // overall SM (shader core) throughput
        "gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed",       // VRAM bandwidth
        "sm__warps_active.avg.pct_of_peak_sustained_active",            // achieved occupancy
        "l1tex__throughput.avg.pct_of_peak_sustained_elapsed",         // L1 / texture unit
        "lts__throughput.avg.pct_of_peak_sustained_elapsed",           // L2 cache
        "sm__pipe_alu_cycles_active.avg.pct_of_peak_sustained_active", // ALU pipe
        "sm__pipe_fma_cycles_active.avg.pct_of_peak_sustained_active", // FMA pipe
    };
}

bool Replayer::PrepareKhrCounters(uint32_t draws)
{
    HwCounterState& hw = *_hw;
    // The extension has to have been enabled on the device. A null check is not enough: the loader
    // returns a working-looking pointer for an extension function it knows, and calling one the
    // driver does not implement takes the process down ("ICD associated with VkPhysicalDevice does
    // not support ..."). NVIDIA's desktop driver does not offer this extension at all.
    if (!_perfQueryAvailable)
    {
        _report->counters.notes.push_back(
            "VK_KHR_performance_query is not enabled on this device, so the portable backend cannot run here. "
            "Mesa's AMD (RADV, which may need RADV_PERFTEST=perfcounters) and Intel (ANV) drivers do offer it");
        return false;
    }
    if (!_fns.EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR || !_fns.GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR)
    {
        _report->counters.notes.push_back("this build's Vulkan loader has no VK_KHR_performance_query entry points");
        return false;
    }
    uint32_t count = 0;
    _fns.EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(_physical, _queueFamily, &count, nullptr, nullptr);
    hw.khrCounters.assign(count, {VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR});
    hw.khrDescs.assign(count, {VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR});
    for (auto& c : hw.khrCounters)
        c.sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR;
    for (auto& d : hw.khrDescs)
        d.sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR;
    _fns.EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(_physical, _queueFamily, &count, hw.khrCounters.data(), hw.khrDescs.data());

    // Command-scoped counters only: a render pass or renderpass-scoped one cannot wrap a draw.
    auto index = [&](const std::string& name) -> int {
        for (uint32_t i = 0; i < count; ++i)
            if (hw.khrCounters[i].scope == VK_PERFORMANCE_COUNTER_SCOPE_COMMAND_KHR && name == hw.khrDescs[i].name)
                return (int)i;
        return -1;
    };
    if (_options.counters.list)
        return true;

    std::vector<std::string> wanted = _options.counters.names;
    if (wanted.empty())
    {
        // No portable default set exists, so take the first several command-scoped counters.
        for (uint32_t i = 0; i < count && hw.khrCounterIndices.size() < 8; ++i)
            if (hw.khrCounters[i].scope == VK_PERFORMANCE_COUNTER_SCOPE_COMMAND_KHR)
            {
                hw.khrCounterIndices.push_back(i);
                HwCounterInfo info;
                info.name = hw.khrDescs[i].name;
                info.description = hw.khrDescs[i].description;
                info.category = hw.khrDescs[i].category;
                info.unit = KhrUnit(hw.khrCounters[i].unit);
                hw.counters.push_back(info);
            }
    }
    else
    {
        for (const std::string& name : wanted)
        {
            const int i = index(name);
            if (i < 0)
            {
                _report->counters.notes.push_back("counter \"" + name + "\" is not a command-scoped counter on this device");
                continue;
            }
            hw.khrCounterIndices.push_back((uint32_t)i);
            HwCounterInfo info;
            info.name = hw.khrDescs[i].name;
            info.description = hw.khrDescs[i].description;
            info.category = hw.khrDescs[i].category;
            info.unit = KhrUnit(hw.khrCounters[i].unit);
            hw.counters.push_back(info);
        }
    }
    if (hw.khrCounterIndices.empty())
    {
        _report->counters.notes.push_back("no command-scoped counters could be selected");
        return false;
    }

    VkQueryPoolPerformanceCreateInfoKHR perf{VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR};
    perf.queueFamilyIndex = _queueFamily;
    perf.counterIndexCount = (uint32_t)hw.khrCounterIndices.size();
    perf.pCounterIndices = hw.khrCounterIndices.data();
    _fns.GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR(_physical, &perf, &hw.khrPasses);
    if (hw.khrPasses == 0)
        hw.khrPasses = 1;

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.pNext = &perf;
    info.queryType = VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR;
    info.queryCount = draws;
    if (_fns.CreateQueryPool(_device, &info, nullptr, &hw.khrPool) != VK_SUCCESS)
    {
        _report->counters.notes.push_back("could not create a performance query pool");
        return false;
    }
    VkAcquireProfilingLockInfoKHR lock{VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR};
    lock.timeout = 5ull * 1000 * 1000 * 1000;
    if (!_fns.AcquireProfilingLockKHR || _fns.AcquireProfilingLockKHR(_device, &lock) != VK_SUCCESS)
    {
        _report->counters.notes.push_back("could not acquire the Vulkan profiling lock");
        return false;
    }
    hw.lockHeld = true;
    hw.passes = hw.khrPasses;
    hw.maxRounds = hw.khrPasses;   // one replay per counter pass index, exactly
    _report->counters.notes.push_back("VK_KHR_performance_query measures draws, not passes");
    return true;
}

void Replayer::ListCounters()
{
    if (!_hw)
        return;
    if (_hw->nvperf)
        _hw->session.ListMetrics(_report->counters.available);
    else if (_hw->khr)
    {
        for (uint32_t i = 0; i < _hw->khrCounters.size(); ++i)
        {
            if (_hw->khrCounters[i].scope != VK_PERFORMANCE_COUNTER_SCOPE_COMMAND_KHR)
                continue;
            HwCounterInfo info;
            info.name = _hw->khrDescs[i].name;
            info.description = _hw->khrDescs[i].description;
            info.category = _hw->khrDescs[i].category;
            info.unit = KhrUnit(_hw->khrCounters[i].unit);
            _report->counters.available.push_back(info);
        }
    }
}

bool Replayer::BeginCounterRound()
{
    if (!_hw)
        return false;
    HwCounterState& hw = *_hw;
    hw.nextQuery = 0;   // ranges are recorded in the same order every round
    if (hw.nvperf)
        return hw.session.BeginPass();
    if (hw.khr)
    {
        hw.submit.counterPassIndex = _hwRound;
        _submitNext = &hw.submit;   // chained into each of the frame's submissions
        return true;
    }
    return false;
}

bool Replayer::EndCounterRound()
{
    if (!_hw)
        return true;
    HwCounterState& hw = *_hw;
    if (hw.nvperf)
    {
        if (!hw.session.EndPass())
            return false;
        _fns.QueueWaitIdle(_queue);
        bool done = false;
        std::string error;
        if (!hw.session.Decode(done, error))
        {
            _report->counters.notes.push_back(error);
            return false;   // stop; CompleteCounters reports what there is
        }
        hw.rangesKnown = true;
        return !done;   // another round while not every pass is decoded
    }
    if (hw.khr)
    {
        _submitNext = nullptr;
        hw.rangesKnown = true;
        return (_hwRound + 1) < hw.khrPasses;
    }
    return false;
}

void Replayer::BeginCounterPass(VkCommandBuffer cb, const PassState& pass)
{
    HwCounterState& hw = *_hw;
    if (!hw.nvperf)
        return;   // KHR cannot wrap a pass
    hw.session.PushRange(cb, PassRangeName(pass.index).c_str());
    if (!hw.rangesKnown)
    {
        HwCounterState::Range r;
        r.pass = true;
        r.command = pass.beginIndex;
        r.frame = pass.frame;
        r.commandBuffer = pass.commandBuffer;
        r.passIndex = pass.index;
        hw.ranges.push_back(r);
    }
}

void Replayer::EndCounterPass(VkCommandBuffer cb)
{
    if (_hw->nvperf)
        _hw->session.PopRange(cb);
}

int Replayer::BeginCounterDraw(VkCommandBuffer cb, uint32_t command, uint32_t frame, uint64_t commandBuffer, uint32_t passIndex)
{
    HwCounterState& hw = *_hw;
    if (hw.nvperf)
    {
        if (!hw.perDraw)
            return -1;   // passes only: a range per draw is opt-in
        hw.session.PushRange(cb, DrawRangeName(command).c_str());
    }
    else if (hw.khr)
    {
        _fns.CmdBeginQuery(cb, hw.khrPool, hw.nextQuery, 0);
    }
    else
    {
        return -1;
    }
    if (!hw.rangesKnown)
    {
        HwCounterState::Range r;
        r.command = command;
        r.frame = frame;
        r.commandBuffer = commandBuffer;
        r.passIndex = passIndex;
        r.query = hw.nextQuery;
        hw.byCommand[command] = hw.ranges.size();
        hw.ranges.push_back(r);
    }
    return (int)hw.nextQuery++;
}

void Replayer::EndCounterDraw(VkCommandBuffer cb, int range)
{
    if (range < 0)
        return;
    HwCounterState& hw = *_hw;
    if (hw.nvperf)
        hw.session.PopRange(cb);
    else if (hw.khr)
        _fns.CmdEndQuery(cb, hw.khrPool, (uint32_t)range);
}

uint32_t Replayer::CounterRounds() const
{
    return _hw ? (uint32_t)_hw->maxRounds : 0;
}

void Replayer::CompleteCounters()
{
    if (!_hw)
        return;
    HwCounterState& hw = *_hw;
    HwCounterReport& out = _report->counters;
    out.counters = hw.counters;
    out.rounds = (uint32_t)hw.passes;

    if (hw.nvperf)
    {
        std::vector<std::pair<std::string, std::vector<double>>> results;
        std::string error;
        if (!hw.session.Results(results, error))
        {
            out.notes.push_back(error);
            return;
        }
        // Each range comes back by its leaf name ("P0" or "D17"); its metadata is in hw.ranges.
        std::unordered_map<uint32_t, const HwCounterState::Range*> passByIndex, drawByCommand;
        for (const auto& r : hw.ranges)
        {
            if (r.pass)
                passByIndex[r.passIndex] = &r;
            else
                drawByCommand[r.command] = &r;
        }
        for (const auto& [leaf, values] : results)
        {
            if (leaf.size() < 2)
                continue;
            const uint32_t id = (uint32_t)std::strtoul(leaf.c_str() + 1, nullptr, 10);
            HwCounterRange range;
            range.values = values;
            if (leaf[0] == 'P')
            {
                auto it = passByIndex.find(id);
                if (it == passByIndex.end())
                    continue;
                range.pass = true;
                range.command = it->second->command;
                range.frame = it->second->frame;
                range.commandBuffer = it->second->commandBuffer;
                range.passIndex = it->second->passIndex;
                out.passes.push_back(std::move(range));
            }
            else if (leaf[0] == 'D')
            {
                auto it = drawByCommand.find(id);
                if (it == drawByCommand.end())
                    continue;
                range.command = it->second->command;
                range.frame = it->second->frame;
                range.commandBuffer = it->second->commandBuffer;
                range.passIndex = it->second->passIndex;
                out.draws.push_back(std::move(range));
            }
        }
    }
    else if (hw.khr)
    {
        const size_t n = hw.counters.size();
        std::vector<VkPerformanceCounterResultKHR> results((size_t)hw.nextQuery * n);
        if (hw.nextQuery && _fns.GetQueryPoolResults(_device, hw.khrPool, 0, hw.nextQuery, results.size() * sizeof(results[0]), results.data(), n * sizeof(results[0]), VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        {
            for (const auto& r : hw.ranges)
            {
                if (r.pass)
                    continue;
                HwCounterRange range;
                range.command = r.command;
                range.frame = r.frame;
                range.commandBuffer = r.commandBuffer;
                range.passIndex = r.passIndex;
                range.values.resize(n);
                for (size_t c = 0; c < n; ++c)
                    range.values[c] = KhrValue(results[(size_t)r.query * n + c], hw.khrCounters[hw.khrCounterIndices[c]].storage,
                        hw.khrCounters[hw.khrCounterIndices[c]].unit);
                out.draws.push_back(std::move(range));
            }
        }
        else
        {
            out.notes.push_back("could not read the performance query results back");
        }
    }
}

void Replayer::DestroyCounters()
{
    if (!_hw)
        return;
    _submitNext = nullptr;
    if (_hw->khr)
    {
        if (_hw->lockHeld && _fns.ReleaseProfilingLockKHR)
            _fns.ReleaseProfilingLockKHR(_device);
        if (_hw->khrPool)
            _fns.DestroyQueryPool(_device, _hw->khrPool, nullptr);
    }
    _hw->session.End();
    delete _hw;
    _hw = nullptr;
}

} // namespace vkreplay
