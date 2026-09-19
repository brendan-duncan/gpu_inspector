// Reads the capture library's JSON back into D3D12 structs: the visitor of dx_reflect.h that fills.
// Arrays and strings go into the replay's arena; objects, GPU addresses and descriptor handles are
// resolved to the replay's own through DecodeEnv, which counts what could not be.
#pragma once

#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "arena.h"
#include "json.h"

#include "dx_reflect.h"

namespace dxreplay {

using vkreplay::Arena;
using vkreplay::JValue;

/** A flags value from "A | B | 0x40", a name, or a number. */
uint64_t ParseFlags(const JValue* v, const dxinsp::EnumEntry* table, size_t count);
/** An enum value from its name or a number. */
int64_t ParseEnum(const JValue* v, const dxinsp::EnumEntry* table, size_t count);

struct DecodeEnv {
    explicit DecodeEnv(Arena& arena) : arena(arena) {}
    Arena& arena;
    /** The replay's object for a capture id, or null. */
    std::function<IUnknown*(uint64_t id)> object;
    /** The replay's GPU address of a captured buffer's byte; 0 when the buffer is not replayed. */
    std::function<D3D12_GPU_VIRTUAL_ADDRESS(uint64_t bufferId, uint64_t offset)> address;
    /** The replay's CPU handle of a slot of a captured descriptor heap; ptr 0 when the heap is not replayed. */
    std::function<D3D12_CPU_DESCRIPTOR_HANDLE(uint64_t heapId, uint32_t index)> cpuHandle;
    /** The bytecode the capture keeps for a stage of the object being decoded. */
    std::function<bool(const char* stage, const uint8_t*& data, size_t& size)> bytecode;
    /** References that resolved to nothing: what names them is left out rather than handed a null. */
    size_t unresolved = 0;
    std::vector<std::string> problems;
    std::string where;

    void Problem(const std::string& message) { problems.push_back(where.empty() ? message : where + ": " + message); }
};

inline uint64_t IdOf(const JValue* v) {
    const JValue* id = v ? v->Get("__id") : nullptr;
    return id ? id->Uint() : 0;
}

class Decoder {
public:
    Decoder(const JValue* json, DecodeEnv& env) : _j(json), _env(env) {}

    template <typename T> void Int(const char* n, T& f) {
        const JValue* v = Get(n);
        if (!v) return;
        if (v->IsBool()) f = (T)(v->boolean ? 1 : 0);
        else if (std::is_signed_v<T>) f = (T)v->Int();
        else f = (T)v->Uint();
    }
    void Float(const char* n, float& f) { if (const JValue* v = Get(n)) f = (float)v->Double(); }
    void Bool(const char* n, BOOL& f) { if (const JValue* v = Get(n)) f = (v->IsBool() ? v->boolean : v->Uint() != 0) ? TRUE : FALSE; }
    template <typename T> void Enum(const char* n, T& f, const dxinsp::EnumEntry* t, size_t c, const char*) {
        if (const JValue* v = Get(n)) f = (T)ParseEnum(v, t, c);
    }
    template <typename T> void Flags(const char* n, T& f, const dxinsp::EnumEntry* t, size_t c, const char*) {
        if (const JValue* v = Get(n)) f = (T)ParseFlags(v, t, c);
    }
    template <typename T> void Struct(const char* n, T& f) {
        if (const JValue* v = Get(n)) {
            Decoder d(v, _env);
            Reflect(d, f);
        }
    }
    Decoder Nested(const char* n) { return Decoder(Get(n), _env); }

    template <typename T> void Object(const char* n, T*& f, const char* cls) {
        const JValue* v = Get(n);
        if (!v) return;
        const uint64_t id = IdOf(v);
        IUnknown* object = id && _env.object ? _env.object(id) : nullptr;
        if (!object) {
            _env.Problem(std::string("no replayed ") + cls + (id ? " for object " + std::to_string(id) : " (the capture did not track it)"));
            ++_env.unresolved;
        }
        f = static_cast<T*>(object);
    }

    template <typename T, typename C> void Array(const char* n, const T*& p, const char*, C& count) {
        const JValue* v = Get(n);
        if (!v || !v->IsArray() || !v->count) { p = nullptr; count = 0; return; }
        T* items = _env.arena.Make<T>(v->count);
        for (uint32_t i = 0; i < v->count; ++i) {
            Decoder d(&v->items[i], _env);
            Reflect(d, items[i]);
        }
        p = items;
        count = (C)v->count;
    }
    template <typename T, typename C> void Ints(const char* n, const T*& p, const char*, C& count, const char*) {
        const JValue* v = Get(n);
        if (!v || !v->IsArray() || !v->count) { p = nullptr; count = 0; return; }
        T* items = _env.arena.Make<T>(v->count);
        for (uint32_t i = 0; i < v->count; ++i) items[i] = (T)v->items[i].Uint();
        p = items;
        count = (C)v->count;
    }
    void FixedFloats(const char* n, float* f, size_t size) {
        const JValue* v = Get(n);
        for (uint32_t i = 0; v && v->IsArray() && i < v->count && i < size; ++i) f[i] = (float)v->items[i].Double();
    }
    template <typename T> void FixedStructs(const char* n, T* f, size_t size) {
        const JValue* v = Get(n);
        for (uint32_t i = 0; v && v->IsArray() && i < v->count && i < size; ++i) {
            Decoder d(&v->items[i], _env);
            Reflect(d, f[i]);
        }
    }
    template <typename T, typename C> void FixedEnums(const char* n, T* f, size_t size, C&, const dxinsp::EnumEntry* t, size_t c, const char*) {
        const JValue* v = Get(n);
        for (uint32_t i = 0; v && v->IsArray() && i < v->count && i < size; ++i) f[i] = (T)ParseEnum(&v->items[i], t, c);
    }
    void String(const char* n, LPCSTR& f) {
        const JValue* v = Get(n);
        if (!v || !v->IsString()) return;
        char* s = static_cast<char*>(_env.arena.Alloc(v->length + 1, 1));
        std::memcpy(s, v->text, v->length);
        f = s;
    }
    void Address(const char* n, D3D12_GPU_VIRTUAL_ADDRESS& f) { f = AddressOf(Get(n)); }
    void CpuHandle(const char* n, D3D12_CPU_DESCRIPTOR_HANDLE& f) { f = HandleOf(Get(n)); }
    void ComponentMapping(const char* n, UINT& f);
    void Bytecode(const char*, D3D12_SHADER_BYTECODE& f, const char* stage) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        // A stage the pipeline has is one the capture keeps the code of, under the stage's name.
        if (_env.bytecode && _env.bytecode(stage, data, size)) f = {data, size};
    }

    /** {address, buffer, offset} as the replay's address; 0 (and unresolved) when the buffer is unknown. */
    D3D12_GPU_VIRTUAL_ADDRESS AddressOf(const JValue* v);
    /** {heap, index} as the replay's handle. */
    D3D12_CPU_DESCRIPTOR_HANDLE HandleOf(const JValue* v);
    const JValue* Get(const char* n) const {
        const JValue* v = _j ? _j->Get(n) : nullptr;
        return v && !v->IsNull() ? v : nullptr;
    }
    const JValue* Json() const { return _j; }
    DecodeEnv& Env() { return _env; }

private:
    const JValue* _j;
    DecodeEnv& _env;
};

/** Decodes a struct the arguments hold under `name`; false when it is absent or null. */
template <typename T>
bool DecodeStruct(DecodeEnv& env, const JValue& args, const char* name, T& out) {
    Decoder d(&args, env);
    if (!d.Get(name)) return false;
    d.Struct(name, out);
    return true;
}

} // namespace dxreplay
