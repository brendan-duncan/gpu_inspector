// Copies of captured graphics pipelines with parts of their state changed: the counting pipelines
// of the overdraw measurement and the query pipelines of pixel history. A copy is decoded from
// the pipeline's creation arguments, takes its shader code from the capture's SPIR-V payloads, and
// is created with whatever the edit changed.
#include "replayer.h"

#include <algorithm>
#include <string>

#include "util.h"

namespace vkreplay {

void PipelineCopy::ReplaceFragment(VkShaderModule module) {
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage.module = module;
    stage.pName = "main";
    auto it = std::find_if(stages.begin(), stages.end(), [](const VkPipelineShaderStageCreateInfo& s) { return s.stage == VK_SHADER_STAGE_FRAGMENT_BIT; });
    if (it != stages.end()) *it = stage;
    else stages.push_back(stage);
}

void PipelineCopy::RemoveDynamic(std::initializer_list<VkDynamicState> states) {
    dynamic.erase(std::remove_if(dynamic.begin(), dynamic.end(),
                                 [&](VkDynamicState s) { return std::find(states.begin(), states.end(), s) != states.end(); }),
                  dynamic.end());
}

void PipelineCopy::AddDynamic(VkDynamicState state) {
    if (!HasDynamic(state)) dynamic.push_back(state);
}

bool PipelineCopy::HasDynamic(VkDynamicState state) const {
    return std::find(dynamic.begin(), dynamic.end(), state) != dynamic.end();
}

VkShaderModule Replayer::CountModule() {
    if (!_countModule) {
        VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        m.codeSize = sizeof(kCountFragmentSpirv);
        m.pCode = kCountFragmentSpirv;
        _fns.CreateShaderModule(_device, &m, nullptr, &_countModule);
    }
    return _countModule;
}

VkPipeline Replayer::CopyGraphicsPipeline(uint64_t pipelineId, const std::string& purpose, const std::function<bool(PipelineCopy&)>& edit) {
    const JValue* object = _capture->Object(pipelineId);
    const JValue* args = object ? object->Get("args") : nullptr;
    if (!object || !args || Str(object->Get("cmd")) != "vkCreateGraphicsPipelines") {
        Problem(purpose + ": pipeline " + std::to_string(pipelineId) + " is not a graphics pipeline the capture holds");
        return VK_NULL_HANDLE;
    }
    const uint32_t index = object->Get("index") ? (uint32_t)object->Get("index")->Uint() : 0;
    const size_t problems = _ctx.problems.size();
    const size_t unresolved = _ctx.unresolved;
    Args_vkCreateGraphicsPipelines a{};
    DecodeArgs(_ctx, *args, a);
    const bool usable = a.pCreateInfos && index < a.createInfoCount && _ctx.unresolved == unresolved;
    _ctx.problems.resize(problems);
    _ctx.unresolved = unresolved;
    if (!usable) {
        _arena.Reset();
        Problem(purpose + ": pipeline " + std::to_string(pipelineId) + " names objects the replay does not have");
        return VK_NULL_HANDLE;
    }

    // Everything the create info points at is decoded into the arena, which stays until the copy exists.
    PipelineCopy p;
    p.info = a.pCreateInfos[index];
    std::vector<VkShaderModule> temporary;
    for (uint32_t s = 0; s < p.info.stageCount; ++s) {
        VkPipelineShaderStageCreateInfo stage = p.info.pStages[s];
        const std::string name = std::string(StageName(stage.stage)) + ":" + (stage.pName ? stage.pName : "main");
        if (VkShaderModule module = ModuleFromBlob(*object, name)) {
            temporary.push_back(module);
            stage.module = module;
            stage.pNext = nullptr;
        }
        p.stages.push_back(stage);
    }
    if (p.info.pRasterizationState) { p.rasterization = *p.info.pRasterizationState; p.hasRasterization = true; }
    if (p.info.pMultisampleState) { p.multisample = *p.info.pMultisampleState; p.hasMultisample = true; }
    if (p.info.pDepthStencilState) { p.depthStencil = *p.info.pDepthStencilState; p.hasDepthStencil = true; }
    if (p.info.pColorBlendState) {
        p.blend = *p.info.pColorBlendState;
        p.hasBlend = true;
        if (p.blend.pAttachments) p.blendAttachments.assign(p.blend.pAttachments, p.blend.pAttachments + p.blend.attachmentCount);
    }
    if (p.info.pDynamicState) {
        p.hasDynamic = true;
        if (p.info.pDynamicState->pDynamicStates)
            p.dynamic.assign(p.info.pDynamicState->pDynamicStates, p.info.pDynamicState->pDynamicStates + p.info.pDynamicState->dynamicStateCount);
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (edit(p)) {
        p.info.stageCount = (uint32_t)p.stages.size();
        p.info.pStages = p.stages.data();
        p.info.pRasterizationState = p.hasRasterization ? &p.rasterization : nullptr;
        p.info.pMultisampleState = p.hasMultisample ? &p.multisample : nullptr;
        p.info.pDepthStencilState = p.hasDepthStencil ? &p.depthStencil : nullptr;
        if (p.hasBlend) {
            p.blend.attachmentCount = (uint32_t)p.blendAttachments.size();
            p.blend.pAttachments = p.blendAttachments.data();
            p.info.pColorBlendState = &p.blend;
        } else {
            p.info.pColorBlendState = nullptr;
        }
        VkPipelineDynamicStateCreateInfo dynamicInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicInfo.dynamicStateCount = (uint32_t)p.dynamic.size();
        dynamicInfo.pDynamicStates = p.dynamic.data();
        p.info.pDynamicState = p.hasDynamic || !p.dynamic.empty() ? &dynamicInfo : nullptr;
        p.info.flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        p.info.basePipelineHandle = VK_NULL_HANDLE;
        p.info.basePipelineIndex = -1;
        const VkResult r = _fns.CreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &p.info, nullptr, &pipeline);
        if (r != VK_SUCCESS) {
            Problem(purpose + ": the copy of pipeline " + std::to_string(pipelineId) + " could not be created (" + std::to_string(r) + ")");
            pipeline = VK_NULL_HANDLE;
        } else {
            Track("VkPipeline", (uint64_t)pipeline);
        }
    }
    for (VkShaderModule m : temporary) _fns.DestroyShaderModule(_device, m, nullptr);
    for (VkShaderModule m : p.temporary) _fns.DestroyShaderModule(_device, m, nullptr);
    _arena.Reset();
    return pipeline;
}

} // namespace vkreplay
