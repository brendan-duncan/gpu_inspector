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
    _armedAtFrame.store(options.atFrame, std::memory_order_release);
    if (options.atFrame == UINT64_MAX) Log("capture armed (%u frames)", _framesLeft);
    else Log("capture armed (%u frames) for frame %llu", _framesLeft, (unsigned long long)options.atFrame);
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
    _buffers.clear();
    _bufferBytes = 0;
    _nextBufferId = 1;
    _commandTotal = 0;
    _frameIndex = dev->frameIndex;
    _frameCount = std::max(1u, _options.frameCount);
    _state = State::Capturing;
    _armedAtFrame.store(UINT64_MAX, std::memory_order_release);
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

void CaptureManager::OnBeginCommandBuffer(DeviceData* dev, VkCommandBuffer cb, VkCommandBufferUsageFlags flags) {
    // A capture queued for a specific frame starts with that frame's first command buffer, so
    // frame 0 (before any present) can be captured whole. Later frames start at the present.
    if (_armedAtFrame.load(std::memory_order_acquire) == dev->frameIndex) {
        std::lock_guard lock(_mutex);
        if (_state == State::Armed) Start(dev);
    }
    if (!IsCapturing() && !RecordAlways()) return;
    std::unique_lock lock(dev->recorderMutex);
    auto& slot = dev->recorders[cb];
    if (!slot) slot = std::make_unique<CommandRecorder>(dev->device, cb, &Tracker::Get());
    slot->Reset((flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) != 0);
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
    sub.frame = (uint32_t)(dev->frameIndex - _frameIndex);
    for (VkCommandBuffer cb : commandBuffers) {
        SubmittedCommandBuffer scb;
        scb.commandBufferId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)cb);
        if (CommandRecorder* rec = RecorderFor(dev, cb)) {
            scb.commands = rec->Snapshot();
            _commandTotal += scb.commands->size();
        }
        sub.commandBuffers.push_back(std::move(scb));
    }
    Log("capture: %s with %zu command buffers", method.c_str(), commandBuffers.size());
    std::lock_guard lock(_mutex);
    // Readbacks recorded while recording belong to the frame their command buffer runs in.
    for (auto& tc : _textures) {
        if (tc.frame != UINT32_MAX) continue;
        for (auto& scb : sub.commandBuffers) {
            if (scb.commandBufferId == tc.commandBufferId) {
                tc.frame = sub.frame;
                break;
            }
        }
    }
    for (auto& bc : _buffers) {
        if (bc.frame != UINT32_MAX || !bc.recorded) continue;
        for (auto& scb : sub.commandBuffers) {
            if (scb.commandBufferId == bc.commandBufferId) {
                bc.frame = sub.frame;
                break;
            }
        }
    }
    _submissions.push_back(std::move(sub));
}

void CaptureManager::OnPresent(DeviceData* dev, VkQueue queue, const VkPresentInfoKHR* info, VkResult result) {
    std::unique_lock lock(_mutex);
    if (_state == State::Armed) {
        // frameIndex was advanced by this present: the frame that starts now is `frameIndex`.
        if (_options.atFrame == UINT64_MAX || dev->frameIndex >= _options.atFrame) Start(dev);
        return;
    }
    if (_state != State::Capturing) return;
    Log("capture: present (result %d, %u swapchains) after %zu submissions", (int)result,
        info ? info->swapchainCount : 0, _submissions.size());

    // The present itself is part of the captured frame.
    {
        JsonWriter w(&Tracker::Get());
        ArgsToJson_vkQueuePresentKHR(w, queue, info);
        CaptureSubmission sub;
        sub.queueId = Tracker::Get().Resolve(HT_VkQueue, (uint64_t)(uintptr_t)queue);
        sub.method = "vkQueuePresentKHR";
        sub.args = std::move(w.str());
        sub.result = (int64_t)result;
        sub.frame = (uint32_t)(dev->frameIndex - _frameIndex - 1);  // the present ends the frame
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
    Log("capture finishing: %zu submissions, %llu commands, %zu textures, %zu buffers (%llu KB)",
        _submissions.size(), (unsigned long long)_commandTotal, _textures.size(), _buffers.size(),
        (unsigned long long)(_bufferBytes >> 10));

    // Everything recorded in the frame has been submitted; wait for it so staging data is valid.
    dev->dispatch.DeviceWaitIdle(dev->device);

    SendCommands();
    SendTextures(dev);
    SendBuffers(dev);
    ReleaseStaging(dev);

    if (!RecordAlways()) {
        std::unique_lock lock(dev->recorderMutex);
        dev->recorders.clear();
    }
    std::lock_guard lock(_mutex);
    _submissions.clear();
    _textures.clear();
    _buffers.clear();
    _state = State::Idle;
    Log("capture sent");
}

static void WriteCommandEntry(JsonWriter& w, uint64_t index, uint32_t frame, const char* method, const char* objectClass,
                              uint64_t objectId, const std::string& args, int64_t result, const std::string& extra) {
    w.BeginObject();
    w.Key("index"); w.Uint(index);
    w.Key("frame"); w.Uint(frame);
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
    if (!extra.empty()) w.str() += extra;  // extra is a pre-separated member list: ,"children":[...]
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
        w.Key("frames"); w.Uint(_frameCount);
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
    auto emit = [&](uint32_t frame, const char* method, const char* cls, uint64_t objectId, const std::string& args,
                    int64_t result, const std::string& extra) {
        begin();
        WriteCommandEntry(batch, index++, frame, method, cls, objectId, args, result, extra);
        if (++inBatch >= kBatch) flush();
    };

    for (auto& s : submissions) {
        emit(s.frame, s.method.c_str(), "VkQueue", s.queueId, s.args, s.result, "");
        for (auto& cb : s.commandBuffers) {
            if (!cb.commands) {
                emit(s.frame, "<unrecorded command buffer>", "VkCommandBuffer", cb.commandBufferId, "", 0, "");
                continue;
            }
            for (auto& c : *cb.commands) {
                emit(s.frame, kVkCommandNames[(int)c.id], "VkCommandBuffer", cb.commandBufferId, c.args, c.result, c.extra);
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
    // Buffers bound during the pass are copied now that transfer commands are allowed again.
    FlushBufferCopies(dev, rec);
}

void CaptureManager::OnExecuteCommands(DeviceData* dev, CommandRecorder* rec, uint32_t count,
                                       const VkCommandBuffer* secondaries) {
    if (!rec || !secondaries) return;
    auto& dst = rec->pendingCopies();
    for (uint32_t i = 0; i < count; ++i) {
        CommandRecorder* sec = RecorderFor(dev, secondaries[i]);
        if (!sec) continue;
        auto& src = sec->pendingCopies();
        dst.insert(dst.end(), src.begin(), src.end());
        src.clear();
    }
    if (!rec->InsidePass()) FlushBufferCopies(dev, rec);
}

// ---------------------------------------------------------------------------------------------
// Buffer readback

uint32_t CaptureManager::QueueBufferCapture(DeviceData* dev, CommandRecorder* rec, VkBuffer buffer,
                                            VkDeviceSize offset, VkDeviceSize size) {
    if (!rec || !buffer || !IsCapturing() || !_options.captureBuffers) return 0;
    BufferInfo bi;
    if (!ResourceRegistry::Get().GetBuffer(buffer, bi)) return 0;
    if (offset >= bi.size) return 0;
    VkDeviceSize avail = bi.size - offset;
    if (size == VK_WHOLE_SIZE || size > avail) size = avail;
    if (size == 0) return 0;

    BufferCapture bc;
    bc.bufferId = Tracker::Get().Resolve(HT_VkBuffer, (uint64_t)(uintptr_t)buffer);
    bc.offset = offset;
    bc.size = size;
    if (size > _options.maxBufferSize) {
        bc.originalSize = size;
        bc.size = _options.maxBufferSize;
    }

    // The same range bound again before the pending copies are flushed reuses the first copy,
    // provided that copy covers everything this binding needs (a shorter earlier binding of the
    // same offset must not stand in for a longer one).
    for (const PendingBufferCopy& p : rec->pendingCopies()) {
        if (p.buffer == buffer && p.offset == offset && p.size >= bc.size) return p.captureId;
    }
    auto fail = [&](const char* why) {
        bc.failed = true;
        bc.note = why;
        std::lock_guard lock(_mutex);
        bc.id = _nextBufferId++;
        _buffers.push_back(bc);
        return bc.id;
    };
    {
        std::lock_guard lock(_mutex);
        if (_bufferBytes + bc.size > _options.maxBufferTotal) {
            bc.failed = true;
            bc.note = "buffer capture budget exceeded";
            bc.id = _nextBufferId++;
            _buffers.push_back(bc);
            return bc.id;
        }
    }
    if (!bi.transferSrc) return fail("buffer lacks TRANSFER_SRC usage");

    uint32_t chunkIndex = 0;
    VkDeviceSize stagingOffset = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    if (!AllocateStaging(dev, bc.size, chunkIndex, stagingOffset, &staging)) return fail("staging allocation failed");
    bc.stagingIndex = chunkIndex;
    bc.stagingOffset = stagingOffset;
    {
        std::lock_guard lock(_mutex);
        bc.id = _nextBufferId++;
        _bufferBytes += bc.size;
        _buffers.push_back(bc);
    }
    rec->pendingCopies().push_back({bc.id, buffer, offset, bc.size, staging, stagingOffset});
    if (!rec->InsidePass()) FlushBufferCopies(dev, rec);
    return bc.id;
}

void CaptureManager::FlushBufferCopies(DeviceData* dev, CommandRecorder* rec) {
    auto& pending = rec->pendingCopies();
    if (pending.empty()) return;
    VkCommandBuffer cb = rec->commandBuffer();

    // Whatever wrote the buffers (host, transfers, shaders) must be visible to the copies.
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dev->dispatch.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     1, &before, 0, nullptr, 0, nullptr);
    for (const PendingBufferCopy& p : pending) {
        VkBufferCopy region{p.offset, p.stagingOffset, p.size};
        dev->dispatch.CmdCopyBuffer(cb, p.buffer, p.staging, 1, &region);
    }
    // Later writes to the source buffers must wait for the copies; the staging data is read by
    // the host after the frame.
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dev->dispatch.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                                     1, &after, 0, nullptr, 0, nullptr);

    uint64_t cbId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)cb);
    std::lock_guard lock(_mutex);
    for (const PendingBufferCopy& p : pending) {
        if (p.captureId == 0 || p.captureId > _buffers.size()) continue;
        BufferCapture& bc = _buffers[p.captureId - 1];   // ids are 1-based indices
        bc.recorded = true;
        bc.commandBufferId = cbId;
    }
    pending.clear();
}

void CaptureManager::SendBuffers(DeviceData* dev) {
    Transport& t = Transport::Get();
    std::vector<BufferCapture> buffers;
    {
        std::lock_guard lock(_mutex);
        buffers = _buffers;
        for (auto& c : _staging) {
            if (!c.mapped) dev->dispatch.MapMemory(dev->device, c.memory, 0, VK_WHOLE_SIZE, 0, &c.mapped);
            VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            r.memory = c.memory;
            r.size = VK_WHOLE_SIZE;
            dev->dispatch.InvalidateMappedMemoryRanges(dev->device, 1, &r);
        }
    }
    for (auto& bc : buffers) {
        if (bc.failed) continue;
        if (!bc.recorded) {
            bc.failed = true;
            bc.note = "copy was never recorded (secondary command buffer not executed)";
        } else if (bc.frame == UINT32_MAX) {
            bc.failed = true;
            bc.note = "command buffer was not submitted during the capture";
        }
        if (bc.failed) bc.frame = 0;
    }

    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureBuffers");
    w.Key("count"); w.Uint(buffers.size());
    w.Key("buffers"); w.BeginArray();
    for (auto& bc : buffers) {
        w.BeginObject();
        w.Key("id"); w.Uint(bc.id);
        w.Key("buffer"); w.Uint(bc.bufferId);
        w.Key("frame"); w.Uint(bc.frame);
        w.Key("commandBuffer"); w.Uint(bc.commandBufferId);
        w.Key("offset"); w.Uint(bc.offset);
        w.Key("size"); w.Uint(bc.failed ? 0 : bc.size);
        if (bc.originalSize) { w.Key("originalSize"); w.Uint(bc.originalSize); }
        if (bc.failed) { w.Key("error"); w.String(bc.note); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    t.SendJson(std::move(w.str()));

    for (auto& bc : buffers) {
        if (bc.failed) continue;
        const StagingChunk& c = _staging[bc.stagingIndex];
        if (!c.mapped) continue;
        JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureBufferData");
        h.Key("id"); h.Uint(bc.id);
        h.Key("size"); h.Uint(bc.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), static_cast<const uint8_t*>(c.mapped) + bc.stagingOffset, (size_t)bc.size);
    }
}

// ---------------------------------------------------------------------------------------------
// Readback

bool CaptureManager::AllocateStaging(DeviceData* dev, VkDeviceSize size, uint32_t& chunkIndex, VkDeviceSize& offset,
                                     VkBuffer* bufferOut) {
    const VkDeviceSize kChunk = 64ull << 20;
    const VkDeviceSize align = 256;
    std::lock_guard lock(_mutex);
    for (uint32_t i = 0; i < _staging.size(); ++i) {
        VkDeviceSize start = (_staging[i].used + align - 1) & ~(align - 1);
        if (start + size <= _staging[i].size) {
            _staging[i].used = start + size;
            chunkIndex = i;
            offset = start;
            if (bufferOut) *bufferOut = _staging[i].buffer;
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
    if (bufferOut) *bufferOut = chunk.buffer;
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
    VkBuffer staging = VK_NULL_HANDLE;
    if (!AllocateStaging(dev, tc.size, chunkIndex, offset, &staging)) return fail("staging allocation failed");
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
    dev->dispatch.CmdCopyImageToBuffer(cb, vi.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

    VkImageMemoryBarrier back = toSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = layout;
    VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.buffer = staging;
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

    // Readbacks recorded into command buffers that were never submitted have no valid data.
    for (auto& tc : textures) {
        if (tc.frame == UINT32_MAX) {
            tc.frame = 0;
            if (!tc.failed) {
                tc.failed = true;
                tc.note = "command buffer was not submitted during the capture";
            }
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
        w.Key("frame"); w.Uint(tc.frame);
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
        h.Key("frame"); h.Uint(tc.frame);
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
