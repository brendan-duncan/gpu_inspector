// Live object tracker: every Vulkan object the application creates, with the serialized
// arguments of the call that created it (its "descriptor"), its parent, and its label.
//
// Mirrors WebGPU Inspector's object database on the page side: objects get stable ids that
// never repeat even if the driver reuses handle values. The tracker is also the HandleResolver
// used by the JSON serializers, so handles inside descriptors and command arguments become
// {"__id": N, "__class": "VkImage"} references.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "json_writer.h"
#include "stacktrace.h"
#include "vk_commands.gen.h"

namespace vkinsp {

struct TrackedObject {
    uint64_t id = 0;
    HandleType type = HT_Count;
    uint64_t handle = 0;
    uint64_t parentId = 0;
    VkCmdId cmd = VkCmdId::Count;
    uint32_t index = 0;          // index into the creating call's output array (pipelines, sets...)
    std::string args;            // JSON: arguments of the creating call (shared by array outputs)
    std::string label;
    std::vector<uint64_t> children;
    std::vector<std::pair<std::string, std::string>> updates;  // key -> ObjectUpdate message JSON
    // Binary payloads retrievable by the UI (RequestBlob {id, index}): SPIR-V code etc.
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> blobs;
    // Where the object was created (VKINSP_STACKTRACES), symbolized on request.
    StackTrace stack;
};

class Tracker : public HandleResolver {
public:
    static Tracker& Get();

    // HandleResolver
    uint64_t Resolve(int handleType, uint64_t handle) override;

    // Thread-local writer for serializing a creating call's arguments before OnCreate().
    JsonWriter& BeginArgs();
    void EndArgs();

    // Registers a new object. Returns its id; a handle that is already tracked keeps its id, and
    // moves to the new owner when one is given (a driver hands the same handle out again under a
    // new parent: the images of a swapchain recreated with oldSwapchain).
    uint64_t OnCreate(HandleType type, uint64_t handle, HandleType parentType, uint64_t parentHandle,
                      VkCmdId cmd, uint32_t index, const std::string& args);
    void OnDestroy(HandleType type, uint64_t handle);
    void OnDestroyChildren(HandleType type, uint64_t handle);
    // Reports the objects still alive under an owner about to be destroyed (a device or an
    // instance) as a LeakReport message: type counts and the first objects with their names.
    // Objects the application cannot destroy (queues, physical devices, swapchain images) and
    // those freed with their pool (descriptor sets, command buffers) are not counted.
    void SendLeakReport(HandleType type, uint64_t handle);
    void SetLabel(HandleType type, uint64_t handle, const char* label);
    // Records a named JSON update (e.g. "memory" binding) on an object and streams it as an
    // ObjectUpdate message. Updates are replayed as part of the snapshot.
    void Update(uint64_t id, const std::string& key, const std::string& messageJson);
    // Attaches named binary data (e.g. SPIR-V) to an object, retrievable by the UI.
    void AddBlob(HandleType type, uint64_t handle, const std::string& name, std::shared_ptr<std::vector<uint8_t>> blob);
    std::shared_ptr<std::vector<uint8_t>> GetBlob(uint64_t id, uint32_t index);
    // The creation stack of an object (empty when none was captured or the object is unknown).
    StackTrace GetStack(uint64_t id);
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> GetBlobs(HandleType type, uint64_t handle);

    // Copies of tracked state for other layer components.
    bool Find(HandleType type, uint64_t handle, TrackedObject& out);
    bool FindById(uint64_t id, TrackedObject& out);

    // Sends AddObject for every live object to the transport, then marks the connection live so
    // subsequent events are streamed. Called by the transport on connect.
    void SendSnapshot();
    void OnDisconnect();

private:
    Tracker() = default;

    void RemoveLocked(uint64_t id, std::vector<uint64_t>& removed);
    std::string AddObjectMessage(const TrackedObject& o) const;

    mutable std::shared_mutex _mutex;
    uint64_t _nextId = 1;
    std::unordered_map<uint64_t, TrackedObject> _byId;
    std::unordered_map<uint64_t, uint64_t> _byHandle[HT_Count];
    bool _live = false;
};

} // namespace vkinsp
