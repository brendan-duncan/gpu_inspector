#include "capture.h"

#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"

#import <Metal/Metal.h>

#include <atomic>
#include <mutex>
#include <set>
#include <vector>

namespace mtlinsp {
namespace {

// A bound buffer range read back with the capture. `data` is filled at once, because a buffer in
// a shared storage mode is already mapped; the Vulkan layer has to record a GPU copy instead.
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

/** Streams CaptureFrameResults then the command batches, and clears the recording. */
void Finish() {
    std::vector<RecordedCommand> commands;
    std::vector<CapturedBuffer> buffers;
    uint32_t frames = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        commands.swap(g_commands);
        buffers.swap(g_buffers);
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
    Log("capture finished: %zu commands over %u frame(s), %zu batch(es), %zu buffer(s)",
        commands.size(), frames, batches, buffers.size());
}

}  // namespace

void RequestCapture(const CaptureOptions &options) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wantFrames = options.frameCount > 0 ? options.frameCount : 1;
    g_pending = true;
    Log("capture requested: %u frame(s)", g_wantFrames);
}

bool Recording() {
    return g_recording;
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
            g_nextBufferId = 1;
            g_recording = true;
            Log("capture started");
            return;
        }
    }
    if (finish) Finish();
}

}  // namespace mtlinsp
