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

    void Reset() {
        _commands = std::make_shared<CommandList>();
        _pass = ActivePass{};
        _passCount = 0;
        _ended = false;
    }

    VkDevice device() const { return _device; }
    VkCommandBuffer commandBuffer() const { return _commandBuffer; }
    ActivePass& pass() { return _pass; }
    uint32_t NextPassIndex() { return _passCount++; }
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
    bool _ended = false;
};

} // namespace vkinsp
