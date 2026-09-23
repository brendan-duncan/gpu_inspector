#include "dx_source.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dxreplay
{

void Source::Line(const std::string& statement)
{
    text += std::string((size_t)indent * 4, ' ') + statement + "\n";
    _lines += 1 + (size_t)std::count(statement.begin(), statement.end(), '\n');
    ++_statements;
}

std::string Source::Local(const std::string& hint) { return hint + "_" + std::to_string(++_locals); }

void Source::Append(const Source& other)
{
    text += other.text;
    _lines += other._lines;
    _statements += other._statements;
    for (const std::string& n : other.notes)
        Note(n);
}

std::string Source::Object(IUnknown* object)
{
    if (!object)
        return "nullptr";
    std::string name = objectName ? objectName(object) : std::string();
    if (name.empty())
    {
        Note("an object the exporter has no name for was spelled as nullptr");
        return "nullptr /* an object the export has no name for */";
    }
    return name;
}

std::string Source::Address(D3D12_GPU_VIRTUAL_ADDRESS address)
{
    if (!address)
        return "0";
    std::string expr = addressExpr ? addressExpr(address) : std::string();
    if (expr.empty())
    {
        Note("a GPU address in no exported buffer was spelled as 0");
        return "0 /* an address in no exported buffer */";
    }
    return expr;
}

std::string Source::CpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    std::string expr = handle.ptr && cpuHandleExpr ? cpuHandleExpr(handle) : std::string();
    if (expr.empty())
    {
        Note("a descriptor handle in no exported heap was spelled as a null handle");
        return "D3D12_CPU_DESCRIPTOR_HANDLE{}";
    }
    return expr;
}

std::string Source::Enum(const dxinsp::EnumEntry* table, size_t count, int64_t value, const char* type)
{
    for (size_t i = 0; i < count; ++i)
        if (table[i].value == value)
            return table[i].name;
    return "(" + std::string(type) + ")" + std::to_string(value);
}

std::string Source::Flags(const dxinsp::EnumEntry* table, size_t count, uint64_t value, const char* type)
{
    for (size_t i = 0; i < count; ++i)
        if ((uint64_t)table[i].value == value)
            return table[i].name;
    std::string out;
    uint64_t rest = value;
    for (size_t i = 0; i < count && rest; ++i)
    {
        const uint64_t bit = (uint64_t)table[i].value;
        if (!bit || (bit & (bit - 1)) || !(rest & bit))
            continue;   // single bits, still unspelled
        out += (out.empty() ? "" : " | ") + std::string(table[i].name);
        rest &= ~bit;
    }
    if (rest || out.empty())
    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "(%s)0x%llx", type, (unsigned long long)rest);
        out += (out.empty() ? "" : " | ") + std::string(buf);
    }
    return out;
}

std::string Source::Uint(uint64_t v)
{
    if (v == UINT32_MAX)
        return "UINT_MAX";
    if (v == UINT64_MAX)
        return "UINT64_MAX";
    return std::to_string(v) + (v > UINT32_MAX ? "ull" : "");
}

std::string Source::Float(float v)
{
    if (std::isnan(v))
        return "NAN";
    if (std::isinf(v))
        return v > 0 ? "INFINITY" : "-INFINITY";
    if (v == 3.402823466e+38f)
        return "D3D12_FLOAT32_MAX";
    char buf[40];
    for (int precision = 6; precision <= 9; ++precision)
    {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtof(buf, nullptr) == v)
            break;
    }
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos)
        s += ".0";
    return s + "f";
}

std::string Source::String(const char* s)
{
    if (!s)
        return "nullptr";
    std::string out = "\"";
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
    {
        if (*p == '"' || *p == '\\')
        {
            out += '\\';
            out += (char)*p;
        }
        else if (*p < 0x20 || *p >= 0x7f)
        {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\%03o", *p);
            out += esc;
        }
        else
        {
            out += (char)*p;
        }
    }
    return out + "\"";
}

} // namespace dxreplay
