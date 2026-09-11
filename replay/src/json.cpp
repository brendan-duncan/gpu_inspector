#include "json.h"

#include <charconv>
#include <cstdlib>
#include <cstring>

namespace vkreplay {

namespace {

const int kMaxDepth = 512;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

uint64_t ParseUnsigned(const char* text, size_t length) {
    if (!text || !length) return 0;
    uint64_t value = 0;
    if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        std::from_chars(text + 2, text + length, value, 16);
        return value;
    }
    auto result = std::from_chars(text, text + length, value);
    if (result.ec == std::errc() && result.ptr == text + length) return value;
    // A negative or fractional number: through double, as JSON writers intend.
    std::string copy(text, length);
    double d = std::strtod(copy.c_str(), nullptr);
    return d < 0 ? (uint64_t)(int64_t)d : (uint64_t)d;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

bool Hex4(const char* p, uint32_t& out) {
    out = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i];
        out <<= 4;
        if (c >= '0' && c <= '9') out |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') out |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') out |= (uint32_t)(c - 'A' + 10);
        else return false;
    }
    return true;
}

} // namespace

const JValue* JValue::Get(std::string_view key) const {
    if (type != JType::Object) return nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        if (members[i].key == key) return &members[i].value;
    }
    return nullptr;
}

int64_t JValue::Int() const {
    if (type == JType::Bool) return boolean ? 1 : 0;
    if (type != JType::Number && type != JType::String) return 0;
    int64_t value = 0;
    auto result = std::from_chars(text, text + length, value);
    if (result.ec == std::errc() && result.ptr == text + length) return value;
    return (int64_t)Double();
}

uint64_t JValue::Uint() const {
    if (type == JType::Bool) return boolean ? 1 : 0;
    if (type != JType::Number && type != JType::String) return 0;
    return ParseUnsigned(text, length);
}

double JValue::Double() const {
    if (type == JType::Bool) return boolean ? 1.0 : 0.0;
    if (type != JType::Number) return 0.0;
    std::string copy(text, length);
    return std::strtod(copy.c_str(), nullptr);
}

bool JsonDocument::Parse(const char* data, size_t size, std::string& error) {
    _arena.Reset();
    _root = JValue{};
    _begin = _pos = data;
    _end = data + size;
    _error = &error;
    _items.clear();
    _members.clear();
    if (!ParseValue(_root, 0)) return false;
    SkipSpace();
    if (_pos != _end) return Fail("unexpected text after the document");
    return true;
}

bool JsonDocument::Fail(const char* message) {
    *_error = std::string(message) + " at byte " + std::to_string(_pos - _begin);
    return false;
}

void JsonDocument::SkipSpace() {
    while (_pos < _end && (*_pos == ' ' || *_pos == '\n' || *_pos == '\r' || *_pos == '\t')) ++_pos;
}

bool JsonDocument::ParseString(const char*& text, size_t& length) {
    // _pos is just past the opening quote.
    const char* start = _pos;
    bool escaped = false;
    while (_pos < _end && *_pos != '"') {
        if (*_pos == '\\') {
            escaped = true;
            ++_pos;
        }
        ++_pos;
    }
    if (_pos >= _end) return Fail("unterminated string");
    const char* stop = _pos++;
    if (!escaped) {
        text = start;
        length = (size_t)(stop - start);
        return true;
    }
    std::string out;
    out.reserve((size_t)(stop - start));
    for (const char* p = start; p < stop; ++p) {
        if (*p != '\\') {
            out += *p;
            continue;
        }
        ++p;
        switch (*p) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (stop - p < 5 || !Hex4(p + 1, cp)) return Fail("bad \\u escape");
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && stop - p >= 7 && p[1] == '\\' && p[2] == 'u') {
                    uint32_t low = 0;
                    if (Hex4(p + 3, low) && low >= 0xDC00 && low < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        p += 6;
                    }
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return Fail("bad escape");
        }
    }
    char* copy = static_cast<char*>(_arena.Alloc(out.size() + 1, 1));
    std::memcpy(copy, out.data(), out.size());
    text = copy;
    length = out.size();
    return true;
}

bool JsonDocument::ParseValue(JValue& out, int depth) {
    if (depth > kMaxDepth) return Fail("nesting too deep");
    SkipSpace();
    if (_pos >= _end) return Fail("unexpected end of input");
    char c = *_pos;
    if (c == '{') {
        ++_pos;
        size_t start = _members.size();
        SkipSpace();
        if (_pos < _end && *_pos == '}') {
            ++_pos;
        } else {
            for (;;) {
                SkipSpace();
                if (_pos >= _end || *_pos != '"') return Fail("expected a member name");
                ++_pos;
                JMember member;
                const char* key = nullptr;
                size_t keyLength = 0;
                if (!ParseString(key, keyLength)) return false;
                member.key = std::string_view(key, keyLength);
                SkipSpace();
                if (_pos >= _end || *_pos != ':') return Fail("expected ':'");
                ++_pos;
                if (!ParseValue(member.value, depth + 1)) return false;
                _members.push_back(member);
                SkipSpace();
                if (_pos < _end && *_pos == ',') { ++_pos; continue; }
                if (_pos < _end && *_pos == '}') { ++_pos; break; }
                return Fail("expected ',' or '}'");
            }
        }
        size_t n = _members.size() - start;
        out.type = JType::Object;
        out.count = (uint32_t)n;
        if (n) {
            out.members = _arena.Make<JMember>(n);
            for (size_t i = 0; i < n; ++i) out.members[i] = _members[start + i];
        }
        _members.resize(start);
        return true;
    }
    if (c == '[') {
        ++_pos;
        size_t start = _items.size();
        SkipSpace();
        if (_pos < _end && *_pos == ']') {
            ++_pos;
        } else {
            for (;;) {
                JValue item;
                if (!ParseValue(item, depth + 1)) return false;
                _items.push_back(item);
                SkipSpace();
                if (_pos < _end && *_pos == ',') { ++_pos; continue; }
                if (_pos < _end && *_pos == ']') { ++_pos; break; }
                return Fail("expected ',' or ']'");
            }
        }
        size_t n = _items.size() - start;
        out.type = JType::Array;
        out.count = (uint32_t)n;
        if (n) {
            out.items = _arena.Make<JValue>(n);
            for (size_t i = 0; i < n; ++i) out.items[i] = _items[start + i];
        }
        _items.resize(start);
        return true;
    }
    if (c == '"') {
        ++_pos;
        out.type = JType::String;
        return ParseString(out.text, out.length);
    }
    if (c == 't' && _end - _pos >= 4 && std::memcmp(_pos, "true", 4) == 0) {
        _pos += 4;
        out.type = JType::Bool;
        out.boolean = true;
        return true;
    }
    if (c == 'f' && _end - _pos >= 5 && std::memcmp(_pos, "false", 5) == 0) {
        _pos += 5;
        out.type = JType::Bool;
        return true;
    }
    if (c == 'n' && _end - _pos >= 4 && std::memcmp(_pos, "null", 4) == 0) {
        _pos += 4;
        out.type = JType::Null;
        return true;
    }
    if (c == '-' || IsDigit(c)) {
        const char* start = _pos++;
        while (_pos < _end && (IsDigit(*_pos) || *_pos == '.' || *_pos == 'e' || *_pos == 'E' || *_pos == '+' || *_pos == '-')) ++_pos;
        out.type = JType::Number;
        out.text = start;
        out.length = (size_t)(_pos - start);
        return true;
    }
    return Fail("unexpected character");
}

} // namespace vkreplay
