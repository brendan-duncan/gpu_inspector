#include "dx_decode.h"

#include <cstdlib>

namespace dxreplay
{

namespace
{

bool Lookup(const dxinsp::EnumEntry* table, size_t count, std::string_view name, int64_t& value)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (name == table[i].name)
        {
            value = table[i].value;
            return true;
        }
    }
    return false;
}

} // namespace

int64_t ParseEnum(const JValue* v, const dxinsp::EnumEntry* table, size_t count)
{
    if (!v)
        return 0;
    if (v->IsNumber())
        return v->Int();
    int64_t value = 0;
    if (v->IsString() && Lookup(table, count, v->Str(), value))
        return value;
    // A value the table does not name is written as its number, sometimes as a string.
    return v->IsString() ? (int64_t)std::strtoll(std::string(v->Str()).c_str(), nullptr, 0) : 0;
}

uint64_t ParseFlags(const JValue* v, const dxinsp::EnumEntry* table, size_t count)
{
    if (!v)
        return 0;
    if (v->IsNumber())
        return v->Uint();
    std::string_view s = v->Str();
    uint64_t value = 0;
    while (!s.empty())
    {
        const size_t bar = s.find('|');
        std::string_view token = s.substr(0, bar);
        while (!token.empty() && token.front() == ' ')
            token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ')
            token.remove_suffix(1);
        int64_t bit = 0;
        if (!token.empty())
        {
            if (Lookup(table, count, token, bit))
                value |= (uint64_t)bit;
            else
                value |= std::strtoull(std::string(token).c_str(), nullptr, 0);
        }
        if (bar == std::string_view::npos)
            break;
        s.remove_prefix(bar + 1);
    }
    return value;
}

void Decoder::ComponentMapping(const char* n, UINT& f)
{
    const JValue* v = Get(n);
    f = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (!v)
        return;
    if (v->IsNumber())
    {
        f = (UINT)v->Uint();
        return;
    }
    if (!v->IsArray() || v->count != 4)
        return;
    UINT c[4] = {0, 1, 2, 3};
    for (uint32_t i = 0; i < 4; ++i)
        c[i] = (UINT)ParseEnum(&v->items[i], dxinsp::kEnum_D3D12_SHADER_COMPONENT_MAPPING, std::size(dxinsp::kEnum_D3D12_SHADER_COMPONENT_MAPPING));
    f = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(c[0], c[1], c[2], c[3]);
}

D3D12_GPU_VIRTUAL_ADDRESS Decoder::AddressOf(const JValue* v)
{
    if (!v || v->IsNull())
        return 0;
    const uint64_t buffer = IdOf(v->Get("buffer"));
    const uint64_t offset = v->Get("offset") ? v->Get("offset")->Uint() : 0;
    const D3D12_GPU_VIRTUAL_ADDRESS address = buffer && _env.address ? _env.address(buffer, offset) : 0;
    if (!address)
    {
        // The captured process's number means nothing here: only a buffer and an offset translate.
        _env.Problem(buffer ? "no replayed buffer for object " + std::to_string(buffer) : "a GPU address the capture could not tie to a buffer");
        ++_env.unresolved;
    }
    return address;
}

D3D12_CPU_DESCRIPTOR_HANDLE Decoder::HandleOf(const JValue* v)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!v || v->IsNull())
        return handle;
    const uint64_t heap = IdOf(v->Get("heap"));
    const uint32_t index = v->Get("index") ? (uint32_t)v->Get("index")->Uint() : 0;
    if (heap && _env.cpuHandle)
        handle = _env.cpuHandle(heap, index);
    if (!handle.ptr)
    {
        _env.Problem(heap ? "no replayed descriptor heap for object " + std::to_string(heap) : "a descriptor in no heap the capture tracked");
        ++_env.unresolved;
    }
    return handle;
}

} // namespace dxreplay
