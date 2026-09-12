#include "capture.h"

#include "pipeline_stats.h"

#include "depth_resolve.h"

#include "image_readback.h"
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
    _imageCaptureByView.clear();
    _imageBytes = 0;
    _passTimings.clear();
    _queriesUsed.store(0, std::memory_order_relaxed);
    _statsUsed.store(0, std::memory_order_relaxed);
    if (_options.profilePasses) EnsureQueryPool(dev);
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
    slot->SetCaptureStacks(IsCapturing() && _options.stacktraces);
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
            ReadBackAfterSubmit(dev, queue, rec, scb.commandBufferId, sub.frame);
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
    for (auto& pt : _passTimings) {
        if (pt.frame != UINT32_MAX) continue;
        for (auto& scb : sub.commandBuffers) {
            if (scb.commandBufferId == pt.commandBufferId) {
                pt.frame = sub.frame;
                break;
            }
        }
    }
    _submissions.push_back(std::move(sub));
}

void CaptureManager::OnFrameEnd(DeviceData* dev, VkQueue queue, const VkPresentInfoKHR* info, VkResult result) {
    std::unique_lock lock(_mutex);
    if (_state == State::Armed) {
        // frameIndex was advanced by this frame end: the frame that starts now is `frameIndex`.
        if (_options.atFrame == UINT64_MAX || dev->frameIndex >= _options.atFrame) Start(dev);
        return;
    }
    if (_state != State::Capturing) return;
    Log("capture: frame end (%s, result %d) after %zu submissions", info ? "present" : "no present", (int)result, _submissions.size());

    // The present itself is part of the captured frame.
    if (info) {
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
    if (uint32_t n = _storeAllPasses.exchange(0, std::memory_order_relaxed)) Log("capture: %u render passes ran with their DONT_CARE store ops forced to STORE, so those attachments read back", n);
    if (uint32_t n = _postSubmitReadbacks.exchange(0, std::memory_order_relaxed)) Log("capture: %u attachments of command buffers recorded before the capture were read back after their submission", n);

    // Everything recorded in the frame has been submitted; wait for it so staging data is valid.
    dev->dispatch.DeviceWaitIdle(dev->device);

    SendCommands();
    SendTextures(dev);
    SendBuffers(dev);
    SendPassTimings(dev);
    // The end of the capture's stream, whichever sections it had: a client waiting for the capture
    // (the MCP server) knows nothing more of it is coming.
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureComplete");
        w.Key("frame"); w.Uint(_frameIndex);
        w.Key("frames"); w.Uint(_frameCount);
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
    }
    ReleaseStaging(dev);
    ReleaseQueryPool(dev);

    if (!RecordAlways()) {
        std::unique_lock lock(dev->recorderMutex);
        dev->recorders.clear();
    }
    std::lock_guard lock(_mutex);
    _submissions.clear();
    _textures.clear();
    _buffers.clear();
    _passTimings.clear();
    _state = State::Idle;
    Log("capture sent");
}

static void WriteCommandEntry(JsonWriter& w, uint64_t index, uint32_t frame, const char* method, const char* objectClass,
                              uint64_t objectId, const std::string& args, int64_t result, const std::string& extra,
                              int64_t slot) {
    w.BeginObject();
    w.Key("index"); w.Uint(index);
    w.Key("frame"); w.Uint(frame);
    // Position within the command buffer's recording: what validation messages refer to.
    if (slot >= 0) { w.Key("slot"); w.Uint((uint64_t)slot); }
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
                    int64_t result, const std::string& extra, int64_t slot = -1) {
        begin();
        WriteCommandEntry(batch, index++, frame, method, cls, objectId, args, result, extra, slot);
        if (++inBatch >= kBatch) flush();
    };

    for (auto& s : submissions) {
        emit(s.frame, s.method.c_str(), "VkQueue", s.queueId, s.args, s.result, "");
        for (auto& cb : s.commandBuffers) {
            if (!cb.commands) {
                emit(s.frame, "<unrecorded command buffer>", "VkCommandBuffer", cb.commandBufferId, "", 0, "");
                continue;
            }
            int64_t slot = 0;
            for (auto& c : *cb.commands) {
                emit(s.frame, kVkCommandNames[(int)c.id], "VkCommandBuffer", cb.commandBufferId, c.args, c.result, c.extra, slot++);
            }
        }
    }
    flush();
}

// ---------------------------------------------------------------------------------------------
// Render pass attachments

// ---------------------------------------------------------------------------------------------
// Pass profiling

void CaptureManager::EnsureQueryPool(DeviceData* dev) {
    if (_queryPool && _queryDevice == dev->device) return;
    if (_queryPool) ReleaseQueryPool(dev);
    if (!dev->properties.limits.timestampComputeAndGraphics) {
        Log("pass profiling: timestamps not supported on all queues; skipped");
        return;
    }
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = 16384;   // 8192 passes per capture
    if (dev->dispatch.CreateQueryPool(dev->device, &ci, nullptr, &_queryPool) != VK_SUCCESS) {
        _queryPool = VK_NULL_HANDLE;
        Log("pass profiling: vkCreateQueryPool failed");
        return;
    }
    _queryDevice = dev->device;
    _queryCount = ci.queryCount;

    // The counters behind the GPU Bottlenecks report. Optional: without the feature, or without a
    // pool, passes still carry their durations and the report says the columns are unavailable.
    if (!dev->pipelineStatistics) {
        Log("pass counters: pipelineStatisticsQuery is not enabled on this device");
        return;
    }
    VkQueryPoolCreateInfo sci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    sci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    sci.queryCount = 8192;                        // one per pass, matching the timestamp pairs
    sci.pipelineStatistics = kPipelineStatistics;
    if (dev->dispatch.CreateQueryPool(dev->device, &sci, nullptr, &_statsPool) != VK_SUCCESS) {
        _statsPool = VK_NULL_HANDLE;
        Log("pass counters: vkCreateQueryPool failed");
        return;
    }
    _statsCount = sci.queryCount;

    // The samples that passed each pass's depth and stencil tests, for the depth-rejection rule.
    // Precise counts need occlusionQueryPrecise; without it the query would only answer "any".
    if (!dev->occlusionPrecise) {
        Log("depth rejection: occlusionQueryPrecise is not enabled on this device");
        return;
    }
    VkQueryPoolCreateInfo oci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    oci.queryType = VK_QUERY_TYPE_OCCLUSION;
    oci.queryCount = 8192;                        // one per pass, matching the statistics queries
    if (dev->dispatch.CreateQueryPool(dev->device, &oci, nullptr, &_occlusionPool) != VK_SUCCESS) {
        _occlusionPool = VK_NULL_HANDLE;
        Log("depth rejection: vkCreateQueryPool failed");
        return;
    }
    _occlusionCount = oci.queryCount;
}

void CaptureManager::ReleaseQueryPool(DeviceData* dev) {
    if (!_queryPool) return;
    DeviceData* d = _queryDevice == dev->device ? dev : GetDeviceData(_queryDevice);
    if (d) {
        d->dispatch.DestroyQueryPool(_queryDevice, _queryPool, nullptr);
        if (_statsPool) d->dispatch.DestroyQueryPool(_queryDevice, _statsPool, nullptr);
        if (_occlusionPool) d->dispatch.DestroyQueryPool(_queryDevice, _occlusionPool, nullptr);
    }
    _queryPool = VK_NULL_HANDLE;
    _queryDevice = VK_NULL_HANDLE;
    _queryCount = 0;
    _statsPool = VK_NULL_HANDLE;
    _statsCount = 0;
    _occlusionPool = VK_NULL_HANDLE;
    _occlusionCount = 0;
}

uint32_t CaptureManager::BeginTimestamp(DeviceData* dev, CommandRecorder* rec) {
    if (!IsCapturing() || !_options.profilePasses || !_queryPool || _queryDevice != dev->device) return UINT32_MAX;
    if (rec->renderPassContinue()) return UINT32_MAX;   // secondaries inside a pass: the primary times the pass
    uint32_t q = _queriesUsed.fetch_add(2, std::memory_order_relaxed);
    if (q + 2 > _queryCount) return UINT32_MAX;         // pool exhausted: later passes go untimed
    VkCommandBuffer cb = rec->commandBuffer();
    dev->dispatch.CmdResetQueryPool(cb, _queryPool, q, 2);
    dev->dispatch.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _queryPool, q);
    return q;
}

uint32_t CaptureManager::BeginPipelineStatistics(DeviceData* dev, CommandRecorder* rec) {
    if (!_statsPool || _queryDevice != dev->device) return UINT32_MAX;
    if (rec->renderPassContinue()) return UINT32_MAX;   // the primary brackets the pass
    uint32_t q = _statsUsed.fetch_add(1, std::memory_order_relaxed);
    if (q >= _statsCount) return UINT32_MAX;            // pool exhausted: later passes go uncounted
    VkCommandBuffer cb = rec->commandBuffer();
    // Both of these are outside the render pass: the layer's begin hook runs before the driver's
    // vkCmdBeginRenderPass and its end hook after vkCmdEndRenderPass, so the query brackets the
    // whole pass without landing inside a subpass.
    dev->dispatch.CmdResetQueryPool(cb, _statsPool, q, 1);
    dev->dispatch.CmdBeginQuery(cb, _statsPool, q, 0);
    return q;
}

uint32_t CaptureManager::BeginOcclusion(DeviceData* dev, CommandRecorder* rec) {
    if (!_occlusionPool || _queryDevice != dev->device) return UINT32_MAX;
    if (rec->renderPassContinue()) return UINT32_MAX;   // the primary brackets the pass
    // Two occlusion queries cannot be active at once, and the application's come first.
    if (rec->appQueryDepth) return UINT32_MAX;
    uint32_t q = _occlusionUsed.fetch_add(1, std::memory_order_relaxed);
    if (q >= _occlusionCount) return UINT32_MAX;        // pool exhausted: later passes go uncounted
    VkCommandBuffer cb = rec->commandBuffer();
    dev->dispatch.CmdResetQueryPool(cb, _occlusionPool, q, 1);
    dev->dispatch.CmdBeginQuery(cb, _occlusionPool, q, VK_QUERY_CONTROL_PRECISE_BIT);
    rec->pendingOcclusionQuery = q;
    return q;
}

void CaptureManager::DropOcclusion(DeviceData* dev, CommandRecorder* rec) {
    if (rec->pendingOcclusionQuery == UINT32_MAX || !_occlusionPool || _queryDevice != dev->device) return;
    dev->dispatch.CmdEndQuery(rec->commandBuffer(), _occlusionPool, rec->pendingOcclusionQuery);
    rec->pendingOcclusionQuery = UINT32_MAX;
    rec->pass().occlusionQuery = UINT32_MAX;   // the pass reports no count rather than a partial one
}

void CaptureManager::OnBeforePass(DeviceData* dev, CommandRecorder* rec) {
    OnEndComputePass(dev, rec);
    rec->pendingQuery = BeginTimestamp(dev, rec);
    // Only alongside a timed pass: an uncounted pass would spend a query for nothing, and the
    // report shows the counters against the pass's duration.
    rec->pendingStatsQuery = rec->pendingQuery == UINT32_MAX ? UINT32_MAX : BeginPipelineStatistics(dev, rec);
    if (rec->pendingQuery != UINT32_MAX) BeginOcclusion(dev, rec);
}

void CaptureManager::OnBeforeDispatch(DeviceData* dev, CommandRecorder* rec) {
    ActiveComputePass& c = rec->compute();
    if (c.active || rec->pass().active) return;
    c.active = true;
    c.index = rec->NextComputeIndex();
    c.query = BeginTimestamp(dev, rec);
}

void CaptureManager::OnEndComputePass(DeviceData* dev, CommandRecorder* rec) {
    ActiveComputePass& c = rec->compute();
    if (!c.active) return;
    c.active = false;
    if (c.query == UINT32_MAX || !_queryPool || _queryDevice != dev->device) return;
    dev->dispatch.CmdWriteTimestamp(rec->commandBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queryPool, c.query + 1);
    PassTiming pt;
    pt.commandBufferId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)rec->commandBuffer());
    pt.passIndex = c.index;
    pt.compute = true;
    pt.query = c.query;
    std::lock_guard lock(_mutex);
    _passTimings.push_back(pt);
}

void CaptureManager::OnBeginRenderPass(DeviceData* dev, CommandRecorder* rec, const VkRenderPassBeginInfo* info) {
    ActivePass& p = rec->pass();
    p = ActivePass{};
    p.active = true;
    p.renderPass = info->renderPass;
    p.framebuffer = info->framebuffer;
    p.renderArea = info->renderArea;
    p.passIndex = rec->NextPassIndex();
    p.query = rec->pendingQuery;
    p.statsQuery = rec->pendingStatsQuery;
    p.occlusionQuery = rec->pendingOcclusionQuery;
    rec->pendingQuery = UINT32_MAX;
    rec->pendingStatsQuery = UINT32_MAX;

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
        // Multiview renders the view mask's layers with a one-layer framebuffer (stereo: both eyes).
        p.layerCount = std::max(p.layerCount, rp.viewLayers);
    }
}

void CaptureManager::OnBeginRendering(DeviceData* dev, CommandRecorder* rec, const VkRenderingInfo* info) {
    ActivePass& p = rec->pass();
    p = ActivePass{};
    p.active = true;
    p.dynamic = true;
    p.renderArea = info->renderArea;
    p.layerCount = info->layerCount;
    for (uint32_t bit = 0; bit < 32; ++bit)
        if (info->viewMask & (1u << bit)) p.layerCount = std::max(p.layerCount, bit + 1);   // multiview
    p.passIndex = rec->NextPassIndex();
    p.query = rec->pendingQuery;
    p.statsQuery = rec->pendingStatsQuery;
    p.occlusionQuery = rec->pendingOcclusionQuery;
    rec->pendingQuery = UINT32_MAX;
    rec->pendingStatsQuery = UINT32_MAX;
    auto add = [&](const VkRenderingAttachmentInfo* a) {
        if (!a || !a->imageView) return;
        p.attachments.push_back(a->imageView);
        p.layouts.push_back(a->imageLayout);
        p.resolveViews.push_back(a->resolveMode != VK_RESOLVE_MODE_NONE ? a->resolveImageView : VK_NULL_HANDLE);
        p.resolveLayouts.push_back(a->resolveImageLayout);
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
    const bool readBack = IsCapturing() && _options.captureTextures;
    if (readBack) {
        for (uint32_t i = 0; i < p.attachments.size(); ++i) {
            CaptureAttachment(dev, rec, i, p.attachments[i], i < p.layouts.size() ? p.layouts[i] : VK_IMAGE_LAYOUT_GENERAL);
            // Dynamic rendering resolves into a separate target that is not among the attachments
            // (a render pass lists its resolve attachments in the framebuffer).
            if (i < p.resolveViews.size() && p.resolveViews[i])
                CaptureAttachment(dev, rec, i, p.resolveViews[i], p.resolveLayouts[i], true);
        }
    }
    rec->passes().push_back({p.attachments, p.layouts, p.resolveViews, p.resolveLayouts, p.passIndex, p.layerCount, readBack});
    p.active = false;
    // The pass's end timestamp: after every command of the pass has completed.
    // The occlusion query, unless it was dropped when the application opened one of its own.
    const uint32_t occlusion = rec->pendingOcclusionQuery;
    if (occlusion != UINT32_MAX && _occlusionPool && _queryDevice == dev->device) {
        dev->dispatch.CmdEndQuery(rec->commandBuffer(), _occlusionPool, occlusion);
        rec->pendingOcclusionQuery = UINT32_MAX;
    }
    if (p.query != UINT32_MAX && _queryPool && _queryDevice == dev->device) {
        dev->dispatch.CmdWriteTimestamp(rec->commandBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _queryPool, p.query + 1);
        if (p.statsQuery != UINT32_MAX && _statsPool) dev->dispatch.CmdEndQuery(rec->commandBuffer(), _statsPool, p.statsQuery);
        PassTiming pt;
        pt.commandBufferId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)rec->commandBuffer());
        pt.passIndex = p.passIndex;
        pt.query = p.query;
        pt.statsQuery = p.statsQuery;
        pt.occlusionQuery = occlusion;
        std::lock_guard lock(_mutex);
        _passTimings.push_back(pt);
    } else if (p.statsQuery != UINT32_MAX && _statsPool && _queryDevice == dev->device) {
        // Begun but not going to be reported: it still has to end, or the command buffer is invalid.
        dev->dispatch.CmdEndQuery(rec->commandBuffer(), _statsPool, p.statsQuery);
    }
    // Buffers and images bound during the pass are copied now that transfer commands are allowed again.
    FlushBufferCopies(dev, rec);
    FlushImageCopies(dev, rec);
}

void CaptureManager::OnExecuteCommands(DeviceData* dev, CommandRecorder* rec, uint32_t count,
                                       const VkCommandBuffer* secondaries) {
    if (!rec || !secondaries) return;
    auto& dst = rec->pendingCopies();
    auto& dstImages = rec->pendingImages();
    for (uint32_t i = 0; i < count; ++i) {
        CommandRecorder* sec = RecorderFor(dev, secondaries[i]);
        if (!sec) continue;
        auto& src = sec->pendingCopies();
        dst.insert(dst.end(), src.begin(), src.end());
        src.clear();
        auto& srcImages = sec->pendingImages();
        dstImages.insert(dstImages.end(), srcImages.begin(), srcImages.end());
        srcImages.clear();
    }
    if (!rec->InsidePass()) {
        FlushBufferCopies(dev, rec);
        FlushImageCopies(dev, rec);
    }
}

// ---------------------------------------------------------------------------------------------
// Sampled image readback

uint32_t CaptureManager::QueueImageCapture(DeviceData* dev, CommandRecorder* rec, VkImageView view, VkImageLayout layout) {
    if (!rec || !view || !IsCapturing() || !_options.captureImages) return 0;
    {
        std::lock_guard lock(_mutex);
        auto it = _imageCaptureByView.find((uint64_t)(uintptr_t)view);
        if (it != _imageCaptureByView.end()) return it->second;
    }
    ResourceRegistry& reg = ResourceRegistry::Get();
    ImageViewInfo vi;
    ImageInfo img;
    if (!reg.GetImageView(view, vi) || !reg.GetImage(vi.image, img)) return 0;

    TextureCapture tc;
    tc.sampled = true;
    tc.recorded = false;
    tc.imageId = Tracker::Get().Resolve(HT_VkImage, (uint64_t)(uintptr_t)vi.image);
    tc.viewId = Tracker::Get().Resolve(HT_VkImageView, (uint64_t)(uintptr_t)view);
    tc.format = img.format;
    tc.mip = vi.range.baseMipLevel;
    tc.baseLayer = vi.range.baseArrayLayer;
    tc.width = std::max(1u, img.extent.width >> tc.mip);
    tc.height = std::max(1u, img.extent.height >> tc.mip);
    tc.depth = std::max(1u, img.extent.depth >> tc.mip);
    tc.layers = vi.range.layerCount == VK_REMAINING_ARRAY_LAYERS ? img.arrayLayers - vi.range.baseArrayLayer : vi.range.layerCount;
    tc.layers = std::max(1u, tc.layers);
    VkImageAspectFlags aspects = FormatAspects(img.format);
    tc.aspect = aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT
              : aspects & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT
              : VK_IMAGE_ASPECT_COLOR_BIT;

    // Registers the capture (failed or pending) under the view, so every later binding of the
    // same view in this capture refers to it.
    auto add = [&]() {
        std::lock_guard lock(_mutex);
        tc.captureId = (uint32_t)_textures.size() + 1;
        _textures.push_back(tc);
        _imageCaptureByView[(uint64_t)(uintptr_t)view] = tc.captureId;
        return tc.captureId;
    };
    auto fail = [&](const char* why) {
        tc.failed = true;
        tc.recorded = true;
        tc.note = why;
        return add();
    };
    tc.samples = (uint32_t)img.samples;
    if (!img.transferSrc) return fail("image lacks TRANSFER_SRC usage");
    if (img.samples != VK_SAMPLE_COUNT_1_BIT && tc.aspect == VK_IMAGE_ASPECT_STENCIL_BIT)
        return fail("multisampled stencil image (no stencil resolve)");
    if (img.samples != VK_SAMPLE_COUNT_1_BIT && tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && !CanResolveDepth(dev))
        return fail("multisampled depth image (the depth resolve needs dynamic rendering, Vulkan 1.2+)");
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
        if (!LayoutTracker::Get().GetLayout(vi.image, layout) || layout == VK_IMAGE_LAYOUT_UNDEFINED) return fail("unknown image layout");
    }
    FormatBlock block = FormatBlockInfo(img.format, tc.aspect);
    if (block.bytes == 0) return fail("unsupported format for readback");
    // Every mip of the view, back to back (each mip: its layers), so the viewer can show them.
    uint32_t mips = vi.range.levelCount == VK_REMAINING_MIP_LEVELS ? img.mipLevels - tc.mip : vi.range.levelCount;
    if (tc.mip >= img.mipLevels) mips = 1;
    else if (tc.mip + mips > img.mipLevels) mips = img.mipLevels - tc.mip;
    tc.mips = std::max(1u, mips);
    std::vector<VkDeviceSize> mipSizes(tc.mips);
    tc.size = 0;
    for (uint32_t m = 0; m < tc.mips; ++m) {
        const uint32_t w = std::max(1u, img.extent.width >> (tc.mip + m));
        const uint32_t h = std::max(1u, img.extent.height >> (tc.mip + m));
        const uint32_t d = std::max(1u, img.extent.depth >> (tc.mip + m));
        mipSizes[m] = (VkDeviceSize)((w + block.width - 1) / block.width) * ((h + block.height - 1) / block.height) * d * tc.layers * block.bytes;
        tc.size += mipSizes[m];
    }
    if (tc.size > _options.maxTextureSize) return fail("exceeds max texture size");
    bool overBudget = false;
    {
        std::lock_guard lock(_mutex);
        overBudget = _imageBytes + tc.size > _options.maxImageTotal;
        if (!overBudget) _imageBytes += tc.size;
    }
    if (overBudget) return fail("image capture budget exceeded");
    uint32_t chunkIndex = 0;
    VkDeviceSize offset = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    if (!AllocateStaging(dev, tc.size, chunkIndex, offset, &staging)) return fail("staging allocation failed");
    VkImage resolve = VK_NULL_HANDLE;
    if (img.samples != VK_SAMPLE_COUNT_1_BIT && !AllocateResolveImage(dev, img, tc.mip, tc.layers, &resolve))
        return fail("resolve image allocation failed");
    tc.stagingIndex = chunkIndex;
    tc.stagingOffset = offset;
    uint32_t id = add();

    VkDeviceSize mipOffset = 0;
    for (uint32_t m = 0; m < tc.mips; ++m) {
        PendingImageCopy p;
        p.resolve = m == 0 ? resolve : VK_NULL_HANDLE;   // multisampled images have one mip
        p.captureId = id;
        p.image = vi.image;
        p.layout = layout;
        p.range = {aspects, tc.mip + m, 1, tc.baseLayer, tc.layers};
        p.copyAspect = tc.aspect;
        p.extent = {std::max(1u, img.extent.width >> (tc.mip + m)), std::max(1u, img.extent.height >> (tc.mip + m)),
                    std::max(1u, img.extent.depth >> (tc.mip + m))};
        p.staging = staging;
        p.stagingOffset = offset + mipOffset;
        p.size = mipSizes[m];
        p.format = img.format;
        mipOffset += mipSizes[m];
        if (p.resolve && tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && !PrepareDepthResolve(dev, p)) {
            std::lock_guard lock(_mutex);
            TextureCapture& t = _textures[id - 1];
            t.failed = true;
            t.recorded = true;
            t.note = "depth resolve views could not be created";
            return id;
        }
        rec->pendingImages().push_back(p);
    }
    if (!rec->InsidePass()) FlushImageCopies(dev, rec);
    return id;
}

void CaptureManager::FlushImageCopies(DeviceData* dev, CommandRecorder* rec) {
    auto& pending = rec->pendingImages();
    if (pending.empty()) return;
    VkCommandBuffer cb = rec->commandBuffer();
    for (const PendingImageCopy& p : pending) RecordImageCopy(dev, cb, p);
    uint64_t cbId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)cb);
    std::lock_guard lock(_mutex);
    for (const PendingImageCopy& p : pending) {
        if (p.captureId == 0 || p.captureId > _textures.size()) continue;
        TextureCapture& tc = _textures[p.captureId - 1];
        tc.recorded = true;
        tc.commandBufferId = cbId;
    }
    pending.clear();
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

bool CreateResolveImage(DeviceData* dev, const ImageInfo& img, uint32_t mip, uint32_t layers, VkImage* image,
                        VkDeviceMemory* memory) {
    const DeviceDispatch& d = dev->dispatch;
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = img.type;
    ici.format = img.format;
    ici.extent = {std::max(1u, img.extent.width >> mip), std::max(1u, img.extent.height >> mip),
                  std::max(1u, img.extent.depth >> mip)};
    ici.mipLevels = 1;
    ici.arrayLayers = std::max(1u, layers);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Depth is resolved by a render pass into the image (depth_resolve.h).
    if (FormatAspects(img.format) & VK_IMAGE_ASPECT_DEPTH_BIT) ici.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.CreateImage(dev->device, &ici, nullptr, image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    d.GetImageMemoryRequirements(dev->device, *image, &req);
    int typeIndex = -1;
    for (int pass = 0; pass < 2 && typeIndex < 0; ++pass) {
        VkMemoryPropertyFlags want = pass == 0 ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0;
        for (uint32_t i = 0; i < dev->memoryProperties.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) && (dev->memoryProperties.memoryTypes[i].propertyFlags & want) == want) {
                typeIndex = (int)i;
                break;
            }
        }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)std::max(0, typeIndex);
    if (typeIndex < 0 || d.AllocateMemory(dev->device, &mai, nullptr, memory) != VK_SUCCESS ||
        d.BindImageMemory(dev->device, *image, *memory, 0) != VK_SUCCESS) {
        d.DestroyImage(dev->device, *image, nullptr);
        if (*memory) d.FreeMemory(dev->device, *memory, nullptr);
        *image = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void RecordImageCopy(DeviceData* dev, VkCommandBuffer cb, const PendingImageCopy& p) {
    const DeviceDispatch& d = dev->dispatch;
    VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = p.layout;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = p.image;
    toSrc.subresourceRange = p.range;
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toSrc);

    VkImage source = p.image;
    VkImageSubresourceLayers sub{p.copyAspect, p.range.baseMipLevel, p.range.baseArrayLayer, p.range.layerCount};
    if (p.resolve && p.srcView) {
        // Multisampled depth: a render pass resolves it (the barrier above is undone first: the
        // resolve needs the attachment layout, and leaves the image in p.layout again).
        VkImageMemoryBarrier undo = toSrc;
        undo.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        undo.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        undo.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        undo.newLayout = p.layout;
        d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &undo);
        RecordDepthResolve(dev, cb, p);
        source = p.resolve;
        sub = {p.copyAspect, 0, 0, p.range.layerCount};
        // The image is already back in p.layout; the copy below reads the resolve image only.
        VkBufferImageCopy region{};
        region.bufferOffset = p.stagingOffset;
        region.imageSubresource = sub;
        region.imageExtent = p.extent;
        d.CmdCopyImageToBuffer(cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.staging, 1, &region);
        VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.buffer = p.staging;
        hostRead.offset = p.stagingOffset;
        hostRead.size = p.size;
        d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &hostRead, 0, nullptr);
        return;
    }
    if (p.resolve) {
        // Multisampled: resolve into the single-sampled image, then copy from that.
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = p.resolve;
        toDst.subresourceRange = {p.copyAspect, 0, 1, 0, p.range.layerCount};
        d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toDst);
        VkImageResolve region{};
        region.srcSubresource = sub;
        region.dstSubresource = {p.copyAspect, 0, 0, p.range.layerCount};
        region.extent = p.extent;
        d.CmdResolveImage(cb, p.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.resolve, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          1, &region);
        VkImageMemoryBarrier resolved = toDst;
        resolved.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolved.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        resolved.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolved.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &resolved);
        source = p.resolve;
        sub = region.dstSubresource;
    }

    VkBufferImageCopy region{};
    region.bufferOffset = p.stagingOffset;
    region.imageSubresource = sub;
    region.imageExtent = p.extent;
    d.CmdCopyImageToBuffer(cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.staging, 1, &region);

    VkImageMemoryBarrier back = toSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = p.layout;
    VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.buffer = p.staging;
    hostRead.offset = p.stagingOffset;
    hostRead.size = p.size;
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &hostRead, 1, &back);
}

bool CaptureManager::AllocateResolveImage(DeviceData* dev, const ImageInfo& img, uint32_t mip, uint32_t layers, VkImage* out) {
    ResolveImage ri;
    if (!CreateResolveImage(dev, img, mip, layers, &ri.image, &ri.memory)) return false;
    std::lock_guard lock(_mutex);
    _resolveImages.push_back(ri);
    *out = ri.image;
    return true;
}

bool CaptureManager::PrepareDepthResolve(DeviceData* dev, PendingImageCopy& p) {
    if (!CanResolveDepth(dev)) return false;
    if (!CreateDepthResolveViews(dev, p, &p.srcView, &p.dstView)) return false;
    std::lock_guard lock(_mutex);
    _resolveViews.push_back(p.srcView);
    _resolveViews.push_back(p.dstView);
    return true;
}

void CaptureManager::CaptureAttachment(DeviceData* dev, CommandRecorder* rec, uint32_t attachmentIndex,
                                       VkImageView view, VkImageLayout layout, bool resolveTarget) {
    TextureCapture tc;
    PendingImageCopy p;
    const uint64_t cbId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)rec->commandBuffer());
    if (!PrepareAttachment(dev, cbId, rec->pass().passIndex, rec->pass().layerCount, attachmentIndex, view, layout, resolveTarget, tc, p)) return;
    RecordImageCopy(dev, rec->commandBuffer(), p);
    std::lock_guard lock(_mutex);
    _textures.push_back(tc);
}

bool CaptureManager::PrepareAttachment(DeviceData* dev, uint64_t commandBufferId, uint32_t passIndex, uint32_t layerCount,
                                       uint32_t attachmentIndex, VkImageView view, VkImageLayout layout, bool resolveTarget,
                                       TextureCapture& tc, PendingImageCopy& p) {
    ResourceRegistry& reg = ResourceRegistry::Get();
    ImageViewInfo vi;
    ImageInfo img;
    if (!reg.GetImageView(view, vi) || !reg.GetImage(vi.image, img)) return false;

    tc = TextureCapture{};
    tc.imageId = Tracker::Get().Resolve(HT_VkImage, (uint64_t)(uintptr_t)vi.image);
    tc.commandBufferId = commandBufferId;
    tc.passIndex = passIndex;
    tc.attachment = attachmentIndex;
    tc.resolveTarget = resolveTarget;
    tc.format = img.format;
    tc.samples = (uint32_t)img.samples;
    tc.mip = vi.range.baseMipLevel;
    tc.width = std::max(1u, img.extent.width >> tc.mip);
    tc.height = std::max(1u, img.extent.height >> tc.mip);
    tc.depth = std::max(1u, img.extent.depth >> tc.mip);
    tc.layers = vi.range.layerCount == VK_REMAINING_ARRAY_LAYERS ? img.arrayLayers - vi.range.baseArrayLayer
                                                                  : vi.range.layerCount;
    tc.layers = std::max(1u, std::min(tc.layers, layerCount));
    tc.aspect = FormatAspects(img.format) & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                      : VK_IMAGE_ASPECT_COLOR_BIT;

    auto fail = [&](const char* why) {
        tc.failed = true;
        tc.note = why;
        std::lock_guard lock(_mutex);
        _textures.push_back(tc);
        return false;
    };
    if (!img.transferSrc) return fail("image lacks TRANSFER_SRC usage");
    if (img.samples != VK_SAMPLE_COUNT_1_BIT && tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && !CanResolveDepth(dev))
        return fail("multisampled depth attachment (the depth resolve needs dynamic rendering, Vulkan 1.2+)");
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
    VkImage resolve = VK_NULL_HANDLE;
    if (img.samples != VK_SAMPLE_COUNT_1_BIT && !AllocateResolveImage(dev, img, tc.mip, tc.layers, &resolve))
        return fail("resolve image allocation failed");
    tc.stagingIndex = chunkIndex;
    tc.stagingOffset = offset;

    p = PendingImageCopy{};
    p.image = vi.image;
    p.layout = layout;
    p.range = {tc.aspect, tc.mip, 1, vi.range.baseArrayLayer, tc.layers};
    if (tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (FormatAspects(img.format) & VK_IMAGE_ASPECT_STENCIL_BIT))
        p.range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;  // barriers must cover both aspects
    p.copyAspect = tc.aspect;
    p.extent = {tc.width, tc.height, tc.depth};
    p.staging = staging;
    p.stagingOffset = offset;
    p.size = tc.size;
    p.resolve = resolve;
    p.format = img.format;
    if (resolve && tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && !PrepareDepthResolve(dev, p)) return fail("depth resolve views could not be created");
    return true;
}

void CaptureManager::ReadBackAfterSubmit(DeviceData* dev, VkQueue queue, CommandRecorder* rec, uint64_t commandBufferId, uint32_t frame) {
    if (!_options.captureTextures) return;
    std::vector<std::pair<TextureCapture, PendingImageCopy>> copies;
    for (const RecordedPass& pass : rec->passes()) {
        if (pass.readBack) continue;
        auto prepare = [&](uint32_t i, VkImageView view, VkImageLayout recordedLayout, bool resolveTarget) {
            if (!view) return;
            // The layout now, after the whole submission (later passes or barriers may have
            // changed it), from the tracker; the pass's own final layout otherwise.
            ImageViewInfo vi;
            VkImageLayout layout = recordedLayout;
            VkImageLayout tracked = VK_IMAGE_LAYOUT_UNDEFINED;
            if (ResourceRegistry::Get().GetImageView(view, vi) && LayoutTracker::Get().GetLayout(vi.image, tracked) && tracked != VK_IMAGE_LAYOUT_UNDEFINED) layout = tracked;
            TextureCapture tc;
            PendingImageCopy p;
            if (PrepareAttachment(dev, commandBufferId, pass.passIndex, pass.layerCount, i, view, layout, resolveTarget, tc, p)) {
                tc.frame = frame;
                copies.emplace_back(tc, p);
            }
        };
        for (uint32_t i = 0; i < pass.attachments.size(); ++i) {
            prepare(i, pass.attachments[i], i < pass.layouts.size() ? pass.layouts[i] : VK_IMAGE_LAYOUT_GENERAL, false);
            if (i < pass.resolveViews.size() && pass.resolveViews[i]) prepare(i, pass.resolveViews[i], pass.resolveLayouts[i], true);
        }
    }
    if (copies.empty()) return;

    // A command buffer of the layer's on the same queue, right behind the application's submission.
    uint32_t family = 0;
    {
        std::lock_guard lock(dev->queueMutex);
        auto it = dev->queueFamilies.find(queue);
        if (it == dev->queueFamilies.end()) return;
        family = it->second;
    }
    const DeviceDispatch& d = dev->dispatch;
    VkCommandPool pool = VK_NULL_HANDLE;
    {
        std::lock_guard lock(dev->queueMutex);
        auto it = dev->readbackPools.find(family);
        if (it != dev->readbackPools.end()) pool = it->second;
    }
    if (!pool) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = family;
        if (d.CreateCommandPool(dev->device, &pci, nullptr, &pool) != VK_SUCCESS) return;
        std::lock_guard lock(dev->queueMutex);
        dev->readbackPools[family] = pool;
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (d.AllocateCommandBuffers(dev->device, &ai, &cb) != VK_SUCCESS) return;
    // The loader's dispatch pointer, which a command buffer allocated by a layer lacks (see image_readback.cpp).
    *reinterpret_cast<void**>(cb) = *reinterpret_cast<void**>(dev->device);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d.BeginCommandBuffer(cb, &bi);
    for (auto& c : copies) RecordImageCopy(dev, cb, c.second);
    d.EndCommandBuffer(cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    d.CreateFence(dev->device, &fci, nullptr, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult res = d.QueueSubmit(queue, 1, &si, fence);
    if (res == VK_SUCCESS) res = d.WaitForFences(dev->device, 1, &fence, VK_TRUE, 5000000000ull);
    d.DestroyFence(dev->device, fence, nullptr);
    d.FreeCommandBuffers(dev->device, pool, 1, &cb);
    std::lock_guard lock(_mutex);
    for (auto& c : copies) {
        if (res != VK_SUCCESS) { c.first.failed = true; c.first.note = "read-back after submission failed"; }
        _textures.push_back(c.first);
    }
    if (res == VK_SUCCESS) _postSubmitReadbacks.fetch_add((uint32_t)copies.size(), std::memory_order_relaxed);
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
        if (!tc.recorded && !tc.failed) {
            tc.failed = true;
            tc.note = "copy was never recorded (secondary command buffer not executed)";
        }
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
        if (tc.mips > 1) { w.Key("mips"); w.Uint(tc.mips); }
        w.Key("size"); w.Uint(tc.failed ? 0 : tc.size);
        if (tc.samples > 1) { w.Key("samples"); w.Uint(tc.samples); }
        if (tc.resolveTarget) { w.Key("resolve"); w.Boolean(true); }
        if (tc.sampled) {
            w.Key("kind"); w.String("sampled");
            w.Key("capture"); w.Uint(tc.captureId);
            w.Key("view"); w.Uint(tc.viewId);
            w.Key("baseLayer"); w.Uint(tc.baseLayer);
        }
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
        if (tc.sampled) { h.Key("capture"); h.Uint(tc.captureId); }
        h.Key("size"); h.Uint(tc.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), static_cast<const uint8_t*>(c.mapped) + tc.stagingOffset, (size_t)tc.size);
    }
}

void CaptureManager::SendPassTimings(DeviceData* dev) {
    std::vector<PassTiming> timings;
    {
        std::lock_guard lock(_mutex);
        timings = _passTimings;
    }
    if (!_queryPool || _queryDevice != dev->device) return;
    uint32_t used = std::min(_queriesUsed.load(std::memory_order_relaxed), _queryCount);
    if (!used) return;
    // Each query: 64-bit value then 64-bit availability (0 when the command buffer never ran).
    std::vector<uint64_t> results((size_t)used * 2, 0);
    VkResult res = dev->dispatch.GetQueryPoolResults(dev->device, _queryPool, 0, used, results.size() * sizeof(uint64_t),
                                                     results.data(), 2 * sizeof(uint64_t),
                                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (res != VK_SUCCESS && res != VK_NOT_READY) {
        Log("pass profiling: vkGetQueryPoolResults failed (%d)", (int)res);
        return;
    }
    // The pass counters, when the device had a statistics pool. Each query writes one value per
    // requested statistic and then its availability (pipeline_stats.h).
    constexpr size_t kStatsStride = kPipelineStatisticCount + 1;
    std::vector<uint64_t> stats;
    uint32_t statsUsed = 0;
    if (_statsPool) {
        statsUsed = std::min(_statsUsed.load(std::memory_order_relaxed), _statsCount);
        if (statsUsed) {
            stats.assign((size_t)statsUsed * kStatsStride, 0);
            VkResult sres = dev->dispatch.GetQueryPoolResults(
                dev->device, _statsPool, 0, statsUsed, stats.size() * sizeof(uint64_t), stats.data(),
                kStatsStride * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (sres != VK_SUCCESS && sres != VK_NOT_READY) {
                Log("pass counters: vkGetQueryPoolResults failed (%d)", (int)sres);
                stats.clear();
                statsUsed = 0;
            }
        }
    }

    // The samples that passed each pass's depth and stencil tests: one value and its availability.
    std::vector<uint64_t> occlusion;
    uint32_t occlusionUsed = 0;
    if (_occlusionPool) {
        occlusionUsed = std::min(_occlusionUsed.load(std::memory_order_relaxed), _occlusionCount);
        if (occlusionUsed) {
            occlusion.assign((size_t)occlusionUsed * 2, 0);
            VkResult ores = dev->dispatch.GetQueryPoolResults(
                dev->device, _occlusionPool, 0, occlusionUsed, occlusion.size() * sizeof(uint64_t), occlusion.data(),
                2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (ores != VK_SUCCESS && ores != VK_NOT_READY) {
                Log("depth rejection: vkGetQueryPoolResults failed (%d)", (int)ores);
                occlusion.clear();
                occlusionUsed = 0;
            }
        }
    }

    const double period = dev->properties.limits.timestampPeriod;   // nanoseconds per tick
    uint64_t earliest = UINT64_MAX;
    for (auto& pt : timings) {
        if (pt.frame == UINT32_MAX || pt.query + 1 >= used) continue;
        if (results[(size_t)pt.query * 2 + 1] && results[((size_t)pt.query + 1) * 2 + 1]) {
            earliest = std::min(earliest, results[(size_t)pt.query * 2]);
        }
    }
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CapturePassTimings");
    w.Key("timestampPeriodNs"); w.Double(period);
    w.Key("passes"); w.BeginArray();
    uint32_t sent = 0;
    uint32_t counted = 0;
    for (auto& pt : timings) {
        if (pt.frame == UINT32_MAX || pt.query + 1 >= used) continue;
        uint64_t begin = results[(size_t)pt.query * 2];
        uint64_t end = results[((size_t)pt.query + 1) * 2];
        if (!results[(size_t)pt.query * 2 + 1] || !results[((size_t)pt.query + 1) * 2 + 1] || end < begin) continue;
        w.BeginObject();
        w.Key("frame"); w.Uint(pt.frame);
        w.Key("commandBuffer"); w.Uint(pt.commandBufferId);
        w.Key("passIndex"); w.Uint(pt.passIndex);
        w.Key("kind"); w.String(pt.compute ? "compute" : "render");
        w.Key("startMs"); w.Double((double)(begin - earliest) * period / 1e6);
        w.Key("durationMs"); w.Double((double)(end - begin) * period / 1e6);
        const uint64_t* v = pt.statsQuery != UINT32_MAX && pt.statsQuery < statsUsed && !stats.empty()
                          ? &stats[(size_t)pt.statsQuery * kStatsStride] : nullptr;
        const bool hasStats = v && v[kPipelineStatisticCount];   // the pass ran and its counters are readable
        const bool hasOcclusion = pt.occlusionQuery != UINT32_MAX && pt.occlusionQuery < occlusionUsed
                               && !occlusion.empty() && occlusion[(size_t)pt.occlusionQuery * 2 + 1];
        if (hasStats || hasOcclusion) {
            w.Key("counters"); w.BeginObject();
            if (hasStats) {
                for (uint32_t i = 0; i < kPipelineStatisticCount; ++i) {
                    w.Key(kPipelineStatisticNames[i]);
                    w.Uint(v[i]);
                }
            }
            // The Metal library's name for the same quantity: samples that survived the tests.
            if (hasOcclusion) {
                w.Key("fragmentsPassed");
                w.Uint(occlusion[(size_t)pt.occlusionQuery * 2]);
            }
            w.EndObject();
            counted++;
        }
        w.EndObject();
        sent++;
    }
    w.EndArray();
    w.Key("count"); w.Uint(sent);
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    Log("pass profiling: %u of %zu passes timed, %u with counters", sent, timings.size(), counted);
}

void CaptureManager::ReleaseStaging(DeviceData* dev) {
    std::lock_guard lock(_mutex);
    for (auto& c : _staging) {
        if (c.mapped) dev->dispatch.UnmapMemory(dev->device, c.memory);
        dev->dispatch.DestroyBuffer(dev->device, c.buffer, nullptr);
        dev->dispatch.FreeMemory(dev->device, c.memory, nullptr);
    }
    _staging.clear();
    for (VkImageView v : _resolveViews) dev->dispatch.DestroyImageView(dev->device, v, nullptr);
    _resolveViews.clear();
    for (auto& r : _resolveImages) {
        dev->dispatch.DestroyImage(dev->device, r.image, nullptr);
        dev->dispatch.FreeMemory(dev->device, r.memory, nullptr);
    }
    _resolveImages.clear();
}

} // namespace vkinsp
