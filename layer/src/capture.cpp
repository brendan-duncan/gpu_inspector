#include "capture.h"

#include "layer.h"
#include "resources.h"
#include "tracker.h"
#include "transport.h"
#include "vk_serialize.gen.h"
#include "format_info.h"

#include <algorithm>
#include <cstring>

namespace vkinsp {

std::atomic<bool> g_captureActive{false};

CommandRecorder* LookupRecorder(DeviceData* dev, VkCommandBuffer cb) {
    return CaptureManager::Get().RecorderFor(dev, cb);
}

CaptureManager& CaptureManager::Get() {
    static CaptureManager* instance = new CaptureManager();
    return *instance;
}

// ---------------------------------------------------------------------------------------------
// Request / state

void CaptureManager::Request(const CaptureOptions& options) {
    std::lock_guard lock(_mutex);
    if (_state != State::Idle) return;
    _options = options;
    _framesLeft = std::max(1u, options.frameCount);
    _state = State::Armed;
    Log("capture armed (%u frames)", _framesLeft);
}

void CaptureManager::SetRecordAlways(bool on) {
    _recordAlways.store(on, std::memory_order_relaxed);
    // Recorders are only reachable from the generated forwarders while the flag is on.
    if (on || IsCapturing()) g_captureActive.store(true, std::memory_order_release);
    else g_captureActive.store(false, std::memory_order_release);
    Log("record always: %s", on ? "on" : "off");
}

void CaptureManager::Start(DeviceData* dev) {
    _submissions.clear();
    _textures.clear();
    _commandTotal = 0;
    _frameIndex = dev->frameIndex;
    _state = State::Capturing;
    _capturing.store(true, std::memory_order_release);
    g_captureActive.store(true, std::memory_order_release);
    Log("capture started at frame %llu", (unsigned long long)_frameIndex);
}

// ---------------------------------------------------------------------------------------------
// Command buffers

CommandRecorder* CaptureManager::RecorderFor(DeviceData* dev, VkCommandBuffer cb) {
    std::shared_lock lock(dev->recorderMutex);
    auto it = dev->recorders.find(cb);
    return it == dev->recorders.end() ? nullptr : it->second.get();
}

void CaptureManager::OnBeginCommandBuffer(DeviceData* dev, VkCommandBuffer cb) {
    if (!IsCapturing() && !RecordAlways()) return;
    std::unique_lock lock(dev->recorderMutex);
    auto& slot = dev->recorders[cb];
    if (!slot) slot = std::make_unique<CommandRecorder>(dev->device, cb, &Tracker::Get());
    else slot->Reset();
}

void CaptureManager::OnEndCommandBuffer(DeviceData* dev, VkCommandBuffer cb) {
    if (CommandRecorder* rec = RecorderFor(dev, cb)) rec->MarkEnded();
}

void CaptureManager::OnResetCommandBuffer(DeviceData* dev, VkCommandBuffer cb) {
    std::unique_lock lock(dev->recorderMutex);
    dev->recorders.erase(cb);
}

void CaptureManager::OnFreeCommandBuffer(DeviceData* dev, VkCommandBuffer cb) {
    std::unique_lock lock(dev->recorderMutex);
    dev->recorders.erase(cb);
}

// ---------------------------------------------------------------------------------------------
// Submission

void CaptureManager::OnSubmit(DeviceData* dev, VkQueue queue, const std::string& method, std::string args,
                              int64_t result, const std::vector<VkCommandBuffer>& commandBuffers) {
    if (!IsCapturing()) return;
    CaptureSubmission sub;
    sub.queueId = Tracker::Get().Resolve(HT_VkQueue, (uint64_t)(uintptr_t)queue);
    sub.method = method;
    sub.args = std::move(args);
    sub.result = result;
    for (VkCommandBuffer cb : commandBuffers) {
        SubmittedCommandBuffer scb;
        scb.commandBufferId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)cb);
        if (CommandRecorder* rec = RecorderFor(dev, cb)) {
            scb.commands = rec->Snapshot();
            _commandTotal += scb.commands->size();
        }
        sub.commandBuffers.push_back(std::move(scb));
    }
    std::lock_guard lock(_mutex);
    _submissions.push_back(std::move(sub));
}

void CaptureManager::OnPresent(DeviceData* dev, VkQueue queue, const VkPresentInfoKHR* info, VkResult result) {
    std::unique_lock lock(_mutex);
    if (_state == State::Armed) {
        Start(dev);
        return;
    }
    if (_state != State::Capturing) return;

    // The present itself is part of the captured frame.
    {
        JsonWriter w(&Tracker::Get());
        ArgsToJson_vkQueuePresentKHR(w, queue, info);
        CaptureSubmission sub;
        sub.queueId = Tracker::Get().Resolve(HT_VkQueue, (uint64_t)(uintptr_t)queue);
        sub.method = "vkQueuePresentKHR";
        sub.args = std::move(w.str());
        sub.result = (int64_t)result;
        _submissions.push_back(std::move(sub));
    }

    if (--_framesLeft > 0) return;
    lock.unlock();
    Finish(dev);
}

// ---------------------------------------------------------------------------------------------
// Finish: wait for the GPU, read back, send

void CaptureManager::Finish(DeviceData* dev) {
    _capturing.store(false, std::memory_order_release);
    if (!RecordAlways()) g_captureActive.store(false, std::memory_order_release);
    Log("capture finishing: %zu submissions, %llu commands, %zu textures", _submissions.size(),
        (unsigned long long)_commandTotal, _textures.size());

    // Everything recorded in the frame has been submitted; wait for it so staging data is valid.
    dev->dispatch.DeviceWaitIdle(dev->device);

    SendCommands();
    SendTextures(dev);
    ReleaseStaging(dev);

    if (!RecordAlways()) {
        std::unique_lock lock(dev->recorderMutex);
        dev->recorders.clear();
    }
    std::lock_guard lock(_mutex);
    _submissions.clear();
    _textures.clear();
    _state = State::Idle;
    Log("capture sent");
}

static void WriteCommandEntry(JsonWriter& w, uint64_t index, const char* method, const char* objectClass,
                              uint64_t objectId, const std::string& args, int64_t result, const std::string& extra) {
    w.BeginObject();
    w.Key("index"); w.Uint(index);
    w.Key("method"); w.String(method);
    w.Key("object");
    if (objectId) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"__id\":%llu,\"__class\":\"%s\"}", (unsigned long long)objectId, objectClass);
        w.Raw(buf);
    } else {
        w.Null();
    }
    w.Key("args"); if (args.empty()) w.Null(); else w.Raw(args);
    if (result) { w.Key("result"); w.Int(result); }
    if (!extra.empty()) w.Raw(extra);  // extra starts with a comma-separated key list: ,"children":[...]
    w.EndObject();
}

void CaptureManager::SendCommands() {
    Transport& t = Transport::Get();
    std::vector<CaptureSubmission> submissions;
    {
        std::lock_guard lock(_mutex);
        submissions = _submissions;
    }

    // Flatten: submit entry, then per command buffer its commands, in submission order.
    const size_t kBatch = 500;
    uint64_t index = 0;
    JsonWriter batch;
    size_t inBatch = 0;
    uint64_t total = 0;
    for (auto& s : submissions) {
        total += 1;
        for (auto& cb : s.commandBuffers) total += cb.commands ? cb.commands->size() : 1;
    }

    {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureFrameResults");
        w.Key("frame"); w.Uint(_frameIndex);
        w.Key("count"); w.Uint(total);
        w.Key("batches"); w.Uint((total + kBatch - 1) / kBatch);
        w.EndObject();
        t.SendJson(std::move(w.str()));
    }

    auto flush = [&]() {
        if (!inBatch) return;
        batch.EndArray();
        batch.EndObject();
        t.SendJson(std::move(batch.str()));
        batch.Reset();
        inBatch = 0;
    };
    auto begin = [&]() {
        if (inBatch) return;
        batch.BeginObject();
        batch.Key("action"); batch.String("CaptureFrameCommands");
        batch.Key("frame"); batch.Uint(_frameIndex);
        batch.Key("index"); batch.Uint(index);
        batch.Key("commands"); batch.BeginArray();
    };
    auto emit = [&](const char* method, const char* cls, uint64_t objectId, const std::string& args, int64_t result,
                    const std::string& extra) {
        begin();
        WriteCommandEntry(batch, index++, method, cls, objectId, args, result, extra);
        if (++inBatch >= kBatch) flush();
    };

    for (auto& s : submissions) {
        emit(s.method.c_str(), "VkQueue", s.queueId, s.args, s.result, "");
        for (auto& cb : s.commandBuffers) {
            if (!cb.commands) {
                emit("<unrecorded command buffer>", "VkCommandBuffer", cb.commandBufferId, "", 0, "");
                continue;
            }
            for (auto& c : *cb.commands) {
                emit(kVkCommandNames[(int)c.id], "VkCommandBuffer", cb.commandBufferId, c.args, c.result, c.extra);
            }
        }
    }
    flush();
}

// ---------------------------------------------------------------------------------------------
// Render pass attachments

void CaptureManager::OnBeginRenderPass(DeviceData* dev, CommandRecorder* rec, const VkRenderPassBeginInfo* info) {
    ActivePass& p = rec->pass();
    p = ActivePass{};
    p.active = true;
    p.renderPass = info->renderPass;
    p.framebuffer = info->framebuffer;
    p.renderArea = info->renderArea;
    p.passIndex = rec->NextPassIndex();

    FramebufferInfo fb;
    if (ResourceRegistry::Get().GetFramebuffer(info->framebuffer, fb)) {
        p.attachments = fb.attachments;
        p.layerCount = fb.layers;
        if (fb.imageless) {
            for (auto* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext) {
                if (n->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
                    auto* ab = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(n);
                    p.attachments.assign(ab->pAttachments, ab->pAttachments + ab->attachmentCount);
                }
            }
        }
    }
    RenderPassInfo rp;
    if (ResourceRegistry::Get().GetRenderPass(info->renderPass, rp)) {
        p.layouts.resize(p.attachments.size(), VK_IMAGE_LAYOUT_UNDEFINED);
        for (size_t i = 0; i < p.attachments.size() && i < rp.attachments.size(); ++i)
            p.layouts[i] = rp.attachments[i].finalLayout;
    }
}

void CaptureManager::OnBeginRendering(DeviceData* dev, CommandRecorder* rec, const VkRenderingInfo* info) {
    ActivePass& p = rec->pass();
    p = ActivePass{};
    p.active = true;
    p.dynamic = true;
    p.renderArea = info->renderArea;
    p.layerCount = info->layerCount;
    p.passIndex = rec->NextPassIndex();
    auto add = [&](const VkRenderingAttachmentInfo* a) {
        if (!a || !a->imageView) return;
        p.attachments.push_back(a->imageView);
        p.layouts.push_back(a->imageLayout);
        p.resolveViews.push_back(a->resolveMode != VK_RESOLVE_MODE_NONE ? a->resolveImageView : VK_NULL_HANDLE);
    };
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) add(&info->pColorAttachments[i]);
    add(info->pDepthAttachment);
    if (info->pStencilAttachment && (!info->pDepthAttachment ||
                                     info->pStencilAttachment->imageView != info->pDepthAttachment->imageView))
        add(info->pStencilAttachment);
}

void CaptureManager::OnEndPass(DeviceData* dev, CommandRecorder* rec) {
    ActivePass& p = rec->pass();
    if (!p.active) return;
    // Readback copies are only injected while a capture is in progress.
    if (IsCapturing() && _options.captureTextures) {
        for (uint32_t i = 0; i < p.attachments.size(); ++i) {
            CaptureAttachment(dev, rec, i, p.attachments[i], i < p.layouts.size() ? p.layouts[i] : VK_IMAGE_LAYOUT_GENERAL);
        }
    }
    p.active = false;
}

// ---------------------------------------------------------------------------------------------
// Readback

bool CaptureManager::AllocateStaging(DeviceData* dev, VkDeviceSize size, uint32_t& chunkIndex, VkDeviceSize& offset) {
    const VkDeviceSize kChunk = 64ull << 20;
    const VkDeviceSize align = 256;
    std::lock_guard lock(_mutex);
    for (uint32_t i = 0; i < _staging.size(); ++i) {
        VkDeviceSize start = (_staging[i].used + align - 1) & ~(align - 1);
        if (start + size <= _staging[i].size) {
            _staging[i].used = start + size;
            chunkIndex = i;
            offset = start;
            return true;
        }
    }
    StagingChunk chunk;
    chunk.size = std::max(kChunk, (size + align - 1) & ~(align - 1));

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = chunk.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dev->dispatch.CreateBuffer(dev->device, &bci, nullptr, &chunk.buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    dev->dispatch.GetBufferMemoryRequirements(dev->device, chunk.buffer, &req);
    int typeIndex = -1;
    for (int pass = 0; pass < 2 && typeIndex < 0; ++pass) {
        VkMemoryPropertyFlags want = pass == 0
            ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for (uint32_t i = 0; i < dev->memoryProperties.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (dev->memoryProperties.memoryTypes[i].propertyFlags & want) == want) {
                typeIndex = (int)i;
                break;
            }
        }
    }
    if (typeIndex < 0) {
        dev->dispatch.DestroyBuffer(dev->device, chunk.buffer, nullptr);
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)typeIndex;
    if (dev->dispatch.AllocateMemory(dev->device, &mai, nullptr, &chunk.memory) != VK_SUCCESS) {
        dev->dispatch.DestroyBuffer(dev->device, chunk.buffer, nullptr);
        return false;
    }
    dev->dispatch.BindBufferMemory(dev->device, chunk.buffer, chunk.memory, 0);
    chunk.used = size;
    _staging.push_back(chunk);
    chunkIndex = (uint32_t)_staging.size() - 1;
    offset = 0;
    Log("staging chunk %u: %llu MB", chunkIndex, (unsigned long long)(chunk.size >> 20));
    return true;
}

void CaptureManager::CaptureAttachment(DeviceData* dev, CommandRecorder* rec, uint32_t attachmentIndex,
                                       VkImageView view, VkImageLayout layout) {
    ResourceRegistry& reg = ResourceRegistry::Get();
    ImageViewInfo vi;
    ImageInfo img;
    if (!reg.GetImageView(view, vi) || !reg.GetImage(vi.image, img)) return;

    TextureCapture tc;
    tc.imageId = Tracker::Get().Resolve(HT_VkImage, (uint64_t)(uintptr_t)vi.image);
    tc.commandBufferId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)rec->commandBuffer());
    tc.passIndex = rec->pass().passIndex;
    tc.attachment = attachmentIndex;
    tc.format = img.format;
    tc.mip = vi.range.baseMipLevel;
    tc.width = std::max(1u, img.extent.width >> tc.mip);
    tc.height = std::max(1u, img.extent.height >> tc.mip);
    tc.depth = std::max(1u, img.extent.depth >> tc.mip);
    tc.layers = vi.range.layerCount == VK_REMAINING_ARRAY_LAYERS ? img.arrayLayers - vi.range.baseArrayLayer
                                                                  : vi.range.layerCount;
    tc.layers = std::max(1u, std::min(tc.layers, rec->pass().layerCount));
    tc.aspect = FormatAspects(img.format) & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                      : VK_IMAGE_ASPECT_COLOR_BIT;

    auto fail = [&](const char* why) {
        tc.failed = true;
        tc.note = why;
        std::lock_guard lock(_mutex);
        _textures.push_back(tc);
    };
    if (!img.transferSrc) return fail("image lacks TRANSFER_SRC usage");
    if (img.samples != VK_SAMPLE_COUNT_1_BIT) return fail("multisampled attachment (resolve not implemented yet)");
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) return fail("unknown final layout");

    uint32_t bpp = FormatBytesPerTexel(img.format, tc.aspect);
    if (bpp == 0) return fail("unsupported format for readback");
    // Depth aspect copies use the depth-only packed size (D24 -> 4 bytes, D16 -> 2, D32 -> 4).
    tc.size = (VkDeviceSize)tc.width * tc.height * tc.depth * tc.layers * bpp;
    if (tc.size > _options.maxTextureSize) return fail("exceeds max texture size");

    uint32_t chunkIndex = 0;
    VkDeviceSize offset = 0;
    if (!AllocateStaging(dev, tc.size, chunkIndex, offset)) return fail("staging allocation failed");
    tc.stagingIndex = chunkIndex;
    tc.stagingOffset = offset;

    VkCommandBuffer cb = rec->commandBuffer();
    VkImageSubresourceRange range{tc.aspect, tc.mip, 1, vi.range.baseArrayLayer, tc.layers};
    if (tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (FormatAspects(img.format) & VK_IMAGE_ASPECT_STENCIL_BIT))
        range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;  // barriers must cover both aspects

    VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = layout;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = vi.image;
    toSrc.subresourceRange = range;
    dev->dispatch.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {tc.aspect, tc.mip, vi.range.baseArrayLayer, tc.layers};
    region.imageExtent = {tc.width, tc.height, tc.depth};
    dev->dispatch.CmdCopyImageToBuffer(cb, vi.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       _staging[chunkIndex].buffer, 1, &region);

    VkImageMemoryBarrier back = toSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = layout;
    VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.buffer = _staging[chunkIndex].buffer;
    hostRead.offset = offset;
    hostRead.size = tc.size;
    dev->dispatch.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                                     0, nullptr, 1, &hostRead, 1, &back);

    std::lock_guard lock(_mutex);
    _textures.push_back(tc);
}

void CaptureManager::SendTextures(DeviceData* dev) {
    Transport& t = Transport::Get();
    std::vector<TextureCapture> textures;
    {
        std::lock_guard lock(_mutex);
        textures = _textures;
        for (auto& c : _staging) {
            if (!c.mapped) dev->dispatch.MapMemory(dev->device, c.memory, 0, VK_WHOLE_SIZE, 0, &c.mapped);
            VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            r.memory = c.memory;
            r.size = VK_WHOLE_SIZE;
            dev->dispatch.InvalidateMappedMemoryRanges(dev->device, 1, &r);
        }
    }

    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureTextureFrames");
    w.Key("count"); w.Uint(textures.size());
    w.Key("textures"); w.BeginArray();
    for (auto& tc : textures) {
        w.BeginObject();
        w.Key("id"); w.Uint(tc.imageId);
        w.Key("commandBuffer"); w.Uint(tc.commandBufferId);
        w.Key("passIndex"); w.Uint(tc.passIndex);
        w.Key("attachment"); w.Uint(tc.attachment);
        w.Key("format"); w.Enum(ToString_VkFormat(tc.format), (int64_t)tc.format);
        w.Key("aspect"); w.String(tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? "depth" : "color");
        w.Key("width"); w.Uint(tc.width);
        w.Key("height"); w.Uint(tc.height);
        w.Key("depth"); w.Uint(tc.depth);
        w.Key("layers"); w.Uint(tc.layers);
        w.Key("mip"); w.Uint(tc.mip);
        w.Key("size"); w.Uint(tc.failed ? 0 : tc.size);
        if (tc.failed) { w.Key("error"); w.String(tc.note); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    t.SendJson(std::move(w.str()));

    for (auto& tc : textures) {
        if (tc.failed) continue;
        const StagingChunk& c = _staging[tc.stagingIndex];
        if (!c.mapped) continue;
        JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureTextureData");
        h.Key("id"); h.Uint(tc.imageId);
        h.Key("commandBuffer"); h.Uint(tc.commandBufferId);
        h.Key("passIndex"); h.Uint(tc.passIndex);
        h.Key("attachment"); h.Uint(tc.attachment);
        h.Key("size"); h.Uint(tc.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), static_cast<const uint8_t*>(c.mapped) + tc.stagingOffset, (size_t)tc.size);
    }
}

void CaptureManager::ReleaseStaging(DeviceData* dev) {
    std::lock_guard lock(_mutex);
    for (auto& c : _staging) {
        if (c.mapped) dev->dispatch.UnmapMemory(dev->device, c.memory);
        dev->dispatch.DestroyBuffer(dev->device, c.buffer, nullptr);
        dev->dispatch.FreeMemory(dev->device, c.memory, nullptr);
    }
    _staging.clear();
}

} // namespace vkinsp
