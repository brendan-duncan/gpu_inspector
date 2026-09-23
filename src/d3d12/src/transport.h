// TCP transport between the capture library and the inspector UI.
//
// The wire format is the Vulkan layer's, byte for byte (src/vulkan/src/transport.h), because the UI
// speaks one protocol regardless of which API produced the messages:
//   u32 payloadLength (little endian, not counting the kind byte), u8 kind, payload
//   kind 0: UTF-8 JSON text
//   kind 1: u32 headerLength, JSON header, raw bytes
// One client at a time, on 127.0.0.1:DXINSP_PORT -- or, when that was not set, on the first free
// port of the small range in target_probe.h, which is also where the handshake that tells a probe
// from a client is described. Outgoing messages are queued and written by a
// sender thread, so an intercepted D3D12 call never blocks on the socket. The Metal library's
// copy of the same code decoupled the three calls the Vulkan one makes into its tracker and
// validation log as the callbacks below; this is that shape again.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dxinsp
{

class Transport
{
public:
    static Transport& Get();

    /** Starts the listener thread. Safe to call more than once. */
    void Start();
    bool Connected() const;

    void SendJson(std::string json);
    /**
     * A binary frame. The payload is copied once into the frame; the vector form is moved when
     * it is the only thing in the frame.
     */
    void SendBinary(std::string headerJson, const void* data, size_t size);
    /** Waits until everything queued has been written, or the timeout: for the last messages before exit. */
    void Flush(uint32_t timeoutMs);

    /** Called on the listener thread once a client has connected, to send it a snapshot. */
    void SetOnConnect(std::function<void()> handler);
    void SetOnDisconnect(std::function<void()> handler);
    /** Incoming JSON from the UI, called on the receiver thread. */
    void SetMessageHandler(std::function<void(const std::string&)> handler);

private:
    Transport() = default;
    struct Impl;
    Impl* _impl = nullptr;
};

}  // namespace dxinsp
