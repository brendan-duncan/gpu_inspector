#include "raytracing.h"

#include "capture.h"
#include "command_recorder.h"
#include "d3d12_enums.gen.h"
#include "d3d12_vtables.gen.h"
#include "descriptors.h"
#include "hook.h"
#include "hooks.h"
#include "json.h"
#include "serialize.h"
#include "shader_reflect.h"
#include "tracker.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxinsp {
namespace {

inline CaptureManager& Cap() { return CaptureManager::Get(); }

/** The object seen as T, or null. Holds no reference, like the rest of the library (hooks_device.cpp). */
template <typename T>
T* QueryAs(IUnknown* object) {
    if (!object) return nullptr;
    ScopedInternal internal;
    T* p = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&p))) || !p) return nullptr;
    p->Release();
    return p;
}

constexpr size_t kIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;   // 32, fixed by the specification
/** What GetShaderStackSize answers for an export that has no stack size of its own. */
constexpr uint64_t kUnknownStackSize = 0xffffffffull;
using Identifier = std::array<uint8_t, kIdentifierSize>;

std::string HexBytes(const uint8_t* bytes, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0xf];
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// What a state object exports
//
// One entry per export the runtime gave an identifier for. An export that is not a shader — a
// plain function in a library, an unused hit group component — has none, and is left out: the
// point of the list is to say what a binding table record could be holding.

struct Export {
    std::string name;
    Identifier identifier{};
    uint64_t stackSize = 0;
};

struct StateObjectInfo {
    std::vector<Export> exports;
    uint64_t pipelineStackSize = 0;
    /** A library the description exported wholesale: its exports cannot be listed, only learned as the application asks. */
    bool hasUnlistedExports = false;
    /**
     * The hit group exports. GetShaderStackSize answers for a *shader*, and a hit group is a name
     * over several of them, so asking one for its stack raises a validation error in the
     * application's process -- which then reads as the application's own. These are never asked.
     */
    std::unordered_set<std::string> hitGroups;
};

std::mutex g_stateObjectMutex;
std::unordered_map<uint64_t, StateObjectInfo> g_stateObjects;   // state object pointer -> what it exports
/** The state object a properties interface belongs to, so a GetShaderIdentifier need not QueryInterface every call. */
std::unordered_map<uint64_t, ID3D12StateObject*> g_propertiesOwner;

/** Streams the state object's exports as an ObjectUpdate. Called with the lock held. */
void EmitExports(ID3D12StateObject* stateObject, const StateObjectInfo& info) {
    Tracker& t = Tracker::Get();
    const uint64_t id = t.IdOf(stateObject);
    if (!id) return;
    JsonWriter w(&t);
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(id);
    w.Key("shaderIdentifiers"); w.BeginObject();
    w.Key("size"); w.Uint(kIdentifierSize);
    w.Key("pipelineStackSize"); w.Uint(info.pipelineStackSize);
    w.Key("unlistedExports"); w.Boolean(info.hasUnlistedExports);
    w.Key("exports"); w.BeginArray();
    for (const Export& e : info.exports) {
        w.BeginObject();
        w.Key("name"); w.String(e.name);
        w.Key("identifier"); w.String(HexBytes(e.identifier.data(), e.identifier.size()));
        // 0xffffffff is the runtime's "ask the components instead", which a hit group always
        // answers: its any hit, closest hit and intersection each have a stack of their own.
        w.Key("stackSize");
        if (e.stackSize == kUnknownStackSize) w.Null(); else w.Uint(e.stackSize);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndObject();
    t.Update(stateObject, "shaderIdentifiers", w.str());
}

/** Adds or replaces an export, and streams the list again. */
void RecordExport(ID3D12StateObject* stateObject, const std::string& name, const void* identifier, uint64_t stackSize) {
    if (!stateObject || !identifier || name.empty()) return;
    std::lock_guard<std::mutex> lock(g_stateObjectMutex);
    StateObjectInfo& info = g_stateObjects[Key(stateObject)];
    Identifier bytes{};
    memcpy(bytes.data(), identifier, kIdentifierSize);
    for (Export& e : info.exports) {
        if (e.name != name) continue;
        if (e.identifier == bytes && e.stackSize == stackSize) return;   // nothing new to say
        e.identifier = bytes;
        e.stackSize = stackSize;
        EmitExports(stateObject, info);
        return;
    }
    info.exports.push_back({name, bytes, stackSize});
    EmitExports(stateObject, info);
}

// ---------------------------------------------------------------------------------------------
// The properties interface
//
// The application asks the runtime for an export's identifier through an interface it
// QueryInterfaces off the state object, and those 32 bytes are what it writes into its binding
// table. Hooking the call is the only way to see the identifiers of a library the description
// exported wholesale (NumExports 0, every function in the container exported under its own name):
// nothing but the container itself lists those, and an application that asks for one is telling
// the library exactly which export the bytes in its table mean.

ID3D12StateObject* OwnerOf(ID3D12StateObjectProperties1* properties) {
    {
        std::lock_guard<std::mutex> lock(g_stateObjectMutex);
        auto it = g_propertiesOwner.find(Key(properties));
        if (it != g_propertiesOwner.end()) return it->second;
    }
    ID3D12StateObject* stateObject = QueryAs<ID3D12StateObject>(properties);
    if (!stateObject) return nullptr;
    std::lock_guard<std::mutex> lock(g_stateObjectMutex);
    g_propertiesOwner[Key(properties)] = stateObject;
    return stateObject;
}

void* STDMETHODCALLTYPE Hook_GetShaderIdentifier(ID3D12StateObjectProperties1* This, LPCWSTR pExportName) {
    auto orig = Orig<PFN_ID3D12StateObjectProperties1_GetShaderIdentifier>(
        This, slot::ID3D12StateObjectProperties1_GetShaderIdentifier);
    void* identifier = orig(This, pExportName);
    // The library asks for identifiers of its own in NoteStateObject and records them there.
    if (Internal() || !identifier || !pExportName) return identifier;
    ID3D12StateObject* stateObject = OwnerOf(This);
    if (!stateObject) return identifier;
    const std::string name = Narrow(pExportName);
    bool hitGroup = true;
    {
        std::lock_guard<std::mutex> lock(g_stateObjectMutex);
        auto it = g_stateObjects.find(Key(stateObject));
        hitGroup = it == g_stateObjects.end() || it->second.hitGroups.count(name) != 0;
    }
    uint64_t stackSize = kUnknownStackSize;
    if (!hitGroup) {
        ScopedInternal internal;
        auto stack = Orig<PFN_ID3D12StateObjectProperties1_GetShaderStackSize>(
            This, slot::ID3D12StateObjectProperties1_GetShaderStackSize);
        if (stack) stackSize = stack(This, pExportName);
    }
    RecordExport(stateObject, name, identifier, stackSize);
    return identifier;
}

/**
 * The properties interface of a state object, with its vtable patched so later identifier requests
 * are seen. Holds no reference, like every other object the library hooks.
 *
 * The vtable is shared by every state object of the runtime's class, so it is patched once; the
 * slot count follows whether the runtime has ID3D12StateObjectProperties1, whose GetProgramIdentifier
 * is the one slot past the older interface's vtable.
 */
ID3D12StateObjectProperties1* PropertiesOf(ID3D12StateObject* stateObject) {
    if (!stateObject) return nullptr;
    auto* properties = QueryAs<ID3D12StateObjectProperties1>(stateObject);
    uint32_t count = slot::ID3D12StateObjectProperties1_Count;
    if (!properties) {
        // A runtime without the work graph interface: the same vtable, one method shorter.
        properties = reinterpret_cast<ID3D12StateObjectProperties1*>(QueryAs<ID3D12StateObjectProperties>(stateObject));
        count = slot::ID3D12StateObjectProperties1_GetProgramIdentifier;
    }
    if (!properties) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_stateObjectMutex);
        g_propertiesOwner[Key(properties)] = stateObject;
    }
    // Not HookD3D12Object: this interface shares the state object's reference count, so a Release
    // hook here would untrack the state object early, and it has no SetName of its own.
    HookVtable(properties, "ID3D12StateObjectProperties", count,
               {{slot::ID3D12StateObjectProperties1_GetShaderIdentifier, (void*)&Hook_GetShaderIdentifier}});
    return properties;
}

// ---------------------------------------------------------------------------------------------
// Reading a description's exports
//
// An export's name is what GetShaderIdentifier takes, and the description says all of them except
// for a DXIL library with NumExports 0, which exports every function in its container. Those are
// left to the hook above; `unlisted` records that there are some, so the UI can say why a table
// record may match nothing.

void CollectExports(const D3D12_STATE_OBJECT_DESC& desc, std::vector<std::wstring>& names, bool& unlisted,
                    std::unordered_set<std::string>& hitGroups) {
    auto add = [&](LPCWSTR name) {
        if (name && *name) names.emplace_back(name);
    };
    for (UINT i = 0; i < desc.NumSubobjects && desc.pSubobjects; ++i) {
        const D3D12_STATE_SUBOBJECT& s = desc.pSubobjects[i];
        if (!s.pDesc) continue;
        switch (s.Type) {
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
                const auto& lib = *static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s.pDesc);
                if (!lib.NumExports || !lib.pExports) { unlisted = true; break; }
                for (UINT e = 0; e < lib.NumExports; ++e) add(lib.pExports[e].Name);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
                const auto& col = *static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc);
                if (!col.NumExports || !col.pExports) { unlisted = true; break; }
                for (UINT e = 0; e < col.NumExports; ++e) add(col.pExports[e].Name);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
                const auto& group = *static_cast<const D3D12_HIT_GROUP_DESC*>(s.pDesc);
                // Only the hit group's own export can be put in a table; the shaders it names are
                // reached through it, and the runtime gives them no identifier of their own.
                add(group.HitGroupExport);
                if (group.HitGroupExport && *group.HitGroupExport) hitGroups.insert(Narrow(group.HitGroupExport));
                break;
            }
            default:
                break;
        }
    }
}

/** The DXIL libraries of a description, as blobs on the state object: the only copy of a DXR shader's code. */
void AddLibraryBlobs(ID3D12StateObject* stateObject, const D3D12_STATE_OBJECT_DESC& desc, JsonWriter& shaders) {
    uint32_t index = 0;
    for (UINT i = 0; i < desc.NumSubobjects && desc.pSubobjects; ++i) {
        const D3D12_STATE_SUBOBJECT& s = desc.pSubobjects[i];
        if (s.Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !s.pDesc) continue;
        const auto& lib = *static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s.pDesc);
        const auto* bytes = static_cast<const uint8_t*>(lib.DXILLibrary.pShaderBytecode);
        const size_t size = lib.DXILLibrary.BytecodeLength;
        if (!bytes || !size) continue;
        const std::string name = "library:" + std::to_string(index++);
        ShaderInfo info;
        {
            // Reflection loads dxcompiler and is the library's own work.
            ScopedInternal internal;
            info = ReflectShader(bytes, size);
        }
        shaders.Key(name.c_str());
        shaders.BeginObject();
        shaders.Key("subobject"); shaders.Uint(i);
        shaders.Key("target"); shaders.String(info.target);
        shaders.Key("hash"); shaders.String(Hex(ShaderHash(bytes, size)));
        shaders.Key("size"); shaders.Uint(size);
        shaders.EndObject();
        Tracker::Get().AddBlob(stateObject, name, std::make_shared<std::vector<uint8_t>>(bytes, bytes + size));
    }
}

// ---------------------------------------------------------------------------------------------
// Acceleration structures
//
// Nothing in D3D12 is an acceleration structure. A build writes one into a UAV buffer at a GPU
// virtual address, and from then on every reference to it — a top level instance naming a bottom
// level, a shader's SRV, a copy, a trace — is that address and nothing else. So the library mints
// the object the UI needs: one tracked ID3D12RaytracingAccelerationStructure per destination
// address, keyed by a sentinel of its own so it can never collide with an interface pointer.

/**
 * One input a build reads: which field of which geometry, and the GPU range it reads. A top level's
 * instances have no geometry index, and are written without one.
 */
struct InputRange {
    const char* field = "";
    int geometry = -1;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    UINT64 size = 0;
};

/** Every range a build reads, sized the way the build reads them. */
std::vector<InputRange> InputRangesOf(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in) {
    std::vector<InputRange> out;
    if (in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        // An array of pointers is a list of addresses rather than of instances; what they point at
        // is not followed, so only the pointers themselves are read.
        const UINT64 stride = in.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS
                            ? sizeof(D3D12_GPU_VIRTUAL_ADDRESS) : sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        if (in.InstanceDescs && in.NumDescs) out.push_back({"InstanceDescs", -1, in.InstanceDescs, (UINT64)in.NumDescs * stride});
        return out;
    }
    if (in.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) return out;
    for (UINT i = 0; i < in.NumDescs; ++i) {
        const D3D12_RAYTRACING_GEOMETRY_DESC* g = in.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS
                                                ? (in.ppGeometryDescs ? in.ppGeometryDescs[i] : nullptr)
                                                : (in.pGeometryDescs ? &in.pGeometryDescs[i] : nullptr);
        if (!g) continue;
        if (g->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
            const auto& tri = g->Triangles;
            if (tri.VertexBuffer.StrideInBytes && tri.VertexCount) {
                out.push_back({"VertexBuffer", (int)i, tri.VertexBuffer.StartAddress, (UINT64)tri.VertexCount * tri.VertexBuffer.StrideInBytes});
            }
            if (tri.IndexBuffer && tri.IndexCount && tri.IndexFormat != DXGI_FORMAT_UNKNOWN) {
                const UINT64 indexSize = tri.IndexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
                out.push_back({"IndexBuffer", (int)i, tri.IndexBuffer, (UINT64)tri.IndexCount * indexSize});
            }
            // A 3x4 row-major float matrix applied to the geometry before it is built in.
            if (tri.Transform3x4) out.push_back({"Transform3x4", (int)i, tri.Transform3x4, 48});
        } else if (g->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
            const auto& aabbs = g->AABBs;
            if (aabbs.AABBs.StrideInBytes && aabbs.AABBCount) {
                out.push_back({"AABBs", (int)i, aabbs.AABBs.StartAddress, (UINT64)aabbs.AABBCount * aabbs.AABBs.StrideInBytes});
            }
        }
    }
    return out;
}

/** One input's entry in `buildData` or `captureInputs`: where it resolved, and its read-back when there is one. */
void AppendInput(std::string& out, uint32_t& count, const InputRange& r, uint64_t buffer, UINT64 offset, uint32_t capture) {
    if (!buffer) return;
    out += count++ ? "," : "";
    out += "{";
    if (r.geometry >= 0) out += "\"geometry\":" + std::to_string(r.geometry) + ",";
    out += std::string("\"field\":\"") + r.field + "\",\"buffer\":" + std::to_string(buffer) + ",\"offset\":" + std::to_string(offset);
    if (capture) out += ",\"capture\":" + std::to_string(capture);
    out += "}";
}

struct StructureObject {
    /** The tracker's key: this object's own address, which no COM object can also have. */
    std::unique_ptr<uint8_t> sentinel = std::make_unique<uint8_t>(0);
    uint64_t id = 0;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    /** The buffer the build wrote it into, so a read-back of that buffer can be refused. */
    ID3D12Resource* buffer = nullptr;
    /**
     * What its last build read, so a capture that begins after the build can read the same ranges
     * back (ReadBackEarlierStructures). An engine builds its bottom levels once, at load, and without
     * these a capture of any later frame knows what a structure is but not what is in it.
     */
    std::vector<InputRange> inputs;
};

std::mutex g_structureMutex;
std::unordered_map<uint64_t, std::unique_ptr<StructureObject>> g_structures;   // address -> object
/** The buffers those structures live in, which nothing may read back or transition. */
std::unordered_set<uint64_t> g_structureBuffers;

/**
 * A structure's name from its buffer's: the buffer's own when the structure starts it, and with the
 * offset when it does not — an engine that packs many structures into one buffer names the buffer
 * once, and "Scene AS +0x4000" still tells them apart.
 */
std::string StructureLabel(const std::string& bufferName, D3D12_GPU_VIRTUAL_ADDRESS address, ID3D12Resource* buffer) {
    ID3D12Resource* owner = nullptr;
    UINT64 offset = 0, remaining = 0;
    if (!AddressMap::Get().Resolve(address, owner, offset, remaining) || owner != buffer || offset == 0) return bufferName;
    return bufferName + " +" + Hex(offset);
}

StructureObject* StructureObjectAt(D3D12_GPU_VIRTUAL_ADDRESS address, ID3D12Device* device, bool create) {
    if (!address) return nullptr;
    std::lock_guard<std::mutex> lock(g_structureMutex);
    auto it = g_structures.find(address);
    if (it != g_structures.end()) return it->second.get();
    if (!create) return nullptr;
    auto object = std::make_unique<StructureObject>();
    object->address = address;
    {
        UINT64 offset = 0, remaining = 0;
        if (AddressMap::Get().Resolve(address, object->buffer, offset, remaining) && object->buffer) {
            g_structureBuffers.insert(Key(object->buffer));
        }
    }
    Args a;
    a.address("Address", address);
    // The structure exists because a build wrote it; the creating "call" is that build, which is
    // also the only thing that could have made it.
    object->id = Tracker::Get().Track(object->sentinel.get(), "ID3D12RaytracingAccelerationStructure",
                                      "BuildRaytracingAccelerationStructure", device, a.str());
    if (!object->id) return nullptr;
    // Named after its buffer, which is the application's only name for it (OnResourceNamed).
    if (object->buffer) {
        TrackedObject owner;
        if (Tracker::Get().Find(object->buffer, owner) && !owner.label.empty()) {
            Tracker::Get().SetLabel(object->sentinel.get(), StructureLabel(owner.label, object->address, object->buffer));
        }
    }
    StructureObject* out = object.get();
    g_structures[address] = std::move(object);
    return out;
}

/**
 * One input address of a build: where it points, and the contents if they could be read back.
 *
 * An address means nothing on its own — it is not a handle, and it is meaningless outside the
 * process that made it. Resolving it to the buffer that owns it is what lets the capture read the
 * vertices, indices and instances a structure was built from.
 */
struct ResolvedInput {
    uint64_t buffer = 0;   // the buffer's object id, 0 when the address resolved to none
    UINT64 offset = 0;
    uint32_t capture = 0;  // the contents read back, 0 when they were not
};

ResolvedInput CaptureInput(CommandRecorder* rec, D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 size) {
    ResolvedInput out;
    if (!address || !size) return out;
    ID3D12Resource* buffer = nullptr;
    UINT64 offset = 0, remaining = 0;
    if (!AddressMap::Get().Resolve(address, buffer, offset, remaining)) return out;
    out.buffer = Tracker::Get().IdOf(buffer);
    out.offset = offset;
    // A size the build implies can run past the buffer when the application over-declared it.
    if (size > remaining) size = remaining;
    if (rec && size) out.capture = Cap().QueueAddressCapture(rec, address, size);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

void NoteStateObject(ID3D12StateObject* stateObject, const D3D12_STATE_OBJECT_DESC* desc,
                     ID3D12StateObject* grownFrom) {
    if (!stateObject) return;
    StateObjectInfo info;
    if (grownFrom) {
        // AddToStateObject keeps everything the original exported, under the same identifiers.
        std::lock_guard<std::mutex> lock(g_stateObjectMutex);
        auto it = g_stateObjects.find(Key(grownFrom));
        if (it != g_stateObjects.end()) info = it->second;
    }

    std::vector<std::wstring> names;
    if (desc) CollectExports(*desc, names, info.hasUnlistedExports, info.hitGroups);
    // What the hook needs to know before the application asks about any of them.
    {
        std::lock_guard<std::mutex> lock(g_stateObjectMutex);
        g_stateObjects[Key(stateObject)] = info;
    }

    ID3D12StateObjectProperties1* properties = PropertiesOf(stateObject);
    if (properties) {
        ScopedInternal internal;
        auto identifierOf = Orig<PFN_ID3D12StateObjectProperties1_GetShaderIdentifier>(
            properties, slot::ID3D12StateObjectProperties1_GetShaderIdentifier);
        auto stackOf = Orig<PFN_ID3D12StateObjectProperties1_GetShaderStackSize>(
            properties, slot::ID3D12StateObjectProperties1_GetShaderStackSize);
        auto pipelineStack = Orig<PFN_ID3D12StateObjectProperties1_GetPipelineStackSize>(
            properties, slot::ID3D12StateObjectProperties1_GetPipelineStackSize);
        if (pipelineStack) info.pipelineStackSize = pipelineStack(properties);
        for (const std::wstring& name : names) {
            // Null for an export that is not a shader, and for every export of a collection: only a
            // raytracing pipeline hands identifiers out.
            void* identifier = identifierOf ? identifierOf(properties, name.c_str()) : nullptr;
            if (!identifier) continue;
            Identifier bytes{};
            memcpy(bytes.data(), identifier, kIdentifierSize);
            const std::string narrow = Narrow(name.c_str());
            auto existing = std::find_if(info.exports.begin(), info.exports.end(),
                                         [&](const Export& e) { return e.name == narrow; });
            // Never for a hit group: the runtime rejects that, loudly, in the application's log.
            const uint64_t stackSize = stackOf && !info.hitGroups.count(narrow) ? stackOf(properties, name.c_str())
                                                                               : kUnknownStackSize;
            if (existing != info.exports.end()) {
                existing->identifier = bytes;
                existing->stackSize = stackSize;
            } else {
                info.exports.push_back({narrow, bytes, stackSize});
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_stateObjectMutex);
        g_stateObjects[Key(stateObject)] = info;
        EmitExports(stateObject, info);
    }

    if (desc) {
        JsonWriter shaders(&Tracker::Get());
        shaders.BeginObject();
        AddLibraryBlobs(stateObject, *desc, shaders);
        shaders.EndObject();
        JsonWriter w(&Tracker::Get());
        w.BeginObject();
        w.Key("action"); w.String("ObjectUpdate");
        w.Key("id"); w.Uint(Tracker::Get().IdOf(stateObject));
        w.Key("shaders"); w.Raw(shaders.str());
        w.EndObject();
        Tracker::Get().Update(stateObject, "shaders", w.str());
    }
}

std::string ExportWithIdentifier(ID3D12StateObject* stateObject, const void* identifier) {
    if (!stateObject || !identifier) return "";
    std::lock_guard<std::mutex> lock(g_stateObjectMutex);
    auto it = g_stateObjects.find(Key(stateObject));
    if (it == g_stateObjects.end()) return "";
    for (const Export& e : it->second.exports) {
        if (memcmp(e.identifier.data(), identifier, kIdentifierSize) == 0) return e.name;
    }
    return "";
}

uint64_t NoteStructureAt(D3D12_GPU_VIRTUAL_ADDRESS address, ID3D12Device* device) {
    StructureObject* object = StructureObjectAt(address, device, true);
    return object ? object->id : 0;
}

uint64_t StructureAt(D3D12_GPU_VIRTUAL_ADDRESS address) {
    StructureObject* object = StructureObjectAt(address, nullptr, false);
    return object ? object->id : 0;
}

void WriteStructureRef(JsonWriter& w, D3D12_GPU_VIRTUAL_ADDRESS address) {
    const uint64_t id = StructureAt(address);
    if (!id) { w.Null(); return; }
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"__id\":%llu,\"__class\":\"ID3D12RaytracingAccelerationStructure\"}",
             (unsigned long long)id);
    w.Raw(buf);
}

std::string NoteAccelerationStructureBuild(CommandRecorder* rec,
                                           const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& desc,
                                           ID3D12Device* device) {
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in = desc.Inputs;
    if (!device && rec) device = rec->device();
    StructureObject* structure = StructureObjectAt(desc.DestAccelerationStructureData, device, true);
    if (!structure) return "";

    std::string inputs;
    uint32_t captured = 0;
    uint64_t primitives = in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL ? in.NumDescs : 0;
    if (in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
        for (UINT i = 0; i < in.NumDescs; ++i) {
            const D3D12_RAYTRACING_GEOMETRY_DESC* g = in.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS
                                                    ? (in.ppGeometryDescs ? in.ppGeometryDescs[i] : nullptr)
                                                    : (in.pGeometryDescs ? &in.pGeometryDescs[i] : nullptr);
            if (!g) continue;
            primitives += g->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES
                        ? (g->Triangles.IndexCount ? g->Triangles.IndexCount / 3 : g->Triangles.VertexCount / 3)
                        : g->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS ? g->AABBs.AABBCount : 0;
        }
    }
    std::vector<InputRange> ranges = InputRangesOf(in);
    for (const InputRange& r : ranges) {
        const ResolvedInput resolved = CaptureInput(rec, r.address, r.size);
        AppendInput(inputs, captured, r, resolved.buffer, resolved.offset, resolved.capture);
    }
    {
        std::lock_guard<std::mutex> lock(g_structureMutex);
        structure->inputs = std::move(ranges);
    }

    // The build on the structure, so selecting it in the Inspect panel says what it holds. This is
    // last-write-wins: an application that rebuilds every frame leaves the newest build here, and
    // the capture ids of the captured frame's build go on the command below.
    {
        Tracker& t = Tracker::Get();
        JsonWriter w(&t);
        w.BeginObject();
        w.Key("action"); w.String("ObjectUpdate");
        w.Key("id"); w.Uint(structure->id);
        w.Key("build"); w.BeginObject();
        w.Key("method"); w.String("BuildRaytracingAccelerationStructure");
        w.Key("Type"); w.Enum(ToString_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE(EnumValue(in.Type)), EnumValue(in.Type));
        w.Key("Flags"); Flags_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(w, (uint64_t)in.Flags);
        w.Key("NumDescs"); w.Uint(in.NumDescs);
        w.Key("primitiveCount"); w.Uint(primitives);
        w.Key("DestAccelerationStructureData"); WriteGpuAddress(w, desc.DestAccelerationStructureData);
        w.Key("SourceAccelerationStructureData"); WriteGpuAddress(w, desc.SourceAccelerationStructureData);
        w.Key("update"); w.Boolean((in.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0);
        w.Key("geometries"); w.BeginArray();
        if (in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
            for (UINT i = 0; i < in.NumDescs; ++i) {
                const D3D12_RAYTRACING_GEOMETRY_DESC* g = in.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS
                                                        ? (in.ppGeometryDescs ? in.ppGeometryDescs[i] : nullptr)
                                                        : (in.pGeometryDescs ? &in.pGeometryDescs[i] : nullptr);
                if (g) Write(w, *g); else w.Null();
            }
        }
        w.EndArray();
        // What the driver says the build needs, which is what the structure costs in memory: the
        // application's buffer only has to be at least this big.
        ID3D12Device5* device5 = nullptr;
        if (device && SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device5), (void**)&device5)) && device5) {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &prebuild);
            device5->Release();
            if (prebuild.ResultDataMaxSizeInBytes) {
                w.Key("resultSize"); w.Uint(prebuild.ResultDataMaxSizeInBytes);
                w.Key("scratchSize"); w.Uint(prebuild.ScratchDataSizeInBytes);
            }
        }
        w.EndObject();
        w.EndObject();
        t.UpdateById(structure->id, "build", w.str());
    }

    std::string extra = ",\"destStructure\":" + std::to_string(structure->id);
    if (captured) extra += ",\"buildData\":[" + inputs + "]";
    return extra;
}

void NoteAccelerationStructureCopy(D3D12_GPU_VIRTUAL_ADDRESS dest, D3D12_GPU_VIRTUAL_ADDRESS source,
                                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode, ID3D12Device* device) {
    StructureObject* to = StructureObjectAt(dest, device, true);
    if (!to) return;
    Tracker& t = Tracker::Get();
    JsonWriter w(&t);
    w.BeginObject();
    w.Key("action"); w.String("ObjectUpdate");
    w.Key("id"); w.Uint(to->id);
    w.Key("copiedFrom"); w.BeginObject();
    w.Key("source"); WriteStructureRef(w, source);
    w.Key("sourceAddress"); w.String(Hex(source));
    w.Key("Mode"); w.Enum(ToString_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE(EnumValue(mode)), EnumValue(mode));
    w.EndObject();
    w.EndObject();
    t.UpdateById(to->id, "copiedFrom", w.str());
}

std::string NoteDispatchRays(CommandRecorder* rec, const D3D12_DISPATCH_RAYS_DESC& desc) {
    if (!rec) return "";
    struct Region {
        const char* name;
        D3D12_GPU_VIRTUAL_ADDRESS address;
        UINT64 size;
        UINT64 stride;
    };
    const Region regions[] = {
        {"RayGeneration", desc.RayGenerationShaderRecord.StartAddress, desc.RayGenerationShaderRecord.SizeInBytes,
         desc.RayGenerationShaderRecord.SizeInBytes},
        {"Miss", desc.MissShaderTable.StartAddress, desc.MissShaderTable.SizeInBytes, desc.MissShaderTable.StrideInBytes},
        {"HitGroup", desc.HitGroupTable.StartAddress, desc.HitGroupTable.SizeInBytes, desc.HitGroupTable.StrideInBytes},
        {"Callable", desc.CallableShaderTable.StartAddress, desc.CallableShaderTable.SizeInBytes,
         desc.CallableShaderTable.StrideInBytes},
    };
    std::string extra;
    uint32_t captured = 0;
    for (const Region& r : regions) {
        if (!r.address || !r.size) continue;
        const uint32_t id = Cap().QueueAddressCapture(rec, r.address, r.size);
        if (!id) continue;
        extra += captured++ ? "," : "";
        extra += std::string("{\"region\":\"") + r.name + "\",\"capture\":" + std::to_string(id)
               + ",\"stride\":" + std::to_string(r.stride) + "}";
    }
    // Which state object's identifiers the table holds: without it a record's 32 bytes match nothing.
    std::string out;
    if (ID3D12StateObject* stateObject = BoundStateObject(rec)) {
        const uint64_t id = Tracker::Get().IdOf(stateObject);
        if (id) out += ",\"stateObject\":{\"__id\":" + std::to_string(id) + ",\"__class\":\"ID3D12StateObject\"}";
    }
    if (captured) out += ",\"bindingTableData\":[" + extra + "]";
    return out;
}

void NoteBoundStateObject(CommandRecorder* rec, ID3D12StateObject* stateObject) {
    if (rec) rec->state().stateObject = stateObject;
}

ID3D12StateObject* BoundStateObject(CommandRecorder* rec) {
    return rec ? rec->state().stateObject : nullptr;
}

void ForgetStructuresIn(ID3D12Resource* buffer) {
    if (!buffer) return;
    std::lock_guard<std::mutex> lock(g_structureMutex);
    for (auto it = g_structures.begin(); it != g_structures.end();) {
        ID3D12Resource* owner = nullptr;
        UINT64 offset = 0, remaining = 0;
        if (AddressMap::Get().Resolve(it->second->address, owner, offset, remaining) && owner == buffer) {
            Tracker::Get().Untrack(it->second->sentinel.get());
            it = g_structures.erase(it);
        } else {
            ++it;
        }
    }
    g_structureBuffers.erase(Key(buffer));
}

void OnResourceNamed(ID3D12Resource* resource, const std::string& name) {
    if (!resource || name.empty()) return;
    std::lock_guard<std::mutex> lock(g_structureMutex);
    if (!g_structureBuffers.count(Key(resource))) return;
    for (auto& [address, object] : g_structures) {
        if (object->buffer == resource) Tracker::Get().SetLabel(object->sentinel.get(), StructureLabel(name, address, resource));
    }
}

void ReadBackEarlierStructures(CommandRecorder* rec, uint64_t captureSerial) {
    static std::atomic<uint64_t> done{0};
    if (!rec || !captureSerial) return;
    uint64_t expected = done.load();
    // Once per capture, whichever thread submits first.
    do {
        if (expected == captureSerial) return;
    } while (!done.compare_exchange_weak(expected, captureSerial));

    // Copied out under the lock, since the read-backs take the capture's own locks.
    struct Pending {
        uint64_t id;
        std::vector<InputRange> inputs;
    };
    std::vector<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(g_structureMutex);
        for (const auto& [address, object] : g_structures) {
            if (!object->inputs.empty()) pending.push_back({object->id, object->inputs});
        }
    }
    Tracker& t = Tracker::Get();
    for (const Pending& p : pending) {
        std::string list;
        uint32_t count = 0;
        for (const InputRange& r : p.inputs) {
            ID3D12Resource* buffer = nullptr;
            UINT64 offset = 0, remaining = 0;
            if (!AddressMap::Get().Resolve(r.address, buffer, offset, remaining)) continue;
            const uint32_t capture = Cap().QueueAddressCaptureAfterSubmit(rec, r.address, std::min(r.size, remaining));
            AppendInput(list, count, r, t.IdOf(buffer), offset, capture);
        }
        if (!count) continue;
        // On the structure rather than a command: no command of the capture built it. `serial`
        // tells a capture's ids from an earlier one's, which a later capture overwrites.
        JsonWriter w(&t);
        w.BeginObject();
        w.Key("action"); w.String("ObjectUpdate");
        w.Key("id"); w.Uint(p.id);
        w.Key("captureInputs"); w.BeginObject();
        w.Key("serial"); w.Uint(captureSerial);
        w.Key("inputs"); w.Raw("[" + list + "]");
        w.EndObject();
        w.EndObject();
        t.UpdateById(p.id, "captureInputs", w.str());
    }
}

bool HoldsAccelerationStructure(ID3D12Resource* buffer) {
    if (!buffer) return false;
    std::lock_guard<std::mutex> lock(g_structureMutex);
    return g_structureBuffers.count(Key(buffer)) != 0;
}

void ForgetStateObject(ID3D12StateObject* stateObject) {
    if (!stateObject) return;
    std::lock_guard<std::mutex> lock(g_stateObjectMutex);
    g_stateObjects.erase(Key(stateObject));
    for (auto it = g_propertiesOwner.begin(); it != g_propertiesOwner.end();) {
        it = it->second == stateObject ? g_propertiesOwner.erase(it) : std::next(it);
    }
}

void ResetRaytracing() {
    {
        std::lock_guard<std::mutex> lock(g_stateObjectMutex);
        g_stateObjects.clear();
        g_propertiesOwner.clear();
    }
    std::lock_guard<std::mutex> lock(g_structureMutex);
    g_structures.clear();
    g_structureBuffers.clear();
}

}  // namespace dxinsp
