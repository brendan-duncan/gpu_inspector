// Incoming messages from the inspector UI.
//
// The counterpart of HandleUiMessage in layer/src/layer.cpp, answering the same actions with the
// same shapes. Only what the Metal side can honour is handled; anything else is logged and
// ignored, so a UI that asks for something Vulkan-only does not wedge the session.
#include "ui_messages.h"

#include "capture.h"
#include "gpu_trace.h"
#include "image.h"
#include "json_parse.h"
#include "stacktrace.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"
#include "validation.h"

#import <Foundation/Foundation.h>

namespace mtlinsp {
namespace {

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
        } else if (action == "Capture") {
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
            options.captureBuffers = message.GetBool("captureBuffers", true);
            options.profilePasses = message.GetBool("profilePasses", true);
            options.stacktraces = message.GetBool("stacktraces", false);
            if (options.maxBufferSize == 0) options.maxBufferSize = 64 * 1024;
            RequestCapture(options);
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
