#include "decode.h"

#include <cstring>

namespace vkreplay {

void DecodeContext::Problem(std::string message) {
    problems.push_back(where.empty() ? std::move(message) : where + ": " + message);
}

bool DecodeContext::Bool(const JValue* v) const {
    if (!v) return false;
    if (v->IsBool()) return v->boolean;
    return v->Uint() != 0;
}

int64_t DecodeContext::Int(const JValue* v) const { return v ? v->Int() : 0; }
uint64_t DecodeContext::Uint(const JValue* v) const { return v ? v->Uint() : 0; }
double DecodeContext::Double(const JValue* v) const { return v ? v->Double() : 0.0; }

uint64_t DecodeContext::Handle(const JValue* v) {
    if (!v || v->IsNull()) return 0;
    const JValue* cls = v->Get("__class");
    std::string_view className = cls ? cls->Str() : std::string_view("object");
    if (const JValue* id = v->Get("__id")) {
        uint64_t handle = resolve ? resolve(id->Uint(), className) : 0;
        if (!handle && !(quiet && quiet(id->Uint(), className))) {
            Problem("no replayed " + std::string(className) + " for object " + std::to_string(id->Uint()));
            ++unresolved;
        }
        return handle;
    }
    if (v->Get("__handle")) {
        Problem("a " + std::string(className) + " the capture did not track");
        ++unresolved;
        return 0;
    }
    return 0;
}

const char* DecodeContext::String(const JValue* v) {
    if (!v || !v->IsString()) return nullptr;
    char* s = static_cast<char*>(arena.Alloc(v->length + 1, 1));
    std::memcpy(s, v->text, v->length);
    return s;
}

const char* const* DecodeContext::Strings(const JValue* v) {
    if (!v || !v->IsArray()) return nullptr;
    auto** out = arena.Make<const char*>(v->count);
    for (uint32_t i = 0; i < v->count; ++i) out[i] = String(&v->items[i]);
    return out;
}

void DecodeContext::FixedString(const JValue* v, char* dst, size_t size) {
    if (!v || !v->IsString() || size == 0) return;
    size_t n = v->length < size - 1 ? v->length : size - 1;
    std::memcpy(dst, v->text, n);
    dst[n] = 0;
}

const void* DecodeContext::Bytes(const JValue* v) {
    if (!v || v->IsNull()) return nullptr;
    size_t size = (size_t)Uint(v->Get("__bytes"));
    const JValue* b64 = v->Get("base64");
    auto* out = static_cast<uint8_t*>(arena.Alloc(size, 1));
    if (!b64) {
        Problem(std::to_string(size) + " bytes of data the capture did not keep");
        return out;
    }
    std::vector<uint8_t> bytes;
    if (!DecodeBase64(b64->Str(), bytes)) {
        Problem("malformed base64 data");
        return out;
    }
    std::memcpy(out, bytes.data(), bytes.size() < size ? bytes.size() : size);
    return out;
}

int64_t DecodeContext::Enum(const JValue* v, const EnumEntry* table, size_t count, const char* type) {
    if (!v || v->IsNull()) return 0;
    if (v->IsNumber()) return v->Int();
    int64_t value = 0;
    if (v->IsString() && LookupEnum(table, count, v->Str(), value)) return value;
    Problem(std::string("unknown ") + type + " value " + std::string(v->Str()));
    return 0;
}

uint64_t DecodeContext::Flags(const JValue* v, const EnumEntry* table, size_t count, const char* type) {
    if (!v || v->IsNull()) return 0;
    if (v->IsNumber()) return v->Uint();
    std::string_view s = v->Str();
    uint64_t value = 0;
    while (!s.empty()) {
        size_t bar = s.find('|');
        std::string_view token = s.substr(0, bar);
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        if (!token.empty()) {
            int64_t bit = 0;
            if (token[0] >= '0' && token[0] <= '9') {
                JValue number;
                number.type = JType::Number;
                number.text = token.data();
                number.length = token.size();
                value |= number.Uint();
            } else if (LookupEnum(table, count, token, bit)) {
                value |= (uint64_t)bit;
            } else {
                Problem(std::string("unknown ") + type + " bit " + std::string(token));
            }
        }
        if (bar == std::string_view::npos) break;
        s.remove_prefix(bar + 1);
    }
    return value;
}

bool LookupEnum(const EnumEntry* table, size_t count, std::string_view name, int64_t& value) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int cmp = name.compare(table[mid].name);
        if (cmp == 0) {
            value = table[mid].value;
            return true;
        }
        if (cmp > 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

bool DecodeBase64(std::string_view text, std::vector<uint8_t>& out) {
    auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    out.reserve(text.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') break;
        int v = sextet(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

} // namespace vkreplay
