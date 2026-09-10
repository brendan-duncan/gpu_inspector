#include "capture.h"

#include "formats.h"
#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"

#import <Metal/Metal.h>

#include <atomic>
#include <mutex>
#include <map>
#include <set>
#include <vector>

namespace mtlinsp {
namespace {

// A bound buffer range read back with the capture. `data` is filled at once, because a buffer in
// a shared storage mode is already mapped; the Vulkan layer has to record a GPU copy instead.
// A colour attachment blitted into a staging buffer at the end of its pass. The bytes are read
// out in the command buffer's completion handler, once the GPU has actually produced them.
struct PendingTexture {
    uint64_t textureId = 0;
    uint32_t frame = 0;
    uint32_t passIndex = 0;
    uint32_t attachment = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string format;
    size_t size = 0;
    uint64_t bytesPerRow = 0;
    std::string error;
    id<MTLBuffer> staging = nil;
};

// A render pass in flight: what to blit when the application ends the encoder.
struct OpenPass {
    id commandBuffer = nil;
    uint32_t passIndex = 0;
    std::vector<PendingTexture> attachments;
    std::vector<id<MTLTexture>> textures;
};

struct CapturedBuffer {
    uint64_t id = 0;
    uint64_t bufferId = 0;     // the tracked MTLBuffer
    uint32_t frame = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t originalSize = 0; // set when the range was truncated
    std::string error;
    std::vector<uint8_t> data;
};

struct RecordedCommand {
    uint32_t frame = 0;
    std::string method;
    uint64_t objectId = 0;      // the encoder or command buffer, when it is tracked
    std::string objectType;     // its protocol name, for the reference the UI shows
    std::string args;
    std::vector<uint64_t> bufferData;  // CapturedBuffer ids, in the order the UI expects
};

std::mutex g_mutex;
// Armed but not started: the next present begins the capture.
bool g_pending = false;
std::atomic<bool> g_recording{false};
uint32_t g_wantFrames = 1;
uint32_t g_frameIndex = 0;
std::vector<RecordedCommand> g_commands;
// Command buffers that have been asked to present: their commit ends a frame.
std::set<const void *> g_presenting;
// Command buffers committed during the capture that have not completed yet. The read-back blits
// are in them, so the staging holds nothing until the count reaches zero.
//
// The handler is registered in the commit hook, before the commit is forwarded, because
// addCompletedHandler: is only legal before a command buffer is committed. Adding one later —
// from a frame boundary that arrives on Metal's scheduled-handler thread, which is where Unity's
// [drawable present] runs — makes Metal assert and abort the application.
int g_outstanding = 0;
bool g_finishPending = false;
// Drawables a command buffer was asked to present. [MTLCommandBuffer presentDrawable:] calls the
// drawable's own present once the queue schedules the buffer — later, and on another thread, so
// the re-entry guard cannot pair them. Without this both boundaries fire and frames count twice.
std::set<const void *> g_presentedByCommandBuffer;
std::vector<CapturedBuffer> g_buffers;
uint64_t g_nextBufferId = 1;

// Matches the Vulkan layer's default: enough for a vertex or uniform buffer, not so much that a
// large storage buffer floods the connection.
constexpr uint64_t kMaxBufferSize = 64 * 1024;

std::vector<PendingTexture> g_textures;
std::map<const void *, OpenPass> g_openPasses;   // encoder -> its pass
std::map<const void *, uint32_t> g_passCounters; // command buffer -> passes begun
uint64_t g_nextTextureId = 1;

thread_local int g_internalDepth = 0;


// A capture's commands go out in batches rather than one message, so a frame with tens of
// thousands of commands does not become a single enormous JSON string.
constexpr size_t kCommandsPerBatch = 2000;

void WriteCommand(vkinsp::JsonWriter &w, const RecordedCommand &c, uint32_t index) {
    w.BeginObject();
    w.Key("index"); w.Uint(index);
    w.Key("frame"); w.Uint(c.frame);
    w.Key("method"); w.String(c.method);
    w.Key("object");
    if (c.objectId == 0) {
        w.Null();
    } else {
        w.BeginObject();
        w.Key("__id"); w.Uint(c.objectId);
        w.Key("__class"); w.String(c.objectType);
        w.EndObject();
    }
    w.Key("args"); if (c.args.empty()) w.Null(); else w.Raw(c.args);
    if (!c.bufferData.empty()) {
        w.Key("bufferData"); w.BeginArray();
        for (uint64_t id : c.bufferData) w.Uint(id);
        w.EndArray();
    }
    w.EndObject();
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
        w.Key("id"); w.Uint(b.id);
        w.Key("buffer"); w.Uint(b.bufferId);
        w.Key("frame"); w.Uint(b.frame);
        w.Key("commandBuffer"); w.Uint(0);
        w.Key("offset"); w.Uint(b.offset);
        w.Key("size"); w.Uint(b.error.empty() ? b.data.size() : 0);
        if (b.originalSize != 0) { w.Key("originalSize"); w.Uint(b.originalSize); }
        if (!b.error.empty()) { w.Key("error"); w.String(b.error); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));

    for (const CapturedBuffer &b : buffers) {
        if (!b.error.empty() || b.data.empty()) continue;
        vkinsp::JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureBufferData");
        h.Key("id"); h.Uint(b.id);
        h.Key("size"); h.Uint(b.data.size());
        h.EndObject();
        Transport::Get().SendBinary(std::move(h.str()), b.data.data(), b.data.size());
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
        w.Key("commandBuffer"); w.Uint(0);
        w.Key("passIndex"); w.Uint(t.passIndex);
        w.Key("attachment"); w.Uint(t.attachment);
        w.Key("format"); w.String(t.format);
        w.Key("aspect"); w.String("color");
        w.Key("width"); w.Uint(t.width);
        w.Key("height"); w.Uint(t.height);
        w.Key("depth"); w.Uint(1);
        w.Key("layers"); w.Uint(1);
        w.Key("mip"); w.Uint(0);
        w.Key("size"); w.Uint(t.size);
        if (!t.error.empty()) { w.Key("error"); w.String(t.error); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));

    for (const PendingTexture &t : textures) {
        if (t.staging == nil || t.size == 0 || !t.error.empty()) continue;
        vkinsp::JsonWriter h;
        h.BeginObject();
        h.Key("action"); h.String("CaptureTextureData");
        h.Key("id"); h.Uint(t.textureId);
        h.Key("frame"); h.Uint(t.frame);
        h.Key("commandBuffer"); h.Uint(0);
        h.Key("passIndex"); h.Uint(t.passIndex);
        h.Key("attachment"); h.Uint(t.attachment);
        h.Key("size"); h.Uint(t.size);
        h.EndObject();
        Transport::Get().SendBinary(std::move(h.str()), t.staging.contents, t.size);
    }
    // Owned since newBufferWithLength: (+1, and this file is built without ARC): a render target
    // is megabytes, so leaking one per pass per capture adds up quickly.
    for (PendingTexture &t : textures) {
        [t.staging release];
        t.staging = nil;
    }
}

/** Streams CaptureFrameResults then the command batches, and clears the recording. */
void Finish() {
    std::vector<RecordedCommand> commands;
    std::vector<CapturedBuffer> buffers;
    std::vector<PendingTexture> textures;
    uint32_t frames = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        commands.swap(g_commands);
        buffers.swap(g_buffers);
        textures.swap(g_textures);
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
    SendBuffers(buffers);
    SendTextures(textures);
    Log("capture finished: %zu commands over %u frame(s), %zu batch(es), %zu buffer(s), "
        "%zu render target(s)", commands.size(), frames, batches, buffers.size(), textures.size());
}

/**
 * A frame ended. Arms a pending capture, counts a recorded frame, or finishes one.
 *
 * `completionSource` is a command buffer whose completion means the frame's GPU work — the
 * read-back blits included — is done; nil falls back to the last one committed.
 */
void AdvanceFrame() {
    bool finishNow = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_recording) {
            if (++g_frameIndex < g_wantFrames) return;
            // The frames are encoded; the capture goes out once their GPU work has completed.
            g_finishPending = true;
            finishNow = g_outstanding == 0;
        } else if (g_pending) {
            g_pending = false;
            g_frameIndex = 0;
            g_commands.clear();
            g_buffers.clear();
            g_textures.clear();
            g_openPasses.clear();
            g_passCounters.clear();
            g_nextBufferId = 1;
            g_nextTextureId = 1;
            g_outstanding = 0;
            g_finishPending = false;
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

}  // namespace

void RequestCapture(const CaptureOptions &options) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wantFrames = options.frameCount > 0 ? options.frameCount : 1;
    g_pending = true;
    Log("capture requested: %u frame(s)", g_wantFrames);
}

bool Recording() {
    return g_recording && g_internalDepth == 0;
}

uint64_t QueueBufferCapture(id buffer, uint64_t offset, uint64_t size) {
    if (!g_recording || buffer == nil) return 0;
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
    if (want > kMaxBufferSize) {
        captured.originalSize = want;
        want = kMaxBufferSize;
    }
    captured.size = want;

    const void *contents = metalBuffer.storageMode == MTLStorageModePrivate ? nullptr
                                                                            : metalBuffer.contents;
    if (contents == nullptr) {
        captured.error = "buffer is in private storage (no mapped contents to read)";
    } else {
        const uint8_t *bytes = static_cast<const uint8_t *>(contents) + offset;
        captured.data.assign(bytes, bytes + want);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording) return 0;
    captured.id = g_nextBufferId++;
    captured.frame = g_frameIndex;
    g_buffers.push_back(std::move(captured));
    return g_buffers.back().id;
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
    command.objectId = IdOf(object);
    if (command.objectId != 0 && object != nil) command.objectType = ClassName(object);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording) return;
    command.frame = g_frameIndex;
    g_commands.push_back(std::move(command));
}

Internal::Internal() { ++g_internalDepth; }
Internal::~Internal() { --g_internalDepth; }

uint32_t BeginPass(id encoder, id commandBuffer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const void *cb = (__bridge const void *)commandBuffer;
    const uint32_t index = g_passCounters[cb]++;
    if (g_recording && encoder != nil) {
        OpenPass pass;
        pass.commandBuffer = commandBuffer;
        pass.passIndex = index;
        g_openPasses[(__bridge const void *)encoder] = std::move(pass);
    }
    return index;
}

/** Called from the render encoder hook once the attachments are known. */
void AddPassAttachment(id encoder, id textureObject, uint32_t attachment) {
    id<MTLTexture> texture = (id<MTLTexture>)textureObject;
    if (texture == nil) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_openPasses.find((__bridge const void *)encoder);
    if (it == g_openPasses.end()) return;

    PendingTexture pending;
    pending.textureId = IdOf(texture);
    pending.frame = g_frameIndex;
    pending.passIndex = it->second.passIndex;
    pending.attachment = attachment;
    pending.width = (uint32_t)texture.width;
    pending.height = (uint32_t)texture.height;
    const PixelFormatInfo info = PixelFormatDetails(texture.pixelFormat);
    pending.format = info.name;
    uint64_t bytesPerRow = 0;
    const uint64_t imageSize = PixelFormatImageSize(info, pending.width, pending.height,
                                                    &bytesPerRow);
    if (pending.format.empty() || imageSize == 0) {
        // Reported, not dropped: an empty Render Targets section with no reason given is the
        // hardest kind of gap to notice.
        const char *enumName = PixelFormatEnumName(texture.pixelFormat);
        Log("render target: unsupported pixel format %s (%lu), not read back", enumName,
            (unsigned long)texture.pixelFormat);
        pending.error = std::string("unsupported pixel format ")
            + (enumName[0] != '\0' ? enumName : std::to_string((int)texture.pixelFormat));
        pending.size = 0;
        it->second.attachments.push_back(std::move(pending));
        it->second.textures.push_back(texture);
        return;
    }
    pending.size = (size_t)imageSize;
    pending.bytesPerRow = bytesPerRow;
    it->second.attachments.push_back(std::move(pending));
    it->second.textures.push_back(texture);
}

void EndRenderPass(id encoder) {
    OpenPass pass;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_openPasses.find((__bridge const void *)encoder);
        if (it == g_openPasses.end()) return;
        pass = std::move(it->second);
        g_openPasses.erase(it);
    }
    if (pass.attachments.empty() || pass.commandBuffer == nil) return;

    // The application has ended its encoder, so the command buffer will take another. This is the
    // Metal counterpart of the layer appending barriers and vkCmdCopyImageToBuffer to the
    // application's command buffer at vkCmdEndRenderPass.
    Internal internal;
    id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)pass.commandBuffer;
    id<MTLDevice> device = commandBuffer.device;
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    blit.label = @"gpu-inspector readback";
    for (size_t i = 0; i < pass.attachments.size(); i++) {
        PendingTexture &t = pass.attachments[i];
        if (!t.error.empty() || t.size == 0) continue;
        id<MTLTexture> texture = pass.textures[i];
        t.staging = [device newBufferWithLength:t.size options:MTLResourceStorageModeShared];
        if (t.staging == nil) continue;
        [blit copyFromTexture:texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(t.width, t.height, 1)
                     toBuffer:t.staging
            destinationOffset:0
       destinationBytesPerRow:(NSUInteger)t.bytesPerRow
     destinationBytesPerImage:t.size];
    }
    [blit endEncoding];

    std::lock_guard<std::mutex> lock(g_mutex);
    for (PendingTexture &t : pass.attachments) {
        if (t.staging != nil || !t.error.empty()) g_textures.push_back(std::move(t));
    }
}

void OnPresentDrawable(id commandBuffer, id drawable) {
    if (commandBuffer == nil) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presenting.insert((__bridge const void *)commandBuffer);
    if (drawable != nil) g_presentedByCommandBuffer.insert((__bridge const void *)drawable);
}

bool OnDrawablePresent(id drawable) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Already accounted for: this is the convenience method calling through, not the
        // application presenting the drawable itself.
        if (g_presentedByCommandBuffer.erase((__bridge const void *)drawable) != 0) return false;
    }
    AdvanceFrame();
    return true;
}

void OnCommit(id commandBuffer) {
    bool presents = false;
    bool recording = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        recording = g_recording;
        presents = g_presenting.erase((__bridge const void *)commandBuffer) != 0;
    }
    // Before the hook forwards the commit, which is the only time this is allowed.
    if (recording) TrackCompletion(commandBuffer);
    if (presents) AdvanceFrame();
}

}  // namespace mtlinsp
