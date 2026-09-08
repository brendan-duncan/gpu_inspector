// Minimal JSON parser for messages from the UI (small objects: actions and parameters).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vkinsp {

struct JsonValue {
    enum Kind { Null, Boolean, Number, String, Array, Object } kind = Null;  // Boolean, not Bool: Xlib macro
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

class JsonParser {
public:
    static bool Parse(const std::string& text, JsonValue& out) {
        JsonParser p(text);
        p.SkipWs();
        if (!p.Value(out)) return false;
        p.SkipWs();
        return p._pos == text.size();
    }

private:
    explicit JsonParser(const std::string& t) : _s(t) {}

    void SkipWs() {
        while (_pos < _s.size() && (_s[_pos] == ' ' || _s[_pos] == '\n' || _s[_pos] == '\r' || _s[_pos] == '\t')) ++_pos;
    }

    bool Value(JsonValue& v) {
        if (_pos >= _s.size()) return false;
        char c = _s[_pos];
        if (c == '{') return Object(v);
        if (c == '[') return Array(v);
        if (c == '"') { v.kind = JsonValue::String; return Str(v.str); }
        if (_s.compare(_pos, 4, "true") == 0) { v.kind = JsonValue::Boolean; v.b = true; _pos += 4; return true; }
        if (_s.compare(_pos, 5, "false") == 0) { v.kind = JsonValue::Boolean; v.b = false; _pos += 5; return true; }
        if (_s.compare(_pos, 4, "null") == 0) { v.kind = JsonValue::Null; _pos += 4; return true; }
        return Number(v);
    }

    bool Number(JsonValue& v) {
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
            if (c == '\\') {
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
            } else {
                out += c;
            }
        }
        return false;
    }

    bool Array(JsonValue& v) {
        v.kind = JsonValue::Array;
        ++_pos;
        SkipWs();
        if (_pos < _s.size() && _s[_pos] == ']') { ++_pos; return true; }
        while (true) {
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

    bool Object(JsonValue& v) {
        v.kind = JsonValue::Object;
        ++_pos;
        SkipWs();
        if (_pos < _s.size() && _s[_pos] == '}') { ++_pos; return true; }
        while (true) {
            SkipWs();
            std::string key;
            if (!Str(key)) return false;
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

} // namespace vkinsp
