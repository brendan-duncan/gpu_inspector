#include "replayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "format_info.h"
#include "util.h"

namespace vkreplay
{

// ---------------------------------------------------------------------------------------------
// Overdraw
//
// Every render pass is measured right after the replay has executed it, in the same command
// buffer, so the buffers and descriptor sets its draws read hold what they held for the pass.
// Its commands are issued again inside a pass of the replay's own into a count target the size
// of the pass's framebuffer: every graphics pipeline bound is replaced by a copy whose fragment
// stage writes 1.0, blended additively (ONE, ONE) into R16_SFLOAT. Twice:
//   * with the pass's depth and stencil tests, against a copy of the depth the pass started
//     from (its load) or its clear value: the fragments that passed, in draw order;
//   * without depth and stencil: every fragment the draws rasterized.
// Limits: fragments a shader discards are counted (the counting shader does not discard),
// and a multiview pass's pipelines cannot be copied into a single-view pass.

bool Replayer::ArgsResolve(const std::string& method, const JValue& args)
{
    DecodeCheckFn fn = FindArgsDecoder(method);
    if (!fn)
        return true;
    const size_t unresolved = _ctx.unresolved;
    const size_t problems = _ctx.problems.size();
    fn(_ctx, args);
    const bool ok = _ctx.unresolved == unresolved;
    _ctx.unresolved = unresolved;
    _ctx.problems.resize(problems);
    _arena.Reset();
    return ok;
}

void Replayer::Barrier(VkCommandBuffer cb, VkImage image, const VkImageSubresourceRange& range, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    _fns.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

Replayer::TransientImage Replayer::CreateTransientImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkSampleCountFlagBits samples)
{
    TransientImage t;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (_fns.CreateImage(_device, &info, nullptr, &t.image) != VK_SUCCESS)
        return TransientImage{};
    VkMemoryRequirements req{};
    _fns.GetImageMemoryRequirements(_device, t.image, &req);
    if (!AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.memory, false) || _fns.BindImageMemory(_device, t.image, t.memory, 0) != VK_SUCCESS)
    {
        if (t.memory)
            _fns.FreeMemory(_device, t.memory, nullptr);
        _fns.DestroyImage(_device, t.image, nullptr);
        return TransientImage{};
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {vkinsp::FormatAspects(format), 0, 1, 0, 1};
    _fns.CreateImageView(_device, &view, nullptr, &t.view);
    _transientImages.push_back(t);
    return t;
}

void Replayer::ReleaseTransients()
{
    if (!_device)
        return;
    for (VkFramebuffer fb : _transientFramebuffers)
        _fns.DestroyFramebuffer(_device, fb, nullptr);
    for (TransientImage& t : _transientImages)
    {
        if (t.view)
            _fns.DestroyImageView(_device, t.view, nullptr);
        if (t.image)
            _fns.DestroyImage(_device, t.image, nullptr);
        if (t.memory)
            _fns.FreeMemory(_device, t.memory, nullptr);
    }
    _transientFramebuffers.clear();
    _transientImages.clear();
}

VkRenderPass Replayer::OverdrawRenderPass(VkFormat depthFormat)
{
    auto it = _overdrawRenderPasses.find(depthFormat);
    if (it != _overdrawRenderPasses.end())
        return it->second;
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = VK_FORMAT_R16_SFLOAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    const bool hasDepth = depthFormat != VK_FORMAT_UNDEFINED;
    if (hasDepth)
        subpass.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = hasDepth ? 2 : 1;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (_fns.CreateRenderPass(_device, &info, nullptr, &rp) == VK_SUCCESS)
        Track("VkRenderPass", (uint64_t)rp);
    _overdrawRenderPasses[depthFormat] = rp;
    return rp;
}

VkPipeline Replayer::OverdrawPipeline(uint64_t pipelineId, bool depthTested, VkFormat depthFormat, ReissueMode mode)
{
    const auto key = std::make_tuple(pipelineId, depthTested, depthFormat, mode, _reissueRendering);
    auto it = _overdrawPipelines.find(key);
    if (it != _overdrawPipelines.end())
        return it->second;
    _overdrawPipelines[key] = VK_NULL_HANDLE;  // a copy that cannot be made is not tried again
    const bool hasDepth = depthFormat != VK_FORMAT_UNDEFINED;
    const JValue* object = _capture->Object(pipelineId);
    // A pass that holds shader-object draws is reissued in dynamic rendering, with the same attachments.
    const VkFormat countFormat = VK_FORMAT_R16_SFLOAT;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &countFormat;
    if (hasDepth && (vkinsp::FormatAspects(depthFormat) & VK_IMAGE_ASPECT_DEPTH_BIT))
        rendering.depthAttachmentFormat = depthFormat;
    if (hasDepth && (vkinsp::FormatAspects(depthFormat) & VK_IMAGE_ASPECT_STENCIL_BIT))
        rendering.stencilAttachmentFormat = depthFormat;
    VkPipeline pipeline = CopyGraphicsPipeline(pipelineId, mode == ReissueMode::Count ? "overdraw" : mode == ReissueMode::Xfb ? "mesh"
                                                                                                                              : "overlay",
        [&](PipelineCopy& p) {
            if (mode == ReissueMode::Xfb)
            {
            // The vertex shader edited to write its outputs to the feedback buffer, and nothing rasterized.
                XfbPatch& layout = _xfbLayouts[pipelineId];
                for (const VkPipelineShaderStageCreateInfo& s : p.stages)
                {
                    if (s.stage & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT))
                    {
                        layout.error = "the pipeline has tessellation or geometry stages, and only a vertex shader's outputs are captured";
                        return false;
                    }
                }
                auto vs = std::find_if(p.stages.begin(), p.stages.end(), [](const VkPipelineShaderStageCreateInfo& s) { return s.stage == VK_SHADER_STAGE_VERTEX_BIT; });
                const uint8_t* data = nullptr;
                size_t size = 0;
                const std::string entry = vs != p.stages.end() && vs->pName ? vs->pName : "main";
                if (vs == p.stages.end() || !object || !_capture->Blob(*object, std::string(StageName(VK_SHADER_STAGE_VERTEX_BIT)) + ":" + entry, data, size))
                {
                    layout.error = "the capture has no vertex shader code for the pipeline";
                    return false;
                }
                std::vector<uint32_t> words(size / 4);
                std::memcpy(words.data(), data, words.size() * 4);
                layout = PatchForTransformFeedback(words.data(), words.size(), entry);
                if (!layout.error.empty())
                    return false;
                VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                m.codeSize = layout.words.size() * 4;
                m.pCode = layout.words.data();
                VkShaderModule module = VK_NULL_HANDLE;
                const VkResult created = _fns.CreateShaderModule(_device, &m, nullptr, &module);
                layout.words.clear();
                layout.words.shrink_to_fit();
                if (created != VK_SUCCESS)
                {
                    layout.error = "the edited vertex shader was refused (" + std::to_string(created) + ")";
                    return false;
                }
                p.temporary.push_back(module);
                vs->module = module;
                vs->pNext = nullptr;
                if (!p.hasRasterization)
                {
                    p.rasterization = VkPipelineRasterizationStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                    p.rasterization.lineWidth = 1.0f;
                    p.hasRasterization = true;
                }
                p.rasterization.rasterizerDiscardEnable = VK_TRUE;
                p.RemoveDynamic({VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE});
            // Nothing is rasterized, so a fragment stage is not allowed.
                p.stages.erase(std::remove_if(p.stages.begin(), p.stages.end(),
                                   [](const VkPipelineShaderStageCreateInfo& s) { return s.stage == VK_SHADER_STAGE_FRAGMENT_BIT; }),
                    p.stages.end());
            }
            else if (p.hasRasterization && p.rasterization.rasterizerDiscardEnable)
            {
                return false;  // no fragments to count
            }
            if (mode == ReissueMode::BackFace)
            {
            // Its own geometry with nothing culled: what is left is where the faces the draw's cull
            // mode threw away would have landed.
                if (!p.hasRasterization)
                {
                    p.rasterization = VkPipelineRasterizationStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                    p.rasterization.lineWidth = 1.0f;
                    p.hasRasterization = true;
                }
                p.rasterization.cullMode = VK_CULL_MODE_NONE;
                p.RemoveDynamic({VK_DYNAMIC_STATE_CULL_MODE});
            }
            if (mode == ReissueMode::Wireframe)
            {
            // The draw's edges, one pixel wide, whatever the application set.
                if (!p.hasRasterization)
                    return false;
                p.rasterization.polygonMode = VK_POLYGON_MODE_LINE;
                p.rasterization.lineWidth = 1.0f;
                p.RemoveDynamic({VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_POLYGON_MODE_EXT});
            }
            if (mode == ReissueMode::BackFace)
                p.ReplaceFragment(BackFaceModule());
            else if (mode != ReissueMode::Xfb)
                p.ReplaceFragment(CountModule());
            VkPipelineColorBlendAttachmentState add{};
            add.blendEnable = VK_TRUE;
            add.srcColorBlendFactor = add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            add.srcAlphaBlendFactor = add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            add.colorBlendOp = add.alphaBlendOp = VK_BLEND_OP_ADD;
        // An overlay's other draws only move the depth and stencil the draw it is for is tested against.
            add.colorWriteMask = mode == ReissueMode::DepthOnly ? 0 : VK_COLOR_COMPONENT_R_BIT;
            p.blendAttachments = {add};
            p.blend = VkPipelineColorBlendStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            p.hasBlend = true;
            p.multisample = VkPipelineMultisampleStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            p.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            p.hasMultisample = true;
            if (!hasDepth)
            {
                p.hasDepthStencil = false;
            }
            else if (!depthTested || !p.hasDepthStencil)
            {
                p.depthStencil = VkPipelineDepthStencilStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
                p.hasDepthStencil = true;
            }
            if (mode == ReissueMode::StencilOnly && p.hasDepthStencil)
            {
            // The stencil test on its own: the depth test is what the Depth Test overlay answers,
            // and a fragment rejected by both would be reported as the stencil's doing.
                p.depthStencil.depthTestEnable = VK_FALSE;
                p.depthStencil.depthWriteEnable = VK_FALSE;
                p.depthStencil.depthBoundsTestEnable = VK_FALSE;
                p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE});
            }
        // Dynamic states that would undo the count, or the tests the untested count leaves out.
            p.RemoveDynamic(kColorOutputDynamicStates);
            p.RemoveDynamic(kMultisampleDynamicStates);
            if (!depthTested || !hasDepth)
            {
                p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE});
            }
            p.info.pNext = StripPNext(p.info.pNext, {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO});
            if (_reissueRendering)
            {
                rendering.pNext = p.info.pNext;
                p.info.pNext = &rendering;
                p.info.renderPass = VK_NULL_HANDLE;
            }
            else
            {
                p.info.renderPass = OverdrawRenderPass(hasDepth ? depthFormat : VK_FORMAT_UNDEFINED);
            }
            p.info.subpass = 0;
            return true;
        });
    _overdrawPipelines[key] = pipeline;
    return pipeline;
}

void Replayer::PrepareOverdraw(VkCommandBuffer cb, PassState& pass)
{
    if (!pass.extent.width || !pass.extent.height)
        return;
    pass.overdraw = true;
    if (pass.depthFormat == VK_FORMAT_UNDEFINED || !pass.depthImage)
        return;
    auto sit = _images.find(pass.depthImage);
    pass.overdrawDepth = sit == _images.end() ? TransientImage{} : CreateTransientImage(pass.depthFormat, pass.extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!pass.overdrawDepth.image)
    {
        pass.depthFormat = VK_FORMAT_UNDEFINED;
        return;
    }
    const VkImageAspectFlags aspects = vkinsp::FormatAspects(pass.depthFormat);
    const VkImageSubresourceRange full{aspects, 0, 1, 0, 1};
    Barrier(cb, pass.overdrawDepth.image, full, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (pass.depthLoadOp != VK_ATTACHMENT_LOAD_OP_CLEAR && pass.depthLayoutBefore != VK_IMAGE_LAYOUT_UNDEFINED)
    {
        const ImageRecord& src = sit->second;
        const VkImageSubresourceRange srcRange{aspects, pass.depthRange.baseMipLevel, 1, pass.depthRange.baseArrayLayer, 1};
        Barrier(cb, src.image, srcRange, pass.depthLayoutBefore, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {aspects, pass.depthRange.baseMipLevel, pass.depthRange.baseArrayLayer, 1};
        copy.dstSubresource = {aspects, 0, 0, 1};
        copy.extent = {std::min(pass.extent.width, std::max(1u, src.extent.width >> pass.depthRange.baseMipLevel)),
            std::min(pass.extent.height, std::max(1u, src.extent.height >> pass.depthRange.baseMipLevel)), 1};
        _fns.CmdCopyImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pass.overdrawDepth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(cb, src.image, srcRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pass.depthLayoutBefore);
    }
    else
    {
        _fns.CmdClearDepthStencilImage(cb, pass.overdrawDepth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &pass.depthClear, 1, &full);
    }
    Barrier(cb, pass.overdrawDepth.image, full, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

void Replayer::ReissueCommand(VkCommandBuffer cb, uint32_t index, bool depthTested, VkFormat depthFormat, bool insidePass)
{
    const JValue& c = _capture->Commands()->items[index];
    const std::string m = Str(c.Get("method"));
    const JValue* args = c.Get("args");
    if (_overlayTarget != UINT32_MAX && _overlayIssued)
        return;
    if (args && m == "vkCmdBindShadersEXT")
    {
        // Shader objects in place of a pipeline: bound as the application bound them, and each draw
        // then binds its fragment stage to the counting shader and sets the state a pipeline copy
        // would have changed (ReissueShaderObjectDraw). The mesh output binds its own copy of the
        // vertex shader at the target draw instead.
        const JValue* stages = args->Get("pStages");
        const JValue* shaders = args->Get("pShaders");
        bool graphics = false;
        for (uint32_t k = 0; stages && k < stages->count; ++k)
        {
            const std::string stage = Str(&stages->items[k]);
            if (stage == "VK_SHADER_STAGE_COMPUTE_BIT")
                continue;
            graphics = true;
            const uint64_t id = shaders && shaders->IsArray() && k < shaders->count ? IdOf(&shaders->items[k]) : 0;
            if (id)
                _reissueLayoutShader = id;
            if (stage == "VK_SHADER_STAGE_VERTEX_BIT")
                _overlayVertexShader = id;
            else if (stage.find("TESSELLATION") != std::string::npos || stage == "VK_SHADER_STAGE_GEOMETRY_BIT")
                _overlayShaderGeometry = _overlayShaderGeometry || id;
        }
        if (graphics)
        {
            _overlayPipeline = 0;
            _reissueShaders = true;
            _overdrawDrawable = _reissueRendering;
        }
        if (ReplayFn fn = FindReplayCommand(m); fn && _reissueRendering)
        {
            const size_t problems = _ctx.problems.size();
            const size_t unresolved = _ctx.unresolved;
            IssueCommand(fn, c, *args, cb);
            _ctx.problems.resize(problems);
            _ctx.unresolved = unresolved;
            _arena.Reset();
        }
        return;
    }
    if (args && (m == "vkCmdSetPrimitiveTopology" || m == "vkCmdSetPrimitiveTopologyEXT"))
        _overlayTopology = Str(args->Get("primitiveTopology"));
    if (!args || (!insidePass && !IsStateCommand(m)) || kOverdrawSkipped.count(m))
        return;
    if ((!depthTested || depthFormat == VK_FORMAT_UNDEFINED) && kDepthState.count(m))
        return;
    const bool overlay = _overlayTarget != UINT32_MAX;
    if (overlay && _overlayIssued)
        return;
    if (m == "vkCmdBindPipeline")
    {
        if (Str(args->Get("pipelineBindPoint")) != "VK_PIPELINE_BIND_POINT_GRAPHICS")
            return;
        // A pipeline unbinds the shader objects, and a copy's static state undoes dynamic state
        // a later shader-object draw needs.
        _reissueShaders = false;
        _reissueStateStale = true;
        if (overlay)
        {
            // The draw the overlay is for gets its own copy when it comes; the rest draw depth only, or not at all.
            _overlayPipeline = IdOf(args->Get("pipeline"));
            _overlayVertexShader = 0;
            _overlayShaderGeometry = false;
            if (_overlayOnlyTarget)
                return;
            VkPipeline pipeline = OverdrawPipeline(_overlayPipeline, depthTested, depthFormat, ReissueMode::DepthOnly);
            _overdrawDrawable = pipeline != VK_NULL_HANDLE;
            _reissueCopy = pipeline;
            if (pipeline)
                _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            return;
        }
        VkPipeline pipeline = OverdrawPipeline(IdOf(args->Get("pipeline")), depthTested, depthFormat);
        _overlayVertexShader = 0;
        _overlayShaderGeometry = false;
        _overdrawDrawable = pipeline != VK_NULL_HANDLE;
        _reissueCopy = pipeline;
        if (pipeline)
            _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (_options.trace)
        {
            std::fprintf(stderr, "overdraw %u: bind pipeline %llu -> %s\n", index, (unsigned long long)IdOf(args->Get("pipeline")), pipeline ? "counting copy" : "none");
            std::fflush(stderr);
        }
        return;
    }
    const bool draw = StartsWith(m, "vkCmdDraw");
    const bool target = overlay && draw && index == _overlayTarget;
    bool feedback = false;
    if (target && !_overlayPipeline && _overlayVertexShader && _overlayTargetMode == ReissueMode::Xfb)
    {
        // Shader objects: the vertex shader's feedback copy alone, and nothing rasterized.
        VkShaderEXT vs = VK_NULL_HANDLE;
        if (_overlayShaderGeometry)
            _xfbLayouts[_overlayVertexShader].error = "tessellation or geometry shader objects are bound, and only a vertex shader's outputs are captured";
        else
            vs = FeedbackShader(_overlayVertexShader);
        const auto setDiscard = _fns.CmdSetRasterizerDiscardEnable ? _fns.CmdSetRasterizerDiscardEnable : _fns.CmdSetRasterizerDiscardEnableEXT;
        _overdrawDrawable = vs && setDiscard;
        _overlayIssued = true;  // drawn or not, nothing after it matters
        _overlayDrawnPipeline = _overlayVertexShader;
        _overlayDrawnTopology = _overlayTopology;
        if (_overdrawDrawable)
        {
            const VkShaderStageFlagBits stages[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
            const VkShaderEXT bound[] = {vs, VK_NULL_HANDLE};
            _fns.CmdBindShadersEXT(cb, 2, stages, bound);
            setDiscard(cb, VK_TRUE);
            feedback = PrepareMeshBuffers();
            if (!feedback)
                _overdrawDrawable = false;
        }
        _overlayDrawn = _overdrawDrawable;
    }
    else if (target && !_overlayPipeline && _reissueShaders)
    {
        // Shader objects: bound and set for the overlay's mode like any other reissued draw, below.
        _overdrawDrawable = _reissueRendering && _overlayTargetMode != ReissueMode::Xfb;
        _overlayIssued = true;  // drawn or not, nothing after it matters
        _overlayDrawnPipeline = _overlayVertexShader;
        _overlayDrawn = _overdrawDrawable;
    }
    else if (target)
    {
        VkPipeline pipeline = _overlayPipeline ? OverdrawPipeline(_overlayPipeline, depthTested, depthFormat, _overlayTargetMode) : VK_NULL_HANDLE;
        _overdrawDrawable = pipeline != VK_NULL_HANDLE;
        _reissueCopy = pipeline;
        if (pipeline)
            _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        _overlayIssued = true;  // drawn or not, nothing after it matters
        _overlayDrawnPipeline = _overlayPipeline;
        // The mesh output view: the draw writes its vertices into a buffer instead of rasterizing.
        if (pipeline && _overlayTargetMode == ReissueMode::Xfb)
        {
            feedback = PrepareMeshBuffers();
            if (!feedback)
                _overdrawDrawable = false;
        }
        _overlayDrawn = _overdrawDrawable;
    }
    else if (overlay && draw && _overlayOnlyTarget)
    {
        return;
    }
    // A shader-object draw is edited here, right before it: the overlay's other draws move only the
    // depth and stencil, and the mesh output's target has bound its own shaders already.
    if (draw && _overdrawDrawable && !_overlayPipeline && _reissueShaders && !feedback)
    {
        const ReissueMode mode = target ? _overlayTargetMode : overlay ? ReissueMode::DepthOnly : ReissueMode::Count;
        _overdrawDrawable = ReissueShaderObjectDraw(cb, mode, depthTested, depthFormat);
        if (target)
            _overlayDrawn = _overdrawDrawable;
    }
    if (draw && !_overdrawDrawable)
    {
        ++_overdrawSkippedDraws;
        return;
    }
    if (StartsWith(m, "vkCmdSet"))
    {
        _reissueSetCommands.push_back(index);
        NoteShaderObjectSet(_reissueSets, m, *args);
        // What the bound copy holds statically is not set; a later shader-object draw sets it again.
        if (!_reissueShaders && _reissueCopy)
            if (const std::string state = DynamicStateOf(m); !state.empty() && !TakesDynamic(_reissueCopy, 0, state))
                return;
    }
    ReplayFn fn = FindReplayCommand(m);
    if (!fn)
        return;
    if (draw)
        ++_overdrawDraws;
    if (_options.trace)
    {
        std::fprintf(stderr, "overdraw %u: %s\n", index, m.c_str());
        std::fflush(stderr);
    }
    // Problems were reported when the pass itself was replayed.
    const size_t problems = _ctx.problems.size();
    const size_t unresolved = _ctx.unresolved;
    VkDeviceSize zero = 0;
    if (feedback)
    {
        _fns.CmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &_meshTarget->buffer.buffer, &zero, &_meshTarget->buffer.size);
        _fns.CmdBeginTransformFeedbackEXT(cb, 0, 0, nullptr, nullptr);
    }
    IssueCommand(fn, c, *args, cb);
    if (feedback)
        _fns.CmdEndTransformFeedbackEXT(cb, 0, 1, &_meshTarget->counter.buffer, &zero);
    _ctx.problems.resize(problems);
    _ctx.unresolved = unresolved;
    _arena.Reset();
}

void Replayer::ReissuePass(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, bool depthTested,
    VkFormat depthFormat, VkRenderPass renderPass, VkFramebuffer framebuffer, VkImageView color, VkImageView depth)
{
    const JValue* commands = _capture->Commands();
    const bool dynamicRendering = !renderPass;
    _reissueRendering = dynamicRendering;
    _reissueShaders = false;
    _reissueSetCommands.clear();
    _reissueLayoutShader = 0;
    _reissueCopy = VK_NULL_HANDLE;
    _reissueSets = ShaderObjectSets{};
    _reissueStateStale = false;
    // The state the pass inherited from the command buffer, then the pass's own commands.
    _overdrawDrawable = false;
    _overdrawDraws = 0;
    _overdrawSkippedDraws = 0;
    _overlayPipeline = 0;
    _overlayVertexShader = 0;
    _overlayShaderGeometry = false;
    _overlayTopology.clear();
    for (uint32_t i = group.first + 1; i < pass.beginIndex; ++i)
        if (!commands->items[i].Get("secondary"))
            ReissueCommand(cb, i, depthTested, depthFormat, false);
    VkClearValue clear{};
    if (dynamicRendering)
    {
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = color;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue = clear;
        // The depth the draws are tested against, loaded and stored as OverdrawRenderPass does.
        VkRenderingAttachmentInfo depthStencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthStencil.imageView = depth;
        depthStencil.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthStencil.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthStencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const VkImageAspectFlags aspects = depth && depthFormat != VK_FORMAT_UNDEFINED ? vkinsp::FormatAspects(depthFormat) : 0;
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, pass.extent};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &attachment;
        info.pDepthAttachment = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &depthStencil : nullptr;
        info.pStencilAttachment = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &depthStencil : nullptr;
        _fns.CmdBeginRendering(cb, &info);
    }
    else
    {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = renderPass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {{0, 0}, pass.extent};
        begin.clearValueCount = 1;
        begin.pClearValues = &clear;
        _fns.CmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    }
    for (uint32_t i = pass.beginIndex + 1; i < endIndex; ++i)
    {
        const JValue& c = commands->items[i];
        if (c.Get("secondary"))
            continue;
        if (Str(c.Get("method")) == "vkCmdExecuteCommands")
        {
            // Secondary command buffers' commands are issued inline, in the order they executed.
            const JValue* list = c.Get("args") ? c.Get("args")->Get("pCommandBuffers") : nullptr;
            for (uint32_t s = 0; list && s < list->count; ++s)
            {
                const uint64_t id = IdOf(&list->items[s]);
                _overdrawDrawable = false;  // a secondary starts without a pipeline
                _reissueShaders = false;
                _reissueCopy = VK_NULL_HANDLE;
                _overlayPipeline = 0;
                _overlayVertexShader = 0;
                _overlayShaderGeometry = false;
                for (uint32_t j = i + 1; j < commands->count && commands->items[j].Get("secondary"); ++j)
                    if (commands->items[j].Get("secondary")->Uint() == id)
                        ReissueCommand(cb, j, depthTested, depthFormat, true);
            }
            continue;
        }
        ReissueCommand(cb, i, depthTested, depthFormat, true);
    }
    if (dynamicRendering)
        _fns.CmdEndRendering(cb);
    else
        _fns.CmdEndRenderPass(cb);
}

bool Replayer::DrawUsesShaderObjects(const CommandGroup& group, uint32_t target) const
{
    const JValue* commands = _capture->Commands();
    const auto secondaryOf = [&](uint32_t i) -> uint64_t {
        const JValue* s = commands->items[i].Get("secondary");
        return s ? s->Uint() : 0;
    };
    const uint64_t secondary = secondaryOf(target);
    for (uint32_t i = target; i-- > group.first;)
    {
        if (secondaryOf(i) != secondary)
            continue;
        const JValue& c = commands->items[i];
        const std::string m = Str(c.Get("method"));
        const JValue* args = c.Get("args");
        if (!args)
            continue;
        if (m == "vkCmdBindPipeline" && Str(args->Get("pipelineBindPoint")) == "VK_PIPELINE_BIND_POINT_GRAPHICS")
            return false;
        if (m != "vkCmdBindShadersEXT")
            continue;
        const JValue* stages = args->Get("pStages");
        for (uint32_t k = 0; stages && k < stages->count; ++k)
            if (Str(&stages->items[k]) == "VK_SHADER_STAGE_VERTEX_BIT")
                return true;
    }
    return false;
}

void Replayer::RecordOverdraw(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, std::vector<PendingOverdraw>& pending)
{
    if (!pass.overdraw)
        return;
    int64_t measured = -1;
    if (const JValue* timings = _capture->Manifest().Get("passTimings"); timings && timings->IsArray())
    {
        for (uint32_t t = 0; t < timings->count; ++t)
        {
            const JValue& p = timings->items[t];
            if (Str(p.Get("kind")) == "compute" || p.Get("commandBuffer")->Uint() != pass.commandBuffer ||
                p.Get("passIndex")->Uint() != pass.index || (p.Get("frame") ? p.Get("frame")->Uint() : 0) != pass.frame)
                continue;
            if (const JValue* counters = p.Get("counters"); counters && counters->Get("fragmentInvocations"))
                measured = (int64_t)counters->Get("fragmentInvocations")->Uint();
        }
    }
    // Shader objects draw only in dynamic rendering; a pass without them keeps the render pass.
    const bool shaderObjects = PassUsesShaderObjects(group, endIndex) && _fns.CmdBeginRendering;
    for (int mode = 0; mode < 2; ++mode)
    {
        const bool tested = mode == 0;
        const VkFormat depthFormat = tested ? pass.depthFormat : VK_FORMAT_UNDEFINED;
        OverdrawResult result;
        result.commandBuffer = pass.commandBuffer;
        result.frame = pass.frame;
        result.passIndex = pass.index;
        result.depthTested = tested;
        result.width = pass.extent.width;
        result.height = pass.extent.height;
        result.capturedFragments = measured;
        if (tested && depthFormat == VK_FORMAT_UNDEFINED)
            result.note = "the pass has no depth attachment";

        TransientImage count = CreateTransientImage(VK_FORMAT_R16_SFLOAT, pass.extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkRenderPass rp = OverdrawRenderPass(depthFormat);
        if (!count.image || !rp)
        {
            result.note = "no memory for the count target";
            _report->overdraw.push_back(std::move(result));
            continue;
        }
        if (shaderObjects)
        {
            // The count target is cleared by the pass's begin, from whatever it held.
            Barrier(cb, count.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            rp = VK_NULL_HANDLE;
        }
        VkImageView views[2] = {count.view, pass.overdrawDepth.view};
        VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = rp;
        fbInfo.attachmentCount = depthFormat != VK_FORMAT_UNDEFINED ? 2 : 1;
        fbInfo.pAttachments = views;
        fbInfo.width = pass.extent.width;
        fbInfo.height = pass.extent.height;
        fbInfo.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (rp && _fns.CreateFramebuffer(_device, &fbInfo, nullptr, &fb) != VK_SUCCESS)
        {
            result.note = "the count target's framebuffer could not be created";
            _report->overdraw.push_back(std::move(result));
            continue;
        }
        if (fb)
            _transientFramebuffers.push_back(fb);

        ReissuePass(cb, group, pass, endIndex, tested, depthFormat, rp, fb, count.view, depthFormat != VK_FORMAT_UNDEFINED ? pass.overdrawDepth.view : VK_NULL_HANDLE);
        result.draws = _overdrawDraws;
        result.skippedDraws = _overdrawSkippedDraws;

        PendingOverdraw p;
        p.result = _report->overdraw.size();
        if (!CreateStaging((VkDeviceSize)pass.extent.width * pass.extent.height * 2, p.staging))
        {
            result.note = "no staging memory for the counts";
            _report->overdraw.push_back(std::move(result));
            continue;
        }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Barrier(cb, count.image, range, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {pass.extent.width, pass.extent.height, 1};
        _fns.CmdCopyImageToBuffer(cb, count.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.staging.buffer, 1, &copy);
        _report->overdraw.push_back(std::move(result));
        pending.push_back(p);
    }
}

void Replayer::CompleteOverdraw(std::vector<PendingOverdraw>& pending)
{
    for (PendingOverdraw& p : pending)
    {
        OverdrawResult& r = _report->overdraw[p.result];
        const size_t pixels = (size_t)r.width * r.height;
        const auto* bytes = static_cast<const uint8_t*>(p.staging.mapped);
        r.counts.resize(pixels);
        for (size_t i = 0; i < pixels; ++i)
        {
            const float value = HalfToFloat((uint16_t)(bytes[i * 2] | (bytes[i * 2 + 1] << 8)));
            const uint32_t n = std::isfinite(value) && value > 0 ? (uint32_t)std::min(65535L, std::lround(value)) : 0;
            r.counts[i] = (uint16_t)n;
            if (!n)
                continue;
            r.fragments += n;
            r.coveredPixels++;
            r.maxCount = std::max(r.maxCount, n);
            const int bucket = n <= 4 ? (int)n - 1 : n <= 8 ? 4
                : n <= 16                                   ? 5
                : n <= 32                                   ? 6
                                                            : 7;
            r.histogram[bucket]++;
        }
        DestroyStaging(p.staging);
    }
    pending.clear();
    ReleaseTransients();
}

} // namespace vkreplay
