// Pixel history: every event of the frame that touched one pixel of one image, and what each draw's
// fragments at that pixel met (after RenderDoc's vk_pixelhistory.cpp, simplified).
//
// Each pass that renders to the image is followed in two steps.
//   * Before the replay executes the pass, every attachment's state at the pass's start is copied
//     into an image of the replay's own (its contents when it loads, its clear value when it
//     clears), and the pixel is read from it: the "load" event.
//   * After the pass, its commands are issued again, one event at a time, into those copies,
//     inside a pass that loads what the last event left:
//       - a draw first runs with six copies of its pipeline, each writing nothing, under an
//         occlusion query and a one-pixel scissor: covered (no culling, no tests), facing (the
//         pipeline's culling), shaded (its fragment shader, which may discard), depth only,
//         stencil only, and every test; then with its own pipeline, blending and depth writes;
//       - vkCmdClearAttachments runs as it is;
//       - bindings and dynamic state are issued between passes, where they stay in effect.
//     After each event the pixel, and the pass's depth at it, are read.
// A pass's state carries across the replay's begin and end of its own passes: they are
// compatible with the pass (the same attachments and subpasses, loading instead of clearing).
//
// Not followed yet: writes outside render passes (clears, copies and blits into the image),
// multisampled targets, layered passes past their first layer, and early fragment tests (a
// shader that discards after the depth test is reported as discarding).
#include "replayer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "format_info.h"
#include "util.h"

namespace vkreplay {

namespace {

enum HistoryVariant { kCovered = 0, kFacing, kShaded, kDepthOnly, kStencilOnly, kAllTests, kVariantCount };

bool IsDepthFormat(VkFormat f) {
    return (vkinsp::FormatAspects(f) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
}

/** The layout an attachment copy stays in between the history's passes. */
VkImageLayout AttachmentLayout(VkFormat f) {
    return IsDepthFormat(f) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

VkStencilOpState KeepOps(VkStencilOpState s) {
    s.failOp = s.passOp = s.depthFailOp = VK_STENCIL_OP_KEEP;
    return s;
}

VkRect2D RectOf(const JValue* v) {
    VkRect2D r{};
    if (!v) return r;
    if (const JValue* o = v->Get("offset")) {
        r.offset.x = (int32_t)(o->Get("x") ? o->Get("x")->Int() : 0);
        r.offset.y = (int32_t)(o->Get("y") ? o->Get("y")->Int() : 0);
    }
    if (const JValue* e = v->Get("extent")) {
        r.extent.width = (uint32_t)(e->Get("width") ? e->Get("width")->Uint() : 0);
        r.extent.height = (uint32_t)(e->Get("height") ? e->Get("height")->Uint() : 0);
    }
    return r;
}

} // namespace

VkPipeline Replayer::HistoryPipeline(uint64_t pipelineId, int variant) {
    const auto key = std::make_pair(pipelineId, variant);
    auto it = _historyPipelines.find(key);
    if (it != _historyPipelines.end()) return it->second;
    _historyPipelines[key] = VK_NULL_HANDLE;
    VkPipeline pipeline = CopyGraphicsPipeline(pipelineId, "pixel history", [&](PipelineCopy& p) {
        if (p.hasRasterization && p.rasterization.rasterizerDiscardEnable) return false;
        if (variant == kCovered || variant == kFacing) p.ReplaceFragment(CountModule());
        if (variant == kCovered && p.hasRasterization) p.rasterization.cullMode = VK_CULL_MODE_NONE;
        // The queries only count: nothing is written.
        for (VkPipelineColorBlendAttachmentState& a : p.blendAttachments) {
            a.blendEnable = VK_FALSE;
            a.colorWriteMask = 0;
        }
        p.blend.logicOpEnable = VK_FALSE;
        const bool depth = variant == kDepthOnly || variant == kAllTests;
        const bool stencil = variant == kStencilOnly || variant == kAllTests;
        if (p.hasDepthStencil) {
            p.depthStencil.depthWriteEnable = VK_FALSE;
            p.depthStencil.front = KeepOps(p.depthStencil.front);
            p.depthStencil.back = KeepOps(p.depthStencil.back);
            if (!depth) {
                p.depthStencil.depthTestEnable = VK_FALSE;
                p.depthStencil.depthBoundsTestEnable = VK_FALSE;
            }
            if (!stencil) p.depthStencil.stencilTestEnable = VK_FALSE;
        }
        p.RemoveDynamic(kColorOutputDynamicStates);
        p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP});
        if (!depth) p.RemoveDynamic({VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE});
        if (!stencil) p.RemoveDynamic({VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE});
        if (variant == kCovered) p.RemoveDynamic({VK_DYNAMIC_STATE_CULL_MODE});
        // The one-pixel scissor.
        if (!p.HasDynamic(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT)) p.AddDynamic(VK_DYNAMIC_STATE_SCISSOR);
        return true;
    });
    _historyPipelines[key] = pipeline;
    return pipeline;
}

VkRenderPass Replayer::HistoryRenderPass(uint64_t renderPassId) {
    auto it = _historyRenderPasses.find(renderPassId);
    if (it != _historyRenderPasses.end()) return it->second;
    _historyRenderPasses[renderPassId] = VK_NULL_HANDLE;
    const JValue* object = _capture->Object(renderPassId);
    const JValue* args = object ? object->Get("args") : nullptr;
    if (!args) return VK_NULL_HANDLE;
    const std::string cmd = Str(object->Get("cmd"));
    const size_t problems = _ctx.problems.size();
    const size_t unresolved = _ctx.unresolved;
    VkRenderPass rp = VK_NULL_HANDLE;
    // Every attachment loads and stores, and stays in its attachment layout between passes.
    if (cmd == "vkCreateRenderPass") {
        Args_vkCreateRenderPass a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo) {
            VkRenderPassCreateInfo info = *a.pCreateInfo;
            std::vector<VkAttachmentDescription> attachments(info.pAttachments, info.pAttachments + info.attachmentCount);
            for (VkAttachmentDescription& att : attachments) {
                att.loadOp = att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                att.storeOp = att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.initialLayout = att.finalLayout = AttachmentLayout(att.format);
            }
            info.pAttachments = attachments.data();
            _fns.CreateRenderPass(_device, &info, nullptr, &rp);
        }
    } else if (cmd == "vkCreateRenderPass2" || cmd == "vkCreateRenderPass2KHR") {
        Args_vkCreateRenderPass2 a{};
        DecodeArgs(_ctx, *args, a);
        if (a.pCreateInfo) {
            VkRenderPassCreateInfo2 info = *a.pCreateInfo;
            std::vector<VkAttachmentDescription2> attachments(info.pAttachments, info.pAttachments + info.attachmentCount);
            for (VkAttachmentDescription2& att : attachments) {
                att.loadOp = att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                att.storeOp = att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.initialLayout = att.finalLayout = AttachmentLayout(att.format);
            }
            info.pAttachments = attachments.data();
            _fns.CreateRenderPass2(_device, &info, nullptr, &rp);
        }
    }
    _ctx.problems.resize(problems);
    _ctx.unresolved = unresolved;
    _arena.Reset();
    if (rp) Track("VkRenderPass", (uint64_t)rp);
    _historyRenderPasses[renderPassId] = rp;
    return rp;
}

Replayer::ScissorInfo Replayer::PipelineScissor(uint64_t pipelineId) {
    auto it = _pipelineScissors.find(pipelineId);
    if (it != _pipelineScissors.end()) return it->second;
    ScissorInfo info;
    const JValue* object = _capture->Object(pipelineId);
    const JValue* args = object ? object->Get("args") : nullptr;
    const JValue* infos = args ? args->Get("pCreateInfos") : nullptr;
    const uint32_t index = object && object->Get("index") ? (uint32_t)object->Get("index")->Uint() : 0;
    if (infos && infos->IsArray() && index < infos->count) {
        const JValue& ci = infos->items[index];
        info.dynamic = false;
        if (const JValue* d = ci.Get("pDynamicState"); d && d->Get("pDynamicStates")) {
            const JValue* states = d->Get("pDynamicStates");
            for (uint32_t k = 0; k < states->count; ++k) {
                const std::string_view s = states->items[k].Str();
                if (s == "VK_DYNAMIC_STATE_SCISSOR") info.dynamic = true;
                if (s == "VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT") info.dynamic = info.withCount = true;
            }
        }
        if (!info.dynamic) {
            if (const JValue* vs = ci.Get("pViewportState"); vs && vs->Get("pScissors") && vs->Get("pScissors")->IsArray() && vs->Get("pScissors")->count) {
                info.rect = RectOf(&vs->Get("pScissors")->items[0]);
                info.hasRect = true;
            }
        }
    }
    _pipelineScissors[pipelineId] = info;
    return info;
}

uint32_t Replayer::CopyHistoryPixel(VkCommandBuffer cb, const PassState& pass, PendingHistory& pending, VkImageLayout layout) {
    const uint32_t slot = pending.nextSlot++;
    const VkDeviceSize base = (VkDeviceSize)slot * (pending.targetTexel + pending.depthTexel);
    auto copy = [&](int attachment, VkDeviceSize offset) {
        const VkFormat format = pass.formats[attachment];
        const VkImage image = pass.shadows[attachment].image;
        const VkImageSubresourceRange range{vkinsp::FormatAspects(format), 0, 1, 0, 1};
        const VkImageLayout current = layout != VK_IMAGE_LAYOUT_UNDEFINED ? layout : AttachmentLayout(format);
        Barrier(cb, image, range, current, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = offset;
        c.imageSubresource = {IsDepthFormat(format) ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {(int32_t)_options.history.x, (int32_t)_options.history.y, 0};
        c.imageExtent = {1, 1, 1};
        _fns.CmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.staging.buffer, 1, &c);
        Barrier(cb, image, range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current);
    };
    copy(pass.historyAttachment, base);
    if (pending.depthTexel) copy(pass.historyDepthAttachment, base + pending.targetTexel);
    return slot;
}

void Replayer::PrepareHistory(VkCommandBuffer cb, PassState& pass, std::vector<PendingHistory>& histories) {
    const auto& req = _options.history;
    PixelHistoryResult& out = _report->history;
    int target = -1;
    for (size_t a = 0; a < pass.views.size() && target < 0; ++a) {
        auto vit = _views.find(pass.views[a]);
        if (vit == _views.end()) continue;
        const ViewRecord& v = vit->second;
        const uint32_t layers = v.range.layerCount == VK_REMAINING_ARRAY_LAYERS ? UINT32_MAX : std::max(1u, v.range.layerCount);
        if (v.image == req.image && v.range.baseMipLevel == req.mip && req.layer >= v.range.baseArrayLayer && req.layer - v.range.baseArrayLayer < layers)
            target = (int)a;
    }
    if (target < 0) return;
    ++_historyPasses;
    const std::string where = "command buffer " + std::to_string(pass.commandBuffer) + ", pass " + std::to_string(pass.index);
    auto note = [&](const std::string& why) { out.notes.push_back(where + ": " + why); };
    const ViewRecord& targetView = _views.find(pass.views[target])->second;
    if (req.layer != targetView.range.baseArrayLayer) return note("only the first layer of a layered pass is followed");
    if (!pass.extent.width || req.x >= pass.extent.width || req.y >= pass.extent.height) return note("the pixel is outside the pass's framebuffer");
    if (pass.formats.size() != pass.views.size()) return note("the pass's attachment formats are not known");

    // Copies of every attachment, holding what the pass starts from.
    pass.shadows.assign(pass.views.size(), TransientImage{});
    int depthAttachment = -1;
    for (size_t a = 0; a < pass.views.size(); ++a) {
        auto vit = _views.find(pass.views[a]);
        auto iit = vit != _views.end() ? _images.find(vit->second.image) : _images.end();
        if (iit == _images.end()) return note("an attachment of the pass was not replayed");
        const ImageRecord& image = iit->second;
        const ViewRecord& view = vit->second;
        const VkFormat format = pass.formats[a];
        const bool depth = IsDepthFormat(format);
        if ((int)a == target && image.samples != VK_SAMPLE_COUNT_1_BIT) return note("multisampled targets are not followed yet");
        if (depth && depthAttachment < 0 && image.samples == VK_SAMPLE_COUNT_1_BIT) depthAttachment = (int)a;
        const VkImageUsageFlags usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        pass.shadows[a] = CreateTransientImage(format, pass.extent, usage, image.samples);
        if (!pass.shadows[a].image) return note("no memory for copies of the pass's attachments");
        const VkImageAspectFlags aspects = vkinsp::FormatAspects(format);
        const VkImageSubresourceRange full{aspects, 0, 1, 0, 1};
        Barrier(cb, pass.shadows[a].image, full, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkAttachmentLoadOp load = a < pass.loadOps.size() ? pass.loadOps[a] : VK_ATTACHMENT_LOAD_OP_LOAD;
        const VkImageLayout before = a < pass.startLayouts.size() ? pass.startLayouts[a] : VK_IMAGE_LAYOUT_UNDEFINED;
        if (load != VK_ATTACHMENT_LOAD_OP_CLEAR && before != VK_IMAGE_LAYOUT_UNDEFINED) {
            const VkImageSubresourceRange srcRange{aspects, view.range.baseMipLevel, 1, view.range.baseArrayLayer, 1};
            Barrier(cb, image.image, srcRange, before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageCopy copy{};
            copy.srcSubresource = {aspects, view.range.baseMipLevel, view.range.baseArrayLayer, 1};
            copy.dstSubresource = {aspects, 0, 0, 1};
            copy.extent = {std::min(pass.extent.width, std::max(1u, image.extent.width >> view.range.baseMipLevel)),
                           std::min(pass.extent.height, std::max(1u, image.extent.height >> view.range.baseMipLevel)), 1};
            _fns.CmdCopyImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pass.shadows[a].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(cb, image.image, srcRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, before);
        } else {
            const VkClearValue clear = load == VK_ATTACHMENT_LOAD_OP_CLEAR && a < pass.clearValues.size() ? pass.clearValues[a] : VkClearValue{};
            if (depth) _fns.CmdClearDepthStencilImage(cb, pass.shadows[a].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear.depthStencil, 1, &full);
            else _fns.CmdClearColorImage(cb, pass.shadows[a].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear.color, 1, &full);
        }
    }

    PendingHistory pending;
    const VkFormat targetFormat = pass.formats[target];
    pending.targetTexel = vkinsp::FormatBlockInfo(targetFormat, IsDepthFormat(targetFormat) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT).bytes;
    if (!pending.targetTexel) return note("the image's format cannot be read back");
    if (depthAttachment >= 0) pending.depthTexel = vkinsp::FormatBlockInfo(pass.formats[depthAttachment], VK_IMAGE_ASPECT_DEPTH_BIT).bytes;
    pass.historyAttachment = target;
    pass.historyDepthAttachment = pending.depthTexel ? depthAttachment : -1;
    if (out.format.empty()) out.format = EnumName(kEnum_VkFormat, kEnumCount_VkFormat, targetFormat);
    if (out.depthFormat.empty() && pending.depthTexel) out.depthFormat = EnumName(kEnum_VkFormat, kEnumCount_VkFormat, pass.formats[depthAttachment]);

    // Room for the pass's events: its start, and every draw and clear up to its end.
    const JValue* commands = _capture->Commands();
    uint32_t draws = 0;
    uint32_t clears = 0;
    for (uint32_t i = pass.beginIndex + 1; i < commands->count; ++i) {
        const JValue& c = commands->items[i];
        const std::string m = Str(c.Get("method"));
        if (!c.Get("secondary") && IsEndPass(m)) break;
        if (StartsWith(m, "vkCmdDraw")) ++draws;
        else if (m == "vkCmdClearAttachments") ++clears;
    }
    if (!CreateStaging((VkDeviceSize)(draws + clears + 1) * (pending.targetTexel + pending.depthTexel), pending.staging))
        return note("no staging memory for the pixel's values");
    if (draws) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = draws * kVariantCount;
        if (_fns.CreateQueryPool(_device, &qi, nullptr, &pending.queries) == VK_SUCCESS) {
            pending.queryCount = qi.queryCount;
            _fns.CmdResetQueryPool(cb, pending.queries, 0, pending.queryCount);
        }
    }

    PixelEvent start;
    start.kind = "load";
    start.command = pass.beginIndex;
    start.method = Str(commands->items[pass.beginIndex].Get("method"));
    start.detail = EnumName(kEnum_VkAttachmentLoadOp, kEnumCount_VkAttachmentLoadOp, target < (int)pass.loadOps.size() ? pass.loadOps[target] : VK_ATTACHMENT_LOAD_OP_LOAD);
    start.commandBuffer = pass.commandBuffer;
    start.frame = pass.frame;
    start.passIndex = pass.index;
    PendingHistory::Entry entry;
    entry.event = out.events.size();
    entry.slot = CopyHistoryPixel(cb, pass, pending, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    pending.entries.push_back(entry);
    out.events.push_back(std::move(start));
    for (size_t a = 0; a < pass.shadows.size(); ++a) {
        Barrier(cb, pass.shadows[a].image, {vkinsp::FormatAspects(pass.formats[a]), 0, 1, 0, 1}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                AttachmentLayout(pass.formats[a]));
    }
    pass.history = true;
    pass.historyPending = histories.size();
    histories.push_back(std::move(pending));
}

void Replayer::RecordHistory(VkCommandBuffer cb, const CommandGroup& group, PassState& pass, uint32_t endIndex, std::vector<PendingHistory>& histories) {
    if (!pass.history) return;
    pass.history = false;
    PendingHistory& pending = histories[pass.historyPending];
    PixelHistoryResult& out = _report->history;
    const JValue* commands = _capture->Commands();
    const bool dynamic = pass.renderPass == 0;
    const std::string where = "command buffer " + std::to_string(pass.commandBuffer) + ", pass " + std::to_string(pass.index);

    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (!dynamic) {
        rp = HistoryRenderPass(pass.renderPass);
        if (rp) {
            std::vector<VkImageView> views;
            for (const TransientImage& s : pass.shadows) views.push_back(s.view);
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = rp;
            info.attachmentCount = (uint32_t)views.size();
            info.pAttachments = views.data();
            info.width = pass.extent.width;
            info.height = pass.extent.height;
            info.layers = 1;
            if (_fns.CreateFramebuffer(_device, &info, nullptr, &fb) == VK_SUCCESS) _transientFramebuffers.push_back(fb);
        }
        if (!rp || !fb) {
            out.notes.push_back(where + ": the pass could not be rebuilt to follow the pixel through it");
            return;
        }
    }

    uint32_t subpass = 0;
    uint64_t pipeline = 0;
    bool scissorSet = false;
    VkRect2D scissor{};
    std::vector<uint32_t> dynamicCommands;

    auto beginPass = [&]() {
        if (!dynamic) {
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = rp;
            begin.framebuffer = fb;
            begin.renderArea = {{0, 0}, pass.extent};
            _fns.CmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
            for (uint32_t s = 0; s < subpass; ++s) _fns.CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
            return;
        }
        auto attachment = [&](int index, VkImageLayout layout) {
            VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            if (index >= 0) {
                a.imageView = pass.shadows[index].view;
                a.imageLayout = layout;
                a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            return a;
        };
        std::vector<VkRenderingAttachmentInfo> colors;
        for (int slot : pass.dynamicColorSlots) colors.push_back(attachment(slot, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
        VkRenderingAttachmentInfo depth = attachment(pass.dynamicDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo stencil = attachment(pass.dynamicStencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, pass.extent};
        info.layerCount = 1;
        info.colorAttachmentCount = (uint32_t)colors.size();
        info.pColorAttachments = colors.data();
        if (pass.dynamicDepth >= 0) info.pDepthAttachment = &depth;
        if (pass.dynamicStencil >= 0) info.pStencilAttachment = &stencil;
        _fns.CmdBeginRendering(cb, &info);
    };
    auto endPass = [&]() {
        if (!dynamic) _fns.CmdEndRenderPass(cb);
        else _fns.CmdEndRendering(cb);
    };
    // A command recorded as it was captured; its problems were reported when the pass was replayed.
    auto issue = [&](uint32_t index) {
        const JValue& c = commands->items[index];
        const JValue* args = c.Get("args");
        ReplayFn fn = FindReplayCommand(Str(c.Get("method")));
        if (!fn || !args) return;
        const size_t problems = _ctx.problems.size();
        const size_t unresolved = _ctx.unresolved;
        fn(_ctx, *args, cb);
        _ctx.problems.resize(problems);
        _ctx.unresolved = unresolved;
        _arena.Reset();
    };
    // Bindings and dynamic state, issued between passes where they stay in effect.
    auto state = [&](uint32_t index) {
        const JValue& c = commands->items[index];
        const std::string m = Str(c.Get("method"));
        const JValue* args = c.Get("args");
        if (!args || !IsStateCommand(m)) return;
        if (m == "vkCmdBindPipeline") {
            if (Str(args->Get("pipelineBindPoint")) == "VK_PIPELINE_BIND_POINT_GRAPHICS") pipeline = IdOf(args->Get("pipeline"));
        } else if (StartsWith(m, "vkCmdSet")) {
            dynamicCommands.push_back(index);
            if (StartsWith(m, "vkCmdSetScissor")) {
                if (const JValue* list = args->Get("pScissors"); list && list->IsArray() && list->count) {
                    scissor = RectOf(&list->items[0]);
                    scissorSet = true;
                }
            }
        }
        issue(index);
    };
    auto event = [&](uint32_t index) {
        const JValue& c = commands->items[index];
        const std::string m = Str(c.Get("method"));
        if (StartsWith(m, "vkCmdNextSubpass")) {
            ++subpass;
            return;
        }
        const bool draw = StartsWith(m, "vkCmdDraw");
        if (!draw && m != "vkCmdClearAttachments") {
            state(index);
            return;
        }
        PixelEvent e;
        e.kind = draw ? "draw" : "clear";
        e.command = index;
        e.method = m;
        e.commandBuffer = pass.commandBuffer;
        e.frame = pass.frame;
        e.passIndex = pass.index;
        PendingHistory::Entry entry;
        entry.event = out.events.size();
        beginPass();
        if (draw) {
            e.pipeline = pipeline;
            const ScissorInfo pipelineScissor = pipeline ? PipelineScissor(pipeline) : ScissorInfo{};
            VkRect2D rect = scissorSet ? scissor : VkRect2D{{0, 0}, pass.extent};
            if (!pipelineScissor.dynamic && pipelineScissor.hasRect) rect = pipelineScissor.rect;
            const int64_t x = _options.history.x;
            const int64_t y = _options.history.y;
            const bool inside = x >= rect.offset.x && x < (int64_t)rect.offset.x + rect.extent.width && y >= rect.offset.y &&
                                y < (int64_t)rect.offset.y + rect.extent.height;
            e.scissored = !inside;
            if (inside && pipeline && pending.queries && pending.nextQuery + kVariantCount <= pending.queryCount) {
                entry.queryBase = (int32_t)pending.nextQuery;
                pending.nextQuery += kVariantCount;
                const VkRect2D pixel{{(int32_t)x, (int32_t)y}, {1, 1}};
                for (int v = 0; v < kVariantCount; ++v) {
                    VkPipeline copy = HistoryPipeline(pipeline, v);
                    if (!copy) continue;
                    // Binding a pipeline with a state static undoes that state's dynamic value: set again.
                    _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, copy);
                    for (uint32_t d : dynamicCommands) issue(d);
                    if (pipelineScissor.withCount) _fns.CmdSetScissorWithCount(cb, 1, &pixel);
                    else _fns.CmdSetScissor(cb, 0, 1, &pixel);
                    _fns.CmdBeginQuery(cb, pending.queries, (uint32_t)entry.queryBase + v, 0);
                    issue(index);
                    _fns.CmdEndQuery(cb, pending.queries, (uint32_t)entry.queryBase + v);
                    entry.issued |= 1u << v;
                }
                // The draw's own pipeline and dynamic state again.
                _fns.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)Handle(pipeline));
                for (uint32_t d : dynamicCommands) issue(d);
            }
        }
        issue(index);
        endPass();
        entry.slot = CopyHistoryPixel(cb, pass, pending, VK_IMAGE_LAYOUT_UNDEFINED);
        pending.entries.push_back(entry);
        out.events.push_back(std::move(e));
    };

    // The state the pass inherited from its command buffer, then its own commands (secondary
    // command buffers' inline, where they executed).
    for (uint32_t i = group.first + 1; i < pass.beginIndex; ++i)
        if (!commands->items[i].Get("secondary")) state(i);
    for (uint32_t i = pass.beginIndex + 1; i < endIndex; ++i) {
        const JValue& c = commands->items[i];
        if (c.Get("secondary")) continue;
        if (Str(c.Get("method")) == "vkCmdExecuteCommands") {
            const JValue* list = c.Get("args") ? c.Get("args")->Get("pCommandBuffers") : nullptr;
            for (uint32_t s = 0; list && s < list->count; ++s) {
                const uint64_t id = IdOf(&list->items[s]);
                for (uint32_t j = i + 1; j < commands->count && commands->items[j].Get("secondary"); ++j)
                    if (commands->items[j].Get("secondary")->Uint() == id) event(j);
            }
            continue;
        }
        event(i);
    }
}

void Replayer::CompleteHistory(std::vector<PendingHistory>& histories) {
    PixelHistoryResult& out = _report->history;
    for (PendingHistory& h : histories) {
        const auto* bytes = static_cast<const uint8_t*>(h.staging.mapped);
        const uint32_t slotSize = h.targetTexel + h.depthTexel;
        for (const PendingHistory::Entry& entry : h.entries) {
            PixelEvent& e = out.events[entry.event];
            if (bytes && (VkDeviceSize)(entry.slot + 1) * slotSize <= h.staging.size) {
                const uint8_t* at = bytes + (size_t)entry.slot * slotSize;
                e.value.assign(at, at + h.targetTexel);
                if (h.depthTexel) e.depth.assign(at + h.targetTexel, at + slotSize);
            }
            if (entry.queryBase < 0 || !entry.issued) continue;
            uint64_t results[kVariantCount] = {};
            for (int v = 0; v < kVariantCount; ++v) {
                if (!(entry.issued & (1u << v))) continue;
                _fns.GetQueryPoolResults(_device, h.queries, (uint32_t)entry.queryBase + v, 1, sizeof(uint64_t), &results[v], sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            }
            e.testsMeasured = entry.issued;
            e.covered = results[kCovered];
            e.facing = results[kFacing];
            e.shaded = results[kShaded];
            e.depthPassed = results[kDepthOnly];
            e.stencilPassed = results[kStencilOnly];
            e.passed = results[kAllTests];
        }
        DestroyStaging(h.staging);
        if (h.queries) _fns.DestroyQueryPool(_device, h.queries, nullptr);
    }
    histories.clear();
}

} // namespace vkreplay
