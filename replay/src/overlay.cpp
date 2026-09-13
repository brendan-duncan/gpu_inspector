#include "replayer.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "format_info.h"
#include "util.h"

namespace vkreplay {

// ---------------------------------------------------------------------------------------------
// Draw-call overlays
//
// Where one draw of the frame landed, for the overlays of a render target: RenderDoc's highlight
// drawcall, depth test and wireframe overlays (vk_overlay.cpp). Like overdraw, it is recorded right
// after the replay has executed the draw's pass, in the same command buffer, so the buffers and
// descriptor sets the draw reads hold what they held for it. The pass's commands are issued again
// into a count target of the pass's size, up to and including the draw, up to three times:
//   * rasterized: the draw alone with the counting fragment shader and no depth or stencil tests,
//     every fragment it rasterized (and its own overdraw);
//   * passed: from a copy of the depth the pass started with, the pass's earlier draws moving the
//     depth and stencil without writing colour, then the draw with its own tests: the fragments
//     that passed them;
//   * wireframe: the draw alone with line polygons (the fillModeNonSolid feature).
// The three are folded into one byte per pixel (OverlayResult::mask).

bool Replayer::PassHoldsAny(uint32_t beginIndex, const std::vector<uint32_t>& wanted) const {
    if (wanted.empty()) return false;
    const JValue* commands = _capture->Commands();
    uint32_t end = beginIndex + 1;
    while (end < commands->count && (commands->items[end].Get("secondary") || !IsEndPass(Str(commands->items[end].Get("method"))))) ++end;
    return std::any_of(wanted.begin(), wanted.end(), [&](uint32_t c) { return c > beginIndex && c < end; });
}

void Replayer::RecordOverlay(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex) {
    const JValue* commands = _capture->Commands();
    for (uint32_t target : _options.overlay.commands) {
        if (target <= pass.beginIndex || target >= endIndex) continue;
        OverlayResult result;
        result.command = target;
        result.commandBuffer = pass.commandBuffer;
        result.frame = pass.frame;
        result.passIndex = pass.index;
        result.method = Str(commands->items[target].Get("method"));
        result.width = pass.extent.width;
        result.height = pass.extent.height;
        if (!StartsWith(result.method, "vkCmdDraw")) {
            result.note = "command " + std::to_string(target) + " is " + result.method + ", not a draw";
            _report->overlays.push_back(std::move(result));
            continue;
        }
        PendingOverlay p;
        p.result = _report->overlays.size();
        if (!DrawOverlayVariant(cb, group, pass, endIndex, target, ReissueMode::Count, false, p.rasterized)) {
            result.note = "the draw could not be drawn again (its pipeline could not be copied, or there was no memory for the overlay)";
            DestroyStaging(p.rasterized);
            _report->overlays.push_back(std::move(result));
            continue;
        }
        if (pass.depthFormat == VK_FORMAT_UNDEFINED || !pass.overdrawDepth.image) {
            result.note = "the pass has no depth attachment, so nothing the draw rasterized was rejected";
        } else {
            result.depthTested = DrawOverlayVariant(cb, group, pass, endIndex, target, ReissueMode::Count, true, p.passed);
            if (!result.depthTested) result.note = "the draw's depth and stencil tests could not be replayed";
        }
        if (_options.overlay.wireframe) {
            if (!_wireframeAvailable) result.note = "this GPU cannot draw wireframes (no fillModeNonSolid)";
            else result.wireframe = DrawOverlayVariant(cb, group, pass, endIndex, target, ReissueMode::Wireframe, false, p.wireframe);
        }
        _report->overlays.push_back(std::move(result));
        _pendingOverlays.push_back(p);
    }
}

bool Replayer::DrawOverlayVariant(VkCommandBuffer cb, const CommandGroup& group, const PassState& pass, uint32_t endIndex, uint32_t target,
                                  ReissueMode mode, bool depthTested, Staging& out) {
    const VkFormat depthFormat = depthTested ? pass.depthFormat : VK_FORMAT_UNDEFINED;
    TransientImage count = CreateTransientImage(VK_FORMAT_R16_SFLOAT, pass.extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkRenderPass rp = OverdrawRenderPass(depthFormat);
    if (!count.image || !rp) return false;

    TransientImage depth;
    if (depthTested) {
        // Every depth-tested variant starts from the depth the pass began with, which the pass's own copy keeps.
        depth = CreateTransientImage(pass.depthFormat, pass.extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!depth.image) return false;
        const VkImageAspectFlags aspects = vkinsp::FormatAspects(pass.depthFormat);
        const VkImageSubresourceRange full{aspects, 0, 1, 0, 1};
        Barrier(cb, pass.overdrawDepth.image, full, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Barrier(cb, depth.image, full, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {aspects, 0, 0, 1};
        copy.dstSubresource = {aspects, 0, 0, 1};
        copy.extent = {pass.extent.width, pass.extent.height, 1};
        _fns.CmdCopyImage(cb, pass.overdrawDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(cb, pass.overdrawDepth.image, full, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        Barrier(cb, depth.image, full, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    VkImageView views[2] = {count.view, depth.view};
    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = rp;
    fbInfo.attachmentCount = depthTested ? 2 : 1;
    fbInfo.pAttachments = views;
    fbInfo.width = pass.extent.width;
    fbInfo.height = pass.extent.height;
    fbInfo.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (_fns.CreateFramebuffer(_device, &fbInfo, nullptr, &fb) != VK_SUCCESS) return false;
    _transientFramebuffers.push_back(fb);

    _overlayTarget = target;
    _overlayOnlyTarget = !depthTested;  // without the tests, the other draws change nothing
    _overlayTargetMode = mode;
    _overlayIssued = false;
    ReissuePass(cb, group, pass, endIndex, depthTested, depthFormat, rp, fb);
    // Nothing is issued after the target draw, so what was bound for it still says whether it drew.
    const bool drawn = _overlayIssued && _overdrawDrawable;
    _overlayTarget = UINT32_MAX;
    _overlayOnlyTarget = false;
    if (!drawn || !CreateStaging((VkDeviceSize)pass.extent.width * pass.extent.height * 2, out)) return false;

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Barrier(cb, count.image, range, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {pass.extent.width, pass.extent.height, 1};
    _fns.CmdCopyImageToBuffer(cb, count.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer, 1, &copy);
    return true;
}

void Replayer::CompleteOverlay(bool submitted) {
    for (PendingOverlay& p : _pendingOverlays) {
        OverlayResult& r = _report->overlays[p.result];
        const size_t pixels = (size_t)r.width * r.height;
        if (!submitted) {
            r.note = "the submission holding the draw did not run";
        } else {
            r.mask.assign(pixels, 0);
            // Folds one variant's counts into a bit of the mask; the rasterized variant also gives the fragments.
            auto fold = [&](const Staging& s, uint8_t bit, bool fragments) {
                if (!s.mapped) return;
                const auto* bytes = static_cast<const uint8_t*>(s.mapped);
                for (size_t i = 0; i < pixels; ++i) {
                    const float value = HalfToFloat((uint16_t)(bytes[i * 2] | (bytes[i * 2 + 1] << 8)));
                    if (!std::isfinite(value) || value <= 0) continue;
                    r.mask[i] |= bit;
                    if (fragments) r.fragments += (uint64_t)std::lround(value);
                }
            };
            fold(p.rasterized, 1, true);
            fold(p.passed, 2, false);
            fold(p.wireframe, 4, false);
            for (uint8_t& m : r.mask) {
                if (!r.depthTested && (m & 1)) m |= 2;  // no tests, nothing rejected
                if (!(m & 1)) continue;
                ++r.pixelsCovered;
                if (m & 2) ++r.pixelsPassed;
                else ++r.pixelsRejected;
            }
        }
        DestroyStaging(p.rasterized);
        DestroyStaging(p.passed);
        DestroyStaging(p.wireframe);
    }
    _pendingOverlays.clear();
    ReleaseTransients();
}

} // namespace vkreplay
