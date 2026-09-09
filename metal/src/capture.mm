#include "capture.h"

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

/**
 * MTLPixelFormat to the protocol's pixel format name.
 *
 * The protocol names formats the way Vulkan does, and the UI's decoder (renderer/vulkan/
 * vk_format.ts, texture_decode.ts) is 570 lines built around those names. Emitting the canonical
 * name for the same layout reuses all of it; the visible cost is that a Metal texture's format
 * reads as VK_FORMAT_B8G8R8A8_UNORM in the UI. Worth revisiting, not worth a second decoder now.
 */
const char *FormatName(MTLPixelFormat format) {
    switch (format) {
        case MTLPixelFormatBGRA8Unorm:      return "VK_FORMAT_B8G8R8A8_UNORM";
        case MTLPixelFormatBGRA8Unorm_sRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
        case MTLPixelFormatRGBA8Unorm:      return "VK_FORMAT_R8G8B8A8_UNORM";
        case MTLPixelFormatRGBA8Unorm_sRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
        case MTLPixelFormatRGBA16Float:     return "VK_FORMAT_R16G16B16A16_SFLOAT";
        case MTLPixelFormatRGBA32Float:     return "VK_FORMAT_R32G32B32A32_SFLOAT";
        case MTLPixelFormatR8Unorm:         return "VK_FORMAT_R8_UNORM";
        case MTLPixelFormatRG8Unorm:        return "VK_FORMAT_R8G8_UNORM";
        default:                            return "";
    }
}

uint32_t BytesPerPixel(MTLPixelFormat format) {
    switch (format) {
        case MTLPixelFormatR8Unorm:         return 1;
        case MTLPixelFormatRG8Unorm:        return 2;
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatRGBA8Unorm_sRGB: return 4;
        case MTLPixelFormatRGBA16Float:     return 8;
        case MTLPixelFormatRGBA32Float:     return 16;
        default:                            return 0;
    }
}

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
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));

    for (const PendingTexture &t : textures) {
        if (t.staging == nil || t.size == 0) continue;
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
    pending.format = FormatName(texture.pixelFormat);
    const uint32_t bpp = BytesPerPixel(texture.pixelFormat);
    if (pending.format.empty() || bpp == 0) {
        Log("render target: unsupported pixel format %lu, not read back",
            (unsigned long)texture.pixelFormat);
        return;
    }
    pending.size = (size_t)pending.width * pending.height * bpp;
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
        id<MTLTexture> texture = pass.textures[i];
        t.staging = [device newBufferWithLength:t.size options:MTLResourceStorageModeShared];
        if (t.staging == nil) continue;
        const NSUInteger bytesPerRow = t.size / t.height;
        [blit copyFromTexture:texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(t.width, t.height, 1)
                     toBuffer:t.staging
            destinationOffset:0
       destinationBytesPerRow:bytesPerRow
     destinationBytesPerImage:t.size];
    }
    [blit endEncoding];

    std::lock_guard<std::mutex> lock(g_mutex);
    for (PendingTexture &t : pass.attachments) {
        if (t.staging != nil) g_textures.push_back(std::move(t));
    }
}

void OnPresentDrawable(id commandBuffer) {
    if (commandBuffer == nil) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presenting.insert((__bridge const void *)commandBuffer);
}

void OnCommit(id commandBuffer) {
    bool finish = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_presenting.erase((__bridge const void *)commandBuffer) == 0) return;
        if (g_recording) {
            // The frame just encoded is complete.
            if (++g_frameIndex >= g_wantFrames) finish = true;
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
            g_recording = true;
            Log("capture started");
            return;
        }
    }
    if (!finish) return;
    // The read-back blits are in this command buffer and their staging holds nothing until the GPU
    // has run them, so the capture is sent from the completion handler rather than here. Added
    // before the hook forwards to the real commit, so it cannot be missed.
    Internal internal;
    [(id<MTLCommandBuffer>)commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _) {
        Finish();
    }];
}

}  // namespace mtlinsp
