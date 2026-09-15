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
    _imageStates.clear();
    _passTimings.clear();
    _commandTotal = 0;
    _frameIndex = dev->frameIndex;
    _homeDevice = dev->device;
    _frameCount = std::max(1u, _options.frameCount);
    _state = State::Capturing;
    _armedAtFrame.store(UINT64_MAX, std::memory_order_release);
    _capturing.store(true, std::memory_order_release);
    g_captureActive.store(true, std::memory_order_release);
    CaptureFor(dev);
    Log("capture started at frame %llu", (unsigned long long)_frameIndex);
}

CaptureManager::DeviceCapture* CaptureManager::CaptureFor(DeviceData* dev) {
    if (!dev || !IsCapturing()) return nullptr;
    std::lock_guard lock(_devicesMutex);
    auto& slot = _devices[dev->device];
    if (!slot) {
        slot = std::make_unique<DeviceCapture>();
        slot->dev = dev;
        slot->startFrame = dev->frameIndex;
        if (_options.profilePasses) CreateQueryPools(*slot);
        if (dev->device != _homeDevice) Log("capture: device %p takes part as well (frame %llu)", (void*)dev->device, (unsigned long long)dev->frameIndex);
    }
    return slot.get();
}

CaptureManager::DeviceCapture* CaptureManager::FindCapture(VkDevice device) {
    std::lock_guard lock(_devicesMutex);
    auto it = _devices.find(device);
    return it == _devices.end() ? nullptr : it->second.get();
}

uint32_t CaptureManager::FrameOf(DeviceData* dev) {
    if (dev->device == _homeDevice) return (uint32_t)(dev->frameIndex - _frameIndex);
    // Another device counts its frames from when it joined the capture.
    const DeviceCapture* dc = CaptureFor(dev);
    return dc && dev->frameIndex >= dc->startFrame ? (uint32_t)(dev->frameIndex - dc->startFrame) : 0;
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
    if (_armedAtFrame.load(std::memory_order_acquire) == dev->frameIndex &&
        (dev->presentSeen.load(std::memory_order_relaxed) || !_presentSeen.load(std::memory_order_relaxed))) {
        std::lock_guard lock(_mutex);
        if (_state == State::Armed) Start(dev);
    }
    if (!IsCapturing() && !RecordAlways()) return;
    std::unique_lock lock(dev->recorderMutex);
    auto& slot = dev->recorders[cb];
    if (!slot) slot = std::make_unique<CommandRecorder>(dev->device, cb, &Tracker::Get());
    slot->Reset((flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) != 0);
    slot->SetCaptureStacks(IsCapturing() && _options.stacktraces);
    lock.unlock();
    CaptureFor(dev);   // the device takes part: its recorders are released with the capture
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

bool CaptureManager::NeedsSubmitReadBack(DeviceData* dev, VkCommandBuffer cb) {
    if (!IsCapturing() || !_options.captureTextures) return false;
    CommandRecorder* rec = RecorderFor(dev, cb);
    if (!rec) return false;
    for (const RecordedPass& pass : rec->passes())
        if (!pass.readBack && !pass.attachments.empty()) return true;
    return false;
}

void CaptureManager::ReadBackSubmitted(DeviceData* dev, VkQueue queue, VkCommandBuffer cb) {
    CommandRecorder* rec = RecorderFor(dev, cb);
    if (!rec || !IsCapturing()) return;
    ReadBackAfterSubmit(dev, queue, rec, Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)cb), FrameOf(dev));
}

void CaptureManager::OnSubmit(DeviceData* dev, VkQueue queue, const std::string& method, std::string args,
                              int64_t result, const std::vector<VkCommandBuffer>& commandBuffers,
                              const std::vector<VkCommandBuffer>& readBack) {
    if (!IsCapturing()) return;
    CaptureSubmission sub;
    sub.queueId = Tracker::Get().Resolve(HT_VkQueue, (uint64_t)(uintptr_t)queue);
    sub.method = method;
    sub.args = std::move(args);
    sub.result = result;
    sub.frame = FrameOf(dev);
    for (VkCommandBuffer cb : commandBuffers) {
        SubmittedCommandBuffer scb;
        scb.commandBufferId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)cb);
        if (CommandRecorder* rec = RecorderFor(dev, cb)) {
            scb.commands = rec->Snapshot();
            _commandTotal += scb.commands->size();
            if (std::find(readBack.begin(), readBack.end(), cb) == readBack.end())
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
    if (info) _presentSeen.store(true, std::memory_order_relaxed);
    std::unique_lock lock(_mutex);
    if (_state == State::Armed) {
        // A process that presents has its frames ended by its presents: another device's
        // substitute boundary (a compute device waiting on its fences) does not start the capture.
        if (!info && _presentSeen.load(std::memory_order_relaxed)) return;
        // frameIndex was advanced by this frame end: the frame that starts now is `frameIndex`.
        if (_options.atFrame == UINT64_MAX || dev->frameIndex >= _options.atFrame) Start(dev);
        return;
    }
    if (_state != State::Capturing) return;
    const bool home = dev->device == _homeDevice;
    if (home) Log("capture: frame end (%s, result %d) after %zu submissions", info ? "present" : "no present", (int)result, _submissions.size());

    // The present itself is part of the captured frame (frameIndex has already moved past it).
    if (info) {
        JsonWriter w(&Tracker::Get());
        ArgsToJson_vkQueuePresentKHR(w, queue, info);
        CaptureSubmission sub;
        sub.queueId = Tracker::Get().Resolve(HT_VkQueue, (uint64_t)(uintptr_t)queue);
        sub.method = "vkQueuePresentKHR";
        sub.args = std::move(w.str());
        sub.result = (int64_t)result;
        const uint32_t frame = FrameOf(dev);
        sub.frame = frame ? frame - 1 : 0;
        _submissions.push_back(std::move(sub));
    }

    // Another device's frames go on; the capture ends with the frames of the device it started on.
    if (!home || --_framesLeft > 0) return;
    lock.unlock();
    Finish(dev);
}

void CaptureManager::OnDestroyDevice(DeviceData* dev) {
    if (!IsCapturing() || !FindCapture(dev->device)) return;
    if (dev->device == _homeDevice) {
        Log("capture: the device the capture started on is being destroyed; sending what was captured");
        Finish(dev);
        return;
    }
    dev->dispatch.DeviceWaitIdle(dev->device);
    std::unique_ptr<DeviceCapture> dc;
    {
        std::lock_guard lock(_devicesMutex);
        auto it = _devices.find(dev->device);
        if (it == _devices.end()) return;
        dc = std::move(it->second);
        _devices.erase(it);
    }
    {
        std::lock_guard lock(_mutex);
        for (auto& tc : _textures) {
            if (tc.device != dev->device || tc.failed) continue;
            tc.failed = true;
            tc.note = "the device was destroyed before the capture finished";
        }
        for (auto& bc : _buffers) {
            if (bc.device != dev->device || bc.failed) continue;
            bc.failed = true;
            bc.note = "the device was destroyed before the capture finished";
        }
        _passTimings.erase(std::remove_if(_passTimings.begin(), _passTimings.end(), [&](const PassTiming& pt) { return pt.device == dev->device; }),
                           _passTimings.end());
    }
    ReleaseDevice(*dc);
    Log("capture: device %p was destroyed during the capture; its read-backs are dropped", (void*)dev->device);
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
    if (uint32_t n = _suspendedPasses.exchange(0, std::memory_order_relaxed)) Log("capture: %u render pass(es) suspended and resumed across command buffers: neither timed nor counted, and read back where they resumed", n);

    // Everything recorded in the frame has been submitted; wait for it so staging data is valid,
    // on every device that took part.
    std::vector<DeviceData*> devices;
    {
        std::lock_guard lock(_devicesMutex);
        for (auto& [device, dc] : _devices) devices.push_back(dc->dev);
    }
    for (DeviceData* d : devices) d->dispatch.DeviceWaitIdle(d->device);
    if (devices.size() > 1) Log("capture: %zu devices took part", devices.size());

    SendCommands();
    MapStaging();
    SendTextures(dev);
    SendBuffers(dev);
    SendPassTimings();
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
    std::unordered_map<VkDevice, std::unique_ptr<DeviceCapture>> taken;
    {
        std::lock_guard lock(_devicesMutex);
        taken.swap(_devices);
    }
    for (auto& [device, dc] : taken) {
        ReleaseDevice(*dc);
        if (!RecordAlways()) {
            std::unique_lock lock(dc->dev->recorderMutex);
            dc->dev->recorders.clear();
        }
    }
    std::lock_guard lock(_mutex);
    _homeDevice = VK_NULL_HANDLE;
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

void CaptureManager::CreateQueryPools(DeviceCapture& dc) {
    DeviceData* dev = dc.dev;
    if (!dev->properties.limits.timestampComputeAndGraphics) {
        Log("pass profiling: timestamps not supported on all queues; skipped");
        return;
    }
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = 16384;   // 8192 passes per capture
    if (dev->dispatch.CreateQueryPool(dev->device, &ci, nullptr, &dc.queryPool) != VK_SUCCESS) {
        dc.queryPool = VK_NULL_HANDLE;
        Log("pass profiling: vkCreateQueryPool failed");
        return;
    }
    dc.queryCount = ci.queryCount;

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
    if (dev->dispatch.CreateQueryPool(dev->device, &sci, nullptr, &dc.statsPool) != VK_SUCCESS) {
        dc.statsPool = VK_NULL_HANDLE;
        Log("pass counters: vkCreateQueryPool failed");
        return;
    }
    dc.statsCount = sci.queryCount;

    // The samples that passed each pass's depth and stencil tests, for the depth-rejection rule.
    // Precise counts need occlusionQueryPrecise; without it the query would only answer "any".
    if (!dev->occlusionPrecise) {
        Log("depth rejection: occlusionQueryPrecise is not enabled on this device");
        return;
    }
    VkQueryPoolCreateInfo oci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    oci.queryType = VK_QUERY_TYPE_OCCLUSION;
    oci.queryCount = 8192;                        // one per pass, matching the statistics queries
    if (dev->dispatch.CreateQueryPool(dev->device, &oci, nullptr, &dc.occlusionPool) != VK_SUCCESS) {
        dc.occlusionPool = VK_NULL_HANDLE;
        Log("depth rejection: vkCreateQueryPool failed");
        return;
    }
    dc.occlusionCount = oci.queryCount;
}

void CaptureManager::ReleaseDevice(DeviceCapture& dc) {
    DeviceData* dev = dc.dev;
    const DeviceDispatch& d = dev->dispatch;
    if (dc.queryPool) d.DestroyQueryPool(dev->device, dc.queryPool, nullptr);
    if (dc.statsPool) d.DestroyQueryPool(dev->device, dc.statsPool, nullptr);
    if (dc.occlusionPool) d.DestroyQueryPool(dev->device, dc.occlusionPool, nullptr);
    dc.queryPool = dc.statsPool = dc.occlusionPool = VK_NULL_HANDLE;
    std::lock_guard lock(_mutex);
    for (auto& c : dc.staging) {
        if (c.mapped) d.UnmapMemory(dev->device, c.memory);
        d.DestroyBuffer(dev->device, c.buffer, nullptr);
        d.FreeMemory(dev->device, c.memory, nullptr);
    }
    dc.staging.clear();
    for (VkImageView v : dc.resolveViews) d.DestroyImageView(dev->device, v, nullptr);
    dc.resolveViews.clear();
    for (auto& r : dc.resolveImages) {
        d.DestroyImage(dev->device, r.image, nullptr);
        d.FreeMemory(dev->device, r.memory, nullptr);
    }
    dc.resolveImages.clear();
}

uint32_t CaptureManager::BeginTimestamp(DeviceData* dev, CommandRecorder* rec) {
    if (!IsCapturing() || !_options.profilePasses) return UINT32_MAX;
    if (rec->renderPassContinue()) return UINT32_MAX;   // secondaries inside a pass: the primary times the pass
    DeviceCapture* dc = CaptureFor(dev);
    if (!dc || !dc->queryPool) return UINT32_MAX;
    uint32_t q = dc->queriesUsed.fetch_add(2, std::memory_order_relaxed);
    if (q + 2 > dc->queryCount) return UINT32_MAX;      // pool exhausted: later passes go untimed
    VkCommandBuffer cb = rec->commandBuffer();
    dev->dispatch.CmdResetQueryPool(cb, dc->queryPool, q, 2);
    dev->dispatch.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dc->queryPool, q);
    return q;
}

uint32_t CaptureManager::BeginPipelineStatistics(DeviceData* dev, CommandRecorder* rec) {
    DeviceCapture* dc = CaptureFor(dev);
    if (!dc || !dc->statsPool) return UINT32_MAX;
    if (rec->renderPassContinue()) return UINT32_MAX;   // the primary brackets the pass
    // Two pipeline statistics queries cannot be active at once, and the application's come first.
    if (dev->appStatisticsQueries.load(std::memory_order_relaxed)) return UINT32_MAX;
    uint32_t q = dc->statsUsed.fetch_add(1, std::memory_order_relaxed);
    if (q >= dc->statsCount) return UINT32_MAX;         // pool exhausted: later passes go uncounted
    VkCommandBuffer cb = rec->commandBuffer();
    // Both of these are outside the render pass: the layer's begin hook runs before the driver's
    // vkCmdBeginRenderPass and its end hook after vkCmdEndRenderPass, so the query brackets the
    // whole pass without landing inside a subpass.
    dev->dispatch.CmdResetQueryPool(cb, dc->statsPool, q, 1);
    dev->dispatch.CmdBeginQuery(cb, dc->statsPool, q, 0);
    return q;
}

uint32_t CaptureManager::BeginOcclusion(DeviceData* dev, CommandRecorder* rec) {
    DeviceCapture* dc = CaptureFor(dev);
    if (!dc || !dc->occlusionPool) return UINT32_MAX;
    if (rec->renderPassContinue()) return UINT32_MAX;   // the primary brackets the pass
    // Two occlusion queries cannot be active at once, and the application's come first.
    if (dev->appOcclusionQueries.load(std::memory_order_relaxed)) return UINT32_MAX;
    uint32_t q = dc->occlusionUsed.fetch_add(1, std::memory_order_relaxed);
    if (q >= dc->occlusionCount) return UINT32_MAX;     // pool exhausted: later passes go uncounted
    VkCommandBuffer cb = rec->commandBuffer();
    dev->dispatch.CmdResetQueryPool(cb, dc->occlusionPool, q, 1);
    dev->dispatch.CmdBeginQuery(cb, dc->occlusionPool, q, VK_QUERY_CONTROL_PRECISE_BIT);
    rec->pendingOcclusionQuery = q;
    return q;
}

void CaptureManager::OnBeforePass(DeviceData* dev, CommandRecorder* rec, const PassShape& shape) {
    rec->pendingQuery = UINT32_MAX;
    rec->pendingStatsQuery = UINT32_MAX;
    // A pass resumed from a part suspended earlier in the submission (in another command buffer,
    // usually): nothing may be recorded between the two, so not even the compute pass's end.
    if (shape.resuming) return;
    OnEndComputePass(dev, rec);
    // A pass suspended here can have nothing recorded after it either, so it would have no end
    // timestamp, and its queries could not be ended: untimed and uncounted, like its resumption.
    if (shape.suspending) return;
    rec->pendingQuery = BeginTimestamp(dev, rec);
    // Only alongside a timed pass: an uncounted pass would spend a query for nothing, and the
    // report shows the counters against the pass's duration. A multiview pass writes one result
    // per view, which would need that many consecutive indices, so it is timed but not counted.
    // A pass that may execute secondary command buffers can only do so with queries active when
    // the secondaries inherit them, which needs inheritedQueries (hooks.cpp, InheritPassQueries).
    const bool counted = rec->pendingQuery != UINT32_MAX && !shape.multiview && (!shape.secondaries || dev->inheritedQueries);
    rec->pendingStatsQuery = counted ? BeginPipelineStatistics(dev, rec) : UINT32_MAX;
    if (counted) BeginOcclusion(dev, rec);
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
    if (c.query == UINT32_MAX) return;
    DeviceCapture* dc = FindCapture(dev->device);   // ended as begun, even if the capture has just finished
    if (!dc || !dc->queryPool) return;
    dev->dispatch.CmdWriteTimestamp(rec->commandBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dc->queryPool, c.query + 1);
    PassTiming pt;
    pt.device = dev->device;
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
    p.suspending = (info->flags & VK_RENDERING_SUSPENDING_BIT) != 0;
    p.resuming = (info->flags & VK_RENDERING_RESUMING_BIT) != 0;
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
    DeviceCapture* suspended = p.suspending || p.resuming ? FindCapture(dev->device) : nullptr;
    if (p.suspending) {
        // Suspended, to be resumed later in the submission: nothing may be recorded between the
        // two parts, in this command buffer or the next, so this end gets no read-back, no
        // timestamp (OnBeforePass took none) and no flush of the copies queued inside it. Those
        // wait for the part that resumes the pass, whose end records them (below). The pass's
        // attachments are read back there too, once the whole pass has run.
        p.active = false;
        _suspendedPasses.fetch_add(1, std::memory_order_relaxed);
        if (suspended) {
            std::lock_guard lock(suspended->suspendedMutex);
            auto& copies = rec->pendingCopies();
            suspended->suspendedCopies.insert(suspended->suspendedCopies.end(), copies.begin(), copies.end());
            copies.clear();
            auto& images = rec->pendingImages();
            suspended->suspendedImages.insert(suspended->suspendedImages.end(), images.begin(), images.end());
            images.clear();
        }
        return;
    }
    if (suspended) {
        // The part that ends a suspended pass: the copies of every part before it are recorded
        // here, after it, with this part's own.
        std::lock_guard lock(suspended->suspendedMutex);
        auto& copies = rec->pendingCopies();
        copies.insert(copies.begin(), suspended->suspendedCopies.begin(), suspended->suspendedCopies.end());
        suspended->suspendedCopies.clear();
        auto& images = rec->pendingImages();
        images.insert(images.begin(), suspended->suspendedImages.begin(), suspended->suspendedImages.end());
        suspended->suspendedImages.clear();
    }
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
    // The pass's end timestamp: after every command of the pass has completed, and its counters' queries end.
    const uint32_t occlusion = rec->pendingOcclusionQuery;
    const bool queried = occlusion != UINT32_MAX || p.query != UINT32_MAX || p.statsQuery != UINT32_MAX;
    DeviceCapture* dc = queried ? FindCapture(dev->device) : nullptr;
    if (occlusion != UINT32_MAX && dc && dc->occlusionPool) {
        dev->dispatch.CmdEndQuery(rec->commandBuffer(), dc->occlusionPool, occlusion);
        rec->pendingOcclusionQuery = UINT32_MAX;
    }
    if (p.query != UINT32_MAX && dc && dc->queryPool) {
        dev->dispatch.CmdWriteTimestamp(rec->commandBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dc->queryPool, p.query + 1);
        if (p.statsQuery != UINT32_MAX && dc->statsPool) dev->dispatch.CmdEndQuery(rec->commandBuffer(), dc->statsPool, p.statsQuery);
        PassTiming pt;
        pt.device = dev->device;
        pt.commandBufferId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)rec->commandBuffer());
        pt.passIndex = p.passIndex;
        pt.query = p.query;
        pt.statsQuery = p.statsQuery;
        pt.occlusionQuery = occlusion;
        std::lock_guard lock(_mutex);
        _passTimings.push_back(pt);
    } else if (p.statsQuery != UINT32_MAX && dc && dc->statsPool) {
        // Begun but not going to be reported: it still has to end, or the command buffer is invalid.
        dev->dispatch.CmdEndQuery(rec->commandBuffer(), dc->statsPool, p.statsQuery);
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
    tc.viewId = Tracker::Get().Resolve(HT_VkImageView, (uint64_t)(uintptr_t)view);
    tc.mip = vi.range.baseMipLevel;
    tc.baseLayer = vi.range.baseArrayLayer;
    tc.layers = vi.range.layerCount == VK_REMAINING_ARRAY_LAYERS ? img.arrayLayers - vi.range.baseArrayLayer : vi.range.layerCount;
    VkImageAspectFlags aspects = FormatAspects(img.format);
    tc.aspect = aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT
              : aspects & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT
              : VK_IMAGE_ASPECT_COLOR_BIT;
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
        if (!LayoutTracker::Get().GetLayout(vi.image, layout)) layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    // Every mip of the view, back to back (each mip: its layers), so the viewer can show them.
    const uint32_t mips = vi.range.levelCount == VK_REMAINING_MIP_LEVELS ? img.mipLevels - tc.mip : vi.range.levelCount;
    const uint32_t id = QueueImageCopy(dev, rec, vi.image, img, tc, mips, layout);
    // Every later binding of the same view in this capture refers to it, failed or pending.
    std::lock_guard lock(_mutex);
    _imageCaptureByView[(uint64_t)(uintptr_t)view] = id;
    return id;
}

uint32_t CaptureManager::QueueImageCopy(DeviceData* dev, CommandRecorder* rec, VkImage image, const ImageInfo& img, TextureCapture tc,
                                        uint32_t mips, VkImageLayout layout) {
    tc.recorded = false;
    tc.imageId = Tracker::Get().Resolve(HT_VkImage, (uint64_t)(uintptr_t)image);
    tc.format = img.format;
    tc.width = std::max(1u, img.extent.width >> tc.mip);
    tc.height = std::max(1u, img.extent.height >> tc.mip);
    tc.depth = std::max(1u, img.extent.depth >> tc.mip);
    tc.layers = std::max(1u, tc.layers);
    const VkImageAspectFlags aspects = FormatAspects(img.format);

    auto add = [&]() {
        std::lock_guard lock(_mutex);
        tc.captureId = (uint32_t)_textures.size() + 1;
        _textures.push_back(tc);
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
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED) return fail("unknown image layout");
    FormatBlock block = FormatBlockInfo(img.format, tc.aspect);
    if (block.bytes == 0) return fail("unsupported format for readback");
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
    tc.device = dev->device;
    tc.stagingIndex = chunkIndex;
    tc.stagingOffset = offset;
    uint32_t id = add();

    VkDeviceSize mipOffset = 0;
    for (uint32_t m = 0; m < tc.mips; ++m) {
        PendingImageCopy p;
        p.resolve = m == 0 ? resolve : VK_NULL_HANDLE;   // multisampled images have one mip
        p.captureId = id;
        p.image = image;
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

// ---------------------------------------------------------------------------------------------
// Frame-start contents

void CaptureManager::SnapshotImageRead(DeviceData* dev, CommandRecorder* rec, VkImage image, VkImageAspectFlags aspect, uint32_t baseMip,
                                       uint32_t mipCount, uint32_t baseLayer, uint32_t layerCount, VkImageLayout layout,
                                       std::vector<uint32_t>& ids) {
    if (!rec || !image || !IsCapturing() || !_options.captureImages || rec->InsidePass()) return;
    ImageInfo img;
    if (!ResourceRegistry::Get().GetImage(image, img) || img.samples != VK_SAMPLE_COUNT_1_BIT) return;
    if (baseMip >= img.mipLevels || baseLayer >= img.arrayLayers) return;
    if (mipCount == VK_REMAINING_MIP_LEVELS || baseMip + mipCount > img.mipLevels) mipCount = img.mipLevels - baseMip;
    if (layerCount == VK_REMAINING_ARRAY_LAYERS || baseLayer + layerCount > img.arrayLayers) layerCount = img.arrayLayers - baseLayer;
    // Each aspect of the image the read names is its own texture: a depth-stencil image's depth
    // and stencil are read, and written, apart.
    for (VkImageAspectFlagBits one : {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT}) {
        if (!(aspect & one) || !(FormatAspects(img.format) & one)) continue;
        for (uint32_t m = baseMip; m < baseMip + mipCount; ++m) {
            {
                std::lock_guard lock(_mutex);
                bool untouched = false;
                for (uint32_t l = baseLayer; l < baseLayer + layerCount; ++l) {
                    uint8_t& s = SubresourceState(image, img, one, m, l);
                    if (s == kUntouched) {
                        s = kRead;
                        untouched = true;
                    }
                }
                if (!untouched) continue;
            }
            TextureCapture tc;
            tc.initial = true;
            tc.mip = m;
            tc.baseLayer = baseLayer;
            tc.layers = layerCount;
            tc.aspect = one;
            ids.push_back(QueueImageCopy(dev, rec, image, img, tc, 1, layout));
        }
    }
}

uint8_t& CaptureManager::SubresourceState(VkImage image, const ImageInfo& img, VkImageAspectFlagBits aspect, uint32_t mip, uint32_t layer) {
    // Per image: every subresource's state for its colour or depth aspect, then the same again for
    // its stencil aspect, which a depth-stencil image loads, clears and copies apart from its depth.
    auto& states = _imageStates[(uint64_t)(uintptr_t)image];
    const size_t perAspect = (size_t)img.mipLevels * img.arrayLayers;
    states.resize(perAspect * 2, kUntouched);
    return states[(aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? perAspect : 0) + (size_t)mip * img.arrayLayers + layer];
}

void CaptureManager::NoteImageWrite(VkImage image, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer,
                                    uint32_t layerCount) {
    if (!image || !IsCapturing()) return;
    ImageInfo img;
    if (!ResourceRegistry::Get().GetImage(image, img) || baseMip >= img.mipLevels || baseLayer >= img.arrayLayers) return;
    if (mipCount == VK_REMAINING_MIP_LEVELS || baseMip + mipCount > img.mipLevels) mipCount = img.mipLevels - baseMip;
    if (layerCount == VK_REMAINING_ARRAY_LAYERS || baseLayer + layerCount > img.arrayLayers) layerCount = img.arrayLayers - baseLayer;
    std::lock_guard lock(_mutex);
    for (VkImageAspectFlagBits one : {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT}) {
        if (!(aspect & one) || !(FormatAspects(img.format) & one)) continue;
        for (uint32_t m = baseMip; m < baseMip + mipCount; ++m)
            for (uint32_t l = baseLayer; l < baseLayer + layerCount; ++l) {
                uint8_t& s = SubresourceState(image, img, one, m, l);
                if (s == kUntouched) s = kWritten;
            }
    }
}

void CaptureManager::OnAttachmentBegin(DeviceData* dev, CommandRecorder* rec, VkImageView view, VkImageAspectFlags aspects,
                                       VkAttachmentLoadOp loadOp, VkImageLayout layout, const VkRect2D& renderArea,
                                       std::vector<uint32_t>& ids) {
    if (!view || !IsCapturing()) return;
    ResourceRegistry& reg = ResourceRegistry::Get();
    ImageViewInfo vi;
    ImageInfo img;
    if (!reg.GetImageView(view, vi) || !reg.GetImage(vi.image, img)) return;
    const uint32_t mip = vi.range.baseMipLevel;
    // LOAD reads the attachment; NONE keeps it for later readers, which is a read as far as the
    // contents a replay needs are concerned.
    if (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || loadOp == VK_ATTACHMENT_LOAD_OP_NONE) {
        if (layout != VK_IMAGE_LAYOUT_UNDEFINED)
            SnapshotImageRead(dev, rec, vi.image, aspects, mip, 1, vi.range.baseArrayLayer, vi.range.layerCount, layout, ids);
        return;
    }
    const uint32_t width = std::max(1u, img.extent.width >> mip);
    const uint32_t height = std::max(1u, img.extent.height >> mip);
    if (renderArea.offset.x <= 0 && renderArea.offset.y <= 0 && (int64_t)renderArea.offset.x + renderArea.extent.width >= width &&
        (int64_t)renderArea.offset.y + renderArea.extent.height >= height)
        NoteImageWrite(vi.image, aspects, mip, 1, vi.range.baseArrayLayer, vi.range.layerCount);
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
                                            VkDeviceSize offset, VkDeviceSize size, bool whole) {
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
    if (size > _options.maxBufferSize && !whole) {
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
    bc.device = dev->device;
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
    }
    for (auto& bc : buffers) {
        if (bc.failed) continue;
        if (!bc.recorded) {
            bc.failed = true;
            bc.note = "copy was never recorded (secondary command buffer not executed, or a suspended render pass never resumed)";
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
        const StagingChunk* c = StagingOf(bc.device, bc.stagingIndex);
        if (!c || !c->mapped) continue;
        JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureBufferData");
        h.Key("id"); h.Uint(bc.id);
        h.Key("size"); h.Uint(bc.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), static_cast<const uint8_t*>(c->mapped) + bc.stagingOffset, (size_t)bc.size);
    }
}

void CaptureManager::MapStaging() {
    // _devicesMutex is never held while taking _mutex (OnFrameEnd takes them the other way round).
    std::vector<DeviceCapture*> captures;
    {
        std::lock_guard devices(_devicesMutex);
        for (auto& [device, dc] : _devices) captures.push_back(dc.get());
    }
    std::lock_guard lock(_mutex);
    for (DeviceCapture* dc : captures) {
        const DeviceDispatch& d = dc->dev->dispatch;
        for (auto& c : dc->staging) {
            if (!c.mapped) d.MapMemory(dc->dev->device, c.memory, 0, VK_WHOLE_SIZE, 0, &c.mapped);
            VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            r.memory = c.memory;
            r.size = VK_WHOLE_SIZE;
            d.InvalidateMappedMemoryRanges(dc->dev->device, 1, &r);
        }
    }
}

const CaptureManager::StagingChunk* CaptureManager::StagingOf(VkDevice device, uint32_t index) {
    DeviceCapture* dc = FindCapture(device);
    return dc && index < dc->staging.size() ? &dc->staging[index] : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Readback

bool CaptureManager::AllocateStaging(DeviceData* dev, VkDeviceSize size, uint32_t& chunkIndex, VkDeviceSize& offset,
                                     VkBuffer* bufferOut) {
    const VkDeviceSize kChunk = 64ull << 20;
    const VkDeviceSize align = 256;
    // Chunks are the device's own: a command buffer can only copy into buffers of its device.
    DeviceCapture* dc = CaptureFor(dev);
    if (!dc) return false;
    std::lock_guard lock(_mutex);
    std::vector<StagingChunk>& staging = dc->staging;
    for (uint32_t i = 0; i < staging.size(); ++i) {
        VkDeviceSize start = (staging[i].used + align - 1) & ~(align - 1);
        if (start + size <= staging[i].size) {
            staging[i].used = start + size;
            chunkIndex = i;
            offset = start;
            if (bufferOut) *bufferOut = staging[i].buffer;
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
    staging.push_back(chunk);
    chunkIndex = (uint32_t)staging.size() - 1;
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
    DeviceCapture* dc = CaptureFor(dev);
    if (!dc) return false;
    ResolveImage ri;
    if (!CreateResolveImage(dev, img, mip, layers, &ri.image, &ri.memory)) return false;
    std::lock_guard lock(_mutex);
    dc->resolveImages.push_back(ri);
    *out = ri.image;
    return true;
}

bool CaptureManager::PrepareDepthResolve(DeviceData* dev, PendingImageCopy& p) {
    DeviceCapture* dc = CaptureFor(dev);
    if (!dc || !CanResolveDepth(dev)) return false;
    if (!CreateDepthResolveViews(dev, p, &p.srcView, &p.dstView)) return false;
    std::lock_guard lock(_mutex);
    dc->resolveViews.push_back(p.srcView);
    dc->resolveViews.push_back(p.dstView);
    return true;
}

std::vector<VkImageAspectFlagBits> CaptureManager::ReadBackAspects(VkImageView view) {
    ResourceRegistry& reg = ResourceRegistry::Get();
    ImageViewInfo vi;
    ImageInfo img;
    std::vector<VkImageAspectFlagBits> out;
    if (!reg.GetImageView(view, vi) || !reg.GetImage(vi.image, img)) return out;
    const VkImageAspectFlags aspects = FormatAspects(img.format);
    if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) out.push_back(VK_IMAGE_ASPECT_DEPTH_BIT);
    if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) out.push_back(VK_IMAGE_ASPECT_STENCIL_BIT);
    if (out.empty()) out.push_back(VK_IMAGE_ASPECT_COLOR_BIT);
    return out;
}

void CaptureManager::CaptureAttachment(DeviceData* dev, CommandRecorder* rec, uint32_t attachmentIndex,
                                       VkImageView view, VkImageLayout layout, bool resolveTarget) {
    const uint64_t cbId = Tracker::Get().Resolve(HT_VkCommandBuffer, (uint64_t)(uintptr_t)rec->commandBuffer());
    for (VkImageAspectFlagBits aspect : ReadBackAspects(view)) {
        TextureCapture tc;
        PendingImageCopy p;
        if (!PrepareAttachment(dev, cbId, rec->pass().passIndex, rec->pass().layerCount, attachmentIndex, view, layout, resolveTarget, aspect, tc, p)) continue;
        RecordImageCopy(dev, rec->commandBuffer(), p);
        std::lock_guard lock(_mutex);
        _textures.push_back(tc);
    }
}

bool CaptureManager::PrepareAttachment(DeviceData* dev, uint64_t commandBufferId, uint32_t passIndex, uint32_t layerCount,
                                       uint32_t attachmentIndex, VkImageView view, VkImageLayout layout, bool resolveTarget,
                                       VkImageAspectFlagBits aspect, TextureCapture& tc, PendingImageCopy& p) {
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
    tc.aspect = aspect;
    const bool depthStencil = aspect == VK_IMAGE_ASPECT_DEPTH_BIT || aspect == VK_IMAGE_ASPECT_STENCIL_BIT;

    auto fail = [&](const char* why) {
        tc.failed = true;
        tc.note = why;
        std::lock_guard lock(_mutex);
        _textures.push_back(tc);
        return false;
    };
    if (!img.transferSrc) return fail("image lacks TRANSFER_SRC usage");
    // Both aspects of a multisampled depth-stencil image resolve through the same render pass
    // (depth_resolve.h), so stencil goes the way depth does.
    if (img.samples != VK_SAMPLE_COUNT_1_BIT && depthStencil && !CanResolveDepth(dev))
        return fail("multisampled depth or stencil attachment (the resolve needs dynamic rendering, Vulkan 1.2+)");
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) return fail("unknown final layout");

    uint32_t bpp = FormatBytesPerTexel(img.format, tc.aspect);
    if (bpp == 0) return fail("unsupported format for readback");
    // Depth aspect copies use the depth-only packed size (D24 -> 4 bytes, D16 -> 2, D32 -> 4); stencil is one byte.
    tc.size = (VkDeviceSize)tc.width * tc.height * tc.depth * tc.layers * bpp;
    if (tc.size > _options.maxTextureSize) return fail("exceeds max texture size");

    uint32_t chunkIndex = 0;
    VkDeviceSize offset = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    if (!AllocateStaging(dev, tc.size, chunkIndex, offset, &staging)) return fail("staging allocation failed");
    VkImage resolve = VK_NULL_HANDLE;
    if (img.samples != VK_SAMPLE_COUNT_1_BIT && !AllocateResolveImage(dev, img, tc.mip, tc.layers, &resolve))
        return fail("resolve image allocation failed");
    tc.device = dev->device;
    tc.stagingIndex = chunkIndex;
    tc.stagingOffset = offset;

    p = PendingImageCopy{};
    p.image = vi.image;
    p.layout = layout;
    // The barriers cover both aspects of a depth-stencil image; the copy takes one.
    p.range = {depthStencil ? FormatAspects(img.format) : tc.aspect, tc.mip, 1, vi.range.baseArrayLayer, tc.layers};
    p.copyAspect = tc.aspect;
    p.extent = {tc.width, tc.height, tc.depth};
    p.staging = staging;
    p.stagingOffset = offset;
    p.size = tc.size;
    p.resolve = resolve;
    p.format = img.format;
    if (resolve && depthStencil && !PrepareDepthResolve(dev, p)) return fail("depth resolve views could not be created");
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
            for (VkImageAspectFlagBits aspect : ReadBackAspects(view)) {
                TextureCapture tc;
                PendingImageCopy p;
                if (PrepareAttachment(dev, commandBufferId, pass.passIndex, pass.layerCount, i, view, layout, resolveTarget, aspect, tc, p)) {
                    tc.frame = frame;
                    copies.emplace_back(tc, p);
                }
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
    (void)dev;
    Transport& t = Transport::Get();
    std::vector<TextureCapture> textures;
    {
        std::lock_guard lock(_mutex);
        textures = _textures;
    }

    // Readbacks recorded into command buffers that were never submitted have no valid data.
    for (auto& tc : textures) {
        if (!tc.recorded && !tc.failed) {
            tc.failed = true;
            tc.note = "copy was never recorded (secondary command buffer not executed, or a suspended render pass never resumed)";
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
        w.Key("aspect"); w.String(tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? "depth" : tc.aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? "stencil" : "color");
        w.Key("width"); w.Uint(tc.width);
        w.Key("height"); w.Uint(tc.height);
        w.Key("depth"); w.Uint(tc.depth);
        w.Key("layers"); w.Uint(tc.layers);
        w.Key("mip"); w.Uint(tc.mip);
        if (tc.mips > 1) { w.Key("mips"); w.Uint(tc.mips); }
        w.Key("size"); w.Uint(tc.failed ? 0 : tc.size);
        if (tc.samples > 1) { w.Key("samples"); w.Uint(tc.samples); }
        if (tc.resolveTarget) { w.Key("resolve"); w.Boolean(true); }
        if (tc.sampled || tc.initial) {
            w.Key("kind"); w.String(tc.initial ? "initial" : "sampled");
            w.Key("capture"); w.Uint(tc.captureId);
            if (tc.viewId) { w.Key("view"); w.Uint(tc.viewId); }
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
        const StagingChunk* c = StagingOf(tc.device, tc.stagingIndex);
        if (!c || !c->mapped) continue;
        JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureTextureData");
        h.Key("id"); h.Uint(tc.imageId);
        h.Key("frame"); h.Uint(tc.frame);
        h.Key("commandBuffer"); h.Uint(tc.commandBufferId);
        h.Key("passIndex"); h.Uint(tc.passIndex);
        h.Key("attachment"); h.Uint(tc.attachment);
        // A depth-stencil attachment has an entry per aspect under the same attachment index.
        h.Key("aspect"); h.String(tc.aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? "depth" : tc.aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? "stencil" : "color");
        if (tc.sampled || tc.initial) { h.Key("capture"); h.Uint(tc.captureId); }
        h.Key("size"); h.Uint(tc.size);
        h.EndObject();
        t.SendBinary(std::move(h.str()), static_cast<const uint8_t*>(c->mapped) + tc.stagingOffset, (size_t)tc.size);
    }
}

void CaptureManager::SendPassTimings() {
    std::vector<DeviceCapture*> captures;
    DeviceCapture* home = nullptr;
    {
        std::lock_guard lock(_devicesMutex);
        for (auto& [device, dc] : _devices) {
            if (!dc->queryPool) continue;
            captures.push_back(dc.get());
            if (device == _homeDevice) home = dc.get();
        }
    }
    if (captures.empty()) return;
    // One message for every device. Each device keeps its own clock, so a pass's start is measured
    // from the earliest pass of its own device, and its duration with its own timestamp period.
    JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CapturePassTimings");
    w.Key("timestampPeriodNs"); w.Double((home ? home : captures[0])->dev->properties.limits.timestampPeriod);
    w.Key("passes"); w.BeginArray();
    uint32_t sent = 0;
    uint32_t counted = 0;
    size_t total = 0;
    for (DeviceCapture* dc : captures) SendPassTimings(*dc, w, sent, counted, total);
    w.EndArray();
    w.Key("count"); w.Uint(sent);
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    Log("pass profiling: %u of %zu passes timed, %u with counters%s", sent, total, counted,
        captures.size() > 1 ? " (several devices)" : "");
}

void CaptureManager::SendPassTimings(DeviceCapture& dc, JsonWriter& w, uint32_t& sent, uint32_t& counted, size_t& total) {
    DeviceData* dev = dc.dev;
    std::vector<PassTiming> timings;
    {
        std::lock_guard lock(_mutex);
        for (const PassTiming& pt : _passTimings)
            if (pt.device == dev->device) timings.push_back(pt);
    }
    total += timings.size();
    uint32_t used = std::min(dc.queriesUsed.load(std::memory_order_relaxed), dc.queryCount);
    if (!used || timings.empty()) return;
    // Each query: 64-bit value then 64-bit availability (0 when the command buffer never ran).
    std::vector<uint64_t> results((size_t)used * 2, 0);
    VkResult res = dev->dispatch.GetQueryPoolResults(dev->device, dc.queryPool, 0, used, results.size() * sizeof(uint64_t),
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
    if (dc.statsPool) {
        statsUsed = std::min(dc.statsUsed.load(std::memory_order_relaxed), dc.statsCount);
        if (statsUsed) {
            stats.assign((size_t)statsUsed * kStatsStride, 0);
            VkResult sres = dev->dispatch.GetQueryPoolResults(
                dev->device, dc.statsPool, 0, statsUsed, stats.size() * sizeof(uint64_t), stats.data(),
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
    if (dc.occlusionPool) {
        occlusionUsed = std::min(dc.occlusionUsed.load(std::memory_order_relaxed), dc.occlusionCount);
        if (occlusionUsed) {
            occlusion.assign((size_t)occlusionUsed * 2, 0);
            VkResult ores = dev->dispatch.GetQueryPoolResults(
                dev->device, dc.occlusionPool, 0, occlusionUsed, occlusion.size() * sizeof(uint64_t), occlusion.data(),
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
}

} // namespace vkinsp
