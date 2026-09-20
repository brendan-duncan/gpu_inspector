// Hardware counters per render pass: the GPU's own counters, which say which unit inside the
// shader core a pass saturates — what docs/PROFILING.md calls "the limiters" and lists as out of
// reach through the public per-pass pipeline statistics.
//
// The Vulkan replay does the same in src/replay/src/hw_counters.cpp, and this is the Direct3D 12
// half of it: NVIDIA's Nsight Perf SDK (dx_nvperf.cpp) profiles named ranges, so the replay pushes
// a range around each render pass and replays the frame once per collection pass the chosen metrics
// need; the SDK sums a range's counters across the passes.
//
// Direct3D 12 has no portable counter API, so there is no second backend here: the Vulkan side's
// VK_KHR_performance_query path has no D3D12 counterpart, and an AMD or Intel GPU reports that
// rather than falling back to something.
//
// Why this lives in the replay rather than in the capture library, where D3D12's overdraw, pixel
// history, draw overlays and mesh output are measured: a range profiler needs the *same* GPU work
// submitted several times over, synchronizing the queue between them. In a live application that
// would mean re-issuing every pass N times inside the frame and stalling it each time, and the
// counters would then describe the re-issued pass rather than the application's own draws.
#include "dx_replayer.h"

#include "dx_counters.h"
#include "dx_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxreplay {

namespace {

/** A JSON string value, as dx_replayer.cpp reads them. */
std::string Str(const JValue* v) { return v && v->IsString() ? std::string(v->Str()) : std::string(); }

/** The name a pass's range carries, so its index survives the round trip through the SDK. */
std::string PassRangeName(uint32_t passIndex) { return "P" + std::to_string(passIndex); }

/**
 * The limiters docs/PROFILING.md names, in NvPerf's spelling — the same set the Vulkan replay
 * collects by default, so a D3D12 capture's GPU Bottlenecks report reads the same columns.
 */
std::vector<std::string> DefaultCounterNames() {
    return {
        "sm__throughput.avg.pct_of_peak_sustained_elapsed",             // overall SM (shader core) throughput
        "gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed",       // VRAM bandwidth
        "sm__warps_active.avg.pct_of_peak_sustained_active",            // achieved occupancy
        "l1tex__throughput.avg.pct_of_peak_sustained_elapsed",          // L1 / texture unit
        "lts__throughput.avg.pct_of_peak_sustained_elapsed",            // L2 cache
        "sm__pipe_alu_cycles_active.avg.pct_of_peak_sustained_active",  // ALU pipe
        "sm__pipe_fma_cycles_active.avg.pct_of_peak_sustained_active",  // FMA pipe
    };
}

} // namespace

/** What a measured pass was, so a range's values can be matched back to it. */
struct DxReplayer::CounterState {
    nvperf::Session session;
    bool ready = false;
    /**
     * The queue the frame's command lists run on, which is the one to profile: the replay submits
     * them on the captured queue it re-created, not on its own upload queue, and a session opened
     * on a queue that never sees the work waits for a trace that never arrives.
     */
    ID3D12CommandQueue* queue = nullptr;
    size_t passes = 1;
    /** Ranges pushed in the round being recorded, by pass index; filled on the first round. */
    struct Range {
        uint32_t command = 0;
        uint32_t frame = 0;
        uint64_t commandBuffer = 0;
        uint32_t passIndex = 0;
    };
    std::unordered_map<uint32_t, Range> ranges;
    /** The list a range is open on, so a pass that ends on another list does not pop the wrong one. */
    ID3D12GraphicsCommandList* openList = nullptr;
};

bool DxReplayer::PrepareCounters() {
    DxCounterReport& out = _report->counters;
    out.requested = true;
    if (!_counters) _counters = new CounterState;
    CounterState& hw = *_counters;

    uint32_t passes = 0;
    if (const JValue* commands = _capture->Commands(); commands && commands->IsArray()) {
        for (uint32_t i = 0; i < commands->count; ++i) {
            const std::string m = Str(commands->items[i].Get("method"));
            if (m == "OMSetRenderTargets" || m == "BeginRenderPass") ++passes;
        }
    }
    if (!passes) {
        out.notes.push_back("the capture has no render passes to measure");
        return false;
    }

    // The queue the frame's first submission uses. A frame that submits on several queues is
    // measured on this one; the others' passes report nothing, and the note says so.
    uint32_t queues = 0;
    if (const JValue* commands = _capture->Commands(); commands && commands->IsArray()) {
        std::vector<uint64_t> seen;
        for (uint32_t i = 0; i < commands->count; ++i) {
            const JValue& c = commands->items[i];
            if (Str(c.Get("method")) != "ExecuteCommandLists") continue;
            const uint64_t id = IdOf(c.Get("object"));
            if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
            seen.push_back(id);
            ++queues;
            if (!hw.queue) hw.queue = static_cast<ID3D12CommandQueue*>(Object(id));
        }
    }
    if (!hw.queue) {
        out.notes.push_back("the frame's command queue was not replayed, so there is nothing to profile");
        return false;
    }
    if (queues > 1)
        out.notes.push_back("the frame submits on " + std::to_string(queues) + " queues; the counters are of the first one's passes");

    std::string note;
    if (!nvperf::Load(note)) {
        out.notes.push_back(note);
        return false;
    }
    if (!hw.session.Init(_device, hw.queue, note)) {
        out.notes.push_back(note);
        return false;
    }
    if (!_options.counters.list && !nvperf::ProfilingPermitted(note)) {
        out.notes.push_back(note);
        return false;
    }
    out.backend = "nvperf";
    out.chip = hw.session.Chip();
    if (_options.counters.list) return true;   // listing needs the evaluator only, not a session

    // The session itself needs GPU performance-counter access enabled (ERR_NVGPUCTRPERM).
    if (!hw.session.Begin(passes + 2, note)) {
        out.notes.push_back(note);
        return false;
    }
    std::vector<std::string> names = _options.counters.names;
    if (names.empty()) names = DefaultCounterNames();
    std::vector<std::string> notes;
    if (!hw.session.Configure(names, 1, out.counters, notes)) {
        for (auto& n : notes) out.notes.push_back(n);
        out.notes.push_back("no counters could be configured");
        return false;
    }
    for (auto& n : notes) out.notes.push_back(n);
    hw.passes = std::max<size_t>(1, hw.session.Passes());
    hw.ready = true;
    return true;
}

void DxReplayer::ListCounters() {
    if (!_counters) return;
    _counters->session.ListMetrics(_report->counters.available);
}

uint32_t DxReplayer::CounterRounds() const {
    return _counters ? (uint32_t)_counters->passes + 1 : 0;
}

namespace {

/** Seconds to give a profiled submission before calling it stuck. */
constexpr uint32_t kCounterWorkTimeoutMs = 30000;

void out_note_timeout(DxReplayReport& report) {
    report.counters.notes.push_back(
        "the profiled submission did not finish within 30 seconds, so the counters were not collected. "
        "That is what a driver that will not profile looks like from here: check that GPU performance "
        "counters are allowed for all users (NVIDIA Control Panel, Developer > Manage GPU Performance "
        "Counters), or run the replay as administrator");
}

} // namespace

/** The round's submissions, waited for with a limit; false when they did not finish in time. */
bool DxReplayer::WaitForCounterWork(CounterState& hw) {
    ID3D12Fence* fence = nullptr;
    if (FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || !fence) return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        fence->Release();
        return false;
    }
    bool ok = false;
    if (SUCCEEDED(hw.queue->Signal(fence, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, event)))
        ok = WaitForSingleObject(event, kCounterWorkTimeoutMs) == WAIT_OBJECT_0;
    CloseHandle(event);
    // The fence outlives the wait only when the work never arrived: releasing it then is safe,
    // since nothing the GPU still holds names it.
    fence->Release();
    return ok;
}

bool DxReplayer::BeginCounterRound() {
    if (!_counters || !_counters->ready) return false;
    if (!_counters->session.BeginPass()) return false;
    _inCounterRound = true;
    return true;
}

bool DxReplayer::EndCounterRound() {
    if (!_counters || !_counters->ready) return false;
    CounterState& hw = *_counters;
    _inCounterRound = false;
    // The frame's submissions were left unwaited for while the pass was open (ReplayCommands), so
    // this is where the round's work is waited for -- with a limit, because a driver that will not
    // profile (counter permission, below) does not fail the submission, it just never finishes it,
    // and EndPass would then block for good.
    if (!WaitForCounterWork(hw)) {
        out_note_timeout(*_report);
        return false;
    }
    if (!hw.session.EndPass()) return false;
    for (ID3D12Resource* r : _transients) r->Release();
    _transients.clear();
    bool done = false;
    std::string error;
    if (!hw.session.Decode(done, error)) {
        _report->counters.notes.push_back(error);
        return false;
    }
    return !done;   // stop the loop as soon as every collection pass has been decoded
}

void DxReplayer::PushCounterRange(ID3D12GraphicsCommandList* list, uint32_t passIndex, uint32_t command, uint32_t frame,
                                  uint64_t listId) {
    CounterState& hw = *_counters;
    if (!hw.ready || !list) return;
    // A pass left open by a list that ended without its marker would nest the next one inside it.
    if (hw.openList) PopCounterRange(hw.openList);
    const std::string name = PassRangeName(passIndex);
    hw.session.PushRange(list, name.c_str());
    hw.openList = list;
    CounterState::Range& range = hw.ranges[passIndex];
    range.command = command;
    range.frame = frame;
    range.commandBuffer = listId;
    range.passIndex = passIndex;
}

void DxReplayer::PopCounterRange(ID3D12GraphicsCommandList* list) {
    CounterState& hw = *_counters;
    if (!hw.ready || !list) return;
    hw.session.PopRange(list);
    if (hw.openList == list) hw.openList = nullptr;
}

void DxReplayer::CompleteCounters() {
    if (!_counters) return;
    CounterState& hw = *_counters;
    DxCounterReport& out = _report->counters;
    out.rounds = (uint32_t)hw.passes;
    if (!hw.ready) return;

    std::vector<std::pair<std::string, std::vector<double>>> results;
    std::string error;
    if (!hw.session.Results(results, error)) {
        out.notes.push_back(error);
        return;
    }
    // Each range comes back by its leaf name ("P0"); what it was measured around is in hw.ranges.
    for (const auto& [leaf, values] : results) {
        if (leaf.size() < 2 || leaf[0] != 'P') continue;
        const uint32_t index = (uint32_t)std::strtoul(leaf.c_str() + 1, nullptr, 10);
        auto it = hw.ranges.find(index);
        if (it == hw.ranges.end()) continue;
        DxCounterRange range;
        range.values = values;
        range.command = it->second.command;
        range.frame = it->second.frame;
        range.commandBuffer = it->second.commandBuffer;
        range.passIndex = it->second.passIndex;
        out.passes.push_back(std::move(range));
    }
    std::sort(out.passes.begin(), out.passes.end(), [](const DxCounterRange& a, const DxCounterRange& b) {
        return a.passIndex < b.passIndex;
    });
    if (out.passes.empty()) out.notes.push_back("the counters were collected but no range matched a pass of the frame");
}

void DxReplayer::DestroyCounters() {
    if (!_counters) return;
    _counters->session.End();
    delete _counters;
    _counters = nullptr;
}

} // namespace dxreplay
