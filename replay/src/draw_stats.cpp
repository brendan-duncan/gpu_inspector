#include "replayer.h"

#include <algorithm>
#include <string>
#include <vector>

#include "util.h"

namespace vkreplay {

// ---------------------------------------------------------------------------------------------
// Per-draw timing and counters
//
// Every draw and dispatch of the replayed frame is issued between a pair of timestamps and inside a
// pipeline statistics query, in the command buffer it belongs to: the frame runs exactly as the
// capture recorded it, with the queries added around each action. RenderDoc measures a frame's
// draws the same way (vk_counters.cpp).
//
// In a multiview pass every query takes one index per view, so a draw there takes that many slots;
// its counts are summed over them (an implementation may put the whole count in the first).
//
// A draw's time is not what that draw costs on its own. The GPU pipelines consecutive draws, so
// their spans overlap and add up to more than the pass takes; what the time is good for is the
// share of a pass a draw accounts for, which is what the Shader Flame Graph needs to split a pass's
// measured duration between the draws in it. The counters are exact.

namespace {

/** The statistics asked for, in VkQueryPipelineStatisticFlagBits order, which is the result order. */
constexpr VkQueryPipelineStatisticFlags kStatistics =
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |       // [0]
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |     // [1]
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |     // [2]
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |          // [3]
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |           // [4]
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |   // [5]
    VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;     // [6]
constexpr uint32_t kStatisticCount = 7;

/** Actions measured per replay; a frame with more is measured up to here. */
constexpr uint32_t kMaxMeasuredActions = 65536;

}  // namespace

bool Replayer::PrepareDrawStats() {
    uint32_t actions = 0;
    if (const JValue* commands = _capture->Commands(); commands && commands->IsArray()) {
        for (uint32_t i = 0; i < commands->count; ++i)
            if (IsAction(Str(commands->items[i].Get("method")))) ++actions;
    }
    if (!actions) {
        _report->drawStatsNote = "the capture has no draws or dispatches to measure";
        return false;
    }
    // Slots per draw: the most views any pass renders.
    uint32_t views = 1;
    for (const auto& [id, rp] : _renderPasses) views = std::max(views, rp.views);
    if (const JValue* commands = _capture->Commands(); commands && commands->IsArray()) {
        for (uint32_t i = 0; i < commands->count; ++i) {
            if (!IsBeginRendering(Str(commands->items[i].Get("method")))) continue;
            const JValue* args = commands->items[i].Get("args");
            const JValue* info = args ? args->Get("pRenderingInfo") : nullptr;
            if (const JValue* mask = info ? info->Get("viewMask") : nullptr) views = std::max(views, ViewCount((uint32_t)mask->Uint()));
        }
    }
    if (actions > kMaxMeasuredActions) {
        _report->drawStatsNote = "the frame has " + std::to_string(actions) + " draws and dispatches; the first "
                               + std::to_string(kMaxMeasuredActions) + " are measured";
        actions = kMaxMeasuredActions;
    }

    // Timestamps need a queue family that writes them and a period to turn ticks into nanoseconds.
    VkPhysicalDeviceProperties props{};
    _fns.GetPhysicalDeviceProperties(_physical, &props);
    uint32_t familyCount = 0;
    _fns.GetPhysicalDeviceQueueFamilyProperties(_physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    if (familyCount) _fns.GetPhysicalDeviceQueueFamilyProperties(_physical, &familyCount, families.data());
    const bool timestamps = props.limits.timestampPeriod > 0
                         && _queueFamily < families.size() && families[_queueFamily].timestampValidBits > 0;
    _timestampPeriod = timestamps ? props.limits.timestampPeriod : 0;

    if (timestamps) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = actions * views * 2;
        if (_fns.CreateQueryPool(_device, &info, nullptr, &_drawTimestamps) != VK_SUCCESS) _drawTimestamps = VK_NULL_HANDLE;
    }
    if (_drawCountersAvailable) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.queryCount = actions * views;
        info.pipelineStatistics = kStatistics;
        if (_fns.CreateQueryPool(_device, &info, nullptr, &_drawStatistics) != VK_SUCCESS) _drawStatistics = VK_NULL_HANDLE;
    }
    if (_drawSamplesAvailable) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_OCCLUSION;
        info.queryCount = actions * views;
        if (_fns.CreateQueryPool(_device, &info, nullptr, &_drawOcclusion) != VK_SUCCESS) _drawOcclusion = VK_NULL_HANDLE;
    }
    _drawQueryCapacity = actions * views;
    if (!_drawTimestamps && !_drawStatistics && !_drawOcclusion) {
        _report->drawStatsNote = "this device has neither timestamps on the replay's queue nor pipeline statistics queries";
        return false;
    }
    if (!_drawTimestamps) _report->drawStatsNote = "the replay's queue writes no timestamps, so the draws carry counters only";
    else if (!_drawStatistics) _report->drawStatsNote = "this device has no pipeline statistics queries, so the draws carry timings only";
    return true;
}

/** Resets the pools for a submission: its results are read once it has completed. */
void Replayer::ResetDrawQueries(VkCommandBuffer cb) {
    if (_drawTimestamps) _fns.CmdResetQueryPool(cb, _drawTimestamps, 0, _drawQueryCapacity * 2);
    if (_drawStatistics) _fns.CmdResetQueryPool(cb, _drawStatistics, 0, _drawQueryCapacity);
    if (_drawOcclusion) _fns.CmdResetQueryPool(cb, _drawOcclusion, 0, _drawQueryCapacity);
}

int Replayer::BeginDrawQuery(VkCommandBuffer cb, uint32_t command, uint32_t frame, uint64_t commandBuffer, uint32_t passIndex) {
    const uint32_t views = std::max(1u, _passViews);
    if (_drawSlot + views > _drawQueryCapacity) return -1;
    const uint32_t slot = _drawSlot;
    _drawSlot += views;
    _pendingDrawSlots.push_back({slot, views});
    DrawResult d;
    d.command = command;
    d.frame = frame;
    d.commandBuffer = commandBuffer;
    d.passIndex = passIndex;
    d.timed = _drawTimestamps != VK_NULL_HANDLE;
    // Neither query can begin while the capture's own query of that type is open.
    d.counted = _drawStatistics != VK_NULL_HANDLE && _appQueryDepth == 0;
    d.sampled = _drawOcclusion != VK_NULL_HANDLE && _appQueryDepth == 0;
    _pendingDraws.push_back(d);
    if (d.timed) _fns.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _drawTimestamps, slot * 2);
    if (d.counted) _fns.CmdBeginQuery(cb, _drawStatistics, slot, 0);
    if (d.sampled) _fns.CmdBeginQuery(cb, _drawOcclusion, slot, VK_QUERY_CONTROL_PRECISE_BIT);
    return (int)_pendingDraws.size() - 1;
}

void Replayer::EndDrawQuery(VkCommandBuffer cb, int pending) {
    if (pending < 0 || (size_t)pending >= _pendingDraws.size()) return;
    const DrawResult& d = _pendingDraws[(size_t)pending];
    const auto [slot, views] = _pendingDrawSlots[(size_t)pending];
    if (d.sampled) _fns.CmdEndQuery(cb, _drawOcclusion, slot);
    if (d.counted) _fns.CmdEndQuery(cb, _drawStatistics, slot);
    // The draw's timestamps: its views' begin times, then their end times.
    if (d.timed) _fns.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _drawTimestamps, slot * 2 + views);
}

void Replayer::CompleteDrawStats(bool submitted) {
    if (_pendingDraws.empty()) {
        _drawSlot = 0;
        return;
    }
    // A submission that never ran leaves its queries unwritten, and waiting for them would hang.
    if (!submitted) {
        _pendingDraws.clear();
        _pendingDrawSlots.clear();
        _drawSlot = 0;
        return;
    }
    std::vector<uint64_t> times;
    if (_drawTimestamps) {
        times.resize((size_t)_drawSlot * 2, 0);
        if (_fns.GetQueryPoolResults(_device, _drawTimestamps, 0, _drawSlot * 2, times.size() * sizeof(uint64_t), times.data(),
                                     sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
            times.clear();
        }
    }
    std::vector<uint64_t> samples;
    if (_drawOcclusion) {
        samples.resize((size_t)_drawSlot * 2, 0);
        if (_fns.GetQueryPoolResults(_device, _drawOcclusion, 0, _drawSlot, samples.size() * sizeof(uint64_t), samples.data(),
                                     2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT
                                     | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS) {
            samples.clear();
        }
    }
    std::vector<uint64_t> stats;
    if (_drawStatistics) {
        stats.resize((size_t)_drawSlot * kStatisticCount, 0);
        if (_fns.GetQueryPoolResults(_device, _drawStatistics, 0, _drawSlot, stats.size() * sizeof(uint64_t), stats.data(),
                                     kStatisticCount * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
            stats.clear();
        }
    }
    for (size_t i = 0; i < _pendingDraws.size(); ++i) {
        DrawResult d = _pendingDraws[i];
        const auto [slot, views] = _pendingDrawSlots[i];
        const size_t end = (size_t)slot + views;
        if (d.timed && times.size() >= end * 2) {
            const uint64_t begin = times[(size_t)slot * 2];
            const uint64_t finish = times[(size_t)slot * 2 + views];
            d.durationMs = finish > begin ? (double)(finish - begin) * _timestampPeriod / 1e6 : 0;
        } else {
            d.timed = false;
        }
        if (d.sampled && samples.size() >= end * 2) {
            d.samplesPassed = 0;
            for (size_t v = slot; v < end && d.sampled; ++v) {
                if (!samples[v * 2 + 1]) d.sampled = false;
                d.samplesPassed += samples[v * 2];
            }
        } else {
            d.sampled = false;
        }
        if (d.counted && stats.size() >= end * kStatisticCount) {
            d.primitives = d.vertexInvocations = d.fragmentInvocations = d.computeInvocations = 0;
            for (size_t v = slot; v < end; ++v) {
                const uint64_t* s = &stats[v * kStatisticCount];
                d.primitives += s[1];
                d.vertexInvocations += s[2];
                d.fragmentInvocations += s[5];
                d.computeInvocations += s[6];
            }
        } else {
            d.counted = false;
        }
        _report->draws.push_back(d);
    }
    _pendingDraws.clear();
    _pendingDrawSlots.clear();
    _drawSlot = 0;
}

void Replayer::DestroyDrawStats() {
    if (_drawTimestamps) _fns.DestroyQueryPool(_device, _drawTimestamps, nullptr);
    if (_drawStatistics) _fns.DestroyQueryPool(_device, _drawStatistics, nullptr);
    if (_drawOcclusion) _fns.DestroyQueryPool(_device, _drawOcclusion, nullptr);
    _drawTimestamps = VK_NULL_HANDLE;
    _drawStatistics = VK_NULL_HANDLE;
    _drawOcclusion = VK_NULL_HANDLE;
    _drawQueryCapacity = 0;
}

}  // namespace vkreplay
