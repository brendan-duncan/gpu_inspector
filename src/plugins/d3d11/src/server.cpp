#include "server.h"

#include "capture.h"
#include "state.h"

#include <gpu_inspector/sdk/config.h>
#include <gpu_inspector/sdk/transport.h>

#include <cstring>
#include <string>

namespace d3d11insp
{

using gpuinsp::sdk::Config;
using gpuinsp::sdk::JsonValue;
using gpuinsp::sdk::Server;

namespace
{

void HandleRequestBlob(const JsonValue& msg)
{
    const uint64_t id = (uint64_t)msg.GetNumber("id");
    const size_t index = (size_t)msg.GetNumber("index");
    std::shared_ptr<std::vector<uint8_t>> blob = BlobOf(id, index);
    JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("ObjectBlob");
    w.Key("id");
    w.Uint(id);
    w.Key("index");
    w.Uint(index);
    w.Key("size");
    w.Uint(blob ? blob->size() : 0);
    w.EndObject();
    if (blob)
        Server::Get().SendBinary(w.str(), blob->data(), blob->size());
    else
        Server::Get().SendJson(w.str());
}

void OnMessage(const std::string& json)
{
    JsonValue msg;
    if (!gpuinsp::sdk::ParseJson(json, msg))
    {
        Log("a message from the inspector does not parse: %.200s", json.c_str());
        return;
    }
    const std::string action = msg.GetString("action");
    if (action == "Ping")
    {
        Server::Get().SendJson("{\"action\":\"Pong\"}");
    }
    else if (action == "RequestSnapshot")
    {
        SendSnapshot();
    }
    else if (action == "Capture")
    {
        RequestCapture(msg);
    }
    else if (action == "RequestBlob")
    {
        HandleRequestBlob(msg);
    }
    else if (action == "RequestStacktraces")
    {
        // The library collects no creation stacks; saying so lets whoever asked stop waiting.
        Server::Get().SendJson("{\"action\":\"Stacktraces\",\"available\":false,\"stacks\":[]}");
    }
    else
    {
        // Settings, RequestImage and the rest: nothing this library answers yet.
        Log("ignored %s", action.c_str());
    }
}

}  // namespace

void StartServer()
{
    static bool started = false;
    if (started)
        return;
    started = true;
    gpuinsp::sdk::ServerOptions o;
    o.api = "Direct3D 11";
    o.socketPrefix = "d3d11insp";
    const long port = Config::Get().Number("D3D11INSP_PORT", 0);
    o.port = port > 0 && port < 65536 ? (uint16_t)port : 0;
    o.log = [](const std::string& line) { LogAlways("%s", line.c_str()); };
    Server& server = Server::Get();
    server.OnConnect([] { SendSnapshot(); });
    server.OnMessage(OnMessage);
    server.OnDisconnect([] { OnDisconnect(); });
    server.Start(std::move(o));
}

}  // namespace d3d11insp

// The application's side of a capture (include/gpu_inspector.h), which finds these by name. The
// request goes to the inspector rather than straight to the capture: the capture bar's options are
// the inspector's to choose, and a tab has to be waiting for what comes back.
extern "C" __declspec(dllexport) int GpuInspectorConnected(void)
{
    return gpuinsp::sdk::Server::Get().Connected() ? 1 : 0;
}

extern "C" __declspec(dllexport) int GpuInspectorCaptureNamed(uint32_t frameCount, const char* label)
{
    if (!gpuinsp::sdk::Server::Get().Connected())
        return 0;
    // The label is the application's words for the capture (the tab's name); bounded, since a
    // string that is not one would otherwise become a message of any length.
    const std::string name = label ? std::string(label, strnlen(label, 200)) : std::string();
    d3d11insp::JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("AppCaptureRequest");
    w.Key("frameCount");
    w.Uint(frameCount ? frameCount : 1u);
    if (!name.empty())
    {
        w.Key("label");
        w.String(name);
    }
    w.EndObject();
    gpuinsp::sdk::Server::Get().SendJson(w.str());
    if (name.empty())
        d3d11insp::Log("capture requested by the application: %u frame(s)", frameCount ? frameCount : 1u);
    else
        d3d11insp::Log("capture requested by the application: %u frame(s), \"%s\"", frameCount ? frameCount : 1u, name.c_str());
    return 1;
}

extern "C" __declspec(dllexport) int GpuInspectorCapture(uint32_t frameCount)
{
    return GpuInspectorCaptureNamed(frameCount, nullptr);
}
