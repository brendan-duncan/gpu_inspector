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
// A draw or a dispatch bound with shader objects has no pipeline to copy: the stage's shader object is
// made again with each variant's code and bound in its place (the baseline is the draw's own), and the
// depth and stencil writes, which are dynamic state for every such draw, are turned off the same way.
//
// A stage before rasterization (vertex, tessellation, geometry) is timed with rasterization discarded,
// the baseline and every variant alike. Its outputs decide what is rasterized: a variant that loses
// its position rasterizes nothing, and with the fragment stage running, the fragment work that goes
// with it would be charged to the vertex code. Without rasterization, what a variant saves is that
// stage's work, and the baseline is the draw's pre-rasterization work alone.
//
// A time is not what the draw costs alone (the GPU pipelines work, so a timestamp also sees what came
// just before), but that is the same for every pipeline of a round, which is why the baseline is
// issued in each round and a variant's cost is the difference of medians.

namespace
{

/** Whether a stage runs before rasterization, and is timed with it discarded. */
bool BeforeRasterization(VkShaderStageFlagBits stage)
{
    return stage == VK_SHADER_STAGE_VERTEX_BIT || stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ||
        stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT || stage == VK_SHADER_STAGE_GEOMETRY_BIT;
}

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

/** The stage bit a VkShaderStageFlagBits name means ("VK_SHADER_STAGE_VERTEX_BIT"). */
uint32_t StageBitOfName(const std::string& name)
{
    static const std::pair<const char*, uint32_t> kStages[] = {
        {"VK_SHADER_STAGE_VERTEX_BIT", VK_SHADER_STAGE_VERTEX_BIT},
        {"VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT", VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT},
        {"VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT", VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT},
        {"VK_SHADER_STAGE_GEOMETRY_BIT", VK_SHADER_STAGE_GEOMETRY_BIT},
        {"VK_SHADER_STAGE_FRAGMENT_BIT", VK_SHADER_STAGE_FRAGMENT_BIT},
        {"VK_SHADER_STAGE_COMPUTE_BIT", VK_SHADER_STAGE_COMPUTE_BIT},
        {"VK_SHADER_STAGE_TASK_BIT_EXT", VK_SHADER_STAGE_TASK_BIT_EXT},
        {"VK_SHADER_STAGE_MESH_BIT_EXT", VK_SHADER_STAGE_MESH_BIT_EXT},
    };
    for (const auto& [n, bit] : kStages)
        if (name == n)
            return bit;
    return 0;
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
        // A pipeline unbinds the shader objects of its bind point's stages.
        const std::string point = Str(args.Get("pipelineBindPoint"));
        if (point == "VK_PIPELINE_BIND_POINT_GRAPHICS")
        {
            stream.graphicsPipeline = IdOf(args.Get("pipeline"));
            for (auto it = stream.shaders.begin(); it != stream.shaders.end();)
                it = it->first != VK_SHADER_STAGE_COMPUTE_BIT ? stream.shaders.erase(it) : std::next(it);
        }
        else if (point == "VK_PIPELINE_BIND_POINT_COMPUTE")
        {
            stream.computePipeline = IdOf(args.Get("pipeline"));
            stream.shaders.erase(VK_SHADER_STAGE_COMPUTE_BIT);
        }
    }
    else if (method == "vkCmdBindShadersEXT")
    {
        // And shader objects the pipeline of theirs.
        const JValue* stages = args.Get("pStages");
        const JValue* shaders = args.Get("pShaders");
        for (uint32_t k = 0; stages && k < stages->count; ++k)
        {
            const uint32_t bit = StageBitOfName(Str(&stages->items[k]));
            const uint64_t id = shaders && shaders->IsArray() && k < shaders->count ? IdOf(&shaders->items[k]) : 0;
            if (!bit)
                continue;
            if (bit == VK_SHADER_STAGE_COMPUTE_BIT)
                stream.computePipeline = 0;
            else
                stream.graphicsPipeline = 0;
            if (id)
                stream.shaders[bit] = id;
            else
                stream.shaders.erase(bit);
        }
    }
    else if (method == "vkCmdSetDepthWriteEnable" || method == "vkCmdSetDepthWriteEnableEXT")
    {
        stream.depthWriteCommands = {index};   // the last one is what is in effect
    }
    else if (method == "vkCmdSetRasterizerDiscardEnable" || method == "vkCmdSetRasterizerDiscardEnableEXT")
    {
        stream.rasterizerDiscardCommands = {index};
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
            // Before rasterization: nothing rasterized, and so no fragment stage (see the top).
            if (BeforeRasterization(stage))
            {
                if (!p.hasRasterization)
                {
                    p.rasterization = VkPipelineRasterizationStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                    p.rasterization.lineWidth = 1.0f;
                    p.hasRasterization = true;
                }
                p.rasterization.rasterizerDiscardEnable = VK_TRUE;
                p.RemoveDynamic({VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE});
                p.stages.erase(std::remove_if(p.stages.begin(), p.stages.end(),
                                   [](const VkPipelineShaderStageCreateInfo& st) { return st.stage == VK_SHADER_STAGE_FRAGMENT_BIT; }),
                    p.stages.end());
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

VkShaderEXT Replayer::AblationShader(uint64_t shaderId, size_t target, int variant)
{
    const auto key = std::make_tuple(shaderId, target, variant);
    if (auto it = _ablationShaders.find(key); it != _ablationShaders.end())
        return it->second;
    _ablationShaders[key] = VK_NULL_HANDLE;   // a variant that cannot be made is not tried again
    const auto& words = _options.ablation.targets[target].variants[(size_t)variant].words;
    std::string error;
    VkShaderEXT shader = words.empty() ? VK_NULL_HANDLE : ShaderObjectWithCode(shaderId, words.data(), words.size(), error);
    _ablationShaders[key] = shader;
    return shader;
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
    // Without a pipeline, the stage's shader object, which is what the variants stand in for.
    const VkShaderStageFlagBits stageBit = StageBit(target.stage);
    uint64_t shaderObject = 0;
    if (!result.pipeline)
        if (auto it = stream.shaders.find(stageBit); it != stream.shaders.end())
            shaderObject = it->second;
    auto fail = [&](std::string why) {
        result.note = std::move(why);
        _report->ablations.push_back(std::move(result));
    };
    if (!_ablationPool)
        return fail("the replay's queue writes no timestamps");
    if (!StartsWith(method, "vkCmdDraw") && !compute)
        return fail(method + " is not a draw or a dispatch");
    if (!StageBit(target.stage))
        return fail("the " + target.stage + " stage cannot be measured");
    if (!result.pipeline && !shaderObject)
        return fail(stream.shaders.empty() ? "no pipeline is bound at the command" : "no shader object is bound to the " + target.stage + " stage at the command");
    if (!compute && _passViews > 1)
        return fail("draws in multiview passes are not measured yet (each timestamp takes one query per view)");
    // What each issue binds: [0] the baseline, then one per variant. A copy of the pipeline, or the
    // stage's shader object made with the variant's code (the baseline is the draw's own).
    const size_t count = target.variants.size() + 1;
    std::vector<VkPipeline> pipelines(count, VK_NULL_HANDLE);
    std::vector<VkShaderEXT> shaders(count, VK_NULL_HANDLE);
    const VkPipelineBindPoint point = compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    bool depthDynamic = false;
    bool stencilDynamic = false;
    if (shaderObject)
    {
        result.pipeline = shaderObject;
        result.shaderObject = true;
        shaders[0] = (VkShaderEXT)(uintptr_t)Handle(shaderObject);
        if (!shaders[0] || !_fns.CmdBindShadersEXT)
            return fail("the " + target.stage + " stage's shader object was not made by the replay");
        for (size_t v = 1; v < count; ++v)
        {
            shaders[v] = AblationShader(shaderObject, t, (int)v - 1);
            if (!shaders[v])
                result.variants[v - 1].note = "the variant's shader object could not be created";
        }
        // Every piece of a shader-object draw's state is dynamic.
        depthDynamic = stencilDynamic = !compute;
    }
    else
    {
        pipelines[0] = AblationPipeline(result.pipeline, t, -1, compute);
        if (!pipelines[0])
            return fail("the pipeline could not be copied (the capture has no code for its " + target.stage + " stage, or the copy was refused)");
        for (size_t v = 1; v < count; ++v)
        {
            pipelines[v] = AblationPipeline(result.pipeline, t, (int)v - 1, compute);
            if (!pipelines[v])
                result.variants[v - 1].note = "the variant's pipeline could not be created";
        }
        const auto dynamic = _ablationDynamic.count(result.pipeline) ? _ablationDynamic[result.pipeline] : std::make_pair(false, false);
        depthDynamic = !compute && dynamic.first;
        stencilDynamic = !compute && dynamic.second;
    }
    const auto bind = [&](size_t v) {
        if (shaderObject)
            _fns.CmdBindShadersEXT(cb, 1, &stageBit, &shaders[v]);
        else
            _fns.CmdBindPipeline(cb, point, pipelines[v]);
    };
    if (depthDynamic)
    {
        if (_fns.CmdSetDepthWriteEnable)
            _fns.CmdSetDepthWriteEnable(cb, VK_FALSE);
        else if (_fns.CmdSetDepthWriteEnableEXT)
            _fns.CmdSetDepthWriteEnableEXT(cb, VK_FALSE);
    }
    if (stencilDynamic)
        _fns.CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    // A stage before rasterization is timed without it: a pipeline copy holds that, a shader-object
    // draw sets it (and gets the application's own setting back after).
    const auto setDiscard = _fns.CmdSetRasterizerDiscardEnable ? _fns.CmdSetRasterizerDiscardEnable : _fns.CmdSetRasterizerDiscardEnableEXT;
    const bool discard = !compute && BeforeRasterization(stageBit);
    if (discard && shaderObject)
    {
        if (!setDiscard)
            return fail("rasterizer discard cannot be set for the shader objects, and a stage before rasterization is timed without it");
        setDiscard(cb, VK_TRUE);
    }
    result.rasterized = !discard;

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
            if (!pipelines[v] && !shaders[v])
                continue;
            const uint32_t query = pending.base + (uint32_t)((r * count + v) * 2);
            bind(v);
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

    // The command buffer's own state again: its pipeline or shader object, and the write state it set.
    if (shaderObject)
        bind(0);
    else
        _fns.CmdBindPipeline(cb, point, (VkPipeline)Handle(result.pipeline));
    // Only what the issues set dynamically: a state the pipeline holds statically must not be set
    // after it is bound, and the application's last setting of it was for an earlier pipeline.
    for (const std::vector<uint32_t>* list : {&stream.depthWriteCommands, &stream.stencilWriteCommands, &stream.rasterizerDiscardCommands})
    {
        const bool changed = list == &stream.depthWriteCommands ? depthDynamic : list == &stream.stencilWriteCommands ? stencilDynamic : discard && shaderObject;
        if (!changed)
            continue;
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
