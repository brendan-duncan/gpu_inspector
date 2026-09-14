// TCP transport between the layer and the inspector UI.
//
// The layer listens on 127.0.0.1:VKINSP_PORT and accepts one client at a time. Frames are:
//   u32 payloadLength (little endian), u8 kind, payload
//   kind 0: UTF-8 JSON text
//   kind 1: u32 headerLength, JSON header, raw bytes
// Outgoing messages are queued and written by a sender thread so API calls never block on I/O.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vkinsp {

class Transport {
public:
    static Transport& Get();

    // Starts the listener thread. Safe to call more than once.
    void Start();
    bool Connected() const;

    void SendJson(std::string json);
    void SendBinary(std::string headerJson, const void* data, size_t size);

    // Handler for incoming JSON messages from the UI (called on the receiver thread).
    void SetMessageHandler(std::function<void(const std::string&)> handler);

private:
    Transport() = default;
    struct Impl;
    Impl* _impl = nullptr;
};

} // namespace vkinsp
