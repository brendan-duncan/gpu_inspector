#include "server.h"

#include "capture.h"
#include "state.h"

#include <gpu_inspector/sdk/config.h>
#include <gpu_inspector/sdk/transport.h>

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
