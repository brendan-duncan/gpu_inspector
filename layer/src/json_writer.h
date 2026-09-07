// Minimal streaming JSON writer used by the generated Vulkan serializers.
//
// Vulkan handles are written through a HandleResolver so the tracker can replace raw handle
// values with stable object ids: {"__id": 42, "__class": "VkImage"}. Unresolved handles are
// written as {"__handle": "0x1234", "__class": "VkImage"}.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace vkinsp {

struct HandleResolver {
    virtual ~HandleResolver() = default;
    // Returns the tracker id for a handle of the given HandleType, or 0 if unknown.
    virtual uint64_t Resolve(int handleType, uint64_t handle) = 0;
};

class JsonWriter {
public:
    explicit JsonWriter(HandleResolver* resolver = nullptr) : _resolver(resolver) {}

    void Reset() { _out.clear(); _needComma = false; }
    std::string& str() { return _out; }
    const std::string& str() const { return _out; }
    void SetResolver(HandleResolver* r) { _resolver = r; }

    // Arrays of scalars longer than this are summarized instead of expanded.
    uint32_t maxScalarArray = 1024;
    // Raw byte blobs up to this size are inlined as base64; larger ones are summarized.
    uint32_t maxInlineBytes = 4096;

    void BeginObject() { Sep(); _out += '{'; _needComma = false; }
    void EndObject() { _out += '}'; _needComma = true; }
    void BeginArray() { Sep(); _out += '['; _needComma = false; }
    void EndArray() { _out += ']'; _needComma = true; }

    void Key(const char* key) {
        Sep();
        _out += '"';
        _out += key;
        _out += "\":";
        _needComma = false;
    }

    void Null() { Sep(); _out += "null"; _needComma = true; }
    void Bool(bool b) { Sep(); _out += b ? "true" : "false"; _needComma = true; }

    void Int(int64_t v) {
        Sep();
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        _out += buf;
        _needComma = true;
    }

    void Uint(uint64_t v) {
        Sep();
        char buf[32];
        if (v > 9007199254740991ull) {
            // Beyond 2^53: keep exact as a string.
            snprintf(buf, sizeof(buf), "\"%llu\"", (unsigned long long)v);
        } else {
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
        }
        _out += buf;
        _needComma = true;
    }

    void Double(double v) {
        Sep();
        if (!std::isfinite(v)) {
            _out += "null";
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.9g", v);
            _out += buf;
        }
        _needComma = true;
    }

    void String(const char* s) {
        if (!s) { Null(); return; }
        String(std::string_view(s));
    }

    void String(std::string_view s) {
        Sep();
        _out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': _out += "\\\""; break;
                case '\\': _out += "\\\\"; break;
                case '\n': _out += "\\n"; break;
                case '\r': _out += "\\r"; break;
                case '\t': _out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        _out += buf;
                    } else {
                        _out += (char)c;
                    }
            }
        }
        _out += '"';
        _needComma = true;
    }

    // Fixed-size char buffer that may or may not be null-terminated.
    void FixedString(const char* s, size_t maxLen) {
        size_t n = 0;
        while (n < maxLen && s[n]) ++n;
        String(std::string_view(s, n));
    }

    // Enum value: name if known, else the numeric value.
    void Enum(const char* name, int64_t value) {
        if (name) String(name); else Int(value);
    }

    void Handle(int handleType, const char* className, uint64_t handle) {
        Sep();
        if (handle == 0) {
            _out += "null";
            _needComma = true;
            return;
        }
        uint64_t id = _resolver ? _resolver->Resolve(handleType, handle) : 0;
        char buf[96];
        if (id) {
            snprintf(buf, sizeof(buf), "{\"__id\":%llu,\"__class\":\"%s\"}", (unsigned long long)id, className);
        } else {
            snprintf(buf, sizeof(buf), "{\"__handle\":\"0x%llx\",\"__class\":\"%s\"}",
                     (unsigned long long)handle, className);
        }
        _out += buf;
        _needComma = true;
    }

    // Opaque pointer value (never dereferenced).
    void Pointer(const void* p) {
        Sep();
        if (!p) { _out += "null"; _needComma = true; return; }
        char buf[32];
        snprintf(buf, sizeof(buf), "\"0x%llx\"", (unsigned long long)(uintptr_t)p);
        _out += buf;
        _needComma = true;
    }

    // Raw bytes: inline base64 if small, otherwise a size summary.
    void Bytes(const void* data, size_t size) {
        if (!data) { Null(); return; }
        Sep();
        if (size > maxInlineBytes) {
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"__bytes\":%llu}", (unsigned long long)size);
            _out += buf;
        } else {
            _out += "{\"__bytes\":";
            char buf[32];
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)size);
            _out += buf;
            _out += ",\"base64\":\"";
            AppendBase64(static_cast<const uint8_t*>(data), size);
            _out += "\"}";
        }
        _needComma = true;
    }

    // Summary emitted in place of an oversized scalar array.
    void ArraySummary(uint64_t count) {
        Sep();
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"__count\":%llu,\"__truncated\":true}", (unsigned long long)count);
        _out += buf;
        _needComma = true;
    }

    void Raw(std::string_view json) {
        Sep();
        _out.append(json.data(), json.size());
        _needComma = true;
    }

private:
    void Sep() {
        if (_needComma) _out += ',';
    }

    void AppendBase64(const uint8_t* data, size_t size) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t i = 0;
        for (; i + 2 < size; i += 3) {
            uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
            _out += tbl[(v >> 18) & 63]; _out += tbl[(v >> 12) & 63];
            _out += tbl[(v >> 6) & 63]; _out += tbl[v & 63];
        }
        if (i < size) {
            uint32_t v = data[i] << 16;
            if (i + 1 < size) v |= data[i + 1] << 8;
            _out += tbl[(v >> 18) & 63]; _out += tbl[(v >> 12) & 63];
            _out += (i + 1 < size) ? tbl[(v >> 6) & 63] : '=';
            _out += '=';
        }
    }

    std::string _out;
    bool _needComma = false;
    HandleResolver* _resolver = nullptr;
};

} // namespace vkinsp
