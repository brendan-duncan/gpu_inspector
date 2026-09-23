#include "replayer.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "util.h"

namespace vkreplay
{

// ---------------------------------------------------------------------------------------------
// Ablation
//
// What a part of a shader costs, measured by taking it out: the request brings variants of one stage
// of a draw's (or dispatch's) pipeline, each with a function or a source line made constant
// (src/app/src/renderer/vulkan/spirv_ablate.ts), and the replay times the draw with each.
//
// The draw is issued again right before it runs in the replayed frame, inside its own pass and
// command buffer, so everything it reads is bound and every pixel it covers has the depth the draw
// itself met. Each round issues it with a copy of its pipeline holding the unchanged shader (the
// baseline) and with every variant: bound, drawn once untimed, then drawn `repeat` times between a
// pair of timestamps. The rounds rotate the order, and a first round warms the pipelines up and is
// not counted. A single draw is too short a span: what it costs to begin and end one (a few tens of
// microseconds on an RTX 4080, and not the same for every pipeline) would be charged to the shader. The copies write no depth and
// no stencil, so every issue is tested against the same depth, with the same early rejection, as the
// draw itself. Then the draw's own pipeline and the dynamic write state it had are put back, and the
// draw runs as captured.
//
// A time is not what the draw costs alone (the GPU pipelines work, so a timestamp also sees what came
// just before), but that is the same for every pipeline of a round, which is why the baseline is
// issued in each round and a variant's cost is the difference of medians.

namespace
{

/** The stage bit a stage name means. */
VkShaderStageFlagBits StageBit(const std::string& stage)
{
    if (stage == "vertex")
        return VK_SHADER_STAGE_VERTEX_BIT;
    if (stage == "fragment")
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (stage == "compute")
        return VK_SHADER_STAGE_COMPUTE_BIT;
    if (stage == "geometry")
        return VK_SHADER_STAGE_GEOMETRY_BIT;
    if (stage == "tess_control")
        return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (stage == "tess_eval")
        return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    return VkShaderStageFlagBits(0);
}

double Median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

}  // namespace

void Replayer::NoteStreamCommand(StreamState& stream, const std::string& method, const JValue& args, uint32_t index)
{
    if (method == "vkCmdBindPipeline")
    {
        const std::string point = Str(args.Get("pipelineBindPoint"));
        if (point == "VK_PIPELINE_BIND_POINT_GRAPHICS")
            stream.graphicsPipeline = IdOf(args.Get("pipeline"));
        else if (point == "VK_PIPELINE_BIND_POINT_COMPUTE")
            stream.computePipeline = IdOf(args.Get("pipeline"));
    }
    else if (method == "vkCmdSetDepthWriteEnable" || method == "vkCmdSetDepthWriteEnableEXT")
    {
        stream.depthWriteCommands = {index};   // the last one is what is in effect
    }
    else if (method == "vkCmdSetStencilWriteMask")
    {
        stream.stencilWriteCommands.push_back(index);   // front and back may be set apart
    }
}

bool Replayer::PrepareAblation()
{
    _ablationBase.clear();
    _ablationTargets.clear();
    VkPhysicalDeviceProperties props{};
    _fns.GetPhysicalDeviceProperties(_physical, &props);
    uint32_t familyCount = 0;
    _fns.GetPhysicalDeviceQueueFamilyProperties(_physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    if (familyCount)
        _fns.GetPhysicalDeviceQueueFamilyProperties(_physical, &familyCount, families.data());
    if (props.limits.timestampPeriod <= 0 || _queueFamily >= families.size() || !families[_queueFamily].timestampValidBits)
        return false;
    _timestampPeriod = props.limits.timestampPeriod;

    const uint32_t rounds = std::max(1u, _options.ablation.rounds) + 1;
    uint32_t queries = 0;
    for (size_t t = 0; t < _options.ablation.targets.size(); ++t)
    {
        const auto& target = _options.ablation.targets[t];
        _ablationBase.push_back(queries);
        _ablationTargets[target.command] = t;
        queries += rounds * ((uint32_t)target.variants.size() + 1) * 2;
    }
    if (!queries)
        return false;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = queries;
    if (_fns.CreateQueryPool(_device, &info, nullptr, &_ablationPool) != VK_SUCCESS)
        _ablationPool = VK_NULL_HANDLE;
    return _ablationPool != VK_NULL_HANDLE;
}

void Replayer::ResetAblationQueries(VkCommandBuffer cb, const CommandGroup& group)
{
    if (!_ablationPool)
        return;
    const uint32_t rounds = std::max(1u, _options.ablation.rounds) + 1;
    for (size_t t = 0; t < _options.ablation.targets.size(); ++t)
    {
        const auto& target = _options.ablation.targets[t];
        if (target.command <= group.first || target.command >= group.last)
            continue;
        _fns.CmdResetQueryPool(cb, _ablationPool, _ablationBase[t], rounds * ((uint32_t)target.variants.size() + 1) * 2);
    }
}

VkPipeline Replayer::AblationPipeline(uint64_t pipelineId, size_t target, int variant, bool compute)
{
    const auto key = std::make_tuple(pipelineId, target, variant);
    if (auto it = _ablationPipelines.find(key); it != _ablationPipelines.end())
        return it->second;
    _ablationPipelines[key] = VK_NULL_HANDLE;   // a copy that cannot be made is not tried again
    const auto& t = _options.ablation.targets[target];
    const VkShaderStageFlagBits stage = StageBit(t.stage);
    // The variant's module, or none for the baseline, whose stage keeps the capture's own code.
    VkShaderModule module = VK_NULL_HANDLE;
    if (variant >= 0)
    {
        const auto& words = t.variants[(size_t)variant].words;
        VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        m.codeSize = words.size() * 4;
        m.pCode = words.data();
        if (words.empty() || _fns.CreateShaderModule(_device, &m, nullptr, &module) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (compute)
    {
        const JValue* object = _capture->Object(pipelineId);
        const JValue* args = object ? object->Get("args") : nullptr;
        if (object && args && Str(object->Get("cmd")) == "vkCreateComputePipelines")
        {
            const uint32_t index = object->Get("index") ? (uint32_t)object->Get("index")->Uint() : 0;
            const size_t unresolved = _ctx.unresolved;
            const size_t problems = _ctx.problems.size();
            Args_vkCreateComputePipelines a{};
            DecodeArgs(_ctx, *args, a);
            const bool usable = a.pCreateInfos && index < a.createInfoCount && _ctx.unresolved == unresolved;
            _ctx.problems.resize(problems);
            _ctx.unresolved = unresolved;
            if (usable)
            {
                VkComputePipelineCreateInfo info = a.pCreateInfos[index];
                VkShaderModule own = VK_NULL_HANDLE;
                if (!module)
                    own = ModuleFromBlob(*object, std::string("compute:") + (info.stage.pName ? info.stage.pName : "main"));
                info.stage.module = module ? module : own;
                info.stage.pNext = nullptr;
                info.flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
                info.basePipelineHandle = VK_NULL_HANDLE;
                info.basePipelineIndex = -1;
                if (info.stage.module && _fns.CreateComputePipelines(_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS)
                    Track("VkPipeline", (uint64_t)pipeline);
                else
                    pipeline = VK_NULL_HANDLE;
                if (own)
                    _fns.DestroyShaderModule(_device, own, nullptr);
            }
            _arena.Reset();
        }
    }
    else
    {
        pipeline = CopyGraphicsPipeline(pipelineId, "ablation", [&](PipelineCopy& p) {
            auto s = std::find_if(p.stages.begin(), p.stages.end(), [&](const VkPipelineShaderStageCreateInfo& st) { return st.stage == stage; });
            if (s == p.stages.end())
                return false;
            if (module)
            {
                s->module = module;
                s->pNext = nullptr;
            }
            // No depth or stencil written: every issue meets the depth the draw itself meets. Where
            // the pipeline takes them dynamically, they are set before the issues instead.
            const bool depthDynamic = p.HasDynamic(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE);
            const bool stencilDynamic = p.HasDynamic(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
            _ablationDynamic[pipelineId] = {depthDynamic, stencilDynamic};
            if (p.hasDepthStencil)
            {
                if (!depthDynamic)
                    p.depthStencil.depthWriteEnable = VK_FALSE;
                if (!stencilDynamic)
                    p.depthStencil.front.writeMask = p.depthStencil.back.writeMask = 0;
            }
            return true;
        });
    }
    if (module)
        _fns.DestroyShaderModule(_device, module, nullptr);
    _ablationPipelines[key] = pipeline;
    return pipeline;
}

void Replayer::IssueAblation(VkCommandBuffer cb, uint32_t index, const std::string& method, const JValue& args, uint32_t frame,
    uint64_t commandBuffer, uint32_t passIndex, const StreamState& stream)
{
    auto found = _ablationTargets.find(index);
    if (found == _ablationTargets.end())
        return;
    const size_t t = found->second;
    const auto& target = _options.ablation.targets[t];
    AblationResult result;
    result.command = index;
    result.stage = target.stage;
    result.frame = frame;
    result.commandBuffer = commandBuffer;
    result.passIndex = passIndex;
    result.rounds = std::max(1u, _options.ablation.rounds);
    result.repeat = std::max(1u, target.repeat);
    result.baseline.name = "baseline";
    for (const auto& v : target.variants)
        result.variants.push_back({v.name});
    const bool compute = StartsWith(method, "vkCmdDispatch");
    result.pipeline = compute ? stream.computePipeline : stream.graphicsPipeline;
    auto fail = [&](std::string why) {
        result.note = std::move(why);
        _report->ablations.push_back(std::move(result));
    };
    if (!_ablationPool)
        return fail("the replay's queue writes no timestamps");
    if (!StartsWith(method, "vkCmdDraw") && !compute)
        return fail(method + " is not a draw or a dispatch");
    if (!result.pipeline)
        return fail("no pipeline is bound at the command");
    if (!StageBit(target.stage))
        return fail("the " + target.stage + " stage cannot be measured");
    if (!compute && _passViews > 1)
        return fail("draws in multiview passes are not measured yet (each timestamp takes one query per view)");

    // Pipelines: [0] the baseline, then one per variant.
    const size_t count = target.variants.size() + 1;
    std::vector<VkPipeline> pipelines(count, VK_NULL_HANDLE);
    pipelines[0] = AblationPipeline(result.pipeline, t, -1, compute);
    if (!pipelines[0])
        return fail("the pipeline could not be copied (the capture has no code for its " + target.stage + " stage, or the copy was refused)");
    for (size_t v = 1; v < count; ++v)
    {
        pipelines[v] = AblationPipeline(result.pipeline, t, (int)v - 1, compute);
        if (!pipelines[v])
            result.variants[v - 1].note = "the variant's pipeline could not be created";
    }
    const VkPipelineBindPoint point = compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    const auto dynamic = _ablationDynamic.count(result.pipeline) ? _ablationDynamic[result.pipeline] : std::make_pair(false, false);
    if (!compute && dynamic.first)
    {
        if (_fns.CmdSetDepthWriteEnable)
            _fns.CmdSetDepthWriteEnable(cb, VK_FALSE);
        else if (_fns.CmdSetDepthWriteEnableEXT)
            _fns.CmdSetDepthWriteEnableEXT(cb, VK_FALSE);
    }
    if (!compute && dynamic.second)
        _fns.CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

    ReplayFn fn = FindReplayCommand(method);
    const uint32_t rounds = result.rounds + 1;
    PendingAblation pending;
    pending.result = _report->ablations.size();
    pending.base = _ablationBase[t];
    pending.issued.assign(rounds * count, false);
    const size_t problems = _ctx.problems.size();
    const size_t unresolved = _ctx.unresolved;
    for (uint32_t r = 0; r < rounds && fn; ++r)
    {
        for (size_t k = 0; k < count; ++k)
        {
            const size_t v = (k + r) % count;
            if (!pipelines[v])
                continue;
            const uint32_t query = pending.base + (uint32_t)((r * count + v) * 2);
            _fns.CmdBindPipeline(cb, point, pipelines[v]);
            // Once untimed: what a driver does at the first draw after a bind stays out of the span.
            fn(_ctx, args, cb);
            _arena.Reset();
            _fns.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _ablationPool, query);
            for (uint32_t n = 0; n < result.repeat; ++n)
            {
                fn(_ctx, args, cb);
                _arena.Reset();
            }
            _fns.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _ablationPool, query + 1);
            pending.issued[r * count + v] = true;
        }
    }
    _ctx.problems.resize(problems);
    _ctx.unresolved = unresolved;

    // The command buffer's own state again: its pipeline, and the write state it set.
    _fns.CmdBindPipeline(cb, point, (VkPipeline)Handle(result.pipeline));
    for (const std::vector<uint32_t>* list : {&stream.depthWriteCommands, &stream.stencilWriteCommands})
    {
        for (uint32_t i : *list)
        {
            const JValue& c = _capture->Commands()->items[i];
            if (ReplayFn restore = FindReplayCommand(Str(c.Get("method"))); restore && c.Get("args"))
                restore(_ctx, *c.Get("args"), cb);
            _arena.Reset();
        }
    }
    _ctx.problems.resize(problems);
    _ctx.unresolved = unresolved;
    _report->ablations.push_back(std::move(result));
    _pendingAblations.push_back(std::move(pending));
}

void Replayer::CompleteAblation(bool submitted)
{
    for (PendingAblation& p : _pendingAblations)
    {
        AblationResult& r = _report->ablations[p.result];
        if (!submitted)
        {
            r.note = "the submission holding the command did not run";
            continue;
        }
        const size_t count = r.variants.size() + 1;
        const uint32_t rounds = r.rounds + 1;
        std::vector<std::vector<double>> samples(count);
        for (uint32_t round = 1; round < rounds; ++round)
        {   // the first round only warms up
            for (size_t v = 0; v < count; ++v)
            {
                if (!p.issued[round * count + v])
                    continue;
                uint64_t stamps[2] = {0, 0};
                const uint32_t query = p.base + (uint32_t)((round * count + v) * 2);
                if (_fns.GetQueryPoolResults(_device, _ablationPool, query, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
                    continue;
                if (stamps[1] >= stamps[0])
                    samples[v].push_back((double)(stamps[1] - stamps[0]) * _timestampPeriod / 1e6 / std::max(1u, r.repeat));
            }
        }
        auto fill = [&](AblationTiming& timing, std::vector<double>& s) {
            timing.samples = s;
            timing.measured = !s.empty();
            timing.ms = Median(s);
        };
        fill(r.baseline, samples[0]);
        for (size_t v = 1; v < count; ++v)
            fill(r.variants[v - 1], samples[v]);
    }
    _pendingAblations.clear();
}

void Replayer::DestroyAblation()
{
    if (_ablationPool)
        _fns.DestroyQueryPool(_device, _ablationPool, nullptr);
    _ablationPool = VK_NULL_HANDLE;
    _pendingAblations.clear();
}

}  // namespace vkreplay
