// Shader bytecode: the container's stage, its reflection in the UI's ReflType shape, its
// disassembly and the source embedded in it (shader_reflect.h). Compiled into the capture library
// and into dxinsp_shader.exe, so it uses nothing of the library but common.h and the enum tables.
//
// DXBC goes through d3dcompiler_47.dll (D3DReflect, D3DDisassemble), which every Windows since 8.1
// has in System32. DXIL goes through dxcompiler.dll, which the system does not have: it is looked
// for beside this module, in the Vulkan SDK, in the Windows SDK and on PATH, and loaded once.
#include "shader_reflect.h"

#include "d3d12_enums.gen.h"

#include <d3d12shader.h>
#include <dxcapi.h>
#include <oleauto.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

// SysFreeString for the BSTRs IDxcPdbUtils hands out; the linker directive keeps the CMake lists
// (which neither target of this file owns) unchanged.
#pragma comment(lib, "oleaut32.lib")

namespace dxinsp
{

namespace
{

// ---------------------------------------------------------------------------------------------
// The container

constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

constexpr uint32_t kMagicDXBC = FourCC('D', 'X', 'B', 'C');
constexpr uint32_t kPartDXIL = FourCC('D', 'X', 'I', 'L');   // the DXIL program: version header + bitcode
constexpr uint32_t kPartSHDR = FourCC('S', 'H', 'D', 'R');   // DXBC shader model 4 program
constexpr uint32_t kPartSHEX = FourCC('S', 'H', 'E', 'X');   // DXBC shader model 5 program
constexpr uint32_t kPartPSV0 = FourCC('P', 'S', 'V', '0');   // pipeline state validation: stage, entry name
constexpr uint32_t kPartSTAT = FourCC('S', 'T', 'A', 'T');   // the reflection copy of the program (-Qstrip_reflect keeps it here)
constexpr uint32_t kPartILDN = FourCC('I', 'L', 'D', 'N');   // the debug name: the file dxc wrote the PDB to (-Zi and -Zs both)
constexpr uint32_t kPartHASH = FourCC('H', 'A', 'S', 'H');   // u32 flags + the 16-byte shader hash, which is what names the PDB

struct Part
{
    uint32_t fourcc = 0;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

uint32_t ReadU32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// Header: "DXBC", 16-byte hash, u32 version, u32 total size, u32 part count, u32 part offsets;
// each part: fourcc, u32 size, data.
bool ParseContainer(const void* bytecode, size_t size, std::vector<Part>& parts)
{
    if (!bytecode || size < 32)
        return false;
    const uint8_t* p = static_cast<const uint8_t*>(bytecode);
    if (ReadU32(p) != kMagicDXBC)
        return false;
    uint32_t total = ReadU32(p + 24);
    uint32_t count = ReadU32(p + 28);
    if (total < 32 || total > size)
        return false;
    if (count > (total - 32) / 4)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t offset = ReadU32(p + 32 + 4 * i);
        if (offset > total || total - offset < 8)
            return false;
        Part part;
        part.fourcc = ReadU32(p + offset);
        part.size = ReadU32(p + offset + 4);
        if (part.size > total - offset - 8)
            return false;
        part.data = p + offset + 8;
        parts.push_back(part);
    }
    return true;
}

const Part* FindPart(const std::vector<Part>& parts, uint32_t fourcc)
{
    for (const Part& p : parts)
        if (p.fourcc == fourcc)
            return &p;
    return nullptr;
}

std::string HexBytes(const uint8_t* p, size_t n)
{
    static const char* kDigits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        s.push_back(kDigits[p[i] >> 4]);
        s.push_back(kDigits[p[i] & 0xf]);
    }
    return s;
}

// The ILDN part is DxilShaderDebugName: u16 flags, u16 name length (without the terminator), then
// the name, NUL-terminated and padded to four bytes. dxc writes it for -Zi and for -Zs alike, and
// it holds exactly the file name -Fd produced, so it is what finds a stripped build's PDB.
std::string DebugNameOf(const std::vector<Part>& parts)
{
    const Part* ildn = FindPart(parts, kPartILDN);
    if (!ildn || ildn->size < 5)
        return std::string();
    uint32_t length = (uint32_t)ildn->data[2] | ((uint32_t)ildn->data[3] << 8);
    if (length == 0 || (uint64_t)length + 5 > ildn->size)
        return std::string();
    const char* name = reinterpret_cast<const char*>(ildn->data + 4);
    return std::string(name, strnlen(name, length));
}

// The shader hash: the HASH part's digest (u32 flags, then 16 bytes), which is what dxc names the
// PDB by; a container without that part falls back to the 16 bytes in its own header.
std::string HashHexOf(const void* bytecode, const std::vector<Part>& parts)
{
    if (const Part* hash = FindPart(parts, kPartHASH); hash && hash->size >= 20)
        return HexBytes(hash->data + 4, 16);
    return HexBytes(static_cast<const uint8_t*>(bytecode) + 4, 16);
}

// ---------------------------------------------------------------------------------------------
// The stage

struct StageNames
{
    const char* ui;        // the UI's stage name
    const char* profile;   // the target prefix
};

// Indexed by the DXIL shader kind (the program version's upper half); DXBC's program type uses
// the same numbering for its first six.
constexpr StageNames kStages[] = {
    {"fragment", "ps"},
    {"vertex", "vs"},
    {"geometry", "gs"},
    {"tess_control", "hs"},
    {"tess_eval", "ds"},
    {"compute", "cs"},
    {"library", "lib"},
    {"raygen", "lib"},
    {"intersection", "lib"},
    {"any_hit", "lib"},
    {"closest_hit", "lib"},
    {"miss", "lib"},
    {"callable", "lib"},
    {"mesh", "ms"},
    {"task", "as"},
};

void SetStage(ShaderInfo& info, uint32_t kind, uint32_t major, uint32_t minor)
{
    const size_t n = sizeof(kStages) / sizeof(kStages[0]);
    const StageNames names = kind < n ? kStages[kind] : StageNames{"unknown", "xs"};
    info.stage = names.ui;
    info.target = std::string(names.profile) + "_" + std::to_string(major) + "_" + std::to_string(minor);
}

// The stage and shader model of the program part: DXIL's version word is (kind << 16) | (major
// << 4) | minor; a DXBC program's first word is (type << 16) | (major << 4) | minor.
bool ProgramInfo(const std::vector<Part>& parts, ShaderInfo& info)
{
    if (const Part* dxil = FindPart(parts, kPartDXIL); dxil && dxil->size >= 8)
    {
        uint32_t v = ReadU32(dxil->data);
        info.dxil = true;
        SetStage(info, (v >> 16) & 0xffff, (v >> 4) & 0xf, v & 0xf);
        return true;
    }
    const Part* program = FindPart(parts, kPartSHEX);
    if (!program)
        program = FindPart(parts, kPartSHDR);
    if (program && program->size >= 4)
    {
        uint32_t v = ReadU32(program->data);
        info.dxil = false;
        SetStage(info, v >> 16, (v >> 4) & 0xf, v & 0xf);
        return true;
    }
    return false;
}

// The entry function name from the PSV0 part, which DXC writes since PSV version 3: the runtime
// info (its size first) carries an offset into the string table that follows the resource
// binding records. Older containers have no name here.
std::string EntryFromPsv(const Part& psv)
{
    const uint8_t* p = psv.data;
    const uint32_t size = psv.size;
    if (size < 4)
        return std::string();
    uint32_t infoSize = ReadU32(p);
    uint32_t pos = 4;
    if (infoSize > size - pos)
        return std::string();
    const uint32_t infoStart = pos;
    pos += infoSize;
    // PSVRuntimeInfo3 (52 bytes) is the first with the entry name, at offset 48.
    if (infoSize < 52)
        return std::string();
    if (size - pos < 4)
        return std::string();
    uint32_t resourceCount = ReadU32(p + pos);
    pos += 4;
    if (resourceCount)
    {
        if (size - pos < 4)
            return std::string();
        uint32_t bindSize = ReadU32(p + pos);
        pos += 4;
        uint64_t bytes = (uint64_t)resourceCount * bindSize;
        if (bytes > size - pos)
            return std::string();
        pos += (uint32_t)bytes;
    }
    if (size - pos < 4)
        return std::string();
    uint32_t stringTableSize = ReadU32(p + pos);
    pos += 4;
    if (stringTableSize > size - pos)
        return std::string();
    uint32_t nameOffset = ReadU32(p + infoStart + 48);
    if (nameOffset >= stringTableSize)
        return std::string();
    const char* table = reinterpret_cast<const char*>(p + pos);
    size_t maxLen = stringTableSize - nameOffset;
    size_t len = strnlen(table + nameOffset, maxLen);
    return std::string(table + nameOffset, len);
}

// ---------------------------------------------------------------------------------------------
// The DLLs

std::wstring ModulePath()
{
    HMODULE module = nullptr;
    // The module holding this code: the capture library or the shader tool, whichever this is.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ShaderHash), &module);
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(module, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0]))
        return std::wstring();
    return std::wstring(path, n);
}

std::wstring DirectoryOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"/\\");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::string BaseNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"/\\");
    return Narrow(p == std::wstring::npos ? path.c_str() : path.c_str() + p + 1);
}

std::wstring EnvW(const wchar_t* name)
{
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
        return std::wstring();
    return std::wstring(buf, n);
}

bool FileExists(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// "10.0.26100.0" > "10.0.22621.0": the newest Windows SDK's dxcompiler.dll is the one to prefer.
std::vector<uint32_t> VersionNumbers(const std::wstring& name)
{
    std::vector<uint32_t> out;
    uint32_t cur = 0;
    bool any = false;
    for (wchar_t c : name)
    {
        if (c >= L'0' && c <= L'9')
        {
            cur = cur * 10 + (uint32_t)(c - L'0');
            any = true;
        }
        else if (c == L'.')
        {
            out.push_back(any ? cur : 0);
            cur = 0;
            any = false;
        }
    }
    if (any)
        out.push_back(cur);
    return out;
}

std::vector<std::wstring> WindowsKitBins()
{
    std::wstring programs = EnvW(L"ProgramFiles(x86)");
    if (programs.empty())
        programs = L"C:\\Program Files (x86)";
    std::wstring bin = programs + L"\\Windows Kits\\10\\bin";
    std::vector<std::wstring> versions;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((bin + L"\\10.*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                versions.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(versions.begin(), versions.end(), [](const std::wstring& a, const std::wstring& b) {
        return VersionNumbers(a) > VersionNumbers(b);
    });
    std::vector<std::wstring> out;
    for (const std::wstring& v : versions)
        out.push_back(bin + L"\\" + v + L"\\x64");
    return out;
}

typedef HRESULT(WINAPI* PFN_Reflect)(LPCVOID data, SIZE_T size, REFIID iid, void** reflector);
typedef HRESULT(WINAPI* PFN_Disassemble)(LPCVOID data, SIZE_T size, UINT flags, LPCSTR comments, ID3DBlob** disassembly);

struct D3DCompilerLib
{
    std::once_flag once;
    HMODULE module = nullptr;
    PFN_Reflect reflect = nullptr;
    PFN_Disassemble disassemble = nullptr;
    std::string error;
};

D3DCompilerLib& D3DCompiler()
{
    static D3DCompilerLib lib;
    std::call_once(lib.once, [] {
        lib.module = LoadLibraryW(L"d3dcompiler_47.dll");
        if (lib.module)
        {
            lib.reflect = reinterpret_cast<PFN_Reflect>(GetProcAddress(lib.module, "D3DReflect"));
            lib.disassemble = reinterpret_cast<PFN_Disassemble>(GetProcAddress(lib.module, "D3DDisassemble"));
        }
        if (!lib.reflect || !lib.disassemble)
        {
            lib.error = "d3dcompiler_47.dll not found: it ships with Windows 8.1 and later; on older systems install the DirectX End-User Runtime";
            LogAlways("%s", lib.error.c_str());
        }
    });
    return lib;
}

struct DxcLib
{
    std::once_flag once;
    HMODULE module = nullptr;
    DxcCreateInstanceProc create = nullptr;
    std::string error;
};

DxcLib& Dxc()
{
    static DxcLib lib;
    std::call_once(lib.once, [] {
        std::wstring self = ModulePath();
        std::vector<std::wstring> candidates;
        std::wstring dir = DirectoryOf(self);
        if (!dir.empty())
            candidates.push_back(dir + L"\\dxcompiler.dll");
        std::wstring sdk = EnvW(L"VULKAN_SDK");
        if (!sdk.empty())
            candidates.push_back(sdk + L"\\Bin\\dxcompiler.dll");
        for (const std::wstring& bin : WindowsKitBins())
            candidates.push_back(bin + L"\\dxcompiler.dll");
        std::wstring found;
        for (const std::wstring& c : candidates)
        {
            if (!FileExists(c))
                continue;
            // The DLL's own directory first for its dependencies, whatever the process's PATH says.
            lib.module = LoadLibraryExW(c.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (lib.module)
            {
                found = c;
                break;
            }
        }
        if (!lib.module)
        {
            lib.module = LoadLibraryW(L"dxcompiler.dll");
            if (lib.module)
                found = L"dxcompiler.dll (PATH)";
        }
        if (lib.module)
            lib.create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(lib.module, "DxcCreateInstance"));
        if (!lib.create)
        {
            std::string beside = self.empty() ? "dxinsp_capture.dll" : BaseNameOf(self);
            lib.error = "dxcompiler.dll not found: install the Vulkan SDK or the Windows SDK, or put it beside " + beside;
            LogAlways("%s", lib.error.c_str());
        }
        else
        {
            Log("dxcompiler.dll: %s", Narrow(found.c_str()).c_str());
        }
    });
    return lib;
}

template <typename T>
ComPtr<T> DxcCreate(REFCLSID clsid)
{
    ComPtr<T> p;
    DxcLib& lib = Dxc();
    if (lib.create)
        lib.create(clsid, IID_PPV_ARGS(p.put()));
    return p;
}

// A DXC blob's text as UTF-8, without the terminator the compiler appends.
std::string BlobText(IDxcUtils* utils, IDxcBlob* blob)
{
    if (!blob)
        return std::string();
    std::string s;
    ComPtr<IDxcBlobUtf8> utf8;
    if (utils && SUCCEEDED(utils->GetBlobAsUtf8(blob, utf8.put())) && utf8)
        s.assign(utf8->GetStringPointer(), utf8->GetStringLength());
    else
        s.assign(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

std::string BstrText(BSTR s)
{
    std::string out = s ? Narrow(s, SysStringLen(s)) : std::string();
    if (s)
        SysFreeString(s);
    return out;
}

// ---------------------------------------------------------------------------------------------
// ReflType

uint32_t RoundUp16(uint32_t v) { return (v + 15) & ~15u; }

struct ReflNode
{
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
    struct Member
    {
        std::string name;
        uint32_t offset = 0;
        std::unique_ptr<ReflNode> type;
    };
    std::vector<Member> members;
    uint32_t size = 0;
};

// The scalar as it is stored: the min16 types occupy a full 32-bit slot in a constant buffer
// unless 16-bit types are enabled, in which case the compiler reports FLOAT16/INT16/UINT16.
std::unique_ptr<ReflNode> ScalarNode(D3D_SHADER_VARIABLE_TYPE type, const char* typeName)
{
    auto n = std::make_unique<ReflNode>();
    n->kind = "scalar";
    switch (type)
    {
        case D3D_SVT_BOOL:
            n->base = "bool";
            n->width = 32;
            break;
        case D3D_SVT_INT:
        case D3D_SVT_MIN12INT:
        case D3D_SVT_MIN16INT:
            n->base = "int";
            n->width = 32;
            break;
        case D3D_SVT_UINT:
        case D3D_SVT_MIN16UINT:
            n->base = "uint";
            n->width = 32;
            break;
        case D3D_SVT_UINT8:
            n->base = "uint";
            n->width = 8;
            break;
        case D3D_SVT_FLOAT:
        case D3D_SVT_MIN8FLOAT:
        case D3D_SVT_MIN10FLOAT:
        case D3D_SVT_MIN16FLOAT:
            n->base = "float";
            n->width = 32;
            break;
        case D3D_SVT_DOUBLE:
            n->base = "float";
            n->width = 64;
            break;
        case D3D_SVT_FLOAT16:
            n->base = "float";
            n->width = 16;
            break;
        case D3D_SVT_INT16:
            n->base = "int";
            n->width = 16;
            break;
        case D3D_SVT_UINT16:
            n->base = "uint";
            n->width = 16;
            break;
        case D3D_SVT_INT64:
            n->base = "int";
            n->width = 64;
            break;
        case D3D_SVT_UINT64:
            n->base = "uint";
            n->width = 64;
            break;
        default:
        {
            n->kind = "opaque";
            const char* e = ToString__D3D_SHADER_VARIABLE_TYPE(type);
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
std::unique_ptr<ReflNode> BuildType(ID3D12ShaderReflectionType* t, bool tight, bool ignoreElements = false)
{
    D3D12_SHADER_TYPE_DESC d{};
    if (!t || FAILED(t->GetDesc(&d)))
    {
        auto n = std::make_unique<ReflNode>();
        n->kind = "opaque";
        n->name = "unknown";
        return n;
    }
    if (d.Elements > 0 && !ignoreElements)
    {
        auto n = std::make_unique<ReflNode>();
        n->kind = "array";
        n->element = BuildType(t, tight, true);
        n->count = d.Elements;
        n->stride = tight ? n->element->size : RoundUp16(n->element->size);
        n->size = (d.Elements - 1) * n->stride + n->element->size;
        return n;
    }
    switch (d.Class)
    {
        case D3D_SVC_SCALAR:
            return ScalarNode(d.Type, d.Name);
        case D3D_SVC_VECTOR:
        {
            auto n = std::make_unique<ReflNode>();
            n->kind = "vector";
            n->element = ScalarNode(d.Type, d.Name);
            n->count = d.Columns ? d.Columns : 1;
            n->size = n->count * n->element->size;
            return n;
        }
        case D3D_SVC_MATRIX_ROWS:
        case D3D_SVC_MATRIX_COLUMNS:
        {
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
        case D3D_SVC_STRUCT:
        {
            auto n = std::make_unique<ReflNode>();
            n->kind = "struct";
            n->name = d.Name ? d.Name : "struct";
            for (UINT i = 0; i < d.Members; ++i)
            {
                ID3D12ShaderReflectionType* mt = t->GetMemberTypeByIndex(i);
                const char* mname = t->GetMemberTypeName(i);
                D3D12_SHADER_TYPE_DESC md{};
                if (!mt || FAILED(mt->GetDesc(&md)))
                    continue;
                ReflNode::Member m;
                m.name = mname ? mname : "";
                m.offset = md.Offset;
                m.type = BuildType(mt, tight);
                n->size = std::max(n->size, m.offset + m.type->size);
                n->members.push_back(std::move(m));
            }
            return n;
        }
        default:
        {
            auto n = std::make_unique<ReflNode>();
            n->kind = "opaque";
            const char* e = ToString__D3D_SHADER_VARIABLE_TYPE(d.Type);
            n->name = d.Name ? d.Name : (e ? e : "object");
            return n;
        }
    }
}

void WriteNode(JsonWriter& w, const ReflNode& n)
{
    w.BeginObject();
    w.Key("kind");
    w.String(n.kind);
    if (n.kind == "scalar")
    {
        w.Key("base");
        w.String(n.base);
        w.Key("width");
        w.Uint(n.width);
        w.Key("size");
        w.Uint(n.size);
    }
    else if (n.kind == "vector")
    {
        w.Key("element");
        WriteNode(w, *n.element);
        w.Key("count");
        w.Uint(n.count);
        w.Key("size");
        w.Uint(n.size);
    }
    else if (n.kind == "matrix")
    {
        w.Key("element");
        WriteNode(w, *n.element);
        w.Key("columns");
        w.Uint(n.columns);
        w.Key("rows");
        w.Uint(n.rows);
        w.Key("stride");
        w.Uint(n.stride);
        w.Key("rowMajor");
        w.Boolean(n.rowMajor);
        w.Key("size");
        w.Uint(n.size);
    }
    else if (n.kind == "array")
    {
        w.Key("element");
        WriteNode(w, *n.element);
        w.Key("count");
        w.Uint(n.count);
        w.Key("stride");
        w.Uint(n.stride);
        w.Key("size");
        w.Uint(n.size);
    }
    else if (n.kind == "struct")
    {
        w.Key("name");
        w.String(n.name);
        w.Key("members");
        w.BeginArray();
        for (const ReflNode::Member& m : n.members)
        {
            w.BeginObject();
            w.Key("name");
            w.String(m.name);
            w.Key("offset");
            w.Uint(m.offset);
            w.Key("type");
            WriteNode(w, *m.type);
            w.EndObject();
        }
        w.EndArray();
        w.Key("size");
        w.Uint(n.size);
    }
    else
    {
        w.Key("name");
        w.String(n.name);
    }
    w.EndObject();
}

// ---------------------------------------------------------------------------------------------
// Reflection JSON

const char* ResourceKind(D3D_SHADER_INPUT_TYPE t)
{
    switch (t)
    {
        case D3D_SIT_CBUFFER:
        case D3D_SIT_TBUFFER: return "cbuffer";
        case D3D_SIT_SAMPLER: return "sampler";
        case D3D_SIT_TEXTURE:
        case D3D_SIT_STRUCTURED:
        case D3D_SIT_BYTEADDRESS:
        case D3D_SIT_RTACCELERATIONSTRUCTURE: return "srv";
        default: return "uav";
    }
}

bool IsStructured(D3D_SHADER_INPUT_TYPE t)
{
    return t == D3D_SIT_STRUCTURED || t == D3D_SIT_UAV_RWSTRUCTURED || t == D3D_SIT_UAV_APPEND_STRUCTURED ||
        t == D3D_SIT_UAV_CONSUME_STRUCTURED || t == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
}

const char* TextureDimension(D3D_SRV_DIMENSION d)
{
    switch (d)
    {
        case D3D_SRV_DIMENSION_BUFFER:
        case D3D_SRV_DIMENSION_BUFFEREX: return "buffer";
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

const char* Dimension(const D3D12_SHADER_INPUT_BIND_DESC& b)
{
    switch (b.Type)
    {
        case D3D_SIT_CBUFFER:
        case D3D_SIT_TBUFFER: return "buffer";
        case D3D_SIT_SAMPLER: return "sampler";
        case D3D_SIT_BYTEADDRESS:
        case D3D_SIT_UAV_RWBYTEADDRESS: return "byteaddress";
        case D3D_SIT_RTACCELERATIONSTRUCTURE: return "accelerationStructure";
        case D3D_SIT_TEXTURE:
        case D3D_SIT_UAV_RWTYPED:
        case D3D_SIT_UAV_FEEDBACKTEXTURE: return TextureDimension(b.Dimension);
        default: return IsStructured(b.Type) ? "structured" : "buffer";
    }
}

const char* ReturnTypeName(D3D_RESOURCE_RETURN_TYPE t)
{
    switch (t)
    {
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
ID3D12ShaderReflectionConstantBuffer* FindBuffer(ID3D12ShaderReflection* r, const D3D12_SHADER_DESC& sd, const char* name, bool bindInfo)
{
    if (!name)
        return nullptr;
    for (UINT i = 0; i < sd.ConstantBuffers; ++i)
    {
        ID3D12ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i);
        D3D12_SHADER_BUFFER_DESC cd{};
        if (!cb || FAILED(cb->GetDesc(&cd)) || !cd.Name || strcmp(cd.Name, name) != 0)
            continue;
        bool isBindInfo = cd.Type == D3D_CT_RESOURCE_BIND_INFO;
        if (isBindInfo == bindInfo)
            return cb;
    }
    return nullptr;
}

void WriteConstantBufferType(JsonWriter& w, ID3D12ShaderReflectionConstantBuffer* cb)
{
    D3D12_SHADER_BUFFER_DESC cd{};
    if (!cb || FAILED(cb->GetDesc(&cd)))
    {
        w.Null();
        return;
    }
    ReflNode n;
    n.kind = "struct";
    n.name = cd.Name ? cd.Name : "cbuffer";
    n.size = cd.Size;
    for (UINT i = 0; i < cd.Variables; ++i)
    {
        ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(i);
        D3D12_SHADER_VARIABLE_DESC vd{};
        if (!var || FAILED(var->GetDesc(&vd)))
            continue;
        ReflNode::Member m;
        m.name = vd.Name ? vd.Name : "";
        m.offset = vd.StartOffset;
        m.type = BuildType(var->GetType(), false);
        n.members.push_back(std::move(m));
    }
    WriteNode(w, n);
}

void WriteStructuredElementType(JsonWriter& w, ID3D12ShaderReflectionConstantBuffer* cb)
{
    D3D12_SHADER_BUFFER_DESC cd{};
    if (!cb || FAILED(cb->GetDesc(&cd)) || cd.Variables == 0)
    {
        w.Null();
        return;
    }
    // The bind info holds one variable, "$Element", whose type is the element.
    ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(0);
    if (!var)
    {
        w.Null();
        return;
    }
    std::unique_ptr<ReflNode> n = BuildType(var->GetType(), true);
    WriteNode(w, *n);
}

void WriteSignatureType(JsonWriter& w, const D3D12_SIGNATURE_PARAMETER_DESC& p)
{
    ReflNode scalar;
    scalar.kind = "scalar";
    switch (p.ComponentType)
    {
        case D3D_REGISTER_COMPONENT_UINT32:
            scalar.base = "uint";
            scalar.width = 32;
            break;
        case D3D_REGISTER_COMPONENT_SINT32:
            scalar.base = "int";
            scalar.width = 32;
            break;
        case D3D_REGISTER_COMPONENT_FLOAT32:
            scalar.base = "float";
            scalar.width = 32;
            break;
        case D3D_REGISTER_COMPONENT_UINT16:
            scalar.base = "uint";
            scalar.width = 16;
            break;
        case D3D_REGISTER_COMPONENT_SINT16:
            scalar.base = "int";
            scalar.width = 16;
            break;
        case D3D_REGISTER_COMPONENT_FLOAT16:
            scalar.base = "float";
            scalar.width = 16;
            break;
        case D3D_REGISTER_COMPONENT_UINT64:
            scalar.base = "uint";
            scalar.width = 64;
            break;
        case D3D_REGISTER_COMPONENT_SINT64:
            scalar.base = "int";
            scalar.width = 64;
            break;
        case D3D_REGISTER_COMPONENT_FLOAT64:
            scalar.base = "float";
            scalar.width = 64;
            break;
        default:
            scalar.base = "float";
            scalar.width = 32;
            break;
    }
    scalar.size = scalar.width / 8;
    uint32_t count = 0;
    for (uint32_t m = p.Mask; m; m >>= 1)
        count += m & 1;
    if (count <= 1)
    {
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

void WriteSignature(JsonWriter& w, ID3D12ShaderReflection* r, UINT count, bool inputs)
{
    w.BeginArray();
    for (UINT i = 0; i < count; ++i)
    {
        D3D12_SIGNATURE_PARAMETER_DESC p{};
        HRESULT hr = inputs ? r->GetInputParameterDesc(i, &p) : r->GetOutputParameterDesc(i, &p);
        if (FAILED(hr))
            continue;
        std::string semantic = p.SemanticName ? p.SemanticName : "";
        w.BeginObject();
        // TEXCOORD1 rather than TEXCOORD twice: the name is what the UI lists.
        w.Key("name");
        w.String(p.SemanticIndex ? semantic + std::to_string(p.SemanticIndex) : semantic);
        w.Key("semantic");
        w.String(semantic);
        w.Key("index");
        w.Uint(p.SemanticIndex);
        w.Key("register");
        w.Uint(p.Register);
        if (p.SystemValueType != D3D_NAME_UNDEFINED)
        {
            const char* sv = ToString_D3D_NAME(p.SystemValueType);
            w.Key("systemValue");
            if (sv)
                w.String(sv);
            else
                w.Uint(p.SystemValueType);
        }
        w.Key("type");
        WriteSignatureType(w, p);
        w.EndObject();
    }
    w.EndArray();
}

std::string BuildReflectionJson(ID3D12ShaderReflection* r, const ShaderInfo& info)
{
    D3D12_SHADER_DESC sd{};
    if (FAILED(r->GetDesc(&sd)))
        return std::string();
    JsonWriter w;
    w.BeginObject();
    w.Key("stage");
    w.String(info.stage);
    if (!info.entryPoint.empty())
    {
        w.Key("entryPoint");
        w.String(info.entryPoint);
    }
    w.Key("target");
    w.String(info.target);
    w.Key("instructionCount");
    w.Uint(sd.InstructionCount);
    w.Key("inputs");
    WriteSignature(w, r, sd.InputParameters, true);
    w.Key("outputs");
    WriteSignature(w, r, sd.OutputParameters, false);
    if (info.stage == "compute" || info.stage == "mesh" || info.stage == "task")
    {
        UINT x = 0, y = 0, z = 0;
        r->GetThreadGroupSize(&x, &y, &z);
        w.Key("threadGroupSize");
        w.BeginArray();
        w.Uint(x);
        w.Uint(y);
        w.Uint(z);
        w.EndArray();
    }
    w.Key("resources");
    w.BeginArray();
    for (UINT i = 0; i < sd.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC b{};
        if (FAILED(r->GetResourceBindingDesc(i, &b)))
            continue;
        w.BeginObject();
        w.Key("kind");
        w.String(ResourceKind(b.Type));
        w.Key("name");
        w.String(b.Name ? b.Name : "");
        w.Key("register");
        w.Uint(b.BindPoint);
        w.Key("space");
        w.Uint(b.Space);
        w.Key("count");
        w.Uint(b.BindCount);
        w.Key("dimension");
        w.String(Dimension(b));
        if (IsStructured(b.Type))
        {
            // NumSamples carries the element stride of a structured buffer.
            w.Key("stride");
            w.Uint(b.NumSamples);
            w.Key("type");
            WriteStructuredElementType(w, FindBuffer(r, sd, b.Name, true));
        }
        else if (b.Type == D3D_SIT_CBUFFER || b.Type == D3D_SIT_TBUFFER)
        {
            ID3D12ShaderReflectionConstantBuffer* cb = FindBuffer(r, sd, b.Name, false);
            if (!cb)
                cb = r->GetConstantBufferByName(b.Name);
            w.Key("type");
            WriteConstantBufferType(w, cb);
        }
        else if (b.Type == D3D_SIT_TEXTURE || b.Type == D3D_SIT_UAV_RWTYPED || b.Type == D3D_SIT_UAV_FEEDBACKTEXTURE)
        {
            if (const char* rt = ReturnTypeName(b.ReturnType))
            {
                w.Key("returnType");
                w.String(rt);
            }
            if ((b.Dimension == D3D_SRV_DIMENSION_TEXTURE2DMS || b.Dimension == D3D_SRV_DIMENSION_TEXTURE2DMSARRAY) &&
                b.NumSamples != UINT_MAX)
            {
                w.Key("samples");
                w.Uint(b.NumSamples);
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

ComPtr<ID3D12ShaderReflection> ReflectDxbc(const void* bytecode, size_t size, std::string& error)
{
    ComPtr<ID3D12ShaderReflection> r;
    D3DCompilerLib& lib = D3DCompiler();
    if (!lib.reflect)
    {
        error = lib.error;
        return r;
    }
    HRESULT hr = lib.reflect(bytecode, size, IID_PPV_ARGS(r.put()));
    if (FAILED(hr))
    {
        error = "D3DReflect failed: " + HrText(hr);
        r.reset();
    }
    return r;
}

ComPtr<ID3D12ShaderReflection> ReflectDxil(const void* bytecode, size_t size, const std::vector<Part>& parts, std::string& error)
{
    ComPtr<ID3D12ShaderReflection> r;
    DxcLib& lib = Dxc();
    if (!lib.create)
    {
        error = lib.error;
        return r;
    }
    ComPtr<IDxcUtils> utils = DxcCreate<IDxcUtils>(CLSID_DxcUtils);
    if (!utils)
    {
        error = "dxcompiler.dll: DxcCreateInstance(CLSID_DxcUtils) failed";
        return r;
    }
    DxcBuffer buffer{bytecode, size, DXC_CP_ACP};
    HRESULT hr = utils->CreateReflection(&buffer, IID_PPV_ARGS(r.put()));
    if (SUCCEEDED(hr) && r)
        return r;
    r.reset();
    // A container whose program part was stripped of reflection keeps a copy in STAT.
    if (const Part* stat = FindPart(parts, kPartSTAT))
    {
        DxcBuffer statBuffer{stat->data, stat->size, DXC_CP_ACP};
        hr = utils->CreateReflection(&statBuffer, IID_PPV_ARGS(r.put()));
        if (SUCCEEDED(hr) && r)
            return r;
        r.reset();
    }
    error = "IDxcUtils::CreateReflection failed: " + HrText(hr);
    return r;
}

// The entry point from the debug information of a container compiled with -Zi, for a DXC too
// old to write it into PSV0.
std::string EntryFromDebugInfo(const void* bytecode, size_t size)
{
    DxcLib& lib = Dxc();
    if (!lib.create)
        return std::string();
    ComPtr<IDxcUtils> utils = DxcCreate<IDxcUtils>(CLSID_DxcUtils);
    ComPtr<IDxcPdbUtils> pdb = DxcCreate<IDxcPdbUtils>(CLSID_DxcPdbUtils);
    if (!utils || !pdb)
        return std::string();
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(utils->CreateBlob(bytecode, (UINT32)size, DXC_CP_ACP, blob.put())) || !blob)
        return std::string();
    if (FAILED(pdb->Load(blob.get())))
        return std::string();
    BSTR name = nullptr;
    if (FAILED(pdb->GetEntryPoint(&name)))
        return std::string();
    return BstrText(name);
}

// ---------------------------------------------------------------------------------------------
// PDBs
//
// -Zs keeps the source out of the container and writes it to a PDB instead, named after the
// shader hash when the build used -Fd <dir>\. IDxcPdbUtils loads such a file exactly as it loads
// a container, so the source of a stripped shader is there for whoever can find the file.

const int kPdbSearchDepth = 5;        // as symbolize.ts searches the symbol directories
const size_t kMaxScannedPdbs = 512;   // a hash scan loads every one of them: enough for a build tree's shader PDBs

/** The sources of a blob IDxcPdbUtils has loaded: a PDB file, or a container carrying its ILDB part. */
std::vector<std::pair<std::string, std::string>> LoadedSources(IDxcUtils* utils, IDxcPdbUtils* pdb)
{
    std::vector<std::pair<std::string, std::string>> out;
    UINT32 count = 0;
    if (FAILED(pdb->GetSourceCount(&count)))
        return out;
    for (UINT32 i = 0; i < count; ++i)
    {
        ComPtr<IDxcBlobEncoding> source;
        if (FAILED(pdb->GetSource(i, source.put())) || !source)
            continue;
        BSTR name = nullptr;
        std::string nameText = SUCCEEDED(pdb->GetSourceName(i, &name)) ? BstrText(name) : std::string();
        out.emplace_back(std::move(nameText), BlobText(utils, source.get()));
    }
    return out;
}

/** Whether an argument is an option, or the start of one that takes its value in the same word. */
bool ArgIs(const std::string& arg, const char* option)
{
    const size_t n = strlen(option);
    if (arg.compare(0, n, option) != 0)
        return false;
    return arg.size() == n || arg[n] == '=' || arg[n] == ':' || n == 2;   // "-D", "-E", "-T", "-I" take a glued value
}

/**
 * How dxc was run, from a loaded PDB or ILDB part: the main file, the defines and the rest of
 * the arguments. The compiler keeps them so a PDB can be recompiled to a full one
 * (CompileForFullPDB); they are just as much what compiling the source to SPIR-V needs.
 */
ShaderCompileInfo LoadedCompile(IDxcPdbUtils* pdb)
{
    ShaderCompileInfo out;
    BSTR text = nullptr;
    if (SUCCEEDED(pdb->GetMainFileName(&text)))
        out.mainFile = BstrText(text);
    text = nullptr;
    if (SUCCEEDED(pdb->GetEntryPoint(&text)))
        out.entryPoint = BstrText(text);
    text = nullptr;
    if (SUCCEEDED(pdb->GetTargetProfile(&text)))
        out.target = BstrText(text);
    UINT32 count = 0;
    if (SUCCEEDED(pdb->GetDefineCount(&count)))
    {
        for (UINT32 i = 0; i < count; ++i)
        {
            text = nullptr;
            if (SUCCEEDED(pdb->GetDefine(i, &text)))
                out.defines.push_back(BstrText(text));
        }
    }
    count = 0;
    if (SUCCEEDED(pdb->GetArgCount(&count)))
    {
        // The arguments as dxc saw them, less what names the input, the output and the debug
        // files and the defines (GetDefine has those): a compile of the same source again supplies
        // them itself. An option's value may sit in the same word ("-HV2021") or the next one.
        static const char* kValued[] = {"-Fo", "-Fd", "-Fe", "-Fh", "-Fc", "-Fi", "-Fre", "-Frs", "-Fsh", "-D", "-E", "-T", "-I", "-fspv-target-env"};
        static const char* kFlags[] = {"-Zi", "-Zs", "-Qembed_debug", "-Qstrip_debug", "-spirv"};
        bool skipValue = false;
        for (UINT32 i = 0; i < count; ++i)
        {
            text = nullptr;
            if (FAILED(pdb->GetArg(i, &text)))
                continue;
            std::string arg = BstrText(text);
            if (skipValue)
            {
                skipValue = false;
                continue;
            }
            if (arg.empty() || arg[0] != '-')
                continue;   // the input file
            bool skip = false;
            for (const char* option : kValued)
            {
                if (!ArgIs(arg, option))
                    continue;
                skip = true;
                skipValue = arg.size() == strlen(option);
                break;
            }
            for (const char* option : kFlags)
                if (arg == option)
                    skip = true;
            if (!skip)
                out.args.push_back(arg);
        }
    }
    return out;
}

struct LoadedPdb
{
    std::vector<std::pair<std::string, std::string>> files;
    /** The shader hash the PDB was written for, lowercase hex; "" when the PDB does not say. */
    std::string hash;
    std::string error;
    ShaderCompileInfo compile;
};

LoadedPdb LoadPdbFile(const std::wstring& path)
{
    LoadedPdb out;
    DxcLib& lib = Dxc();
    if (!lib.create)
    {
        out.error = lib.error;
        return out;
    }
    ComPtr<IDxcUtils> utils = DxcCreate<IDxcUtils>(CLSID_DxcUtils);
    ComPtr<IDxcPdbUtils> pdb = DxcCreate<IDxcPdbUtils>(CLSID_DxcPdbUtils);
    if (!utils || !pdb)
    {
        out.error = "dxcompiler.dll: DxcCreateInstance failed";
        return out;
    }
    ComPtr<IDxcBlobEncoding> blob;
    HRESULT hr = utils->LoadFile(path.c_str(), nullptr, blob.put());
    if (FAILED(hr) || !blob)
    {
        out.error = Narrow(path.c_str()) + ": cannot be read (" + HrText(hr) + ")";
        return out;
    }
    hr = pdb->Load(blob.get());
    if (FAILED(hr))
    {
        out.error = Narrow(path.c_str()) + ": not a shader PDB (" + HrText(hr) + ")";
        return out;
    }
    ComPtr<IDxcBlob> hash;
    if (SUCCEEDED(pdb->GetHash(hash.put())) && hash)
    {
        // The blob is the container's HASH part as it stands: u32 flags, then the 16-byte digest.
        const uint8_t* p = static_cast<const uint8_t*>(hash->GetBufferPointer());
        const size_t n = (size_t)hash->GetBufferSize();
        if (n >= 20)
            out.hash = HexBytes(p + 4, 16);
        else if (n >= 16)
            out.hash = HexBytes(p, 16);
    }
    out.files = LoadedSources(utils.get(), pdb.get());
    out.compile = LoadedCompile(pdb.get());
    if (out.files.empty())
        out.error = Narrow(path.c_str()) + ": carries no source";
    return out;
}

/** The file named `name` in `dir` or a few levels below it; "" when it is nowhere there. */
std::wstring FindFileNamed(const std::wstring& dir, const std::wstring& name, int depth = 0)
{
    std::wstring direct = dir + L"\\" + name;
    if (FileExists(direct))
        return direct;
    if (depth >= kPdbSearchDepth)
        return std::wstring();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return std::wstring();
    std::vector<std::wstring> children;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == L'.')
            continue;   // ".", "..", and the tool directories nobody builds into
        children.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const std::wstring& child : children)
    {
        std::wstring found = FindFileNamed(child, name, depth + 1);
        if (!found.empty())
            return found;
    }
    return std::wstring();
}

/** Every .pdb lying directly in `dir`, for the hash scan a build with its own naming needs. */
std::vector<std::wstring> PdbsIn(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.pdb").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        out.push_back(dir + L"\\" + fd.cFileName);
        if (out.size() >= kMaxScannedPdbs)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

/** A UTF-8 file name (the PDB's, which dxc spells in hex) as the wide string the file APIs take. */
std::wstring WidenUtf8(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string JoinPaths(const std::vector<std::wstring>& paths)
{
    std::string s;
    for (const std::wstring& p : paths)
    {
        if (!s.empty())
            s += ", ";
        s += Narrow(p.c_str());
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public

bool IsShaderContainer(const void* bytecode, size_t size)
{
    std::vector<Part> parts;
    return ParseContainer(bytecode, size, parts);
}

ShaderInfo ReflectShader(const void* bytecode, size_t size)
{
    ShaderInfo info;
    std::vector<Part> parts;
    if (!ParseContainer(bytecode, size, parts))
    {
        info.error = "not a DXBC/DXIL container";
        return info;
    }
    if (!ProgramInfo(parts, info))
    {
        info.error = "the container has no shader program (no DXIL, SHEX or SHDR part)";
        return info;
    }
    if (info.dxil)
    {
        if (const Part* psv = FindPart(parts, kPartPSV0))
            info.entryPoint = EntryFromPsv(*psv);
        if (info.entryPoint.empty() && info.stage != "library")
            info.entryPoint = EntryFromDebugInfo(bytecode, size);
    }
    if (info.stage == "library" || info.target.rfind("lib", 0) == 0)
    {
        info.error = "a shader library has no single-stage reflection";
        return info;
    }
    std::string error;
    ComPtr<ID3D12ShaderReflection> r = info.dxil ? ReflectDxil(bytecode, size, parts, error) : ReflectDxbc(bytecode, size, error);
    if (!r)
    {
        info.error = error;
        return info;
    }
    info.reflectionJson = BuildReflectionJson(r.get(), info);
    if (info.reflectionJson.empty())
        info.error = "ID3D12ShaderReflection::GetDesc failed";
    return info;
}

std::vector<ShaderOutputParam> ShaderOutputSignature(const void* bytecode, size_t size)
{
    std::vector<ShaderOutputParam> out;
    std::vector<Part> parts;
    ShaderInfo info;
    if (!ParseContainer(bytecode, size, parts) || !ProgramInfo(parts, info))
        return out;
    std::string error;
    ComPtr<ID3D12ShaderReflection> r = info.dxil ? ReflectDxil(bytecode, size, parts, error) : ReflectDxbc(bytecode, size, error);
    if (!r)
        return out;
    D3D12_SHADER_DESC sd{};
    if (FAILED(r->GetDesc(&sd)))
        return out;
    for (UINT i = 0; i < sd.OutputParameters; ++i)
    {
        D3D12_SIGNATURE_PARAMETER_DESC p{};
        if (FAILED(r->GetOutputParameterDesc(i, &p)))
            continue;
        ShaderOutputParam o;
        o.semantic = p.SemanticName ? p.SemanticName : "";
        o.semanticIndex = p.SemanticIndex;
        // The write mask says which components the shader wrote, and a stream-output entry takes
        // the first of them and how many there are (the mask is contiguous in practice).
        uint32_t first = 0, count = 0;
        for (uint32_t c = 0; c < 4; ++c)
        {
            if (!(p.Mask & (1u << c)))
                continue;
            if (!count)
                first = c;
            count++;
        }
        if (!count)
            continue;
        o.startComponent = first;
        o.componentCount = count;
        switch (p.ComponentType)
        {
            case D3D_REGISTER_COMPONENT_UINT32: o.base = "uint"; break;
            case D3D_REGISTER_COMPONENT_SINT32: o.base = "int"; break;
            default: o.base = "float"; break;
        }
        if (p.SystemValueType != D3D_NAME_UNDEFINED)
        {
            const char* sv = ToString_D3D_NAME(p.SystemValueType);
            o.systemValue = sv ? sv : "";
        }
        out.push_back(std::move(o));
    }
    return out;
}

bool DisassembleShader(const void* bytecode, size_t size, std::string& text, std::string& error)
{
    text.clear();
    std::vector<Part> parts;
    if (!ParseContainer(bytecode, size, parts))
    {
        error = "not a DXBC/DXIL container";
        return false;
    }
    if (FindPart(parts, kPartDXIL))
    {
        DxcLib& lib = Dxc();
        if (!lib.create)
        {
            error = lib.error;
            return false;
        }
        ComPtr<IDxcUtils> utils = DxcCreate<IDxcUtils>(CLSID_DxcUtils);
        ComPtr<IDxcCompiler> compiler = DxcCreate<IDxcCompiler>(CLSID_DxcCompiler);
        if (!utils || !compiler)
        {
            error = "dxcompiler.dll: DxcCreateInstance failed";
            return false;
        }
        ComPtr<IDxcBlobEncoding> blob;
        HRESULT hr = utils->CreateBlob(bytecode, (UINT32)size, DXC_CP_ACP, blob.put());
        if (FAILED(hr) || !blob)
        {
            error = "IDxcUtils::CreateBlob failed: " + HrText(hr);
            return false;
        }
        ComPtr<IDxcBlobEncoding> disassembly;
        hr = compiler->Disassemble(blob.get(), disassembly.put());
        if (FAILED(hr) || !disassembly)
        {
            error = "IDxcCompiler::Disassemble failed: " + HrText(hr);
            return false;
        }
        text = BlobText(utils.get(), disassembly.get());
        return true;
    }
    D3DCompilerLib& lib = D3DCompiler();
    if (!lib.disassemble)
    {
        error = lib.error;
        return false;
    }
    ID3DBlob* blob = nullptr;
    HRESULT hr = lib.disassemble(bytecode, size, 0, nullptr, &blob);
    if (FAILED(hr) || !blob)
    {
        error = "D3DDisassemble failed: " + HrText(hr);
        return false;
    }
    text.assign(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    blob->Release();
    while (!text.empty() && text.back() == '\0')
        text.pop_back();
    return true;
}

bool AssembleDxil(const std::string& text, std::vector<uint8_t>& container, std::string& error)
{
    container.clear();
    DxcLib& lib = Dxc();
    if (!lib.create)
    {
        error = lib.error;
        return false;
    }
    ComPtr<IDxcUtils> utils = DxcCreate<IDxcUtils>(CLSID_DxcUtils);
    ComPtr<IDxcAssembler> assembler = DxcCreate<IDxcAssembler>(CLSID_DxcAssembler);
    if (!utils || !assembler)
    {
        error = "dxcompiler.dll: DxcCreateInstance failed (no assembler in this build of it)";
        return false;
    }
    ComPtr<IDxcBlobEncoding> source;
    HRESULT hr = utils->CreateBlob(text.data(), (UINT32)text.size(), DXC_CP_UTF8, source.put());
    if (FAILED(hr) || !source)
    {
        error = "IDxcUtils::CreateBlob failed: " + HrText(hr);
        return false;
    }

    /** The operation's blob when it succeeded, else its error text in `error`. */
    auto result = [&](IDxcOperationResult* op, const char* what, ComPtr<IDxcBlob>* out) {
        HRESULT status = E_FAIL;
        if (op)
            op->GetStatus(&status);
        if (SUCCEEDED(status))
        {
            if (out)
                op->GetResult(out->put());
            return true;
        }
        ComPtr<IDxcBlobEncoding> errors;
        if (op)
            op->GetErrorBuffer(errors.put());
        const std::string said = errors ? BlobText(utils.get(), errors.get()) : std::string();
        error = std::string(what) + " failed" + (said.empty() ? ": " + HrText(status) : ": " + said);
        return false;
    };

    ComPtr<IDxcOperationResult> assembled;
    hr = assembler->AssembleToContainer(source.get(), assembled.put());
    if (FAILED(hr))
    {
        error = "IDxcAssembler::AssembleToContainer failed: " + HrText(hr);
        return false;
    }
    ComPtr<IDxcBlob> blob;
    if (!result(assembled.get(), "assembling the module", &blob) || !blob)
        return false;

    // Unsigned, the runtime refuses it. The validator checks the module and, editing in place,
    // writes the container's hash. The one built into dxcompiler.dll validates the DXIL version
    // that dxcompiler itself writes, which a separate dxil.dll of another SDK may be too old for.
    ComPtr<IDxcValidator> validator = DxcCreate<IDxcValidator>(CLSID_DxcValidator);
    if (!validator)
    {
        error = "dxcompiler.dll has no validator, so the module cannot be signed";
        return false;
    }
    ComPtr<IDxcOperationResult> validated;
    hr = validator->Validate(blob.get(), DxcValidatorFlags_InPlaceEdit, validated.put());
    if (FAILED(hr))
    {
        error = "IDxcValidator::Validate failed: " + HrText(hr);
        return false;
    }
    if (!result(validated.get(), "validating the module", nullptr))
        return false;
    const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
    container.assign(bytes, bytes + blob->GetBufferSize());
    return true;
}

std::vector<std::pair<std::string, std::string>> EmbeddedSources(const void* bytecode, size_t size)
{
    return EmbeddedSources(bytecode, size, nullptr);
}

std::vector<std::pair<std::string, std::string>> EmbeddedSources(const void* bytecode, size_t size, ShaderCompileInfo* compile)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<Part> parts;
    if (!ParseContainer(bytecode, size, parts) || !FindPart(parts, kPartDXIL))
        return out;
    DxcLib& lib = Dxc();
    if (!lib.create)
        return out;
    ComPtr<IDxcUtils> utils = DxcCreate<IDxcUtils>(CLSID_DxcUtils);
    ComPtr<IDxcPdbUtils> pdb = DxcCreate<IDxcPdbUtils>(CLSID_DxcPdbUtils);
    if (!utils || !pdb)
        return out;
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(utils->CreateBlob(bytecode, (UINT32)size, DXC_CP_ACP, blob.put())) || !blob)
        return out;
    // Load fails for a container without debug information, which is simply "no sources".
    if (FAILED(pdb->Load(blob.get())))
        return out;
    if (compile)
        *compile = LoadedCompile(pdb.get());
    return LoadedSources(utils.get(), pdb.get());
}

std::string ShaderDebugName(const void* bytecode, size_t size)
{
    std::vector<Part> parts;
    if (!ParseContainer(bytecode, size, parts))
        return std::string();
    return DebugNameOf(parts);
}

std::string ShaderHashHex(const void* bytecode, size_t size)
{
    std::vector<Part> parts;
    if (!ParseContainer(bytecode, size, parts))
        return std::string();
    return HashHexOf(bytecode, parts);
}

std::vector<std::pair<std::string, std::string>> PdbSources(const std::wstring& path, std::string& error)
{
    LoadedPdb loaded = LoadPdbFile(path);
    error = loaded.error;
    return loaded.files;
}

ShaderSourceFiles FindShaderSources(const void* bytecode, size_t size,
    const std::vector<std::wstring>& pdbFiles,
    const std::vector<std::wstring>& pdbDirs)
{
    ShaderSourceFiles out;
    out.files = EmbeddedSources(bytecode, size, &out.compile);
    if (!out.files.empty())
        return out;

    const std::string debugName = ShaderDebugName(bytecode, size);
    const std::string hash = ShaderHashHex(bytecode, size);
    std::vector<std::string> failures;
    std::vector<std::wstring> tried;

    // One candidate: its sources are the answer, and anything it has to say about itself is kept
    // for the note when no candidate works out.
    auto take = [&](const std::wstring& file) {
        tried.push_back(file);
        LoadedPdb loaded = LoadPdbFile(file);
        if (loaded.files.empty())
        {
            if (!loaded.error.empty())
                failures.push_back(loaded.error);
            return false;
        }
        out.files = std::move(loaded.files);
        out.compile = std::move(loaded.compile);
        out.pdb = Narrow(file.c_str());
        return true;
    };

    // A PDB named outright answers whatever the container says about its name.
    for (const std::wstring& file : pdbFiles)
        if (take(file))
            return out;

    // -Fd <dir>\ names the PDB after the shader hash; -Fd <file> records the path it was given,
    // so a build still standing where it was made is found by that path, and a build tree that
    // moved by the file's own name under each directory.
    std::vector<std::wstring> names;
    if (!debugName.empty())
    {
        std::wstring wide = WidenUtf8(debugName);
        size_t slash = wide.find_last_of(L"/\\");
        if (slash != std::wstring::npos)
        {
            if (FileExists(wide) && take(wide))
                return out;
            wide = wide.substr(slash + 1);
        }
        if (!wide.empty())
            names.push_back(wide);
    }
    if (!hash.empty() && debugName != hash + ".pdb")
        names.push_back(WidenUtf8(hash + ".pdb"));
    for (const std::wstring& dir : pdbDirs)
    {
        for (const std::wstring& name : names)
        {
            std::wstring found = FindFileNamed(dir, name);
            if (!found.empty() && take(found))
                return out;
        }
    }

    // Last, any .pdb lying in a directory that was written for this very shader: a build with a
    // naming of its own (-Fd <dir>\<name>.pdb) still records the hash inside the file.
    for (const std::wstring& dir : pdbDirs)
    {
        for (const std::wstring& file : PdbsIn(dir))
        {
            if (std::find(tried.begin(), tried.end(), file) != tried.end())
                continue;
            LoadedPdb loaded = LoadPdbFile(file);
            if (loaded.files.empty() || loaded.hash.empty() || loaded.hash != hash)
                continue;
            out.files = std::move(loaded.files);
            out.pdb = Narrow(file.c_str());
            return out;
        }
    }

    out.note = "the container carries no HLSL (built without -Zi)";
    if (!debugName.empty())
        out.note += "; dxc wrote its source to " + debugName;
    if (pdbDirs.empty() && pdbFiles.empty())
        out.note += "; no PDB directory was given";
    else
        out.note += "; not found in " + JoinPaths(pdbDirs.empty() ? pdbFiles : pdbDirs);
    for (const std::string& f : failures)
        out.note += "; " + f;
    return out;
}

uint64_t ShaderHash(const void* bytecode, size_t size)
{
    // FNV-1a 64.
    uint64_t h = 14695981039346656037ull;
    const uint8_t* p = static_cast<const uint8_t*>(bytecode);
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace dxinsp
