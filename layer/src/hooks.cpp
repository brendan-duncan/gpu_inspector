// Post-call and pre-call hooks: shader code retention, memory bindings, the resource registry,
// and the frame-capture entry points.
#include "hooks.h"

#include "capture.h"
#include "layer.h"
#include "resources.h"
#include "tracker.h"
#include "transport.h"
#include "vk_serialize.gen.h"

#include <memory>
#include <string>
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

void PreHook_vkCreateSwapchainKHR(VkDevice& device, const VkSwapchainCreateInfoKHR*& pCreateInfo,
                                  const VkAllocationCallbacks*& pAllocator, VkSwapchainKHR*& pSwapchain) {
    if (!pCreateInfo) return;
    DeviceData* dev = GetDeviceData(device);
    VkSurfaceCapabilitiesKHR caps{};
    if (dev && dev->instance->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR &&
        dev->instance->dispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physicalDevice, pCreateInfo->surface, &caps) == VK_SUCCESS &&
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        thread_local VkSwapchainCreateInfoKHR copy;
        copy = *pCreateInfo;
        copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        pCreateInfo = &copy;
    }
}

void PreHook_vkBeginCommandBuffer(VkCommandBuffer& commandBuffer, const VkCommandBufferBeginInfo*& pBeginInfo) {
    CaptureManager::Get().OnBeginCommandBuffer(GetDeviceData(commandBuffer), commandBuffer);
}

void PreHook_vkResetCommandBuffer(VkCommandBuffer& commandBuffer, VkCommandBufferResetFlags& flags) {
    CaptureManager::Get().OnResetCommandBuffer(GetDeviceData(commandBuffer), commandBuffer);
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

void Hook_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
                             const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    if (!pCreateInfo || !pRenderPass || !*pRenderPass) return;
    RenderPassInfo info;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
        const VkAttachmentDescription& a = pCreateInfo->pAttachments[i];
        info.attachments.push_back({a.format, a.samples, a.finalLayout, a.storeOp});
    }
    for (uint32_t s = 0; s < pCreateInfo->subpassCount; ++s) {
        const VkSubpassDescription& sp = pCreateInfo->pSubpasses[s];
        std::vector<uint32_t> color;
        for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) color.push_back(sp.pColorAttachments[i].attachment);
        info.subpassColor.push_back(color);
        info.subpassDepth.push_back(sp.pDepthStencilAttachment ? (int32_t)sp.pDepthStencilAttachment->attachment : -1);
    }
    ResourceRegistry::Get().AddRenderPass(*pRenderPass, info);
}

void Hook_vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                              const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    if (!pCreateInfo || !pRenderPass || !*pRenderPass) return;
    RenderPassInfo info;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
        const VkAttachmentDescription2& a = pCreateInfo->pAttachments[i];
        info.attachments.push_back({a.format, a.samples, a.finalLayout, a.storeOp});
    }
    for (uint32_t s = 0; s < pCreateInfo->subpassCount; ++s) {
        const VkSubpassDescription2& sp = pCreateInfo->pSubpasses[s];
        std::vector<uint32_t> color;
        for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) color.push_back(sp.pColorAttachments[i].attachment);
        info.subpassColor.push_back(color);
        info.subpassDepth.push_back(sp.pDepthStencilAttachment ? (int32_t)sp.pDepthStencilAttachment->attachment : -1);
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
    ResourceRegistry::Get().AddSwapchain(*pSwapchain, info);
}

void Hook_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount,
                                  VkImage* pSwapchainImages) {
    if (!pSwapchainImages || !pSwapchainImageCount) return;
    SwapchainInfo sc;
    if (!ResourceRegistry::Get().GetSwapchain(swapchain, sc)) return;
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

void Hook_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule) {
    if (!pCreateInfo || !pCreateInfo->pCode || !pShaderModule || !*pShaderModule) return;
    auto blob = std::make_shared<std::vector<uint8_t>>(
        reinterpret_cast<const uint8_t*>(pCreateInfo->pCode),
        reinterpret_cast<const uint8_t*>(pCreateInfo->pCode) + pCreateInfo->codeSize);
    Tracker::Get().AddBlob(HT_VkShaderModule, (uint64_t)(uintptr_t)*pShaderModule, "SPIR-V", std::move(blob));
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

void Hook_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
                                    const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    if (!pCreateInfos || !pPipelines) return;
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        if (!pPipelines[i]) continue;
        const VkGraphicsPipelineCreateInfo& ci = pCreateInfos[i];
        for (uint32_t s = 0; s < ci.stageCount && ci.pStages; ++s) AttachStage(pPipelines[i], ci.pStages[s]);
    }
}

void Hook_vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
                                   const VkComputePipelineCreateInfo* pCreateInfos,
                                   const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    if (!pCreateInfos || !pPipelines) return;
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        if (pPipelines[i]) AttachStage(pPipelines[i], pCreateInfos[i].stage);
    }
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
}

void Hook_vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    SendBinding(HT_VkImage, (uint64_t)(uintptr_t)image, memory, memoryOffset);
}

void Hook_vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos) {
    for (uint32_t i = 0; pBindInfos && i < bindInfoCount; ++i)
        SendBinding(HT_VkBuffer, (uint64_t)(uintptr_t)pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
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
    for (uint32_t i = 0; pCommandBuffers && i < commandBufferCount; ++i)
        CaptureManager::Get().OnFreeCommandBuffer(dev, pCommandBuffers[i]);
}

void Hook_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    if (!CaptureManager::Get().IsCapturing()) return;
    std::vector<VkCommandBuffer> cbs;
    for (uint32_t i = 0; pSubmits && i < submitCount; ++i)
        for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; ++j) cbs.push_back(pSubmits[i].pCommandBuffers[j]);
    JsonWriter w(&Tracker::Get());
    ArgsToJson_vkQueueSubmit(w, queue, submitCount, pSubmits, fence);
    CaptureManager::Get().OnSubmit(GetDeviceData(queue), queue, "vkQueueSubmit", std::move(w.str()), 0, cbs);
}

void Hook_vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    if (!CaptureManager::Get().IsCapturing()) return;
    std::vector<VkCommandBuffer> cbs;
    for (uint32_t i = 0; pSubmits && i < submitCount; ++i)
        for (uint32_t j = 0; j < pSubmits[i].commandBufferInfoCount; ++j)
            cbs.push_back(pSubmits[i].pCommandBufferInfos[j].commandBuffer);
    JsonWriter w(&Tracker::Get());
    ArgsToJson_vkQueueSubmit2(w, queue, submitCount, pSubmits, fence);
    CaptureManager::Get().OnSubmit(GetDeviceData(queue), queue, "vkQueueSubmit2", std::move(w.str()), 0, cbs);
}

void Hook_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    Hook_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

void Hook_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                               VkSubpassContents contents) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnBeginRenderPass(dev, rec, pRenderPassBegin);
}

void Hook_vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                                const VkSubpassBeginInfo* pSubpassBeginInfo) {
    DeviceData* dev = GetDeviceData(commandBuffer);
    if (CommandRecorder* rec = dev->RecorderFor(commandBuffer)) CaptureManager::Get().OnBeginRenderPass(dev, rec, pRenderPassBegin);
}

void Hook_vkCmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                                   const VkSubpassBeginInfo* pSubpassBeginInfo) {
    Hook_vkCmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

void Hook_vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo) {
    DeviceData* dev = GetDeviceData(commandBuffer);
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
            for (auto& c : *snap) {
                if (!first) extra += ',';
                first = false;
                extra += "{\"method\":\"";
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
}

} // namespace vkinsp
