// GPU Inspector plugin SDK: JSON in and out.
//
// A capture library talks to the inspector in JSON messages (src/app/src/shared/protocol.ts is the
// vocabulary). JsonWriter builds them as a stream, with the two conventions the inspector reads
// arguments by: an object is referenced as {"__id": N, "__class": "GLTexture"} (Ref), and bytes are
// {"__bytes": size, "base64": "..."} (Bytes). JsonValue / ParseJson read what the inspector sends,
// which is small objects of actions and their parameters.
//
// Header-only and dependency-free, like the rest of the SDK (docs/PLUGINS.md). Adapted from the
// Vulkan layer's own writer and parser (src/vulkan/src/json_writer.h, json_parse.h).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace gpuinsp::sdk {

class JsonWriter {
public:
    void Reset() { _out.clear(); _needComma = false; }
    std::string& str() { return _out; }
    const std::string& str() const { return _out; }

    /** Raw byte blobs up to this size are inlined as base64; larger ones only say their size. */
    uint32_t maxInlineBytes = 4096;

    void BeginObject() { Sep(); _out += '{'; _needComma = false; }
    void EndObject() { _out += '}'; _needComma = true; }
    void BeginArray() { Sep(); _out += '['; _needComma = false; }
    void EndArray() { _out += ']'; _needComma = true; }

    void Key(std::string_view key) {
        Sep();
        Quoted(key);
        _out += ':';
        _needComma = false;
    }

    void Null() { Sep(); _out += "null"; _needComma = true; }
    void Boolean(bool b) { Sep(); _out += b ? "true" : "false"; _needComma = true; }

    void Int(int64_t v) {
        Sep();
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        _out += buf;
        _needComma = true;
    }

    /** Beyond 2^53 the value is written as a string, which a JSON number could not hold exactly. */
    void Uint(uint64_t v) {
        Sep();
        char buf[32];
        if (v > 9007199254740991ull) snprintf(buf, sizeof(buf), "\"%llu\"", (unsigned long long)v);
        else snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
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
        Quoted(s);
        _needComma = true;
    }

    /** An enum by name, or its value when it has none the library knows. */
    void Enum(const char* name, int64_t value) {
        if (name) String(name); else Int(value);
    }

    /** A tracked object: the id its AddObject gave it, and its type. Id 0 is written as null. */
    void Ref(uint64_t id, const char* className) {
        Sep();
        if (id == 0) {
            _out += "null";
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"__id\":%llu,\"__class\":\"%s\"}", (unsigned long long)id, className);
            _out += buf;
        }
        _needComma = true;
    }

    /** An object the library does not track, by its raw value. */
    void UntrackedRef(uint64_t handle, const char* className) {
        Sep();
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"__handle\":\"0x%llx\",\"__class\":\"%s\"}", (unsigned long long)handle, className);
        _out += buf;
        _needComma = true;
    }

    /** A pointer value, never dereferenced: "0x...". */
    void Pointer(const void* p) {
        Sep();
        if (!p) {
            _out += "null";
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "\"0x%llx\"", (unsigned long long)(uintptr_t)p);
            _out += buf;
        }
        _needComma = true;
    }

    /** Raw bytes: inline base64 when small, otherwise their size alone. */
    void Bytes(const void* data, size_t size) {
        if (!data) { Null(); return; }
        Sep();
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"__bytes\":%llu", (unsigned long long)size);
        _out += buf;
        if (size <= maxInlineBytes) {
            _out += ",\"base64\":\"";
            AppendBase64(static_cast<const uint8_t*>(data), size);
            _out += '"';
        }
        _out += '}';
        _needComma = true;
    }

    /** What stands in for an array too long to write: {"__count": n, "__truncated": true}. */
    void ArraySummary(uint64_t count) {
        Sep();
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"__count\":%llu,\"__truncated\":true}", (unsigned long long)count);
        _out += buf;
        _needComma = true;
    }

    /** Already-serialized JSON, inserted as one value. */
    void Raw(std::string_view json) {
        Sep();
        _out.append(json.data(), json.size());
        _needComma = true;
    }

private:
    void Sep() {
        if (_needComma) _out += ',';
    }

    void Quoted(std::string_view s) {
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
};

/** A parsed JSON value: what the inspector sends is small objects of actions and parameters. */
struct JsonValue {
    enum Kind { Null, Boolean, Number, String, Array, Object } kind = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    const JsonValue* Get(const char* key) const {
        if (kind != Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    std::string GetString(const char* key, const char* def = "") const {
        const JsonValue* v = Get(key);
        return (v && v->kind == String) ? v->str : def;
    }
    double GetNumber(const char* key, double def = 0) const {
        const JsonValue* v = Get(key);
        return (v && v->kind == Number) ? v->num : def;
    }
    bool GetBool(const char* key, bool def = false) const {
        const JsonValue* v = Get(key);
        return (v && v->kind == Boolean) ? v->b : def;
    }
};

namespace detail {

class JsonParser {
public:
    explicit JsonParser(const std::string& t) : _s(t) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!Value(out)) return false;
        SkipWs();
        return _pos == _s.size();
    }

private:
    void SkipWs() {
        while (_pos < _s.size() && (_s[_pos] == ' ' || _s[_pos] == '\n' || _s[_pos] == '\r' || _s[_pos] == '\t')) ++_pos;
    }

    bool Value(JsonValue& v) {
        if (_pos >= _s.size()) return false;
        char c = _s[_pos];
        if (c == '{') return ParseObject(v);
        if (c == '[') return ParseArray(v);
        if (c == '"') { v.kind = JsonValue::String; return Str(v.str); }
        if (_s.compare(_pos, 4, "true") == 0) { v.kind = JsonValue::Boolean; v.b = true; _pos += 4; return true; }
        if (_s.compare(_pos, 5, "false") == 0) { v.kind = JsonValue::Boolean; v.b = false; _pos += 5; return true; }
        if (_s.compare(_pos, 4, "null") == 0) { v.kind = JsonValue::Null; _pos += 4; return true; }
        const char* start = _s.c_str() + _pos;
        char* end = nullptr;
        double d = strtod(start, &end);
        if (end == start) return false;
        v.kind = JsonValue::Number;
        v.num = d;
        _pos += (size_t)(end - start);
        return true;
    }

    bool Str(std::string& out) {
        if (_s[_pos] != '"') return false;
        ++_pos;
        while (_pos < _s.size()) {
            char c = _s[_pos++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (_pos >= _s.size()) return false;
            char e = _s[_pos++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (_pos + 4 > _s.size()) return false;
                    unsigned cp = (unsigned)strtoul(_s.substr(_pos, 4).c_str(), nullptr, 16);
                    _pos += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e; break;
            }
        }
        return false;
    }

    bool ParseArray(JsonValue& v) {
        v.kind = JsonValue::Array;
        ++_pos;
        SkipWs();
        if (_pos < _s.size() && _s[_pos] == ']') { ++_pos; return true; }
        for (;;) {
            SkipWs();
            JsonValue e;
            if (!Value(e)) return false;
            v.arr.push_back(std::move(e));
            SkipWs();
            if (_pos >= _s.size()) return false;
            if (_s[_pos] == ',') { ++_pos; continue; }
            if (_s[_pos] == ']') { ++_pos; return true; }
            return false;
        }
    }

    bool ParseObject(JsonValue& v) {
        v.kind = JsonValue::Object;
        ++_pos;
        SkipWs();
        if (_pos < _s.size() && _s[_pos] == '}') { ++_pos; return true; }
        for (;;) {
            SkipWs();
            std::string key;
            if (_pos >= _s.size() || !Str(key)) return false;
            SkipWs();
            if (_pos >= _s.size() || _s[_pos] != ':') return false;
            ++_pos;
            SkipWs();
            JsonValue e;
            if (!Value(e)) return false;
            v.obj[key] = std::move(e);
            SkipWs();
            if (_pos >= _s.size()) return false;
            if (_s[_pos] == ',') { ++_pos; continue; }
            if (_s[_pos] == '}') { ++_pos; return true; }
            return false;
        }
    }

    const std::string& _s;
    size_t _pos = 0;
};

}  // namespace detail

/** Parses one JSON document; false when it is not one. */
inline bool ParseJson(const std::string& text, JsonValue& out) {
    return detail::JsonParser(text).Parse(out);
}

}  // namespace gpuinsp::sdk
