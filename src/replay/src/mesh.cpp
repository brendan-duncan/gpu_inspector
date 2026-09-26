#include "replayer.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "identity_patch.h"
#include "store_patch.h"
#include "util.h"

namespace vkreplay
{

// ---------------------------------------------------------------------------------------------
// Mesh output
//
// What a draw's last stage before rasterization wrote, for the mesh output view: RenderDoc's VS Out,
// or GS/DS Out past a geometry or tessellation stage (vk_postvs.cpp). Recorded right after the replay
// has executed the draw's pass, like the overlays, so the vertex buffers, descriptor sets and push
// constants the shaders read hold what they held for the draw. The pass's state is issued again and
// then the draw alone, with a copy of its pipeline whose last pre-rasterization stage writes
// transform feedback (xfb_patch.cpp) and which rasterizes nothing. The buffer holds every vertex
// that stage emitted, in order, and the counter says how much of it was written.
//
// A GPU without transform feedback (MoltenVK, for one) has the vertex shader store its outputs
// itself instead (store_patch.h), into a buffer bound in a descriptor set of the replay's own after the
// application's: a record per vertex of the draw, which the replay puts in transform feedback's order
// once the submission is done, from the draw's index buffer (index order, strips and fans as lists,
// instance after instance). `--mesh-stores` does the same where transform feedback is, to check the
// one against the other.
//
// Transform feedback cannot be active in a multiview pass, so a multiview draw is recorded once per
// view outside one, each time with gl_ViewIndex made that view's constant in every stage: a mesh
// per view, as the shaders computed it for that view.

namespace
{

/** Bytes a mesh may take: a draw that writes more is captured up to here, and marked truncated. */
constexpr uint64_t kMaxMeshBytes = 256ull * 1024 * 1024;
/** Vertices assumed for an indirect draw, whose counts are in a buffer. */
constexpr uint64_t kIndirectVertices = 1ull << 20;

uint64_t ArgUint(const JValue* args, const char* name, uint64_t fallback = 0)
{
    const JValue* v = args ? args->Get(name) : nullptr;
    return v ? v->Uint() : fallback;
}

} // namespace

std::string Replayer::PipelineTopology(uint64_t pipelineId, bool& dynamic) const
{
    dynamic = false;
    const JValue* object = _capture->Object(pipelineId);
    // Shader objects have no topology of their own: the draw's is the dynamic state it set.
    if (object && Str(object->Get("type")) == "VkShaderEXT")
        return _overlayDrawnTopology;
    // A pipeline linked from libraries has its topology in the vertex input library's create info.
    dynamic = PipelineDynamic(pipelineId, "VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY");
    const JValue* assembly = PipelineState(pipelineId, "pInputAssemblyState", "VERTEX_INPUT_INTERFACE");
    return assembly ? Str(assembly->Get("topology")) : "";
}

void Replayer::RecordMesh(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex)
{
    // A multiview pass: every view the pass renders; any other pass: the draw once.
    std::vector<int32_t> views;
    for (uint32_t v = 0; v < 32; ++v)
        if (pass.viewMask & (1u << v))
            views.push_back((int32_t)v);
    if (views.empty())
        views.push_back(-1);
    for (uint32_t target : _options.mesh.commands)
    {
        if (target <= pass.beginIndex || target >= endIndex)
            continue;
        for (int32_t view : views)
        {
            _meshView = view;
            RecordMeshView(cb, group, pass, endIndex, target, view);
            _meshView = -1;
        }
    }
}

void Replayer::RecordMeshView(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, uint32_t target, int32_t view)
{
    const JValue* commands = _capture->Commands();
    {
        MeshResult result;
        result.view = view;
        result.command = target;
        result.commandBuffer = pass.commandBuffer;
        result.frame = pass.frame;
        result.passIndex = pass.index;
        const JValue& command = commands->items[target];
        result.method = Str(command.Get("method"));
        if (!StartsWith(result.method, "vkCmdDraw"))
        {
            result.note = "command " + std::to_string(target) + " is " + result.method + ", not a draw";
            _report->meshes.push_back(std::move(result));
            return;
        }
        // Transform feedback, or the vertex shader's own stores where there is none.
        const bool stores = _options.mesh.stores || !_xfbAvailable;
        if (stores && !_vertexStoresAvailable)
        {
            result.note = "this GPU cannot capture vertex shader outputs: it has neither VK_EXT_transform_feedback nor vertexPipelineStoresAndAtomics";
            _report->meshes.push_back(std::move(result));
            return;
        }
        result.capturedBy = stores ? "vertex stores" : "transform feedback";

        // How many vertices the draw assembles, from its arguments; the buffer is sized from that once the
        // pipeline's record layout is known, at the draw (PrepareMeshBuffers).
        PendingMesh p;
        p.stores = stores;
        p.result = _report->meshes.size();
        const JValue* args = command.Get("args");
        if (result.method.find("Indirect") == std::string::npos)
        {
            const uint64_t instances = std::max<uint64_t>(1, ArgUint(args, "instanceCount", 1));
            const uint64_t count = result.method.find("Indexed") != std::string::npos ? ArgUint(args, "indexCount") : ArgUint(args, "vertexCount");
            p.estimate = count * instances;
        }

        // A layered pass's framebuffer layers, which its shaders' gl_Layer may pick; one view at a time
        // of a multiview pass, in a pass without a view mask.
        const uint32_t layers = pass.viewMask ? 1 : std::max(1u, pass.framebufferLayers);
        TransientImage color = CreateTransientImage(VK_FORMAT_R16_SFLOAT, pass.extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT, layers);
        // A draw with shader objects must be issued in dynamic rendering; a pipeline copy is made for a render pass.
        const bool shaderObjects = DrawUsesShaderObjects(group, target) && _fns.CmdBeginRendering;
        VkRenderPass rp = shaderObjects ? VK_NULL_HANDLE : OverdrawRenderPass(VK_FORMAT_UNDEFINED);
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (color.image && shaderObjects)
        {
            Barrier(cb, color.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        else if (color.image && rp)
        {
            VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbInfo.renderPass = rp;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &color.view;
            fbInfo.width = pass.extent.width;
            fbInfo.height = pass.extent.height;
            fbInfo.layers = layers;
            if (_fns.CreateFramebuffer(_device, &fbInfo, nullptr, &fb) == VK_SUCCESS)
                _transientFramebuffers.push_back(fb);
            else
                fb = VK_NULL_HANDLE;
        }
        if (shaderObjects ? !color.image : !fb)
        {
            result.note = "no memory for the pass the draw is issued in";
            _report->meshes.push_back(std::move(result));
            return;
        }

        _overlayTarget = target;
        _overlayOnlyTarget = true;
        _overlayTargetMode = ReissueMode::Xfb;
        _overlayIssued = false;
        _overlayDrawn = false;
        _overlayDrawnPipeline = 0;
        _meshTarget = &p;
        _reissueLayers = layers;
        ReissuePass(cb, group, pass, endIndex, false, VK_FORMAT_UNDEFINED, rp, fb, shaderObjects ? color.view : VK_NULL_HANDLE);
        _reissueLayers = 1;
        const bool drawn = _overlayIssued && _overlayDrawn && p.buffer.buffer;
        // Vertex stores: the draw's indices, in order, for putting the records in that order once they are back.
        if (drawn && p.stores && p.indexed && p.count && _reissueIndexBinding.bound)
        {
            const VkDeviceSize size = p.indexType == VK_INDEX_TYPE_UINT16 ? 2 : p.indexType == VK_INDEX_TYPE_UINT8_EXT ? 1 : 4;
            VkBuffer indexBuffer = (VkBuffer)(uintptr_t)Handle(_reissueIndexBinding.buffer);
            if (indexBuffer && CreateStaging(size * p.count, p.indices))
            {
                VkBufferCopy copy{_reissueIndexBinding.offset + size * p.firstIndex, 0, size * p.count};
                _fns.CmdCopyBuffer(cb, indexBuffer, p.indices.buffer, 1, &copy);
            }
        }
        // The pipeline the draw was issued with (a later secondary of the pass resets the one bound last).
        const uint64_t pipeline = _overlayIssued ? _overlayDrawnPipeline : _overlayPipeline;
        _meshTarget = nullptr;
        _overlayTarget = UINT32_MAX;
        _overlayOnlyTarget = false;

        bool dynamic = false;
        result.topology = pipeline ? PipelineTopology(pipeline, dynamic) : "";
        if (dynamic)
            result.topology += " (dynamic)";
        auto layout = _xfbLayouts.find(pipeline);
        // Past a tessellation or geometry stage, the primitives that stage emitted, as lists.
        if (layout != _xfbLayouts.end() && !layout->second.topology.empty())
            result.topology = layout->second.topology;
        if (layout != _xfbLayouts.end())
            result.stage = layout->second.stage;
        if (!drawn || layout == _xfbLayouts.end())
        {
            // Buffers a draw was issued with stay until the submission completes; only unused ones go now.
            if (p.buffer.buffer && _overlayIssued)
            {
                result.note = "the draw's shader outputs could not be read";
                _report->meshes.push_back(std::move(result));
                _pendingMeshes.push_back(p);
                return;
            }
            result.note = layout != _xfbLayouts.end() && !layout->second.error.empty() ? layout->second.error
                : !pipeline                                                            ? "no pipeline or vertex shader object is bound at the draw"
                                                                                       : "the draw could not be issued again (its pipeline could not be copied, or there was no memory for its vertices)";
            DestroyStaging(p.buffer);
            DestroyStaging(p.counter);
            DestroyStaging(p.indices);
            if (p.set && _meshStorePool)
                _fns.FreeDescriptorSets(_device, _meshStorePool, 1, &p.set);
            _report->meshes.push_back(std::move(result));
            return;
        }
        result.stride = layout->second.stride;
        result.outputs = layout->second.outputs;
        _report->meshes.push_back(std::move(result));
        _pendingMeshes.push_back(p);
    }
}

void Replayer::MeshStageCode(uint64_t objectId, VkShaderStageFlagBits stage, const std::string& entry, bool feedback, XfbPatch& layout,
    std::vector<uint32_t>& words, const std::vector<uint32_t>& controlCode)
{
    const std::string label = stage == VK_SHADER_STAGE_GEOMETRY_BIT         ? "geometry"
        : stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT               ? "tessellation evaluation"
        : stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT                  ? "tessellation control"
                                                                             : "vertex";
    words.clear();
    if (!StageCode(objectId, std::string(StageName(stage)) + ":" + entry, words))
    {
        words.clear();
        if (feedback)
            layout.error = "the capture has no SPIR-V for the " + label + " shader";
        return;
    }
    if (_meshView >= 0)
        words = PatchViewIndex(words.data(), words.size(), (uint32_t)_meshView);
    if (!feedback)
        return;
    std::string topology;
    if (stage != VK_SHADER_STAGE_VERTEX_BIT)
    {
        topology = OutputTopology(words.data(), words.size());
        if (topology.empty() && !controlCode.empty())
            topology = OutputTopology(controlCode.data(), controlCode.size());
    }
    // Past the vertex stage, which invocation made each vertex too, for the shader debugger.
    std::vector<IdentityOutput> identity;
    if (stage != VK_SHADER_STAGE_VERTEX_BIT)
    {
        IdentityPatch added = AddIdentityOutputs(words.data(), words.size(), entry);
        if (added.error.empty() && !added.words.empty())
        {
            words = std::move(added.words);
            identity = std::move(added.outputs);
        }
    }
    layout = PatchForTransformFeedback(words.data(), words.size(), entry);
    for (XfbOutput& o : layout.outputs)
    {
        for (const IdentityOutput& i : identity)
        {
            if (o.builtin.empty() && o.location == (int32_t)i.location)
            {
                o.name = i.name;
                o.builtin = i.builtin;
                o.location = -1;
                o.added = true;
            }
        }
    }
    layout.stage = label;
    layout.topology = topology;
    words = std::move(layout.words);
    layout.words.clear();
    if (!layout.error.empty())
        words.clear();
}

VkShaderEXT Replayer::FeedbackShader(uint64_t shaderId, uint64_t tessellationControl)
{
    if (auto it = _xfbShaders.find(shaderId); it != _xfbShaders.end())
        return it->second;
    _xfbShaders[shaderId] = VK_NULL_HANDLE;  // a copy that cannot be made is not tried again
    XfbPatch& layout = _xfbLayouts[shaderId];
    VkShaderCreateInfoEXT info{};
    std::string error;
    if (!_fns.CreateShadersEXT || !_fns.CmdBindShadersEXT || !ShaderObjectInfo(shaderId, info, error))
    {
        layout.error = "the shader object could not be made again" + (error.empty() ? std::string() : ": " + error);
        return VK_NULL_HANDLE;
    }
    const VkShaderStageFlagBits stage = info.stage;
    const std::string entry = info.pName ? info.pName : "main";
    _arena.Reset();
    // A tessellator's output primitives may be named by the control shader alone.
    std::vector<uint32_t> control;
    if (tessellationControl && ShaderObjectInfo(tessellationControl, info, error))
    {
        const std::string controlEntry = info.pName ? info.pName : "main";
        _arena.Reset();
        StageCode(tessellationControl, std::string(StageName(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)) + ":" + controlEntry, control);
    }
    std::vector<uint32_t> words;
    MeshStageCode(shaderId, stage, entry, true, layout, words, control);
    if (!layout.error.empty())
        return VK_NULL_HANDLE;
    VkShaderEXT shader = ShaderObjectWithCode(shaderId, words.data(), words.size(), error);
    if (!shader)
    {
        layout.error = "the edited " + layout.stage + " shader object was refused: " + error;
        return VK_NULL_HANDLE;
    }
    _xfbShaders[shaderId] = shader;
    return shader;
}

bool Replayer::MeshStoreLayout(uint64_t source, bool shaderObject, VkPipelineLayout& layout, uint32_t& set, std::string& error)
{
    if (auto it = _meshStoreLayouts.find(source); it != _meshStoreLayouts.end())
    {
        layout = it->second.first;
        set = it->second.second;
        if (!layout)
            error = "the draw's pipeline layout could not be made again with a set for the outputs";
        return layout != VK_NULL_HANDLE;
    }
    _meshStoreLayouts[source] = {VK_NULL_HANDLE, 0};
    if (!_meshStoreSetLayout)
    {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 1;
        info.pBindings = &binding;
        if (_fns.CreateDescriptorSetLayout(_device, &info, nullptr, &_meshStoreSetLayout) != VK_SUCCESS)
        {
            error = "the outputs' descriptor set layout could not be made";
            return false;
        }
        Track("VkDescriptorSetLayout", (uint64_t)_meshStoreSetLayout);
    }
    // The application's sets and push constants, as the pipeline's layout (or the shader object) has them.
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<VkPushConstantRange> ranges;
    const size_t problems = _ctx.problems.size();
    const size_t unresolved = _ctx.unresolved;
    if (shaderObject)
    {
        VkShaderCreateInfoEXT info{};
        if (!ShaderObjectInfo(source, info, error))
            return false;
        sets.assign(info.pSetLayouts, info.pSetLayouts + info.setLayoutCount);
        ranges.assign(info.pPushConstantRanges, info.pPushConstantRanges + info.pushConstantRangeCount);
        _arena.Reset();
    }
    else
    {
        const uint64_t layoutId = IdOf(PipelineState(source, "layout", "PRE_RASTERIZATION_SHADERS"));
        const JValue* object = layoutId ? _capture->Object(layoutId) : nullptr;
        if (!object || !object->Get("args"))
        {
            error = "the pipeline's layout is not in the capture";
            return false;
        }
        Args_vkCreatePipelineLayout a{};
        DecodeArgs(_ctx, *object->Get("args"), a);
        const bool usable = a.pCreateInfo && _ctx.unresolved == unresolved;
        if (usable)
        {
            sets.assign(a.pCreateInfo->pSetLayouts, a.pCreateInfo->pSetLayouts + a.pCreateInfo->setLayoutCount);
            ranges.assign(a.pCreateInfo->pPushConstantRanges, a.pCreateInfo->pPushConstantRanges + a.pCreateInfo->pushConstantRangeCount);
        }
        _ctx.problems.resize(problems);
        _ctx.unresolved = unresolved;
        _arena.Reset();
        if (!usable)
        {
            error = "the pipeline's layout names objects the replay does not have";
            return false;
        }
    }
    if (std::any_of(sets.begin(), sets.end(), [](VkDescriptorSetLayout l) { return !l; }))
    {
        error = "the pipeline's layout leaves a set out, which a layout of the replay's own cannot";
        return false;
    }
    VkPhysicalDeviceProperties props{};
    _fns.GetPhysicalDeviceProperties(_physical, &props);
    if (sets.size() >= props.limits.maxBoundDescriptorSets)
    {
        error = "the pipeline uses every descriptor set the GPU can bind, and the outputs need one more";
        return false;
    }
    // Sets 0 to n-1 and the push constants are the application's, so what it bound stays bound.
    sets.push_back(_meshStoreSetLayout);
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = (uint32_t)sets.size();
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = (uint32_t)ranges.size();
    info.pPushConstantRanges = ranges.data();
    if (_fns.CreatePipelineLayout(_device, &info, nullptr, &layout) != VK_SUCCESS)
    {
        error = "the pipeline layout with a set for the outputs was refused";
        return false;
    }
    Track("VkPipelineLayout", (uint64_t)layout);
    set = (uint32_t)sets.size() - 1;
    _meshStoreLayouts[source] = {layout, set};
    return true;
}

bool Replayer::PrepareMeshStores(const std::string& method, const JValue& args, uint64_t source, bool shaderObject)
{
    PendingMesh& p = *_meshTarget;
    auto fail = [&](std::string why) {
        _xfbLayouts[source].error = std::move(why);
        return false;
    };
    if (method.find("Indirect") != std::string::npos)
        return fail("without transform feedback an indirect draw's outputs are not captured: its counts are in a buffer");
    std::string error;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t set = 0;
    if (!MeshStoreLayout(source, shaderObject, layout, set, error))
        return fail(error);
    const auto arg = [&](const char* name) -> int64_t {
        const JValue* v = args.Get(name);
        return v ? v->Int() : 0;
    };
    p.indexed = method.find("Indexed") != std::string::npos;
    p.instances = (uint32_t)std::max<int64_t>(1, arg("instanceCount"));
    p.count = (uint32_t)std::max<int64_t>(0, arg(p.indexed ? "indexCount" : "vertexCount"));
    p.firstIndex = p.indexed ? (uint32_t)std::max<int64_t>(0, arg("firstIndex")) : 0;
    StoreParams params;
    params.set = set;
    params.binding = 0;
    params.firstInstance = (int32_t)arg("firstInstance");
    params.base = (int32_t)arg(p.indexed ? "vertexOffset" : "firstVertex");
    // A slot per vertex the draw can name: its index type's range, or its vertex count. Past 4 million
    // in all, what a larger index names is not kept, and the mesh says it was truncated.
    uint64_t perInstance = p.count;
    if (p.indexed)
    {
        if (!_reissueIndexBinding.bound)
            return fail("no index buffer is bound at the indexed draw");
        p.indexType = _reissueIndexBinding.type;
        perInstance = p.indexType == VK_INDEX_TYPE_UINT16 ? 65536 : p.indexType == VK_INDEX_TYPE_UINT8_EXT ? 256 : 1u << 20;
    }
    perInstance = std::clamp<uint64_t>(perInstance, 1, std::max<uint64_t>(1, (4ull << 20) / p.instances));
    p.perInstance = (uint32_t)perInstance;
    params.perInstance = p.perInstance;
    params.capacity = p.perInstance * p.instances;
    // How the records are put in order afterwards: the topology and primitive restart the draw had.
    bool dynamic = false;
    const std::string baked = shaderObject ? "" : PipelineTopology(source, dynamic);
    p.topology = shaderObject || dynamic ? _overlayTopology : baked;
    if (shaderObject || PipelineDynamic(source, "VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE"))
    {
        p.restart = _reissueRestart == 1;
    }
    else
    {
        const JValue* assembly = PipelineState(source, "pInputAssemblyState", "VERTEX_INPUT_INTERFACE");
        const JValue* restart = assembly ? assembly->Get("primitiveRestartEnable") : nullptr;
        p.restart = restart && (restart->IsBool() ? restart->boolean : restart->Uint() != 0);
    }
    _meshStores.active = true;
    _meshStores.params = params;
    _meshStores.layout = layout;
    _meshStores.key = ((uint64_t)set << 56) ^ ((uint64_t)(uint32_t)params.firstInstance << 40) ^ ((uint64_t)params.perInstance << 16) ^
        (uint64_t)(uint32_t)params.base ^ ((uint64_t)params.capacity << 24) ^ 1;
    return true;
}

void Replayer::MeshStoreCode(uint64_t objectId, const std::string& entry, XfbPatch& layout, std::vector<uint32_t>& words)
{
    // The record layout transform feedback would have: what is stored, where in a record.
    std::vector<uint32_t> feedback;
    MeshStageCode(objectId, VK_SHADER_STAGE_VERTEX_BIT, entry, true, layout, feedback, {});
    words.clear();
    if (!layout.error.empty())
        return;
    std::vector<uint32_t> code;
    XfbPatch unused;
    MeshStageCode(objectId, VK_SHADER_STAGE_VERTEX_BIT, entry, false, unused, code, {});
    const StorePatch stored = PatchForVertexStores(code.data(), code.size(), entry, layout.outputs, layout.stride, _meshStores.params);
    if (!stored.error.empty())
    {
        layout.error = "the vertex shader could not be edited to store its outputs: " + stored.error;
        return;
    }
    words = stored.words;
}

VkShaderEXT Replayer::StoreShader(uint64_t shaderId)
{
    const auto key = std::make_pair(shaderId, _meshStores.key);
    if (auto it = _meshStoreShaders.find(key); it != _meshStoreShaders.end())
        return it->second;
    _meshStoreShaders[key] = VK_NULL_HANDLE;
    XfbPatch& layout = _xfbLayouts[shaderId];
    VkShaderCreateInfoEXT info{};
    std::string error;
    if (!ShaderObjectInfo(shaderId, info, error))
    {
        layout.error = "the vertex shader object could not be made again: " + error;
        return VK_NULL_HANDLE;
    }
    const std::string entry = info.pName ? info.pName : "main";
    _arena.Reset();
    std::vector<uint32_t> words;
    MeshStoreCode(shaderId, entry, layout, words);
    if (!layout.error.empty())
        return VK_NULL_HANDLE;
    VkShaderEXT shader = ShaderObjectWithCode(shaderId, words.data(), words.size(), error, _meshStoreSetLayout);
    if (!shader)
        layout.error = "the edited vertex shader object was refused: " + error;
    _meshStoreShaders[key] = shader;
    return shader;
}

bool Replayer::BindMeshStores(VkCommandBuffer cb, uint64_t source)
{
    PendingMesh& p = *_meshTarget;
    const MeshStores stores = _meshStores;
    _meshStores.active = false;
    auto layout = _xfbLayouts.find(source);
    if (layout == _xfbLayouts.end() || !layout->second.stride || !stores.layout)
        return false;
    const uint64_t bytes = (uint64_t)stores.params.capacity * layout->second.stride;
    if (bytes > kMaxMeshBytes)
    {
        layout->second.error = "the draw's vertices would take more than 256 MB of outputs";
        return false;
    }
    if (!_meshStorePool)
    {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = 64;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        if (_fns.CreateDescriptorPool(_device, &info, nullptr, &_meshStorePool) != VK_SUCCESS)
            return false;
        Track("VkDescriptorPool", (uint64_t)_meshStorePool);
    }
    if (!CreateStaging(bytes, p.buffer, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;
    std::memset(p.buffer.mapped, 0, (size_t)bytes);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = _meshStorePool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &_meshStoreSetLayout;
    if (_fns.AllocateDescriptorSets(_device, &allocate, &p.set) != VK_SUCCESS)
    {
        p.set = VK_NULL_HANDLE;
        DestroyStaging(p.buffer);
        return false;
    }
    VkDescriptorBufferInfo range{p.buffer.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = p.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &range;
    _fns.UpdateDescriptorSets(_device, 1, &write, 0, nullptr);
    // After the application's sets: what it bound stays bound (the layout is compatible up to them).
    _fns.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, stores.layout, stores.params.set, 1, &p.set, 0, nullptr);
    p.pipeline = source;
    return true;
}

bool Replayer::PrepareMeshBuffers(uint64_t source)
{
    if (!_meshTarget)
        return false;
    auto layout = _xfbLayouts.find(source);
    if (layout == _xfbLayouts.end() || !layout->second.stride)
        return false;
    bool dynamic = false;
    const std::string topology = PipelineTopology(source, dynamic);
    // A strip or fan is captured as a list: up to three vertices a primitive. A geometry shader emits
    // at most its declared vertices per primitive, strips again as lists; a tessellator's output has
    // no bound the draw states, so it is given room for a finely divided patch and marked truncated
    // past that.
    uint64_t perVertex = dynamic || topology.find("STRIP") != std::string::npos || topology.find("FAN") != std::string::npos ? 3 : 1;
    if (layout->second.maxVerticesOut)
        perVertex = (uint64_t)layout->second.maxVerticesOut * 3;
    else if (layout->second.stage == "tessellation evaluation")
        perVertex = 1024;
    // One vertex more than expected, so a buffer written to the end means the draw wrote more.
    const uint64_t vertices = (_meshTarget->estimate ? _meshTarget->estimate * perVertex : kIndirectVertices) + 1;
    const uint64_t bytes = std::clamp<uint64_t>(vertices * layout->second.stride, layout->second.stride, kMaxMeshBytes / layout->second.stride * layout->second.stride);
    _meshTarget->pipeline = source;
    if (!CreateStaging(bytes, _meshTarget->buffer, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT) ||
        !CreateStaging(sizeof(uint32_t), _meshTarget->counter, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT))
    {
        DestroyStaging(_meshTarget->buffer);
        DestroyStaging(_meshTarget->counter);
        return false;
    }
    std::memset(_meshTarget->counter.mapped, 0, sizeof(uint32_t));
    return true;
}

void Replayer::CompleteMesh(bool submitted)
{
    for (PendingMesh& p : _pendingMeshes)
    {
        MeshResult& r = _report->meshes[p.result];
        if (!submitted)
        {
            r.note = "the submission holding the draw did not run";
        }
        else if (p.stores && p.buffer.mapped && r.stride)
        {
            // The draw's vertices in order: its index values, or its vertices from the first.
            std::vector<uint32_t> order;
            const uint32_t restartValue = p.indexType == VK_INDEX_TYPE_UINT16 ? 0xFFFFu : p.indexType == VK_INDEX_TYPE_UINT8_EXT ? 0xFFu : 0xFFFFFFFFu;
            if (p.indexed && p.indices.mapped)
            {
                const auto* bytes = static_cast<const uint8_t*>(p.indices.mapped);
                for (uint32_t i = 0; i < p.count; ++i)
                {
                    uint32_t v = 0;
                    if (p.indexType == VK_INDEX_TYPE_UINT16)
                        v = (uint32_t)bytes[i * 2] | (uint32_t)bytes[i * 2 + 1] << 8;
                    else if (p.indexType == VK_INDEX_TYPE_UINT8_EXT)
                        v = bytes[i];
                    else
                        std::memcpy(&v, bytes + i * 4, 4);
                    order.push_back(v);
                }
            }
            else if (!p.indexed)
            {
                for (uint32_t i = 0; i < p.count; ++i)
                    order.push_back(i);
            }
            // Split where primitive restart does, then each run of vertices assembled as transform
            // feedback writes it: lists as they are, strips and fans as lists.
            std::vector<std::vector<uint32_t>> runs(1);
            for (uint32_t v : order)
            {
                if (p.indexed && p.restart && v == restartValue)
                    runs.emplace_back();
                else
                    runs.back().push_back(v);
            }
            const std::string topology = p.topology;
            const bool triangles = topology.find("TRIANGLE") != std::string::npos;
            const bool lines = topology.find("LINE") != std::string::npos;
            const bool strip = topology.find("STRIP") != std::string::npos;
            const bool fan = topology.find("FAN") != std::string::npos;
            std::vector<uint32_t> assembled;
            bool known = topology.find("ADJACENCY") == std::string::npos && topology.find("PATCH") == std::string::npos && !topology.empty();
            for (const auto& run : runs)
            {
                const size_t n = run.size();
                if (fan)
                {
                    for (size_t i = 0; i + 2 < n; ++i)
                        assembled.insert(assembled.end(), {run[i + 1], run[i + 2], run[0]});
                }
                else if (strip && triangles)
                {
                    for (size_t i = 0; i + 2 < n; ++i)
                        assembled.insert(assembled.end(), {run[i], run[i + 1 + (i % 2)], run[i + 2 - (i % 2)]});
                }
                else if (strip && lines)
                {
                    for (size_t i = 0; i + 1 < n; ++i)
                        assembled.insert(assembled.end(), {run[i], run[i + 1]});
                }
                else
                {
                    const size_t per = triangles ? 3 : lines ? 2 : 1;
                    assembled.insert(assembled.end(), run.begin(), run.begin() + (ptrdiff_t)(n / per * per));
                }
            }
            if (!known)
            {
                r.note = "without transform feedback, a draw of " + (topology.empty() ? std::string("an unknown topology") : topology) + " is not put in order";
            }
            else
            {
                const auto* data = static_cast<const uint8_t*>(p.buffer.mapped);
                r.data.assign((size_t)assembled.size() * p.instances * r.stride, 0);
                size_t at = 0;
                for (uint32_t instance = 0; instance < p.instances; ++instance)
                {
                    for (uint32_t v : assembled)
                    {
                        if (v < p.perInstance)
                            std::memcpy(r.data.data() + at, data + ((size_t)instance * p.perInstance + v) * r.stride, r.stride);
                        else
                            r.truncated = true;
                        at += r.stride;
                    }
                }
                r.vertices = (uint32_t)(assembled.size() * p.instances);
                if (r.truncated)
                    r.note = "the draw names vertices past what was kept of its outputs: those read as zero";
            }
        }
        else if (p.buffer.mapped && p.counter.mapped && r.stride)
        {
            // The counter holds the byte offset the next vertex would have been written at.
            uint32_t written = 0;
            std::memcpy(&written, p.counter.mapped, sizeof(written));
            const uint64_t bytes = std::min<uint64_t>(written, p.buffer.size) / r.stride * r.stride;
            r.truncated = written >= p.buffer.size;
            r.vertices = (uint32_t)(bytes / r.stride);
            const auto* data = static_cast<const uint8_t*>(p.buffer.mapped);
            r.data.assign(data, data + bytes);
            if (r.truncated)
                r.note = "the draw wrote more vertices than were captured";
        }
        DestroyStaging(p.buffer);
        DestroyStaging(p.counter);
        DestroyStaging(p.indices);
        if (p.set && _meshStorePool)
            _fns.FreeDescriptorSets(_device, _meshStorePool, 1, &p.set);
    }
    _pendingMeshes.clear();
    ReleaseTransients();
}

} // namespace vkreplay
