// Post-call and pre-call hooks: shader code retention, memory bindings, the resource registry,
// and the frame-capture entry points.
#include "hooks.h"

#include "capture.h"
#include "device_lost.h"
#include "shader_statistics.h"
#include "descriptors.h"
#include "descriptor_buffer.h"
#include "format_info.h"
#include "image_readback.h"
#include "pipeline_stats.h"
#include "refresh_rate.h"
#include "shader_edit.h"
#include "layer.h"
#include "hud.h"
#include "resources.h"
#include "tracker.h"
#include "transport.h"
#include "vk_serialize.gen.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace vkinsp {

// =============================================================================================
// Pre-call hooks: add TRANSFER_SRC so render targets and buffers can be read back.

void PreHook_vkCreateImage(VkDevice& device, const VkImageCreateInfo*& pCreateInfo,
                           const VkAllocationCallbacks*& pAllocator, VkImage*& pImage) {
    if (!pCreateInfo) return;
    thread_local VkImageCreateInfo copy;
    copy = *pCreateInfo;
    copy.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    pCreateInfo = &copy;
}

void PreHook_vkCreateBuffer(VkDevice& device, const VkBufferCreateInfo*& pCreateInfo,
                            const VkAllocationCallbacks*& pAllocator, VkBuffer*& pBuffer) {
    if (!pCreateInfo) return;
    thread_local VkBufferCreateInfo copy;
    copy = *pCreateInfo;
    copy.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    pCreateInfo = &copy;
}

// Compiler statistics: the driver keeps what it made of a stage only when the pipeline asked it to
// (shader_statistics.h). Adding the flag here means every pipeline carries them, including ones
// created before a capture starts.
void PreHook_vkCreateGraphicsPipelines(VkDevice& device, VkPipelineCache& pipelineCache, uint32_t& createInfoCount,
                                       const VkGraphicsPipelineCreateInfo*& pCreateInfos,
                                       const VkAllocationCallbacks*& pAllocator, VkPipeline*& pPipelines) {
    pCreateInfos = CaptureStatisticsFlags(GetDeviceData(device), createInfoCount, pCreateInfos);
}

void PreHook_vkCreateComputePipelines(VkDevice& device, VkPipelineCache& pipelineCache, uint32_t& createInfoCount,
                                      const VkComputePipelineCreateInfo*& pCreateInfos,
                                      const VkAllocationCallbacks*& pAllocator, VkPipeline*& pPipelines) {
    pCreateInfos = CaptureStatisticsFlags(GetDeviceData(device), createInfoCount, pCreateInfos);
}

void PreHook_vkCreateRayTracingPipelinesKHR(VkDevice& device, VkDeferredOperationKHR& deferredOperation,
                                            VkPipelineCache& pipelineCache, uint32_t& createInfoCount,
                                            const VkRayTracingPipelineCreateInfoKHR*& pCreateInfos,
                                            const VkAllocationCallbacks*& pAllocator, VkPipeline*& pPipelines) {
    pCreateInfos = CaptureStatisticsFlags(GetDeviceData(device), createInfoCount, pCreateInfos);
}

void PreHook_vkCreateSwapchainKHR(VkDevice& device, const VkSwapchainCreateInfoKHR*& pCreateInfo,
                                  const VkAllocationCallbacks*& pAllocator, VkSwapchainKHR*& pSwapchain) {
    if (!pCreateInfo) return;
    DeviceData* dev = GetDeviceData(device);
    VkSurfaceCapabilitiesKHR caps{};
    thread_local VkSwapchainCreateInfoKHR copy;
    copy = *pCreateInfo;
    bool changed = false;
    if (dev && dev->instance->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR &&
        dev->instance->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physicalDevice, pCreateInfo->surface, &caps) == VK_SUCCESS &&
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        changed = true;
    }
    // Present timing (the display refresh period) needs the flag on the swapchain.
    if (dev && dev->presentTiming && SurfaceSupportsPresentTiming(dev, pCreateInfo->surface)) {
        copy.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
        changed = true;
    }
    if (changed) pCreateInfo = &copy;
}

// Pass counters across secondary command buffers: a pass's pipeline statistics and occlusion
// queries are active while it runs, and vkCmdExecuteCommands inside it is only valid when each
// secondary was begun inheriting queries of those kinds. With inheritedQueries on the device, every
// secondary is begun so (the application's inheritance info plus the layer's query kinds); the
// record keeps what the application passed. pInheritanceInfo is only read for a secondary, since a
// primary's may be anything.
static thread_local VkCommandBufferBeginInfo t_inheritingBegin;
static thread_local VkCommandBufferInheritanceInfo t_inheritance;

static const VkCommandBufferBeginInfo* InheritPassQueries(DeviceData* dev, VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    if (!dev || !dev->inheritedQueries || !info) return info;
    {
        std::shared_lock lock(dev->secondariesMutex);
        if (!dev->secondaries.count(cb)) return info;
    }
    if (!info->pInheritanceInfo) return info;
    t_inheritance = *info->pInheritanceInfo;
    if (dev->occlusionPrecise) {
        t_inheritance.occlusionQueryEnable = VK_TRUE;
        t_inheritance.queryFlags |= VK_QUERY_CONTROL_PRECISE_BIT;
    }
    if (dev->pipelineStatistics) t_inheritance.pipelineStatistics |= kPipelineStatistics;
    t_inheritingBegin = *info;
    t_inheritingBegin.pInheritanceInfo = &t_inheritance;
    return &t_inheritingBegin;
}

void PreHook_vkBeginCommandBuffer(VkCommandBuffer& commandBuffer, const VkCommandBufferBeginInfo*& pBeginInfo) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    CaptureManager::Get().OnBeginCommandBuffer(dev, commandBuffer, pBeginInfo ? pBeginInfo->flags : 0);
    LayoutTracker::Get().OnBeginCommandBuffer(commandBuffer);
    pBeginInfo = InheritPassQueries(dev, commandBuffer, pBeginInfo);
}

void Hook_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers) {
    DeviceData* dev = GetDeviceData(device);
    if (!dev || !dev->inheritedQueries || !pAllocateInfo || !pCommandBuffers) return;
    // A handle is noted or forgotten at every allocation, so one reused from a freed buffer of the
    // other level (or a destroyed pool's) is never taken for what it was.
    const bool secondary = pAllocateInfo->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    std::unique_lock lock(dev->secondariesMutex);
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
        if (!pCommandBuffers[i]) continue;
        if (secondary) dev->secondaries.insert(pCommandBuffers[i]);
        else dev->secondaries.erase(pCommandBuffers[i]);
    }
}

void Hook_vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                            VkQueryPool* pQueryPool) {
    DeviceData* dev = GetDeviceData(device);
    if (!dev || !(dev->pipelineStatistics || dev->occlusionPrecise) || !pCreateInfo || !pQueryPool || !*pQueryPool) return;
    std::unique_lock lock(dev->queryPoolsMutex);
    if (pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION || pCreateInfo->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS)
        dev->appQueryPools[*pQueryPool] = pCreateInfo->queryType;
    else
        dev->appQueryPools.erase(*pQueryPool);   // a handle reused from a pool of either type
}

void PreHook_vkResetCommandBuffer(VkCommandBuffer& commandBuffer, VkCommandBufferResetFlags& flags) {
    CaptureManager::Get().OnResetCommandBuffer(GetDeviceData(commandBuffer), commandBuffer);
}

// Live shader editing: an edited pipeline is bound as its replacement (see shader_edit.h).
void PreHook_vkCmdBindPipeline(VkCommandBuffer& commandBuffer, VkPipelineBindPoint& pipelineBindPoint, VkPipeline& pipeline) {
    pipeline = ShaderEditor::Get().Resolve(pipeline);
}

// Live shader editing of shader objects: the edited replacements are bound instead.
static thread_local std::vector<VkShaderEXT> t_boundShaders;
void PreHook_vkCmdBindShadersEXT(VkCommandBuffer& commandBuffer, uint32_t& stageCount, const VkShaderStageFlagBits*& pStages,
                                 const VkShaderEXT*& pShaders) {
    pShaders = ShaderEditor::Get().ResolveShaders(stageCount, pShaders, t_boundShaders);
}

// Live shader editing in captures: the generated forwarder records the application's arguments,
// but what ran was the replacement the pre-hooks above bound, an object of its own in the tracker
// with the edited code as its shader payloads. The record is rewritten to name it, with the
// original it stood in for as "replaced", so a capture and its replay carry what the frame drew
// rather than what the application asked for.
void Hook_vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    CommandRecorder* rec = dev ? dev->RecorderFor(commandBuffer) : nullptr;
    if (!rec) return;
    VkPipeline original = ShaderEditor::Get().OriginalOf(pipeline);
    if (!original) return;
    JsonWriter args(&Tracker::Get());
    ArgsToJson_vkCmdBindPipeline(args, commandBuffer, pipelineBindPoint, pipeline);
    rec->ReplaceLastArgs(std::move(args.str()));
    JsonWriter replaced(&Tracker::Get());
    replaced.Handle(HT_VkPipeline, "VkPipeline", (uint64_t)(uintptr_t)original);
    rec->SetExtraOnLast(",\"replaced\":" + replaced.str());
}

void Hook_vkCmdBindShadersEXT(VkCommandBuffer commandBuffer, uint32_t stageCount, const VkShaderStageFlagBits* pStages,
                              const VkShaderEXT* pShaders) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    CommandRecorder* rec = dev ? dev->RecorderFor(commandBuffer) : nullptr;
    if (!rec || !pShaders) return;
    // One entry per bound shader: the original of a replacement, null for the application's own.
    std::vector<VkShaderEXT> originals(stageCount, VK_NULL_HANDLE);
    bool any = false;
    for (uint32_t i = 0; i < stageCount; ++i) {
        originals[i] = ShaderEditor::Get().OriginalShaderOf(pShaders[i]);
        any |= originals[i] != VK_NULL_HANDLE;
    }
    if (!any) return;
    JsonWriter args(&Tracker::Get());
    ArgsToJson_vkCmdBindShadersEXT(args, commandBuffer, stageCount, pStages, pShaders);
    rec->ReplaceLastArgs(std::move(args.str()));
    JsonWriter replaced(&Tracker::Get());
    replaced.BeginArray();
    for (VkShaderEXT original : originals) replaced.Handle(HT_VkShaderEXT, "VkShaderEXT", (uint64_t)(uintptr_t)original);
    replaced.EndArray();
    rec->SetExtraOnLast(",\"replaced\":" + replaced.str());
}

// What about the pass about to begin limits what the capture records around it (PassShape in
// capture.h). A multiview pass's query would need one index per view, so it goes uncounted while
// the rest of an application that merely enables multiview keeps its counters. A render pass says
// whether its first subpass executes secondaries; a later subpass could too, which vkCmdNextSubpass
// says only once the queries have begun.
static PassShape RenderPassShape(const VkRenderPassBeginInfo* begin, VkSubpassContents contents) {
    PassShape shape;
    shape.secondaries = contents != VK_SUBPASS_CONTENTS_INLINE;
    RenderPassInfo rp;
    if (begin && ResourceRegistry::Get().GetRenderPass(begin->renderPass, rp)) {
        shape.multiview = rp.viewLayers > 1;
        shape.secondaries |= rp.subpassColor.size() > 1;
    }
    return shape;
}

static PassShape RenderingShape(const VkRenderingInfo* info) {
    PassShape shape;
    if (!info) return shape;
    shape.multiview = info->viewMask != 0;
    shape.secondaries = (info->flags & (VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT | VK_RENDERING_CONTENTS_INLINE_BIT_KHR)) != 0;
    shape.suspending = (info->flags & VK_RENDERING_SUSPENDING_BIT) != 0;
    shape.resuming = (info->flags & VK_RENDERING_RESUMING_BIT) != 0;
    return shape;
}

// Pass profiling: the begin timestamp goes before the pass (see CaptureManager::OnBeforePass).
static void BeforePass(VkCommandBuffer commandBuffer, const PassShape& shape) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnBeforePass(dev, rec, shape);
}

// Store ops while capturing: an attachment with storeOp DONT_CARE has undefined contents after
// the pass (a tiled GPU never writes it back), so the capture's read-back of it would show
// garbage. While a capture is being recorded, a render pass begins its store-everything copy
// instead (StoreAllRenderPass: compatible with the application's framebuffers and pipelines,
// since store ops do not take part in render pass compatibility), and dynamic rendering gets
// its attachment infos rewritten. The record keeps what the application passed (the generated
// forwarders serialize the original arguments), and the post-hooks map the copy back.
static bool RecordingCapture(VkCommandBuffer cb) {
    // "Record all command buffers" records ahead of any capture (for applications that record
    // once and resubmit), so its recordings get the copies too: whichever of them the capture
    // later shows must read back.
    DeviceData* dev = GetDeviceData(cb);
    return dev && dev->RecorderFor(cb) && (CaptureManager::Get().IsCapturing() || CaptureManager::Get().RecordAlways());
}

static thread_local const VkRenderPassBeginInfo* t_beginOriginal = nullptr;
static thread_local VkRenderPassBeginInfo t_beginCopy;

static const VkRenderPassBeginInfo* StoreAllBegin(VkCommandBuffer cb, const VkRenderPassBeginInfo* info) {
    t_beginOriginal = nullptr;
    if (!info || !RecordingCapture(cb)) return info;
    RenderPassInfo rp;
    if (!ResourceRegistry::Get().GetRenderPass(info->renderPass, rp) || !rp.storeAll) return info;
    t_beginCopy = *info;
    t_beginCopy.renderPass = rp.storeAll;
    t_beginOriginal = info;
    CaptureManager::Get().NoteStoreAllPass();
    return &t_beginCopy;
}

/** The application's begin info when the pre-hook substituted the copy. */
static const VkRenderPassBeginInfo* OriginalBegin(const VkRenderPassBeginInfo* info) {
    return info == &t_beginCopy && t_beginOriginal ? t_beginOriginal : info;
}

static thread_local const VkRenderingInfo* t_renderingOriginal = nullptr;
static thread_local VkRenderingInfo t_renderingCopy;
static thread_local std::vector<VkRenderingAttachmentInfo> t_renderingColors;
static thread_local VkRenderingAttachmentInfo t_renderingDepth, t_renderingStencil;

static const VkRenderingInfo* StoreAllRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
    t_renderingOriginal = nullptr;
    if (!info || !RecordingCapture(cb)) return info;
    bool needed = false;
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) needed |= info->pColorAttachments[i].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (info->pDepthAttachment) needed |= info->pDepthAttachment->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (info->pStencilAttachment) needed |= info->pStencilAttachment->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (!needed) return info;
    t_renderingCopy = *info;
    t_renderingColors.assign(info->pColorAttachments, info->pColorAttachments + info->colorAttachmentCount);
    for (auto& a : t_renderingColors) if (a.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    t_renderingCopy.pColorAttachments = t_renderingColors.data();
    if (info->pDepthAttachment) {
        t_renderingDepth = *info->pDepthAttachment;
        if (t_renderingDepth.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) t_renderingDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        t_renderingCopy.pDepthAttachment = &t_renderingDepth;
    }
    if (info->pStencilAttachment) {
        t_renderingStencil = *info->pStencilAttachment;
        if (t_renderingStencil.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) t_renderingStencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        t_renderingCopy.pStencilAttachment = &t_renderingStencil;
    }
    t_renderingOriginal = info;
    CaptureManager::Get().NoteStoreAllPass();
    return &t_renderingCopy;
}

static const VkRenderingInfo* OriginalRendering(const VkRenderingInfo* info) {
    return info == &t_renderingCopy && t_renderingOriginal ? t_renderingOriginal : info;
}

// Frame-start contents (CaptureManager::SnapshotImageRead): what a command reads that the capture
// has not written yet is copied by its pre-call hook, before the command runs, and the ids go onto
// the command once it is recorded (AttachPendingData, from the post-call hook).
static CommandRecorder* CapturingRecorder(VkCommandBuffer cb, DeviceData*& dev) {
    dev = GetDeviceData(cb);
    return dev && CaptureManager::Get().IsCapturing() ? dev->RecorderFor(cb) : nullptr;
}

static void AttachPendingData(VkCommandBuffer cb) {
    DeviceData* dev = GetDeviceData(cb);
    CommandRecorder* rec = dev ? dev->RecorderFor(cb) : nullptr;
    if (!rec || (rec->pendingImageData.empty() && rec->pendingBufferData.empty())) return;
    auto list = [](const char* key, const std::vector<uint32_t>& ids) {
        std::string s = std::string(",\"") + key + "\":[";
        for (size_t i = 0; i < ids.size(); ++i) s += (i ? "," : "") + std::to_string(ids[i]);
        return s + "]";
    };
    std::string extra;
    if (!rec->pendingImageData.empty()) extra += list("imageData", rec->pendingImageData);
    if (!rec->pendingBufferData.empty()) extra += list("bufferData", rec->pendingBufferData);
    rec->SetExtraOnLast(std::move(extra));
    rec->pendingImageData.clear();
    rec->pendingBufferData.clear();
}

/** A render pass's attachment views: the framebuffer's, or an imageless framebuffer's from the begin info. */
static std::vector<VkImageView> PassAttachmentViews(const VkRenderPassBeginInfo* info, const FramebufferInfo& fb) {
    std::vector<VkImageView> views = fb.attachments;
    if (fb.imageless) {
        for (auto* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
                auto* ab = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(n);
                views.assign(ab->pAttachments, ab->pAttachments + ab->attachmentCount);
            }
        }
    }
    return views;
}

static void SnapshotPassLoads(VkCommandBuffer cb, const VkRenderPassBeginInfo* info) {
    DeviceData* dev = nullptr;
    CommandRecorder* rec = CapturingRecorder(cb, dev);
    FramebufferInfo fb;
    RenderPassInfo rp;
    if (!rec || !info || !ResourceRegistry::Get().GetFramebuffer(info->framebuffer, fb) ||
        !ResourceRegistry::Get().GetRenderPass(info->renderPass, rp))
        return;
    // A run of dispatches before the pass ends here, so its timing leaves the copies out.
    CaptureManager::Get().OnEndComputePass(dev, rec);
    const std::vector<VkImageView> views = PassAttachmentViews(info, fb);
    for (size_t i = 0; i < views.size() && i < rp.attachments.size(); ++i) {
        // A depth/stencil attachment's loadOp is its depth's; its stencil has a load op of its own.
        const RenderPassAttachment& a = rp.attachments[i];
        const VkImageAspectFlags aspects = FormatAspects(a.format);
        if (aspects & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT))
            CaptureManager::Get().OnAttachmentBegin(dev, rec, views[i], aspects & ~VK_IMAGE_ASPECT_STENCIL_BIT, a.loadOp, a.initialLayout, info->renderArea, rec->pendingImageData);
        if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
            CaptureManager::Get().OnAttachmentBegin(dev, rec, views[i], VK_IMAGE_ASPECT_STENCIL_BIT, a.stencilLoadOp, a.initialLayout, info->renderArea, rec->pendingImageData);
    }
}

static void SnapshotRenderingLoads(VkCommandBuffer cb, const VkRenderingInfo* info) {
    DeviceData* dev = nullptr;
    CommandRecorder* rec = CapturingRecorder(cb, dev);
    // A resumed pass carries on from its suspended part, whatever its load ops say.
    if (!rec || !info || (info->flags & VK_RENDERING_RESUMING_BIT)) return;
    CaptureManager::Get().OnEndComputePass(dev, rec);
    auto begin = [&](const VkRenderingAttachmentInfo* a, VkImageAspectFlags aspects) {
        if (a && a->imageView)
            CaptureManager::Get().OnAttachmentBegin(dev, rec, a->imageView, aspects, a->loadOp, a->imageLayout, info->renderArea, rec->pendingImageData);
    };
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) begin(&info->pColorAttachments[i], VK_IMAGE_ASPECT_COLOR_BIT);
    begin(info->pDepthAttachment, VK_IMAGE_ASPECT_DEPTH_BIT);
    begin(info->pStencilAttachment, VK_IMAGE_ASPECT_STENCIL_BIT);
}

/** Whether offset + extent covers all of an image's mip level. */
static bool CoversMip(VkImage image, uint32_t mip, VkOffset3D offset, VkExtent3D extent) {
    ImageInfo img;
    if (!ResourceRegistry::Get().GetImage(image, img) || mip >= img.mipLevels) return false;
    return offset.x <= 0 && offset.y <= 0 && offset.z <= 0 && (int64_t)offset.x + extent.width >= std::max(1u, img.extent.width >> mip) &&
           (int64_t)offset.y + extent.height >= std::max(1u, img.extent.height >> mip) &&
           (int64_t)offset.z + extent.depth >= std::max(1u, img.extent.depth >> mip);
}

/** A blit's destination corners, which may be given in either order. */
static void BlitArea(const VkOffset3D offsets[2], VkOffset3D& offset, VkExtent3D& extent) {
    offset = {std::min(offsets[0].x, offsets[1].x), std::min(offsets[0].y, offsets[1].y), std::min(offsets[0].z, offsets[1].z)};
    extent = {(uint32_t)std::abs(offsets[1].x - offsets[0].x), (uint32_t)std::abs(offsets[1].y - offsets[0].y),
              (uint32_t)std::abs(offsets[1].z - offsets[0].z)};
}

/** Copies, blits and resolves between images (VkImageCopy, VkImageBlit, VkImageResolve and their 2 forms). */
template <typename Region>
static void BeforeImageTransfer(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst, uint32_t count, const Region* regions,
                                void (*area)(const Region&, VkOffset3D&, VkExtent3D&)) {
    DeviceData* dev = nullptr;
    CommandRecorder* rec = CapturingRecorder(cb, dev);
    if (!rec || !regions) return;
    CaptureManager& cm = CaptureManager::Get();
    for (uint32_t i = 0; i < count; ++i) {
        const VkImageSubresourceLayers& s = regions[i].srcSubresource;
        cm.SnapshotImageRead(dev, rec, src, s.aspectMask, s.mipLevel, 1, s.baseArrayLayer, s.layerCount, srcLayout, rec->pendingImageData);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const VkImageSubresourceLayers& d = regions[i].dstSubresource;
        VkOffset3D offset{};
        VkExtent3D extent{};
        area(regions[i], offset, extent);
        if (CoversMip(dst, d.mipLevel, offset, extent)) cm.NoteImageWrite(dst, d.aspectMask, d.mipLevel, 1, d.baseArrayLayer, d.layerCount);
    }
}

template <typename Region>
static void CopyArea(const Region& r, VkOffset3D& offset, VkExtent3D& extent) {
    offset = r.dstOffset;
    extent = r.extent;
}

template <typename Region>
static void BlitRegionArea(const Region& r, VkOffset3D& offset, VkExtent3D& extent) {
    BlitArea(r.dstOffsets, offset, extent);
}

template <typename Region>
static void BeforeImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, uint32_t count, const Region* regions) {
    DeviceData* dev = nullptr;
    CommandRecorder* rec = CapturingRecorder(cb, dev);
    if (!rec || !regions) return;
    for (uint32_t i = 0; i < count; ++i) {
        const VkImageSubresourceLayers& s = regions[i].imageSubresource;
        CaptureManager::Get().SnapshotImageRead(dev, rec, src, s.aspectMask, s.mipLevel, 1, s.baseArrayLayer, s.layerCount, srcLayout,
                                                rec->pendingImageData);
    }
}

/** A buffer's texels copied into an image: the source ranges in full (a replay writes them), and whole-extent writes. */
template <typename Region>
static void BeforeBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, uint32_t count, const Region* regions) {
    DeviceData* dev = nullptr;
    CommandRecorder* rec = CapturingRecorder(cb, dev);
    if (!rec || !regions) return;
    CaptureManager& cm = CaptureManager::Get();
    ImageInfo img;
    const bool known = ResourceRegistry::Get().GetImage(dst, img);
    for (uint32_t i = 0; i < count; ++i) {
        const Region& r = regions[i];
        // The texels the region reads, as the buffer lays them out (rows of bufferRowLength, images of bufferImageHeight).
        VkDeviceSize size = VK_WHOLE_SIZE;
        if (known) {
            const FormatBlock block = FormatBlockInfo(img.format, r.imageSubresource.aspectMask);
            const uint32_t rowLength = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
            const uint32_t imageHeight = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
            const uint32_t layers = r.imageSubresource.layerCount == VK_REMAINING_ARRAY_LAYERS
                                  ? img.arrayLayers - std::min(img.arrayLayers, r.imageSubresource.baseArrayLayer) : r.imageSubresource.layerCount;
            if (block.bytes)
                size = (VkDeviceSize)((rowLength + block.width - 1) / block.width) * ((imageHeight + block.height - 1) / block.height) *
                       std::max(1u, r.imageExtent.depth) * std::max(1u, layers) * block.bytes;
        }
        rec->pendingBufferData.push_back(cm.QueueBufferCapture(dev, rec, src, r.bufferOffset, size, true));
        const VkImageSubresourceLayers& d = r.imageSubresource;
        if (CoversMip(dst, d.mipLevel, r.imageOffset, r.imageExtent)) cm.NoteImageWrite(dst, d.aspectMask, d.mipLevel, 1, d.baseArrayLayer, d.layerCount);
    }
}

template <typename Region>
static void BeforeBufferCopy(VkCommandBuffer cb, VkBuffer src, uint32_t count, const Region* regions) {
    DeviceData* dev = nullptr;
    CommandRecorder* rec = CapturingRecorder(cb, dev);
    if (!rec || !regions) return;
    for (uint32_t i = 0; i < count; ++i)
        rec->pendingBufferData.push_back(CaptureManager::Get().QueueBufferCapture(dev, rec, src, regions[i].srcOffset, regions[i].size, true));
}

void PreHook_vkCmdCopyImage(VkCommandBuffer& commandBuffer, VkImage& srcImage, VkImageLayout& srcImageLayout, VkImage& dstImage,
                            VkImageLayout& dstImageLayout, uint32_t& regionCount, const VkImageCopy*& pRegions) {
    BeforeImageTransfer<VkImageCopy>(commandBuffer, srcImage, srcImageLayout, dstImage, regionCount, pRegions, CopyArea);
}
void PreHook_vkCmdCopyImage2(VkCommandBuffer& commandBuffer, const VkCopyImageInfo2*& pCopyImageInfo) {
    if (const VkCopyImageInfo2* i = pCopyImageInfo)
        BeforeImageTransfer<VkImageCopy2>(commandBuffer, i->srcImage, i->srcImageLayout, i->dstImage, i->regionCount, i->pRegions, CopyArea);
}
void PreHook_vkCmdCopyImage2KHR(VkCommandBuffer& commandBuffer, const VkCopyImageInfo2*& pCopyImageInfo) { PreHook_vkCmdCopyImage2(commandBuffer, pCopyImageInfo); }
void PreHook_vkCmdBlitImage(VkCommandBuffer& commandBuffer, VkImage& srcImage, VkImageLayout& srcImageLayout, VkImage& dstImage,
                            VkImageLayout& dstImageLayout, uint32_t& regionCount, const VkImageBlit*& pRegions, VkFilter& filter) {
    BeforeImageTransfer<VkImageBlit>(commandBuffer, srcImage, srcImageLayout, dstImage, regionCount, pRegions, BlitRegionArea);
}
void PreHook_vkCmdBlitImage2(VkCommandBuffer& commandBuffer, const VkBlitImageInfo2*& pBlitImageInfo) {
    if (const VkBlitImageInfo2* i = pBlitImageInfo)
        BeforeImageTransfer<VkImageBlit2>(commandBuffer, i->srcImage, i->srcImageLayout, i->dstImage, i->regionCount, i->pRegions, BlitRegionArea);
}
void PreHook_vkCmdBlitImage2KHR(VkCommandBuffer& commandBuffer, const VkBlitImageInfo2*& pBlitImageInfo) { PreHook_vkCmdBlitImage2(commandBuffer, pBlitImageInfo); }
// A resolve reads a multisampled image, whose contents are not taken; its destination can be written whole.
void PreHook_vkCmdResolveImage(VkCommandBuffer& commandBuffer, VkImage& srcImage, VkImageLayout& srcImageLayout, VkImage& dstImage,
                               VkImageLayout& dstImageLayout, uint32_t& regionCount, const VkImageResolve*& pRegions) {
    BeforeImageTransfer<VkImageResolve>(commandBuffer, srcImage, srcImageLayout, dstImage, regionCount, pRegions, CopyArea);
}
void PreHook_vkCmdResolveImage2(VkCommandBuffer& commandBuffer, const VkResolveImageInfo2*& pResolveImageInfo) {
    if (const VkResolveImageInfo2* i = pResolveImageInfo)
        BeforeImageTransfer<VkImageResolve2>(commandBuffer, i->srcImage, i->srcImageLayout, i->dstImage, i->regionCount, i->pRegions, CopyArea);
}
void PreHook_vkCmdResolveImage2KHR(VkCommandBuffer& commandBuffer, const VkResolveImageInfo2*& pResolveImageInfo) { PreHook_vkCmdResolveImage2(commandBuffer, pResolveImageInfo); }
void PreHook_vkCmdCopyImageToBuffer(VkCommandBuffer& commandBuffer, VkImage& srcImage, VkImageLayout& srcImageLayout, VkBuffer& dstBuffer,
                                    uint32_t& regionCount, const VkBufferImageCopy*& pRegions) {
    BeforeImageToBuffer(commandBuffer, srcImage, srcImageLayout, regionCount, pRegions);
}
void PreHook_vkCmdCopyImageToBuffer2(VkCommandBuffer& commandBuffer, const VkCopyImageToBufferInfo2*& pCopyImageToBufferInfo) {
    if (const VkCopyImageToBufferInfo2* i = pCopyImageToBufferInfo) BeforeImageToBuffer(commandBuffer, i->srcImage, i->srcImageLayout, i->regionCount, i->pRegions);
}
void PreHook_vkCmdCopyImageToBuffer2KHR(VkCommandBuffer& commandBuffer, const VkCopyImageToBufferInfo2*& pCopyImageToBufferInfo) {
    PreHook_vkCmdCopyImageToBuffer2(commandBuffer, pCopyImageToBufferInfo);
}
void PreHook_vkCmdCopyBuffer(VkCommandBuffer& commandBuffer, VkBuffer& srcBuffer, VkBuffer& dstBuffer, uint32_t& regionCount, const VkBufferCopy*& pRegions) {
    BeforeBufferCopy(commandBuffer, srcBuffer, regionCount, pRegions);
}
void PreHook_vkCmdCopyBuffer2(VkCommandBuffer& commandBuffer, const VkCopyBufferInfo2*& pCopyBufferInfo) {
    if (const VkCopyBufferInfo2* i = pCopyBufferInfo) BeforeBufferCopy(commandBuffer, i->srcBuffer, i->regionCount, i->pRegions);
}
void PreHook_vkCmdCopyBuffer2KHR(VkCommandBuffer& commandBuffer, const VkCopyBufferInfo2*& pCopyBufferInfo) { PreHook_vkCmdCopyBuffer2(commandBuffer, pCopyBufferInfo); }
void PreHook_vkCmdCopyBufferToImage(VkCommandBuffer& commandBuffer, VkBuffer& srcBuffer, VkImage& dstImage, VkImageLayout& dstImageLayout,
                                    uint32_t& regionCount, const VkBufferImageCopy*& pRegions) {
    BeforeBufferToImage(commandBuffer, srcBuffer, dstImage, regionCount, pRegions);
}
void PreHook_vkCmdCopyBufferToImage2(VkCommandBuffer& commandBuffer, const VkCopyBufferToImageInfo2*& pCopyBufferToImageInfo) {
    if (const VkCopyBufferToImageInfo2* i = pCopyBufferToImageInfo) BeforeBufferToImage(commandBuffer, i->srcBuffer, i->dstImage, i->regionCount, i->pRegions);
}
void PreHook_vkCmdCopyBufferToImage2KHR(VkCommandBuffer& commandBuffer, const VkCopyBufferToImageInfo2*& pCopyBufferToImageInfo) {
    PreHook_vkCmdCopyBufferToImage2(commandBuffer, pCopyBufferToImageInfo);
}

void PreHook_vkCmdBeginRenderPass(VkCommandBuffer& commandBuffer, const VkRenderPassBeginInfo*& pRenderPassBegin, VkSubpassContents& contents) {
    SnapshotPassLoads(commandBuffer, pRenderPassBegin);
    BeforePass(commandBuffer, RenderPassShape(pRenderPassBegin, contents));
    pRenderPassBegin = StoreAllBegin(commandBuffer, pRenderPassBegin);
}
void PreHook_vkCmdBeginRenderPass2(VkCommandBuffer& commandBuffer, const VkRenderPassBeginInfo*& pRenderPassBegin, const VkSubpassBeginInfo*& pSubpassBeginInfo) {
    SnapshotPassLoads(commandBuffer, pRenderPassBegin);
    BeforePass(commandBuffer, RenderPassShape(pRenderPassBegin, pSubpassBeginInfo ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE));
    pRenderPassBegin = StoreAllBegin(commandBuffer, pRenderPassBegin);
}
void PreHook_vkCmdBeginRenderPass2KHR(VkCommandBuffer& commandBuffer, const VkRenderPassBeginInfo*& pRenderPassBegin, const VkSubpassBeginInfo*& pSubpassBeginInfo) {
    PreHook_vkCmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}
void PreHook_vkCmdBeginRendering(VkCommandBuffer& commandBuffer, const VkRenderingInfo*& pRenderingInfo) {
    SnapshotRenderingLoads(commandBuffer, pRenderingInfo);
    BeforePass(commandBuffer, RenderingShape(pRenderingInfo));
    pRenderingInfo = StoreAllRendering(commandBuffer, pRenderingInfo);
}
// Compute pass timing: a dispatch outside a render pass opens a compute pass; barriers, event
// waits, debug labels, secondary execution and the end of the buffer close it (all before the
// command runs, so the timestamps bracket exactly the dispatches).
static void BeforeDispatch(VkCommandBuffer commandBuffer) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnBeforeDispatch(dev, rec);
}
static void EndComputePass(VkCommandBuffer commandBuffer) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnEndComputePass(dev, rec);
}
void PreHook_vkCmdDispatch(VkCommandBuffer& commandBuffer, uint32_t&, uint32_t&, uint32_t&) { BeforeDispatch(commandBuffer); }
void PreHook_vkCmdDispatchBase(VkCommandBuffer& commandBuffer, uint32_t&, uint32_t&, uint32_t&, uint32_t&, uint32_t&, uint32_t&) { BeforeDispatch(commandBuffer); }
void PreHook_vkCmdDispatchBaseKHR(VkCommandBuffer& commandBuffer, uint32_t&, uint32_t&, uint32_t&, uint32_t&, uint32_t&, uint32_t&) { BeforeDispatch(commandBuffer); }
void PreHook_vkCmdDispatchIndirect(VkCommandBuffer& commandBuffer, VkBuffer&, VkDeviceSize&) { BeforeDispatch(commandBuffer); }
void PreHook_vkCmdPipelineBarrier(VkCommandBuffer& commandBuffer, VkPipelineStageFlags&, VkPipelineStageFlags&, VkDependencyFlags&, uint32_t&,
                                  const VkMemoryBarrier*&, uint32_t&, const VkBufferMemoryBarrier*&, uint32_t&, const VkImageMemoryBarrier*&) {
    EndComputePass(commandBuffer);
}
void PreHook_vkCmdPipelineBarrier2(VkCommandBuffer& commandBuffer, const VkDependencyInfo*&) { EndComputePass(commandBuffer); }
void PreHook_vkCmdPipelineBarrier2KHR(VkCommandBuffer& commandBuffer, const VkDependencyInfo*&) { EndComputePass(commandBuffer); }
void PreHook_vkCmdWaitEvents(VkCommandBuffer& commandBuffer, uint32_t&, const VkEvent*&, VkPipelineStageFlags&, VkPipelineStageFlags&, uint32_t&,
                             const VkMemoryBarrier*&, uint32_t&, const VkBufferMemoryBarrier*&, uint32_t&, const VkImageMemoryBarrier*&) {
    EndComputePass(commandBuffer);
}
void PreHook_vkCmdWaitEvents2(VkCommandBuffer& commandBuffer, uint32_t&, const VkEvent*&, const VkDependencyInfo*&) { EndComputePass(commandBuffer); }
void PreHook_vkCmdWaitEvents2KHR(VkCommandBuffer& commandBuffer, uint32_t&, const VkEvent*&, const VkDependencyInfo*&) { EndComputePass(commandBuffer); }
// The application's own queries: two queries of one type cannot be active in a command buffer at
// once, and the layer's queries over a pass begin outside it, so they cannot be ended early inside
// it to make way. Instead the first query the application begins of an occlusion or pipeline
// statistics pool turns the layer's counter of that type off for the device. An application that
// uses such queries does so from its first frames, well before a capture; only a query begun for
// the first time inside a captured pass still overlaps the layer's.
static void NoteAppQuery(VkCommandBuffer commandBuffer, VkQueryPool pool) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (!dev || (dev->appOcclusionQueries.load(std::memory_order_relaxed) && dev->appStatisticsQueries.load(std::memory_order_relaxed))) return;
    VkQueryType type;
    {
        std::shared_lock lock(dev->queryPoolsMutex);
        auto it = dev->appQueryPools.find(pool);
        if (it == dev->appQueryPools.end()) return;
        type = it->second;
    }
    const bool occlusion = type == VK_QUERY_TYPE_OCCLUSION;
    std::atomic<bool>& used = occlusion ? dev->appOcclusionQueries : dev->appStatisticsQueries;
    if (!used.exchange(true, std::memory_order_relaxed))
        Log("pass counters: the application begins %s queries of its own; passes go without the layer's", occlusion ? "occlusion" : "pipeline statistics");
}

void PreHook_vkCmdBeginQuery(VkCommandBuffer& commandBuffer, VkQueryPool& queryPool, uint32_t&, VkQueryControlFlags&) { NoteAppQuery(commandBuffer, queryPool); }
void PreHook_vkCmdBeginQueryIndexedEXT(VkCommandBuffer& commandBuffer, VkQueryPool& queryPool, uint32_t&, VkQueryControlFlags&, uint32_t&) {
    NoteAppQuery(commandBuffer, queryPool);
}

void PreHook_vkCmdExecuteCommands(VkCommandBuffer& commandBuffer, uint32_t&, const VkCommandBuffer*&) { EndComputePass(commandBuffer); }
void PreHook_vkEndCommandBuffer(VkCommandBuffer& commandBuffer) { EndComputePass(commandBuffer); }
void PreHook_vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer& commandBuffer, const VkDebugUtilsLabelEXT*&) { EndComputePass(commandBuffer); }
void PreHook_vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer& commandBuffer) { EndComputePass(commandBuffer); }
void PreHook_vkCmdDebugMarkerBeginEXT(VkCommandBuffer& commandBuffer, const VkDebugMarkerMarkerInfoEXT*&) { EndComputePass(commandBuffer); }
void PreHook_vkCmdDebugMarkerEndEXT(VkCommandBuffer& commandBuffer) { EndComputePass(commandBuffer); }

void PreHook_vkCmdBeginRenderingKHR(VkCommandBuffer& commandBuffer, const VkRenderingInfo*& pRenderingInfo) {
    PreHook_vkCmdBeginRendering(commandBuffer, pRenderingInfo);
}

// CPU submit time: the wall-clock time the application spends inside vkQueueSubmit*, accumulated
// per device for the frame stats.
static thread_local std::chrono::steady_clock::time_point t_submitStart;
static void SubmitBegin() { t_submitStart = std::chrono::steady_clock::now(); }
static void SubmitEnd(VkQueue queue) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_submitStart).count();
    if (DeviceData* dev = GetDeviceData(queue)) dev->submitNanos.fetch_add((uint64_t)ns, std::memory_order_relaxed);
}
// Command buffers recorded before the capture began carry no read-back copies, so their attachments
// are read after they run (CaptureManager::ReadBackSubmitted). Read after the whole submission, a
// target a later buffer of the same submission renders over would show the later contents, so the
// submission is split after each such buffer: the parts before the last are submitted here, each
// followed by its read-back, and the application's call submits the last part with its fence. An
// info whose buffers are split waits on its semaphores in its first part and signals them in its
// last. Submissions extending their infos with anything but timeline semaphore values stay whole.
struct SubmitPart {
    std::vector<VkSubmitInfo> infos;
    std::vector<VkTimelineSemaphoreSubmitInfo> timelines;   // reserved up front: infos point into it
    std::vector<VkSubmitInfo2> infos2;
};
struct SplitSubmit {
    bool active = false;
    uint32_t count = 0;
    const VkSubmitInfo* submits = nullptr;
    const VkSubmitInfo2* submits2 = nullptr;
    std::vector<VkCommandBuffer> readBack;
    SubmitPart last;
};
static thread_local SplitSubmit t_split;

// Where the flattened command buffers [first, end) of `total` fall in an info holding [lo, hi): the
// range it contributes, and whether that includes the info's first and last buffers. An info with
// no buffers goes with the part its position starts.
static bool PartRange(size_t lo, size_t hi, size_t first, size_t end, size_t total, size_t& from, size_t& to, bool& head, bool& tail) {
    if (lo == hi) {
        from = to = lo;
        head = tail = true;
        return lo >= first && (lo < end || (end == total && lo == total));
    }
    from = std::max(lo, first);
    to = std::min(hi, end);
    head = from == lo;
    tail = to == hi;
    return from < to;
}

static const VkTimelineSemaphoreSubmitInfo* TimelineOf(const VkSubmitInfo& s, bool& other) {
    const VkTimelineSemaphoreSubmitInfo* timeline = nullptr;
    for (auto* p = static_cast<const VkBaseInStructure*>(s.pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(p);
        else other = true;
    }
    return timeline;
}

static void BuildPart(uint32_t count, const VkSubmitInfo* submits, size_t first, size_t end, size_t total, SubmitPart& out) {
    out.infos.clear();
    out.timelines.clear();
    out.timelines.reserve(count);
    size_t base = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const VkSubmitInfo& s = submits[i];
        size_t lo = base, hi = base + s.commandBufferCount, from, to;
        bool head, tail, other = false;
        base = hi;
        if (!PartRange(lo, hi, first, end, total, from, to, head, tail)) continue;
        VkSubmitInfo part = s;
        part.pNext = nullptr;
        part.commandBufferCount = (uint32_t)(to - from);
        part.pCommandBuffers = s.pCommandBuffers ? s.pCommandBuffers + (from - lo) : nullptr;
        if (!head) { part.waitSemaphoreCount = 0; part.pWaitSemaphores = nullptr; part.pWaitDstStageMask = nullptr; }
        if (!tail) { part.signalSemaphoreCount = 0; part.pSignalSemaphores = nullptr; }
        if (const VkTimelineSemaphoreSubmitInfo* t = TimelineOf(s, other)) {
            VkTimelineSemaphoreSubmitInfo tp = *t;
            tp.pNext = nullptr;
            if (!head) { tp.waitSemaphoreValueCount = 0; tp.pWaitSemaphoreValues = nullptr; }
            if (!tail) { tp.signalSemaphoreValueCount = 0; tp.pSignalSemaphoreValues = nullptr; }
            out.timelines.push_back(tp);
            part.pNext = &out.timelines.back();
        }
        out.infos.push_back(part);
    }
}

static void BuildPart(uint32_t count, const VkSubmitInfo2* submits, size_t first, size_t end, size_t total, SubmitPart& out) {
    out.infos2.clear();
    size_t base = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const VkSubmitInfo2& s = submits[i];
        size_t lo = base, hi = base + s.commandBufferInfoCount, from, to;
        bool head, tail;
        base = hi;
        if (!PartRange(lo, hi, first, end, total, from, to, head, tail)) continue;
        VkSubmitInfo2 part = s;
        part.commandBufferInfoCount = (uint32_t)(to - from);
        part.pCommandBufferInfos = s.pCommandBufferInfos ? s.pCommandBufferInfos + (from - lo) : nullptr;
        if (!head) { part.waitSemaphoreInfoCount = 0; part.pWaitSemaphoreInfos = nullptr; }
        if (!tail) { part.signalSemaphoreInfoCount = 0; part.pSignalSemaphoreInfos = nullptr; }
        out.infos2.push_back(part);
    }
}

// The submission's command buffers, and after which of them to split; none when it stays whole.
static std::vector<size_t> SplitPoints(DeviceData* dev, const std::vector<VkCommandBuffer>& cbs, bool splittable) {
    std::vector<size_t> cuts;
    if (!dev || !splittable || cbs.size() < 2 || !CaptureManager::Get().IsCapturing()) return cuts;
    for (size_t k = 0; k + 1 < cbs.size(); ++k)
        if (CaptureManager::Get().NeedsSubmitReadBack(dev, cbs[k])) cuts.push_back(k);
    return cuts;
}

// Submits each part before a cut and reads its buffer back; returns where the last part starts.
template <typename Info>
static size_t SubmitParts(DeviceData* dev, VkQueue queue, uint32_t count, const Info* submits, const std::vector<VkCommandBuffer>& cbs,
                          const std::vector<size_t>& cuts) {
    size_t first = 0;
    for (size_t cut : cuts) {
        SubmitPart part;
        BuildPart(count, submits, first, cut + 1, cbs.size(), part);
        VkResult res = std::is_same_v<Info, VkSubmitInfo>
            ? dev->dispatch.QueueSubmit(queue, (uint32_t)part.infos.size(), part.infos.data(), VK_NULL_HANDLE)
            : dev->dispatch.QueueSubmit2(queue, (uint32_t)part.infos2.size(), part.infos2.data(), VK_NULL_HANDLE);
        if (res != VK_SUCCESS) break;
        LayoutTracker::Get().OnSubmit((uint32_t)(cut + 1 - first), cbs.data() + first);
        CaptureManager::Get().ReadBackSubmitted(dev, queue, cbs[cut]);
        t_split.readBack.push_back(cbs[cut]);
        first = cut + 1;
    }
    return first;
}

void PreHook_vkQueueSubmit(VkQueue& queue, uint32_t& submitCount, const VkSubmitInfo*& pSubmits, VkFence& fence) {
    t_split = SplitSubmit{};
    if (!pSubmits || submitCount == 0 || !CaptureManager::Get().IsCapturing()) return SubmitBegin();
    DeviceData* dev = GetDeviceData(queue);
    std::vector<VkCommandBuffer> cbs;
    bool splittable = pSubmits != nullptr;
    for (uint32_t i = 0; pSubmits && i < submitCount; ++i) {
        bool other = false;
        TimelineOf(pSubmits[i], other);
        splittable = splittable && !other;
        for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; ++j) cbs.push_back(pSubmits[i].pCommandBuffers[j]);
    }
    std::vector<size_t> cuts = SplitPoints(dev, cbs, splittable);
    if (!cuts.empty()) {
        t_split.active = true;
        t_split.count = submitCount;
        t_split.submits = pSubmits;
        size_t first = SubmitParts(dev, queue, submitCount, pSubmits, cbs, cuts);
        BuildPart(submitCount, pSubmits, first, cbs.size(), cbs.size(), t_split.last);
        submitCount = (uint32_t)t_split.last.infos.size();
        pSubmits = t_split.last.infos.data();
    }
    SubmitBegin();
}

// An application without a swapchain marks its frames by waiting on its fences (see layer.cpp).
void PreHook_vkWaitForFences(VkDevice& device, uint32_t& fenceCount, const VkFence*& pFences, VkBool32& waitAll, uint64_t& timeout) {
    OnWaitForFrames(GetDeviceData(device));
}

void PreHook_vkQueueSubmit2(VkQueue& queue, uint32_t& submitCount, const VkSubmitInfo2*& pSubmits, VkFence& fence) {
    t_split = SplitSubmit{};
    if (!pSubmits || submitCount == 0 || !CaptureManager::Get().IsCapturing()) return SubmitBegin();
    DeviceData* dev = GetDeviceData(queue);
    std::vector<VkCommandBuffer> cbs;
    bool splittable = pSubmits != nullptr;
    for (uint32_t i = 0; pSubmits && i < submitCount; ++i) {
        splittable = splittable && !pSubmits[i].pNext;
        for (uint32_t j = 0; j < pSubmits[i].commandBufferInfoCount; ++j) cbs.push_back(pSubmits[i].pCommandBufferInfos[j].commandBuffer);
    }
    std::vector<size_t> cuts = SplitPoints(dev, cbs, splittable);
    if (!cuts.empty()) {
        t_split.active = true;
        t_split.count = submitCount;
        t_split.submits2 = pSubmits;
        size_t first = SubmitParts(dev, queue, submitCount, pSubmits, cbs, cuts);
        BuildPart(submitCount, pSubmits, first, cbs.size(), cbs.size(), t_split.last);
        submitCount = (uint32_t)t_split.last.infos2.size();
        pSubmits = t_split.last.infos2.data();
    }
    SubmitBegin();
}

void PreHook_vkQueueSubmit2KHR(VkQueue& queue, uint32_t& submitCount, const VkSubmitInfo2*& pSubmits, VkFence& fence) {
    PreHook_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

// =============================================================================================
// Resource registry

void Hook_vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
                        const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    if (!pCreateInfo || !pImage || !*pImage) return;
    ImageInfo info;
    info.device = device;
    info.format = pCreateInfo->format;
    info.type = pCreateInfo->imageType;
    info.extent = pCreateInfo->extent;
    info.mipLevels = pCreateInfo->mipLevels;
    info.arrayLayers = pCreateInfo->arrayLayers;
    info.samples = pCreateInfo->samples;
    info.usage = pCreateInfo->usage;
    info.tiling = pCreateInfo->tiling;
    info.transferSrc = (pCreateInfo->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    ResourceRegistry::Get().AddImage(*pImage, info);
}

void Hook_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
                            const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
    if (!pCreateInfo || !pView || !*pView) return;
    ImageViewInfo info;
    info.image = pCreateInfo->image;
    info.format = pCreateInfo->format;
    info.range = pCreateInfo->subresourceRange;
    ResourceRegistry::Get().AddImageView(*pView, info);
}

void Hook_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                         const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
    if (!pCreateInfo || !pBuffer || !*pBuffer) return;
    BufferInfo info;
    info.device = device;
    info.size = pCreateInfo->size;
    info.usage = pCreateInfo->usage;
    info.transferSrc = (pCreateInfo->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) != 0;
    ResourceRegistry::Get().AddBuffer(*pBuffer, info);
}

void Hook_vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo,
                              const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer) {
    if (!pCreateInfo || !pFramebuffer || !*pFramebuffer) return;
    FramebufferInfo info;
    info.renderPass = pCreateInfo->renderPass;
    info.width = pCreateInfo->width;
    info.height = pCreateInfo->height;
    info.layers = pCreateInfo->layers;
    info.imageless = (pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
    if (!info.imageless && pCreateInfo->pAttachments)
        info.attachments.assign(pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);
    ResourceRegistry::Get().AddFramebuffer(*pFramebuffer, info);
}

// The layers a multiview mask renders to: the highest set bit + 1 (0 without multiview).
static uint32_t ViewMaskLayers(uint32_t viewMask) {
    uint32_t layers = 0;
    for (uint32_t bit = 0; bit < 32; ++bit)
        if (viewMask & (1u << bit)) layers = bit + 1;
    return layers;
}

// The store-everything copy of a render pass (see the store-op notes above the begin pre-hooks):
// the same create info with every storeOp DONT_CARE turned into STORE, created straight through
// the dispatch table so it stays out of the object list. VK_NULL_HANDLE when nothing to change.
/** Turns an attachment's DONT_CARE store ops into STORE (the stencil one only where the format has stencil); whether any changed. */
template <typename Attachment>
static bool StoreAll(Attachment& a) {
    bool changed = false;
    if (a.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) { a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; changed = true; }
    if ((FormatAspects(a.format) & VK_IMAGE_ASPECT_STENCIL_BIT) && a.stencilStoreOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) {
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        changed = true;
    }
    return changed;
}

static VkRenderPass StoreAllRenderPass(VkDevice device, const VkRenderPassCreateInfo* ci) {
    std::vector<VkAttachmentDescription> atts(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    bool needed = false;
    for (auto& a : atts) needed |= StoreAll(a);
    DeviceData* dev = GetDeviceData(device);
    if (!needed || !dev || !dev->dispatch.CreateRenderPass) return VK_NULL_HANDLE;
    VkRenderPassCreateInfo copy = *ci;
    copy.pAttachments = atts.data();
    VkRenderPass rp = VK_NULL_HANDLE;
    return dev->dispatch.CreateRenderPass(device, &copy, nullptr, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}

static VkRenderPass StoreAllRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* ci) {
    std::vector<VkAttachmentDescription2> atts(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    bool needed = false;
    for (auto& a : atts) needed |= StoreAll(a);
    DeviceData* dev = GetDeviceData(device);
    if (!needed || !dev) return VK_NULL_HANDLE;
    PFN_vkCreateRenderPass2 create = dev->dispatch.CreateRenderPass2 ? dev->dispatch.CreateRenderPass2 : dev->dispatch.CreateRenderPass2KHR;
    if (!create) return VK_NULL_HANDLE;
    VkRenderPassCreateInfo2 copy = *ci;
    copy.pAttachments = atts.data();
    VkRenderPass rp = VK_NULL_HANDLE;
    return create(device, &copy, nullptr, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}

void Hook_vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
    RenderPassInfo info;
    if (!renderPass || !ResourceRegistry::Get().GetRenderPass(renderPass, info) || !info.storeAll) return;
    DeviceData* dev = GetDeviceData(device);
    if (dev && dev->dispatch.DestroyRenderPass) dev->dispatch.DestroyRenderPass(device, info.storeAll, nullptr);
    info.storeAll = VK_NULL_HANDLE;
    ResourceRegistry::Get().AddRenderPass(renderPass, info);
}

void Hook_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
                             const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    if (!pCreateInfo || !pRenderPass || !*pRenderPass) return;
    RenderPassInfo info;
    info.storeAll = StoreAllRenderPass(device, pCreateInfo);
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
        const VkAttachmentDescription& a = pCreateInfo->pAttachments[i];
        info.attachments.push_back({a.format, a.samples, a.finalLayout, a.storeOp, a.loadOp, a.stencilLoadOp, a.initialLayout});
    }
    for (uint32_t s = 0; s < pCreateInfo->subpassCount; ++s) {
        const VkSubpassDescription& sp = pCreateInfo->pSubpasses[s];
        std::vector<uint32_t> color;
        for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) color.push_back(sp.pColorAttachments[i].attachment);
        info.subpassColor.push_back(color);
        info.subpassDepth.push_back(sp.pDepthStencilAttachment ? (int32_t)sp.pDepthStencilAttachment->attachment : -1);
    }
    // Multiview (VK_KHR_multiview on a version 1 render pass): the view masks come in the pNext chain.
    for (auto* n = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); n; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) continue;
        auto* mv = reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(n);
        for (uint32_t s = 0; s < mv->subpassCount; ++s) info.viewLayers = std::max(info.viewLayers, ViewMaskLayers(mv->pViewMasks[s]));
    }
    ResourceRegistry::Get().AddRenderPass(*pRenderPass, info);
}

void Hook_vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                              const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    if (!pCreateInfo || !pRenderPass || !*pRenderPass) return;
    RenderPassInfo info;
    info.storeAll = StoreAllRenderPass2(device, pCreateInfo);
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
        const VkAttachmentDescription2& a = pCreateInfo->pAttachments[i];
        info.attachments.push_back({a.format, a.samples, a.finalLayout, a.storeOp, a.loadOp, a.stencilLoadOp, a.initialLayout});
    }
    for (uint32_t s = 0; s < pCreateInfo->subpassCount; ++s) {
        const VkSubpassDescription2& sp = pCreateInfo->pSubpasses[s];
        std::vector<uint32_t> color;
        for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) color.push_back(sp.pColorAttachments[i].attachment);
        info.subpassColor.push_back(color);
        info.subpassDepth.push_back(sp.pDepthStencilAttachment ? (int32_t)sp.pDepthStencilAttachment->attachment : -1);
        info.viewLayers = std::max(info.viewLayers, ViewMaskLayers(sp.viewMask));
    }
    ResourceRegistry::Get().AddRenderPass(*pRenderPass, info);
}

void Hook_vkCreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    Hook_vkCreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
}

void Hook_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    if (!pCreateInfo || !pSwapchain || !*pSwapchain) return;
    SwapchainInfo info;
    info.format = pCreateInfo->imageFormat;
    info.extent = pCreateInfo->imageExtent;
    info.usage = pCreateInfo->imageUsage;
    info.arrayLayers = pCreateInfo->imageArrayLayers;
    info.presentMode = pCreateInfo->presentMode;
    RefreshSource source = RefreshSource::Unknown;
    info.refreshMs = QueryRefreshMs(GetDeviceData(device), *pSwapchain, source);
    info.refreshSource = (int)source;
    if (info.refreshMs > 0) Log("swapchain refresh period %.3f ms (%s)", info.refreshMs, RefreshSourceName(source));
    ResourceRegistry::Get().AddSwapchain(*pSwapchain, info);
}

void Hook_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount,
                                  VkImage* pSwapchainImages) {
    if (!pSwapchainImages || !pSwapchainImageCount) return;
    SwapchainInfo sc;
    if (!ResourceRegistry::Get().GetSwapchain(swapchain, sc)) {
        Log("swapchain images: %u handed out for a swapchain the layer does not know", *pSwapchainImageCount);
        return;
    }
    Log("swapchain images: %u tracked", *pSwapchainImageCount);
    // The HUD draws into these images by index, so it needs them in the order handed out here.
    Hud::Get().OnSwapchainImages(device, swapchain, *pSwapchainImageCount, pSwapchainImages);
    ImageInfo info;
    info.device = device;
    info.swapchainImage = true;
    info.format = sc.format;
    info.extent = {sc.extent.width, sc.extent.height, 1};
    info.arrayLayers = sc.arrayLayers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = sc.usage;
    info.transferSrc = (sc.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    for (uint32_t i = 0; i < *pSwapchainImageCount; ++i)
        if (pSwapchainImages[i]) ResourceRegistry::Get().AddImage(pSwapchainImages[i], info);
}

// =============================================================================================
// Shader code

// The stage(s) of a SPIR-V module from its OpEntryPoint instructions, as "vertex", "fragment",
// "vertex+fragment" (execution models per the SPIR-V spec).
static std::string SpirvStages(const uint32_t* words, size_t count) {
    if (count < 5 || words[0] != 0x07230203u) return "";
    std::string stages;
    for (size_t i = 5; i < count;) {
        uint32_t op = words[i] & 0xffff;
        uint32_t len = words[i] >> 16;
        if (!len || i + len > count) break;
        if (op == 54) break;   // OpFunction: declarations are over
        if (op == 15 && len >= 3) {
            const char* name = nullptr;
            switch (words[i + 1]) {
                case 0: name = "vertex"; break;
                case 1: name = "tess control"; break;
                case 2: name = "tess eval"; break;
                case 3: name = "geometry"; break;
                case 4: name = "fragment"; break;
                case 5: name = "compute"; break;
                case 5267: case 5364: name = "task"; break;
                case 5268: case 5365: name = "mesh"; break;
                case 5313: name = "raygen"; break;
                case 5314: name = "intersection"; break;
                case 5315: name = "any hit"; break;
                case 5316: name = "closest hit"; break;
                case 5317: name = "miss"; break;
                case 5318: name = "callable"; break;
                default: name = "shader"; break;
            }
            if (stages.find(name) == std::string::npos) {
                if (!stages.empty()) stages += "+";
                stages += name;
            }
        }
        i += len;
    }
    return stages;
}

void Hook_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule) {
    if (!pCreateInfo || !pCreateInfo->pCode || !pShaderModule || !*pShaderModule) return;
    auto blob = std::make_shared<std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(pCreateInfo->pCode),
        reinterpret_cast<const uint8_t*>(pCreateInfo->pCode) + pCreateInfo->codeSize);
    Tracker& t = Tracker::Get();
    t.AddBlob(HT_VkShaderModule, (uint64_t)(uintptr_t)*pShaderModule, "SPIR-V", std::move(blob));
    // The stage, for the object list ("vertex shader, 12 KB SPIR-V").
    std::string stages = SpirvStages(pCreateInfo->pCode, pCreateInfo->codeSize / 4);
    uint64_t id = t.Resolve(HT_VkShaderModule, (uint64_t)(uintptr_t)*pShaderModule);
    if (!stages.empty() && id) {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("ObjectUpdate");
        w.Key("id"); w.Uint(id);
        w.Key("stage"); w.String(stages);
        w.EndObject();
        t.Update(id, "stage", w.str());
    }
}

static const char* StageName(VkShaderStageFlagBits stage) {
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

// Attaches each stage's SPIR-V to the pipeline. The stage may reference a module, or carry the
// code inline through VkShaderModuleCreateInfo in its pNext chain (VK_KHR_maintenance5).
static void AttachStage(VkPipeline pipeline, const VkPipelineShaderStageCreateInfo& stage) {
    std::string name = StageName(stage.stage);
    if (stage.pName) name += std::string(":") + stage.pName;
    if (stage.module) {
        auto blobs = Tracker::Get().GetBlobs(HT_VkShaderModule, (uint64_t)(uintptr_t)stage.module);
        for (auto& [n, data] : blobs) {
            if (n == "SPIR-V") {
                Tracker::Get().AddBlob(HT_VkPipeline, (uint64_t)(uintptr_t)pipeline, name, data);
                return;
            }
        }
    }
    for (auto* p = static_cast<const VkBaseInStructure*>(stage.pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            auto* ci = reinterpret_cast<const VkShaderModuleCreateInfo*>(p);
            if (ci->pCode && ci->codeSize) {
                auto blob = std::make_shared<std::vector<uint8_t>>(
                    reinterpret_cast<const uint8_t*>(ci->pCode),
                    reinterpret_cast<const uint8_t*>(ci->pCode) + ci->codeSize);
                Tracker::Get().AddBlob(HT_VkPipeline, (uint64_t)(uintptr_t)pipeline, name, std::move(blob));
            }
            return;
        }
    }
}

// Physical devices: what the GPU offers (properties and limits, memory heaps and types, queue
// families, features, extensions), attached to the VkPhysicalDevice object as an update so the
// Inspect tab (and capture files) can show it without further queries.
void Hook_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices) {
    if (!pPhysicalDevices || !pPhysicalDeviceCount) return;
    InstanceData* inst = GetInstanceData(instance);
    if (!inst) return;
    const InstanceDispatch& d = inst->dispatch;
    Tracker& t = Tracker::Get();
    for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i) {
        VkPhysicalDevice pd = pPhysicalDevices[i];
        if (!pd) continue;
        uint64_t id = t.Resolve(HT_VkPhysicalDevice, (uint64_t)(uintptr_t)pd);
        if (!id) continue;
        JsonWriter w(&t);
        w.BeginObject();
        w.Key("action"); w.String("ObjectUpdate");
        w.Key("id"); w.Uint(id);
        VkPhysicalDeviceProperties props{};
        d.GetPhysicalDeviceProperties(pd, &props);
        w.Key("properties"); ToJson(w, props);
        VkPhysicalDeviceMemoryProperties mem{};
        d.GetPhysicalDeviceMemoryProperties(pd, &mem);
        w.Key("memoryProperties"); ToJson(w, mem);
        uint32_t familyCount = 0;
        d.GetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        if (familyCount) d.GetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, families.data());
        w.Key("queueFamilies"); w.BeginArray();
        for (uint32_t f = 0; f < familyCount; ++f) ToJson(w, families[f]);
        w.EndArray();
        VkPhysicalDeviceFeatures features{};
        d.GetPhysicalDeviceFeatures(pd, &features);
        w.Key("features"); ToJson(w, features);
        uint32_t extCount = 0;
        w.Key("extensions"); w.BeginArray();
        if (d.EnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr) == VK_SUCCESS && extCount) {
            std::vector<VkExtensionProperties> exts(extCount);
            if (d.EnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data()) >= VK_SUCCESS) {
                for (uint32_t e = 0; e < extCount; ++e) ToJson(w, exts[e]);
            }
        }
        w.EndArray();
        w.EndObject();
        t.Update(id, "properties", w.str());
    }
}

void Hook_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
                                    const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    if (!pCreateInfos || !pPipelines) return;
    Tracker& t = Tracker::Get();
    DeviceData* dev = GetDeviceData(device);
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        if (!pPipelines[i]) continue;
        // What the driver's compiler made of each stage (shader_statistics.h).
        CollectShaderStatistics(dev, pPipelines[i]);
        const VkGraphicsPipelineCreateInfo& ci = pCreateInfos[i];
        for (uint32_t s = 0; s < ci.stageCount && ci.pStages; ++s) AttachStage(pPipelines[i], ci.pStages[s]);
        // A pipeline linked from graphics pipeline libraries has its shaders in them: their stages'
        // code (already gathered from any libraries of their own) is attached to it as well, so the
        // pipeline a draw binds shows every stage it runs.
        for (auto* n = static_cast<const VkBaseInStructure*>(ci.pNext); n; n = n->pNext) {
            if (n->sType != VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) continue;
            const auto* libs = reinterpret_cast<const VkPipelineLibraryCreateInfoKHR*>(n);
            auto own = t.GetBlobs(HT_VkPipeline, (uint64_t)(uintptr_t)pPipelines[i]);
            for (uint32_t l = 0; libs->pLibraries && l < libs->libraryCount; ++l) {
                for (auto& [name, data] : t.GetBlobs(HT_VkPipeline, (uint64_t)(uintptr_t)libs->pLibraries[l])) {
                    const std::string stage = name.substr(0, name.find(':'));
                    const bool present = std::any_of(own.begin(), own.end(), [&](const auto& b) { return b.first.substr(0, b.first.find(':')) == stage; });
                    if (present) continue;
                    t.AddBlob(HT_VkPipeline, (uint64_t)(uintptr_t)pPipelines[i], name, data);
                    own.emplace_back(name, data);
                }
            }
        }
    }
    ShaderEditor::Get().OnCreateGraphicsPipelines(device, createInfoCount, pCreateInfos, pPipelines);
}

void Hook_vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
                                   const VkComputePipelineCreateInfo* pCreateInfos,
                                   const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    if (!pCreateInfos || !pPipelines) return;
    DeviceData* dev = GetDeviceData(device);
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        if (!pPipelines[i]) continue;
        AttachStage(pPipelines[i], pCreateInfos[i].stage);
        CollectShaderStatistics(dev, pPipelines[i]);
    }
    ShaderEditor::Get().OnCreateComputePipelines(device, createInfoCount, pCreateInfos, pPipelines);
}

// Ray tracing pipelines: every stage's SPIR-V. A ray tracing pipeline usually has several stages of
// one kind (miss shaders, hit shaders), so each payload carries its index in pStages as well:
// "<stage>:<entry point>#<index>", which is what the shader groups refer to.
void Hook_vkCreateRayTracingPipelinesKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache,
                                         uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
                                         const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    if (!pCreateInfos || !pPipelines) return;
    Tracker& t = Tracker::Get();
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        if (!pPipelines[i]) continue;
        const VkRayTracingPipelineCreateInfoKHR& ci = pCreateInfos[i];
        for (uint32_t s = 0; s < ci.stageCount && ci.pStages; ++s) {
            const VkPipelineShaderStageCreateInfo& stage = ci.pStages[s];
            std::shared_ptr<std::vector<uint8_t>> code;
            if (stage.module) {
                for (auto& [n, data] : t.GetBlobs(HT_VkShaderModule, (uint64_t)(uintptr_t)stage.module))
                    if (n == "SPIR-V") code = data;
            }
            for (auto* p = static_cast<const VkBaseInStructure*>(stage.pNext); !code && p; p = p->pNext) {
                if (p->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
                auto* mci = reinterpret_cast<const VkShaderModuleCreateInfo*>(p);
                if (mci->pCode && mci->codeSize)
                    code = std::make_shared<std::vector<uint8_t>>(reinterpret_cast<const uint8_t*>(mci->pCode),
                                                                 reinterpret_cast<const uint8_t*>(mci->pCode) + mci->codeSize);
            }
            if (!code) continue;
            std::string name = std::string(StageName(stage.stage)) + ":" + (stage.pName ? stage.pName : "main") + "#" + std::to_string(s);
            t.AddBlob(HT_VkPipeline, (uint64_t)(uintptr_t)pPipelines[i], name, code);
        }
    }
}

// Acceleration structures: what the last build of each put in it (its geometries and primitive
// counts), as an update on the structure, so the object shows what it holds without its commands.


// ---------------------------------------------------------------------------------------------
// The shader binding table
//
// A trace does not name the shaders it runs. It names a table in memory, and each record of that
// table holds an opaque handle the driver gave for one of the pipeline's shader groups. So saying
// which group a record runs needs both halves: the handles, kept as a blob on the pipeline, and the
// table's contents, read back from the addresses the trace points at.

void Hook_vkGetRayTracingShaderGroupHandlesKHR(VkDevice device, VkPipeline pipeline, uint32_t firstGroup,
                                               uint32_t groupCount, size_t dataSize, void* pData) {
    (void)device;
    if (!pipeline || !pData || !dataSize || !groupCount) return;
    // Only the whole table from the start is kept: an application that asks for a slice would need
    // the pieces stitched, and every one seen so far asks for all of them at once.
    if (firstGroup != 0) return;
    auto blob = std::make_shared<std::vector<uint8_t>>((const uint8_t*)pData, (const uint8_t*)pData + dataSize);
    Tracker::Get().AddBlob(HT_VkPipeline, (uint64_t)(uintptr_t)pipeline, "group handles", std::move(blob));
    // The handle size the records are laid out by, which the table's stride is a multiple of.
    const uint64_t id = Tracker::Get().Resolve(HT_VkPipeline, (uint64_t)(uintptr_t)pipeline);
    if (!id) return;
    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("shaderGroupHandles"); w.BeginObject();
    w.Key("count"); w.Uint(groupCount);
    w.Key("handleSize"); w.Uint(dataSize / groupCount);
    w.EndObject();
    w.EndObject();
    Tracker::Get().Update(id, "shaderGroupHandles", w.str());
}

void Hook_vkCmdTraceRaysKHR(VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
                            const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable,
                            const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
                            const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable,
                            uint32_t width, uint32_t height, uint32_t depth) {
    (void)width; (void)height; (void)depth;
    DeviceData* dev = GetDeviceData(commandBuffer);
    CommandRecorder* rec = dev ? dev->RecorderFor(commandBuffer) : nullptr;
    if (!rec || !CaptureManager::Get().IsCapturing()) return;
    const struct { const char* name; const VkStridedDeviceAddressRegionKHR* region; } regions[] = {
        {"raygen", pRaygenShaderBindingTable}, {"miss", pMissShaderBindingTable},
        {"hit", pHitShaderBindingTable}, {"callable", pCallableShaderBindingTable},
    };
    std::string extra;
    uint32_t captured = 0;
    for (const auto& r : regions) {
        if (!r.region || !r.region->deviceAddress || !r.region->size) continue;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, remaining = 0;
        if (!ResourceRegistry::Get().ResolveAddress(r.region->deviceAddress, buffer, offset, remaining)) continue;
        const VkDeviceSize size = r.region->size < remaining ? r.region->size : remaining;
        const uint32_t id = CaptureManager::Get().QueueBufferCapture(dev, rec, buffer, offset, size);
        if (!id) continue;
        extra += captured++ ? "," : "";
        extra += std::string("{\"region\":\"") + r.name + "\",\"capture\":" + std::to_string(id) + "}";
    }
    if (captured) rec->SetExtraOnLast(",\"bindingTableData\":[" + extra + "]");
}

// ---------------------------------------------------------------------------------------------
// Device addresses (src/vulkan/src/resources.h)
//
// A ray tracing build names the geometry it reads by device address, so these are what make a build
// say anything about what it built. The address is the return value, which is why these are
// RESULT_HOOKS rather than ordinary ones.

void Hook_vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo, VkDeviceAddress result) {
    (void)device;
    if (pInfo) ResourceRegistry::Get().NoteBufferAddress(pInfo->buffer, result);
}

void Hook_vkGetBufferDeviceAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo* pInfo, VkDeviceAddress result) {
    Hook_vkGetBufferDeviceAddress(device, pInfo, result);
}

void Hook_vkGetBufferDeviceAddressEXT(VkDevice device, const VkBufferDeviceAddressInfo* pInfo, VkDeviceAddress result) {
    Hook_vkGetBufferDeviceAddress(device, pInfo, result);
}

void Hook_vkGetAccelerationStructureDeviceAddressKHR(VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* pInfo,
                                                     VkDeviceAddress result) {
    (void)device;
    if (!pInfo) return;
    // How a top level names the bottom levels under it: every instance holds one of these.
    ResourceRegistry::Get().NoteStructureAddress(pInfo->accelerationStructure, result);
    // Also on the structure itself, so the UI can turn an instance's reference back into the
    // object it names — the only link there is from a top level to what is under it.
    Tracker& t = Tracker::Get();
    const uint64_t id = t.Resolve(HT_VkAccelerationStructureKHR, (uint64_t)(uintptr_t)pInfo->accelerationStructure);
    if (!id) return;
    JsonWriter w(&t);
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("deviceAddress"); w.Uint(result);
    w.EndObject();
    t.Update(id, "deviceAddress", w.str());
}


/**
 * One of a build's input addresses: where it points, and the contents if they could be captured.
 *
 * A build reads its geometry from device addresses, and an address on its own says nothing — it is
 * not a handle, and it means nothing outside the process that made it. Resolving it to the buffer
 * that owns it is what lets the capture read the vertices, indices and instances a structure was
 * actually built from, which is the only view there is of an otherwise opaque object.
 */
/** What an address resolved to, for the command's own record of it (see the note in NoteAccelerationStructureBuilds). */
struct ResolvedAddress {
    uint64_t buffer = 0;        // the buffer's object id, 0 when the address resolved to none
    VkDeviceSize offset = 0;
    uint32_t capture = 0;       // the contents read back, 0 when none were
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;      // what the build reads, within the buffer
};

static ResolvedAddress WriteBuildAddress(JsonWriter& w, const char* key, VkDeviceAddress address, VkDeviceSize size,
                                         DeviceData* dev, CommandRecorder* rec) {
    w.Key(key); w.BeginObject();
    w.Key("deviceAddress"); w.Uint(address);
    ResolvedAddress out;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, remaining = 0;
    if (address && ResourceRegistry::Get().ResolveAddress(address, buffer, offset, remaining)) {
        out.buffer = Tracker::Get().Resolve(HT_VkBuffer, (uint64_t)(uintptr_t)buffer);
        out.offset = offset;
        w.Key("buffer"); w.Handle(HT_VkBuffer, "VkBuffer", (uint64_t)(uintptr_t)buffer);
        w.Key("offset"); w.Uint(offset);
        // A size the build implies can run past the buffer when the application over-declared it.
        if (size > remaining) size = remaining;
        out.address = address;
        out.size = size;
        if (rec && size) out.capture = CaptureManager::Get().QueueBufferCapture(dev, rec, buffer, offset, size);
        if (out.capture) { w.Key("capture"); w.Uint(out.capture); }
    }
    w.EndObject();
    return out;
}

static void NoteAccelerationStructureBuilds(const char* method, uint32_t infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* infos,
                                            const VkAccelerationStructureBuildRangeInfoKHR* const* ranges, const uint32_t* const* maxPrimitiveCounts,
                                            VkCommandBuffer commandBuffer) {
    if (!infos) return;
    Tracker& t = Tracker::Get();
    // A host build (vkBuildAccelerationStructuresKHR) has no command buffer, so its inputs cannot
    // be read back the way a recorded build's can; the addresses are still resolved and named.
    DeviceData* dev = commandBuffer ? GetDeviceData(commandBuffer) : nullptr;
    CommandRecorder* rec = dev ? dev->RecorderFor(commandBuffer) : nullptr;

    // The capture ids also go on the recorded command, not only on the structure. The structure's
    // update is last-write-wins, and an application that rebuilds its top level every frame — the
    // usual thing — overwrites the captured build's ids with a later build's, which has none
    // because nothing was capturing then. The command belongs to the captured frame and keeps them.
    std::string captures;
    uint32_t captured = 0;
    // Every geometry's range, per info. The arguments carry only each info's first: a range array's
    // length is the info's geometryCount, which the generated serializer does not follow.
    std::string buildRanges;
    for (uint32_t i = 0; i < infoCount; ++i) {
        buildRanges += i ? ",[" : "[";
        for (uint32_t g = 0; ranges && ranges[i] && g < infos[i].geometryCount; ++g) {
            const VkAccelerationStructureBuildRangeInfoKHR& r = ranges[i][g];
            buildRanges += (g ? ",{" : "{") + std::string("\"primitiveCount\":") + std::to_string(r.primitiveCount)
                         + ",\"primitiveOffset\":" + std::to_string(r.primitiveOffset) + ",\"firstVertex\":" + std::to_string(r.firstVertex)
                         + ",\"transformOffset\":" + std::to_string(r.transformOffset) + "}";
        }
        buildRanges += "]";
    }
    for (uint32_t i = 0; i < infoCount; ++i) {
        const VkAccelerationStructureBuildGeometryInfoKHR& info = infos[i];
        const uint64_t id = t.Resolve(HT_VkAccelerationStructureKHR, (uint64_t)(uintptr_t)info.dstAccelerationStructure);
        if (!id) continue;
        JsonWriter w(&t);
        w.BeginObject();
        w.Key("action"); w.String("ObjectUpdate");
        w.Key("id"); w.Uint(id);
        w.Key("build"); w.BeginObject();
        w.Key("method"); w.String(method);
        w.Key("type"); w.Enum(ToString_VkAccelerationStructureTypeKHR(info.type), (int64_t)info.type);
        w.Key("mode"); w.Enum(ToString_VkBuildAccelerationStructureModeKHR(info.mode), (int64_t)info.mode);
        w.Key("flags"); Flags_VkBuildAccelerationStructureFlagsKHR(w, info.flags);
        uint64_t primitives = 0;
        w.Key("geometries"); w.BeginArray();
        uint32_t g = 0;
        std::vector<ResourceRegistry::StructureInput> inputs;
        std::vector<uint32_t> counts(info.geometryCount, 0);
        // Every address the layer resolved, on the command: the buffer and offset are what lets a
        // replay turn the captured process's address into one of its own (docs/REPLAY.md), and the
        // capture id is the contents for the UI. On the command rather than on the structure because
        // a structure's update is last-write-wins (see above).
        auto note = [&](const char* what, const ResolvedAddress& r) {
            if (!r.buffer) return;
            if (r.size) inputs.push_back({what, g, r.address, r.size});
            captures += captured++ ? "," : "";
            captures += "{\"info\":" + std::to_string(i) + ",\"geometry\":" + std::to_string(g)
                      + ",\"field\":\"" + what + "\",\"buffer\":" + std::to_string(r.buffer)
                      + ",\"offset\":" + std::to_string(r.offset);
            if (r.capture) captures += ",\"capture\":" + std::to_string(r.capture);
            captures += "}";
        };
        for (; g < info.geometryCount; ++g) {
            const VkAccelerationStructureGeometryKHR* geometry = info.pGeometries ? &info.pGeometries[g]
                                                               : info.ppGeometries ? info.ppGeometries[g] : nullptr;
            if (!geometry) continue;
            const uint32_t count = ranges && ranges[i] ? ranges[i][g].primitiveCount
                                 : maxPrimitiveCounts && maxPrimitiveCounts[i] ? maxPrimitiveCounts[i][g] : 0;
            primitives += count;
            counts[g] = count;
            w.BeginObject();
            w.Key("geometryType"); w.Enum(ToString_VkGeometryTypeKHR(geometry->geometryType), (int64_t)geometry->geometryType);
            w.Key("flags"); Flags_VkGeometryFlagsKHR(w, geometry->flags);
            w.Key("primitiveCount"); w.Uint(count);
            if (geometry->geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
                const auto& tri = geometry->geometry.triangles;
                w.Key("vertexFormat"); w.Enum(ToString_VkFormat(tri.vertexFormat), (int64_t)tri.vertexFormat);
                w.Key("vertexStride"); w.Uint(tri.vertexStride);
                w.Key("maxVertex"); w.Uint(tri.maxVertex);
                w.Key("indexType"); w.Enum(ToString_VkIndexType(tri.indexType), (int64_t)tri.indexType);
                // maxVertex is the highest index the build may read, so the array holds one more.
                note("vertexData", WriteBuildAddress(w, "vertexData", tri.vertexData.deviceAddress,
                                  (VkDeviceSize)(tri.maxVertex + 1) * tri.vertexStride, dev, rec));
                if (tri.indexType != VK_INDEX_TYPE_NONE_KHR) {
                    const VkDeviceSize indexSize = tri.indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
                    note("indexData", WriteBuildAddress(w, "indexData", tri.indexData.deviceAddress,
                                      (VkDeviceSize)count * 3 * indexSize, dev, rec));
                }
                if (tri.transformData.deviceAddress) {
                    note("transformData", WriteBuildAddress(w, "transformData", tri.transformData.deviceAddress,
                                      sizeof(VkTransformMatrixKHR), dev, rec));
                }
            } else if (geometry->geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
                const auto& aabbs = geometry->geometry.aabbs;
                w.Key("stride"); w.Uint(aabbs.stride);
                note("data", WriteBuildAddress(w, "data", aabbs.data.deviceAddress, (VkDeviceSize)count * aabbs.stride, dev, rec));
            } else if (geometry->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
                const auto& instances = geometry->geometry.instances;
                w.Key("arrayOfPointers"); w.Boolean(instances.arrayOfPointers == VK_TRUE);
                // An array of pointers is a list of addresses rather than of instances; its size is
                // per pointer, and what they point at is not followed.
                const VkDeviceSize stride = instances.arrayOfPointers ? sizeof(VkDeviceAddress)
                                                                      : sizeof(VkAccelerationStructureInstanceKHR);
                note("data", WriteBuildAddress(w, "data", instances.data.deviceAddress, (VkDeviceSize)count * stride, dev, rec));
            }
            w.EndObject();
        }
        w.EndArray();
        w.Key("primitiveCount"); w.Uint(primitives);
        // What the driver says the build needs, which is what the structure costs in memory.
        if (dev && dev->dispatch.GetAccelerationStructureBuildSizesKHR) {
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            dev->dispatch.GetAccelerationStructureBuildSizesKHR(dev->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                                &info, counts.data(), &sizes);
            if (sizes.accelerationStructureSize) {
                w.Key("resultSize"); w.Uint(sizes.accelerationStructureSize);
                w.Key("scratchSize"); w.Uint(info.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? sizes.updateScratchSize : sizes.buildScratchSize);
            }
        }
        w.EndObject();
        w.EndObject();
        t.Update(id, "build", w.str());
        // A host build's addresses are host pointers, which a later capture has no way to read.
        if (dev) {
            CaptureManager& cm = CaptureManager::Get();
            ResourceRegistry::Get().NoteStructureInputs(info.dstAccelerationStructure, dev->device, id,
                                                        cm.IsCapturing() ? cm.CaptureSerial() : 0, std::move(inputs));
        }
    }
    if (rec) {
        std::string extra = ranges ? ",\"buildRanges\":[" + buildRanges + "]" : std::string();
        if (captured) extra += ",\"buildData\":[" + captures + "]";
        if (!extra.empty()) rec->SetExtraOnLast(std::move(extra));
    }
}

void Hook_vkCmdBuildAccelerationStructuresKHR(VkCommandBuffer commandBuffer, uint32_t infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
                                              const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
    NoteAccelerationStructureBuilds("vkCmdBuildAccelerationStructuresKHR", infoCount, pInfos, ppBuildRangeInfos, nullptr, commandBuffer);
}

void Hook_vkCmdBuildAccelerationStructuresIndirectKHR(VkCommandBuffer commandBuffer, uint32_t infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
                                                      const VkDeviceAddress* pIndirectDeviceAddresses, const uint32_t* pIndirectStrides,
                                                      const uint32_t* const* ppMaxPrimitiveCounts) {
    NoteAccelerationStructureBuilds("vkCmdBuildAccelerationStructuresIndirectKHR", infoCount, pInfos, nullptr, ppMaxPrimitiveCounts,
                                   commandBuffer);
}

void Hook_vkBuildAccelerationStructuresKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, uint32_t infoCount,
                                           const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
                                           const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
    // A host build has no command buffer: its inputs cannot be read back (see the note above).
    NoteAccelerationStructureBuilds("vkBuildAccelerationStructuresKHR", infoCount, pInfos, ppBuildRangeInfos, nullptr,
                                   VK_NULL_HANDLE);
}

// A shader object's SPIR-V, attached to it as "<stage>:<entry point>" like a pipeline's stages.
void Hook_vkCreateShadersEXT(VkDevice device, uint32_t createInfoCount, const VkShaderCreateInfoEXT* pCreateInfos,
                             const VkAllocationCallbacks* pAllocator, VkShaderEXT* pShaders) {
    if (!pCreateInfos || !pShaders) return;
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        const VkShaderCreateInfoEXT& ci = pCreateInfos[i];
        if (!pShaders[i] || ci.codeType != VK_SHADER_CODE_TYPE_SPIRV_EXT || !ci.pCode || !ci.codeSize) continue;
        auto blob = std::make_shared<std::vector<uint8_t>>(static_cast<const uint8_t*>(ci.pCode),
                                                          static_cast<const uint8_t*>(ci.pCode) + ci.codeSize);
        std::string name = std::string(StageName(ci.stage)) + ":" + (ci.pName ? ci.pName : "main");
        Tracker::Get().AddBlob(HT_VkShaderEXT, (uint64_t)(uintptr_t)pShaders[i], name, std::move(blob));
    }
    ShaderEditor::Get().OnCreateShaders(device, createInfoCount, pCreateInfos, pShaders);
}

// =============================================================================================
// Memory bindings

static void SendBinding(HandleType type, uint64_t handle, VkDeviceMemory memory, VkDeviceSize offset) {
    Tracker& t = Tracker::Get();
    uint64_t id = t.Resolve(type, handle);
    if (!id) return;
    JsonWriter w(&t);
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("memory"); w.Handle(HT_VkDeviceMemory, "VkDeviceMemory", (uint64_t)(uintptr_t)memory);
    w.Key("memoryOffset"); w.Uint(offset);
    w.EndObject();
    t.Update(id, "memory", w.str());
}

void Hook_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    SendBinding(HT_VkBuffer, (uint64_t)(uintptr_t)buffer, memory, memoryOffset);
    ResourceRegistry::Get().NoteBufferMemory(buffer, memory, memoryOffset);
}

void Hook_vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    SendBinding(HT_VkImage, (uint64_t)(uintptr_t)image, memory, memoryOffset);
}

void Hook_vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos) {
    for (uint32_t i = 0; pBindInfos && i < bindInfoCount; ++i) {
        SendBinding(HT_VkBuffer, (uint64_t)(uintptr_t)pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        ResourceRegistry::Get().NoteBufferMemory(pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
    }
}

void Hook_vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo* pBindInfos) {
    for (uint32_t i = 0; pBindInfos && i < bindInfoCount; ++i)
        SendBinding(HT_VkImage, (uint64_t)(uintptr_t)pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
}

void Hook_vkBindBufferMemory2KHR(VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos) {
    Hook_vkBindBufferMemory2(device, bindInfoCount, pBindInfos);
}

void Hook_vkBindImageMemory2KHR(VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo* pBindInfos) {
    Hook_vkBindImageMemory2(device, bindInfoCount, pBindInfos);
}

// =============================================================================================
// Frame capture: command buffers, submits, passes

void Hook_vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    CaptureManager::Get().OnEndCommandBuffer(GetDeviceData(commandBuffer), commandBuffer);
}

void Hook_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
                               const VkCommandBuffer* pCommandBuffers) {
    DeviceData* dev = GetDeviceData(device);
    for (uint32_t i = 0; pCommandBuffers && i < commandBufferCount; ++i) {
        CaptureManager::Get().OnFreeCommandBuffer(dev, pCommandBuffers[i]);
        LayoutTracker::Get().OnFreeCommandBuffer(pCommandBuffers[i]);
    }
    if (dev && dev->inheritedQueries && pCommandBuffers) {
        std::unique_lock lock(dev->secondariesMutex);
        for (uint32_t i = 0; i < commandBufferCount; ++i) dev->secondaries.erase(pCommandBuffers[i]);
    }
}

void Hook_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue) {
    if (!pQueue || !*pQueue) return;
    DeviceData* dev = GetDeviceData(device);
    std::lock_guard lock(dev->queueMutex);
    dev->queueFamilies[*pQueue] = queueFamilyIndex;
}

void Hook_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue) {
    if (!pQueueInfo || !pQueue || !*pQueue) return;
    DeviceData* dev = GetDeviceData(device);
    std::lock_guard lock(dev->queueMutex);
    dev->queueFamilies[*pQueue] = pQueueInfo->queueFamilyIndex;
}

// Image layout tracking (see image_readback.h): barriers and render pass final layouts.
void Hook_vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                               VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                               uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
                               uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers,
                               uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers) {
    for (uint32_t i = 0; pImageMemoryBarriers && i < imageMemoryBarrierCount; ++i)
        LayoutTracker::Get().NoteTransition(commandBuffer, pImageMemoryBarriers[i].image, pImageMemoryBarriers[i].newLayout);
}

void Hook_vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo) {
    if (!pDependencyInfo) return;
    for (uint32_t i = 0; pDependencyInfo->pImageMemoryBarriers && i < pDependencyInfo->imageMemoryBarrierCount; ++i)
        LayoutTracker::Get().NoteTransition(commandBuffer, pDependencyInfo->pImageMemoryBarriers[i].image,
                                            pDependencyInfo->pImageMemoryBarriers[i].newLayout);
}

void Hook_vkCmdPipelineBarrier2KHR(VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo) {
    Hook_vkCmdPipelineBarrier2(commandBuffer, pDependencyInfo);
}

static void NotePassFinalLayouts(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* info) {
    if (!info) return;
    ResourceRegistry& reg = ResourceRegistry::Get();
    FramebufferInfo fb;
    RenderPassInfo rp;
    if (!reg.GetFramebuffer(info->framebuffer, fb) || !reg.GetRenderPass(info->renderPass, rp)) return;
    const std::vector<VkImageView> views = PassAttachmentViews(info, fb);
    for (size_t i = 0; i < views.size() && i < rp.attachments.size(); ++i) {
        ImageViewInfo vi;
        if (reg.GetImageView(views[i], vi))
            LayoutTracker::Get().NoteTransition(commandBuffer, vi.image, rp.attachments[i].finalLayout);
    }
}

static void NoteRenderingLayouts(VkCommandBuffer commandBuffer, const VkRenderingInfo* info) {
    if (!info) return;
    ResourceRegistry& reg = ResourceRegistry::Get();
    auto note = [&](const VkRenderingAttachmentInfo* a) {
        ImageViewInfo vi;
        if (a && a->imageView && reg.GetImageView(a->imageView, vi))
            LayoutTracker::Get().NoteTransition(commandBuffer, vi.image, a->imageLayout);
    };
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) note(&info->pColorAttachments[i]);
    note(info->pDepthAttachment);
    note(info->pStencilAttachment);
}

void Hook_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    SubmitEnd(queue);
    // VKINSP_SIMULATE_DEVICE_LOST: exercise the diagnosis without hanging the GPU. The queue is
    // deliberately not drained first, so the markers are read with work still in flight, which is
    // the state a real loss leaves them in.
    bool vkinsp_pretendHung = false;
    if (DeviceData* dev = GetDeviceData(queue); SimulateDeviceLost(dev, &vkinsp_pretendHung))
        OnDeviceLost(dev, "vkQueueSubmit (simulated)", vkinsp_pretendHung);
    // A split submission (PreHook_vkQueueSubmit) is recorded as the application made it.
    SplitSubmit split = std::move(t_split);
    t_split = SplitSubmit{};
    if (split.active && split.submits) {
        submitCount = split.count;
        pSubmits = split.submits;
    }
    OnSubmitForFrames(GetDeviceData(queue), queue);
    std::vector<VkCommandBuffer> cbs;
    for (uint32_t i = 0; pSubmits && i < submitCount; ++i)
        for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; ++j) cbs.push_back(pSubmits[i].pCommandBuffers[j]);
    LayoutTracker::Get().OnSubmit((uint32_t)cbs.size(), cbs.data());
    if (!CaptureManager::Get().IsCapturing()) return;
    JsonWriter w(&Tracker::Get());
    ArgsToJson_vkQueueSubmit(w, queue, submitCount, pSubmits, fence);
    CaptureManager::Get().OnSubmit(GetDeviceData(queue), queue, "vkQueueSubmit", std::move(w.str()), 0, cbs, split.readBack);
}

void Hook_vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    SubmitEnd(queue);
    SplitSubmit split = std::move(t_split);
    t_split = SplitSubmit{};
    if (split.active && split.submits2) {
        submitCount = split.count;
        pSubmits = split.submits2;
    }
    OnSubmitForFrames(GetDeviceData(queue), queue);
    std::vector<VkCommandBuffer> cbs;
    for (uint32_t i = 0; pSubmits && i < submitCount; ++i)
        for (uint32_t j = 0; j < pSubmits[i].commandBufferInfoCount; ++j)
            cbs.push_back(pSubmits[i].pCommandBufferInfos[j].commandBuffer);
    LayoutTracker::Get().OnSubmit((uint32_t)cbs.size(), cbs.data());
    if (!CaptureManager::Get().IsCapturing()) return;
    JsonWriter w(&Tracker::Get());
    ArgsToJson_vkQueueSubmit2(w, queue, submitCount, pSubmits, fence);
    CaptureManager::Get().OnSubmit(GetDeviceData(queue), queue, "vkQueueSubmit2", std::move(w.str()), 0, cbs, split.readBack);
}

void Hook_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    Hook_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

void Hook_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                               VkSubpassContents contents) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    AttachPendingData(commandBuffer);
    pRenderPassBegin = OriginalBegin(pRenderPassBegin);
    NotePassFinalLayouts(commandBuffer, pRenderPassBegin);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnBeginRenderPass(dev, rec, pRenderPassBegin);
}

void Hook_vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                                const VkSubpassBeginInfo* pSubpassBeginInfo) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    AttachPendingData(commandBuffer);
    pRenderPassBegin = OriginalBegin(pRenderPassBegin);
    NotePassFinalLayouts(commandBuffer, pRenderPassBegin);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnBeginRenderPass(dev, rec, pRenderPassBegin);
}

void Hook_vkCmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                                   const VkSubpassBeginInfo* pSubpassBeginInfo) {
    Hook_vkCmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

void Hook_vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    AttachPendingData(commandBuffer);
    pRenderingInfo = OriginalRendering(pRenderingInfo);
    NoteRenderingLayouts(commandBuffer, pRenderingInfo);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnBeginRendering(dev, rec, pRenderingInfo);
}

void Hook_vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo) {
    Hook_vkCmdBeginRendering(commandBuffer, pRenderingInfo);
}

static void EndPass(VkCommandBuffer commandBuffer) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnEndPass(dev, rec);
}

void Hook_vkCmdEndRenderPass(VkCommandBuffer commandBuffer) { EndPass(commandBuffer); }
void Hook_vkCmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo) { EndPass(commandBuffer); }
void Hook_vkCmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo) { EndPass(commandBuffer); }
void Hook_vkCmdEndRendering(VkCommandBuffer commandBuffer) { EndPass(commandBuffer); }
void Hook_vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer) { EndPass(commandBuffer); }

// Secondary command buffers: embed their (already recorded) commands under the execute command.
void Hook_vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount,
                               const VkCommandBuffer* pCommandBuffers) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    LayoutTracker::Get().OnExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
    CommandRecorder* rec = dev->RecorderFor(commandBuffer);
    if (!rec || !pCommandBuffers) return;
    std::string extra = ",\"children\":[";
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        if (i) extra += ',';
        uint64_t id = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)pCommandBuffers[i]);
        extra += "{\"commandBuffer\":" + std::to_string(id) + ",\"commands\":[";
        if (CommandRecorder* sec = dev->RecorderFor(pCommandBuffers[i])) {
            auto snap = sec->Snapshot();
            bool first = true;
            size_t slot = 0;
            for (auto& c : *snap) {
                if (!first) extra += ',';
                first = false;
                extra += "{\"slot\":" + std::to_string(slot++) + ",\"method\":\"";
                extra += kVkCommandNames[(int)c.id];
                extra += "\",\"args\":";
                extra += c.args.empty() ? "null" : c.args;
                if (!c.extra.empty()) extra += c.extra;
                extra += '}';
            }
        }
        extra += "]}";
    }
    extra += ']';
    rec->SetExtraOnLast(std::move(extra));
    CaptureManager::Get().OnExecuteCommands(dev, rec, commandBufferCount, pCommandBuffers);
}

// =============================================================================================
// Descriptor sets: contents tracking, and snapshots + buffer readbacks when they are bound

void Hook_vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                                      const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout) {
    if (pSetLayout && *pSetLayout) DescriptorTracker::Get().OnCreateLayout(*pSetLayout, pCreateInfo);
}

void Hook_vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo,
                                   VkDescriptorSet* pDescriptorSets) {
    DescriptorTracker::Get().OnAllocateSets(pAllocateInfo, pDescriptorSets);
}

void Hook_vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites,
                                 uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies) {
    DescriptorTracker::Get().OnUpdateSets(descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

void Hook_vkCreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator,
                                           VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    if (pDescriptorUpdateTemplate && *pDescriptorUpdateTemplate)
        DescriptorTracker::Get().OnCreateTemplate(*pDescriptorUpdateTemplate, pCreateInfo);
}

void Hook_vkCreateDescriptorUpdateTemplateKHR(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator,
                                              VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    Hook_vkCreateDescriptorUpdateTemplate(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}

void Hook_vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet descriptorSet,
                                            VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pData) {
    DescriptorTracker::Get().OnUpdateWithTemplate(descriptorSet, descriptorUpdateTemplate, pData);
}

void Hook_vkUpdateDescriptorSetWithTemplateKHR(VkDevice device, VkDescriptorSet descriptorSet,
                                               VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pData) {
    DescriptorTracker::Get().OnUpdateWithTemplate(descriptorSet, descriptorUpdateTemplate, pData);
}

// Queues readbacks of the buffers in a set's bindings; returns the capture ids in the bindings'
// shape. Dynamic offsets are consumed in binding order, like WriteDescriptorSetJson does.
static std::vector<std::vector<uint32_t>> CaptureSetBuffers(DeviceData* dev, CommandRecorder* rec,
                                                            const DescriptorSetContents& c, const uint32_t* dynamicOffsets,
                                                            uint32_t dynamicOffsetCount, uint32_t& dynamicIndex) {
    std::vector<std::vector<uint32_t>> ids(c.bindings.size());
    for (size_t bi = 0; bi < c.bindings.size(); ++bi) {
        const DescriptorBinding& b = c.bindings[bi];
        ids[bi].assign(b.entries.size(), 0);
        for (size_t k = 0; k < b.entries.size(); ++k) {
            const DescriptorEntry& e = b.entries[k];
            uint32_t dyn = 0;
            if (IsDynamicDescriptor(b.type)) {
                dyn = dynamicOffsets && dynamicIndex < dynamicOffsetCount ? dynamicOffsets[dynamicIndex] : 0;
                dynamicIndex++;
            }
            if (!e.written) continue;
            if (IsBufferDescriptor(b.type) && e.buffer) {
                ids[bi][k] = CaptureManager::Get().QueueBufferCapture(dev, rec, e.buffer, e.offset + dyn, DescriptorBufferRange(e));
            } else if (IsImageDescriptor(b.type) && e.imageView) {
                // Sampled / storage images: read back once per view so the capture shows what was sampled.
                ids[bi][k] = CaptureManager::Get().QueueImageCapture(dev, rec, e.imageView, e.imageLayout);
            }
        }
    }
    return ids;
}

static VkPipelineBindPoint BindPointFromStages(VkShaderStageFlags stages) {
    if (stages & VK_SHADER_STAGE_COMPUTE_BIT) return VK_PIPELINE_BIND_POINT_COMPUTE;
    if (stages & (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                  VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR))
        return VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
    return VK_PIPELINE_BIND_POINT_GRAPHICS;
}

// Attaches {"descriptors": {"bindPoint", "sets": [...]}} to the bind command just recorded.
static void SnapshotBoundSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, uint32_t firstSet,
                              uint32_t setCount, const VkDescriptorSet* sets, uint32_t dynamicOffsetCount,
                              const uint32_t* dynamicOffsets) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    CommandRecorder* rec = dev->RecorderFor(commandBuffer);
    if (!rec || !sets) return;
    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("bindPoint"); w.Enum(ToString_VkPipelineBindPoint(bindPoint), (int64_t)bindPoint);
    w.Key("sets"); w.BeginArray();
    uint32_t dynamicIndex = 0;
    for (uint32_t i = 0; i < setCount; ++i) {
        DescriptorSetContents c;
        if (!DescriptorTracker::Get().GetSet(sets[i], c)) {
            w.BeginObject();
            w.Key("set"); w.Uint(firstSet + i);
            w.Key("descriptorSet"); w.Handle(HT_VkDescriptorSet, "VkDescriptorSet", (uint64_t)(uintptr_t)sets[i]);
            w.Key("bindings"); w.BeginArray(); w.EndArray();
            w.EndObject();
            continue;
        }
        uint32_t captureDynamicIndex = dynamicIndex;
        auto ids = CaptureSetBuffers(dev, rec, c, dynamicOffsets, dynamicOffsetCount, captureDynamicIndex);
        WriteDescriptorSetJson(w, firstSet + i, sets[i], c, dynamicOffsets, dynamicOffsetCount, dynamicIndex, &ids);
    }
    w.EndArray();
    w.EndObject();
    rec->SetExtraOnLast(",\"descriptors\":" + w.str());
}

void Hook_vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
                                  uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets,
                                  uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
    SnapshotBoundSets(commandBuffer, pipelineBindPoint, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount,
                      pDynamicOffsets);
}

void Hook_vkCmdBindDescriptorSets2(VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo* pBindDescriptorSetsInfo) {
    const VkBindDescriptorSetsInfo* i = pBindDescriptorSetsInfo;
    if (!i) return;
    SnapshotBoundSets(commandBuffer, BindPointFromStages(i->stageFlags), i->firstSet, i->descriptorSetCount,
                      i->pDescriptorSets, i->dynamicOffsetCount, i->pDynamicOffsets);
}

void Hook_vkCmdBindDescriptorSets2KHR(VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo* pBindDescriptorSetsInfo) {
    Hook_vkCmdBindDescriptorSets2(commandBuffer, pBindDescriptorSetsInfo);
}

static void SnapshotPushedContents(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, uint32_t set,
                                   const DescriptorSetContents& c);

static void SnapshotPushedSet(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, uint32_t set,
                              uint32_t writeCount, const VkWriteDescriptorSet* writes) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (!dev->RecorderFor(commandBuffer)) return;
    SnapshotPushedContents(commandBuffer, bindPoint, set, DescriptorTracker::FromWrites(writeCount, writes));
}

static void SnapshotPushedTemplate(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate tmpl, uint32_t set, const void* data) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (!dev->RecorderFor(commandBuffer)) return;
    DescriptorSetContents c;
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (DescriptorTracker::Get().FromTemplate(tmpl, data, c, bindPoint)) SnapshotPushedContents(commandBuffer, bindPoint, set, c);
}

// Attaches {"descriptors": {...}} for a push descriptor set, which has no set object of its own.
static void SnapshotPushedContents(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, uint32_t set,
                                   const DescriptorSetContents& c) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    CommandRecorder* rec = dev->RecorderFor(commandBuffer);
    if (!rec) return;
    uint32_t dynamicIndex = 0;
    auto ids = CaptureSetBuffers(dev, rec, c, nullptr, 0, dynamicIndex);
    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("bindPoint"); w.Enum(ToString_VkPipelineBindPoint(bindPoint), (int64_t)bindPoint);
    w.Key("sets"); w.BeginArray();
    dynamicIndex = 0;
    WriteDescriptorSetJson(w, set, VK_NULL_HANDLE, c, nullptr, 0, dynamicIndex, &ids);
    w.EndArray();
    w.EndObject();
    rec->SetExtraOnLast(",\"descriptors\":" + w.str());
}

// ---------------------------------------------------------------------------------------------
// Descriptor buffers (src/vulkan/src/descriptor_buffer.h).

void Hook_vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout) {
    if (pPipelineLayout) DescriptorTracker::Get().OnCreatePipelineLayout(*pPipelineLayout, pCreateInfo);
}

void Hook_vkGetDescriptorEXT(VkDevice device, const VkDescriptorGetInfoEXT* pDescriptorInfo, size_t dataSize,
                             void* pDescriptor) {
    DescriptorBufferTracker::Get().OnGetDescriptor(pDescriptorInfo, dataSize, pDescriptor);
}

void Hook_vkCmdBindDescriptorBuffersEXT(VkCommandBuffer commandBuffer, uint32_t bufferCount,
                                        const VkDescriptorBufferBindingInfoEXT* pBindingInfos) {
    DescriptorBufferTracker::Get().OnBindBuffers(commandBuffer, bufferCount, pBindingInfos);
}

/**
 * Attaches the same "descriptors" snapshot a bound set produces, read out of the descriptor buffer.
 *
 * This is where a descriptor buffer's sets become bound, so it is the counterpart of
 * vkCmdBindDescriptorSets and snapshots at the same moment: what the memory held when the command
 * was recorded. A set whose memory cannot be read here, or whose layout is unknown, is written with
 * no bindings rather than left out, so the capture says the set was bound and that its contents
 * could not be read — which is different from a draw that bound nothing.
 */
static void SnapshotDescriptorBufferSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint,
                                         VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    CommandRecorder* rec = dev ? dev->RecorderFor(commandBuffer) : nullptr;
    if (!rec) return;
    DescriptorBufferTracker& tracker = DescriptorBufferTracker::Get();
    JsonWriter w(&Tracker::Get());
    w.BeginObject();
    w.Key("bindPoint"); w.Enum(ToString_VkPipelineBindPoint(bindPoint), (int64_t)bindPoint);
    w.Key("sets"); w.BeginArray();
    for (uint32_t i = 0; i < setCount; ++i) {
        const uint32_t set = firstSet + i;
        DescriptorSetContents c;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, size = 0;
        const VkDescriptorSetLayout setLayout = DescriptorTracker::Get().SetLayoutOf(layout, set);
        const uint8_t* bytes = nullptr;
        if (setLayout && tracker.SetSource(commandBuffer, bindPoint, set, buffer, offset)
            && tracker.LayoutSize(dev, setLayout, size)) {
            bytes = ResourceRegistry::Get().HostPointer(buffer, offset, size);
        }
        if (!bytes || !tracker.Decode(dev, setLayout, bytes, (size_t)size, c)) {
            w.BeginObject();
            w.Key("set"); w.Uint(set);
            w.Key("bindings"); w.BeginArray(); w.EndArray();
            w.EndObject();
            continue;
        }
        uint32_t dynamicIndex = 0;
        auto ids = CaptureSetBuffers(dev, rec, c, nullptr, 0, dynamicIndex);
        dynamicIndex = 0;
        WriteDescriptorSetJson(w, set, VK_NULL_HANDLE, c, nullptr, 0, dynamicIndex, &ids);
    }
    w.EndArray();
    w.EndObject();
    rec->SetExtraOnLast(",\"descriptors\":" + w.str());
}

void Hook_vkCmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                             VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount,
                                             const uint32_t* pBufferIndices, const VkDeviceSize* pOffsets) {
    DescriptorBufferTracker::Get().OnSetOffsets(commandBuffer, pipelineBindPoint, firstSet, setCount, pBufferIndices,
                                                pOffsets);
    SnapshotDescriptorBufferSets(commandBuffer, pipelineBindPoint, layout, firstSet, setCount);
}

void Hook_vkCmdSetDescriptorBufferOffsets2EXT(VkCommandBuffer commandBuffer,
                                              const VkSetDescriptorBufferOffsetsInfoEXT* pInfo) {
    if (!pInfo) return;
    // The stage mask says which bind point the offsets are for; the same mapping the pushed-set
    // path uses, since one call can name stages of only one pipeline type.
    const VkPipelineBindPoint point = BindPointFromStages(pInfo->stageFlags);
    DescriptorBufferTracker::Get().OnSetOffsets(commandBuffer, point, pInfo->firstSet, pInfo->setCount,
                                                pInfo->pBufferIndices, pInfo->pOffsets);
    SnapshotDescriptorBufferSets(commandBuffer, point, pInfo->layout, pInfo->firstSet, pInfo->setCount);
}

void Hook_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                      VkMemoryMapFlags flags, void** ppData) {
    if (ppData && *ppData) ResourceRegistry::Get().NoteMemoryMapped(memory, *ppData, offset, size);
}

void Hook_vkMapMemory2(VkDevice device, const VkMemoryMapInfo* pMemoryMapInfo, void** ppData) {
    if (pMemoryMapInfo && ppData && *ppData)
        ResourceRegistry::Get().NoteMemoryMapped(pMemoryMapInfo->memory, *ppData, pMemoryMapInfo->offset,
                                                 pMemoryMapInfo->size);
}

void Hook_vkMapMemory2KHR(VkDevice device, const VkMemoryMapInfo* pMemoryMapInfo, void** ppData) {
    Hook_vkMapMemory2(device, pMemoryMapInfo, ppData);
}

void Hook_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    ResourceRegistry::Get().NoteMemoryUnmapped(memory);
}

void Hook_vkCmdPushDescriptorSet(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
                                 uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites) {
    SnapshotPushedSet(commandBuffer, pipelineBindPoint, set, descriptorWriteCount, pDescriptorWrites);
}

void Hook_vkCmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
                                    uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites) {
    SnapshotPushedSet(commandBuffer, pipelineBindPoint, set, descriptorWriteCount, pDescriptorWrites);
}

void Hook_vkCmdPushDescriptorSet2(VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfo* pPushDescriptorSetInfo) {
    const VkPushDescriptorSetInfo* i = pPushDescriptorSetInfo;
    if (!i) return;
    SnapshotPushedSet(commandBuffer, BindPointFromStages(i->stageFlags), i->set, i->descriptorWriteCount, i->pDescriptorWrites);
}

void Hook_vkCmdPushDescriptorSet2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfo* pPushDescriptorSetInfo) {
    Hook_vkCmdPushDescriptorSet2(commandBuffer, pPushDescriptorSetInfo);
}

void Hook_vkCmdPushDescriptorSetWithTemplate(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                             VkPipelineLayout layout, uint32_t set, const void* pData) {
    SnapshotPushedTemplate(commandBuffer, descriptorUpdateTemplate, set, pData);
}

void Hook_vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                VkPipelineLayout layout, uint32_t set, const void* pData) {
    SnapshotPushedTemplate(commandBuffer, descriptorUpdateTemplate, set, pData);
}

void Hook_vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pPushDescriptorSetWithTemplateInfo) {
    const VkPushDescriptorSetWithTemplateInfo* i = pPushDescriptorSetWithTemplateInfo;
    if (i) SnapshotPushedTemplate(commandBuffer, i->descriptorUpdateTemplate, i->set, i->pData);
}

void Hook_vkCmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pPushDescriptorSetWithTemplateInfo) {
    Hook_vkCmdPushDescriptorSetWithTemplate2(commandBuffer, pPushDescriptorSetWithTemplateInfo);
}

// =============================================================================================
// Vertex, index and indirect buffers: readbacks attached as {"bufferData": [id, ...]}

static void AttachBufferData(VkCommandBuffer commandBuffer, uint32_t count, const VkBuffer* buffers,
                             const VkDeviceSize* offsets, const VkDeviceSize* sizes, VkDeviceSize fixedSize) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    CommandRecorder* rec = dev->RecorderFor(commandBuffer);
    if (!rec || !buffers || !CaptureManager::Get().IsCapturing()) return;
    std::string extra = ",\"bufferData\":[";
    for (uint32_t i = 0; i < count; ++i) {
        VkDeviceSize size = sizes && sizes[i] ? sizes[i] : fixedSize;
        uint32_t id = CaptureManager::Get().QueueBufferCapture(dev, rec, buffers[i], offsets ? offsets[i] : 0, size);
        if (i) extra += ',';
        extra += std::to_string(id);
    }
    extra += ']';
    rec->SetExtraOnLast(std::move(extra));
}

void Hook_vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                                 const VkBuffer* pBuffers, const VkDeviceSize* pOffsets) {
    AttachBufferData(commandBuffer, bindingCount, pBuffers, pOffsets, nullptr, VK_WHOLE_SIZE);
}

void Hook_vkCmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                                  const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes,
                                  const VkDeviceSize* pStrides) {
    AttachBufferData(commandBuffer, bindingCount, pBuffers, pOffsets, pSizes, VK_WHOLE_SIZE);
}

void Hook_vkCmdBindVertexBuffers2EXT(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                                     const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes,
                                     const VkDeviceSize* pStrides) {
    AttachBufferData(commandBuffer, bindingCount, pBuffers, pOffsets, pSizes, VK_WHOLE_SIZE);
}

void Hook_vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType) {
    AttachBufferData(commandBuffer, 1, &buffer, &offset, nullptr, VK_WHOLE_SIZE);
}

void Hook_vkCmdBindIndexBuffer2(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                                VkIndexType indexType) {
    AttachBufferData(commandBuffer, 1, &buffer, &offset, &size, VK_WHOLE_SIZE);
}

void Hook_vkCmdBindIndexBuffer2KHR(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                                   VkIndexType indexType) {
    AttachBufferData(commandBuffer, 1, &buffer, &offset, &size, VK_WHOLE_SIZE);
}

void Hook_vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount,
                            uint32_t stride) {
    VkDeviceSize size = drawCount ? (VkDeviceSize)(drawCount - 1) * stride + sizeof(VkDrawIndirectCommand) : 0;
    AttachBufferData(commandBuffer, 1, &buffer, &offset, nullptr, size);
}

void Hook_vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount,
                                   uint32_t stride) {
    VkDeviceSize size = drawCount ? (VkDeviceSize)(drawCount - 1) * stride + sizeof(VkDrawIndexedIndirectCommand) : 0;
    AttachBufferData(commandBuffer, 1, &buffer, &offset, nullptr, size);
}

void Hook_vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset) {
    AttachBufferData(commandBuffer, 1, &buffer, &offset, nullptr, sizeof(VkDispatchIndirectCommand));
}

// =============================================================================================
// Frame-start contents: the transfers' pre-call hooks took what they read; it goes onto the command

void Hook_vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*) {
    AttachPendingData(commandBuffer);
}
void Hook_vkCmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdCopyImage2KHR(VkCommandBuffer commandBuffer, const VkCopyImageInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter) {
    AttachPendingData(commandBuffer);
}
void Hook_vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdBlitImage2KHR(VkCommandBuffer commandBuffer, const VkBlitImageInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageResolve*) {
    AttachPendingData(commandBuffer);
}
void Hook_vkCmdResolveImage2(VkCommandBuffer commandBuffer, const VkResolveImageInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdResolveImage2KHR(VkCommandBuffer commandBuffer, const VkResolveImageInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*) {
    AttachPendingData(commandBuffer);
}
void Hook_vkCmdCopyImageToBuffer2(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdCopyImageToBuffer2KHR(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdCopyBuffer2(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdCopyBuffer2KHR(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*) {
    AttachPendingData(commandBuffer);
}
void Hook_vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2*) { AttachPendingData(commandBuffer); }
void Hook_vkCmdCopyBufferToImage2KHR(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2*) { AttachPendingData(commandBuffer); }

static void NoteClears(VkImage image, uint32_t rangeCount, const VkImageSubresourceRange* ranges) {
    if (!CaptureManager::Get().IsCapturing()) return;
    for (uint32_t i = 0; ranges && i < rangeCount; ++i)
        CaptureManager::Get().NoteImageWrite(image, ranges[i].aspectMask, ranges[i].baseMipLevel, ranges[i].levelCount, ranges[i].baseArrayLayer,
                                             ranges[i].layerCount);
}

void Hook_vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor,
                               uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
    if (GetDeviceData(commandBuffer)->RecorderFor(commandBuffer)) NoteClears(image, rangeCount, pRanges);
}

void Hook_vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                                      const VkClearDepthStencilValue* pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
    if (GetDeviceData(commandBuffer)->RecorderFor(commandBuffer)) NoteClears(image, rangeCount, pRanges);
}

} // namespace vkinsp
