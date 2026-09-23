// Validation messages: the D3D12 debug layer's info queue as the UI's ValidationMessage stream.
//
// With DXINSP_DEBUG_LAYER=1 the debug layer is enabled before the device is created; the device's
// ID3D12InfoQueue then collects every message. Where ID3D12InfoQueue1 exists its callback delivers
// each message as it is produced, so one fired inside a command list method the library is
// recording is attached to that command (CommandScope::Current); otherwise the queue is drained
// at every present. Messages are deduplicated by text with repeat counts sent as ValidationCount,
// the first 2000 unique ones kept for a UI that connects later, the way the Vulkan layer keeps
// its messenger's output.
#pragma once

#include "common.h"

#include <string>

namespace dxinsp
{

class ValidationLog
{
public:
    static ValidationLog& Get();

    /** DXINSP_DEBUG_LAYER: the launch dialog's "Validation layer". */
    static bool DebugLayerRequested();
    /** Before D3D12CreateDevice is forwarded: enables the debug layer when requested (once). */
    void EnableDebugLayer();
    /** After a device was created: takes its info queue, registers the callback or notes that polling is needed. */
    void OnDeviceCreated(ID3D12Device* device);
    void OnDeviceReleased(ID3D12Device* device);
    /** At every present: drains the info queues without a callback, and sends the repeat counts. */
    void Poll(uint64_t frame);
    /** Every kept message, for a client that just connected (after the object snapshot). */
    void SendSnapshot();
    /** A message of the library's own (a read-back that failed), as an info-severity message. */
    void Note(const std::string& text);

private:
    ValidationLog() = default;
    struct Impl;
    Impl* _impl = nullptr;
    Impl& impl();
};

}  // namespace dxinsp
