// TCP transport between the capture library and the inspector UI.
//
// The wire format is the Vulkan layer's, byte for byte (layer/src/transport.h), because the UI
// speaks one protocol regardless of which API produced the messages:
//   u32 payloadLength (little endian, not counting the kind byte), u8 kind, payload
//   kind 0: UTF-8 JSON text
//   kind 1: u32 headerLength, JSON header, raw bytes
// One client at a time, on 127.0.0.1:MTLINSP_PORT. Outgoing messages are queued and written by a
// sender thread, so an intercepted Metal call never blocks on the socket.
//
// This duplicates layer/src/transport.cpp, which is the same code with `Log`, `Tracker` and
// `ValidationLog` wired in directly. The two should become one shared module — the coupling is
// three calls, replaced here by the OnConnect/OnDisconnect callbacks below, which is what such a
// module would need anyway. That refactor is not done here because it changes shipping Vulkan
// code that does not build on macOS, so it cannot be tested from this side.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mtlinsp {

class Transport {
public:
    static Transport &Get();

    /** Starts the listener thread. Safe to call more than once. */
    void Start();
    bool Connected() const;

    void SendJson(std::string json);
    /**
     * A binary frame. The payload is queued as its own buffer rather than appended to the
     * header, so a render target of tens of megabytes is copied once, from the staging buffer,
     * and the vector form is not copied at all.
     */
    void SendBinary(std::string headerJson, const void *data, size_t size);
    void SendBinary(std::string headerJson, std::vector<uint8_t> payload);

    /** Called on the listener thread once a client has connected, to send it a snapshot. */
    void SetOnConnect(std::function<void()> handler);
    void SetOnDisconnect(std::function<void()> handler);
    /** Incoming JSON from the UI, called on the receiver thread. */
    void SetMessageHandler(std::function<void(const std::string &)> handler);

private:
    Transport() = default;
    struct Impl;
    Impl *impl_ = nullptr;
};

}  // namespace mtlinsp
