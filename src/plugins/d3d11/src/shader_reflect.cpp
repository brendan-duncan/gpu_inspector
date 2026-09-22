#include "shader_reflect.h"

#include "../gen/d3d11_enums.gen.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace d3d11insp {

namespace {

// ---------------------------------------------------------------------------------------------
// The container

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

constexpr uint32_t kMagicDXBC = FourCC('D', 'X', 'B', 'C');
constexpr uint32_t kPartSHDR = FourCC('S', 'H', 'D', 'R');   // DXBC shader model 4 program
constexpr uint32_t kPartSHEX = FourCC('S', 'H', 'E', 'X');   // DXBC shader model 5 program

struct Part {
    uint32_t fourcc = 0;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// Header: "DXBC", 16-byte hash, u32 version, u32 total size, u32 part count, u32 part offsets;
// each part: fourcc, u32 size, data.
bool ParseContainer(const void* bytecode, size_t size, std::vector<Part>& parts) {
    if (!bytecode || size < 32) return false;
    const uint8_t* p = static_cast<const uint8_t*>(bytecode);
    if (ReadU32(p) != kMagicDXBC) return false;
    uint32_t total = ReadU32(p + 24);
    uint32_t count = ReadU32(p + 28);
    if (total < 32 || total > size) return false;
    if (count > (total - 32) / 4) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t offset = ReadU32(p + 32 + 4 * i);
        if (offset > total || total - offset < 8) return false;
        Part part;
        part.fourcc = ReadU32(p + offset);
        part.size = ReadU32(p + offset + 4);
        if (part.size > total - offset - 8) return false;
        part.data = p + offset + 8;
        parts.push_back(part);
    }
    return true;
}

const Part* FindPart(const std::vector<Part>& parts, uint32_t fourcc) {
    for (const Part& p : parts)
        if (p.fourcc == fourcc) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// The stage

struct StageNames {
    const char* ui;        // the inspector's stage name
    const char* profile;   // the target prefix
};

// Indexed by the DXBC program type.
constexpr StageNames kStages[] = {
    {"fragment", "ps"}, {"vertex", "vs"}, {"geometry", "gs"}, {"tess_control", "hs"}, {"tess_eval", "ds"}, {"compute", "cs"},
};

void SetStage(ShaderInfo& info, uint32_t kind, uint32_t major, uint32_t minor) {
    const size_t n = sizeof(kStages) / sizeof(kStages[0]);
    const StageNames names = kind < n ? kStages[kind] : StageNames{"unknown", "xs"};
    info.stage = names.ui;
    info.target = std::string(names.profile) + "_" + std::to_string(major) + "_" + std::to_string(minor);
}

// The stage and shader model of the program part: its first word is (type << 16) | (major << 4) | minor.
bool ProgramInfo(const std::vector<Part>& parts, ShaderInfo& info) {
    const Part* program = FindPart(parts, kPartSHEX);
    if (!program) program = FindPart(parts, kPartSHDR);
    if (program && program->size >= 4) {
        uint32_t v = ReadU32(program->data);
        SetStage(info, v >> 16, (v >> 4) & 0xf, v & 0xf);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// d3dcompiler_47.dll

typedef HRESULT(WINAPI* PFN_Reflect)(LPCVOID data, SIZE_T size, REFIID iid, void** reflector);

struct D3DCompilerLib {
    std::once_flag once;
    HMODULE module = nullptr;
    PFN_Reflect reflect = nullptr;
    std::string error;
};

D3DCompilerLib& D3DCompiler() {
    static D3DCompilerLib lib;
    std::call_once(lib.once, [] {
        lib.module = LoadLibraryW(L"d3dcompiler_47.dll");
        if (lib.module) lib.reflect = reinterpret_cast<PFN_Reflect>(GetProcAddress(lib.module, "D3DReflect"));
        if (!lib.reflect) {
            lib.error = "d3dcompiler_47.dll not found: it ships with Windows 8.1 and later";
            LogAlways("%s", lib.error.c_str());
        }
    });
    return lib;
}


// ---------------------------------------------------------------------------------------------
// ReflType

uint32_t RoundUp16(uint32_t v) { return (v + 15) & ~15u; }

struct ReflNode {
    std::string kind;   // scalar, vector, matrix, array, struct, opaque
    std::string base;   // scalar: float, int, uint, bool
    uint32_t width = 0;
    std::unique_ptr<ReflNode> element;   // vector, matrix, array
    uint32_t count = 0;                  // vector, array
    uint32_t columns = 0;                // matrix
    uint32_t rows = 0;
    uint32_t stride = 0;                 // matrix, array
    bool rowMajor = false;
    std::string name;                    // struct, opaque
    struct Member {
        std::string name;
        uint32_t offset = 0;
        std::unique_ptr<ReflNode> type;
    };
    std::vector<Member> members;
    uint32_t size = 0;
};

// The scalar as it is stored: the min16 types occupy a full 32-bit slot in a constant buffer
// unless 16-bit types are enabled, in which case the compiler reports FLOAT16/INT16/UINT16.
std::unique_ptr<ReflNode> ScalarNode(D3D_SHADER_VARIABLE_TYPE type, const char* typeName) {
    auto n = std::make_unique<ReflNode>();
    n->kind = "scalar";
    switch (type) {
        case D3D_SVT_BOOL: n->base = "bool"; n->width = 32; break;
        case D3D_SVT_INT: case D3D_SVT_MIN12INT: case D3D_SVT_MIN16INT: n->base = "int"; n->width = 32; break;
        case D3D_SVT_UINT: case D3D_SVT_MIN16UINT: n->base = "uint"; n->width = 32; break;
        case D3D_SVT_UINT8: n->base = "uint"; n->width = 8; break;
        case D3D_SVT_FLOAT: case D3D_SVT_MIN8FLOAT: case D3D_SVT_MIN10FLOAT: case D3D_SVT_MIN16FLOAT: n->base = "float"; n->width = 32; break;
        case D3D_SVT_DOUBLE: n->base = "float"; n->width = 64; break;
        case D3D_SVT_FLOAT16: n->base = "float"; n->width = 16; break;
        case D3D_SVT_INT16: n->base = "int"; n->width = 16; break;
        case D3D_SVT_UINT16: n->base = "uint"; n->width = 16; break;
        case D3D_SVT_INT64: n->base = "int"; n->width = 64; break;
        case D3D_SVT_UINT64: n->base = "uint"; n->width = 64; break;
        default: {
            n->kind = "opaque";
            const char* e = ToString_D3D_SHADER_VARIABLE_TYPE(type);
            n->name = typeName ? typeName : (e ? e : "unknown");
            return n;
        }
    }
    n->size = n->width / 8;
    return n;
}

// `tight` is the layout of a structured buffer's element (C-like packing: no 16-byte vector
// slots); a constant buffer follows the HLSL packing rules, where array elements and matrix
// vectors start on 16-byte boundaries. Member offsets come from the reflection itself either way.
std::unique_ptr<ReflNode> BuildType(ID3D11ShaderReflectionType* t, bool tight, bool ignoreElements = false) {
    D3D11_SHADER_TYPE_DESC d{};
    if (!t || FAILED(t->GetDesc(&d))) {
        auto n = std::make_unique<ReflNode>();
        n->kind = "opaque";
        n->name = "unknown";
        return n;
    }
    if (d.Elements > 0 && !ignoreElements) {
        auto n = std::make_unique<ReflNode>();
        n->kind = "array";
        n->element = BuildType(t, tight, true);
        n->count = d.Elements;
        n->stride = tight ? n->element->size : RoundUp16(n->element->size);
        n->size = (d.Elements - 1) * n->stride + n->element->size;
        return n;
    }
    switch (d.Class) {
        case D3D_SVC_SCALAR:
            return ScalarNode(d.Type, d.Name);
        case D3D_SVC_VECTOR: {
            auto n = std::make_unique<ReflNode>();
            n->kind = "vector";
            n->element = ScalarNode(d.Type, d.Name);
            n->count = d.Columns ? d.Columns : 1;
            n->size = n->count * n->element->size;
            return n;
        }
        case D3D_SVC_MATRIX_ROWS:
        case D3D_SVC_MATRIX_COLUMNS: {
            auto n = std::make_unique<ReflNode>();
            n->kind = "matrix";
            n->element = ScalarNode(d.Type, d.Name);
            n->columns = d.Columns ? d.Columns : 1;
            n->rows = d.Rows ? d.Rows : 1;
            n->rowMajor = d.Class == D3D_SVC_MATRIX_ROWS;
            // Column-major storage (HLSL's default) is Columns vectors of Rows elements each.
            uint32_t vectorLength = n->rowMajor ? n->columns : n->rows;
            uint32_t vectors = n->rowMajor ? n->rows : n->columns;
            n->stride = tight ? vectorLength * n->element->size : 16;
            n->size = (vectors - 1) * n->stride + vectorLength * n->element->size;
            return n;
        }
        case D3D_SVC_STRUCT: {
            auto n = std::make_unique<ReflNode>();
            n->kind = "struct";
            n->name = d.Name ? d.Name : "struct";
            for (UINT i = 0; i < d.Members; ++i) {
                ID3D11ShaderReflectionType* mt = t->GetMemberTypeByIndex(i);
                const char* mname = t->GetMemberTypeName(i);
                D3D11_SHADER_TYPE_DESC md{};
                if (!mt || FAILED(mt->GetDesc(&md))) continue;
                ReflNode::Member m;
                m.name = mname ? mname : "";
                m.offset = md.Offset;
                m.type = BuildType(mt, tight);
                n->size = std::max(n->size, m.offset + m.type->size);
                n->members.push_back(std::move(m));
            }
            return n;
        }
        default: {
            auto n = std::make_unique<ReflNode>();
            n->kind = "opaque";
            const char* e = ToString_D3D_SHADER_VARIABLE_TYPE(d.Type);
            n->name = d.Name ? d.Name : (e ? e : "object");
            return n;
        }
    }
}

void WriteNode(JsonWriter& w, const ReflNode& n) {
    w.BeginObject();
    w.Key("kind"); w.String(n.kind);
    if (n.kind == "scalar") {
        w.Key("base"); w.String(n.base);
        w.Key("width"); w.Uint(n.width);
        w.Key("size"); w.Uint(n.size);
    } else if (n.kind == "vector") {
        w.Key("element"); WriteNode(w, *n.element);
        w.Key("count"); w.Uint(n.count);
        w.Key("size"); w.Uint(n.size);
    } else if (n.kind == "matrix") {
        w.Key("element"); WriteNode(w, *n.element);
        w.Key("columns"); w.Uint(n.columns);
        w.Key("rows"); w.Uint(n.rows);
        w.Key("stride"); w.Uint(n.stride);
        w.Key("rowMajor"); w.Boolean(n.rowMajor);
        w.Key("size"); w.Uint(n.size);
    } else if (n.kind == "array") {
        w.Key("element"); WriteNode(w, *n.element);
        w.Key("count"); w.Uint(n.count);
        w.Key("stride"); w.Uint(n.stride);
        w.Key("size"); w.Uint(n.size);
    } else if (n.kind == "struct") {
        w.Key("name"); w.String(n.name);
        w.Key("members"); w.BeginArray();
        for (const ReflNode::Member& m : n.members) {
            w.BeginObject();
            w.Key("name"); w.String(m.name);
            w.Key("offset"); w.Uint(m.offset);
            w.Key("type"); WriteNode(w, *m.type);
            w.EndObject();
        }
        w.EndArray();
        w.Key("size"); w.Uint(n.size);
    } else {
        w.Key("name"); w.String(n.name);
    }
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Reflection JSON

const char* ResourceKind(D3D_SHADER_INPUT_TYPE t) {
    switch (t) {
        case D3D_SIT_CBUFFER: case D3D_SIT_TBUFFER: return "cbuffer";
        case D3D_SIT_SAMPLER: return "sampler";
        case D3D_SIT_TEXTURE: case D3D_SIT_STRUCTURED: case D3D_SIT_BYTEADDRESS: case D3D_SIT_RTACCELERATIONSTRUCTURE: return "srv";
        default: return "uav";
    }
}

bool IsStructured(D3D_SHADER_INPUT_TYPE t) {
    return t == D3D_SIT_STRUCTURED || t == D3D_SIT_UAV_RWSTRUCTURED || t == D3D_SIT_UAV_APPEND_STRUCTURED ||
           t == D3D_SIT_UAV_CONSUME_STRUCTURED || t == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
}

const char* TextureDimension(D3D_SRV_DIMENSION d) {
    switch (d) {
        case D3D_SRV_DIMENSION_BUFFER: case D3D_SRV_DIMENSION_BUFFEREX: return "buffer";
        case D3D_SRV_DIMENSION_TEXTURE1D: return "texture1d";
        case D3D_SRV_DIMENSION_TEXTURE1DARRAY: return "texture1darray";
        case D3D_SRV_DIMENSION_TEXTURE2D: return "texture2d";
        case D3D_SRV_DIMENSION_TEXTURE2DARRAY: return "texture2darray";
        case D3D_SRV_DIMENSION_TEXTURE2DMS: return "texture2dms";
        case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: return "texture2dmsarray";
        case D3D_SRV_DIMENSION_TEXTURE3D: return "texture3d";
        case D3D_SRV_DIMENSION_TEXTURECUBE: return "texturecube";
        case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: return "texturecubearray";
        default: return "unknown";
    }
}

const char* Dimension(const D3D11_SHADER_INPUT_BIND_DESC& b) {
    switch (b.Type) {
        case D3D_SIT_CBUFFER: case D3D_SIT_TBUFFER: return "buffer";
        case D3D_SIT_SAMPLER: return "sampler";
        case D3D_SIT_BYTEADDRESS: case D3D_SIT_UAV_RWBYTEADDRESS: return "byteaddress";
        case D3D_SIT_RTACCELERATIONSTRUCTURE: return "accelerationStructure";
        case D3D_SIT_TEXTURE: case D3D_SIT_UAV_RWTYPED: case D3D_SIT_UAV_FEEDBACKTEXTURE: return TextureDimension(b.Dimension);
        default: return IsStructured(b.Type) ? "structured" : "buffer";
    }
}

const char* ReturnTypeName(D3D_RESOURCE_RETURN_TYPE t) {
    switch (t) {
        case D3D_RETURN_TYPE_UNORM: return "unorm";
        case D3D_RETURN_TYPE_SNORM: return "snorm";
        case D3D_RETURN_TYPE_SINT: return "sint";
        case D3D_RETURN_TYPE_UINT: return "uint";
        case D3D_RETURN_TYPE_FLOAT: return "float";
        case D3D_RETURN_TYPE_MIXED: return "mixed";
        case D3D_RETURN_TYPE_DOUBLE: return "double";
        case D3D_RETURN_TYPE_CONTINUED: return "continued";
        default: return nullptr;
    }
}

// The reflection's constant buffer of a binding: a cbuffer/tbuffer by name, or the resource bind
// info (type D3D_CT_RESOURCE_BIND_INFO) describing a structured buffer's element. The two can
// share a name, so the type is matched too.
ID3D11ShaderReflectionConstantBuffer* FindBuffer(ID3D11ShaderReflection* r, const D3D11_SHADER_DESC& sd, const char* name, bool bindInfo) {
    if (!name) return nullptr;
    for (UINT i = 0; i < sd.ConstantBuffers; ++i) {
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC cd{};
        if (!cb || FAILED(cb->GetDesc(&cd)) || !cd.Name || strcmp(cd.Name, name) != 0) continue;
        bool isBindInfo = cd.Type == D3D_CT_RESOURCE_BIND_INFO;
        if (isBindInfo == bindInfo) return cb;
    }
    return nullptr;
}

void WriteConstantBufferType(JsonWriter& w, ID3D11ShaderReflectionConstantBuffer* cb) {
    D3D11_SHADER_BUFFER_DESC cd{};
    if (!cb || FAILED(cb->GetDesc(&cd))) { w.Null(); return; }
    ReflNode n;
    n.kind = "struct";
    n.name = cd.Name ? cd.Name : "cbuffer";
    n.size = cd.Size;
    for (UINT i = 0; i < cd.Variables; ++i) {
        ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(i);
        D3D11_SHADER_VARIABLE_DESC vd{};
        if (!var || FAILED(var->GetDesc(&vd))) continue;
        ReflNode::Member m;
        m.name = vd.Name ? vd.Name : "";
        m.offset = vd.StartOffset;
        m.type = BuildType(var->GetType(), false);
        n.members.push_back(std::move(m));
    }
    WriteNode(w, n);
}

void WriteStructuredElementType(JsonWriter& w, ID3D11ShaderReflectionConstantBuffer* cb) {
    D3D11_SHADER_BUFFER_DESC cd{};
    if (!cb || FAILED(cb->GetDesc(&cd)) || cd.Variables == 0) { w.Null(); return; }
    // The bind info holds one variable, "$Element", whose type is the element.
    ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(0);
    if (!var) { w.Null(); return; }
    std::unique_ptr<ReflNode> n = BuildType(var->GetType(), true);
    WriteNode(w, *n);
}

void WriteSignatureType(JsonWriter& w, const D3D11_SIGNATURE_PARAMETER_DESC& p) {
    ReflNode scalar;
    scalar.kind = "scalar";
    switch (p.ComponentType) {
        case D3D_REGISTER_COMPONENT_UINT32: scalar.base = "uint"; scalar.width = 32; break;
        case D3D_REGISTER_COMPONENT_SINT32: scalar.base = "int"; scalar.width = 32; break;
        case D3D_REGISTER_COMPONENT_FLOAT32: scalar.base = "float"; scalar.width = 32; break;
        case D3D_REGISTER_COMPONENT_UINT16: scalar.base = "uint"; scalar.width = 16; break;
        case D3D_REGISTER_COMPONENT_SINT16: scalar.base = "int"; scalar.width = 16; break;
        case D3D_REGISTER_COMPONENT_FLOAT16: scalar.base = "float"; scalar.width = 16; break;
        case D3D_REGISTER_COMPONENT_UINT64: scalar.base = "uint"; scalar.width = 64; break;
        case D3D_REGISTER_COMPONENT_SINT64: scalar.base = "int"; scalar.width = 64; break;
        case D3D_REGISTER_COMPONENT_FLOAT64: scalar.base = "float"; scalar.width = 64; break;
        default: scalar.base = "float"; scalar.width = 32; break;
    }
    scalar.size = scalar.width / 8;
    uint32_t count = 0;
    for (uint32_t m = p.Mask; m; m >>= 1) count += m & 1;
    if (count <= 1) {
        WriteNode(w, scalar);
        return;
    }
    ReflNode v;
    v.kind = "vector";
    v.count = count;
    v.size = count * scalar.size;
    v.element = std::make_unique<ReflNode>(std::move(scalar));
    WriteNode(w, v);
}

void WriteSignature(JsonWriter& w, ID3D11ShaderReflection* r, UINT count, bool inputs) {
    w.BeginArray();
    for (UINT i = 0; i < count; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        HRESULT hr = inputs ? r->GetInputParameterDesc(i, &p) : r->GetOutputParameterDesc(i, &p);
        if (FAILED(hr)) continue;
        std::string semantic = p.SemanticName ? p.SemanticName : "";
        w.BeginObject();
        // TEXCOORD1 rather than TEXCOORD twice: the name is what the UI lists.
        w.Key("name"); w.String(p.SemanticIndex ? semantic + std::to_string(p.SemanticIndex) : semantic);
        w.Key("semantic"); w.String(semantic);
        w.Key("index"); w.Uint(p.SemanticIndex);
        w.Key("register"); w.Uint(p.Register);
        if (p.SystemValueType != D3D_NAME_UNDEFINED) {
            const char* sv = ToString_D3D_NAME(p.SystemValueType);
            w.Key("systemValue"); if (sv) w.String(sv); else w.Uint(p.SystemValueType);
        }
        w.Key("type"); WriteSignatureType(w, p);
        w.EndObject();
    }
    w.EndArray();
}

std::string BuildReflectionJson(ID3D11ShaderReflection* r, const ShaderInfo& info) {
    D3D11_SHADER_DESC sd{};
    if (FAILED(r->GetDesc(&sd))) return std::string();
    JsonWriter w;
    w.BeginObject();
    w.Key("stage"); w.String(info.stage);
    w.Key("target"); w.String(info.target);
    w.Key("instructionCount"); w.Uint(sd.InstructionCount);
    w.Key("inputs"); WriteSignature(w, r, sd.InputParameters, true);
    w.Key("outputs"); WriteSignature(w, r, sd.OutputParameters, false);
    if (info.stage == "compute" || info.stage == "mesh" || info.stage == "task") {
        UINT x = 0, y = 0, z = 0;
        r->GetThreadGroupSize(&x, &y, &z);
        w.Key("threadGroupSize"); w.BeginArray(); w.Uint(x); w.Uint(y); w.Uint(z); w.EndArray();
    }
    w.Key("resources"); w.BeginArray();
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        if (FAILED(r->GetResourceBindingDesc(i, &b))) continue;
        w.BeginObject();
        w.Key("kind"); w.String(ResourceKind(b.Type));
        w.Key("name"); w.String(b.Name ? b.Name : "");
        w.Key("register"); w.Uint(b.BindPoint);
        w.Key("space"); w.Uint(0);
        w.Key("count"); w.Uint(b.BindCount);
        w.Key("dimension"); w.String(Dimension(b));
        if (IsStructured(b.Type)) {
            // NumSamples carries the element stride of a structured buffer.
            w.Key("stride"); w.Uint(b.NumSamples);
            w.Key("type"); WriteStructuredElementType(w, FindBuffer(r, sd, b.Name, true));
        } else if (b.Type == D3D_SIT_CBUFFER || b.Type == D3D_SIT_TBUFFER) {
            ID3D11ShaderReflectionConstantBuffer* cb = FindBuffer(r, sd, b.Name, false);
            if (!cb) cb = r->GetConstantBufferByName(b.Name);
            w.Key("type"); WriteConstantBufferType(w, cb);
        } else if (b.Type == D3D_SIT_TEXTURE || b.Type == D3D_SIT_UAV_RWTYPED || b.Type == D3D_SIT_UAV_FEEDBACKTEXTURE) {
            if (const char* rt = ReturnTypeName(b.ReturnType)) { w.Key("returnType"); w.String(rt); }
            if ((b.Dimension == D3D_SRV_DIMENSION_TEXTURE2DMS || b.Dimension == D3D_SRV_DIMENSION_TEXTURE2DMSARRAY) &&
                b.NumSamples != UINT_MAX) {
                w.Key("samples"); w.Uint(b.NumSamples);
            }
        }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return std::move(w.str());
}

// ---------------------------------------------------------------------------------------------
// Reflection interfaces

ComPtr<ID3D11ShaderReflection> ReflectDxbc(const void* bytecode, size_t size, std::string& error) {
    ComPtr<ID3D11ShaderReflection> r;
    D3DCompilerLib& lib = D3DCompiler();
    if (!lib.reflect) { error = lib.error; return r; }
    HRESULT hr = lib.reflect(bytecode, size, IID_ID3D11ShaderReflection, r.putVoid());
    if (FAILED(hr)) {
        error = "D3DReflect failed: " + Hex((uint32_t)hr);
        r.reset();
    }
    return r;
}

}  // namespace

bool IsShaderContainer(const void* bytecode, size_t size) {
    std::vector<Part> parts;
    return ParseContainer(bytecode, size, parts);
}

ShaderInfo ReflectShader(const void* bytecode, size_t size) {
    ShaderInfo info;
    std::vector<Part> parts;
    if (!ParseContainer(bytecode, size, parts)) {
        info.error = "not a DXBC container";
        return info;
    }
    if (!ProgramInfo(parts, info)) {
        info.error = "the container has no shader program (no SHEX or SHDR part)";
        return info;
    }
    std::string error;
    ComPtr<ID3D11ShaderReflection> r = ReflectDxbc(bytecode, size, error);
    if (!r) {
        info.error = error;
        return info;
    }
    info.reflectionJson = BuildReflectionJson(r.get(), info);
    if (info.reflectionJson.empty()) info.error = "ID3D11ShaderReflection::GetDesc failed";
    return info;
}

}  // namespace d3d11insp
