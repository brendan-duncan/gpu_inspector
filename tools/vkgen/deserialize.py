"""
Emits the replay tool's JSON decoders: the inverse of serialize.py. The layer writes an object's
creation arguments and a command's arguments as JSON (layer/src/json_writer.h); these read that
JSON back into Vulkan structs allocated in the replay's arena (replay/src/decode.h has the helpers
the generated code calls).

  vk_decode.gen.h / .cpp

    int64_t DecodeEnum_VkFormat(DecodeContext& c, const JValue* v);          // every enum with values
    uint64_t DecodeFlags_VkImageUsageFlags(DecodeContext& c, const JValue* v); // every bitmask with bits
    void Decode(DecodeContext& c, const JValue& j, VkImageCreateInfo& s, bool chain = true);
    const void* DecodePNext(DecodeContext& c, const JValue* j);
    struct Args_vkCreateImage { ... };                                        // every command's parameters
    void DecodeArgs(DecodeContext& c, const JValue& j, Args_vkCreateImage& a);
    struct VkFunctions { PFN_vkCreateImage CreateImage; ... };               // resolved at run time
    ReplayFn FindReplayCommand(std::string_view name);                        // vkCmd*: decode and record

How the JSON maps back (see serialize.py for the other direction):
  * handles {"__id": N} resolve through DecodeContext::Handle to the replay's own objects;
  * enums and flags accept the names the layer writes and plain numbers;
  * a pNext chain is a JSON array of every struct in the chain, and each element repeats the
    rest of the chain under its own "pNext": elements are decoded without their nested chain
    (chain = false) and linked in array order;
  * a union without a selector was written with every member: the largest member is decoded
    (the last of equal size), which for VkClearValue is color.uint32, the exact bits;
  * byte blobs over the layer's inline limit and summarized scalar arrays were written without
    their data: they decode to zeroed memory of the right size and a problem report.
"""
import os

from .registry import SIGNED_SCALARS, FLOAT_SCALARS
from .serialize import SKIP_TYPES, UINT_BASETYPES, guard

HEADER = "// GENERATED FILE - do not edit. Produced by tools/gen_replay.py from vk.xml.\n"


class Decoder:
    def __init__(self, reg):
        self.reg = reg
        self.enum_tables = set()   # real enum names with a kEnum_ table
        self.flag_funcs = set()    # bitmask types with a DecodeFlags_ function
        self.union_selectors = {}  # union name -> selector enum type

    # ------------------------------------------------------------------ values
    def value_lines(self, type_name, dst, src):
        """Lines assigning the JSON value `src` (a const JValue*, possibly null) to `dst`, or None."""
        reg = self.reg
        cat = reg.category(type_name)
        if cat in ("struct", "union"):
            return [f"if ({src}) Decode(c, *{src}, {dst});"]
        if cat == "enum":
            real = reg.real_enum_name(type_name)
            if real in self.enum_tables:
                return [f"{dst} = ({type_name})DecodeEnum_{real}(c, {src});"]
            return [f"{dst} = ({type_name})c.Int({src});"]
        if cat == "bitmask":
            real = reg.real_enum_name(type_name)
            if real in self.flag_funcs:
                return [f"{dst} = ({type_name})DecodeFlags_{real}(c, {src});"]
            return [f"{dst} = ({type_name})c.Uint({src});"]
        if cat == "handle":
            return [f"{dst} = ({type_name})(uintptr_t)c.Handle({src});"]
        if cat == "basetype":
            if type_name == "VkBool32":
                return [f"{dst} = c.Bool({src}) ? VK_TRUE : VK_FALSE;"]
            if type_name in UINT_BASETYPES:
                return [f"{dst} = ({type_name})c.Uint({src});"]
            return None
        if cat == "scalar":
            if type_name in SIGNED_SCALARS or type_name == "char":
                return [f"{dst} = ({type_name})c.Int({src});"]
            if type_name in FLOAT_SCALARS:
                return [f"{dst} = ({type_name})c.Double({src});"]
            if type_name == "void":
                return None
            return [f"{dst} = ({type_name})c.Uint({src});"]
        return None

    def field_lines(self, f, dst, src, prefix, in_union=False):
        """Lines decoding field f from `src` (const JValue*) into `dst`, or None when it is not decoded."""
        reg = self.reg
        cat = reg.category(f.type)
        if f.type in SKIP_TYPES:
            return None
        if f.name == "pNext" and f.ptr_depth == 1:
            return [f"if (chain) {dst} = (decltype({dst}))DecodePNext(c, {src});"]
        if cat in ("funcpointer", "external", "unknown"):
            return None

        # Fixed-size arrays (nested JSON arrays for several dimensions).
        if f.dims and f.ptr_depth == 0:
            if f.type == "char":
                return [f"c.FixedString({src}, {dst}, {f.dims[0]});"]
            depth = len(f.dims)
            inner = self.value_lines(f.type, dst + "".join(f"[i{d}]" for d in range(depth)), f"a{depth}")
            if inner is None:
                return None
            lines = [f"{{ const JValue* a0 = {src};"]
            for d, dim in enumerate(f.dims):
                pad = "    " * (d + 1)
                lines.append(f"{pad}if (a{d} && a{d}->IsArray()) for (size_t i{d} = 0; i{d} < (size_t)({dim}) && i{d} < a{d}->count; ++i{d}) {{ const JValue* a{d + 1} = &a{d}->items[i{d}];")
            lines += ["    " * (depth + 1) + l for l in inner]
            for d in reversed(range(depth)):
                lines.append("    " * (d + 1) + "}")
            lines.append("}")
            return lines

        if f.ptr_depth == 0:
            if cat == "union" and f.selector and reg.real_struct(f.type).name in self.union_selectors:
                return [f"if ({src}) Decode(c, *{src}, {dst}, {prefix}{f.selector});"]
            return self.value_lines(f.type, dst, src)

        # Pointers. Pointers inside unions were written as addresses; non-const pointers are outputs.
        if in_union or not f.is_const:
            return None
        if f.type == "void":
            return [f"{dst} = c.Bytes({src});"] if f.len and f.ptr_depth == 1 else None
        if f.type == "char":
            if f.ptr_depth == 1:
                return [f"{dst} = c.String({src});"]
            if f.ptr_depth == 2 and f.len:
                return [f"{dst} = c.Strings({src});"]
            return None

        if f.len and f.len[0] != "null-terminated":
            # Locals are ja/je/jarr/jp: DecodeArgs names its argument struct `a`.
            if f.ptr_depth == 1:
                inner = self.value_lines(f.type, "jarr[i]", "je")
                if inner is None:
                    return None
                return ([f"if (const JValue* ja = {src}) {{",
                         "    if (ja->IsArray()) {",
                         f"        auto* jarr = c.Make<{f.type}>(ja->count);",
                         "        for (size_t i = 0; i < ja->count; ++i) {",
                         "            const JValue* je = &ja->items[i];"]
                        + ["            " + l for l in inner]
                        + ["        }",
                           f"        {dst} = jarr;",
                           "    } else if (ja->IsObject()) {",
                           f"        {dst} = c.Make<{f.type}>((size_t)c.Uint(ja->Get(\"__count\")));",
                           f"        c.Problem(\"{f.name}: the capture summarized this array without its values\");",
                           "    }",
                           "}"])
            if f.ptr_depth == 2:
                inner = self.value_lines(f.type, "(*jp)", "je")
                if inner is None:
                    return None
                return ([f"if (const JValue* ja = {src}; ja && ja->IsArray()) {{",
                         f"    auto** jarr = c.Make<{f.type}*>(ja->count);",
                         "    for (size_t i = 0; i < ja->count; ++i) {",
                         "        const JValue* je = &ja->items[i];",
                         "        if (je->IsNull()) continue;",
                         f"        auto* jp = c.Make<{f.type}>(1);"]
                        + ["        " + l for l in inner]
                        + ["        jarr[i] = jp;",
                           "    }",
                           f"    {dst} = jarr;",
                           "}"])
            return None

        if f.ptr_depth == 1:
            inner = self.value_lines(f.type, "(*p)", src)
            if inner is None:
                return None
            return ([f"if ({src} && !{src}->IsNull()) {{",
                     f"    auto* p = c.Make<{f.type}>(1);"]
                    + ["    " + l for l in inner]
                    + [f"    {dst} = p;", "}"])
        return None

    @staticmethod
    def selector_type(struct, member):
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
                entries.setdefault(v.name, v)
            if not entries:
                continue
            self.enum_tables.add(name)
            guard(h, en.protect, [f"int64_t DecodeEnum_{name}(DecodeContext& c, const JValue* v);",
                                  f"extern const EnumEntry kEnum_{name}[];",
                                  f"extern const size_t kEnumCount_{name};"])
            body = [f"const EnumEntry kEnum_{name}[] = {{"]
            for ename in sorted(entries):
                v = entries[ename]
                guard(body, v.protect, [f'    {{ "{ename}", (int64_t){ename} }},'])
            body += ["    { nullptr, 0 },", "};",
                     f"const size_t kEnumCount_{name} = sizeof(kEnum_{name}) / sizeof(kEnum_{name}[0]) - 1;",
                     f"int64_t DecodeEnum_{name}(DecodeContext& c, const JValue* v) {{ return c.Enum(v, kEnum_{name}, kEnumCount_{name}, \"{name}\"); }}"]
            guard(cpp, en.protect, body)

        for flags, bits in sorted(reg.bitmask_bits.items()):
            en = reg.enums.get(bits) if bits else None
            if en is None or not en.bits or bits not in self.enum_tables:
                continue
            self.flag_funcs.add(flags)
            protect = reg.type_protect.get(flags, en.protect)
            decl = [f"uint64_t DecodeFlags_{flags}(DecodeContext& c, const JValue* v);"]
            inner_h = []
            guard(inner_h, en.protect, decl)
            guard(h, protect, inner_h)
            body = [f"uint64_t DecodeFlags_{flags}(DecodeContext& c, const JValue* v) {{ return c.Flags(v, kEnum_{bits}, kEnumCount_{bits}, \"{flags}\"); }}"]
            inner_cpp = []
            guard(inner_cpp, en.protect, body)
            guard(cpp, protect, inner_cpp)

    # ------------------------------------------------------------------ structs
    def emit_structs(self, h, cpp):
        reg = self.reg
        for st in reg.structs.values():
            for m in st.members:
                if not m.selector:
                    continue
                u = reg.real_struct(m.type)
                sel_type = self.selector_type(st, m)
                if u and u.is_union and sel_type and any(x.selection for x in u.members):
                    self.union_selectors[u.name] = sel_type

        # Declarations first: struct decoders call each other in any order.
        for name, st in sorted(reg.structs.items()):
            if st.protect == "VKINSP_UNUSED_TYPE":
                continue
            decls = [f"void Decode(DecodeContext& c, const JValue& j, {name}& s, bool chain = true);"]
            if st.is_union and name in self.union_selectors:
                decls.append(f"void Decode(DecodeContext& c, const JValue& j, {name}& s, {self.union_selectors[name]} sel);")
            guard(h, st.protect, decls)

        for name, st in sorted(reg.structs.items()):
            if st.protect == "VKINSP_UNUSED_TYPE":
                continue
            prefix = "s."
            if st.is_union:
                # Every member was written: decode the largest (the last of equal size).
                body = [f"void Decode(DecodeContext& c, const JValue& j, {name}& s, bool chain) {{",
                        "    (void)c; (void)chain;",
                        "    size_t best = 0;"]
                for m in st.members:
                    lines = self.field_lines(m, f"s.{m.name}", "v", prefix, in_union=True)
                    if not lines:
                        continue
                    body += [f'    if (const JValue* v = j.Get("{m.name}"); v && sizeof(s.{m.name}) >= best) {{',
                             f"        best = sizeof(s.{m.name});"]
                    body += ["        " + l for l in lines]
                    body.append("    }")
                body.append("}")
                guard(cpp, st.protect, body)
                if name in self.union_selectors:
                    sel_type = self.union_selectors[name]
                    body = [f"void Decode(DecodeContext& c, const JValue& j, {name}& s, {sel_type} sel) {{",
                            "    (void)c; const bool chain = true; (void)chain;",
                            "    switch (sel) {"]
                    for m in st.members:
                        if not m.selection:
                            continue
                        for selv in m.selection.split(","):
                            body.append(f"    case {selv}:")
                        lines = self.field_lines(m, f"s.{m.name}", "v", prefix)
                        if lines:
                            body.append(f'        if (const JValue* v = j.Get("{m.name}")) {{')
                            body += ["            " + l for l in lines]
                            body.append("        }")
                        body.append("        break;")
                    body += ["    default: break;", "    }", "}"]
                    guard(cpp, st.protect, body)
                continue

            body = [f"void Decode(DecodeContext& c, const JValue& j, {name}& s, bool chain) {{",
                    "    (void)c; (void)j; (void)chain;"]
            if st.stype:
                body.append(f"    s.sType = {st.stype};")
            for m in st.members:
                if m.name == "sType" and st.stype:
                    continue
                lines = self.field_lines(m, f"s.{m.name}", "v", prefix)
                if not lines:
                    continue
                body.append(f'    if (const JValue* v = j.Get("{m.name}")) {{')
                body += ["        " + l for l in lines]
                body.append("    }")
            body.append("}")
            guard(cpp, st.protect, body)

    def emit_pnext(self, h, cpp):
        reg = self.reg
        h.append("const void* DecodePNext(DecodeContext& c, const JValue* j);")
        body = [
            "const void* DecodePNext(DecodeContext& c, const JValue* j) {",
            "    if (!j || !j->IsArray()) return nullptr;",
            "    VkBaseOutStructure* head = nullptr;",
            "    VkBaseOutStructure* tail = nullptr;",
            "    for (size_t i = 0; i < j->count; ++i) {",
            "        const JValue& e = j->items[i];",
            "        int64_t st = DecodeEnum_VkStructureType(c, e.Get(\"sType\"));",
            "        VkBaseOutStructure* p = nullptr;",
            "        switch (st) {",
        ]
        seen = set()
        for name, st in sorted(reg.structs.items()):
            if not st.stype or st.stype in seen or st.protect == "VKINSP_UNUSED_TYPE":
                continue
            seen.add(st.stype)
            guard(body, st.protect, [f"        case {st.stype}: {{ auto* x = c.Make<{name}>(1); Decode(c, e, *x, false); p = reinterpret_cast<VkBaseOutStructure*>(x); break; }}"])
        body += [
            "        default:",
            "            c.Problem(\"pNext: a struct the replay cannot decode (sType \" + std::to_string(st) + \")\");",
            "            break;",
            "        }",
            "        if (!p) continue;",
            "        p->pNext = nullptr;",
            "        if (tail) tail->pNext = p; else head = p;",
            "        tail = p;",
            "    }",
            "    return head;",
            "}",
        ]
        cpp.extend(body)

    # ------------------------------------------------------------------ commands
    def emit_commands(self, cmds, h, cpp):
        reg = self.reg
        for cmd in cmds:
            members = []
            for p in cmd.params:
                decl = p.decl
                if p.dims:
                    decl = decl.replace("const ", "", 1)  # a fixed array parameter becomes a mutable array
                members.append(f"    {decl}{{}};")
            guard(h, cmd.protect, [f"struct Args_{cmd.name} {{"] + members + ["};",
                                   f"void DecodeArgs(DecodeContext& c, const JValue& j, Args_{cmd.name}& a);"])
            body = [f"void DecodeArgs(DecodeContext& c, const JValue& j, Args_{cmd.name}& a) {{",
                    "    (void)c; (void)j; (void)a; const bool chain = true; (void)chain;"]
            for p in cmd.params:
                lines = self.field_lines(p, f"a.{p.name}", "v", "a.")
                if not lines:
                    continue
                body.append(f'    if (const JValue* v = j.Get("{p.name}")) {{')
                body += ["        " + l for l in lines]
                body.append("    }")
            body.append("}")
            guard(cpp, cmd.protect, body)

        # Function pointers, resolved at run time so the replay links against no loader.
        h += ["", "struct VkFunctions {"]
        for cmd in cmds:
            guard(h, cmd.protect, [f"    PFN_{cmd.name} {cmd.name[2:]} = nullptr;"])
        h += ["};",
              "void LoadGlobalFunctions(VkFunctions& f, PFN_vkGetInstanceProcAddr gipa);",
              "void LoadInstanceFunctions(VkFunctions& f, VkInstance instance, PFN_vkGetInstanceProcAddr gipa);",
              "void LoadDeviceFunctions(VkFunctions& f, VkDevice device, PFN_vkGetDeviceProcAddr gdpa);", ""]
        for level, sig, getter in (("global", "PFN_vkGetInstanceProcAddr gipa", 'gipa(nullptr, "{n}")'),
                                   ("instance", "VkInstance instance, PFN_vkGetInstanceProcAddr gipa", 'gipa(instance, "{n}")'),
                                   ("device", "VkDevice device, PFN_vkGetDeviceProcAddr gdpa", 'gdpa(device, "{n}")')):
            fname = {"global": "LoadGlobalFunctions", "instance": "LoadInstanceFunctions", "device": "LoadDeviceFunctions"}[level]
            cpp.append(f"void {fname}(VkFunctions& f, {sig}) {{")
            for cmd in cmds:
                if cmd.level == level:
                    guard(cpp, cmd.protect, [f"    f.{cmd.name[2:]} = (PFN_{cmd.name}){getter.format(n=cmd.name)};"])
            cpp += ["}", ""]

        # vkCmd*: decode the recorded arguments and record the command into a command buffer.
        recordable = [c for c in cmds if c.level == "device" and c.name.startswith("vkCmd") and c.params and c.params[0].type == "VkCommandBuffer"]
        h += ["using ReplayFn = void (*)(DecodeContext& c, const JValue& args, VkCommandBuffer cb);",
              "// The recorder for a vkCmd* command by name, or null.",
              "ReplayFn FindReplayCommand(std::string_view name);",
              "// A function that decodes a command's arguments (any command) and discards them, or null: for checking captures.",
              "using DecodeCheckFn = void (*)(DecodeContext& c, const JValue& args);",
              "DecodeCheckFn FindArgsDecoder(std::string_view name);", ""]
        for cmd in recordable:
            call = ", ".join("cb" if i == 0 else f"a.{p.name}" for i, p in enumerate(cmd.params))
            guard(cpp, cmd.protect, [
                f"static void Replay_{cmd.name}(DecodeContext& c, const JValue& j, VkCommandBuffer cb) {{",
                f"    if (!c.fns->{cmd.name[2:]}) {{ c.Problem(\"{cmd.name} is not available on this device\"); return; }}",
                f"    Args_{cmd.name} a;",
                "    const size_t unresolved = c.unresolved;",
                "    DecodeArgs(c, j, a);",
                "    // A command naming objects the replay does not have is left out rather than given null handles.",
                f"    if (c.unresolved != unresolved) {{ c.Problem(\"left out: it names objects the replay does not have\"); return; }}",
                f"    c.fns->{cmd.name[2:]}({call});",
                "}"])
        cpp += ["struct ReplayEntry { const char* name; ReplayFn fn; };", "static const ReplayEntry kReplayCommands[] = {"]
        for cmd in sorted(recordable, key=lambda c: c.name):
            guard(cpp, cmd.protect, [f'    {{ "{cmd.name}", Replay_{cmd.name} }},'])
        cpp += ["    { nullptr, nullptr },", "};", "",
                "ReplayFn FindReplayCommand(std::string_view name) {",
                "    size_t lo = 0, hi = sizeof(kReplayCommands) / sizeof(kReplayCommands[0]) - 1;",
                "    while (lo < hi) {",
                "        size_t mid = (lo + hi) / 2;",
                "        int cmp = name.compare(kReplayCommands[mid].name);",
                "        if (cmp == 0) return kReplayCommands[mid].fn;",
                "        if (cmp > 0) lo = mid + 1; else hi = mid;",
                "    }",
                "    return nullptr;",
                "}", ""]
        for cmd in cmds:
            guard(cpp, cmd.protect, [f"static void Check_{cmd.name}(DecodeContext& c, const JValue& j) {{ Args_{cmd.name} a; DecodeArgs(c, j, a); }}"])
        cpp += ["struct CheckEntry { const char* name; DecodeCheckFn fn; };", "static const CheckEntry kCheckCommands[] = {"]
        for cmd in sorted(cmds, key=lambda c: c.name):
            guard(cpp, cmd.protect, [f'    {{ "{cmd.name}", Check_{cmd.name} }},'])
        cpp += ["    { nullptr, nullptr },", "};", "",
                "DecodeCheckFn FindArgsDecoder(std::string_view name) {",
                "    size_t lo = 0, hi = sizeof(kCheckCommands) / sizeof(kCheckCommands[0]) - 1;",
                "    while (lo < hi) {",
                "        size_t mid = (lo + hi) / 2;",
                "        int cmp = name.compare(kCheckCommands[mid].name);",
                "        if (cmp == 0) return kCheckCommands[mid].fn;",
                "        if (cmp > 0) lo = mid + 1; else hi = mid;",
                "    }",
                "    return nullptr;",
                "}", ""]


def emit(reg, cmds, out):
    d = Decoder(reg)
    h = [HEADER, "#pragma once", "#include <vulkan/vulkan.h>", "#include <string_view>", '#include "decode.h"', "",
         "namespace vkreplay {", ""]
    cpp = [HEADER, '#include "vk_decode.gen.h"', "#include <string>", "", "namespace vkreplay {", ""]
    d.emit_enums(h, cpp)
    h.append("")
    d.emit_structs(h, cpp)
    h.append("")
    d.emit_pnext(h, cpp)
    h.append("")
    d.emit_commands(cmds, h, cpp)
    h += ["} // namespace vkreplay", ""]
    cpp += ["} // namespace vkreplay", ""]
    with open(os.path.join(out, "vk_decode.gen.h"), "w", newline="\n") as f:
        f.write("\n".join(h))
    with open(os.path.join(out, "vk_decode.gen.cpp"), "w", newline="\n") as f:
        f.write("\n".join(cpp))
