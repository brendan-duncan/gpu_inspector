// What the generated decoders (gen/vk_decode.gen.cpp) call to read the layer's JSON back into
// Vulkan values: numbers, enum and flag names, handles resolved to the replay's own objects,
// strings and byte blobs copied into the arena, and a list of what could not be decoded.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "arena.h"
#include "json.h"

namespace vkreplay {

struct VkFunctions;

struct EnumEntry {
    const char* name;
    int64_t value;
};

class DecodeContext {
public:
    explicit DecodeContext(Arena& arena) : arena(arena) {}

    Arena& arena;
    /** The Vulkan functions the generated recorders call. */
    const VkFunctions* fns = nullptr;
    /** A tracker id ("__id") to the replay's handle for it; 0 when there is none. */
    std::function<uint64_t(uint64_t id, std::string_view className)> resolve;
    /** Ids the replay leaves out on purpose (surfaces, device memory): no problem is reported when they resolve to nothing. */
    std::function<bool(uint64_t id, std::string_view className)> quiet;
    /** Handles that resolved to nothing (not counting quiet ones): an object or command naming them is not created or recorded. */
    size_t unresolved = 0;
    /** Prefixed to problems: the object or command being decoded. */
    std::string where;
    std::vector<std::string> problems;

    template <typename T>
    T* Make(size_t count) { return arena.Make<T>(count); }

    void Problem(std::string message);

    bool Bool(const JValue* v) const;
    int64_t Int(const JValue* v) const;
    uint64_t Uint(const JValue* v) const;
    double Double(const JValue* v) const;
    uint64_t Handle(const JValue* v);
    const char* String(const JValue* v);
    const char* const* Strings(const JValue* v);
    void FixedString(const JValue* v, char* dst, size_t size);
    /** A {"__bytes", "base64"} blob; zeroed memory of the recorded size (and a problem) when the data was not captured. */
    const void* Bytes(const JValue* v);
    int64_t Enum(const JValue* v, const EnumEntry* table, size_t count, const char* type);
    uint64_t Flags(const JValue* v, const EnumEntry* table, size_t count, const char* type);
};

/** Looks a name up in a table sorted by name; false when absent. */
bool LookupEnum(const EnumEntry* table, size_t count, std::string_view name, int64_t& value);

/** Decodes standard base64 into `out`; false on malformed input. */
bool DecodeBase64(std::string_view text, std::vector<uint8_t>& out);

} // namespace vkreplay
