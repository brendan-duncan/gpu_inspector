#include "replayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "format_info.h"
#include "util.h"

namespace vkreplay {

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

bool Replayer::ArgsResolve(const std::string& method, const JValue& args) {
    DecodeCheckFn fn = FindArgsDecoder(method);
    if (!fn) return true;
    const size_t unresolved = _ctx.unresolved;
    const size_t problems = _ctx.problems.size();
    fn(_ctx, args);
    const bool ok = _ctx.unresolved == unresolved;
    _ctx.unresolved = unresolved;
    _ctx.problems.resize(problems);
    _arena.Reset();
    return ok;
}

void Replayer::Barrier(VkCommandBuffer cb, VkImage image, const VkImageSubresourceRange& range, VkImageLayout from, VkImageLayout to) {
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

Replayer::TransientImage Replayer::CreateTransientImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkSampleCountFlagBits samples) {
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
    if (_fns.CreateImage(_device, &info, nullptr, &t.image) != VK_SUCCESS) return TransientImage{};
    VkMemoryRequirements req{};
    _fns.GetImageMemoryRequirements(_device, t.image, &req);
    if (!AllocateBound(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.memory, false) || _fns.BindImageMemory(_device, t.image, t.memory, 0) != VK_SUCCESS) {
        if (t.memory) _fns.FreeMemory(_device, t.memory, nullptr);
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

void Replayer::ReleaseTransients() {
    if (!_device) return;
    for (VkFramebuffer fb : _transientFramebuffers) _fns.DestroyFramebuffer(_device, fb, nullptr);
    for (TransientImage& t : _transientImages) {
        if (t.view) _fns.DestroyImageView(_device, t.view, nullptr);
        if (t.image) _fns.DestroyImage(_device, t.image, nullptr);
        if (t.memory) _fns.FreeMemory(_device, t.memory, nullptr);
    }
    _transientFramebuffers.clear();
    _transientImages.clear();
}

VkRenderPass Replayer::OverdrawRenderPass(VkFormat depthFormat) {
    auto it = _overdrawRenderPasses.find(depthFormat);
    if (it != _overdrawRenderPasses.end()) return it->second;
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
    if (hasDepth) subpass.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = hasDepth ? 2 : 1;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (_fns.CreateRenderPass(_device, &info, nullptr, &rp) == VK_SUCCESS) Track("VkRenderPass", (uint64_t)rp);
    _overdrawRenderPasses[depthFormat] = rp;
    return rp;
}

VkPipeline Replayer::OverdrawPipeline(uint64_t pipelineId, bool depthTested, VkFormat depthFormat, ReissueMode mode) {
    const auto key = std::make_tuple(pipelineId, depthTested, depthFormat, mode);
    auto it = _overdrawPipelines.find(key);
    if (it != _overdrawPipelines.end()) return it->second;
    _overdrawPipelines[key] = VK_NULL_HANDLE;  // a copy that cannot be made is not tried again
    const bool hasDepth = depthFormat != VK_FORMAT_UNDEFINED;
    VkPipeline pipeline = CopyGraphicsPipeline(pipelineId, mode == ReissueMode::Count ? "overdraw" : "overlay", [&](PipelineCopy& p) {
        if (p.hasRasterization && p.rasterization.rasterizerDiscardEnable) return false;  // no fragments to count
        if (mode == ReissueMode::Wireframe) {
            // The draw's edges, one pixel wide, whatever the application set.
            if (!p.hasRasterization) return false;
            p.rasterization.polygonMode = VK_POLYGON_MODE_LINE;
            p.rasterization.lineWidth = 1.0f;
            p.RemoveDynamic({VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_POLYGON_MODE_EXT});
        }
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
        if (!hasDepth) {
            p.hasDepthStencil = false;
        } else if (!depthTested || !p.hasDepthStencil) {
            p.depthStencil = VkPipelineDepthStencilStateCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            p.hasDepthStencil = true;
        }
        // Dynamic states that would undo the count, or the tests the untested count leaves out.
        p.RemoveDynamic(kColorOutputDynamicStates);
        p.RemoveDynamic(kMultisampleDynamicStates);
        if (!depthTested || !hasDepth) {
            p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                             VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE});
        }
        p.info.pNext = StripPNext(p.info.pNext, {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO});
        p.info.renderPass = OverdrawRenderPass(hasDepth ? depthFormat : VK_FORMAT_UNDEFINED);
        p.info.subpass = 0;
        return true;
    });
    _overdrawPipelines[key] = pipeline;
    return pipeline;
}

void Replayer::PrepareOverdraw(VkCommandBuffer cb, PassState& pass) {
    if (!pass.extent.width || !pass.extent.height) return;
    pass.overdraw = true;
    if (pass.depthFormat == VK_FORMAT_UNDEFINED || !pass.depthImage) return;
    auto sit = _images.find(pass.depthImage);
    pass.overdrawDepth = sit == _images.end() ? TransientImage{} :
        CreateTransientImage(pass.depthFormat, pass.extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!pass.overdrawDepth.image) {
        pass.depthFormat = VK_FORMAT_UNDEFINED;
        return;
    }
    const VkImageAspectFlags aspects = vkinsp::FormatAspects(pass.depthFormat);
    const VkImageSubresourceRange full{aspects, 0, 1, 0, 1};
    Barrier(cb, pass.overdrawDepth.image, full, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (pass.depthLoadOp != VK_ATTACHMENT_LOAD_OP_CLEAR && pass.depthLayoutBefore != VK_IMAGE_LAYOUT_UNDEFINED) {
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
    } else {
        _fns.CmdClearDepthStencilImage(cb, pass.overdrawDepth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &pass.depthClear, 1, &full);
    }
    Barrier(cb, pass.overdrawDepth.image, full, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

void Replayer::ReissueCommand(VkCommandBuffer cb, uint32_t index, bool depthTested, VkFormat depthFormat, bool insidePass) {
    const JValue& c = _capture->Commands()->items[index];
    const std::string m = Str(c.Get("method"));
    const JValue* args = c.Get("args");
    if (!args || (!insidePass && !IsStateCommand(m)) || kOverdrawSkipped.count(m)) return;
    if ((!depthTested || depthFormat == VK_FORMAT_UNDEFINED) && kDepthState.count(m)) return;
    const bool overlay = _overlayTarget != UINT32_MAX;
    if (overlay && _overlayIssued) return;
    if (m == "vkCmdBindPipeline") {
        if (Str(args->Get("pipelineBindPoint")) != "VK_PIPELINE_BIND_POINT_GRAPHICS") return;
        if (overlay) {
            // The draw the overlay is for gets its own copy when it comes; the rest draw depth only, or not at all.
            _overlayPipeline = IdOf(args->Get("pipeline"));
            if (_overlayOnlyTarget) return;
            VkPipeline pipeline = OverdrawPipeline(_overlayPipeline, depthTested, depthFormat, ReissueMode::DepthOnly);
            _overdrawDrawable = pipeline != VK_NULL_HANDLE;
            if (pipeline) _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            return;
        }
        VkPipeline pipeline = OverdrawPipeline(IdOf(args->Get("pipeline")), depthTested, depthFormat);
        _overdrawDrawable = pipeline != VK_NULL_HANDLE;
        if (pipeline) _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (_options.trace) {
            std::fprintf(stderr, "overdraw %u: bind pipeline %llu -> %s\n", index, (unsigned long long)IdOf(args->Get("pipeline")), pipeline ? "counting copy" : "none");
            std::fflush(stderr);
        }
        return;
    }
    const bool draw = StartsWith(m, "vkCmdDraw");
    const bool target = overlay && draw && index == _overlayTarget;
    if (target) {
        VkPipeline pipeline = _overlayPipeline ? OverdrawPipeline(_overlayPipeline, depthTested, depthFormat, _overlayTargetMode) : VK_NULL_HANDLE;
        _overdrawDrawable = pipeline != VK_NULL_HANDLE;
        if (pipeline) _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        _overlayIssued = true;  // drawn or not, nothing after it matters
    } else if (overlay && draw && _overlayOnlyTarget) {
        return;
    }
    if (draw && !_overdrawDrawable) {
        ++_overdrawSkippedDraws;
        return;
    }
    ReplayFn fn = FindReplayCommand(m);
    if (!fn) return;
    if (draw) ++_overdrawDraws;
    if (_options.trace) {
        std::fprintf(stderr, "overdraw %u: %s\n", index, m.c_str());
        std::fflush(stderr);
    }
    // Problems were reported when the pass itself was replayed.
    const size_t problems = _ctx.problems.size();
    const size_t unresolved = _ctx.unresolved;
    fn(_ctx, *args, cb);
    _ctx.problems.resize(problems);
    _ctx.unresolved = unresolved;
    _arena.Reset();
}

void Replayer::ReissuePass(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, bool depthTested,
                           VkFormat depthFormat, VkRenderPass renderPass, VkFramebuffer framebuffer) {
    const JValue* commands = _capture->Commands();
    // The state the pass inherited from the command buffer, then the pass's own commands.
    _overdrawDrawable = false;
    _overdrawDraws = 0;
    _overdrawSkippedDraws = 0;
    _overlayPipeline = 0;
    for (uint32_t i = group.first + 1; i < pass.beginIndex; ++i)
        if (!commands->items[i].Get("secondary")) ReissueCommand(cb, i, depthTested, depthFormat, false);
    VkClearValue clear{};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = renderPass;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, pass.extent};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    _fns.CmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    for (uint32_t i = pass.beginIndex + 1; i < endIndex; ++i) {
        const JValue& c = commands->items[i];
        if (c.Get("secondary")) continue;
        if (Str(c.Get("method")) == "vkCmdExecuteCommands") {
            // Secondary command buffers' commands are issued inline, in the order they executed.
            const JValue* list = c.Get("args") ? c.Get("args")->Get("pCommandBuffers") : nullptr;
            for (uint32_t s = 0; list && s < list->count; ++s) {
                const uint64_t id = IdOf(&list->items[s]);
                _overdrawDrawable = false;  // a secondary starts without a pipeline
                _overlayPipeline = 0;
                for (uint32_t j = i + 1; j < commands->count && commands->items[j].Get("secondary"); ++j)
                    if (commands->items[j].Get("secondary")->Uint() == id) ReissueCommand(cb, j, depthTested, depthFormat, true);
            }
            continue;
        }
        ReissueCommand(cb, i, depthTested, depthFormat, true);
    }
    _fns.CmdEndRenderPass(cb);
}

void Replayer::RecordOverdraw(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, std::vector<PendingOverdraw>& pending) {
    if (!pass.overdraw) return;
    int64_t measured = -1;
    if (const JValue* timings = _capture->Manifest().Get("passTimings"); timings && timings->IsArray()) {
        for (uint32_t t = 0; t < timings->count; ++t) {
            const JValue& p = timings->items[t];
            if (Str(p.Get("kind")) == "compute" || p.Get("commandBuffer")->Uint() != pass.commandBuffer ||
                p.Get("passIndex")->Uint() != pass.index || (p.Get("frame") ? p.Get("frame")->Uint() : 0) != pass.frame)
                continue;
            if (const JValue* counters = p.Get("counters"); counters && counters->Get("fragmentInvocations"))
                measured = (int64_t)counters->Get("fragmentInvocations")->Uint();
        }
    }
    for (int mode = 0; mode < 2; ++mode) {
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
        if (tested && depthFormat == VK_FORMAT_UNDEFINED) result.note = "the pass has no depth attachment";

        TransientImage count = CreateTransientImage(VK_FORMAT_R16_SFLOAT, pass.extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkRenderPass rp = OverdrawRenderPass(depthFormat);
        if (!count.image || !rp) {
            result.note = "no memory for the count target";
            _report->overdraw.push_back(std::move(result));
            continue;
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
        if (_fns.CreateFramebuffer(_device, &fbInfo, nullptr, &fb) != VK_SUCCESS) {
            result.note = "the count target's framebuffer could not be created";
            _report->overdraw.push_back(std::move(result));
            continue;
        }
        _transientFramebuffers.push_back(fb);

        ReissuePass(cb, group, pass, endIndex, tested, depthFormat, rp, fb);
        result.draws = _overdrawDraws;
        result.skippedDraws = _overdrawSkippedDraws;

        PendingOverdraw p;
        p.result = _report->overdraw.size();
        if (!CreateStaging((VkDeviceSize)pass.extent.width * pass.extent.height * 2, p.staging)) {
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

void Replayer::CompleteOverdraw(std::vector<PendingOverdraw>& pending) {
    for (PendingOverdraw& p : pending) {
        OverdrawResult& r = _report->overdraw[p.result];
        const size_t pixels = (size_t)r.width * r.height;
        const auto* bytes = static_cast<const uint8_t*>(p.staging.mapped);
        r.counts.resize(pixels);
        for (size_t i = 0; i < pixels; ++i) {
            const float value = HalfToFloat((uint16_t)(bytes[i * 2] | (bytes[i * 2 + 1] << 8)));
            const uint32_t n = std::isfinite(value) && value > 0 ? (uint32_t)std::min(65535L, std::lround(value)) : 0;
            r.counts[i] = (uint16_t)n;
            if (!n) continue;
            r.fragments += n;
            r.coveredPixels++;
            r.maxCount = std::max(r.maxCount, n);
            const int bucket = n <= 4 ? (int)n - 1 : n <= 8 ? 4 : n <= 16 ? 5 : n <= 32 ? 6 : 7;
            r.histogram[bucket]++;
        }
        DestroyStaging(p.staging);
    }
    pending.clear();
    ReleaseTransients();
}

} // namespace vkreplay
