// Copies of captured graphics pipelines with parts of their state changed: the counting pipelines
// of the overdraw measurement and the query pipelines of pixel history. A copy is decoded from
// the pipeline's creation arguments, takes its shader code from the capture's SPIR-V payloads, and
// is created with whatever the edit changed.
#include "replayer.h"

#include <algorithm>
#include <string>

#include "util.h"

namespace vkreplay {

namespace {

/** A create info's pNext entry of a structure type, from its JSON (the capture writes pNext as an array). */
const JValue* PNextEntry(const JValue* info, std::string_view sType) {
    const JValue* chain = info ? info->Get("pNext") : nullptr;
    for (uint32_t i = 0; chain && chain->IsArray() && i < chain->count; ++i)
        if (Str(chain->items[i].Get("sType")) == sType) return &chain->items[i];
    return nullptr;
}

/** An object's own create info, from its JSON. */
const JValue* CreateInfoOf(const JValue& object) {
    const JValue* args = object.Get("args");
    const JValue* infos = args ? args->Get("pCreateInfos") : nullptr;
    const uint32_t index = object.Get("index") ? (uint32_t)object.Get("index")->Uint() : 0;
    return infos && infos->IsArray() && index < infos->count ? &infos->items[index] : nullptr;
}

/** A create info and those of the libraries it links, libraries linked from libraries included. */
void CreateInfosWithLibraries(const CaptureFile& capture, const JValue& object, std::vector<std::pair<const JValue*, bool>>& out, int depth = 0) {
    const JValue* info = CreateInfoOf(object);
    if (!info || depth > 8) return;
    const JValue* link = PNextEntry(info, "VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR");
    const JValue* list = link ? link->Get("pLibraries") : nullptr;
    out.emplace_back(info, depth > 0);
    for (uint32_t k = 0; list && list->IsArray() && k < list->count; ++k)
        if (const JValue* library = capture.Object(IdOf(&list->items[k]))) CreateInfosWithLibraries(capture, *library, out, depth + 1);
}

} // namespace

const JValue* Replayer::PipelineState(uint64_t pipelineId, std::string_view member, std::string_view part) const {
    const JValue* object = _capture->Object(pipelineId);
    if (!object) return nullptr;
    std::vector<std::pair<const JValue*, bool>> infos;
    CreateInfosWithLibraries(*_capture, *object, infos);
    for (const auto& [info, isLibrary] : infos) {
        const JValue* parts = PNextEntry(info, "VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT");
        // A library's members for parts it does not hold are ignored, as the driver ignores them.
        if ((isLibrary || parts) && (!parts || Str(parts->Get("flags")).find(part) == std::string::npos)) continue;
        const JValue* v = info->Get(member);
        if (v && !v->IsNull()) return v;
    }
    return nullptr;
}

bool Replayer::PipelineDynamic(uint64_t pipelineId, std::string_view state) const {
    const JValue* object = _capture->Object(pipelineId);
    if (!object) return false;
    std::vector<std::pair<const JValue*, bool>> infos;
    CreateInfosWithLibraries(*_capture, *object, infos);
    for (const auto& entry : infos) {
        const JValue* d = entry.first->Get("pDynamicState");
        const JValue* states = d ? d->Get("pDynamicStates") : nullptr;
        for (uint32_t k = 0; states && states->IsArray() && k < states->count; ++k)
            if (Str(&states->items[k]) == state) return true;
    }
    return false;
}

bool Replayer::MergeLibraries(const JValue& linked, const JValue& info, PipelineCopy& p, std::vector<VkShaderModule>& temporary, int depth) {
    const JValue* link = PNextEntry(&info, "VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR");
    const JValue* list = link ? link->Get("pLibraries") : nullptr;
    if (!list || !list->IsArray() || depth > 8) return false;
    for (uint32_t k = 0; k < list->count; ++k) {
        const JValue* library = _capture->Object(IdOf(&list->items[k]));
        const JValue* libraryInfo = library ? CreateInfoOf(*library) : nullptr;
        if (!libraryInfo || Str(library->Get("cmd")) != "vkCreateGraphicsPipelines") return false;
        // A library's shader modules are often destroyed by now: its payloads stand in for them, so
        // what it names that the replay does not have is not a problem here.
        const size_t problems = _ctx.problems.size();
        const size_t unresolved = _ctx.unresolved;
        Args_vkCreateGraphicsPipelines a{};
        DecodeArgs(_ctx, *library->Get("args"), a);
        _ctx.problems.resize(problems);
        _ctx.unresolved = unresolved;
        const uint32_t index = library->Get("index") ? (uint32_t)library->Get("index")->Uint() : 0;
        if (!a.pCreateInfos || index >= a.createInfoCount) return false;
        const VkGraphicsPipelineCreateInfo& l = a.pCreateInfos[index];
        MergeLibraries(linked, *libraryInfo, p, temporary, depth + 1);

        // Which parts of a pipeline the library holds; what it points at for the rest is ignored, as
        // the driver ignores it.
        const JValue* parts = PNextEntry(libraryInfo, "VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT");
        const std::string flags = parts ? Str(parts->Get("flags")) : "";
        const bool vertexInput = flags.find("VERTEX_INPUT_INTERFACE") != std::string::npos;
        const bool preRasterization = flags.find("PRE_RASTERIZATION_SHADERS") != std::string::npos;
        const bool fragment = flags.find("FRAGMENT_SHADER_BIT") != std::string::npos;
        const bool output = flags.find("FRAGMENT_OUTPUT_INTERFACE") != std::string::npos;

        for (uint32_t s = 0; s < l.stageCount; ++s) {
            VkPipelineShaderStageCreateInfo stage = l.pStages[s];
            if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT ? !fragment : !preRasterization) continue;
            if (std::any_of(p.stages.begin(), p.stages.end(), [&](const VkPipelineShaderStageCreateInfo& x) { return x.stage == stage.stage; })) continue;
            const std::string name = std::string(StageName(stage.stage)) + ":" + (stage.pName ? stage.pName : "main");
            VkShaderModule module = ModuleFromBlob(linked, name);
            if (!module) module = ModuleFromBlob(*library, name);
            if (module) {
                temporary.push_back(module);
                stage.module = module;
                stage.pNext = nullptr;
            }
            p.stages.push_back(stage);
        }
        VkGraphicsPipelineCreateInfo& info = p.info;
        if (vertexInput) {
            if (!info.pVertexInputState) info.pVertexInputState = l.pVertexInputState;
            if (!info.pInputAssemblyState) info.pInputAssemblyState = l.pInputAssemblyState;
        }
        if (preRasterization) {
            if (!info.pViewportState) info.pViewportState = l.pViewportState;
            if (!info.pRasterizationState) info.pRasterizationState = l.pRasterizationState;
            if (!info.pTessellationState) info.pTessellationState = l.pTessellationState;
        }
        if (fragment && !info.pDepthStencilState) info.pDepthStencilState = l.pDepthStencilState;
        if ((fragment || output) && !info.pMultisampleState) info.pMultisampleState = l.pMultisampleState;
        if (output && !info.pColorBlendState) info.pColorBlendState = l.pColorBlendState;
        if ((vertexInput || preRasterization || fragment || output) && l.pDynamicState && l.pDynamicState->pDynamicStates) {
            p.hasDynamic = true;
            for (uint32_t d = 0; d < l.pDynamicState->dynamicStateCount; ++d) p.AddDynamic(l.pDynamicState->pDynamicStates[d]);
        }
        if (!info.layout) info.layout = l.layout;
        if ((preRasterization || fragment || output) && !info.renderPass && l.renderPass) {
            info.renderPass = l.renderPass;
            info.subpass = l.subpass;
        }
        // Dynamic rendering's attachment formats travel in pNext.
        for (auto* in = static_cast<const VkBaseInStructure*>(l.pNext); in; in = in->pNext) {
            if (in->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) continue;
            bool present = false;
            for (auto* q = static_cast<const VkBaseInStructure*>(info.pNext); q; q = q->pNext) present = present || q->sType == in->sType;
            if (!present) {
                auto* rendering = _arena.Make<VkPipelineRenderingCreateInfo>();
                *rendering = *reinterpret_cast<const VkPipelineRenderingCreateInfo*>(in);
                rendering->pNext = info.pNext;
                info.pNext = rendering;
            }
            break;
        }
    }
    return true;
}

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

VkShaderModule Replayer::PrimitiveIdModule() {
    if (!_primitiveIdModule) {
        VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        m.codeSize = sizeof(kPrimitiveIdFragmentSpirv);
        m.pCode = kPrimitiveIdFragmentSpirv;
        _fns.CreateShaderModule(_device, &m, nullptr, &_primitiveIdModule);
    }
    return _primitiveIdModule;
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
    // A pipeline linked from graphics pipeline libraries holds none of their stages or state, so the
    // copy is made whole from the libraries' records: an edit may change state that several of them
    // hold, and a copy could not be linked from libraries made again with only some of it changed.
    const JValue* ownInfo = CreateInfoOf(*object);
    if (ownInfo && MergeLibraries(*object, *ownInfo, p, temporary)) {
        p.info.pNext = StripPNext(p.info.pNext, {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT});
        p.info.flags &= ~(VkPipelineCreateFlags)(VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT |
                                                  VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT);
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
        for (uint32_t d = 0; p.info.pDynamicState->pDynamicStates && d < p.info.pDynamicState->dynamicStateCount; ++d)
            p.AddDynamic(p.info.pDynamicState->pDynamicStates[d]);
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
