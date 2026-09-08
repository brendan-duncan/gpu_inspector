// Validation messages: the layer registers a VK_EXT_debug_utils messenger on every instance and
// forwards what the validation layer (or the driver) reports, so the UI can list the messages,
// link them to the objects they name, and mark those objects. The messages are kept in the layer
// too: a UI that connects later still gets everything reported since the instance was created,
// and repeats of the same message (engines re-issue the same mistake every frame) are counted
// instead of resent.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkinsp {

struct InstanceData;

class ValidationLog {
public:
    static ValidationLog& Get();

    // Adds VK_EXT_debug_utils to the create info's extensions when the loader offers it and the
    // application did not enable it. `names` owns the new extension list; returns whether the
    // extension is enabled afterwards.
    static bool EnsureDebugUtils(PFN_vkGetInstanceProcAddr nextGipa, VkInstanceCreateInfo& info, std::vector<const char*>& names);

    void CreateMessenger(InstanceData* inst);
    void DestroyMessenger(InstanceData* inst);

    // The messenger callback (any thread).
    void OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                   const VkDebugUtilsMessengerCallbackDataEXT* data);

    // Frame ordinal attached to new messages (the presenting device's frame counter).
    void SetFrame(uint64_t frame) { _frame.store(frame, std::memory_order_relaxed); }
    // Sends the repeat counts that changed since the last call (from the frame tick).
    void Flush();
    // Resends every message on connect, after the object snapshot.
    void SendSnapshot();

private:
    ValidationLog() = default;

    struct ObjectRef {
        int handleType;       // HandleType, or -1 when unknown
        std::string className;
        uint64_t handle;
        std::string name;     // pObjectName, when given
    };
    struct Entry {
        uint64_t key = 0;
        std::string severity;
        std::vector<std::string> types;
        std::string idName;
        int32_t idNumber = 0;
        std::string message;
        uint64_t frame = 0;
        std::vector<ObjectRef> objects;
        std::vector<std::string> queueLabels;
        std::vector<std::string> cmdBufLabels;
        uint64_t count = 0;
        bool dirty = false;   // count changed since it was last sent
    };

    std::string MessageJson(const Entry& e) const;

    std::mutex _mutex;
    std::unordered_map<std::string, uint64_t> _keys;   // dedupe text -> key
    std::vector<Entry> _entries;                        // by key - 1
    uint64_t _dropped = 0;                              // unique messages beyond the cap
    bool _droppedDirty = false;
    bool _anyDirty = false;
    std::atomic<uint64_t> _frame{0};
};

} // namespace vkinsp
