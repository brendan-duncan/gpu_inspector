// What a replayed frame costs, measured: every draw and dispatch timed and counted (--draws), and
// one draw timed again with variants of a shader stage (--ablate). The D3D12 counterpart of the
// Vulkan replay's draw_stats.cpp and ablation.cpp, writing the files they write, so GPU Inspector
// reads both the same way.
//
// A D3D12 capture can already measure its draws while it is taken (src/d3d12/src/capture.cpp,
// BeginDrawQueries). What that cannot do is measure a capture that has already been taken — a file
// somebody sent, a frame that is gone — and it cannot time a draw with a shader the application
// never had. Both need the frame run again, which is what this is.
//
// Queries in D3D12 are resolved by the command list rather than read by the host, so the pattern
// differs from Vulkan's in one way: each list resolves the queries it used into a readback buffer
// before it closes, and the results are read after the submission's wait. A list is recorded whole
// before the next begins, so the slots one takes are a contiguous run.
#include "dx_replayer.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace dxreplay
{

using vkreplay::JValue;

namespace
{

std::string Str(const JValue* v) { return v && v->IsString() ? std::string(v->Str()) : std::string(); }

/** Actions measured per submission; a submission with more is measured up to here. */
constexpr uint32_t kMaxMeasuredActions = 65536;

constexpr uint64_t kTimestampBytes = sizeof(uint64_t);
constexpr uint64_t kStatisticsBytes = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
constexpr uint64_t kOcclusionBytes = sizeof(uint64_t);

double Median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

template <typename T>
void SafeRelease(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

}  // namespace

bool IsActionMethod(const std::string& m)
{
    return m == "DrawInstanced" || m == "DrawIndexedInstanced" || m == "Dispatch" || m == "ExecuteIndirect" || m == "DispatchMesh" ||
        m == "DispatchRays" || m == "ExecuteBundle";
}

struct DxReplayer::MeasureState
{
    // --draws
    ID3D12QueryHeap* timestamps = nullptr;
    ID3D12QueryHeap* statistics = nullptr;
    ID3D12QueryHeap* occlusion = nullptr;
    /** Three regions: timestamps (two per slot), statistics, occlusion. */
    ID3D12Resource* readback = nullptr;
    uint32_t capacity = 0;
    uint32_t slot = 0;        // the next free one, over the submission
    uint32_t listFirst = 0;   // the first the list being recorded took
    struct Pending
    {
        DxDrawResult result;
        uint32_t slot = 0;
    };
    std::vector<Pending> pending;
    /** Which slots began a statistics and an occlusion query: only those may be resolved. */
    std::vector<uint8_t> flags;
    static constexpr uint8_t kCounted = 1, kSampled = 2;

    // --ablate
    ID3D12QueryHeap* ablationTimestamps = nullptr;
    ID3D12Resource* ablationReadback = nullptr;
    std::vector<uint32_t> ablationBase;                 // per target: its first query
    std::unordered_map<uint32_t, size_t> ablationAt;    // command -> target
    std::map<std::tuple<uint64_t, size_t, int>, ID3D12PipelineState*> pipelines;
    struct PendingAblation
    {
        size_t result = 0;
        uint32_t base = 0;
        std::vector<bool> issued;
    };
    std::vector<PendingAblation> pendingAblations;
    /** The query ranges the list being recorded wrote, resolved before it closes. */
    std::vector<std::pair<uint32_t, uint32_t>> ablationRanges;
};

bool DxReplayer::PrepareMeasurements()
{
    _measure = new MeasureState();
    MeasureState& s = *_measure;
    auto readbackBuffer = [&](uint64_t bytes) -> ID3D12Resource* {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* buffer = nullptr;
        if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))))
            return nullptr;
        return buffer;
    };
    auto queryHeap = [&](D3D12_QUERY_HEAP_TYPE type, uint32_t count) -> ID3D12QueryHeap* {
        D3D12_QUERY_HEAP_DESC desc{};
        desc.Type = type;
        desc.Count = count;
        ID3D12QueryHeap* heap = nullptr;
        if (FAILED(_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap))))
            return nullptr;
        return heap;
    };

    if (_options.drawStats)
    {
        // The most actions any one submission has: the slots start over with each.
        uint32_t actions = 0, inSubmission = 0;
        if (const JValue* commands = _capture->Commands(); commands && commands->IsArray())
        {
            for (uint32_t i = 0; i < commands->count; ++i)
            {
                const std::string m = Str(commands->items[i].Get("method"));
                if (m == "ExecuteCommandLists")
                    inSubmission = 0;
                else if (IsActionMethod(m) && !commands->items[i].Get("secondary"))
                    actions = std::max(actions, ++inSubmission);
            }
        }
        if (!actions)
        {
            _report->drawStatsNote = "the capture has no draws or dispatches to measure";
        }
        else
        {
            if (actions > kMaxMeasuredActions)
            {
                _report->drawStatsNote = "a submission has " + std::to_string(actions) + " draws and dispatches; the first " +
                    std::to_string(kMaxMeasuredActions) + " of each are measured";
                actions = kMaxMeasuredActions;
            }
            s.capacity = actions;
            s.timestamps = queryHeap(D3D12_QUERY_HEAP_TYPE_TIMESTAMP, actions * 2);
            s.statistics = queryHeap(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, actions);
            s.occlusion = queryHeap(D3D12_QUERY_HEAP_TYPE_OCCLUSION, actions);
            s.readback = readbackBuffer((uint64_t)actions * (2 * kTimestampBytes + kStatisticsBytes + kOcclusionBytes));
            s.flags.assign(actions, 0);
            if (!s.readback || (!s.timestamps && !s.statistics && !s.occlusion))
            {
                _report->drawStatsNote = "this device made none of the query heaps the measurement needs";
                s.capacity = 0;
            }
            else if (!s.timestamps)
            {
                _report->drawStatsNote = "this device made no timestamp query heap, so the draws carry counters only";
            }
        }
    }

    if (_options.ablation.enabled)
    {
        const uint32_t rounds = std::max(1u, _options.ablation.rounds) + 1;
        uint32_t queries = 0;
        for (size_t t = 0; t < _options.ablation.targets.size(); ++t)
        {
            const auto& target = _options.ablation.targets[t];
            s.ablationBase.push_back(queries);
            s.ablationAt[target.command] = t;
            queries += rounds * ((uint32_t)target.variants.size() + 1) * 2;
        }
        if (queries)
        {
            s.ablationTimestamps = queryHeap(D3D12_QUERY_HEAP_TYPE_TIMESTAMP, queries);
            s.ablationReadback = readbackBuffer((uint64_t)queries * kTimestampBytes);
        }
    }
    return true;
}

void DxReplayer::DestroyMeasurements()
{
    if (!_measure)
        return;
    MeasureState& s = *_measure;
    for (auto& [key, pipeline] : s.pipelines)
        SafeRelease(pipeline);
    SafeRelease(s.timestamps);
    SafeRelease(s.statistics);
    SafeRelease(s.occlusion);
    SafeRelease(s.readback);
    SafeRelease(s.ablationTimestamps);
    SafeRelease(s.ablationReadback);
    delete _measure;
    _measure = nullptr;
}

void DxReplayer::BeginListMeasurements()
{
    if (!_measure)
        return;
    _measure->listFirst = _measure->slot;
    _measure->ablationRanges.clear();
}

int DxReplayer::BeginDrawQuery(ID3D12GraphicsCommandList* list, uint32_t command, uint32_t frame, uint64_t listId, uint32_t passIndex)
{
    if (!_measure || !_measure->capacity)
        return -1;
    MeasureState& s = *_measure;
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return -1;
    if (s.slot >= s.capacity)
        return -1;
    const uint32_t slot = s.slot++;
    MeasureState::Pending p;
    p.slot = slot;
    p.result.command = command;
    p.result.frame = frame;
    p.result.commandList = listId;
    p.result.passIndex = passIndex;
    p.result.timed = s.timestamps != nullptr;
    // Statistics and occlusion are a direct list's; and occlusion is not begun inside a query of
    // the capture's own, where the two would be one query to the hardware.
    const bool graphics = type == D3D12_COMMAND_LIST_TYPE_DIRECT;
    p.result.counted = graphics && s.statistics != nullptr;
    p.result.sampled = graphics && s.occlusion != nullptr && _appQueryDepth == 0;
    s.flags[slot] = (p.result.counted ? MeasureState::kCounted : 0) | (p.result.sampled ? MeasureState::kSampled : 0);
    if (p.result.timed)
        list->EndQuery(s.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    if (p.result.counted)
        list->BeginQuery(s.statistics, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, slot);
    if (p.result.sampled)
        list->BeginQuery(s.occlusion, D3D12_QUERY_TYPE_OCCLUSION, slot);
    s.pending.push_back(p);
    return (int)s.pending.size() - 1;
}

void DxReplayer::EndDrawQuery(ID3D12GraphicsCommandList* list, int pending)
{
    if (!_measure || pending < 0 || (size_t)pending >= _measure->pending.size())
        return;
    MeasureState& s = *_measure;
    const MeasureState::Pending& p = s.pending[(size_t)pending];
    if (p.result.sampled)
        list->EndQuery(s.occlusion, D3D12_QUERY_TYPE_OCCLUSION, p.slot);
    if (p.result.counted)
        list->EndQuery(s.statistics, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, p.slot);
    if (p.result.timed)
        list->EndQuery(s.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, p.slot * 2 + 1);
}

void DxReplayer::ResolveListMeasurements(ID3D12GraphicsCommandList* list)
{
    if (!_measure)
        return;
    MeasureState& s = *_measure;
    const uint32_t first = s.listFirst, count = s.slot - s.listFirst;
    if (count)
    {
        const uint64_t statisticsBase = (uint64_t)s.capacity * 2 * kTimestampBytes;
        const uint64_t occlusionBase = statisticsBase + (uint64_t)s.capacity * kStatisticsBytes;
        if (s.timestamps)
            list->ResolveQueryData(s.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, first * 2, count * 2, s.readback, (uint64_t)first * 2 * kTimestampBytes);
        // A query that was never begun may not be resolved, so these go by runs of the slots that began one.
        auto runs = [&](uint8_t bit, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, uint64_t base, uint64_t bytes) {
            if (!heap)
                return;
            for (uint32_t i = first; i < first + count;)
            {
                if (!(s.flags[i] & bit))
                {
                    ++i;
                    continue;
                }
                uint32_t end = i;
                while (end + 1 < first + count && (s.flags[end + 1] & bit))
                    ++end;
                list->ResolveQueryData(heap, type, i, end - i + 1, s.readback, base + (uint64_t)i * bytes);
                i = end + 1;
            }
        };
        runs(MeasureState::kCounted, s.statistics, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, statisticsBase, kStatisticsBytes);
        runs(MeasureState::kSampled, s.occlusion, D3D12_QUERY_TYPE_OCCLUSION, occlusionBase, kOcclusionBytes);
    }
    for (const auto& [base, queries] : s.ablationRanges)
        list->ResolveQueryData(s.ablationTimestamps, D3D12_QUERY_TYPE_TIMESTAMP, base, queries, s.ablationReadback, (uint64_t)base * kTimestampBytes);
    s.ablationRanges.clear();
}

void DxReplayer::CompleteMeasurements(ID3D12CommandQueue* queue, bool ran)
{
    if (!_measure)
        return;
    MeasureState& s = *_measure;
    UINT64 frequency = 0;
    if (FAILED(queue->GetTimestampFrequency(&frequency)))
        frequency = 0;
    const double msPerTick = frequency ? 1000.0 / (double)frequency : 0;

    if (!s.pending.empty() && ran && s.readback)
    {
        void* mapped = nullptr;
        const D3D12_RANGE read{0, (SIZE_T)((uint64_t)s.capacity * (2 * kTimestampBytes + kStatisticsBytes + kOcclusionBytes))};
        if (SUCCEEDED(s.readback->Map(0, &read, &mapped)) && mapped)
        {
            const auto* bytes = static_cast<const uint8_t*>(mapped);
            const uint64_t statisticsBase = (uint64_t)s.capacity * 2 * kTimestampBytes;
            const uint64_t occlusionBase = statisticsBase + (uint64_t)s.capacity * kStatisticsBytes;
            for (MeasureState::Pending& p : s.pending)
            {
                DxDrawResult d = p.result;
                if (d.timed && msPerTick > 0)
                {
                    uint64_t stamps[2];
                    std::memcpy(stamps, bytes + (uint64_t)p.slot * 2 * kTimestampBytes, sizeof(stamps));
                    d.durationMs = stamps[1] > stamps[0] ? (double)(stamps[1] - stamps[0]) * msPerTick : 0;
                }
                else
                {
                    d.timed = false;
                }
                if (d.counted)
                {
                    D3D12_QUERY_DATA_PIPELINE_STATISTICS stats{};
                    std::memcpy(&stats, bytes + statisticsBase + (uint64_t)p.slot * kStatisticsBytes, sizeof(stats));
                    d.primitives = stats.IAPrimitives;
                    d.vertexInvocations = stats.VSInvocations;
                    d.fragmentInvocations = stats.PSInvocations;
                    d.computeInvocations = stats.CSInvocations;
                }
                if (d.sampled)
                    std::memcpy(&d.samplesPassed, bytes + occlusionBase + (uint64_t)p.slot * kOcclusionBytes, sizeof(uint64_t));
                _report->draws.push_back(d);
            }
            const D3D12_RANGE none{0, 0};
            s.readback->Unmap(0, &none);
        }
    }
    s.pending.clear();
    s.slot = 0;
    std::fill(s.flags.begin(), s.flags.end(), (uint8_t)0);

    for (MeasureState::PendingAblation& p : s.pendingAblations)
    {
        DxAblationResult& r = _report->ablations[p.result];
        void* mapped = nullptr;
        if (!ran || msPerTick <= 0 || !s.ablationReadback || FAILED(s.ablationReadback->Map(0, nullptr, &mapped)) || !mapped)
        {
            r.note = !ran ? "the submission holding the command did not run" : "the queue reports no timestamp frequency";
            continue;
        }
        const auto* stamps = static_cast<const uint64_t*>(mapped);
        const size_t count = r.variants.size() + 1;
        const uint32_t rounds = r.rounds + 1;
        std::vector<std::vector<double>> samples(count);
        for (uint32_t round = 1; round < rounds; ++round)
        {   // the first round only warms up
            for (size_t v = 0; v < count; ++v)
            {
                if (!p.issued[round * count + v])
                    continue;
                const uint32_t query = p.base + (uint32_t)((round * count + v) * 2);
                if (stamps[query + 1] >= stamps[query])
                    samples[v].push_back((double)(stamps[query + 1] - stamps[query]) * msPerTick / std::max(1u, r.repeat));
            }
        }
        const D3D12_RANGE none{0, 0};
        s.ablationReadback->Unmap(0, &none);
        auto fill = [&](DxAblationTiming& timing, std::vector<double>& v) {
            timing.samples = v;
            timing.measured = !v.empty();
            timing.ms = Median(v);
        };
        fill(r.baseline, samples[0]);
        for (size_t v = 1; v < count; ++v)
            fill(r.variants[v - 1], samples[v]);
    }
    s.pendingAblations.clear();
}

// ---------------------------------------------------------------------------------------------
// Ablation
//
// What a part of a shader costs, measured by taking it out: the request brings variants of one
// stage of a draw's (or dispatch's) pipeline, each with a function, a source line or a texture made
// constant (src/app/src/renderer/d3d12/dxil_ablate.ts), and the replay times the draw with each.
//
// The draw is issued again right before it runs in the replayed frame, inside its own pass and
// command list, so everything it reads is bound and every pixel it covers has the depth the draw
// itself met. Each round issues it with a copy of its pipeline holding the unchanged shader (the
// baseline) and with every variant: set, drawn once untimed, then drawn `repeat` times between a
// pair of timestamps. The rounds rotate the order, and a first round warms the pipelines up and is
// not counted. The copies write no depth and no stencil, so every issue is tested against the same
// depth as the draw itself. Then the draw's own pipeline is set again and the draw runs as captured.
//
// A time is not what the draw costs alone (the GPU pipelines work, so a timestamp also sees what
// came just before), but that is the same for every pipeline of a round, which is why the baseline
// is issued in each round and a variant's cost is the difference of medians.

ID3D12PipelineState* DxReplayer::AblationPipeline(uint64_t pipelineId, size_t target, int variant)
{
    MeasureState& s = *_measure;
    const auto key = std::make_tuple(pipelineId, target, variant);
    if (auto it = s.pipelines.find(key); it != s.pipelines.end())
        return it->second;
    s.pipelines[key] = nullptr;   // a copy that cannot be made is not tried again
    const JValue* object = _capture->Object(pipelineId);
    const JValue* args = object ? object->Get("args") : nullptr;
    const JValue* json = args ? args->Get("pDesc") : nullptr;
    if (!object || !json || json->IsNull())
        return nullptr;
    const std::string cmd = Str(object->Get("cmd"));
    const bool stream = cmd == "CreatePipelineState" || cmd == "LoadPipeline";
    const bool compute = cmd == "CreateComputePipelineState" || cmd == "LoadComputePipeline" || (stream && json->Get("CS") && !json->Get("CS")->IsNull());
    const auto& t = _options.ablation.targets[target];

    // The description decoded again, with the variant's code where the stage's own would go.
    _currentObject = object;
    _overrideStage = variant >= 0 ? t.stage : std::string();
    _overrideCode = variant >= 0 ? &t.variants[(size_t)variant].code : nullptr;
    const size_t unresolved = _env.unresolved;
    const size_t problems = _env.problems.size();
    ID3D12PipelineState* pipeline = nullptr;
    if (compute)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        DecodeStruct(_env, *args, "pDesc", desc);
        desc.CachedPSO = {};
        if (_env.unresolved == unresolved)
            _device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
    }
    else
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        DecodeStruct(_env, *args, "pDesc", desc);
        if (desc.SampleDesc.Count == 0)
            desc.SampleDesc.Count = 1;
        if (stream && !json->Get("SampleMask"))
            desc.SampleMask = UINT_MAX;
        desc.CachedPSO = {};
        // No depth or stencil written: every issue meets the depth the draw itself meets.
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.StencilWriteMask = 0;
        if (_env.unresolved == unresolved)
            _device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    }
    _currentObject = nullptr;
    _overrideStage.clear();
    _overrideCode = nullptr;
    _env.problems.resize(problems);
    _env.unresolved = unresolved;
    s.pipelines[key] = pipeline;
    return pipeline;
}

void DxReplayer::IssueAblation(uint32_t index, const std::string& method, const JValue& command, const JValue* args,
    ID3D12GraphicsCommandList* list, uint64_t listId, uint32_t frame, uint32_t passIndex)
{
    if (!_measure)
        return;
    MeasureState& s = *_measure;
    auto found = s.ablationAt.find(index);
    if (found == s.ablationAt.end())
        return;
    const size_t t = found->second;
    const auto& target = _options.ablation.targets[t];
    DxAblationResult result;
    result.command = index;
    result.stage = target.stage;
    result.frame = frame;
    result.commandList = listId;
    result.passIndex = passIndex;
    result.rounds = std::max(1u, _options.ablation.rounds);
    result.repeat = std::max(1u, target.repeat);
    result.baseline.name = "baseline";
    for (const auto& v : target.variants)
        result.variants.push_back({v.name});
    result.pipeline = _boundPipeline;
    auto fail = [&](std::string why) {
        result.note = std::move(why);
        _report->ablations.push_back(std::move(result));
    };
    const bool action = method == "DrawInstanced" || method == "DrawIndexedInstanced" || method == "Dispatch" || method == "ExecuteIndirect" || method == "DispatchMesh";
    if (!s.ablationTimestamps || !s.ablationReadback)
        return fail("this device made no timestamp query heap");
    if (!action)
        return fail(method + " is not a draw or a dispatch the replay can issue again");
    if (!result.pipeline)
        return fail("no pipeline is set at the command");
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return fail("the command is in a list that cannot write timestamps");

    // Pipelines: [0] the baseline, then one per variant.
    const size_t count = target.variants.size() + 1;
    std::vector<ID3D12PipelineState*> pipelines(count, nullptr);
    pipelines[0] = AblationPipeline(result.pipeline, t, -1);
    if (!pipelines[0])
        return fail("the pipeline could not be made again (the capture has no description of it, or the copy was refused)");
    for (size_t v = 1; v < count; ++v)
    {
        pipelines[v] = AblationPipeline(result.pipeline, t, (int)v - 1);
        if (!pipelines[v])
            result.variants[v - 1].note = "the variant's pipeline could not be created";
    }

    const uint32_t rounds = result.rounds + 1;
    MeasureState::PendingAblation pending;
    pending.result = _report->ablations.size();
    pending.base = s.ablationBase[t];
    pending.issued.assign(rounds * count, false);
    const size_t problems = _report->problems.size();
    for (uint32_t r = 0; r < rounds; ++r)
    {
        for (size_t k = 0; k < count; ++k)
        {
            const size_t v = (k + r) % count;
            if (!pipelines[v])
                continue;
            const uint32_t query = pending.base + (uint32_t)((r * count + v) * 2);
            list->SetPipelineState(pipelines[v]);
            // Once untimed: what a driver does at the first draw after a pipeline change stays out of the span.
            IssueCommand(index, method, command, args, list, listId);
            list->EndQuery(s.ablationTimestamps, D3D12_QUERY_TYPE_TIMESTAMP, query);
            for (uint32_t n = 0; n < result.repeat; ++n)
                IssueCommand(index, method, command, args, list, listId);
            list->EndQuery(s.ablationTimestamps, D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
            pending.issued[r * count + v] = true;
            // Only what was written is resolved: a variant whose pipeline was refused leaves its
            // pair unwritten, and resolving a query that never ended is an error.
            if (!s.ablationRanges.empty() && s.ablationRanges.back().first + s.ablationRanges.back().second == query)
                s.ablationRanges.back().second += 2;
            else
                s.ablationRanges.emplace_back(query, 2u);
        }
    }
    // Whatever the repeats said about the command, the captured issue after this says once.
    _report->problems.resize(problems);

    // The list's own pipeline again.
    if (auto own = static_cast<ID3D12PipelineState*>(Object(result.pipeline)))
        list->SetPipelineState(own);
    _report->ablations.push_back(std::move(result));
    s.pendingAblations.push_back(std::move(pending));
}

}  // namespace dxreplay
