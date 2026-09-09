#include "capture.h"

#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"

#import <Foundation/Foundation.h>

#include <atomic>
#include <mutex>
#include <set>
#include <vector>

namespace mtlinsp {
namespace {

struct RecordedCommand {
    uint32_t frame = 0;
    std::string method;
    uint64_t objectId = 0;      // the encoder or command buffer, when it is tracked
    std::string objectType;     // its protocol name, for the reference the UI shows
    std::string args;
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
    w.EndObject();
}

/** Streams CaptureFrameResults then the command batches, and clears the recording. */
void Finish() {
    std::vector<RecordedCommand> commands;
    uint32_t frames = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        commands.swap(g_commands);
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
    Log("capture finished: %zu commands over %u frame(s), %zu batch(es)", commands.size(), frames,
        batches);
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

void RecordCommand(const char *method, id object, const std::string &argsJson) {
    if (!g_recording) return;
    RecordedCommand command;
    command.method = method;
    command.args = argsJson;
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
            g_recording = true;
            Log("capture started");
            return;
        }
    }
    if (finish) Finish();
}

}  // namespace mtlinsp
