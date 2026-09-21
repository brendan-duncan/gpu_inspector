// Incoming messages from the inspector UI.
//
// The counterpart of HandleUiMessage in src/vulkan/src/layer.cpp, answering the same actions with the
// same shapes. Only what the Metal side can honour is handled; anything else is logged and
// ignored, so a UI that asks for something Vulkan-only does not wedge the session.
#include "ui_messages.h"

#include "capture.h"
#include "cpu_timeline.h"
#include "frame_pause.h"
#include "gpu_trace.h"
#include "hud.h"
#include "image.h"
#include "json_parse.h"
#include "json_writer.h"
#include "stacktrace.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"
#include "validation.h"

#import <Foundation/Foundation.h>

namespace mtlinsp {
namespace {

// Tells the UI what the pause state is now, so its button follows a pause the UI did not ask for
// (a capture resuming the application below) as well as one it did.
void SendPauseState() {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("PauseState");
    w.Key("paused"); w.Boolean(gpuinsp::FramePause::Get().Paused());
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
}

void HandleMessage(const std::string &text) {
    // The receiver is a plain std::thread with no autorelease pool of its own, and the image
    // read-back below autoreleases a command buffer, an encoder and the texture it looked up.
    // Without a pool here every one of those leaked, with a runtime warning each time.
    @autoreleasepool {
        vkinsp::JsonValue message;
        if (!vkinsp::JsonParser::Parse(text, message)) {
            Log("bad message from the UI: %s", text.c_str());
            return;
        }
        const std::string action = message.GetString("action");
        Log("ui message: %s", action.c_str());

        if (action == "Ping") {
            Transport::Get().SendJson("{\"action\":\"Pong\"}");
        } else if (action == "RequestSnapshot") {
            SendSnapshot();
            SendValidationSnapshot();
        } else if (action == "Hud") {
            // The in-app HUD (hud.h): the application's frame time drawn over its own window.
            Hud::Get().SetEnabled(message.GetBool("enabled", false));
        } else if (action == "Pause") {
            // Live pause (frame_pause.h): the application is held at its frame boundary. "step"
            // lets that many frames through and stays paused.
            if (const vkinsp::JsonValue *v = message.Get("step")) {
                gpuinsp::FramePause::Get().Step(v->num >= 1 ? (uint32_t)v->num : 1u);
            } else {
                gpuinsp::FramePause::Get().SetPaused(message.GetBool("paused", false));
            }
            SendPauseState();
        } else if (action == "Capture") {
            // A capture is recorded from frames the application renders, and a paused application
            // renders none: waiting here would simply hang. Resuming is the honest answer, and the
            // UI is told so its pause button follows.
            if (gpuinsp::FramePause::Get().Paused()) {
                Log("capture requested while paused: resuming");
                gpuinsp::FramePause::Get().SetPaused(false);
                SendPauseState();
            }
            // The same fields the Vulkan layer reads (layer.cpp); what the Metal side cannot
            // honour (sampled images, stack traces) is left at its default.
            CaptureOptions options;
            options.frameCount = (uint32_t)message.GetNumber("frameCount", 1);
            if (const vkinsp::JsonValue *v = message.Get("atFrame")) {
                if (v->kind == vkinsp::JsonValue::Number && v->num >= 0) options.atFrame = (uint64_t)v->num;
            }
            options.maxBufferSize = (uint64_t)message.GetNumber("maxBufferSize", (double)options.maxBufferSize);
            options.maxBufferTotal = (uint64_t)message.GetNumber("maxBufferTotal", (double)options.maxBufferTotal);
            options.maxTextureSize = (uint64_t)message.GetNumber("maxTextureSize", (double)options.maxTextureSize);
            options.captureTextures = message.GetBool("captureTextures", true);
            options.captureSampledTextures = message.GetBool("captureSampledTextures", true);
            options.maxSampledTextureTotal =
                (uint64_t)message.GetNumber("maxSampledTextureTotal", (double)options.maxSampledTextureTotal);
            options.captureBuffers = message.GetBool("captureBuffers", true);
            options.profilePasses = message.GetBool("profilePasses", true);
            options.stacktraces = message.GetBool("stacktraces", false);
            options.overdraw = message.GetBool("overdraw", false);
            // {texture, x, y, mip, layer}: the pixel to follow through the captured frame.
            if (const vkinsp::JsonValue *h = message.Get("pixelHistory"); h != nullptr && h->kind == vkinsp::JsonValue::Object) {
                options.pixelHistory.enabled = true;
                options.pixelHistory.texture = (uint64_t)h->GetNumber("texture");
                options.pixelHistory.x = (uint32_t)h->GetNumber("x");
                options.pixelHistory.y = (uint32_t)h->GetNumber("y");
                options.pixelHistory.level = (uint32_t)h->GetNumber("mip");
                options.pixelHistory.slice = (uint32_t)h->GetNumber("layer");
            }
            if (options.maxBufferSize == 0) options.maxBufferSize = 64 * 1024;
            RequestCapture(options);
        } else if (action == "TimingCapture") {
            // Frame timings over minutes, for finding a hitch rather than a slow frame
            // (cpu_timeline.h). `sampleHz` is accepted and ignored: the call stack sampler is
            // Windows-only.
            if (message.GetBool("start", false)) BeginTimingCapture((uint32_t)message.GetNumber("sampleHz", 0));
            else EndTimingCapture();
        } else if (action == "SaveGpuTrace") {
            RequestGpuTrace(message.GetString("path"));
        } else if (action == "RequestStacktraces") {
            std::vector<uint64_t> ids;
            if (const vkinsp::JsonValue *list = message.Get("ids")) {
                for (const vkinsp::JsonValue &v : list->arr) {
                    if (v.kind == vkinsp::JsonValue::Number) ids.push_back((uint64_t)v.num);
                }
            }
            SendStacktraces(ids);
        } else if (action == "RequestSymbols") {
            // The addresses a capture's commands carry: a frame per address, in request order.
            StackTrace addresses;
            if (const vkinsp::JsonValue *list = message.Get("addresses")) {
                for (const vkinsp::JsonValue &v : list->arr) {
                    if (v.kind == vkinsp::JsonValue::String) addresses.push_back(strtoull(v.str.c_str(), nullptr, 0));
                    else if (v.kind == vkinsp::JsonValue::Number) addresses.push_back((uint64_t)v.num);
                }
            }
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("action"); w.String("Symbols");
            w.Key("frames"); WriteStackFrames(w, Symbolize(addresses));
            w.EndObject();
            Transport::Get().SendJson(std::move(w.str()));
        } else if (action == "RequestBlob") {
            SendBlob((uint64_t)message.GetNumber("id"), (uint32_t)message.GetNumber("index"));
        } else if (action == "RequestImage") {
            SendImageData((uint64_t)message.GetNumber("id"), (uint32_t)message.GetNumber("mip"),
                          (uint32_t)message.GetNumber("layer"));
        } else if (action == "Settings") {
            // "Record all command buffers" has no Metal counterpart: a command buffer is encoded
            // and submitted once, so there is no earlier recording for a capture to have missed.
        } else {
            Log("unhandled ui message: %s", action.c_str());
        }
    }
}

}  // namespace

void StartUiMessages() {
    Transport::Get().SetMessageHandler(HandleMessage);
}

}  // namespace mtlinsp
