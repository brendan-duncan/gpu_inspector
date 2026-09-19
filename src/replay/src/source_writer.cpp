#include "source_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vkreplay {

void SourceWriter::Line(const std::string& statement) {
    text += std::string((size_t)indent * 4, ' ') + statement + "\n";
    _lines += 1 + (size_t)std::count(statement.begin(), statement.end(), '\n');
    ++_statements;
}

void SourceWriter::Comment(const std::string& c) { Line("// " + c); }

void SourceWriter::Note(const std::string& n) {
    if (notes.size() < 1000) notes.push_back(n);
}

void SourceWriter::ResetPart() {
    text.clear();
    _lines = 0;
    _statements = 0;
    _locals = 0;
}

void SourceWriter::Append(const SourceWriter& other) {
    text += other.text;
    _lines += other._lines;
    _statements += other._statements;
    used.insert(other.used.begin(), other.used.end());
    for (const std::string& n : other.notes) Note(n);
}

std::string SourceWriter::Local(const char* hint) { return std::string(hint) + "_" + std::to_string(++_locals); }

std::string SourceWriter::Enum(const EnumEntry* table, size_t count, int64_t value, const char* type) const {
    for (size_t i = 0; i < count; ++i)
        if (table[i].value == value) return table[i].name;
    return EnumNumber(value, type);
}

std::string SourceWriter::EnumNumber(int64_t value, const char* type) const {
    return "(" + std::string(type) + ")" + std::to_string(value);
}

std::string SourceWriter::Flags(const EnumEntry* table, size_t count, uint64_t value, const char* type) const {
    (void)type;
    if (!value) return "0";
    // One name for the whole value where there is one (VK_CULL_MODE_FRONT_AND_BACK), else the
    // bits by name, then whatever is left as a number.
    for (size_t i = 0; i < count; ++i)
        if ((uint64_t)table[i].value == value) return table[i].name;
    std::string out;
    uint64_t rest = value;
    for (size_t i = 0; i < count && rest; ++i) {
        const uint64_t bit = (uint64_t)table[i].value;
        if (!bit || (bit & (bit - 1)) || !(rest & bit)) continue;   // single bits only, still unspelled
        if (!out.empty()) out += " | ";
        out += table[i].name;
        rest &= ~bit;
    }
    if (rest) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s0x%llx", out.empty() ? "" : " | ", (unsigned long long)rest);
        out += buf;
    }
    return out;
}

std::string SourceWriter::Handle(const char* type, uint64_t value) {
    if (!value) return "VK_NULL_HANDLE";
    std::string name = handle ? handle(type, value) : std::string();
    if (name.empty()) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)value);
        Note(std::string("a ") + type + " (" + buf + ") the exporter has no name for was spelled as VK_NULL_HANDLE");
        return std::string("VK_NULL_HANDLE /* ") + type + " " + buf + " */";
    }
    return name;
}

std::string SourceWriter::Address(uint64_t value) {
    if (!value) return "0";
    if (address) {
        std::string expr = address(value);
        if (!expr.empty()) return expr;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "0x%llxull", (unsigned long long)value);
    return buf;
}

std::string SourceWriter::Bytes(const void* bytes, size_t size) {
    if (!bytes || !size) return "nullptr";
    return data ? data(bytes, size) : "nullptr";
}

std::string SourceWriter::String(const char* s) {
    if (!s) return "nullptr";
    std::string out = "\"";
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            out += '\\';
            out += (char)*p;
        } else if (*p < 0x20 || *p >= 0x7f) {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\%03o", *p);
            out += esc;
        } else {
            out += (char)*p;
        }
    }
    return out + "\"";
}

std::string SourceWriter::FixedString(const char* s, size_t size) {
    std::string copy(s, strnlen(s, size));
    return String(copy.c_str());
}

std::string SourceWriter::Strings(const char* hint, const char* const* strings, size_t count) {
    if (!strings || !count) return "nullptr";
    std::string body;
    for (size_t i = 0; i < count; ++i) body += (i ? ", " : "") + String(strings[i]);
    return EmitArrayLocal(*this, "const char* const", hint, body);
}

std::string SourceWriter::FloatComment(const float* values, size_t count) {
    std::string out = " /* as floats: ";
    for (size_t i = 0; i < count; ++i) out += (i ? ", " : "") + Float(values[i]);
    return out + " */";
}

std::string SourceWriter::PNextCast(const std::string& expr, bool constMember) {
    if (constMember || expr == "nullptr") return expr;
    return "(void*)" + expr;
}

std::string SourceWriter::Uint(uint64_t v) {
    // Sentinels read better by name than as their bit pattern.
    if (v == UINT64_MAX) return "VK_WHOLE_SIZE";
    if (v == UINT32_MAX) return "UINT32_MAX";
    return std::to_string(v) + (v > UINT32_MAX ? "ull" : "");
}

std::string SourceWriter::Int(int64_t v) {
    if (v == INT64_MIN) return "INT64_MIN";
    return std::to_string(v) + (v > INT32_MAX || v < INT32_MIN ? "ll" : "");
}

std::string SourceWriter::Float(float v) {
    if (std::isnan(v)) return "NAN";
    if (std::isinf(v)) return v > 0 ? "INFINITY" : "-INFINITY";
    // Shortest text that reads back to the same float.
    char buf[40];
    for (int precision = 6; precision <= 9; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtof(buf, nullptr) == v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s + "f";
}

std::string SourceWriter::Double(double v) {
    if (std::isnan(v)) return "NAN";
    if (std::isinf(v)) return v > 0 ? "INFINITY" : "-INFINITY";
    char buf[40];
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string EmitLocal(SourceWriter& w, const char* type, const char* hint, const std::string& expr) {
    const std::string name = w.Local(hint);
    w.Line("const " + std::string(type) + " " + name + " = " + expr + ";");
    return name;
}

std::string EmitArrayLocal(SourceWriter& w, const char* type, const char* hint, const std::string& items) {
    const std::string name = w.Local(hint);
    w.Line("const " + std::string(type) + " " + name + "[] = {" + items + "};");
    return name;
}

} // namespace vkreplay
