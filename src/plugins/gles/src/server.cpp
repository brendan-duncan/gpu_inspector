#include "server.h"

#include "capture.h"
#include "state.h"

#include <gpu_inspector/sdk/config.h>
#include <gpu_inspector/sdk/transport.h>

#include <cstring>
#include <string>

namespace glesinsp {

using gpuinsp::sdk::Config;
using gpuinsp::sdk::JsonValue;
using gpuinsp::sdk::Server;

namespace {

void OnMessage(const std::string& json) {
    JsonValue msg;
    if (!gpuinsp::sdk::ParseJson(json, msg)) {
        Log("a message from the inspector does not parse: %.200s", json.c_str());
        return;
    }
    const std::string action = msg.GetString("action");
    if (action == "Ping") {
        Server::Get().SendJson("{\"action\":\"Pong\"}");
    } else if (action == "RequestSnapshot") {
        SendSnapshot();
    } else if (action == "Capture") {
        RequestCapture(msg);
    } else if (action == "RequestStacktraces") {
        // The library collects no creation stacks; saying so lets whoever asked stop waiting.
        Server::Get().SendJson("{\"action\":\"Stacktraces\",\"available\":false,\"stacks\":[]}");
    } else {
        // Settings, RequestBlob, RequestImage and the rest: nothing this library answers yet.
        Log("ignored %s", action.c_str());
    }
}

}  // namespace

void StartServer() {
    static bool started = false;
    if (started) return;
    started = true;
    gpuinsp::sdk::ServerOptions o;
    o.api = "OpenGL ES";
    o.socketPrefix = "glesinsp";
    const long port = Config::Get().Number("GLESINSP_PORT", 0);
    o.port = port > 0 && port < 65536 ? (uint16_t)port : 0;
    o.log = [](const std::string& line) { LogAlways("%s", line.c_str()); };
    Server& server = Server::Get();
    server.OnConnect([] { SendSnapshot(); });
    server.OnMessage(OnMessage);
    server.OnDisconnect([] { OnDisconnect(); });
    server.Start(std::move(o));
}

}  // namespace glesinsp

// The application's side of a capture (include/gpu_inspector.h), which finds these by name: on
// Windows in the injected DLL, on Linux in the preloaded library, on Android in the OpenGL ES
// layer. The request goes to the inspector rather than straight to the capture: the capture bar's
// options are the inspector's to choose, and a tab has to be waiting for what comes back.
#if defined(_WIN32)
#define GLESINSP_API extern "C" __declspec(dllexport)
#else
#define GLESINSP_API extern "C" __attribute__((visibility("default")))
#endif

GLESINSP_API int GpuInspectorConnected(void) {
    return gpuinsp::sdk::Server::Get().Connected() ? 1 : 0;
}

GLESINSP_API int GpuInspectorCaptureNamed(uint32_t frameCount, const char* label) {
    if (!gpuinsp::sdk::Server::Get().Connected()) return 0;
    // The label is the application's words for the capture (the tab's name); bounded, since a
    // string that is not one would otherwise become a message of any length.
    const std::string name = label ? std::string(label, strnlen(label, 200)) : std::string();
    gpuinsp::sdk::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("AppCaptureRequest");
    w.Key("frameCount"); w.Uint(frameCount ? frameCount : 1u);
    if (!name.empty()) { w.Key("label"); w.String(name); }
    w.EndObject();
    gpuinsp::sdk::Server::Get().SendJson(w.str());
    if (name.empty()) glesinsp::Log("capture requested by the application: %u frame(s)", frameCount ? frameCount : 1u);
    else glesinsp::Log("capture requested by the application: %u frame(s), \"%s\"", frameCount ? frameCount : 1u, name.c_str());
    return 1;
}

GLESINSP_API int GpuInspectorCapture(uint32_t frameCount) {
    return GpuInspectorCaptureNamed(frameCount, nullptr);
}
