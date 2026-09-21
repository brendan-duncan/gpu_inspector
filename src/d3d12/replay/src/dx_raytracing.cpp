// DXR in the replay: state objects, acceleration structure builds and traces.
//
// Most of a captured frame replays because every object it names has an id and every buffer range
// an offset. Ray tracing names almost nothing that way, and the three things it does name are the
// three things this file exists to translate:
//
//   Addresses. A build reads its geometry, its instances and its scratch from GPU virtual
//   addresses, and a trace reads its binding table from three more. The capture library resolved
//   each one to the buffer and offset that owned it (src/d3d12/src/raytracing.h), so the ordinary
//   Address() decode already turns them into this machine's — that half is free (dx_reflect.h).
//
//   Addresses *inside* buffers. An instance description holds the address of the bottom level it
//   places, as eight bytes in the middle of a 64-byte record. Nothing resolved those, because the
//   application wrote them into memory rather than passing them to a call. The replay rewrites
//   them: the capture's structure objects each carry the address they were built at, which gives
//   captured address -> buffer and offset -> this machine's address, and the instances go into a
//   buffer of the replay's own.
//
//   Shader identifiers. A binding table record begins with the 32 bytes the captured runtime gave
//   for an export, and this machine's runtime gives different ones for the same state object. So
//   the table is rebuilt the same way: captured identifier -> the export it named (the capture
//   kept that list) -> this runtime's identifier for that name.
//
// What is left out, and why: an opacity micromap array (nothing in the capture describes its
// input layout well enough to rebuild), a build whose source structure was written before the
// capture began (there is nothing to copy from), and local root arguments in a binding table
// record that hold GPU addresses — the record's bytes are copied as they were, and a descriptor
// handle or an address among them will point at the captured process's memory. A record's
// identifier is always fixed; only what follows it is copied blind.
#include "dx_replayer.h"

#include "dx_decode.h"
#include "dx_exporter.h"
#include "dx_reflect.h"
#include "dx_source.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace dxreplay {

using vkreplay::JValue;

namespace {

/** One 64-byte D3D12_RAYTRACING_INSTANCE_DESC, as the offsets this needs to reach into it. */
constexpr size_t kInstanceStride = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
constexpr size_t kInstanceStructureOffset = offsetof(D3D12_RAYTRACING_INSTANCE_DESC, AccelerationStructure);
constexpr size_t kIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

std::string Str(const JValue* v) { return v ? std::string(v->Str()) : std::string(); }

/** An HRESULT as "0x80004005", for a message; the replayer's own copy is not shared. */
std::string HrText(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)hr);
    return buf;
}

/** A narrow name as the wide string the DXR entry points take, kept in the arena. */
LPCWSTR Wide(vkreplay::Arena& arena, const std::string& text) {
    if (text.empty()) return nullptr;
    wchar_t* out = (wchar_t*)arena.Alloc((text.size() + 1) * sizeof(wchar_t), alignof(wchar_t));
    size_t i = 0;
    for (; i < text.size(); ++i) out[i] = (wchar_t)(unsigned char)text[i];
    out[i] = 0;
    return out;
}

/** Lowercase hex of some bytes, the spelling the capture writes an identifier in. */
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

}  // namespace

// ---------------------------------------------------------------------------------------------
// What the capture says about its structures and its state objects
//
// Gathered before the objects are made, because a build's instances and a trace's table both need
// to look back at objects by something other than their id, and because a buffer holding a
// structure has to be created in the one state it may ever be in.

void DxReplayer::PrepareRaytracing() {
    const JValue* objects = _capture->Objects();
    if (!objects || !objects->IsArray()) return;
    for (uint32_t i = 0; i < objects->count; ++i) {
        const JValue& o = objects->items[i];
        if (Str(o.Get("type")) != "ID3D12RaytracingAccelerationStructure") continue;
        const JValue* address = o.Get("args") ? o.Get("args")->Get("Address") : nullptr;
        if (!address || address->IsNull()) continue;
        const JValue* number = address->Get("address");
        if (!number || !number->IsString()) continue;
        // The library writes it as "0x...", which is what an instance holds as a number.
        const uint64_t captured = strtoull(Str(number).c_str(), nullptr, 0);
        if (!captured) continue;
        StructurePlace place;
        place.buffer = IdOf(address->Get("buffer"));
        place.offset = address->Get("offset") ? address->Get("offset")->Uint() : 0;
        if (!place.buffer) continue;
        _structureAddresses[captured] = place;
        _structureBuffers.insert(place.buffer);
    }

    // Which of them the frame builds. One it does not is memory the replay never wrote: the
    // structure the captured process had there was built before the capture began, and a top
    // level over it traces a scene of nothing. That has to be said, because an empty scene
    // replays without a single error and looks like a frame that simply had nothing in it.
    const JValue* commands = _capture->Commands();
    for (uint32_t i = 0; commands && commands->IsArray() && i < commands->count; ++i) {
        const JValue& c = commands->items[i];
        if (Str(c.Get("method")) != "BuildRaytracingAccelerationStructure") continue;
        const JValue* args = c.Get("args");
        const JValue* desc = args ? args->Get("pDesc") : nullptr;
        const JValue* dest = desc ? desc->Get("DestAccelerationStructureData") : nullptr;
        const JValue* number = dest ? dest->Get("address") : nullptr;
        if (number && number->IsString()) _structuresBuiltInFrame.insert(strtoull(Str(number).c_str(), nullptr, 0));
    }
}

D3D12_GPU_VIRTUAL_ADDRESS DxReplayer::RemapStructureAddress(uint64_t captured) const {
    if (!captured) return 0;
    auto it = _structureAddresses.find(captured);
    if (it == _structureAddresses.end()) return 0;
    auto r = _resources.find(it->second.buffer);
    if (r == _resources.end() || !r->second.resource || !r->second.IsBuffer()) return 0;
    return r->second.resource->GetGPUVirtualAddress() + it->second.offset;
}

// ---------------------------------------------------------------------------------------------
// State objects

ID3D12StateObject* DxReplayer::CreateStateObject(uint64_t id, const JValue& object, const JValue& args) {
    if (!RaytracingDevice()) {
        Problem("state object " + std::to_string(id) + ": this GPU has no DXR (D3D12_RAYTRACING_TIER_1_0)");
        return nullptr;
    }
    const JValue* desc = args.Get("pDesc");
    if (!desc || desc->IsNull()) {
        Problem("state object " + std::to_string(id) + ": the capture has no description of it");
        return nullptr;
    }
    const JValue* subobjects = desc->Get("pSubobjects");
    if (!subobjects || !subobjects->IsArray() || !subobjects->count) {
        Problem("state object " + std::to_string(id) + ": its description has no subobjects");
        return nullptr;
    }

    // The subobjects are built into the arena, in the order the capture holds them, because an
    // association names one by its position in the array and the runtime follows that pointer.
    auto* built = (D3D12_STATE_SUBOBJECT*)_arena.Alloc(sizeof(D3D12_STATE_SUBOBJECT) * subobjects->count,
                                                          alignof(D3D12_STATE_SUBOBJECT));
    std::memset(built, 0, sizeof(D3D12_STATE_SUBOBJECT) * subobjects->count);
    uint32_t libraryIndex = 0;
    bool failed = false;
    std::string why;

    for (uint32_t i = 0; i < subobjects->count && !failed; ++i) {
        const JValue& s = subobjects->items[i];
        const std::string type = Str(s.Get("Type"));
        D3D12_STATE_SUBOBJECT& out = built[i];
        auto allocate = [&](size_t size, size_t align) {
            void* p = _arena.Alloc(size, align);
            std::memset(p, 0, size);
            out.pDesc = p;
            return p;
        };
        auto exportsOf = [&](const JValue* list, UINT& count) -> const D3D12_EXPORT_DESC* {
            count = 0;
            if (!list || !list->IsArray() || !list->count) return nullptr;
            auto* out = (D3D12_EXPORT_DESC*)_arena.Alloc(sizeof(D3D12_EXPORT_DESC) * list->count, alignof(D3D12_EXPORT_DESC));
            std::memset(out, 0, sizeof(D3D12_EXPORT_DESC) * list->count);
            for (uint32_t k = 0; k < list->count; ++k) {
                const JValue& e = list->items[k];
                out[k].Name = Wide(_arena, Str(e.Get("Name")));
                const std::string rename = Str(e.Get("ExportToRename"));
                if (!rename.empty()) out[k].ExportToRename = Wide(_arena, rename);
                out[k].Flags = D3D12_EXPORT_FLAG_NONE;
            }
            count = list->count;
            return out;
        };

        if (type.find("DXIL_LIBRARY") != std::string::npos) {
            auto* lib = (D3D12_DXIL_LIBRARY_DESC*)allocate(sizeof(D3D12_DXIL_LIBRARY_DESC), alignof(D3D12_DXIL_LIBRARY_DESC));
            const uint8_t* code = nullptr;
            size_t size = 0;
            // The library's bytes are the blob the capture kept on the state object, one per
            // library subobject in the order they appear (src/d3d12/src/raytracing.cpp).
            const std::string blob = "library:" + std::to_string(libraryIndex++);
            if (!_capture->Blob(object, blob, code, size) || !size) {
                failed = true;
                why = "the capture has no code for its " + blob;
                break;
            }
            lib->DXILLibrary = {code, size};
            lib->pExports = exportsOf(s.Get("pExports"), lib->NumExports);
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        } else if (type.find("HIT_GROUP") != std::string::npos) {
            auto* group = (D3D12_HIT_GROUP_DESC*)allocate(sizeof(D3D12_HIT_GROUP_DESC), alignof(D3D12_HIT_GROUP_DESC));
            group->HitGroupExport = Wide(_arena, Str(s.Get("HitGroupExport")));
            group->Type = Str(s.Get("HitGroupType")).find("PROCEDURAL") != std::string::npos
                        ? D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE : D3D12_HIT_GROUP_TYPE_TRIANGLES;
            const std::string anyHit = Str(s.Get("AnyHitShaderImport"));
            const std::string closestHit = Str(s.Get("ClosestHitShaderImport"));
            const std::string intersection = Str(s.Get("IntersectionShaderImport"));
            if (!anyHit.empty()) group->AnyHitShaderImport = Wide(_arena, anyHit);
            if (!closestHit.empty()) group->ClosestHitShaderImport = Wide(_arena, closestHit);
            if (!intersection.empty()) group->IntersectionShaderImport = Wide(_arena, intersection);
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        } else if (type.find("RAYTRACING_SHADER_CONFIG") != std::string::npos) {
            auto* config = (D3D12_RAYTRACING_SHADER_CONFIG*)allocate(sizeof(D3D12_RAYTRACING_SHADER_CONFIG), alignof(UINT));
            config->MaxPayloadSizeInBytes = (UINT)(s.Get("MaxPayloadSizeInBytes") ? s.Get("MaxPayloadSizeInBytes")->Uint() : 0);
            config->MaxAttributeSizeInBytes = (UINT)(s.Get("MaxAttributeSizeInBytes") ? s.Get("MaxAttributeSizeInBytes")->Uint() : 0);
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        } else if (type.find("RAYTRACING_PIPELINE_CONFIG1") != std::string::npos) {
            auto* config = (D3D12_RAYTRACING_PIPELINE_CONFIG1*)allocate(sizeof(D3D12_RAYTRACING_PIPELINE_CONFIG1), alignof(UINT));
            config->MaxTraceRecursionDepth = (UINT)(s.Get("MaxTraceRecursionDepth") ? s.Get("MaxTraceRecursionDepth")->Uint() : 1);
            config->Flags = (D3D12_RAYTRACING_PIPELINE_FLAGS)ParseFlags(s.Get("Flags"), dxinsp::kEnum_D3D12_RAYTRACING_PIPELINE_FLAGS,
                                                            std::size(dxinsp::kEnum_D3D12_RAYTRACING_PIPELINE_FLAGS));
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
        } else if (type.find("RAYTRACING_PIPELINE_CONFIG") != std::string::npos) {
            auto* config = (D3D12_RAYTRACING_PIPELINE_CONFIG*)allocate(sizeof(D3D12_RAYTRACING_PIPELINE_CONFIG), alignof(UINT));
            config->MaxTraceRecursionDepth = (UINT)(s.Get("MaxTraceRecursionDepth") ? s.Get("MaxTraceRecursionDepth")->Uint() : 1);
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        } else if (type.find("GLOBAL_ROOT_SIGNATURE") != std::string::npos ||
                   type.find("LOCAL_ROOT_SIGNATURE") != std::string::npos) {
            const bool global = type.find("GLOBAL_ROOT_SIGNATURE") != std::string::npos;
            const uint64_t rootId = IdOf(s.Get(global ? "pGlobalRootSignature" : "pLocalRootSignature"));
            auto* root = (D3D12_GLOBAL_ROOT_SIGNATURE*)allocate(sizeof(D3D12_GLOBAL_ROOT_SIGNATURE), alignof(void*));
            root->pGlobalRootSignature = static_cast<ID3D12RootSignature*>(Object(rootId));
            if (!root->pGlobalRootSignature) {
                failed = true;
                why = "its " + std::string(global ? "global" : "local") + " root signature was not replayed";
                break;
            }
            out.Type = global ? D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE : D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
        } else if (type.find("STATE_OBJECT_CONFIG") != std::string::npos) {
            auto* config = (D3D12_STATE_OBJECT_CONFIG*)allocate(sizeof(D3D12_STATE_OBJECT_CONFIG), alignof(UINT));
            config->Flags = (D3D12_STATE_OBJECT_FLAGS)ParseFlags(s.Get("Flags"), dxinsp::kEnum_D3D12_STATE_OBJECT_FLAGS,
                                                      std::size(dxinsp::kEnum_D3D12_STATE_OBJECT_FLAGS));
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG;
        } else if (type.find("DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION") != std::string::npos) {
            auto* assoc = (D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*)allocate(
                sizeof(D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION), alignof(void*));
            assoc->SubobjectToAssociate = Wide(_arena, Str(s.Get("SubobjectToAssociate")));
            const JValue* list = s.Get("pExports");
            if (list && list->IsArray() && list->count) {
                auto* names = (LPCWSTR*)_arena.Alloc(sizeof(LPCWSTR) * list->count, alignof(LPCWSTR));
                for (uint32_t k = 0; k < list->count; ++k) names[k] = Wide(_arena, Str(&list->items[k]));
                assoc->pExports = names;
                assoc->NumExports = list->count;
            }
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
        } else if (type.find("SUBOBJECT_TO_EXPORTS_ASSOCIATION") != std::string::npos) {
            auto* assoc = (D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*)allocate(
                sizeof(D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION), alignof(void*));
            // The capture writes the associated subobject as its index in this array, which is the
            // only way a pointer into the description can be written down at all.
            const JValue* which = s.Get("pSubobjectToAssociate");
            const uint64_t index = which && which->IsNumber() ? which->Uint()
                                 : which && which->Get("subobject") ? which->Get("subobject")->Uint() : UINT64_MAX;
            if (index >= subobjects->count) {
                failed = true;
                why = "an association names a subobject the description does not have";
                break;
            }
            assoc->pSubobjectToAssociate = &built[index];
            const JValue* list = s.Get("pExports");
            if (list && list->IsArray() && list->count) {
                auto* names = (LPCWSTR*)_arena.Alloc(sizeof(LPCWSTR) * list->count, alignof(LPCWSTR));
                for (uint32_t k = 0; k < list->count; ++k) names[k] = Wide(_arena, Str(&list->items[k]));
                assoc->pExports = names;
                assoc->NumExports = list->count;
            }
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
        } else if (type.find("NODE_MASK") != std::string::npos) {
            auto* mask = (D3D12_NODE_MASK*)allocate(sizeof(D3D12_NODE_MASK), alignof(UINT));
            mask->NodeMask = 0;
            out.Type = D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK;
        } else {
            failed = true;
            why = "its " + type + " subobject is not replayed yet";
            break;
        }
    }
    if (failed) {
        Problem("state object " + std::to_string(id) + ": " + why);
        return nullptr;
    }

    D3D12_STATE_OBJECT_DESC built_desc{};
    const std::string kind = Str(desc->Get("Type"));
    built_desc.Type = kind.find("COLLECTION") != std::string::npos ? D3D12_STATE_OBJECT_TYPE_COLLECTION
                                                                   : D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    built_desc.NumSubobjects = subobjects->count;
    built_desc.pSubobjects = built;

    ID3D12StateObject* stateObject = nullptr;
    const HRESULT hr = _device5->CreateStateObject(&built_desc, IID_PPV_ARGS(&stateObject));
    if (FAILED(hr) || !stateObject) {
        Problem("state object " + std::to_string(id) + ": CreateStateObject failed (" + HrText(hr) + ")");
        return nullptr;
    }
    NoteStateObjectIdentifiers(id, object, stateObject);
    if (_x) {
        _x->Declare("ID3D12StateObject", "stateObject", id, stateObject);
        _x->Comment(DxExporter::Create, "ID3D12StateObject " + std::to_string(id)
                    + ": exported programs do not rebuild state objects yet (its shader identifiers would have to be looked up again)");
        _x->CountObject();
    }
    return stateObject;
}

/**
 * The identifier this runtime gives for each export the capture saw one for, keyed by the captured
 * identifier. This is the whole of what makes a captured binding table mean anything here: the
 * bytes in it are the other machine's, and only the export name is the same on both.
 */
void DxReplayer::NoteStateObjectIdentifiers(uint64_t id, const JValue& object, ID3D12StateObject* stateObject) {
    const JValue* updates = object.Get("updates");
    const JValue* identifiers = updates ? updates->Get("shaderIdentifiers") : nullptr;
    const JValue* exports = identifiers ? identifiers->Get("exports") : nullptr;
    if (!exports || !exports->IsArray()) return;
    ID3D12StateObjectProperties* properties = nullptr;
    if (FAILED(stateObject->QueryInterface(IID_PPV_ARGS(&properties))) || !properties) {
        Problem("state object " + std::to_string(id) + ": it has no ID3D12StateObjectProperties, so its binding tables cannot be rebuilt");
        return;
    }
    auto& map = _shaderIdentifiers[id];
    for (uint32_t i = 0; i < exports->count; ++i) {
        const JValue& e = exports->items[i];
        const std::string name = Str(e.Get("name"));
        const std::string captured = Str(e.Get("identifier"));
        if (name.empty() || captured.size() != kIdentifierSize * 2) continue;
        const void* here = properties->GetShaderIdentifier(Wide(_arena, name));
        if (!here) {
            Problem("state object " + std::to_string(id) + ": this runtime gives no identifier for \"" + name
                    + "\", so records of it in a binding table cannot be replayed");
            continue;
        }
        Identifier bytes{};
        std::memcpy(bytes.data(), here, kIdentifierSize);
        map[captured] = bytes;
    }
    properties->Release();
}

// ---------------------------------------------------------------------------------------------
// Builds

/**
 * The instances a top level build reads, with each one's bottom level address remapped, in a
 * buffer of the replay's own.
 *
 * The captured instance buffer is uploaded as it was, so its 64-byte records hold the captured
 * process's addresses in their last eight bytes. Those cannot be rewritten in place — the buffer
 * may be the application's upload heap, rewritten every frame — so the build is pointed at a copy.
 */
D3D12_GPU_VIRTUAL_ADDRESS DxReplayer::RemapInstances(const JValue& command, UINT count) {
    return RemapInstancesFrom(command.Get("buildData"), count);
}

D3D12_GPU_VIRTUAL_ADDRESS DxReplayer::RemapInstancesFrom(const JValue* list, UINT count) {
    if (!count) return 0;
    uint64_t dataId = 0;
    for (uint32_t i = 0; list && list->IsArray() && i < list->count; ++i) {
        const JValue& e = list->items[i];
        if (Str(e.Get("field")) == "InstanceDescs" && e.Get("capture")) dataId = e.Get("capture")->Uint();
    }
    if (!dataId) {
        Problem("a top level build's instances were not read back, so the scene it describes cannot be rebuilt");
        return 0;
    }
    auto it = _bufferData.find(dataId);
    const JValue* info = it != _bufferData.end() ? it->second->Get("info") : nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!info || info->Get("error") || !_capture->Payload(it->second->Get("payload"), data, size) || !size) {
        Problem("a top level build's instances are not in the capture");
        return 0;
    }
    const size_t have = std::min<size_t>(count, size / kInstanceStride);
    if (have < count) {
        Problem("a top level build reads " + std::to_string(count) + " instances and the capture holds "
                + std::to_string(have) + "; the rest are replayed as they were read back");
    }

    std::vector<uint8_t> instances(data, data + have * kInstanceStride);
    for (size_t i = 0; i < have; ++i) {
        uint64_t captured = 0;
        std::memcpy(&captured, instances.data() + i * kInstanceStride + kInstanceStructureOffset, sizeof(captured));
        const D3D12_GPU_VIRTUAL_ADDRESS here = RemapStructureAddress(captured);
        if (!here && captured) {
            // A bottom level the capture holds no object for: its rays would read whatever is at
            // that address here, so the instance is made empty instead, which at worst loses one
            // object rather than reading memory at random.
            Problem("an instance names a bottom level the capture holds no structure for; it is left pointing nowhere");
        } else if (here && !_structuresBuiltInFrame.count(captured) && _reportedUnbuilt.insert(captured).second) {
            // The usual case for a real application: a bottom level is built once, at load, and the
            // captured frame holds no build of it. The replay has its buffer but never wrote it, so
            // every ray through this instance misses and the object is missing from the frame.
            Problem("an instance names a bottom level the captured frame does not build (it was built before the "
                    "capture began), so the replay's copy of it is empty and rays through that instance miss");
        }
        std::memcpy(instances.data() + i * kInstanceStride + kInstanceStructureOffset, &here, sizeof(here));
    }
    return UploadTransient(instances.data(), instances.size(), "instances");
}

// ---------------------------------------------------------------------------------------------
// The shader binding table

/**
 * One region of a trace's binding table, rebuilt with this runtime's shader identifiers, in a
 * buffer of the replay's own. Returns 0 when the region has no contents in the capture.
 *
 * Only the identifier at the head of each record is rewritten. What follows it is the local root
 * signature's arguments, which the replay copies as they were: constants survive, and a descriptor
 * handle or a GPU address among them does not. Nothing in the capture says which a record's bytes
 * are, so guessing would be worse than copying.
 */
D3D12_GPU_VIRTUAL_ADDRESS DxReplayer::RemapBindingTable(const JValue& command, const char* region, UINT64 stride,
                                                        UINT64 size, uint64_t stateObjectId) {
    if (!size) return 0;
    const JValue* list = command.Get("bindingTableData");
    uint64_t dataId = 0;
    UINT64 capturedStride = stride;
    for (uint32_t i = 0; list && list->IsArray() && i < list->count; ++i) {
        const JValue& e = list->items[i];
        if (Str(e.Get("region")) != region) continue;
        if (e.Get("capture")) dataId = e.Get("capture")->Uint();
        if (e.Get("stride")) capturedStride = e.Get("stride")->Uint();
    }
    if (!dataId) {
        Problem(std::string("a trace's ") + region + " table was not read back, so its records cannot be replayed");
        return 0;
    }
    auto it = _bufferData.find(dataId);
    const JValue* info = it != _bufferData.end() ? it->second->Get("info") : nullptr;
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    if (!info || info->Get("error") || !_capture->Payload(it->second->Get("payload"), data, bytes) || !bytes) {
        Problem(std::string("a trace's ") + region + " table is not in the capture");
        return 0;
    }

    const auto identifiers = _shaderIdentifiers.find(stateObjectId);
    if (identifiers == _shaderIdentifiers.end()) {
        Problem(std::string("a trace's ") + region + " table belongs to a state object the replay did not make");
        return 0;
    }
    std::vector<uint8_t> table(data, data + std::min<size_t>(bytes, (size_t)size));
    const UINT64 walk = capturedStride ? capturedStride : table.size();
    uint32_t unresolved = 0;
    for (size_t at = 0; at + kIdentifierSize <= table.size(); at += (size_t)walk) {
        const std::string captured = HexBytes(table.data() + at, kIdentifierSize);
        auto hit = identifiers->second.find(captured);
        if (hit == identifiers->second.end()) {
            // A record whose identifier matched nothing: the capture's own UI reports these too, and
            // they are the interesting failure. Left as it was, which traces into nothing.
            ++unresolved;
            continue;
        }
        std::memcpy(table.data() + at, hit->second.data(), kIdentifierSize);
    }
    if (unresolved) {
        Problem(std::string("a trace's ") + region + " table has " + std::to_string(unresolved)
                + " record(s) whose shader identifier no export of the state object gave out; those rays run nothing");
    }
    return UploadTransient(table.data(), table.size(), region);
}

/** A buffer of the replay's own holding these bytes, alive until the replay ends. */
D3D12_GPU_VIRTUAL_ADDRESS DxReplayer::UploadTransient(const void* data, size_t size, const char* what) {
    if (!size) return 0;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // A binding table's start must be 64-byte aligned, which a committed buffer always is.
    desc.Width = std::max<size_t>(size, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    void* mapped = nullptr;
    if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&buffer))) ||
        FAILED(buffer->Map(0, nullptr, &mapped))) {
        if (buffer) buffer->Release();
        Problem(std::string("a buffer for the replay's ") + what + " could not be made");
        return 0;
    }
    std::memset(mapped, 0, (size_t)desc.Width);
    std::memcpy(mapped, data, size);
    buffer->Unmap(0, nullptr);
    _transients.push_back(buffer);
    return buffer->GetGPUVirtualAddress();
}

// ---------------------------------------------------------------------------------------------
// The commands

bool DxReplayer::RaytracingDevice() {
    if (_device5) return true;
    if (_noRaytracing) return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    if (FAILED(_device->QueryInterface(IID_PPV_ARGS(&_device5))) || !_device5 ||
        FAILED(_device5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) ||
        options.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        if (_device5) { _device5->Release(); _device5 = nullptr; }
        _noRaytracing = true;
        return false;
    }
    return true;
}

/** The list as the interface the ray tracing commands are on, or null on a runtime without it. */
ID3D12GraphicsCommandList4* DxReplayer::RaytracingList(ID3D12GraphicsCommandList* list) {
    ID3D12GraphicsCommandList4* out = nullptr;
    if (!list || FAILED(list->QueryInterface(IID_PPV_ARGS(&out))) || !out) return nullptr;
    out->Release();   // the replay holds no reference of its own; the list outlives the call
    return out;
}

bool DxReplayer::IssueRaytracingCommand(const std::string& method, const JValue& command, const JValue* args,
                                        ID3D12GraphicsCommandList* list, std::string& leftOut) {
    if (!RaytracingDevice()) {
        leftOut = "this GPU has no DXR (D3D12_RAYTRACING_TIER_1_0)";
        return false;
    }
    ID3D12GraphicsCommandList4* rtList = RaytracingList(list);
    if (!rtList) {
        leftOut = "the runtime has no ID3D12GraphicsCommandList4";
        return false;
    }
    static const JValue kNoArgs;
    const JValue& a = args && !args->IsNull() ? *args : kNoArgs;
    Decoder d(&a, _env);

    if (method == "SetPipelineState1") {
        auto* stateObject = static_cast<ID3D12StateObject*>(Object(IdOf(a.Get("pStateObject"))));
        if (!stateObject) {
            leftOut = "the state object was not replayed";
            return false;
        }
        rtList->SetPipelineState1(stateObject);
        _boundStateObject = IdOf(a.Get("pStateObject"));
        return true;
    }

    if (method == "BuildRaytracingAccelerationStructure") {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        const size_t unresolved = _env.unresolved;
        d.Struct("pDesc", desc);
        if (_env.unresolved != unresolved) {
            leftOut = "it reads memory the replay has no buffer for";
            return false;
        }
        if (desc.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
            // The instances' own bytes name bottom levels by the captured process's addresses.
            desc.Inputs.InstanceDescs = RemapInstances(command, desc.Inputs.NumDescs);
            desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            if (!desc.Inputs.InstanceDescs) {
                leftOut = "its instances could not be rebuilt";
                return false;
            }
        } else if (desc.Inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
            leftOut = "an opacity micromap array build is not replayed yet";
            return false;
        }
        if (!desc.DestAccelerationStructureData || !desc.ScratchAccelerationStructureData) {
            leftOut = "its destination or its scratch is not a buffer the replay has";
            return false;
        }
        rtList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        if (_x) _x->Comment(DxExporter::Frame, "BuildRaytracingAccelerationStructure: exported programs do not rebuild acceleration structures yet");
        return true;
    }

    if (method == "DispatchRays") {
        D3D12_DISPATCH_RAYS_DESC desc{};
        const size_t unresolved = _env.unresolved;
        d.Struct("pDesc", desc);
        if (_env.unresolved != unresolved) {
            leftOut = "it reads memory the replay has no buffer for";
            return false;
        }
        // Whose identifiers the table holds. The capture names it on the command; a capture taken
        // before that did not, and the last SetPipelineState1 is the next best thing.
        uint64_t stateObjectId = IdOf(command.Get("stateObject"));
        if (!stateObjectId) stateObjectId = _boundStateObject;
        desc.RayGenerationShaderRecord.StartAddress =
            RemapBindingTable(command, "RayGeneration", desc.RayGenerationShaderRecord.SizeInBytes,
                              desc.RayGenerationShaderRecord.SizeInBytes, stateObjectId);
        desc.MissShaderTable.StartAddress =
            RemapBindingTable(command, "Miss", desc.MissShaderTable.StrideInBytes, desc.MissShaderTable.SizeInBytes, stateObjectId);
        desc.HitGroupTable.StartAddress =
            RemapBindingTable(command, "HitGroup", desc.HitGroupTable.StrideInBytes, desc.HitGroupTable.SizeInBytes, stateObjectId);
        desc.CallableShaderTable.StartAddress =
            RemapBindingTable(command, "Callable", desc.CallableShaderTable.StrideInBytes, desc.CallableShaderTable.SizeInBytes, stateObjectId);
        // A trace with no raygen record runs nothing and the runtime rejects it.
        if (!desc.RayGenerationShaderRecord.StartAddress) {
            leftOut = "its ray generation record could not be rebuilt";
            return false;
        }
        if (!desc.MissShaderTable.StartAddress) desc.MissShaderTable = {};
        if (!desc.HitGroupTable.StartAddress) desc.HitGroupTable = {};
        if (!desc.CallableShaderTable.StartAddress) desc.CallableShaderTable = {};
        rtList->DispatchRays(&desc);
        if (_x) _x->Comment(DxExporter::Frame, "DispatchRays: exported programs do not rebuild shader binding tables yet");
        return true;
    }

    if (method == "CopyRaytracingAccelerationStructure") {
        D3D12_GPU_VIRTUAL_ADDRESS dest = 0, source = 0;
        d.Address("DestAccelerationStructureData", dest);
        d.Address("SourceAccelerationStructureData", source);
        if (!dest || !source) {
            leftOut = "its source or destination is not a buffer the replay has";
            return false;
        }
        const auto mode = (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE)
            ParseEnum(a.Get("Mode"), dxinsp::kEnum_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE,
                      std::size(dxinsp::kEnum_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE));
        rtList->CopyRaytracingAccelerationStructure(dest, source, mode);
        return true;
    }

    if (method == "EmitRaytracingAccelerationStructurePostbuildInfo") {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC desc{};
        const size_t unresolved = _env.unresolved;
        d.Struct("pDesc", desc);
        const JValue* sources = a.Get("pSourceAccelerationStructureData");
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addresses;
        for (uint32_t i = 0; sources && sources->IsArray() && i < sources->count; ++i) {
            Decoder one(&sources->items[i], _env);
            addresses.push_back(one.AddressOf(&sources->items[i]));
        }
        if (_env.unresolved != unresolved || !desc.DestBuffer || addresses.empty()) {
            leftOut = "it writes or reads memory the replay has no buffer for";
            return false;
        }
        rtList->EmitRaytracingAccelerationStructurePostbuildInfo(&desc, (UINT)addresses.size(), addresses.data());
        return true;
    }

    leftOut = "the replay does not issue it yet";
    return false;
}

// ---------------------------------------------------------------------------------------------
// Structures built before the capture began
//
// An engine builds its bottom levels once, at load, so the captured frame holds no build of them and
// the replay's copy of each is a buffer nothing ever wrote: every ray through it misses. The capture
// library records each structure's last build as it goes by and, when a capture starts, reads back
// what that build read (src/d3d12/src/raytracing.h, ReadBackEarlierStructures). From those the
// replay builds each such structure once, before the frame: the bottom levels, then the top levels
// over them.
//
// What was read back is what those buffers held when the capture began, which is what the build
// read for geometry that does not change. The inputs go into buffers of the replay's own rather than
// the application's, whose contents in the replay are only what the frame's own commands read.

/** Bytes of a read-back, or null when this capture has none of it (or has one of another buffer). */
static bool ReadBack(const CaptureFile& capture, const std::unordered_map<uint64_t, const JValue*>& data, const JValue& input,
                     const uint8_t*& bytes, size_t& size) {
    const uint64_t id = input.Get("capture") ? input.Get("capture")->Uint() : 0;
    auto it = data.find(id);
    if (!id || it == data.end()) return false;
    const JValue* info = it->second->Get("info");
    // `captureInputs` is overwritten by every capture: an id this capture read back of another
    // buffer is not this input.
    if (!info || info->Get("error") || !info->Get("buffer") || !input.Get("buffer") ||
        info->Get("buffer")->Uint() != input.Get("buffer")->Uint()) return false;
    return capture.Payload(it->second->Get("payload"), bytes, size) && size;
}

void DxReplayer::BuildEarlierStructures() {
    const JValue* objects = _capture->Objects();
    if (!objects || !objects->IsArray()) return;

    struct Earlier {
        uint64_t id = 0;
        uint64_t captured = 0;
        bool topLevel = false;
        const JValue* build = nullptr;
        const JValue* inputs = nullptr;
    };
    std::vector<Earlier> earlier;
    for (uint32_t i = 0; i < objects->count; ++i) {
        const JValue& o = objects->items[i];
        if (Str(o.Get("type")) != "ID3D12RaytracingAccelerationStructure") continue;
        const JValue* updates = o.Get("updates");
        const JValue* build = updates ? updates->Get("build") : nullptr;
        const JValue* read = updates ? updates->Get("captureInputs") : nullptr;
        const JValue* inputs = read ? read->Get("inputs") : nullptr;
        const JValue* address = o.Get("args") ? o.Get("args")->Get("Address") : nullptr;
        const JValue* number = address ? address->Get("address") : nullptr;
        if (!build || !inputs || !inputs->IsArray() || !number || !number->IsString()) continue;
        Earlier e;
        e.id = o.Get("id")->Uint();
        e.captured = strtoull(Str(number).c_str(), nullptr, 0);
        // A structure the frame builds itself is left to the frame.
        if (!e.captured || _structuresBuiltInFrame.count(e.captured)) continue;
        e.topLevel = Str(build->Get("Type")).find("TOP_LEVEL") != std::string::npos;
        e.build = build;
        e.inputs = inputs;
        earlier.push_back(e);
    }
    if (earlier.empty()) return;
    if (!RaytracingDevice()) {
        Problem("the capture has acceleration structures built before it began, and this GPU has no DXR to build them with");
        return;
    }
    // Bottom levels first: a top level's instances point at them.
    std::stable_sort(earlier.begin(), earlier.end(), [](const Earlier& a, const Earlier& b) { return !a.topLevel && b.topLevel; });

    struct Planned {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
        bool topLevel = false;
        uint64_t captured = 0;
    };
    std::vector<Planned> planned;
    planned.reserve(earlier.size());
    for (const Earlier& e : earlier) {
        Planned p;
        p.topLevel = e.topLevel;
        p.captured = e.captured;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in = p.desc.Inputs;
        in.Type = e.topLevel ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS)ParseFlags(e.build->Get("Flags"),
            dxinsp::kEnum_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS, std::size(dxinsp::kEnum_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS));
        // A build made from scratch: an update needs a source, and there is none before the frame.
        in.Flags &= ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = (UINT)(e.build->Get("NumDescs") ? e.build->Get("NumDescs")->Uint() : 0);
        bool complete = true;
        if (e.topLevel) {
            in.InstanceDescs = RemapInstancesFrom(e.inputs, in.NumDescs);
            complete = in.InstanceDescs != 0;
        } else {
            const JValue* list = e.build->Get("geometries");
            for (uint32_t g = 0; list && list->IsArray() && g < list->count; ++g) {
                D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
                // The description's own addresses name the application's buffers, whose contents
                // the replay does not have before the frame; each is replaced below by an upload of
                // what was read back.
                const size_t unresolved = _env.unresolved;
                Decoder d(&list->items[g], _env);
                Reflect(d, geometry);
                _env.unresolved = unresolved;
                // Cleared, so an input that was not read back stays visibly missing rather than
                // pointing at the application's buffer. The two arms share storage: only the one
                // the type names is touched.
                if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
                    geometry.Triangles.VertexBuffer.StartAddress = 0;
                    geometry.Triangles.IndexBuffer = 0;
                    geometry.Triangles.Transform3x4 = 0;
                } else {
                    geometry.AABBs.AABBs.StartAddress = 0;
                }
                p.geometries.push_back(geometry);
            }
            for (uint32_t k = 0; k < e.inputs->count; ++k) {
                const JValue& input = e.inputs->items[k];
                const uint32_t g = input.Get("geometry") ? (uint32_t)input.Get("geometry")->Uint() : UINT32_MAX;
                if (g >= p.geometries.size()) continue;
                const uint8_t* bytes = nullptr;
                size_t size = 0;
                if (!ReadBack(*_capture, _bufferData, input, bytes, size)) continue;
                const std::string field = Str(input.Get("field"));
                const D3D12_GPU_VIRTUAL_ADDRESS here = UploadTransient(bytes, size, "earlier build input");
                D3D12_RAYTRACING_GEOMETRY_DESC& geometry = p.geometries[g];
                if (field == "VertexBuffer") geometry.Triangles.VertexBuffer.StartAddress = here;
                else if (field == "IndexBuffer") geometry.Triangles.IndexBuffer = here;
                else if (field == "Transform3x4") geometry.Triangles.Transform3x4 = here;
                else if (field == "AABBs") geometry.AABBs.AABBs.StartAddress = here;
            }
            // Every geometry has to have been read back: a build over a buffer the replay never
            // wrote would make a structure of garbage and look like one.
            for (const auto& geometry : p.geometries) {
                if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
                    if (!geometry.Triangles.VertexBuffer.StartAddress) complete = false;
                    if (geometry.Triangles.IndexCount && geometry.Triangles.IndexFormat != DXGI_FORMAT_UNKNOWN &&
                        !geometry.Triangles.IndexBuffer) complete = false;
                } else if (!geometry.AABBs.AABBs.StartAddress) {
                    complete = false;
                }
            }
            in.NumDescs = (UINT)p.geometries.size();
        }
        p.desc.DestAccelerationStructureData = RemapStructureAddress(e.captured);
        if (!complete || !p.desc.DestAccelerationStructureData) {
            Problem("acceleration structure " + std::to_string(e.id) + ": built before the capture began, and what it was built "
                    "from was not all read back, so the replay cannot build it; rays through it miss");
            continue;
        }
        // Counted as built from here, so the top levels planned after it know their instances land
        // on something. A build that then fails takes it back out below.
        _structuresBuiltInFrame.insert(e.captured);
        planned.push_back(std::move(p));
    }

    // Scratch of the replay's own for each, sized by this driver: another GPU's sizes mean nothing here.
    std::vector<ID3D12Resource*> scratch;
    for (Planned& p : planned) {
        if (!p.topLevel) p.desc.Inputs.pGeometryDescs = p.geometries.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        _device5->GetRaytracingAccelerationStructurePrebuildInfo(&p.desc.Inputs, &info);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = std::max<UINT64>(info.ScratchDataSizeInBytes, 256);
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* buffer = nullptr;
        // A buffer starts in COMMON whatever it is asked for, and a build promotes it.
        if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                    nullptr, IID_PPV_ARGS(&buffer)))) {
            p.desc.DestAccelerationStructureData = 0;
            _structuresBuiltInFrame.erase(p.captured);
            continue;
        }
        scratch.push_back(buffer);
        p.desc.ScratchAccelerationStructureData = buffer->GetGPUVirtualAddress();
    }

    uint32_t built = 0;
    const bool ok = RunOneTime([&](ID3D12GraphicsCommandList* list) {
        ID3D12GraphicsCommandList4* rtList = RaytracingList(list);
        if (!rtList) return;
        bool lastTopLevel = false;
        for (Planned& p : planned) {
            if (!p.desc.DestAccelerationStructureData || !p.desc.ScratchAccelerationStructureData) continue;
            // Every bottom level finished before the first top level reads one.
            if (p.topLevel && !lastTopLevel) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                list->ResourceBarrier(1, &barrier);
            }
            lastTopLevel = p.topLevel;
            rtList->BuildRaytracingAccelerationStructure(&p.desc, 0, nullptr);
            ++built;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &barrier);
    });
    for (ID3D12Resource* r : scratch) r->Release();
    if (!ok) {
        for (const Planned& p : planned) _structuresBuiltInFrame.erase(p.captured);
        Problem("the builds of the structures made before the capture began could not be run");
        return;
    }
    _report->earlierStructuresBuilt = built;
}

}  // namespace dxreplay
