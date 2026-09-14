// The JSON argument builder every hook uses, and the writers for the two D3D12 values that are
// not objects but must be readable: descriptor handles and GPU virtual addresses.
//
// Every hook that records or tracks builds an Args in the order it received its parameters, under
// the parameter's D3D12 name, and hands `str()` to the recorder or the tracker. The writer is
// exposed for nested structure; the struct serializers in serialize.h write into it.
#pragma once

#include "common.h"
#include "tracker.h"

#include <string>

namespace dxinsp {

/** A descriptor handle resolved to its heap and slot: {"heap": {ref}, "index": N} or {"ptr": "0x.."} (descriptors.h). */
void WriteCpuHandle(JsonWriter& w, D3D12_CPU_DESCRIPTOR_HANDLE handle);
void WriteGpuHandle(JsonWriter& w, D3D12_GPU_DESCRIPTOR_HANDLE handle);
/** A GPU virtual address resolved to the buffer holding it: {"address": "0x..", "buffer": {ref}, "offset": N} (descriptors.h). */
void WriteGpuAddress(JsonWriter& w, D3D12_GPU_VIRTUAL_ADDRESS address);

class Args {
public:
    Args() : w_(&Tracker::Get()) { w_.BeginObject(); }
    Args& u(const char* key, uint64_t value) { w_.Key(key); w_.Uint(value); return *this; }
    Args& i(const char* key, int64_t value) { w_.Key(key); w_.Int(value); return *this; }
    Args& d(const char* key, double value) { w_.Key(key); w_.Double(value); return *this; }
    Args& b(const char* key, bool value) { w_.Key(key); w_.Boolean(value); return *this; }
    Args& s(const char* key, const char* value) { w_.Key(key); if (value) w_.String(value); else w_.Null(); return *this; }
    Args& s(const char* key, const std::string& value) { w_.Key(key); w_.String(value); return *this; }
    Args& ws(const char* key, const wchar_t* value) { w_.Key(key); if (value) w_.String(Narrow(value)); else w_.Null(); return *this; }
    /** An enum by its generated name, the number when the table has none. */
    Args& e(const char* key, const char* name, int64_t value) { w_.Key(key); w_.Enum(name, value); return *this; }
    Args& ref(const char* key, const void* object, const char* type) { w_.Key(key); WriteRef(w_, object, type); return *this; }
    Args& ptr(const char* key, const void* p) { w_.Key(key); w_.Pointer(p); return *this; }
    Args& bytes(const char* key, const void* data, size_t length) { w_.Key(key); w_.Bytes(data, length); return *this; }
    Args& cpuHandle(const char* key, D3D12_CPU_DESCRIPTOR_HANDLE h) { w_.Key(key); WriteCpuHandle(w_, h); return *this; }
    Args& gpuHandle(const char* key, D3D12_GPU_DESCRIPTOR_HANDLE h) { w_.Key(key); WriteGpuHandle(w_, h); return *this; }
    Args& address(const char* key, D3D12_GPU_VIRTUAL_ADDRESS a) { w_.Key(key); WriteGpuAddress(w_, a); return *this; }
    Args& raw(const char* key, const std::string& json) { w_.Key(key); if (json.empty()) w_.Null(); else w_.Raw(json); return *this; }
    Args& null(const char* key) { w_.Key(key); w_.Null(); return *this; }
    /** Positions the writer at `key` for a nested value written through writer(). */
    JsonWriter& key(const char* key) { w_.Key(key); return w_; }
    JsonWriter& writer() { return w_; }
    /** Closes the object. The Args is spent afterwards. */
    std::string str() { w_.EndObject(); return std::move(w_.str()); }

private:
    JsonWriter w_;
};

}  // namespace dxinsp
