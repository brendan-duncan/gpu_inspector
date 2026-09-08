// Records serialized vkCmd* calls for one command buffer during frame capture.
//
// Generated forwarders call DeviceData::RecorderFor(commandBuffer); when it returns a recorder,
// they serialize the command's arguments into the JSON writer between Begin() and End().
// A command buffer is externally synchronized by the application, so a recorder needs no lock.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "json_writer.h"
#include "vk_commands.gen.h"

namespace vkinsp {

struct RecordedCommand {
    VkCmdId id;
    int64_t result;
    std::string args;      // JSON object with the command's arguments
    std::string extra;     // optional JSON fragment merged into the command entry ("children":[...])
};

using CommandList = std::vector<RecordedCommand>;

// Render pass / dynamic rendering state while recording, used to read attachments back at pass end.
struct ActivePass {
    bool active = false;
    bool dynamic = false;                    // vkCmdBeginRendering
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::vector<VkImageView> attachments;    // framebuffer or imageless/dynamic attachments
    std::vector<VkImageLayout> layouts;      // layout of each attachment after the pass ends
    std::vector<VkImageView> resolveViews;   // dynamic rendering: resolve targets (may be null)
    uint32_t subpass = 0;
    VkRect2D renderArea{};
    uint32_t layerCount = 1;
    uint32_t passIndex = 0;                  // index of this pass within the command buffer
    uint32_t query = UINT32_MAX;             // timestamp query pair (begin, begin + 1) when profiling
};

// A run of dispatches outside a render pass, timed as one "compute pass": from the first dispatch
// to the next barrier, event wait, render pass, debug label, secondary execution or the end of
// the command buffer (see CaptureManager::OnBeforeDispatch / OnEndComputePass).
struct ActiveComputePass {
    bool active = false;
    uint32_t index = 0;                      // index of this compute pass within the command buffer
    uint32_t query = UINT32_MAX;             // timestamp query pair when profiling
};

// A buffer range queued for readback (see CaptureManager::QueueBufferCapture). The copy into
// staging is recorded when the pending list is flushed: at once outside a render pass, at the
// end of the pass otherwise (transfer commands are not allowed inside one).
struct PendingBufferCopy {
    uint32_t captureId = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceSize stagingOffset = 0;
};

class CommandRecorder {
public:
    CommandRecorder(VkDevice device, VkCommandBuffer cb, HandleResolver* resolver)
        : _device(device), _commandBuffer(cb), _writer(resolver) {}

    JsonWriter& Begin(VkCmdId id) {
        _current = id;
        _writer.Reset();
        return _writer;
    }

    void End(JsonWriter& w, int64_t result) {
        _commands->push_back({_current, result, std::move(w.str()), std::string()});
        w.Reset();
    }

    // Attaches extra JSON to the most recently recorded command.
    void SetExtraOnLast(std::string extra) {
        if (!_commands->empty()) _commands->back().extra = std::move(extra);
    }

    // Frozen snapshot of the commands recorded so far (shared; the recorder starts a new list
    // only when the command buffer is re-begun).
    std::shared_ptr<const CommandList> Snapshot() const { return _commands; }

    void Reset(bool renderPassContinue = false) {
        _commands = std::make_shared<CommandList>();
        _pass = ActivePass{};
        _passCount = 0;
        _compute = ActiveComputePass{};
        _computeCount = 0;
        _ended = false;
        _renderPassContinue = renderPassContinue;
        _pendingCopies.clear();
    }

    // True while transfer commands cannot be recorded: inside a render pass, or in a secondary
    // command buffer that executes inside one (VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT).
    bool InsidePass() const { return _pass.active || _renderPassContinue; }
    bool renderPassContinue() const { return _renderPassContinue; }
    std::vector<PendingBufferCopy>& pendingCopies() { return _pendingCopies; }
    // Query pair written by the pass-begin pre-hook, claimed by the pass when it starts.
    uint32_t pendingQuery = UINT32_MAX;

    VkDevice device() const { return _device; }
    VkCommandBuffer commandBuffer() const { return _commandBuffer; }
    ActivePass& pass() { return _pass; }
    uint32_t NextPassIndex() { return _passCount++; }
    ActiveComputePass& compute() { return _compute; }
    uint32_t NextComputeIndex() { return _computeCount++; }
    bool ended() const { return _ended; }
    void MarkEnded() { _ended = true; }
    size_t commandCount() const { return _commands->size(); }

private:
    VkDevice _device;
    VkCommandBuffer _commandBuffer;
    JsonWriter _writer;
    VkCmdId _current = VkCmdId::Count;
    std::shared_ptr<CommandList> _commands = std::make_shared<CommandList>();
    ActivePass _pass;
    uint32_t _passCount = 0;
    ActiveComputePass _compute;
    uint32_t _computeCount = 0;
    bool _ended = false;
    bool _renderPassContinue = false;
    std::vector<PendingBufferCopy> _pendingCopies;
};

} // namespace vkinsp
