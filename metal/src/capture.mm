#include "capture.h"

#include "formats.h"
#include "frame_stats.h"
#include "gpu_trace.h"
#include "stacktrace.h"
#include "validation.h"
#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace mtlinsp {
namespace {

// A colour or depth attachment blitted into a staging buffer at the end of its pass. The bytes
// are read out in the command buffer's completion handler, once the GPU has produced them.
struct PendingTexture {
    uint64_t textureId = 0;
    uint32_t frame = 0;
    uint64_t commandBufferId = 0;
    uint32_t passIndex = 0;
    uint32_t attachment = 0;
    const char *aspect = "color";
    uint32_t width = 0;
    uint32_t height = 0;
    std::string format;
    size_t size = 0;
    uint64_t bytesPerRow = 0;
    std::string error;
    // What to copy from: the attachment, or its resolve texture. Retained until the blit.
    id<MTLTexture> source = nil;
    uint32_t level = 0;
    uint32_t slice = 0;
    uint32_t depthPlane = 0;
    MTLBlitOption options = MTLBlitOptionNone;
    id<MTLBuffer> staging = nil;
};

// A bound buffer range read back with the capture. `data` is filled at once for a buffer in
// shared storage; a managed or private one is filled at Finish, from the buffer or its staging.
struct CapturedBuffer {
    // Not `id`: an Objective-C++ member of that name hides the `id` type for the whole struct,
    // so the id<MTLBuffer> fields below stop parsing. PendingTexture calls its own `textureId`
    // for the same reason.
    uint64_t captureId = 0;
    uint64_t bufferId = 0;     // the tracked MTLBuffer, 0 for inline bytes
    uint32_t frame = 0;
    uint64_t commandBufferId = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t originalSize = 0; // set when the range was truncated
    std::string error;
    std::vector<uint8_t> data;
    // Deferred read-back. Retained until it is done.
    id<MTLBuffer> source = nil;
    id<MTLBuffer> staging = nil;
    bool managed = false;
};

struct PassTiming {
    uint32_t frame = 0;
    uint64_t commandBufferId = 0;
    uint32_t passIndex = 0;
    PassKind kind = PassKind::Render;
    PassTimingSlot slot;
};

// A pass in flight: what to blit and sample when the application ends the encoder.
struct OpenPass {
    id commandBuffer = nil;
    uint64_t commandBufferId = 0;
    uint32_t passIndex = 0;
    PassKind kind = PassKind::Render;
    PassTimingSlot timing;
    std::vector<PendingTexture> attachments;
    std::vector<uint64_t> deferredBuffers;   // CapturedBuffer ids to blit at the end of the pass
};

struct RecordedCommand {
    uint32_t frame = 0;
    std::string method;
    uint64_t commandBufferId = 0;
    uint64_t encoderId = 0;
    const char *encoderType = nullptr;
    std::string args;
    std::vector<uint64_t> bufferData;  // CapturedBuffer ids, in the order the UI expects
    StackTrace stack;                  // where the application issued it, when asked for
};

struct EncoderInfo {
    id commandBuffer = nil;
    const char *type = "";
    uint64_t captureId = 0;   // from the tracker's sequence, so it cannot collide with an object
    id parent = nil;          // the parallel render encoder that handed this one out, or nil
};

struct DrawableInfo {
    const void *drawable = nullptr;
    uint64_t drawableID = 0;
};

std::mutex g_mutex;
// Armed but not started: the next frame boundary begins the capture.
bool g_pending = false;
std::atomic<bool> g_recording{false};
CaptureOptions g_options;
uint32_t g_wantFrames = 1;
uint32_t g_frameIndex = 0;
uint64_t g_bufferBytes = 0;   // captured so far, against maxBufferTotal
std::vector<RecordedCommand> g_commands;
std::vector<CapturedBuffer> g_buffers;
// (buffer id, offset, size) -> CapturedBuffer id, so a range bound at every draw is read once.
std::map<std::tuple<uint64_t, uint64_t, uint64_t>, uint64_t> g_bufferRanges;
uint64_t g_nextBufferId = 1;
std::vector<PendingTexture> g_textures;
std::vector<PassTiming> g_passTimings;

std::unordered_map<const void *, EncoderInfo> g_encoders;   // encoder -> owner
std::unordered_map<const void *, OpenPass> g_openPasses;    // encoder -> its pass
std::unordered_map<const void *, uint32_t> g_passCounters;  // command buffer -> passes begun

// Command buffers committed during the capture that have not completed yet. The read-back blits
// are in them, so the staging holds nothing until the count reaches zero.
//
// The handler is registered in the commit hook, before the commit is forwarded, because
// addCompletedHandler: is only legal before a command buffer is committed. Adding one later —
// from a frame boundary that arrives on Metal's scheduled-handler thread, which is where Unity's
// [drawable present] runs — makes Metal assert and abort the application.
int g_outstanding = 0;
bool g_finishPending = false;

// Frame boundaries. See the header.
std::set<const void *> g_presenting;                          // command buffers asked to present
std::vector<DrawableInfo> g_presentedByCommandBuffer;         // drawables presentDrawable: took
std::unordered_map<const void *, DrawableInfo> g_drawableOfTexture;
std::unordered_map<const void *, DrawableInfo> g_targetOfCommandBuffer;
std::set<const void *> g_countedDrawables;                    // frame ended at commit already
bool g_directPresent = false;
void (*g_commitBoundaryLogger)(id) = nullptr;                                 // the app presents drawables itself

// A capture's commands go out in batches rather than one message, so a frame with tens of
// thousands of commands does not become a single enormous JSON string.
constexpr size_t kCommandsPerBatch = 2000;

// --------------------------------------------------------------------------------------------
// GPU timestamps
//
// The Metal counterpart of vkCmdWriteTimestamp is a counter sample buffer. A render or compute
// pass samples into it at its stage boundaries through the pass descriptor, which is why the
// hooks copy the descriptor while recording; a GPU that cannot sample at stage boundaries but can
// at draw, dispatch or blit boundaries takes the samples from the encoder instead. The timestamps
// are resolved once the frame's command buffers have completed and mapped to nanoseconds with two
// CPU/GPU timestamp pairs taken around the capture.

constexpr uint32_t kSampleCapacity = 4096;

/** One counter set's sample buffer and how much of it a capture has used. */
struct CounterBuffer {
    id<MTLCounterSampleBuffer> buffer = nil;
    uint32_t used = 0;
    uint32_t capacity = 0;
    /** Two samples, start then end, or false when the buffer is absent or full. */
    bool Reserve(uint32_t count, uint32_t *start) {
        if (buffer == nil || used + count > capacity) return false;
        *start = used;
        used += count;
        return true;
    }
};

struct Timing {
    id<MTLDevice> device = nil;
    // Timestamps: the pass durations. `buffer`, `used` and `capacity` keep their names since
    // the rest of the file reads them.
    id<MTLCounterSampleBuffer> buffer = nil;
    uint32_t used = 0;
    uint32_t capacity = 0;
    // The statistic set (vertex, fragment and kernel invocations, clipper counts) and the
    // stage-utilization set (cycles per stage): Xcode's counters, when the GPU has them.
    CounterBuffer statistic;
    CounterBuffer utilization;
    bool tried = false;
    bool stageBoundary = false;
    bool drawBoundary = false;
    bool dispatchBoundary = false;
    bool blitBoundary = false;
    MTLTimestamp cpuStart = 0;
    MTLTimestamp gpuStart = 0;
};
Timing g_timing;

/** Under g_mutex. Creates the sample buffer on first use in a capture. */
void EnsureTiming(id<MTLDevice> device) {
    if (g_timing.tried || device == nil) return;
    g_timing.tried = true;
    if (!g_options.profilePasses) return;
    g_timing.device = device;
    if (@available(macOS 11.0, *)) {
        Internal internal;
        g_timing.stageBoundary = [device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
        g_timing.drawBoundary = [device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary];
        g_timing.dispatchBoundary = [device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary];
        g_timing.blitBoundary = [device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary];
        id<MTLCounterSet> timestamps = nil;
        id<MTLCounterSet> statistic = nil;
        id<MTLCounterSet> utilization = nil;
        for (id<MTLCounterSet> set in device.counterSets) {
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) timestamps = set;
            else if ([set.name isEqualToString:MTLCommonCounterSetStatistic]) statistic = set;
            else if ([set.name isEqualToString:MTLCommonCounterSetStageUtilization]) utilization = set;
        }
        if (timestamps == nil) {
            Log("pass timings: no timestamp counter set on %s", device.name.UTF8String);
            return;
        }
        // The largest the device accepts, from a generous size down.
        auto make = [&](id<MTLCounterSet> set, NSString *label, uint32_t *capacity) -> id<MTLCounterSampleBuffer> {
            MTLCounterSampleBufferDescriptor *descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
            descriptor.counterSet = set;
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.label = label;
            id<MTLCounterSampleBuffer> buffer = nil;
            for (uint32_t n = kSampleCapacity; n >= 64; n /= 2) {
                descriptor.sampleCount = n;
                NSError *error = nil;
                buffer = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
                if (buffer != nil) {
                    *capacity = n;
                    break;
                }
            }
            [descriptor release];
            return buffer;
        };
        g_timing.buffer = make(timestamps, @"gpu-inspector pass timestamps", &g_timing.capacity);
        if (g_timing.buffer == nil) {
            Log("pass timings: could not create a counter sample buffer");
            return;
        }
        if (statistic != nil) {
            g_timing.statistic.buffer = make(statistic, @"gpu-inspector pass statistics", &g_timing.statistic.capacity);
        }
        if (utilization != nil) {
            g_timing.utilization.buffer = make(utilization, @"gpu-inspector stage utilization", &g_timing.utilization.capacity);
        }
        [device sampleTimestamps:&g_timing.cpuStart gpuTimestamp:&g_timing.gpuStart];
        Log("pass timings: %u samples, stage boundary %d, draw %d, dispatch %d, blit %d, statistics %d, utilization %d",
            g_timing.capacity, g_timing.stageBoundary, g_timing.drawBoundary,
            g_timing.dispatchBoundary, g_timing.blitBoundary, g_timing.statistic.buffer != nil,
            g_timing.utilization.buffer != nil);
    }
}

/** Under g_mutex. Two sample indices for a pass, or a slot with no buffer. */
/**
 * Timestamp samples for a pass — two, or four for a render pass sampled at every stage
 * boundary — and a start and end in each counter buffer the device has. A counter set whose
 * buffer is full is left out of the pass rather than failing the timestamps.
 */
PassTimingSlot ReserveSamples(bool onEncoder, bool fourStages) {
    PassTimingSlot slot;
    const uint32_t count = fourStages ? 4 : 2;
    if (g_timing.buffer == nil || g_timing.used + count > g_timing.capacity) return slot;
    slot.sampleBuffer = g_timing.buffer;
    slot.startIndex = g_timing.used;
    if (fourStages) {
        slot.vertexEndIndex = g_timing.used + 1;
        slot.fragmentStartIndex = g_timing.used + 2;
    }
    slot.endIndex = g_timing.used + count - 1;
    slot.onEncoder = onEncoder;
    g_timing.used += count;
    uint32_t start = 0;
    if (g_timing.statistic.Reserve(2, &start)) {
        slot.statisticBuffer = g_timing.statistic.buffer;
        slot.statisticStart = start;
        slot.statisticEnd = start + 1;
    }
    if (g_timing.utilization.Reserve(2, &start)) {
        slot.utilizationBuffer = g_timing.utilization.buffer;
        slot.utilizationStart = start;
        slot.utilizationEnd = start + 1;
    }
    return slot;
}

void ReleaseTiming(Timing &timing) {
    [timing.buffer release];
    [timing.statistic.buffer release];
    [timing.utilization.buffer release];
    timing = Timing();
}

/** The counter sets' end-minus-start for one pass, as JSON members; nothing when unresolved. */
template <typename T>
const T *ResolvedSamples(NSData *data, uint32_t count) {
    if (data == nil || data.length < (NSUInteger)count * sizeof(T)) return nullptr;
    return (const T *)data.bytes;
}

uint64_t Delta(uint64_t start, uint64_t end) {
    if (start == MTLCounterErrorValue || end == MTLCounterErrorValue || end < start) return UINT64_MAX;
    return end - start;
}

void WriteCounter(vkinsp::JsonWriter &w, const char *key, uint64_t delta) {
    if (delta == UINT64_MAX) return;
    w.Key(key); w.Uint(delta);
}

/** Resolves the samples, sends CapturePassTimings and releases the buffer. Not under g_mutex. */
void SendTimings(std::vector<PassTiming> &timings, Timing &timing) {
    if (timing.buffer == nil || timing.used == 0 || timings.empty()) {
        ReleaseTiming(timing);
        return;
    }
    if (@available(macOS 11.0, *)) {
        Internal internal;
        MTLTimestamp cpuEnd = 0, gpuEnd = 0;
        [timing.device sampleTimestamps:&cpuEnd gpuTimestamp:&gpuEnd];
        NSData *resolved = [timing.buffer resolveCounterRange:NSMakeRange(0, timing.used)];
        if (resolved == nil || resolved.length < timing.used * sizeof(MTLCounterResultTimestamp)) {
            Log("pass timings: the counter buffer did not resolve");
            ReleaseTiming(timing);
            return;
        }
        const MTLCounterResultTimestamp *samples = (const MTLCounterResultTimestamp *)resolved.bytes;
        // GPU ticks to nanoseconds, from the two CPU/GPU pairs around the capture. On Apple
        // Silicon the ratio is one; on other GPUs the timestamp counter runs at its own rate.
        double nsPerTick = 1.0;
        if (gpuEnd > timing.gpuStart && cpuEnd > timing.cpuStart) {
            nsPerTick = (double)(cpuEnd - timing.cpuStart) / (double)(gpuEnd - timing.gpuStart);
        }
        auto valid = [&](uint32_t index) {
            if (index >= timing.used) return false;
            const uint64_t t = samples[index].timestamp;
            return t != 0 && t != MTLCounterErrorValue;
        };
        uint64_t earliest = UINT64_MAX;
        for (const PassTiming &pt : timings) {
            if (valid(pt.slot.startIndex) && valid(pt.slot.endIndex)) {
                earliest = std::min(earliest, samples[pt.slot.startIndex].timestamp);
            }
        }
        // The counter sets, resolved once each; a pass without a sample in one shows nothing.
        const MTLCounterResultStatistic *statistics = timing.statistic.used == 0 ? nullptr
            : ResolvedSamples<MTLCounterResultStatistic>(
                  [timing.statistic.buffer resolveCounterRange:NSMakeRange(0, timing.statistic.used)],
                  timing.statistic.used);
        const MTLCounterResultStageUtilization *utilization = timing.utilization.used == 0 ? nullptr
            : ResolvedSamples<MTLCounterResultStageUtilization>(
                  [timing.utilization.buffer resolveCounterRange:NSMakeRange(0, timing.utilization.used)],
                  timing.utilization.used);
        const double toMs = nsPerTick / 1e6;
        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CapturePassTimings");
        w.Key("timestampPeriodNs"); w.Double(nsPerTick);
        w.Key("passes"); w.BeginArray();
        uint32_t sent = 0;
        for (const PassTiming &pt : timings) {
            const PassTimingSlot &s = pt.slot;
            if (!valid(s.startIndex) || !valid(s.endIndex)) continue;
            const uint64_t begin = samples[s.startIndex].timestamp;
            const uint64_t end = samples[s.endIndex].timestamp;
            if (end < begin) continue;
            w.BeginObject();
            w.Key("frame"); w.Uint(pt.frame);
            w.Key("commandBuffer"); w.Uint(pt.commandBufferId);
            w.Key("passIndex"); w.Uint(pt.passIndex);
            w.Key("kind"); w.String(pt.kind == PassKind::Compute ? "compute" : "render");
            w.Key("startMs"); w.Double((double)(begin - earliest) * toMs);
            w.Key("durationMs"); w.Double((double)(end - begin) * toMs);
            // The stage split, where the four boundaries all sampled. On a tile-based GPU the
            // two stages overlap, so the parts can sum to more than the whole.
            if (s.vertexEndIndex != UINT32_MAX && valid(s.vertexEndIndex) && valid(s.fragmentStartIndex)) {
                const uint64_t vertexEnd = samples[s.vertexEndIndex].timestamp;
                const uint64_t fragmentStart = samples[s.fragmentStartIndex].timestamp;
                if (vertexEnd >= begin) { w.Key("vertexMs"); w.Double((double)(vertexEnd - begin) * toMs); }
                if (end >= fragmentStart) { w.Key("fragmentMs"); w.Double((double)(end - fragmentStart) * toMs); }
            }
            if (s.statisticBuffer != nil && statistics != nullptr && s.statisticEnd < timing.statistic.used) {
                const MTLCounterResultStatistic &a = statistics[s.statisticStart];
                const MTLCounterResultStatistic &b = statistics[s.statisticEnd];
                w.Key("counters"); w.BeginObject();
                WriteCounter(w, "vertexInvocations", Delta(a.vertexInvocations, b.vertexInvocations));
                WriteCounter(w, "clipperInvocations", Delta(a.clipperInvocations, b.clipperInvocations));
                WriteCounter(w, "clipperPrimitivesOut", Delta(a.clipperPrimitivesOut, b.clipperPrimitivesOut));
                WriteCounter(w, "fragmentInvocations", Delta(a.fragmentInvocations, b.fragmentInvocations));
                WriteCounter(w, "fragmentsPassed", Delta(a.fragmentsPassed, b.fragmentsPassed));
                WriteCounter(w, "computeKernelInvocations", Delta(a.computeKernelInvocations, b.computeKernelInvocations));
                WriteCounter(w, "tessellationInputPatches", Delta(a.tessellationInputPatches, b.tessellationInputPatches));
                WriteCounter(w, "postTessellationVertexInvocations",
                             Delta(a.postTessellationVertexInvocations, b.postTessellationVertexInvocations));
                w.EndObject();
            }
            if (s.utilizationBuffer != nil && utilization != nullptr && s.utilizationEnd < timing.utilization.used) {
                const MTLCounterResultStageUtilization &a = utilization[s.utilizationStart];
                const MTLCounterResultStageUtilization &b = utilization[s.utilizationEnd];
                w.Key("utilization"); w.BeginObject();
                WriteCounter(w, "totalCycles", Delta(a.totalCycles, b.totalCycles));
                WriteCounter(w, "vertexCycles", Delta(a.vertexCycles, b.vertexCycles));
                WriteCounter(w, "tessellationCycles", Delta(a.tessellationCycles, b.tessellationCycles));
                WriteCounter(w, "postTessellationVertexCycles",
                             Delta(a.postTessellationVertexCycles, b.postTessellationVertexCycles));
                WriteCounter(w, "fragmentCycles", Delta(a.fragmentCycles, b.fragmentCycles));
                WriteCounter(w, "renderTargetCycles", Delta(a.renderTargetCycles, b.renderTargetCycles));
                w.EndObject();
            }
            w.EndObject();
            sent++;
        }
        w.EndArray();
        w.Key("count"); w.Uint(sent);
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
        Log("pass timings: %u of %zu passes timed", sent, timings.size());
    }
    ReleaseTiming(timing);
}

// --------------------------------------------------------------------------------------------
// Sending

void WriteCommand(vkinsp::JsonWriter &w, const RecordedCommand &c, uint32_t index) {
    w.BeginObject();
    w.Key("index"); w.Uint(index);
    w.Key("frame"); w.Uint(c.frame);
    w.Key("method"); w.String(c.method);
    w.Key("object");
    if (c.commandBufferId == 0) {
        w.Null();
    } else {
        w.BeginObject();
        w.Key("__id"); w.Uint(c.commandBufferId);
        w.Key("__class"); w.String("MTLCommandBuffer");
        w.EndObject();
    }
    if (c.encoderId != 0) {
        w.Key("encoder");
        w.BeginObject();
        w.Key("__id"); w.Uint(c.encoderId);
        w.Key("__class"); w.String(c.encoderType != nullptr ? c.encoderType : "MTLCommandEncoder");
        w.EndObject();
    }
    w.Key("args"); if (c.args.empty()) w.Null(); else w.Raw(c.args);
    if (!c.bufferData.empty()) {
        w.Key("bufferData"); w.BeginArray();
        for (uint64_t id : c.bufferData) w.Uint(id);
        w.EndArray();
    }
    if (!c.stack.empty()) {
        w.Key("stack"); w.BeginArray();
        for (uint64_t address : c.stack) w.String(HexAddress(address));
        w.EndArray();
    }
    w.EndObject();
}

/** Fills in the deferred read-backs, now that the GPU has completed the frame. */
void ResolveBuffers(std::vector<CapturedBuffer> &buffers) {
    for (CapturedBuffer &b : buffers) {
        if (b.staging != nil) {
            const uint8_t *bytes = static_cast<const uint8_t *>(b.staging.contents);
            if (bytes != nullptr) b.data.assign(bytes, bytes + b.size);
            else b.error = "staging buffer has no contents";
        } else if (b.managed && b.source != nil) {
            const uint8_t *bytes = static_cast<const uint8_t *>(b.source.contents);
            if (bytes != nullptr) b.data.assign(bytes + b.offset, bytes + b.offset + b.size);
            else b.error = "managed buffer has no contents";
        }
        [b.staging release];
        b.staging = nil;
        [b.source release];
        b.source = nil;
    }
}

/** CaptureBuffers (what was read) then one CaptureBufferData binary frame per buffer. */
void SendBuffers(std::vector<CapturedBuffer> &buffers) {
    if (buffers.empty()) return;
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureBuffers");
    w.Key("count"); w.Uint(buffers.size());
    w.Key("buffers"); w.BeginArray();
    for (const CapturedBuffer &b : buffers) {
        w.BeginObject();
        w.Key("id"); w.Uint(b.captureId);
        w.Key("buffer"); w.Uint(b.bufferId);
        w.Key("frame"); w.Uint(b.frame);
        w.Key("commandBuffer"); w.Uint(b.commandBufferId);
        w.Key("offset"); w.Uint(b.offset);
        w.Key("size"); w.Uint(b.error.empty() ? b.data.size() : 0);
        if (b.originalSize != 0) { w.Key("originalSize"); w.Uint(b.originalSize); }
        if (!b.error.empty()) { w.Key("error"); w.String(b.error); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));

    for (CapturedBuffer &b : buffers) {
        if (!b.error.empty() || b.data.empty()) continue;
        vkinsp::JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureBufferData");
        h.Key("id"); h.Uint(b.captureId);
        h.Key("size"); h.Uint(b.data.size());
        h.EndObject();
        Transport::Get().SendBinary(std::move(h.str()), std::move(b.data));
    }
}

/** CaptureTextureFrames (what was read) then one CaptureTextureData binary frame per attachment. */
void SendTextures(std::vector<PendingTexture> &textures) {
    if (textures.empty()) return;
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("CaptureTextureFrames");
    w.Key("count"); w.Uint(textures.size());
    w.Key("textures"); w.BeginArray();
    for (const PendingTexture &t : textures) {
        w.BeginObject();
        w.Key("id"); w.Uint(t.textureId);
        w.Key("frame"); w.Uint(t.frame);
        w.Key("commandBuffer"); w.Uint(t.commandBufferId);
        w.Key("passIndex"); w.Uint(t.passIndex);
        w.Key("attachment"); w.Uint(t.attachment);
        w.Key("format"); w.String(t.format);
        w.Key("aspect"); w.String(t.aspect);
        w.Key("width"); w.Uint(t.width);
        w.Key("height"); w.Uint(t.height);
        w.Key("depth"); w.Uint(1);
        w.Key("layers"); w.Uint(1);
        w.Key("mip"); w.Uint(t.level);
        w.Key("size"); w.Uint(t.size);
        if (!t.error.empty()) { w.Key("error"); w.String(t.error); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));

    for (PendingTexture &t : textures) {
        if (t.staging != nil && t.size != 0 && t.error.empty() && t.staging.contents != nullptr) {
            vkinsp::JsonWriter h;
            h.BeginObject();
            h.Key("action"); h.String("CaptureTextureData");
            h.Key("id"); h.Uint(t.textureId);
            h.Key("frame"); h.Uint(t.frame);
            h.Key("commandBuffer"); h.Uint(t.commandBufferId);
            h.Key("passIndex"); h.Uint(t.passIndex);
            h.Key("attachment"); h.Uint(t.attachment);
            h.Key("size"); h.Uint(t.size);
            h.EndObject();
            Transport::Get().SendBinary(std::move(h.str()), t.staging.contents, t.size);
        }
        // Owned since newBufferWithLength: (+1, and this file is built without ARC): a render
        // target is megabytes, so leaking one per pass per capture adds up quickly.
        [t.staging release];
        t.staging = nil;
        [t.source release];
        t.source = nil;
    }
}

/** Streams CaptureFrameResults then the command batches, and clears the recording. */
void Finish() {
  // Reached from a completion handler or from whichever thread committed the last command
  // buffer, neither of which is promised an autorelease pool; resolving the counters returns
  // an autoreleased NSData.
  @autoreleasepool {
    std::vector<RecordedCommand> commands;
    std::vector<CapturedBuffer> buffers;
    std::vector<PendingTexture> textures;
    std::vector<PassTiming> timings;
    Timing timing;
    uint32_t frames = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        commands.swap(g_commands);
        buffers.swap(g_buffers);
        textures.swap(g_textures);
        timings.swap(g_passTimings);
        timing = g_timing;
        g_timing = Timing();
        g_bufferRanges.clear();
        frames = g_frameIndex;
        g_frameIndex = 0;
        g_recording = false;
        g_finishPending = false;
    }

    const size_t batches = (commands.size() + kCommandsPerBatch - 1) / kCommandsPerBatch;
    {
        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureFrameResults");
        w.Key("frame"); w.Uint(0);
        w.Key("frames"); w.Uint(frames);
        w.Key("count"); w.Uint(commands.size());
        w.Key("batches"); w.Uint(batches);
        // Tells the UI which command-name vocabulary this capture uses, so it classifies draws,
        // passes and submits by Metal selectors rather than by vkCmd* names. A capture without
        // the field is Vulkan (app/src/renderer/command_sets.ts).
        w.Key("api"); w.String("metal");
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
    }

    for (size_t batch = 0; batch < batches; batch++) {
        const size_t begin = batch * kCommandsPerBatch;
        const size_t end = std::min(begin + kCommandsPerBatch, commands.size());
        vkinsp::JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("CaptureFrameCommands");
        w.Key("frame"); w.Uint(0);
        w.Key("index"); w.Uint(batch);
        w.Key("commands"); w.BeginArray();
        for (size_t i = begin; i < end; i++) WriteCommand(w, commands[i], (uint32_t)i);
        w.EndArray();
        w.EndObject();
        Transport::Get().SendJson(std::move(w.str()));
    }
    ResolveBuffers(buffers);
    SendBuffers(buffers);
    SendTextures(textures);
    SendTimings(timings, timing);
    Log("capture finished: %zu commands over %u frame(s), %zu batch(es), %zu buffer(s), "
        "%zu render target(s)", commands.size(), frames, batches, buffers.size(), textures.size());
  }
}

/** A frame ended. Arms a pending capture, counts a recorded frame, or finishes one. */
void AdvanceFrame() {
    // The frame counter and the timing report, on every boundary; the validation counts too.
    const uint64_t frame = OnFrameEnded();
    FlushValidation();
    bool finishNow = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_recording) {
            if (++g_frameIndex < g_wantFrames) return;
            // The frames are encoded; the capture goes out once their GPU work has completed.
            g_finishPending = true;
            finishNow = g_outstanding == 0;
        } else if (g_pending) {
            // A queued capture waits for its frame: the frame that starts now is `frame`, so a
            // frame already passed captures this one.
            if (g_options.atFrame != UINT64_MAX && frame < g_options.atFrame) return;
            g_pending = false;
            g_frameIndex = 0;
            g_commands.clear();
            g_buffers.clear();
            g_bufferRanges.clear();
            g_textures.clear();
            g_passTimings.clear();
            g_openPasses.clear();
            g_passCounters.clear();
            g_nextBufferId = 1;
            g_bufferBytes = 0;
            g_outstanding = 0;
            g_finishPending = false;
            ReleaseTiming(g_timing);
            g_recording = true;
            Log("capture started");
            return;
        } else {
            return;
        }
    }
    if (finishNow) Finish();
}

/** Counts a command buffer in flight, and sends the capture when the last one completes. */
void TrackCompletion(id commandBuffer) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_outstanding;
    }
    Internal internal;
    [(id<MTLCommandBuffer>)commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _) {
        bool finish = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            finish = --g_outstanding == 0 && g_finishPending;
        }
        if (finish) Finish();
    }];
}

/** The command buffer a recorded call belongs to, and the encoder's capture id if it was one. */
void Attribute(id object, id *commandBuffer, uint64_t *encoderId, const char **encoderType) {
    *commandBuffer = nil;
    *encoderId = 0;
    *encoderType = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_encoders.find((__bridge const void *)object);
        if (it != g_encoders.end()) {
            *commandBuffer = it->second.commandBuffer;
            *encoderId = it->second.captureId;
            *encoderType = it->second.type;
            return;
        }
    }
    // Not an encoder that was registered: the call was on the command buffer itself.
    if ([object conformsToProtocol:@protocol(MTLCommandBuffer)]) *commandBuffer = object;
}

/** Records a present marker on a command buffer, for a frame that ends without presentDrawable:. */
void RecordPresentMarker(id commandBuffer, const DrawableInfo &drawable) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("drawable"); w.Pointer(drawable.drawable);
    w.Key("drawableID"); w.Uint(drawable.drawableID);
    w.EndObject();
    RecordCommand("present", commandBuffer, w.str());
}

}  // namespace

// --------------------------------------------------------------------------------------------

void RequestCapture(const CaptureOptions &options) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_options = options;
    g_wantFrames = options.frameCount > 0 ? options.frameCount : 1;
    g_pending = true;
    if (options.atFrame == UINT64_MAX) Log("capture requested: %u frame(s)", g_wantFrames);
    else Log("capture requested: %u frame(s) at frame %llu", g_wantFrames, (unsigned long long)options.atFrame);
}

bool Recording() {
    return g_recording && !IsInternal();
}

uint64_t CommandBufferId(id commandBuffer) {
    if (commandBuffer == nil) return 0;
    const uint64_t existing = IdOf(commandBuffer);
    if (existing != 0) return existing;
    id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)commandBuffer;
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("label");
    if (cb.label == nil) w.Null(); else w.String(cb.label.UTF8String);
    w.Key("retainedReferences"); w.Boolean(cb.retainedReferences);
    w.EndObject();
    return TrackObject(commandBuffer, "MTLCommandBuffer", "commandBuffer", cb.commandQueue, w.str());
}

void RegisterEncoder(id encoder, id commandBuffer, const char *type, id parent) {
    if (encoder == nil || commandBuffer == nil) return;
    // Before taking g_mutex: the id comes from the tracker, which has a lock of its own.
    const uint64_t captureId = AllocateId();
    std::lock_guard<std::mutex> lock(g_mutex);
    EncoderInfo &info = g_encoders[(__bridge const void *)encoder];
    info.commandBuffer = commandBuffer;
    info.type = type;
    info.captureId = captureId;
    info.parent = parent;
}

void ForgetEncoder(id encoder) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_encoders.erase((__bridge const void *)encoder);
}

id EncoderCommandBuffer(id encoder, bool *secondary) {
    if (secondary != nullptr) *secondary = false;
    if (encoder == nil) return nil;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_encoders.find((__bridge const void *)encoder);
    if (it == g_encoders.end()) return nil;
    if (secondary != nullptr) *secondary = it->second.parent != nil;
    return it->second.commandBuffer;
}

bool IsCommandStreamObject(id object) {
    if (object == nil) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_encoders.count((__bridge const void *)object) != 0) return true;
    }
    return [object conformsToProtocol:@protocol(MTLCommandBuffer)];
}

void RecordCommand(const char *method, id object, const std::string &argsJson) {
    RecordCommandWithBuffers(method, object, argsJson, {});
}

void RecordCommandWithBuffers(const char *method, id object, const std::string &argsJson,
                              std::vector<uint64_t> bufferData) {
    if (!g_recording) return;
    RecordedCommand command;
    command.method = method;
    command.args = argsJson;
    command.bufferData = std::move(bufferData);
    // Two frames above: RecordCommand and the hook; the application's call follows Metal's
    // own frames, which the symbolizer marks internal.
    if (g_options.stacktraces) command.stack = CaptureStack(2);
    id commandBuffer = nil;
    Attribute(object, &commandBuffer, &command.encoderId, &command.encoderType);
    command.commandBufferId = CommandBufferId(commandBuffer);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording) return;
    command.frame = g_frameIndex;
    g_commands.push_back(std::move(command));
}

uint64_t QueueBufferCapture(id encoder, id buffer, uint64_t offset, uint64_t size) {
    if (!g_recording || buffer == nil || !g_options.captureBuffers) return 0;
    const uint64_t bufferId = IdOf(buffer);
    if (bufferId == 0) return 0;

    id<MTLBuffer> metalBuffer = (id<MTLBuffer>)buffer;
    const uint64_t length = metalBuffer.length;
    if (offset >= length) return 0;
    const uint64_t available = length - offset;
    uint64_t want = size == 0 ? available : std::min(size, available);

    CapturedBuffer captured;
    captured.bufferId = bufferId;
    captured.offset = offset;
    if (want > g_options.maxBufferSize) {
        captured.originalSize = want;
        want = g_options.maxBufferSize;
    }
    captured.size = want;

    id commandBuffer = nil;
    uint64_t encoderId = 0;
    const char *encoderType = nullptr;
    Attribute(encoder, &commandBuffer, &encoderId, &encoderType);
    captured.commandBufferId = CommandBufferId(commandBuffer);

    const MTLStorageMode storage = metalBuffer.storageMode;
    if (storage == MTLStorageModeShared) {
        const uint8_t *bytes = static_cast<const uint8_t *>(metalBuffer.contents);
        if (bytes == nullptr) captured.error = "shared buffer has no contents";
        else captured.data.assign(bytes + offset, bytes + offset + want);
    } else if (storage == MTLStorageModeManaged) {
        // The CPU copy may lag a GPU write; synchronizeResource: at the end of the pass brings it
        // up to date, and the bytes are read after the frame completes.
        captured.managed = true;
        captured.source = [metalBuffer retain];
    } else if (storage == MTLStorageModePrivate) {
        // No CPU copy at all: blitted into staging at the end of the pass, the way the Vulkan
        // layer reads every buffer.
        captured.source = [metalBuffer retain];
    } else {
        captured.error = "buffer storage mode cannot be read";
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording) {
        [captured.source release];
        return 0;
    }
    const auto key = std::make_tuple(bufferId, offset, want);
    auto known = g_bufferRanges.find(key);
    if (known != g_bufferRanges.end()) {
        [captured.source release];
        return known->second;
    }
    captured.captureId = g_nextBufferId++;
    captured.frame = g_frameIndex;
    const bool deferred = captured.source != nil;
    const uint64_t id = captured.captureId;
    // The per-capture budget, so a frame binding thousands of ranges does not swamp the
    // connection; what is over it is reported, not silently dropped.
    if (g_bufferBytes + captured.size > g_options.maxBufferTotal) {
        captured.error = "buffer capture budget exceeded";
        captured.data.clear();
        [captured.source release];
        captured.source = nil;
        captured.managed = false;
    } else {
        g_bufferBytes += captured.size;
    }
    if (deferred && captured.source != nil) {
        auto pass = g_openPasses.find((__bridge const void *)encoder);
        if (pass == g_openPasses.end()) {
            // A parallel render encoder's sub-encoder: the pass is its parent's.
            auto owner = g_encoders.find((__bridge const void *)encoder);
            if (owner != g_encoders.end() && owner->second.parent != nil) {
                pass = g_openPasses.find((__bridge const void *)owner->second.parent);
            }
        }
        if (pass == g_openPasses.end()) {
            captured.error = "bound outside a pass; nothing to copy it through";
            [captured.source release];
            captured.source = nil;
            captured.managed = false;
        } else {
            pass->second.deferredBuffers.push_back(id);
        }
    }
    g_bufferRanges[key] = id;
    g_buffers.push_back(std::move(captured));
    return id;
}

uint64_t QueueBytesCapture(const void *bytes, uint64_t size) {
    if (!g_recording || bytes == nullptr || size == 0 || !g_options.captureBuffers) return 0;
    CapturedBuffer captured;
    captured.size = std::min<uint64_t>(size, g_options.maxBufferSize);
    if (size > captured.size) captured.originalSize = size;
    const uint8_t *data = static_cast<const uint8_t *>(bytes);
    captured.data.assign(data, data + captured.size);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording) return 0;
    captured.captureId = g_nextBufferId++;
    captured.frame = g_frameIndex;
    if (g_bufferBytes + captured.size > g_options.maxBufferTotal) {
        captured.error = "buffer capture budget exceeded";
        captured.data.clear();
    } else {
        g_bufferBytes += captured.size;
    }
    g_buffers.push_back(std::move(captured));
    return g_buffers.back().captureId;
}

// --------------------------------------------------------------------------------------------
// Passes

PassTimingSlot ReserveRenderPassTiming(id commandBuffer, MTLRenderPassDescriptor *descriptor) {
    PassTimingSlot slot;
    if (!g_recording || commandBuffer == nil) return slot;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording) return slot;
    EnsureTiming(((id<MTLCommandBuffer>)commandBuffer).device);
    if (g_timing.buffer == nil) return slot;
    if (@available(macOS 11.0, *)) {
        if (g_timing.stageBoundary && descriptor != nil) {
            // The first attachment slot the application is not using itself.
            // The free attachment slots, the application's own left alone: one for the
            // timestamps, then one each for the counter sets the device has.
            std::vector<MTLRenderPassSampleBufferAttachmentDescriptor *> free;
            for (NSUInteger i = 0; i < 4; i++) {
                MTLRenderPassSampleBufferAttachmentDescriptor *a = descriptor.sampleBufferAttachments[i];
                if (a.sampleBuffer == nil) free.push_back(a);
            }
            if (free.empty()) return slot;
            slot = ReserveSamples(false, true);
            if (slot.sampleBuffer == nil) return slot;
            size_t next = 0;
            // NSUInteger throughout: MTLCounterDontSample is NSUIntegerMax, and truncating it to
            // 32 bits would ask for a sample at index 4294967295 instead of none.
            auto attach = [&](id buffer, NSUInteger start, NSUInteger vertexEnd, NSUInteger fragmentStart, NSUInteger end) {
                if (next >= free.size()) return false;
                MTLRenderPassSampleBufferAttachmentDescriptor *a = free[next++];
                a.sampleBuffer = (id<MTLCounterSampleBuffer>)buffer;
                a.startOfVertexSampleIndex = start;
                a.endOfVertexSampleIndex = vertexEnd;
                a.startOfFragmentSampleIndex = fragmentStart;
                a.endOfFragmentSampleIndex = end;
                return true;
            };
            attach(slot.sampleBuffer, slot.startIndex, slot.vertexEndIndex, slot.fragmentStartIndex, slot.endIndex);
            if (slot.statisticBuffer != nil
                && !attach(slot.statisticBuffer, slot.statisticStart, MTLCounterDontSample, MTLCounterDontSample, slot.statisticEnd)) {
                slot.statisticBuffer = nil;
            }
            if (slot.utilizationBuffer != nil
                && !attach(slot.utilizationBuffer, slot.utilizationStart, MTLCounterDontSample, MTLCounterDontSample, slot.utilizationEnd)) {
                slot.utilizationBuffer = nil;
            }
            return slot;
        }
    }
    if (g_timing.drawBoundary) slot = ReserveSamples(true, false);
    return slot;
}

PassTimingSlot ReserveComputePassTiming(id commandBuffer, MTLComputePassDescriptor *descriptor) {
    PassTimingSlot slot;
    if (!g_recording || commandBuffer == nil) return slot;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording) return slot;
    EnsureTiming(((id<MTLCommandBuffer>)commandBuffer).device);
    if (g_timing.buffer == nil) return slot;
    if (@available(macOS 11.0, *)) {
        if (g_timing.stageBoundary && descriptor != nil) {
            std::vector<MTLComputePassSampleBufferAttachmentDescriptor *> free;
            for (NSUInteger i = 0; i < 4; i++) {
                MTLComputePassSampleBufferAttachmentDescriptor *a = descriptor.sampleBufferAttachments[i];
                if (a.sampleBuffer == nil) free.push_back(a);
            }
            if (free.empty()) return slot;
            slot = ReserveSamples(false, false);
            if (slot.sampleBuffer == nil) return slot;
            // Cycles per stage mean nothing to a compute pass; the statistic set does.
            slot.utilizationBuffer = nil;
            size_t next = 0;
            auto attach = [&](id buffer, NSUInteger start, NSUInteger end) {
                if (next >= free.size()) return false;
                MTLComputePassSampleBufferAttachmentDescriptor *a = free[next++];
                a.sampleBuffer = (id<MTLCounterSampleBuffer>)buffer;
                a.startOfEncoderSampleIndex = start;
                a.endOfEncoderSampleIndex = end;
                return true;
            };
            attach(slot.sampleBuffer, slot.startIndex, slot.endIndex);
            if (slot.statisticBuffer != nil && !attach(slot.statisticBuffer, slot.statisticStart, slot.statisticEnd)) {
                slot.statisticBuffer = nil;
            }
            return slot;
        }
    }
    if (g_timing.dispatchBoundary) slot = ReserveSamples(true, false);
    return slot;
}

PassTimingSlot ReserveBlitPassTiming(id commandBuffer, MTLBlitPassDescriptor *descriptor) {
    PassTimingSlot slot;
    if (!g_recording || commandBuffer == nil) return slot;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording) return slot;
    EnsureTiming(((id<MTLCommandBuffer>)commandBuffer).device);
    if (g_timing.buffer == nil) return slot;
    if (@available(macOS 11.0, *)) {
        if (g_timing.stageBoundary && descriptor != nil) {
            for (NSUInteger i = 0; i < 4; i++) {
                MTLBlitPassSampleBufferAttachmentDescriptor *a = descriptor.sampleBufferAttachments[i];
                if (a.sampleBuffer != nil) continue;
                slot = ReserveSamples(false, false);
                if (slot.sampleBuffer == nil) return slot;
                slot.statisticBuffer = nil;
                slot.utilizationBuffer = nil;
                a.sampleBuffer = (id<MTLCounterSampleBuffer>)slot.sampleBuffer;
                a.startOfEncoderSampleIndex = slot.startIndex;
                a.endOfEncoderSampleIndex = slot.endIndex;
                return slot;
            }
            return slot;
        }
    }
    if (g_timing.blitBoundary) slot = ReserveSamples(true, false);
    if (slot.sampleBuffer != nil) {
        slot.statisticBuffer = nil;
        slot.utilizationBuffer = nil;
    }
    return slot;
}

uint32_t BeginPass(id encoder, id commandBuffer, PassKind kind, const PassTimingSlot &timing) {
    const void *cb = (__bridge const void *)commandBuffer;
    uint32_t index = 0;
    bool recording = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        index = g_passCounters[cb]++;
        recording = g_recording;
    }
    if (!recording || encoder == nil) return index;
    // Outside the lock: tracking the command buffer talks to the tracker and the transport.
    const uint64_t cbId = CommandBufferId(commandBuffer);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        OpenPass pass;
        pass.commandBuffer = commandBuffer;
        pass.commandBufferId = cbId;
        pass.passIndex = index;
        pass.kind = kind;
        pass.timing = timing;
        g_openPasses[(__bridge const void *)encoder] = std::move(pass);
    }
    if (timing.sampleBuffer != nil && timing.onEncoder) {
        if (@available(macOS 11.0, *)) {
            Internal internal;
            if ([encoder respondsToSelector:@selector(sampleCountersInBuffer:atSampleIndex:withBarrier:)]) {
                [(id<MTLComputeCommandEncoder>)encoder
                    sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)timing.sampleBuffer
                             atSampleIndex:timing.startIndex
                               withBarrier:YES];
                if (timing.statisticBuffer != nil) {
                    [(id<MTLComputeCommandEncoder>)encoder
                        sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)timing.statisticBuffer
                                 atSampleIndex:timing.statisticStart
                                   withBarrier:NO];
                }
            }
        }
    }
    return index;
}

void AddPassAttachment(id encoder, MTLRenderPassAttachmentDescriptor *a, uint32_t index,
                       bool depth) {
    if (a == nil || a.texture == nil || !g_options.captureTextures) return;
    id<MTLTexture> texture = a.texture;

    PendingTexture pending;
    pending.textureId = IdOf(texture);
    pending.attachment = index;
    pending.aspect = depth ? "depth" : "color";

    // Multisample attachments cannot be copied to a buffer; what can be read is the resolve.
    id<MTLTexture> source = texture;
    uint32_t level = (uint32_t)a.level;
    uint32_t slice = (uint32_t)a.slice;
    uint32_t depthPlane = (uint32_t)a.depthPlane;
    if (texture.sampleCount > 1) {
        if (a.resolveTexture != nil && (a.storeAction == MTLStoreActionMultisampleResolve
                                        || a.storeAction == MTLStoreActionStoreAndMultisampleResolve)) {
            source = a.resolveTexture;
            level = (uint32_t)a.resolveLevel;
            slice = (uint32_t)a.resolveSlice;
            depthPlane = (uint32_t)a.resolveDepthPlane;
        } else {
            pending.error = "multisample attachment is not resolved, and cannot be read directly";
        }
    }
    if (pending.error.empty() && source.framebufferOnly) {
        pending.error = "texture is framebufferOnly and cannot be a copy source";
    }
    if (pending.error.empty() && source.storageMode == MTLStorageModeMemoryless) {
        pending.error = "memoryless texture has no contents after the pass";
    }

    MTLBlitOption options = MTLBlitOptionNone;
    PixelFormatInfo info = depth ? DepthReadbackDetails(source.pixelFormat, &options)
                                 : PixelFormatDetails(source.pixelFormat);
    if (pending.error.empty() && (info.name == nullptr || info.name[0] == '\0')) {
        const char *enumName = PixelFormatEnumName(source.pixelFormat);
        pending.error = std::string("unsupported pixel format ")
            + (enumName[0] != '\0' ? enumName : std::to_string((int)source.pixelFormat));
    }
    if (pending.error.empty() && !depth && PixelFormatHasDepth(source.pixelFormat)) {
        // A depth texture in a colour slot: read it as depth.
        info = DepthReadbackDetails(source.pixelFormat, &options);
        pending.aspect = "depth";
    }

    pending.width = (uint32_t)std::max<NSUInteger>(1, source.width >> level);
    pending.height = (uint32_t)std::max<NSUInteger>(1, source.height >> level);
    pending.level = level;
    pending.slice = slice;
    pending.depthPlane = source.textureType == MTLTextureType3D ? depthPlane : 0;
    pending.options = options;
    if (pending.error.empty()) {
        pending.format = info.name;
        uint64_t bytesPerRow = 0;
        pending.size = (size_t)PixelFormatImageSize(info, pending.width, pending.height, &bytesPerRow);
        pending.bytesPerRow = bytesPerRow;
        if (pending.size > g_options.maxTextureSize) {
            pending.error = "exceeds max texture size";
            pending.size = 0;
        } else {
            pending.source = [source retain];
        }
    }
    if (!pending.error.empty()) {
        // Reported, not dropped: an empty Render Targets section with no reason given is the
        // hardest kind of gap to notice.
        Log("render target: %s, not read back", pending.error.c_str());
        const char *enumName = PixelFormatEnumName(source.pixelFormat);
        pending.format = enumName;
        pending.size = 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_openPasses.find((__bridge const void *)encoder);
    if (it == g_openPasses.end()) {
        [pending.source release];
        return;
    }
    pending.frame = g_frameIndex;
    pending.commandBufferId = it->second.commandBufferId;
    pending.passIndex = it->second.passIndex;
    it->second.attachments.push_back(std::move(pending));
}

void BeforeEndEncoding(id encoder) {
    PassTimingSlot timing;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_openPasses.find((__bridge const void *)encoder);
        if (it == g_openPasses.end()) return;
        timing = it->second.timing;
    }
    if (timing.sampleBuffer == nil || !timing.onEncoder) return;
    if (@available(macOS 11.0, *)) {
        Internal internal;
        if ([encoder respondsToSelector:@selector(sampleCountersInBuffer:atSampleIndex:withBarrier:)]) {
            [(id<MTLComputeCommandEncoder>)encoder
                sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)timing.sampleBuffer
                         atSampleIndex:timing.endIndex
                           withBarrier:YES];
            if (timing.statisticBuffer != nil) {
                [(id<MTLComputeCommandEncoder>)encoder
                    sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)timing.statisticBuffer
                             atSampleIndex:timing.statisticEnd
                               withBarrier:NO];
            }
        }
    }
}

void AfterEndEncoding(id encoder) {
    OpenPass pass;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_openPasses.find((__bridge const void *)encoder);
        if (it == g_openPasses.end()) return;
        pass = std::move(it->second);
        g_openPasses.erase(it);
        if (pass.timing.sampleBuffer != nil) {
            PassTiming pt;
            pt.frame = g_frameIndex;
            pt.commandBufferId = pass.commandBufferId;
            pt.passIndex = pass.passIndex;
            pt.kind = pass.kind;
            pt.slot = pass.timing;
            g_passTimings.push_back(pt);
        }
    }
    if (pass.commandBuffer == nil) return;

    bool anything = false;
    for (const PendingTexture &t : pass.attachments) {
        if (t.error.empty() && t.size != 0) anything = true;
    }
    anything = anything || !pass.deferredBuffers.empty();
    if (!anything) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (PendingTexture &t : pass.attachments) g_textures.push_back(std::move(t));
        return;
    }

    // The application has ended its encoder, so the command buffer will take another. This is the
    // Metal counterpart of the layer appending barriers and vkCmdCopyImageToBuffer to the
    // application's command buffer at vkCmdEndRenderPass.
    Internal internal;
    id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)pass.commandBuffer;
    id<MTLDevice> device = commandBuffer.device;
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    if (blit == nil) {
        Log("read-back: could not open a blit encoder on the command buffer");
        std::lock_guard<std::mutex> lock(g_mutex);
        for (PendingTexture &t : pass.attachments) {
            t.error = "could not open a blit encoder for the read-back";
            [t.source release];
            t.source = nil;
            g_textures.push_back(std::move(t));
        }
        return;
    }
    blit.label = @"gpu-inspector readback";
    for (PendingTexture &t : pass.attachments) {
        if (!t.error.empty() || t.size == 0 || t.source == nil) continue;
        t.staging = [device newBufferWithLength:t.size options:MTLResourceStorageModeShared];
        if (t.staging == nil) {
            t.error = "could not allocate a staging buffer";
            continue;
        }
        [blit copyFromTexture:t.source
                  sourceSlice:t.slice
                  sourceLevel:t.level
                 sourceOrigin:MTLOriginMake(0, 0, t.depthPlane)
                   sourceSize:MTLSizeMake(t.width, t.height, 1)
                     toBuffer:t.staging
            destinationOffset:0
       destinationBytesPerRow:(NSUInteger)t.bytesPerRow
     destinationBytesPerImage:t.size
                      options:t.options];
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint64_t id : pass.deferredBuffers) {
            for (CapturedBuffer &b : g_buffers) {
                if (b.captureId != id || b.source == nil) continue;
                if (b.managed) {
                    [blit synchronizeResource:b.source];
                } else {
                    b.staging = [device newBufferWithLength:b.size
                                                    options:MTLResourceStorageModeShared];
                    if (b.staging == nil) {
                        b.error = "could not allocate a staging buffer";
                        continue;
                    }
                    [blit copyFromBuffer:b.source
                            sourceOffset:b.offset
                                toBuffer:b.staging
                       destinationOffset:0
                                    size:b.size];
                }
                break;
            }
        }
    }
    [blit endEncoding];

    std::lock_guard<std::mutex> lock(g_mutex);
    for (PendingTexture &t : pass.attachments) g_textures.push_back(std::move(t));
}

// --------------------------------------------------------------------------------------------
// Frame boundaries

void OnDrawableAcquired(id drawable, id texture) {
    if (drawable == nil || texture == nil) return;
    DrawableInfo info;
    info.drawable = (__bridge const void *)drawable;
    if ([drawable respondsToSelector:@selector(drawableID)]) {
        info.drawableID = (uint64_t)[(id<MTLDrawable>)drawable drawableID];
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_drawableOfTexture[(__bridge const void *)texture] = info;
}

void OnRenderTarget(id commandBuffer, id texture) {
    if (commandBuffer == nil || texture == nil) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_drawableOfTexture.find((__bridge const void *)texture);
    if (it == g_drawableOfTexture.end()) return;
    g_targetOfCommandBuffer[(__bridge const void *)commandBuffer] = it->second;
}

void OnPresentDrawable(id commandBuffer, id drawable) {
    if (commandBuffer == nil) return;
    DrawableInfo info;
    info.drawable = (__bridge const void *)drawable;
    if (drawable != nil && [drawable respondsToSelector:@selector(drawableID)]) {
        info.drawableID = (uint64_t)[(id<MTLDrawable>)drawable drawableID];
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presenting.insert((__bridge const void *)commandBuffer);
    if (drawable != nil) g_presentedByCommandBuffer.push_back(info);
}

bool OnDrawablePresent(id drawable) {
    if (drawable == nil) return false;
    const void *pointer = (__bridge const void *)drawable;
    uint64_t drawableID = 0;
    if ([drawable respondsToSelector:@selector(drawableID)]) {
        drawableID = (uint64_t)[(id<MTLDrawable>)drawable drawableID];
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Already accounted for: the convenience method calling through, not the application
        // presenting the drawable itself. Matched by pointer, then by drawableID for a driver
        // that hands the present a different object for the same drawable.
        for (auto it = g_presentedByCommandBuffer.begin(); it != g_presentedByCommandBuffer.end(); ++it) {
            if (it->drawable == pointer || (drawableID != 0 && it->drawableID == drawableID)) {
                g_presentedByCommandBuffer.erase(it);
                return false;
            }
        }
        // The application presents drawables itself. From here on the frame ends at the commit
        // of the command buffer that rendered into the drawable.
        g_directPresent = true;
        if (g_countedDrawables.erase(pointer) != 0) return false;
    }
    AdvanceFrame();
    return true;
}

void OnCommit(id commandBuffer) {
    bool presents = false;
    bool recording = false;
    bool boundary = false;
    DrawableInfo target;
    bool hasTarget = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        recording = g_recording;
        presents = g_presenting.erase((__bridge const void *)commandBuffer) != 0;
        auto it = g_targetOfCommandBuffer.find((__bridge const void *)commandBuffer);
        if (it != g_targetOfCommandBuffer.end()) {
            target = it->second;
            hasTarget = true;
            g_targetOfCommandBuffer.erase(it);
        }
        g_passCounters.erase((__bridge const void *)commandBuffer);
        boundary = presents;
        if (!presents && hasTarget && g_directPresent
            && g_countedDrawables.count(target.drawable) == 0) {
            // The frame ends here; the drawable's own present, when it arrives from the
            // scheduled handler, will find it already counted.
            g_countedDrawables.insert(target.drawable);
            boundary = true;
        }
    }
    if (recording) {
        // The present marker first, so a frame that ends through the drawable's own present
        // still reads present-then-commit the way the convenience path does.
        if (boundary && !presents) RecordPresentMarker(commandBuffer, target);
        RecordCommand("commit", commandBuffer, {});
        // Before the hook forwards the commit, which is the only time this is allowed.
        TrackCompletion(commandBuffer);
    }
    // Logged only for a boundary the present hooks did not report themselves.
    if (boundary && !presents && g_commitBoundaryLogger != nullptr) {
        g_commitBoundaryLogger(commandBuffer);
    }
    if (boundary) {
        AdvanceFrame();
        // An Xcode trace starts and stops here too, so it holds whole frames.
        GpuTraceAtFrameBoundary(((id<MTLCommandBuffer>)commandBuffer).device);
    }
}

void SetCommitBoundaryLogger(void (*logger)(id)) {
    g_commitBoundaryLogger = logger;
}

}  // namespace mtlinsp
