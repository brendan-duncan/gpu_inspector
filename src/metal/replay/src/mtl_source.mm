#include "mtl_source.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mtlreplay {

void Source::Line(const std::string& statement) {
    text += std::string((size_t)indent * 4, ' ') + statement + "\n";
    _lines += 1 + (size_t)std::count(statement.begin(), statement.end(), '\n');
    ++_statements;
}

std::string Source::Local(const std::string& hint) { return hint + "_" + std::to_string(++_locals); }

void Source::Append(const Source& other) {
    text += other.text;
    _lines += other._lines;
    _statements += other._statements;
    for (const std::string& n : other.notes) Note(n);
}

std::string Source::Object(id object) {
    if (object == nil) return "nil";
    const std::string name = objectName ? objectName(object) : std::string();
    if (name.empty()) {
        Note("an object the exporter has no name for was spelled as nil");
        return "nil /* an object the export has no name for */";
    }
    return name;
}

std::string Source::Enum(const EnumTable& table, int64_t value) {
    if (const char* name = mtlinsp::EnumName(table.entries, table.count, value)) return name;
    return "(" + std::string(table.type) + ")" + std::to_string(value);
}

std::string Source::Flags(const EnumTable& table, uint64_t value) {
    // An exact entry first: Metal spells "no flags" with a name of its own (MTLTextureUsageUnknown,
    // MTLResourceOptionsDefault), and a combination sometimes has one too.
    if (const char* exact = mtlinsp::EnumName(table.entries, table.count, (int64_t)value)) return exact;
    std::string out;
    uint64_t rest = value;
    for (size_t i = 0; i < table.count && rest; ++i) {
        const uint64_t bit = (uint64_t)table.entries[i].value;
        if (!bit || (bit & (bit - 1)) || !(rest & bit)) continue;   // single bits, still unspelled
        out += (out.empty() ? "" : " | ") + std::string(table.entries[i].name);
        rest &= ~bit;
    }
    if (rest || out.empty()) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "(%s)0x%llx", table.type, (unsigned long long)rest);
        out += (out.empty() ? "" : " | ") + std::string(buf);
    }
    return out;
}

std::string Source::Uint(uint64_t v) {
    if (v == UINT32_MAX) return "UINT32_MAX";
    if (v == UINT64_MAX) return "UINT64_MAX";
    return std::to_string(v) + (v > UINT32_MAX ? "ull" : "");
}

std::string Source::Float(double v) {
    if (std::isnan(v)) return "NAN";
    if (std::isinf(v)) return v > 0 ? "INFINITY" : "-INFINITY";
    // The sampler's default lodMaxClamp, which is FLT_MAX written out in full.
    if (v == (double)3.402823466e+38f) return "FLT_MAX";
    char buf[48];
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s + "f";
}

std::string Source::NSString(std::string_view s, bool present) {
    if (!present) return "nil";
    std::string out = "@\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\%03o", c);
            out += esc;
        } else {
            // UTF-8 bytes pass through: the source is written as UTF-8 and read back as UTF-8.
            out += (char)c;
        }
    }
    return out + "\"";
}

std::string Source::Size(uint64_t w, uint64_t h, uint64_t d) {
    return "MTLSizeMake(" + Uint(w) + ", " + Uint(h) + ", " + Uint(d) + ")";
}

std::string Source::Origin(uint64_t x, uint64_t y, uint64_t z) {
    return "MTLOriginMake(" + Uint(x) + ", " + Uint(y) + ", " + Uint(z) + ")";
}

} // namespace mtlreplay
