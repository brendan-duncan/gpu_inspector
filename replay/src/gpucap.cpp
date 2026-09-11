#include "gpucap.h"

#include <cstdio>
#include <cstring>

namespace vkreplay {

namespace {
const char kMagic[] = "GPUCAP 1\n";
const size_t kMagicSize = sizeof(kMagic) - 1;
} // namespace

bool CaptureFile::Load(const std::string& path, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    long long size = (long long)ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    _bytes.resize((size_t)size);
    size_t read = size > 0 ? std::fread(_bytes.data(), 1, (size_t)size, f) : 0;
    std::fclose(f);
    if (read != (size_t)size) {
        error = "cannot read " + path;
        return false;
    }
    if (_bytes.size() < kMagicSize + 4 || std::memcmp(_bytes.data(), kMagic, kMagicSize) != 0) {
        error = "not a GPU Inspector capture file (bad header)";
        return false;
    }
    uint32_t length = 0;
    std::memcpy(&length, _bytes.data() + kMagicSize, 4);
    size_t start = kMagicSize + 4;
    if (start + length > _bytes.size()) {
        error = "the capture file is truncated";
        return false;
    }
    _manifestSize = length;
    _payloadBase = start + length;
    if (!_doc.Parse(_bytes.data() + start, length, error)) {
        error = "the manifest is not valid JSON: " + error;
        return false;
    }
    const JValue* format = _doc.Root().Get("format");
    if (!format || format->Str() != "gpu-inspector-capture") {
        error = "not a GPU Inspector capture file (unknown format)";
        return false;
    }
    if (const JValue* objects = Objects(); objects && objects->IsArray()) {
        for (uint32_t i = 0; i < objects->count; ++i) {
            const JValue& o = objects->items[i];
            if (const JValue* id = o.Get("id")) _objects[id->Uint()] = &o;
        }
    }
    return true;
}

const JValue* CaptureFile::Object(uint64_t id) const {
    auto it = _objects.find(id);
    return it == _objects.end() ? nullptr : it->second;
}

bool CaptureFile::Payload(const JValue* payload, const uint8_t*& data, size_t& size) const {
    if (!payload || !payload->IsArray() || payload->count < 2) return false;
    uint64_t offset = payload->items[0].Uint();
    uint64_t length = payload->items[1].Uint();
    if (_payloadBase + offset + length > _bytes.size()) return false;
    data = reinterpret_cast<const uint8_t*>(_bytes.data() + _payloadBase + offset);
    size = (size_t)length;
    return true;
}

bool CaptureFile::Blob(const JValue& object, std::string_view name, const uint8_t*& data, size_t& size) const {
    const JValue* blobs = object.Get("blobs");
    if (!blobs || !blobs->IsArray()) return false;
    for (uint32_t i = 0; i < blobs->count; ++i) {
        const JValue& b = blobs->items[i];
        const JValue* n = b.Get("name");
        if (n && n->Str() == name) return Payload(b.Get("payload"), data, size);
    }
    return false;
}

} // namespace vkreplay
