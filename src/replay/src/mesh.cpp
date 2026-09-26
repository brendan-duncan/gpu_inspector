#include "replayer.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "identity_patch.h"
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
        if (!_xfbAvailable)
        {
            result.note = "this GPU cannot capture vertex shader outputs (no VK_EXT_transform_feedback)";
            _report->meshes.push_back(std::move(result));
            return;
        }

        // How many vertices the draw assembles, from its arguments; the buffer is sized from that once the
        // pipeline's record layout is known, at the draw (PrepareMeshBuffers).
        PendingMesh p;
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
    }
    _pendingMeshes.clear();
    ReleaseTransients();
}

} // namespace vkreplay
