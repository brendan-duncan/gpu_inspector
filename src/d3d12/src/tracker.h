// Live object tracker: every D3D12 and DXGI object the application creates, with the arguments of
// the call that created it (its "descriptor"), its parent and its name.
//
// The counterpart of src/vulkan/src/tracker.h and src/metal/src/tracker.h, emitting the same messages —
// AddObject, DeleteObjects, ObjectSetLabel, ObjectUpdate, ObjectBlobs — because the UI's object
// database does not care which API the objects came from. What identifies an object here is its
// COM interface pointer: the side table is keyed by it, and the "type" is the base interface name
// the application sees (ID3D12Resource, ID3D12PipelineState), whatever version it was created as.
//
// Lifetime comes from the Release hook on every tracked vtable: a count reaching zero drops the
// entry and streams DeleteObjects, which is what makes the pointer key safe when the allocator
// hands the address out again. Objects are never AddRef'd by the library, so it changes nothing
// about what stays resident. Ids are stable and never reused.
#pragma once

#include "common.h"
#include "stacktrace.h"

#include <memory>
#include <string>
#include <vector>

namespace dxinsp
{

struct TrackedObject
{
    uint64_t id = 0;
    std::string type;        // "ID3D12Resource"
    uint64_t handle = 0;     // the interface pointer
    uint64_t parentId = 0;
    std::string cmd;         // "CreateCommittedResource"
    uint32_t index = 0;
    std::string args;        // JSON: the creating call's arguments
    std::string label;
    std::vector<std::pair<std::string, std::string>> updates;   // key -> ObjectUpdate message JSON
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> blobs;
    StackTrace stack;
};

class Tracker : public vkinsp::HandleResolver
{
public:
    static Tracker& Get();

    /** HandleResolver: the id of the object at `handle` (the type is ignored: one namespace of pointers). */
    uint64_t Resolve(int handleType, uint64_t handle) override;

    /**
     * Registers an object and streams AddObject. `type` is the base interface name, `cmd` the
     * creating call, `parent` the owner's interface pointer (nullptr for none), `args` the
     * serialized arguments (or empty). An object already tracked keeps its id and record (a
     * swap chain buffer retrieved twice). Returns the id; 0 for a null object or a call of the
     * library's own (Internal()).
     */
    uint64_t Track(void* object, const char* type, const char* cmd, void* parent, std::string args, uint32_t index = 0);

    /** Drops the object (its Release reached zero, or its owner discarded it) and streams DeleteObjects. */
    void Untrack(void* object);
    /** Drops every object whose parent is `parent`, then `parent` itself. */
    void UntrackWithChildren(void* parent);

    uint64_t IdOf(const void* object);
    /** The tracked object's interface pointer, or nullptr when the id is unknown or the object is gone. */
    void* ObjectOf(uint64_t id);
    bool Find(const void* object, TrackedObject& out);
    bool FindById(uint64_t id, TrackedObject& out);
    /** The type of a tracked object, or "" (cheaper than Find for a hook deciding what it was handed). */
    std::string TypeOf(const void* object);

    /** ObjectSetLabel, when the label changed. */
    void SetLabel(void* object, const std::string& label);
    /** A named update (`key` replaces an earlier one with the same key), streamed and replayed in snapshots. */
    void Update(void* object, const std::string& key, const std::string& messageJson);
    void UpdateById(uint64_t id, const std::string& key, const std::string& messageJson);
    /** Attaches named bytes (shader bytecode), announced with ObjectBlobs and served by RequestBlob. */
    void AddBlob(void* object, const std::string& name, std::shared_ptr<std::vector<uint8_t>> blob);
    std::shared_ptr<std::vector<uint8_t>> GetBlob(uint64_t id, uint32_t index);
    StackTrace GetStack(uint64_t id);

    /** Every live object as AddObject (plus its updates), then live streaming. Called by the transport on connect. */
    void SendSnapshot();
    void OnDisconnect();
    /** The Vulkan layer's LeakReport for what is still alive under `owner` (a device about to go). */
    void SendLeakReport(void* owner);

    /** An id from the same sequence, for something the capture names but never announces. */
    uint64_t AllocateId();

private:
    Tracker() = default;
    struct Impl;
    Impl* _impl;
    Impl& impl();
};

/** The tracked id of an object as "{"__id":N,"__class":"T"}", or null when unknown. */
void WriteRef(JsonWriter& w, const void* object, const char* type);

}  // namespace dxinsp
