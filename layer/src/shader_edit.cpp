#include "shader_edit.h"

#include "layer.h"
#include "tracker.h"
#include "transport.h"
#include "vk_serialize.gen.h"

#include <cstring>

namespace vkinsp {

// ---------------------------------------------------------------------------------------------
// Arena

void* Arena::Alloc(size_t size) {
    // new[] returns storage aligned for any fundamental type, which is all these structs need.
    blocks.emplace_back(new uint8_t[size ? size : 1]);
    return blocks.back().get();
}

const char* Arena::CopyString(const char* s) {
    if (!s) return nullptr;
    size_t n = strlen(s) + 1;
    char* d = static_cast<char*>(Alloc(n));
    memcpy(d, s, n);
    return d;
}

// ---------------------------------------------------------------------------------------------
// Deep copies of the pipeline create infos

namespace {

template <typename T>
VkBaseOutStructure* Plain(Arena& a, const VkBaseInStructure* n) {
    return reinterpret_cast<VkBaseOutStructure*>(a.Copy(reinterpret_cast<const T*>(n)));
}

// Copies the extension structs of a pNext chain that are known to be inputs of pipeline or
// stage creation. Output-only structs are dropped; unknown ones are dropped and noted. Inline
// stage code (VkShaderModuleCreateInfo) is skipped here and kept by ExtractInlineCode.
const void* CopyChain(Arena& a, const void* pNext, std::string& note, bool& unsupported) {
    VkBaseOutStructure* head = nullptr;
    VkBaseOutStructure** tail = &head;
    for (auto* n = static_cast<const VkBaseInStructure*>(pNext); n; n = n->pNext) {
        VkBaseOutStructure* c = nullptr;
        switch ((int)n->sType) {
            case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO: {
                auto* s = a.Copy(reinterpret_cast<const VkPipelineRenderingCreateInfo*>(n));
                s->pColorAttachmentFormats = a.Copy(s->pColorAttachmentFormats, s->colorAttachmentCount);
                c = reinterpret_cast<VkBaseOutStructure*>(s);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT: {
                auto* s = a.Copy(reinterpret_cast<const VkPipelineDiscardRectangleStateCreateInfoEXT*>(n));
                s->pDiscardRectangles = a.Copy(s->pDiscardRectangles, s->discardRectangleCount);
                c = reinterpret_cast<VkBaseOutStructure*>(s);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT: {
                auto* s = a.Copy(reinterpret_cast<const VkPipelineColorWriteCreateInfoEXT*>(n));
                s->pColorWriteEnables = a.Copy(s->pColorWriteEnables, s->attachmentCount);
                c = reinterpret_cast<VkBaseOutStructure*>(s);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT: {
                auto* s = a.Copy(reinterpret_cast<const VkPipelineSampleLocationsStateCreateInfoEXT*>(n));
                s->sampleLocationsInfo.pNext = nullptr;
                s->sampleLocationsInfo.pSampleLocations =
                    a.Copy(s->sampleLocationsInfo.pSampleLocations, s->sampleLocationsInfo.sampleLocationsCount);
                c = reinterpret_cast<VkBaseOutStructure*>(s);
                break;
            }
            case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO: {
                auto* s = a.Copy(reinterpret_cast<const VkPipelineVertexInputDivisorStateCreateInfo*>(n));
                s->pVertexBindingDivisors = a.Copy(s->pVertexBindingDivisors, s->vertexBindingDivisorCount);
                c = reinterpret_cast<VkBaseOutStructure*>(s);
                break;
            }
            // Structs without pointers: a plain copy is complete.
            case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO: c = Plain<VkPipelineCreateFlags2CreateInfo>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO: c = Plain<VkPipelineRobustnessCreateInfo>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO: c = Plain<VkPipelineShaderStageRequiredSubgroupSizeCreateInfo>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO: c = Plain<VkPipelineTessellationDomainOriginStateCreateInfo>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO: c = Plain<VkPipelineRasterizationLineStateCreateInfo>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT: c = Plain<VkPipelineRasterizationConservativeStateCreateInfoEXT>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT: c = Plain<VkPipelineRasterizationDepthClipStateCreateInfoEXT>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT: c = Plain<VkPipelineRasterizationStateStreamCreateInfoEXT>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT: c = Plain<VkPipelineColorBlendAdvancedStateCreateInfoEXT>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT: c = Plain<VkPipelineViewportDepthClipControlCreateInfoEXT>(a, n); break;
            case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR: c = Plain<VkPipelineFragmentShadingRateStateCreateInfoKHR>(a, n); break;
            case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
            case VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO:
                break;   // inline code is kept separately; creation feedback is output only
            case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
            case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
                unsupported = true;
                note += "graphics pipeline libraries are not supported; ";
                break;
            default: {
                const char* name = ToString_VkStructureType(n->sType);
                note += "dropped ";
                note += name ? name : "unknown pNext struct";
                note += "; ";
                break;
            }
        }
        if (c) {
            c->pNext = nullptr;
            *tail = c;
            tail = &c->pNext;
        }
    }
    return head;
}

// A copy of the inline code struct of a stage's chain (VK_KHR_maintenance5), or null.
const VkShaderModuleCreateInfo* ExtractInlineCode(Arena& a, const void* pNext) {
    for (auto* n = static_cast<const VkBaseInStructure*>(pNext); n; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
        auto* s = a.Copy(reinterpret_cast<const VkShaderModuleCreateInfo*>(n));
        s->pNext = nullptr;
        const uint8_t* code = a.Copy(reinterpret_cast<const uint8_t*>(s->pCode), s->codeSize);
        s->pCode = reinterpret_cast<const uint32_t*>(code);
        return s;
    }
    return nullptr;
}

const VkShaderModuleCreateInfo* CopyStage(Arena& a, VkPipelineShaderStageCreateInfo& s, std::string& note, bool& unsupported) {
    const VkShaderModuleCreateInfo* inlineCode = ExtractInlineCode(a, s.pNext);
    s.pNext = CopyChain(a, s.pNext, note, unsupported);
    s.pName = a.CopyString(s.pName);
    if (s.pSpecializationInfo) {
        auto* spec = a.Copy(s.pSpecializationInfo);
        spec->pMapEntries = a.Copy(spec->pMapEntries, spec->mapEntryCount);
        if (spec->pData && spec->dataSize) {
            void* data = a.Alloc(spec->dataSize);
            memcpy(data, spec->pData, spec->dataSize);
            spec->pData = data;
        } else {
            spec->pData = nullptr;
        }
        s.pSpecializationInfo = spec;
    }
    return inlineCode;
}

bool HasDynamicState(const VkPipelineDynamicStateCreateInfo* d, VkDynamicState state) {
    if (!d || !d->pDynamicStates) return false;
    for (uint32_t i = 0; i < d->dynamicStateCount; ++i) if (d->pDynamicStates[i] == state) return true;
    return false;
}

VkGraphicsPipelineCreateInfo* CopyGraphics(Arena& a, const VkGraphicsPipelineCreateInfo& src, std::string& note, bool& unsupported,
                                           std::vector<const VkShaderModuleCreateInfo*>& inlineCode) {
    VkGraphicsPipelineCreateInfo* g = a.Copy(&src);
    g->pNext = CopyChain(a, src.pNext, note, unsupported);
    if (g->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) {
        unsupported = true;
        note += "pipeline library; ";
    }
    g->flags &= ~(VkPipelineCreateFlags)(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
    g->basePipelineHandle = VK_NULL_HANDLE;
    g->basePipelineIndex = -1;

    auto* stages = a.Copy(src.pStages, src.stageCount);
    inlineCode.assign(src.stageCount, nullptr);
    for (uint32_t i = 0; stages && i < src.stageCount; ++i) inlineCode[i] = CopyStage(a, stages[i], note, unsupported);
    g->pStages = stages;

    const VkPipelineDynamicStateCreateInfo* dyn = src.pDynamicState;
    if (src.pVertexInputState) {
        auto* s = a.Copy(src.pVertexInputState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        s->pVertexBindingDescriptions = a.Copy(s->pVertexBindingDescriptions, s->vertexBindingDescriptionCount);
        s->pVertexAttributeDescriptions = a.Copy(s->pVertexAttributeDescriptions, s->vertexAttributeDescriptionCount);
        g->pVertexInputState = s;
    }
    if (src.pInputAssemblyState) {
        auto* s = a.Copy(src.pInputAssemblyState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        g->pInputAssemblyState = s;
    }
    if (src.pTessellationState) {
        auto* s = a.Copy(src.pTessellationState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        g->pTessellationState = s;
    }
    if (src.pViewportState) {
        auto* s = a.Copy(src.pViewportState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        // The arrays are ignored (and may be invalid) when the state is dynamic.
        bool dynViewport = HasDynamicState(dyn, VK_DYNAMIC_STATE_VIEWPORT) || HasDynamicState(dyn, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
        bool dynScissor = HasDynamicState(dyn, VK_DYNAMIC_STATE_SCISSOR) || HasDynamicState(dyn, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);
        s->pViewports = dynViewport ? nullptr : a.Copy(s->pViewports, s->viewportCount);
        s->pScissors = dynScissor ? nullptr : a.Copy(s->pScissors, s->scissorCount);
        g->pViewportState = s;
    }
    if (src.pRasterizationState) {
        auto* s = a.Copy(src.pRasterizationState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        g->pRasterizationState = s;
    }
    if (src.pMultisampleState) {
        auto* s = a.Copy(src.pMultisampleState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        uint32_t words = ((uint32_t)s->rasterizationSamples + 31) / 32;
        s->pSampleMask = a.Copy(s->pSampleMask, words ? words : 1);
        g->pMultisampleState = s;
    }
    if (src.pDepthStencilState) {
        auto* s = a.Copy(src.pDepthStencilState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        g->pDepthStencilState = s;
    }
    if (src.pColorBlendState) {
        auto* s = a.Copy(src.pColorBlendState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        s->pAttachments = a.Copy(s->pAttachments, s->attachmentCount);
        g->pColorBlendState = s;
    }
    if (src.pDynamicState) {
        auto* s = a.Copy(src.pDynamicState);
        s->pNext = CopyChain(a, s->pNext, note, unsupported);
        s->pDynamicStates = a.Copy(s->pDynamicStates, s->dynamicStateCount);
        g->pDynamicState = s;
    }
    return g;
}

VkComputePipelineCreateInfo* CopyCompute(Arena& a, const VkComputePipelineCreateInfo& src, std::string& note, bool& unsupported,
                                         std::vector<const VkShaderModuleCreateInfo*>& inlineCode) {
    VkComputePipelineCreateInfo* c = a.Copy(&src);
    c->pNext = CopyChain(a, src.pNext, note, unsupported);
    c->flags &= ~(VkPipelineCreateFlags)(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
    c->basePipelineHandle = VK_NULL_HANDLE;
    c->basePipelineIndex = -1;
    inlineCode.assign(1, CopyStage(a, c->stage, note, unsupported));
    return c;
}

// A stage as it is created: an edited stage uses its replacement module and drops any inline
// code; an untouched stage that had inline code gets it back at the head of its chain.
void PrepareStage(Arena& scratch, VkPipelineShaderStageCreateInfo& s, const VkShaderModuleCreateInfo* inlineCode,
                  const std::map<VkShaderStageFlagBits, VkShaderModule>& edits) {
    auto e = edits.find(s.stage);
    if (e != edits.end()) {
        s.module = e->second;
        return;
    }
    if (inlineCode) {
        auto* n = scratch.Copy(inlineCode);
        n->pNext = s.pNext;
        s.pNext = n;
    }
}

const char* StageName(VkShaderStageFlagBits stage) {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT: return "vertex";
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tess_control";
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tess_eval";
        case VK_SHADER_STAGE_GEOMETRY_BIT: return "geometry";
        case VK_SHADER_STAGE_FRAGMENT_BIT: return "fragment";
        case VK_SHADER_STAGE_COMPUTE_BIT: return "compute";
        case VK_SHADER_STAGE_TASK_BIT_EXT: return "task";
        case VK_SHADER_STAGE_MESH_BIT_EXT: return "mesh";
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR: return "raygen";
        case VK_SHADER_STAGE_ANY_HIT_BIT_KHR: return "any_hit";
        case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return "closest_hit";
        case VK_SHADER_STAGE_MISS_BIT_KHR: return "miss";
        case VK_SHADER_STAGE_INTERSECTION_BIT_KHR: return "intersection";
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR: return "callable";
        default: return "shader";
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// ShaderEditor

ShaderEditor& ShaderEditor::Get() {
    static ShaderEditor* instance = new ShaderEditor();
    return *instance;
}

void ShaderEditor::OnCreateGraphicsPipelines(VkDevice device, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
                                             const VkPipeline* pipelines) {
    if (!infos || !pipelines) return;
    std::unique_lock lock(_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        if (!pipelines[i]) continue;
        auto rec = std::make_unique<Record>();
        rec->device = device;
        rec->graphics = CopyGraphics(rec->arena, infos[i], rec->note, rec->unsupported, rec->inlineCode);
        _records[pipelines[i]] = std::move(rec);
    }
}

void ShaderEditor::OnCreateComputePipelines(VkDevice device, uint32_t count, const VkComputePipelineCreateInfo* infos,
                                            const VkPipeline* pipelines) {
    if (!infos || !pipelines) return;
    std::unique_lock lock(_mutex);
    for (uint32_t i = 0; i < count; ++i) {
        if (!pipelines[i]) continue;
        auto rec = std::make_unique<Record>();
        rec->device = device;
        rec->compute = CopyCompute(rec->arena, infos[i], rec->note, rec->unsupported, rec->inlineCode);
        _records[pipelines[i]] = std::move(rec);
    }
}

void ShaderEditor::OnDestroyPipeline(uint64_t handle) {
    std::lock_guard lock(_retiredMutex);
    _destroyed.push_back((VkPipeline)(uintptr_t)handle);
}

VkPipeline ShaderEditor::ResolveSlow(VkPipeline pipeline) {
    std::shared_lock lock(_mutex);
    auto it = _active.find(pipeline);
    return it == _active.end() ? pipeline : it->second;
}

void ShaderEditor::Retire(VkDevice device, VkPipeline pipeline, VkShaderModule module) {
    std::lock_guard lock(_retiredMutex);
    if (pipeline) _retiredPipelines.emplace_back(device, pipeline);
    if (module) _retiredModules.emplace_back(device, module);
}

void ShaderEditor::OnPresent(DeviceData* dev) {
    std::vector<VkPipeline> destroyed;
    {
        std::lock_guard lock(_retiredMutex);
        destroyed.swap(_destroyed);
    }
    // Pipelines the application destroyed: drop their records and retire their replacements.
    std::vector<VkPipeline> replacementsToUntrack;
    if (!destroyed.empty()) {
        std::unique_lock lock(_mutex);
        for (VkPipeline p : destroyed) {
            auto it = _records.find(p);
            if (it == _records.end()) continue;
            Record& rec = *it->second;
            if (rec.replacement) {
                Retire(rec.device, rec.replacement, VK_NULL_HANDLE);
                replacementsToUntrack.push_back(rec.replacement);
            }
            for (auto& [stage, module] : rec.edits) Retire(rec.device, VK_NULL_HANDLE, module);
            _active.erase(p);
            _records.erase(it);
        }
        _anyActive.store(!_active.empty(), std::memory_order_relaxed);
    }
    for (VkPipeline r : replacementsToUntrack) Tracker::Get().OnDestroy(HT_VkPipeline, (uint64_t)(uintptr_t)r);

    std::vector<std::pair<VkDevice, VkPipeline>> pipelines;
    std::vector<std::pair<VkDevice, VkShaderModule>> modules;
    {
        std::lock_guard lock(_retiredMutex);
        if (_retiredPipelines.empty() && _retiredModules.empty()) return;
        pipelines.swap(_retiredPipelines);
        modules.swap(_retiredModules);
    }
    // Replacements may still be referenced by command buffers in flight.
    dev->dispatch.DeviceWaitIdle(dev->device);
    for (auto& [device, p] : pipelines) {
        DeviceData* d = GetDeviceData(device);
        if (d) d->dispatch.DestroyPipeline(device, p, nullptr);
    }
    for (auto& [device, m] : modules) {
        DeviceData* d = GetDeviceData(device);
        if (d) d->dispatch.DestroyShaderModule(device, m, nullptr);
    }
}

void ShaderEditor::Reply(uint64_t pipelineId, VkShaderStageFlagBits stage, bool ok, const std::string& message, uint64_t replacementId) {
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ShaderReplaced");
    w.Key("pipeline"); w.Uint(pipelineId);
    w.Key("stage"); w.String(StageName(stage));
    w.Key("ok"); w.Boolean(ok);
    if (!message.empty()) { w.Key(ok ? "note" : "error"); w.String(message); }
    if (replacementId) { w.Key("replacement"); w.Uint(replacementId); }
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    Log("shader edit: pipeline %llu %s: %s %s", (unsigned long long)pipelineId, StageName(stage), ok ? "ok" : "failed",
        message.c_str());
}

// Creates (or recreates) the replacement pipeline of `original` from its record and current
// edits, registers it with the tracker and activates it. Called with _mutex held exclusively.
bool ShaderEditor::Rebuild(VkPipeline original, Record& rec, std::string& error) {
    DeviceData* dev = GetDeviceData(rec.device);
    if (!dev) { error = "device not found"; return false; }
    if (rec.unsupported) { error = "cannot rebuild this pipeline: " + rec.note; return false; }

    Arena scratch;
    VkPipeline created = VK_NULL_HANDLE;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    if (rec.graphics) {
        VkGraphicsPipelineCreateInfo ci = *rec.graphics;
        auto* stages = scratch.Copy(rec.graphics->pStages, rec.graphics->stageCount);
        for (uint32_t i = 0; stages && i < ci.stageCount; ++i) {
            PrepareStage(scratch, stages[i], i < rec.inlineCode.size() ? rec.inlineCode[i] : nullptr, rec.edits);
        }
        ci.pStages = stages;
        res = dev->dispatch.CreateGraphicsPipelines(rec.device, VK_NULL_HANDLE, 1, &ci, nullptr, &created);
    } else if (rec.compute) {
        VkComputePipelineCreateInfo ci = *rec.compute;
        PrepareStage(scratch, ci.stage, rec.inlineCode.empty() ? nullptr : rec.inlineCode[0], rec.edits);
        res = dev->dispatch.CreateComputePipelines(rec.device, VK_NULL_HANDLE, 1, &ci, nullptr, &created);
    } else {
        error = "no create info recorded";
        return false;
    }
    if (res != VK_SUCCESS || !created) {
        error = "pipeline creation failed (VkResult " + std::to_string((int)res) + ")";
        if (!rec.note.empty()) error += "; " + rec.note;
        return false;
    }

    // Register the replacement as an object of its own: the original's creation arguments and
    // shader payloads, with the edited stages' new code, so captures and reflection work on it.
    Tracker& t = Tracker::Get();
    TrackedObject orig;
    uint64_t newId = 0;
    if (t.Find(HT_VkPipeline, (uint64_t)(uintptr_t)original, orig)) {
        newId = t.OnCreate(HT_VkPipeline, (uint64_t)(uintptr_t)created, HT_VkDevice, (uint64_t)(uintptr_t)rec.device,
                           orig.cmd, orig.index, orig.args);
        std::string label = (orig.label.empty() ? "Pipeline " + std::to_string(orig.id) : orig.label) + " (edited)";
        t.SetLabel(HT_VkPipeline, (uint64_t)(uintptr_t)created, label.c_str());
        for (auto& [name, data] : orig.blobs) {
            std::shared_ptr<std::vector<uint8_t>> payload = data;
            for (auto& [stage, code] : rec.editCode) {
                std::string prefix = std::string(StageName(stage)) + ":";
                if (name.compare(0, prefix.size(), prefix) == 0) payload = code;
            }
            t.AddBlob(HT_VkPipeline, (uint64_t)(uintptr_t)created, name, payload);
        }
    }

    if (rec.replacement) {
        Retire(rec.device, rec.replacement, VK_NULL_HANDLE);
        if (rec.replacementId) t.OnDestroy(HT_VkPipeline, (uint64_t)(uintptr_t)rec.replacement);
    }
    rec.replacement = created;
    rec.replacementId = newId;
    _active[original] = created;
    _anyActive.store(true, std::memory_order_relaxed);
    return true;
}

void ShaderEditor::Replace(uint64_t pipelineId, VkShaderStageFlagBits stage, std::vector<uint32_t> spirv) {
    TrackedObject obj;
    if (!Tracker::Get().FindById(pipelineId, obj) || obj.type != HT_VkPipeline) {
        Reply(pipelineId, stage, false, "pipeline not found", 0);
        return;
    }
    VkPipeline original = (VkPipeline)(uintptr_t)obj.handle;
    std::unique_lock lock(_mutex);
    auto it = _records.find(original);
    if (it == _records.end()) {
        Reply(pipelineId, stage, false, "the pipeline's creation was not recorded", 0);
        return;
    }
    Record& rec = *it->second;
    DeviceData* dev = GetDeviceData(rec.device);
    if (!dev) {
        Reply(pipelineId, stage, false, "device not found", 0);
        return;
    }
    if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
        Reply(pipelineId, stage, false, "not a SPIR-V module", 0);
        return;
    }

    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = spirv.size() * sizeof(uint32_t);
    mci.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = dev->dispatch.CreateShaderModule(rec.device, &mci, nullptr, &module);
    if (res != VK_SUCCESS || !module) {
        Reply(pipelineId, stage, false, "vkCreateShaderModule failed (VkResult " + std::to_string((int)res) + ")", 0);
        return;
    }
    auto code = std::make_shared<std::vector<uint8_t>>(reinterpret_cast<const uint8_t*>(spirv.data()),
                                                       reinterpret_cast<const uint8_t*>(spirv.data()) + mci.codeSize);

    auto previous = rec.edits.find(stage);
    VkShaderModule previousModule = previous == rec.edits.end() ? VK_NULL_HANDLE : previous->second;
    auto previousCode = rec.editCode.find(stage);
    std::shared_ptr<std::vector<uint8_t>> previousBytes = previousCode == rec.editCode.end() ? nullptr : previousCode->second;
    rec.edits[stage] = module;
    rec.editCode[stage] = code;

    std::string error;
    if (!Rebuild(original, rec, error)) {
        // Keep the previous edit (if any) in place.
        if (previousModule) {
            rec.edits[stage] = previousModule;
            rec.editCode[stage] = previousBytes;
        } else {
            rec.edits.erase(stage);
            rec.editCode.erase(stage);
        }
        Retire(rec.device, VK_NULL_HANDLE, module);
        Reply(pipelineId, stage, false, error, 0);
        return;
    }
    if (previousModule) Retire(rec.device, VK_NULL_HANDLE, previousModule);
    Reply(pipelineId, stage, true, rec.note, rec.replacementId);
}

void ShaderEditor::Restore(uint64_t pipelineId, VkShaderStageFlagBits stage) {
    TrackedObject obj;
    if (!Tracker::Get().FindById(pipelineId, obj) || obj.type != HT_VkPipeline) {
        Reply(pipelineId, stage, false, "pipeline not found", 0);
        return;
    }
    VkPipeline original = (VkPipeline)(uintptr_t)obj.handle;
    std::unique_lock lock(_mutex);
    auto it = _records.find(original);
    if (it == _records.end()) {
        Reply(pipelineId, stage, true, "", 0);
        return;
    }
    Record& rec = *it->second;
    std::vector<VkShaderStageFlagBits> removed;
    for (auto& [s, module] : rec.edits) {
        if (stage == 0 || s == stage) removed.push_back(s);
    }
    for (VkShaderStageFlagBits s : removed) {
        Retire(rec.device, VK_NULL_HANDLE, rec.edits[s]);
        rec.edits.erase(s);
        rec.editCode.erase(s);
    }
    if (rec.edits.empty()) {
        if (rec.replacement) {
            Retire(rec.device, rec.replacement, VK_NULL_HANDLE);
            if (rec.replacementId) Tracker::Get().OnDestroy(HT_VkPipeline, (uint64_t)(uintptr_t)rec.replacement);
            rec.replacement = VK_NULL_HANDLE;
            rec.replacementId = 0;
        }
        _active.erase(original);
        _anyActive.store(!_active.empty(), std::memory_order_relaxed);
        Reply(pipelineId, stage, true, "", 0);
        return;
    }
    std::string error;
    if (!Rebuild(original, rec, error)) {
        Reply(pipelineId, stage, false, error, rec.replacementId);
        return;
    }
    Reply(pipelineId, stage, true, "", rec.replacementId);
}

// ---------------------------------------------------------------------------------------------

bool DecodeBase64(const std::string& text, std::vector<uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    out.clear();
    out.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xff));
        }
    }
    return true;
}

} // namespace vkinsp
