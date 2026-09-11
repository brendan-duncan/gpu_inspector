// A .gpucap capture file (app/src/renderer/capture_format.ts): the "GPUCAP 1\n" header, a u32
// manifest length, the JSON manifest, then the binary payloads the manifest references as
// [offset, length] from the end of the manifest.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "json.h"

namespace vkreplay {

class CaptureFile {
public:
    bool Load(const std::string& path, std::string& error);

    const JValue& Manifest() const { return _doc.Root(); }
    const JValue* Objects() const { return _doc.Root().Get("objects"); }
    const JValue* Commands() const { return _doc.Root().Get("commands"); }
    const JValue* Textures() const { return _doc.Root().Get("textures"); }
    const JValue* Buffers() const { return _doc.Root().Get("buffers"); }

    /** An object by tracker id, or null. */
    const JValue* Object(uint64_t id) const;
    /** The bytes a [offset, length] payload names; false when absent or out of range. */
    bool Payload(const JValue* payload, const uint8_t*& data, size_t& size) const;
    /** A payload of an object's blob by name ("SPIR-V", "fragment:main"); false when absent. */
    bool Blob(const JValue& object, std::string_view name, const uint8_t*& data, size_t& size) const;

    size_t FileSize() const { return _bytes.size(); }
    size_t ManifestSize() const { return _manifestSize; }

private:
    std::vector<char> _bytes;
    size_t _manifestSize = 0;
    size_t _payloadBase = 0;
    JsonDocument _doc;
    std::unordered_map<uint64_t, const JValue*> _objects;
};

} // namespace vkreplay
