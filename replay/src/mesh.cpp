#include "replayer.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "util.h"

namespace vkreplay {

// ---------------------------------------------------------------------------------------------
// Mesh output
//
// What a draw's vertex shader wrote, for the mesh output view: RenderDoc's VS Out (vk_postvs.cpp).
// Recorded right after the replay has executed the draw's pass, like the overlays, so the vertex
// buffers, descriptor sets and push constants the shader reads hold what they held for the draw.
// The pass's state is issued again and then the draw alone, with a copy of its pipeline whose vertex
// shader writes transform feedback (xfb_patch.cpp) and which rasterizes nothing. The buffer holds
// every vertex the draw assembled, in the order it assembled them, and the counter says how much of
// it was written.

namespace {

/** Bytes a mesh may take: a draw that writes more is captured up to here, and marked truncated. */
constexpr uint64_t kMaxMeshBytes = 256ull * 1024 * 1024;
/** Vertices assumed for an indirect draw, whose counts are in a buffer. */
constexpr uint64_t kIndirectVertices = 1ull << 20;

uint64_t ArgUint(const JValue* args, const char* name, uint64_t fallback = 0) {
    const JValue* v = args ? args->Get(name) : nullptr;
    return v ? v->Uint() : fallback;
}

} // namespace

std::string Replayer::PipelineTopology(uint64_t pipelineId, bool& dynamic) const {
    dynamic = false;
    const JValue* object = _capture->Object(pipelineId);
    const JValue* args = object ? object->Get("args") : nullptr;
    const JValue* infos = args ? args->Get("pCreateInfos") : nullptr;
    const uint32_t index = object && object->Get("index") ? (uint32_t)object->Get("index")->Uint() : 0;
    if (!infos || !infos->IsArray() || index >= infos->count) return "";
    const JValue& info = infos->items[index];
    if (const JValue* states = info.Get("pDynamicState") ? info.Get("pDynamicState")->Get("pDynamicStates") : nullptr; states && states->IsArray())
        for (uint32_t i = 0; i < states->count; ++i)
            if (Str(&states->items[i]) == "VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY") dynamic = true;
    const JValue* assembly = info.Get("pInputAssemblyState");
    return assembly ? Str(assembly->Get("topology")) : "";
}

void Replayer::RecordMesh(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex) {
    const JValue* commands = _capture->Commands();
    for (uint32_t target : _options.mesh.commands) {
        if (target <= pass.beginIndex || target >= endIndex) continue;
        MeshResult result;
        result.command = target;
        result.commandBuffer = pass.commandBuffer;
        result.frame = pass.frame;
        result.passIndex = pass.index;
        const JValue& command = commands->items[target];
        result.method = Str(command.Get("method"));
        if (!StartsWith(result.method, "vkCmdDraw")) {
            result.note = "command " + std::to_string(target) + " is " + result.method + ", not a draw";
            _report->meshes.push_back(std::move(result));
            continue;
        }
        if (!_xfbAvailable) {
            result.note = "this GPU cannot capture vertex shader outputs (no VK_EXT_transform_feedback)";
            _report->meshes.push_back(std::move(result));
            continue;
        }

        // How many vertices the draw assembles, from its arguments; the buffer is sized from that once the
        // pipeline's record layout is known, at the draw (PrepareMeshBuffers).
        PendingMesh p;
        p.result = _report->meshes.size();
        const JValue* args = command.Get("args");
        if (result.method.find("Indirect") == std::string::npos) {
            const uint64_t instances = std::max<uint64_t>(1, ArgUint(args, "instanceCount", 1));
            const uint64_t count = result.method.find("Indexed") != std::string::npos ? ArgUint(args, "indexCount") : ArgUint(args, "vertexCount");
            p.estimate = count * instances;
        }

        TransientImage colour = CreateTransientImage(VK_FORMAT_R16_SFLOAT, pass.extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        VkRenderPass rp = OverdrawRenderPass(VK_FORMAT_UNDEFINED);
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (colour.image && rp) {
            VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbInfo.renderPass = rp;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &colour.view;
            fbInfo.width = pass.extent.width;
            fbInfo.height = pass.extent.height;
            fbInfo.layers = 1;
            if (_fns.CreateFramebuffer(_device, &fbInfo, nullptr, &fb) == VK_SUCCESS) _transientFramebuffers.push_back(fb);
            else fb = VK_NULL_HANDLE;
        }
        if (!fb) {
            result.note = "no memory for the pass the draw is issued in";
            _report->meshes.push_back(std::move(result));
            continue;
        }

        _overlayTarget = target;
        _overlayOnlyTarget = true;
        _overlayTargetMode = ReissueMode::Xfb;
        _overlayIssued = false;
        _meshTarget = &p;
        ReissuePass(cb, group, pass, endIndex, false, VK_FORMAT_UNDEFINED, rp, fb);
        const bool drawn = _overlayIssued && _overdrawDrawable && p.buffer.buffer;
        const uint64_t pipeline = _overlayPipeline;
        _meshTarget = nullptr;
        _overlayTarget = UINT32_MAX;
        _overlayOnlyTarget = false;

        bool dynamic = false;
        result.topology = pipeline ? PipelineTopology(pipeline, dynamic) : "";
        if (dynamic) result.topology += " (dynamic)";
        auto layout = _xfbLayouts.find(pipeline);
        if (!drawn) {
            result.note = layout != _xfbLayouts.end() && !layout->second.error.empty() ? layout->second.error
                        : !pipeline ? "no pipeline is bound at the draw"
                        : "the draw could not be issued again (its pipeline could not be copied, or there was no memory for its vertices)";
            DestroyStaging(p.buffer);
            DestroyStaging(p.counter);
            _report->meshes.push_back(std::move(result));
            continue;
        }
        result.stride = layout->second.stride;
        result.outputs = layout->second.outputs;
        _report->meshes.push_back(std::move(result));
        _pendingMeshes.push_back(p);
    }
}

bool Replayer::PrepareMeshBuffers() {
    if (!_meshTarget) return false;
    auto layout = _xfbLayouts.find(_overlayPipeline);
    if (layout == _xfbLayouts.end() || !layout->second.stride) return false;
    bool dynamic = false;
    const std::string topology = PipelineTopology(_overlayPipeline, dynamic);
    // A strip or fan is captured as a list: up to three vertices a primitive.
    const uint64_t perVertex = dynamic || topology.find("STRIP") != std::string::npos || topology.find("FAN") != std::string::npos ? 3 : 1;
    // One vertex more than expected, so a buffer written to the end means the draw wrote more.
    const uint64_t vertices = (_meshTarget->estimate ? _meshTarget->estimate * perVertex : kIndirectVertices) + 1;
    const uint64_t bytes = std::clamp<uint64_t>(vertices * layout->second.stride, layout->second.stride, kMaxMeshBytes / layout->second.stride * layout->second.stride);
    _meshTarget->pipeline = _overlayPipeline;
    if (!CreateStaging(bytes, _meshTarget->buffer, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT) ||
        !CreateStaging(sizeof(uint32_t), _meshTarget->counter, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT)) {
        DestroyStaging(_meshTarget->buffer);
        DestroyStaging(_meshTarget->counter);
        return false;
    }
    std::memset(_meshTarget->counter.mapped, 0, sizeof(uint32_t));
    return true;
}

void Replayer::CompleteMesh(bool submitted) {
    for (PendingMesh& p : _pendingMeshes) {
        MeshResult& r = _report->meshes[p.result];
        if (!submitted) {
            r.note = "the submission holding the draw did not run";
        } else if (p.buffer.mapped && p.counter.mapped && r.stride) {
            // The counter holds the byte offset the next vertex would have been written at.
            uint32_t written = 0;
            std::memcpy(&written, p.counter.mapped, sizeof(written));
            const uint64_t bytes = std::min<uint64_t>(written, p.buffer.size) / r.stride * r.stride;
            r.truncated = written >= p.buffer.size;
            r.vertices = (uint32_t)(bytes / r.stride);
            const auto* data = static_cast<const uint8_t*>(p.buffer.mapped);
            r.data.assign(data, data + bytes);
            if (r.truncated) r.note = "the draw wrote more vertices than were captured";
        }
        DestroyStaging(p.buffer);
        DestroyStaging(p.counter);
    }
    _pendingMeshes.clear();
    ReleaseTransients();
}

} // namespace vkreplay
