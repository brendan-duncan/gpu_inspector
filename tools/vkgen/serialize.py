"""
Emits JSON serializers for Vulkan enums, bitmasks, structs, pNext chains and command arguments.

  vk_serialize.gen.h / .cpp

    const char* ToString_VkFormat(VkFormat v);              // every enum / FlagBits type
    void Flags_VkImageUsageFlags(JsonWriter& w, VkFlags v); // every bitmask type with known bits
    void ToJson(JsonWriter& w, const VkImageCreateInfo& s); // every struct / union
    void PNextToJson(JsonWriter& w, const void* pNext);     // walks a pNext chain
    void ArgsToJson_vkCmdDraw(JsonWriter& w, <params>);     // every command
"""
import os
import re

from .registry import SIGNED_SCALARS, FLOAT_SCALARS

HEADER = "// GENERATED FILE - do not edit. Produced by tools/gen_vulkan.py from vk.xml.\n"

# Parameters / members of these types are never serialized.
SKIP_TYPES = {"VkAllocationCallbacks"}

# Members whose pointer is only valid under a condition (C expression over `s`).
MEMBER_CONDITIONS = {
    ("VkWriteDescriptorSet", "pImageInfo"):
        "(s.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || s.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || "
        "s.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || s.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || "
        "s.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)",
    ("VkWriteDescriptorSet", "pBufferInfo"):
        "(s.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || s.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || "
        "s.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || s.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)",
    ("VkWriteDescriptorSet", "pTexelBufferView"):
        "(s.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || s.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)",
    ("VkDescriptorSetLayoutBinding", "pImmutableSamplers"):
        "(s.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || s.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)",
    ("VkFramebufferCreateInfo", "pAttachments"):
        "((s.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) == 0)",
}

UINT_BASETYPES = {"VkDeviceSize", "VkDeviceAddress", "VkFlags64", "VkFlags", "VkSampleMask", "VkRemoteAddressNV"}


def guard(lines, protect, body):
    if protect:
        lines.append(f"#ifdef {protect}")
    lines.extend(body)
    if protect:
        lines.append("#endif")


class Context:
    """Resolves count expressions for struct members (prefix 's.') or command params (no prefix)."""

    def __init__(self, fields, prefix):
        self.fields = {f.name: f for f in fields}
        self.prefix = prefix

    def count(self, expr):
        def repl(m):
            ident = m.group(0)
            f = self.fields.get(ident)
            if f is None:
                return ident
            ref = self.prefix + ident
            if f.ptr_depth > 0:
                return f"({ref} ? *{ref} : 0)"
            return ref
        # Identifiers followed by "->" are pointers being dereferenced explicitly; leave them alone.
        return re.sub(r"[A-Za-z_][A-Za-z0-9_]*(?!\s*->)(?![A-Za-z0-9_])", repl, expr)


class Emitter:
    def __init__(self, reg):
        self.reg = reg
        self.enum_funcs = set()   # enums with a ToString function
        self.flag_funcs = set()   # bitmask types with a Flags function
        self.union_selectors = {} # union name -> selector enum type

    # ------------------------------------------------------------------ helpers
    def scalar_value(self, type_name, expr):
        """C++ statement(s) writing a single non-pointer value, or None if unsupported."""
        reg = self.reg
        cat = reg.category(type_name)
        if cat in ("struct", "union"):
            return f"ToJson(w, {expr});"
        if cat == "enum":
            real = reg.real_enum_name(type_name)
            if real in self.enum_funcs:
                return f"w.Enum(ToString_{real}({expr}), (int64_t){expr});"
            return f"w.Int((int64_t){expr});"
        if cat == "bitmask":
            real = reg.real_enum_name(type_name)
            if real in self.flag_funcs:
                return f"Flags_{real}(w, {expr});"
            return f"w.Uint((uint64_t){expr});"
        if cat == "handle":
            real = reg.handle_aliases.get(type_name, type_name)
            return f'w.Handle(HT_{real}, "{real}", (uint64_t)(uintptr_t){expr});'
        if cat == "basetype":
            if type_name == "VkBool32":
                return f"w.Bool({expr} != 0);"
            if type_name in UINT_BASETYPES:
                return f"w.Uint((uint64_t){expr});"
            return None
        if cat == "scalar":
            if type_name in SIGNED_SCALARS:
                return f"w.Int((int64_t){expr});"
            if type_name in FLOAT_SCALARS:
                return f"w.Double((double){expr});"
            if type_name == "char":
                return f"w.Int((int64_t){expr});"
            if type_name == "void":
                return None
            return f"w.Uint((uint64_t){expr});"
        return None

    def is_scalar_like(self, type_name):
        return self.reg.category(type_name) in ("scalar", "basetype", "enum", "bitmask")

    def field_lines(self, f, expr, ctx, owner=None, in_union=False):
        """Returns C++ lines that write the value of field f (already positioned after Key), or None to skip."""
        reg = self.reg
        cat = reg.category(f.type)
        if f.type in SKIP_TYPES:
            return None
        if f.name == "pNext" and f.ptr_depth == 1:
            return [f"PNextToJson(w, {expr});"]
        if cat in ("funcpointer", "external", "unknown"):
            return None

        # Fixed-size arrays.
        if f.dims and f.ptr_depth == 0:
            if f.type == "char":
                return [f"w.FixedString({expr}, {f.dims[0]});"]
            inner = self.scalar_value(f.type, expr + "".join(f"[i{d}]" for d in range(len(f.dims))))
            if inner is None:
                return None
            lines = []
            for d, dim in enumerate(f.dims):
                lines.append("    " * d + f"w.BeginArray(); for (size_t i{d} = 0; i{d} < {dim}; ++i{d}) {{")
            lines.append("    " * len(f.dims) + inner)
            for d in reversed(range(len(f.dims))):
                lines.append("    " * d + "} w.EndArray();")
            return lines

        if f.ptr_depth == 0:
            if cat == "union" and f.selector and reg.real_struct(f.type).name in self.union_selectors:
                return [f"ToJson(w, {expr}, {ctx.prefix}{f.selector});"]
            v = self.scalar_value(f.type, expr)
            return [v] if v else None

        # Pointers.
        if in_union:
            return [f"w.Pointer((const void*){expr});"]
        if f.type == "void":
            if f.len:
                return [f"w.Bytes({expr}, (size_t)({ctx.count(f.len[0])}));"]
            return [f"w.Pointer({expr});"]
        if f.type == "char":
            if f.ptr_depth == 1:
                return [f"w.String({expr});"]
            if f.ptr_depth == 2 and f.len:
                n = ctx.count(f.len[0])
                return [f"if (!{expr}) w.Null(); else {{ w.BeginArray(); for (size_t i = 0; i < (size_t)({n}); ++i) w.String({expr}[i]); w.EndArray(); }}"]
            return None

        if f.len and f.len[0] != "null-terminated":
            n = ctx.count(f.len[0])
            elem = f"{expr}[i]"
            if f.ptr_depth >= 2:
                inner = self.scalar_value(f.type, f"(*{elem})")
                if inner is None:
                    return None
                body = f"if (!{elem}) w.Null(); else {inner}"
            else:
                inner = self.scalar_value(f.type, elem)
                if inner is None:
                    return None
                body = inner
            lines = [f"if (!{expr}) w.Null(); else {{", f"    size_t n = (size_t)({n});"]
            if self.is_scalar_like(f.type):
                lines.append("    if (n > w.maxScalarArray) { w.ArraySummary(n); } else {")
                lines.append(f"    w.BeginArray(); for (size_t i = 0; i < n; ++i) {{ {body} }} w.EndArray(); }}")
            else:
                lines.append(f"    w.BeginArray(); for (size_t i = 0; i < n; ++i) {{ {body} }} w.EndArray();")
            lines.append("}")
            return lines

        # Single pointer (nullable).
        if f.ptr_depth == 1:
            inner = self.scalar_value(f.type, f"(*{expr})")
            if inner is None:
                return None
            return [f"if (!{expr}) w.Null(); else {inner}"]
        return None

    def selector_type(self, struct, member):
        """Type of the member named by `member.selector` in `struct`, or None."""
        for m in struct.members:
            if m.name == member.selector:
                return m.type
        return None

    # ------------------------------------------------------------------ enums
    def emit_enums(self, h, cpp):
        reg = self.reg
        for name, en in sorted(reg.enums.items()):
            entries = {}
            for v in en.values + en.bits:
                entries.setdefault(v.value, v)
            if not entries:
                continue
            self.enum_funcs.add(name)
            guard(h, en.protect, [f"const char* ToString_{name}({name} v);"])
            body = [f"const char* ToString_{name}({name} v) {{", "    switch ((int64_t)v) {"]
            for value, v in sorted(entries.items()):
                guard(body, v.protect, [f'        case {v.name}: return "{v.name}";'])
            body += ["        default: return nullptr;", "    }", "}"]
            guard(cpp, en.protect, body)

        for flags, bits in sorted(reg.bitmask_bits.items()):
            en = reg.enums.get(bits) if bits else None
            if en is None or not en.bits:
                continue
            self.flag_funcs.add(flags)
            ctype = "VkFlags64" if flags in reg.bitmask_64 else "VkFlags"
            protect = reg.type_protect.get(flags, en.protect)
            guard(h, protect, [f"void Flags_{flags}(JsonWriter& w, {ctype} v);"])
            body = [f"void Flags_{flags}(JsonWriter& w, {ctype} v) {{", "    static const FlagBit bits[] = {"]
            for b in sorted(en.bits, key=lambda b: b.value):
                guard(body, b.protect, [f'        {{ (uint64_t){b.name}, "{b.name}" }},'])
            body += ["    };", "    WriteFlags(w, bits, sizeof(bits) / sizeof(bits[0]), (uint64_t)v);", "}"]
            guard(cpp, protect, body)

    # ------------------------------------------------------------------ structs
    def emit_structs(self, h, cpp):
        reg = self.reg
        # Find unions with selections and their selector types (from the struct that contains them).
        for st in reg.structs.values():
            for m in st.members:
                if not m.selector:
                    continue
                u = reg.real_struct(m.type)
                sel_type = self.selector_type(st, m)
                if u and u.is_union and sel_type and any(x.selection for x in u.members):
                    self.union_selectors[u.name] = sel_type

        for name, st in sorted(reg.structs.items()):
            if st.protect == "VKINSP_UNUSED_TYPE":
                continue
            ctx = Context(st.members, "s.")
            if st.is_union and name in self.union_selectors:
                sel_type = self.union_selectors[name]
                guard(h, st.protect, [f"void ToJson(JsonWriter& w, const {name}& s, {sel_type} sel);"])
                body = [f"void ToJson(JsonWriter& w, const {name}& s, {sel_type} sel) {{", "    w.BeginObject();", "    switch (sel) {"]
                for m in st.members:
                    if not m.selection:
                        continue
                    for selv in m.selection.split(","):
                        body.append(f"    case {selv}:")
                    lines = self.field_lines(m, f"s.{m.name}", ctx, st, in_union=False)
                    if lines:
                        body.append(f'        w.Key("{m.name}");')
                        body += ["        " + l for l in lines]
                    body.append("        break;")
                body += ["    default: break;", "    }", "    w.EndObject();", "}"]
                guard(cpp, st.protect, body)
                # Also a selector-less overload for use inside arrays etc.
                guard(h, st.protect, [f"void ToJson(JsonWriter& w, const {name}& s);"])
                guard(cpp, st.protect, [f"void ToJson(JsonWriter& w, const {name}& s) {{ w.BeginObject(); w.EndObject(); }}"])
                continue

            guard(h, st.protect, [f"void ToJson(JsonWriter& w, const {name}& s);"])
            body = [f"void ToJson(JsonWriter& w, const {name}& s) {{", "    w.BeginObject();"]
            for m in st.members:
                lines = self.field_lines(m, f"s.{m.name}", ctx, st, in_union=st.is_union)
                if not lines:
                    continue
                cond = MEMBER_CONDITIONS.get((name, m.name))
                body.append(f'    w.Key("{m.name}");')
                if cond:
                    body.append(f"    if (!{cond}) w.Null(); else {{")
                    body += ["        " + l for l in lines]
                    body.append("    }")
                else:
                    body += ["    " + l for l in lines]
            body += ["    w.EndObject();", "}"]
            guard(cpp, st.protect, body)

    def emit_pnext(self, h, cpp):
        reg = self.reg
        h.append("void PNextToJson(JsonWriter& w, const void* pNext);")
        body = [
            "void PNextToJson(JsonWriter& w, const void* pNext) {",
            "    if (!pNext) { w.Null(); return; }",
            "    w.BeginArray();",
            "    for (auto* p = static_cast<const VkBaseInStructure*>(pNext); p; p = p->pNext) {",
            "        // Loader-internal link structs are not part of the application's chain.",
            "        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||",
            "            p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;",
            "        switch ((int64_t)p->sType) {",
        ]
        seen = set()
        for name, st in sorted(reg.structs.items()):
            if not st.stype or st.stype in seen or st.protect == "VKINSP_UNUSED_TYPE":
                continue
            seen.add(st.stype)
            guard(body, st.protect, [f"        case {st.stype}: ToJson(w, *reinterpret_cast<const {name}*>(p)); break;"])
        body += [
            "        default:",
            "            w.BeginObject(); w.Key(\"sType\"); w.Enum(ToString_VkStructureType(p->sType), (int64_t)p->sType); w.EndObject();",
            "            break;",
            "        }",
            "    }",
            "    w.EndArray();",
            "}",
        ]
        cpp.extend(body)

    # ------------------------------------------------------------------ commands
    def emit_commands(self, cmds, h, cpp):
        for c in cmds:
            params = ", ".join(p.decl for p in c.params)
            ctx = Context(c.params, "")
            guard(h, c.protect, [f"void ArgsToJson_{c.name}(JsonWriter& w, {params});"])
            body = [f"void ArgsToJson_{c.name}(JsonWriter& w, {params}) {{", "    w.BeginObject();"]
            for p in c.params:
                lines = self.field_lines(p, p.name, ctx)
                if not lines:
                    continue
                body.append(f'    w.Key("{p.name}");')
                body += ["    " + l for l in lines]
            body += ["    w.EndObject();", "}"]
            guard(cpp, c.protect, body)


def emit(reg, cmds, out):
    em = Emitter(reg)
    h = [HEADER, "#pragma once", "#include <vulkan/vulkan.h>", '#include "json_writer.h"', '#include "vk_commands.gen.h"', "",
         "namespace vkinsp {", "",
         "struct FlagBit { uint64_t value; const char* name; };",
         "void WriteFlags(JsonWriter& w, const FlagBit* bits, size_t count, uint64_t value);", ""]
    cpp = [HEADER, '#include "vk_serialize.gen.h"', "#include <string>", "", "namespace vkinsp {", "",
           "void WriteFlags(JsonWriter& w, const FlagBit* bits, size_t count, uint64_t value) {",
           "    std::string s;",
           "    uint64_t rem = value;",
           "    for (size_t i = 0; i < count; ++i) {",
           "        if (bits[i].value && (value & bits[i].value) == bits[i].value) {",
           "            if (!s.empty()) s += \" | \";",
           "            s += bits[i].name;",
           "            rem &= ~bits[i].value;",
           "        }",
           "    }",
           "    if (rem) { char buf[32]; snprintf(buf, sizeof(buf), \"%s0x%llx\", s.empty() ? \"\" : \" | \", (unsigned long long)rem); s += buf; }",
           "    if (s.empty()) s = \"0\";",
           "    w.String(s);",
           "}", ""]
    em.emit_enums(h, cpp)
    h.append("")
    em.emit_structs(h, cpp)
    h.append("")
    em.emit_pnext(h, cpp)
    h.append("")
    em.emit_commands(cmds, h, cpp)
    h += ["", "} // namespace vkinsp", ""]
    cpp += ["", "} // namespace vkinsp", ""]
    with open(os.path.join(out, "vk_serialize.gen.h"), "w", newline="\n") as f:
        f.write("\n".join(h))
    with open(os.path.join(out, "vk_serialize.gen.cpp"), "w", newline="\n") as f:
        f.write("\n".join(cpp))
