#!/usr/bin/env python3
"""Generates the Direct3D 11 plugin's tables from the Windows SDK headers.

The D3D11 counterpart of gen_d3d12.py, with the same job: D3D11 has no registry, so the struct
serializers are written by hand (src/plugins/d3d11/src/serialize.cpp), and what is generated here is
what would be hopeless to keep by hand:

  d3d11_enums.gen.h / .cpp   a name table for every enum of d3d11.h and its versions, d3dcommon.h and
                             the DXGI headers: ToString_<Enum>(value) and, for the flag enums,
                             Flags_<Enum>(writer, value) writing "A | B". The tables hold names and
                             numbers only, so they compile with any SDK.
  d3d11_vtables.gen.h        for the interfaces the library hooks, the vtable slot of every method
                             (slot::ID3D11DeviceContext4_DrawIndexed) and a function pointer typedef
                             of its C signature (PFN_ID3D11DeviceContext4_DrawIndexed), read off the
                             C-style vtable structs the headers carry for CINTERFACE.
  d3d11_context_proxy.gen.h  ContextProxy, an ID3D11DeviceContext4 whose every method forwards to
                             the real context it wraps. A device context's vtable lives inside the
                             context object and the runtime rewrites its entries as the pipeline
                             state changes, so it cannot be patched the way the other objects' can:
                             the application is handed a proxy instead (src/plugins/d3d11/src/hooks_context.cpp
                             overrides the methods it records).

The generated files are committed (src/plugins/d3d11/gen), so a build needs neither Python nor a
particular SDK.

Usage: gen_d3d11.py [--sdk "<Windows Kits>/10/Include/<version>"] [--out src/plugins/d3d11/gen]
"""
import argparse
import os
import re
import sys

# The interfaces whose vtables the library patches (src/plugins/d3d11/src/hook.h): the newest
# version of each, whose vtable holds every older version's methods first.
HOOKED_INTERFACES = [
    "ID3D11DeviceChild",
    "ID3D11Device5",
    "ID3D11DeviceContext4",
    "ID3D11Buffer",
    "ID3D11Texture1D",
    "ID3D11Texture2D1",
    "ID3D11Texture3D1",
    "ID3D11ShaderResourceView1",
    "ID3D11RenderTargetView1",
    "ID3D11DepthStencilView",
    "ID3D11UnorderedAccessView1",
    "ID3D11VertexShader",
    "ID3D11PixelShader",
    "ID3D11GeometryShader",
    "ID3D11HullShader",
    "ID3D11DomainShader",
    "ID3D11ComputeShader",
    "ID3D11InputLayout",
    "ID3D11SamplerState",
    "ID3D11RasterizerState2",
    "ID3D11BlendState1",
    "ID3D11DepthStencilState",
    "ID3D11Query1",
    "ID3D11Predicate",
    "ID3D11Counter",
    "ID3D11ClassLinkage",
    "ID3D11CommandList",
    "ID3D11Fence",
    "IDXGIFactory7",
    "IDXGISwapChain4",
]

HEADERS = [
    "shared/dxgiformat.h", "shared/dxgicommon.h", "shared/dxgitype.h", "shared/dxgi.h",
    "shared/dxgi1_2.h", "shared/dxgi1_3.h", "shared/dxgi1_4.h", "shared/dxgi1_5.h", "shared/dxgi1_6.h",
    "um/d3dcommon.h", "um/d3d11.h", "um/d3d11_1.h", "um/d3d11_2.h", "um/d3d11_3.h", "um/d3d11_4.h",
    "um/d3d11sdklayers.h",
]

# The D3D11 headers give almost none of their flag enums DEFINE_ENUM_FLAG_OPERATORS (d3d12.h does),
# so the ones that are flags are named here.
EXTRA_FLAGS = {
    "D3D11_BIND_FLAG", "D3D11_CPU_ACCESS_FLAG", "D3D11_RESOURCE_MISC_FLAG", "D3D11_MAP_FLAG",
    "D3D11_CLEAR_FLAG", "D3D11_COLOR_WRITE_ENABLE", "D3D11_CREATE_DEVICE_FLAG", "D3D11_FORMAT_SUPPORT",
    "D3D11_FORMAT_SUPPORT2", "D3D11_BUFFER_UAV_FLAG", "D3D11_BUFFEREX_SRV_FLAG", "D3D11_DSV_FLAG",
    "D3D11_ASYNC_GETDATA_FLAG", "D3D11_QUERY_MISC_FLAG", "D3D11_COPY_FLAGS", "D3D11_LOGIC_OP",
    "D3D11_RAISE_FLAG", "D3D11_1_CREATE_DEVICE_CONTEXT_STATE_FLAG", "D3D11_TILE_MAPPING_FLAG",
    "D3D11_TILE_RANGE_FLAG", "D3D11_TILE_COPY_FLAG", "D3D11_FENCE_FLAG", "D3D11_SHADER_CACHE_SUPPORT_FLAGS",
    "D3D11_RLDO_FLAGS", "DXGI_SWAP_CHAIN_FLAG", "DXGI_USAGE", "DXGI_PRESENT",
}


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


# ---------------------------------------------------------------------------------------------
# Enums

ENUM_RE = re.compile(r"typedef\s*\n?\s*enum\s+(\w+)\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
FLAGS_RE = re.compile(r"DEFINE_ENUM_FLAG_OPERATORS\(\s*(\w+)\s*\)")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def parse_enums(text, known):
    """Returns [(name, [(entry, value)...])] in header order; `known` maps every constant seen so far."""
    out = []
    for m in ENUM_RE.finditer(text):
        # The typedef name, not the tag: d3dcommon.h tags some enums `_D3D_CBUFFER_TYPE`.
        name = m.group(3)
        body = m.group(2)
        entries = []
        next_value = 0
        for raw in body.split(","):
            item = raw.strip()
            if not item:
                continue
            if "=" in item:
                ename, expr = item.split("=", 1)
                ename = ename.strip()
                expr = expr.strip()
                # Header expressions: hex and decimal literals, shifts, ors, other constants.
                expr_py = re.sub(r"\b(\d+)[uUlL]+\b", r"\1", expr)
                expr_py = re.sub(r"\b(0x[0-9a-fA-F]+)[uUlL]+\b", r"\1", expr_py)
                try:
                    value = int(eval(expr_py, {"__builtins__": {}}, known))
                except Exception:
                    sys.stderr.write(f"gen_d3d11: cannot evaluate {ename} = {expr}\n")
                    continue
            else:
                ename = item
                value = next_value
            if not re.match(r"^\w+$", ename):
                continue
            if value >= 2 ** 63:
                value -= 2 ** 64
            entries.append((ename, value))
            known[ename] = value
            next_value = value + 1
        if entries:
            out.append((name, entries))
    return out


def emit_enums(enums, flags, out_dir):
    h = []
    cpp = []
    h.append("// Generated by tools/gen_d3d11.py from the Windows SDK headers: do not edit.\n")
    h.append("// Name tables of every D3D11, DXGI and debug-layer enum. Values only, so they compile with any SDK.\n")
    h.append("#pragma once\n#include <cstddef>\n#include <cstdint>\n\nnamespace gpuinsp::sdk { class JsonWriter; }\n\nnamespace d3d11insp {\n\n")
    h.append("struct EnumEntry { int64_t value; const char* name; };\n\n")
    h.append("/** The name of `value` in the table, or nullptr. */\nconst char* EnumName(const EnumEntry* entries, size_t count, int64_t value);\n")
    h.append("/** Writes a flags value as \"A | B\" from the single-bit entries of the table, an exact entry first, unknown bits in hex. */\n")
    h.append("void WriteFlags(gpuinsp::sdk::JsonWriter& w, const EnumEntry* entries, size_t count, uint64_t value);\n\n")
    cpp.append("// Generated by tools/gen_d3d11.py from the Windows SDK headers: do not edit.\n")
    cpp.append('#include "d3d11_enums.gen.h"\n\n#include <gpu_inspector/sdk/json.h>\n\n#include <cstdio>\n#include <string>\n\nnamespace d3d11insp {\n\n')
    cpp.append("""const char* EnumName(const EnumEntry* entries, size_t count, int64_t value) {
    for (size_t i = 0; i < count; ++i)
        if (entries[i].value == value) return entries[i].name;
    return nullptr;
}

void WriteFlags(gpuinsp::sdk::JsonWriter& w, const EnumEntry* entries, size_t count, uint64_t value) {
    if (const char* exact = EnumName(entries, count, (int64_t)value)) { w.String(exact); return; }
    std::string s;
    uint64_t rest = value;
    for (size_t i = 0; i < count && rest; ++i) {
        uint64_t bit = (uint64_t)entries[i].value;
        if (!bit || (bit & (bit - 1))) continue;   // zero, or a composite
        if (!(rest & bit)) continue;
        if (!s.empty()) s += " | ";
        s += entries[i].name;
        rest &= ~bit;
    }
    if (rest) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)rest);
        if (!s.empty()) s += " | ";
        s += buf;
    }
    if (s.empty()) s = "0";
    w.String(s);
}

""")
    for name, entries in enums:
        h.append(f"extern const EnumEntry kEnum_{name}[{len(entries)}];\n")
        h.append(f"inline const char* ToString_{name}(int64_t v) {{ return EnumName(kEnum_{name}, {len(entries)}, v); }}\n")
        if name in flags:
            h.append(f"inline void Flags_{name}(gpuinsp::sdk::JsonWriter& w, uint64_t v) {{ WriteFlags(w, kEnum_{name}, {len(entries)}, v); }}\n")
        cpp.append(f"const EnumEntry kEnum_{name}[{len(entries)}] = {{\n")
        for ename, value in entries:
            cpp.append(f"    {{{value}LL, \"{ename}\"}},\n")
        cpp.append("};\n")
    h.append("\n}  // namespace d3d11insp\n")
    cpp.append("\n}  // namespace d3d11insp\n")
    write_if_changed(os.path.join(out_dir, "d3d11_enums.gen.h"), "".join(h))
    write_if_changed(os.path.join(out_dir, "d3d11_enums.gen.cpp"), "".join(cpp))


# ---------------------------------------------------------------------------------------------
# Vtables

VTBL_RE = re.compile(r"typedef struct (\w+)Vtbl\s*\{(.*?)\}\s*\1Vtbl\s*;", re.S)
METHOD_RE = re.compile(r"([\w\s\*]+?)\(\s*STDMETHODCALLTYPE\s*\*\s*(\w+)\s*\)\s*\((.*?)\)\s*;", re.S)


def drop_else_branches(body):
    """Keeps the first branch of every #if/#else in a vtable struct (the aggregate-return
    variants of GetDesc sit in an #else and occupy the same slot)."""
    out = []
    skipping = 0
    depth = 0
    for line in body.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            depth += 1
        elif s.startswith("#else") and depth and not skipping:
            skipping = depth
            continue
        elif s.startswith("#endif"):
            if skipping == depth:
                skipping = 0
            depth -= 1
            continue
        if skipping:
            continue
        if s.startswith("#"):
            continue
        out.append(line)
    return "\n".join(out)


def strip_sal(params):
    """Removes SAL annotations (_In_, _In_reads_(n), _COM_Outptr_, ...) from a parameter list."""
    out = []
    i = 0
    n = len(params)
    while i < n:
        m = re.match(r"_[A-Z][A-Za-z_]*_\b", params[i:])
        if m and (i == 0 or not (params[i - 1].isalnum() or params[i - 1] == "_")):
            i += m.end()
            # An argument list may follow: skip balanced parentheses.
            j = i
            while j < n and params[j] in " \t\n":
                j += 1
            if j < n and params[j] == "(":
                depth = 0
                while j < n:
                    if params[j] == "(":
                        depth += 1
                    elif params[j] == ")":
                        depth -= 1
                        if depth == 0:
                            j += 1
                            break
                    j += 1
                i = j
            continue
        out.append(params[i])
        i += 1
    text = "".join(out)
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"\s*,\s*", ", ", text)
    text = re.sub(r"\(\s+", "(", text)
    return text.strip()


def parse_vtables(text):
    out = {}
    for m in VTBL_RE.finditer(text):
        iface = m.group(1)
        body = drop_else_branches(m.group(2))
        methods = []
        for mm in METHOD_RE.finditer(body):
            ret = re.sub(r"\s+", " ", mm.group(1)).strip()
            ret = ret.replace("BEGIN_INTERFACE", "").replace("END_INTERFACE", "").strip()
            name = mm.group(2)
            params = strip_sal(mm.group(3))
            methods.append((ret, name, params))
        out[iface] = methods
    return out


def emit_vtables(vtables, out_dir):
    h = []
    h.append("// Generated by tools/gen_d3d11.py from the Windows SDK headers: do not edit.\n")
    h.append("// The vtable slot of every method of the interfaces the library hooks, and a typedef of its\n")
    h.append("// C signature for calling the original through the saved entry (src/plugins/d3d11/src/hook.h).\n")
    h.append("#pragma once\n\n#include <d3d11_4.h>\n#include <d3d11sdklayers.h>\n#include <dxgi1_6.h>\n\n#include <cstdint>\n\nnamespace d3d11insp {\n\n")
    missing = [i for i in HOOKED_INTERFACES if i not in vtables]
    if missing:
        raise SystemExit(f"gen_d3d11: no vtable found for {', '.join(missing)}: is the SDK new enough?")
    h.append("namespace slot {\n")
    for iface in HOOKED_INTERFACES:
        methods = vtables[iface]
        h.append(f"enum {iface} : uint32_t {{\n")
        for i, (_ret, name, _params) in enumerate(methods):
            h.append(f"    {iface}_{name} = {i},\n")
        h.append(f"    {iface}_Count = {len(methods)}\n}};\n")
    h.append("}  // namespace slot\n\n")
    for iface in HOOKED_INTERFACES:
        for _i, (ret, name, params) in enumerate(vtables[iface]):
            h.append(f"typedef {ret} (STDMETHODCALLTYPE* PFN_{iface}_{name})({params});\n")
        h.append("\n")
    h.append("}  // namespace d3d11insp\n")
    write_if_changed(os.path.join(out_dir, "d3d11_vtables.gen.h"), "".join(h))


# ---------------------------------------------------------------------------------------------
# The context proxy

PROXY_INTERFACE = "ID3D11DeviceContext4"


def param_names(params):
    """The parameter names of a stripped parameter list ("UINT StartSlot, const FLOAT Color[ 4 ]" -> ["StartSlot", "Color"])."""
    names = []
    for p in split_params(params):
        m = re.search(r"(\w+)\s*(\[[^\]]*\])?\s*$", p.strip())
        names.append(m.group(1) if m else "")
    return names


def split_params(params):
    """Splits a parameter list on the commas outside parentheses and brackets."""
    out = []
    depth = 0
    cur = ""
    for ch in params:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def emit_proxy(vtables, out_dir):
    methods = vtables[PROXY_INTERFACE]
    h = []
    h.append("// Generated by tools/gen_d3d11.py from the Windows SDK headers: do not edit.\n")
    h.append(f"// ContextProxy: an {PROXY_INTERFACE} that forwards every method to the context it wraps. The\n")
    h.append("// library hands the application one of these in place of each device context (hooks_context.cpp\n")
    h.append("// overrides what it records), because a context's own vtable is rewritten by the runtime.\n")
    h.append("#pragma once\n\n#include <d3d11_4.h>\n\nnamespace d3d11insp {\n\n")
    h.append(f"class ContextProxy : public {PROXY_INTERFACE} {{\npublic:\n")
    h.append(f"    explicit ContextProxy({PROXY_INTERFACE}* real) : real_(real) {{}}\n")
    h.append("    virtual ~ContextProxy() = default;\n")
    h.append(f"    {PROXY_INTERFACE}* real() const {{ return real_; }}\n\n")
    for ret, name, params in methods:
        parts = split_params(params)[1:]   # without This
        decl = ", ".join(p.strip() for p in parts)
        names = ", ".join(param_names(", ".join(parts))) if parts else ""
        call = f"real_->{name}({names})"
        body = f"return {call};" if ret != "void" else f"{call};"
        h.append(f"    virtual {ret} STDMETHODCALLTYPE {name}({decl}) override {{ {body} }}\n")
    h.append("\nprotected:\n")
    h.append(f"    {PROXY_INTERFACE}* real_;\n")
    h.append("};\n\n}  // namespace d3d11insp\n")
    write_if_changed(os.path.join(out_dir, "d3d11_context_proxy.gen.h"), "".join(h))


def write_if_changed(path, content):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == content:
                return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)


def find_sdk():
    """The newest Windows 10/11 SDK include directory on this machine."""
    roots = [os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "Include")]
    if os.environ.get("WindowsSdkDir"):
        roots.insert(0, os.path.join(os.environ["WindowsSdkDir"], "Include"))
    for root in roots:
        if not os.path.isdir(root):
            continue
        versions = sorted((v for v in os.listdir(root) if re.match(r"^\d+\.", v) and os.path.exists(os.path.join(root, v, "um", "d3d11_4.h"))),
                          key=lambda v: [int(x) for x in v.split(".")], reverse=True)
        if versions:
            return os.path.join(root, versions[0])
    return None


def main():
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", help="Windows SDK include directory (the one holding um/ and shared/)")
    ap.add_argument("--out", default=os.path.join(root_dir, "src", "plugins", "d3d11", "gen"))
    args = ap.parse_args()
    sdk = args.sdk or find_sdk()
    if not sdk or not os.path.exists(os.path.join(sdk, "um", "d3d11_4.h")):
        raise SystemExit("gen_d3d11: Windows SDK not found (pass --sdk)")
    os.makedirs(args.out, exist_ok=True)
    enums = []
    flags = set(EXTRA_FLAGS)
    vtables = {}
    known = {}
    for rel in HEADERS:
        path = os.path.join(sdk, rel)
        if not os.path.exists(path):
            sys.stderr.write(f"gen_d3d11: {path} not found, skipping\n")
            continue
        text = strip_comments(read(path))
        enums.extend(parse_enums(text, known))
        flags.update(FLAGS_RE.findall(text))
        vtables.update(parse_vtables(text))
    seen = set()
    unique = []
    for name, entries in enums:
        if name in seen:
            continue
        seen.add(name)
        unique.append((name, entries))
    emit_enums(unique, flags, args.out)
    emit_vtables(vtables, args.out)
    emit_proxy(vtables, args.out)
    print(f"gen_d3d11: {len(unique)} enums, {len(HOOKED_INTERFACES)} vtables from {sdk}")


if __name__ == "__main__":
    main()
