#include "replayer.h"

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
        info.queryCount = actions * 2;
        if (_fns.CreateQueryPool(_device, &info, nullptr, &_drawTimestamps) != VK_SUCCESS) _drawTimestamps = VK_NULL_HANDLE;
    }
    if (_drawCountersAvailable) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.queryCount = actions;
        info.pipelineStatistics = kStatistics;
        if (_fns.CreateQueryPool(_device, &info, nullptr, &_drawStatistics) != VK_SUCCESS) _drawStatistics = VK_NULL_HANDLE;
    }
    _drawQueryCapacity = actions;
    if (!_drawTimestamps && !_drawStatistics) {
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
}

int Replayer::BeginDrawQuery(VkCommandBuffer cb, uint32_t command, uint32_t frame, uint64_t commandBuffer, uint32_t passIndex) {
    if (_drawSlot >= _drawQueryCapacity) return -1;
    const uint32_t slot = _drawSlot++;
    DrawResult d;
    d.command = command;
    d.frame = frame;
    d.commandBuffer = commandBuffer;
    d.passIndex = passIndex;
    d.timed = _drawTimestamps != VK_NULL_HANDLE;
    // A statistics query cannot begin while the capture's own query of that type is open.
    d.counted = _drawStatistics != VK_NULL_HANDLE && _appQueryDepth == 0;
    _pendingDraws.push_back(d);
    if (d.timed) _fns.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _drawTimestamps, slot * 2);
    if (d.counted) _fns.CmdBeginQuery(cb, _drawStatistics, slot, 0);
    return (int)slot;
}

void Replayer::EndDrawQuery(VkCommandBuffer cb, int slot) {
    if (slot < 0 || (size_t)slot >= _pendingDraws.size()) return;
    const DrawResult& d = _pendingDraws[(size_t)slot];
    if (d.counted) _fns.CmdEndQuery(cb, _drawStatistics, (uint32_t)slot);
    if (d.timed) _fns.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _drawTimestamps, (uint32_t)slot * 2 + 1);
}

void Replayer::CompleteDrawStats(bool submitted) {
    if (_pendingDraws.empty()) {
        _drawSlot = 0;
        return;
    }
    // A submission that never ran leaves its queries unwritten, and waiting for them would hang.
    if (!submitted) {
        _pendingDraws.clear();
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
    std::vector<uint64_t> stats;
    if (_drawStatistics) {
        stats.resize((size_t)_drawSlot * kStatisticCount, 0);
        if (_fns.GetQueryPoolResults(_device, _drawStatistics, 0, _drawSlot, stats.size() * sizeof(uint64_t), stats.data(),
                                     kStatisticCount * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
            stats.clear();
        }
    }
    for (size_t slot = 0; slot < _pendingDraws.size(); ++slot) {
        DrawResult d = _pendingDraws[slot];
        if (d.timed && times.size() >= (slot + 1) * 2) {
            const uint64_t begin = times[slot * 2];
            const uint64_t end = times[slot * 2 + 1];
            d.durationMs = end > begin ? (double)(end - begin) * _timestampPeriod / 1e6 : 0;
        } else {
            d.timed = false;
        }
        if (d.counted && stats.size() >= (slot + 1) * kStatisticCount) {
            const uint64_t* v = &stats[slot * kStatisticCount];
            d.primitives = v[1];
            d.vertexInvocations = v[2];
            d.fragmentInvocations = v[5];
            d.computeInvocations = v[6];
        } else {
            d.counted = false;
        }
        _report->draws.push_back(d);
    }
    _pendingDraws.clear();
    _drawSlot = 0;
}

void Replayer::DestroyDrawStats() {
    if (_drawTimestamps) _fns.DestroyQueryPool(_device, _drawTimestamps, nullptr);
    if (_drawStatistics) _fns.DestroyQueryPool(_device, _drawStatistics, nullptr);
    _drawTimestamps = VK_NULL_HANDLE;
    _drawStatistics = VK_NULL_HANDLE;
    _drawQueryCapacity = 0;
}

}  // namespace vkreplay
