// Incoming messages from the inspector UI.
//
// The counterpart of HandleUiMessage in layer/src/layer.cpp, answering the same actions with the
// same shapes. Only what the Metal side can honour is handled; anything else is logged and
// ignored, so a UI that asks for something Vulkan-only does not wedge the session.
#include "ui_messages.h"

#include "capture.h"
#include "image.h"
#include "json_parse.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"

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
        } else if (action == "Capture") {
            CaptureOptions options;
            options.frameCount = (uint32_t)message.GetNumber("frameCount", 1);
            RequestCapture(options);
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
