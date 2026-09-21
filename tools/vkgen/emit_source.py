"""
Emits C++ *source text* from decoded Vulkan values: what the replay engine has built (its arena
structs and command arguments, after its own fixups) written out as compilable brace initializers
and calls. This is the generated half of Export to C++ (src/replay/src/exporter.h): the exporter
watches the replay create each object and record each command, and asks these to spell them.

Modeled on serialize.py, which walks the same members with the same count expressions and writes
JSON; here the walk writes designated initializers, so the output reads like the specification.

  vk_emit.gen.h / .cpp

    std::string Emit(SourceWriter& w, const VkImageCreateInfo& s, int indent);   // every struct and union: a brace initializer
    std::string Emit(SourceWriter& w, const VkClearValue& s, int indent, VkX sel); // unions with a selector, by it
    std::string EmitPNext(SourceWriter& w, const void* pNext);                   // the chain as locals declared first; "&head" or "nullptr"
    void Emit_vkCmdDraw(SourceWriter& w, const Args_vkCmdDraw& a);               // every vkCmd*: the call, as a statement
    ExportFn FindExportCommand(std::string_view name);                           // decodes captured arguments and emits the call
    int VulkanFunctionLevel(std::string_view name);                              // 0 global, 1 instance, 2 device; -1 unknown

How values are spelled (SourceWriter in src/replay/src/source_writer.h does the spelling):
  * enums and flags by name, from the decoder's tables (kEnum_*); an unknown value as a cast number;
  * handles as the variable the exporter named them (image_18); a handle it never named as
    VK_NULL_HANDLE with a note;
  * a pointer to a struct as a local declared before the statement, and passed by address; an
    array of anything as a local array; a byte blob and a long scalar array as Data(offset, size)
    into the exported project's data file;
  * a union without a selector by its largest member (the last of equal size), which is what the
    decoder filled; VkClearColorValue's bits get the float reading in a comment beside them;
  * a struct with few members and no pointers on one line, the others one member per line.
"""
import os

from .registry import SIGNED_SCALARS, FLOAT_SCALARS
from .serialize import SKIP_TYPES, UINT_BASETYPES, MEMBER_CONDITIONS, Context, guard

HEADER = "// GENERATED FILE - do not edit. Produced by tools/gen_replay.py from vk.xml.\n"

# A struct this small, with no pointers or fixed arrays, is spelled on one line.
INLINE_MEMBERS = 6


class SourceEmitter:
    def __init__(self, reg):
        self.reg = reg
        self.enum_tables = set()
        self.flag_types = set()
        self.union_selectors = {}
        self.inline = {}

    # ------------------------------------------------------------------ values
    def value(self, type_name, expr, indent):
        """A C++ expression (a std::string) spelling one non-pointer value, or None if it is not spelled."""
        reg = self.reg
        cat = reg.category(type_name)
        if cat in ("struct", "union"):
            return f"Emit(w, {expr}, {indent})"
        if cat == "enum":
            real = reg.real_enum_name(type_name)
            if real in self.enum_tables:
                return f'w.Enum(kEnum_{real}, kEnumCount_{real}, (int64_t){expr}, "{type_name}")'
            return f'w.EnumNumber((int64_t){expr}, "{type_name}")'
        if cat == "bitmask":
            real = reg.real_enum_name(type_name)
            bits = reg.bitmask_bits.get(real)
            if real in self.flag_types:
                return f'w.Flags(kEnum_{bits}, kEnumCount_{bits}, (uint64_t){expr}, "{type_name}")'
            return f"w.Uint((uint64_t){expr})"
        if cat == "handle":
            real = reg.handle_aliases.get(type_name, type_name)
            return f'w.Handle("{real}", (uint64_t)(uintptr_t){expr})'
        if cat == "basetype":
            if type_name == "VkBool32":
                return f"w.Bool({expr})"
            if type_name == "VkDeviceAddress":
                return f"w.Address((uint64_t){expr})"
            if type_name in UINT_BASETYPES:
                return f"w.Uint((uint64_t){expr})"
            return None
        if cat == "scalar":
            if type_name in SIGNED_SCALARS:
                return f"w.Int((int64_t){expr})"
            if type_name == "float":
                return f"w.Float({expr})"
            if type_name == "double":
                return f"w.Double({expr})"
            if type_name == "char":
                return f"w.Int((int64_t){expr})"
            if type_name == "void":
                return None
            return f"w.Uint((uint64_t){expr})"
        return None

    @staticmethod
    def hint(name):
        """A local variable's stem from a member's name: pAttachments -> attachments."""
        if len(name) > 1 and name[0] == "p" and name[1].isupper():
            name = name[1:]
        if len(name) > 2 and name[:2] == "pp" and name[2].isupper():
            name = name[2:]
        return name[0].lower() + name[1:]

    def field_expr(self, f, expr, ctx, owner=None, in_union=False):
        """
        C++ code for a std::string expression spelling field f (whose C++ value is `expr`), or None
        when the field is not spelled. `indent` is in scope as the enclosing struct's depth.
        """
        reg = self.reg
        cat = reg.category(f.type)
        if f.type in SKIP_TYPES:
            return '"nullptr"'
        if f.name == "pNext" and f.ptr_depth == 1:
            return f"w.PNextCast(EmitPNext(w, {expr}), {'true' if f.is_const else 'false'})"
        if cat == "funcpointer":
            return '"nullptr"'
        if cat in ("external", "unknown"):
            return None
        hint = self.hint(f.name)

        # Fixed-size arrays.
        if f.dims and f.ptr_depth == 0:
            if f.type == "char":
                return f"w.FixedString({expr}, {f.dims[0]})"
            depth = len(f.dims)
            inner = self.value(f.type, expr + "".join(f"[i{d}]" for d in range(depth)), "indent + 1")
            if inner is None:
                return None
            code = "[&] { std::string a0 = \"{\";"
            for d, dim in enumerate(f.dims):
                code += f" for (size_t i{d} = 0; i{d} < (size_t)({dim}); ++i{d}) {{"
                if d + 1 < depth:
                    code += f" std::string a{d + 1} = \"{{\";"
                else:
                    code += f" a{d} += (i{d} ? \", \" : \"\") + {inner};"
            for d in reversed(range(depth)):
                if d + 1 < depth:
                    code += f" a{d + 1} += \"}}\"; a{d} += (i{d} ? \", \" : \"\") + a{d + 1};"
                code += " }"
            code += " return a0 + \"}\"; }()"
            return code

        if f.ptr_depth == 0:
            if cat == "union" and f.selector and reg.real_struct(f.type).name in self.union_selectors:
                return f"Emit(w, {expr}, indent + 1, {ctx.prefix}{f.selector})"
            return self.value(f.type, expr, "indent + 1")

        # Pointers. Inside a union they were written as addresses: not spelled.
        if in_union:
            return None
        if f.type == "void":
            if f.len:
                return f"w.Bytes({expr}, (size_t)({ctx.count(f.len[0])}))"
            return '"nullptr"'
        if f.type == "char":
            if f.ptr_depth == 1:
                return f"w.String({expr})"
            if f.ptr_depth == 2 and f.len:
                return f'w.Strings("{hint}", {expr}, (size_t)({ctx.count(f.len[0])}))'
            return None

        if f.len and f.len[0] != "null-terminated":
            n = ctx.count(f.len[0])
            if f.ptr_depth == 1:
                if cat in ("struct", "union"):
                    if cat == "union" and f.selector and reg.real_struct(f.type).name in self.union_selectors:
                        return None
                    return (f'EmitStructArray(w, "{hint}", "{f.type}", {expr}, (size_t)({n}), '
                            f"[&](const {f.type}& e) {{ return Emit(w, e, w.indent + 1); }})")
                inner = self.value(f.type, "e", "w.indent + 1")
                if inner is None:
                    return None
                array = "EmitHandleArray" if cat == "handle" else "EmitScalarArray"
                return (f'{array}(w, "{hint}", "{f.type}", {expr}, (size_t)({n}), '
                        f"[&]({f.type} e) {{ return {inner}; }})")
            if f.ptr_depth == 2:
                if cat not in ("struct", "union"):
                    return None
                return (f'EmitPointerArray(w, "{hint}", "{f.type}", {expr}, (size_t)({n}), '
                        f'[&](const {f.type}* p) {{ return p ? "&" + EmitLocal(w, "{f.type}", "{hint}", Emit(w, *p, w.indent)) : std::string("nullptr"); }})')
            return None

        # A single pointer: a local, passed by address.
        if f.ptr_depth == 1:
            if cat in ("struct", "union"):
                return (f'({expr} ? "&" + EmitLocal(w, "{f.type}", "{hint}", Emit(w, *{expr}, w.indent)) : std::string("nullptr"))')
            inner = self.value(f.type, f"(*{expr})", "w.indent")
            if inner is None:
                return None
            return f'({expr} ? "&" + EmitLocal(w, "{f.type}", "{hint}", {inner}) : std::string("nullptr"))'
        return None

    @staticmethod
    def selector_type(struct, member):
        for m in struct.members:
            if m.name == member.selector:
                return m.type
        return None

    def struct_inline(self, st):
        """Whether a struct is spelled on one line: few members, no pointers or fixed arrays, nested ones inline too."""
        if st.name in self.inline:
            return self.inline[st.name]
        self.inline[st.name] = False   # against cycles
        ok = not st.is_union and len(st.members) <= INLINE_MEMBERS and not st.stype
        for m in st.members:
            if m.ptr_depth or m.dims or m.bitfield is not None:
                ok = False
                break
            cat = self.reg.category(m.type)
            if cat in ("struct", "union"):
                inner = self.reg.real_struct(m.type)
                if inner is None or not self.struct_inline(inner):
                    ok = False
                    break
            elif cat in ("funcpointer", "external", "unknown"):
                ok = False
                break
        self.inline[st.name] = ok
        return ok

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

        for name, st in sorted(reg.structs.items()):
            if st.protect == "VKINSP_UNUSED_TYPE":
                continue
            decls = [f"std::string Emit(SourceWriter& w, const {name}& s, int indent);"]
            if st.is_union and name in self.union_selectors:
                decls.append(f"std::string Emit(SourceWriter& w, const {name}& s, int indent, {self.union_selectors[name]} sel);")
            guard(h, st.protect, decls)

        for name, st in sorted(reg.structs.items()):
            if st.protect == "VKINSP_UNUSED_TYPE":
                continue
            ctx = Context(st.members, "s.")
            if st.is_union:
                # Without a selector, the largest member (the last of equal size) is the one the decoder filled.
                body = [f"std::string Emit(SourceWriter& w, const {name}& s, int indent) {{",
                        "    (void)w; (void)indent;",
                        "    size_t best = 0;",
                        "    std::string chosen;"]
                for m in st.members:
                    v = self.field_expr(m, f"s.{m.name}", ctx, st, in_union=True)
                    if not v:
                        continue
                    body += [f"    if (sizeof(s.{m.name}) >= best) {{",
                             f"        best = sizeof(s.{m.name});",
                             f'        chosen = ".{m.name} = " + {v};',
                             "    }"]
                if name == "VkClearColorValue":
                    body.append('    chosen += w.FloatComment(s.float32, 4);')
                body += ['    return "{ " + chosen + " }";', "}"]
                guard(cpp, st.protect, body)
                if name in self.union_selectors:
                    sel_type = self.union_selectors[name]
                    body = [f"std::string Emit(SourceWriter& w, const {name}& s, int indent, {sel_type} sel) {{",
                            "    (void)w;",
                            "    switch (sel) {"]
                    for m in st.members:
                        if not m.selection:
                            continue
                        for selv in m.selection.split(","):
                            body.append(f"    case {selv}:")
                        v = self.field_expr(m, f"s.{m.name}", ctx, st)
                        if v:
                            body.append(f'        return "{{ .{m.name} = " + {v} + " }}";')
                        else:
                            body.append("        break;")
                    body += ["    default: break;", "    }", "    return Emit(w, s, indent);", "}"]
                    guard(cpp, st.protect, body)
                continue

            inline = self.struct_inline(st)
            features = "Features" in name
            body = [f"std::string Emit(SourceWriter& w, const {name}& s, int indent) {{", "    (void)w; (void)indent; (void)s;"]
            if inline:
                body += ['    std::string out = "{";', "    const char* sep = \"\";"]
            else:
                body += ["    const std::string pad((size_t)(indent + 1) * 4, ' ');",
                         "    const std::string end((size_t)indent * 4, ' ');",
                         '    std::string out = "{\\n";']
            for m in st.members:
                if m.name == "sType" and st.stype:
                    v = f'std::string("{st.stype}")'
                else:
                    v = self.field_expr(m, f"s.{m.name}", ctx, st)
                if not v:
                    continue
                cond = MEMBER_CONDITIONS.get((name, m.name))
                if cond:
                    v = f'({cond} ? {v} : std::string("nullptr"))'
                if inline:
                    line = f'    out += std::string(sep) + ".{m.name} = " + {v}; sep = ", ";'
                else:
                    line = f'    out += pad + ".{m.name} = " + {v} + ",\\n";'
                # A features struct lists dozens of VkBool32 members: only the ones that are on are spelled.
                if features and m.type == "VkBool32" and not m.dims and not m.ptr_depth:
                    line = f"    if (s.{m.name}) {{{line.strip()} }}"
                body.append(line)
            if inline:
                body.append('    return out + "}";')
            else:
                body.append('    return out + end + "}";')
            body.append("}")
            guard(cpp, st.protect, body)

    def emit_pnext(self, h, cpp):
        reg = self.reg
        h.append("std::string EmitPNext(SourceWriter& w, const void* pNext);")
        # One small function per struct, so that EmitPNext itself, which recurses along the chain (each
        # struct's emitter spells its own pNext), holds no temporaries: as one function of several
        # hundred cases its frame ran to hundreds of kilobytes, and a chain of a dozen structs (any
        # engine's device create info) overflowed the stack.
        helpers = []
        body = [
            "std::string EmitPNext(SourceWriter& w, const void* pNext) {",
            "    if (!pNext) return \"nullptr\";",
            "    auto* p = static_cast<const VkBaseInStructure*>(pNext);",
            "    switch ((int64_t)p->sType) {",
        ]
        seen = set()
        for name, st in sorted(reg.structs.items()):
            if not st.stype or st.stype in seen or st.protect == "VKINSP_UNUSED_TYPE":
                continue
            seen.add(st.stype)
            hint = self.hint(name[2:])
            guard(helpers, st.protect, [
                f"static std::string EmitChained_{name}(SourceWriter& w, const void* p) {{",
                f'    return "&" + EmitLocal(w, "{name}", "{hint}", Emit(w, *static_cast<const {name}*>(p), w.indent));',
                "}"])
            guard(body, st.protect, [f"    case {st.stype}: return EmitChained_{name}(w, p);"])
        cpp.extend(helpers)
        body += [
            "    default:",
            "        w.Note(\"a pNext struct the exporter cannot spell (sType \" + std::to_string((int64_t)p->sType) + \") was left out of the chain\");",
            "        return EmitPNext(w, p->pNext);",
            "    }",
            "}",
        ]
        cpp.extend(body)

    # ------------------------------------------------------------------ commands
    def emit_commands(self, cmds, h, cpp):
        reg = self.reg
        recordable = [c for c in cmds if c.level == "device" and c.name.startswith("vkCmd") and c.params and c.params[0].type == "VkCommandBuffer"]
        for cmd in recordable:
            ctx = Context(cmd.params, "a.")
            guard(h, cmd.protect, [f"void Emit_{cmd.name}(SourceWriter& w, const Args_{cmd.name}& a);"])
            body = [f"void Emit_{cmd.name}(SourceWriter& w, const Args_{cmd.name}& a) {{",
                    "    (void)a; const int indent = w.indent; (void)indent;",
                    "    std::string args = w.cb;"]
            for p in cmd.params[1:]:
                if p.dims and p.ptr_depth == 0:
                    # A fixed array parameter (blendConstants[4]) is a local array passed by name.
                    inner = self.value(p.type, f"a.{p.name}[i]", "indent")
                    if inner is None:
                        body.append('    args += ", nullptr";')
                        continue
                    body.append(f'    {{ std::string items; for (size_t i = 0; i < (size_t)({p.dims[0]}); ++i) items += (i ? ", " : "") + {inner};'
                                f' args += ", " + EmitArrayLocal(w, "{p.type}", "{self.hint(p.name)}", items); }}')
                    continue
                v = self.field_expr(p, f"a.{p.name}", ctx)
                if not v:
                    body.append('    args += ", nullptr";')
                    continue
                body.append(f'    args += std::string(", ") + {v};')
            body += [f'    w.Use("{cmd.name}");',
                     f'    w.Line("{cmd.name}(" + args + ");");',
                     "}"]
            guard(cpp, cmd.protect, body)

        h += ["",
              "// Decodes a vkCmd*'s captured arguments and emits the call; a command naming objects the replay",
              "// does not have is left out with a comment, as the replay left it out.",
              "using ExportFn = void (*)(DecodeContext& c, const JValue& args, SourceWriter& w);",
              "ExportFn FindExportCommand(std::string_view name);",
              "// 0 global, 1 instance, 2 device; -1 for a name that is not a Vulkan command.",
              "int VulkanFunctionLevel(std::string_view name);", ""]
        for cmd in recordable:
            guard(cpp, cmd.protect, [
                f"static void Export_{cmd.name}(DecodeContext& c, const JValue& j, SourceWriter& w) {{",
                f"    if (!c.fns->{cmd.name[2:]}) {{ w.Comment(\"left out: {cmd.name} is not available on the replaying device\"); return; }}",
                f"    Args_{cmd.name} a;",
                "    const size_t unresolved = c.unresolved;",
                "    DecodeArgs(c, j, a);",
                "    if (c.unresolved != unresolved) { w.Comment(\"left out: it names objects the replay does not have\"); return; }",
                f"    Emit_{cmd.name}(w, a);",
                "}"])
        cpp += ["struct ExportEntry { const char* name; ExportFn fn; };", "static const ExportEntry kExportCommands[] = {"]
        for cmd in sorted(recordable, key=lambda c: c.name):
            guard(cpp, cmd.protect, [f'    {{ "{cmd.name}", Export_{cmd.name} }},'])
        cpp += ["    { nullptr, nullptr },", "};", "",
                "ExportFn FindExportCommand(std::string_view name) {",
                "    size_t lo = 0, hi = sizeof(kExportCommands) / sizeof(kExportCommands[0]) - 1;",
                "    while (lo < hi) {",
                "        size_t mid = (lo + hi) / 2;",
                "        int cmp = name.compare(kExportCommands[mid].name);",
                "        if (cmp == 0) return kExportCommands[mid].fn;",
                "        if (cmp > 0) lo = mid + 1; else hi = mid;",
                "    }",
                "    return nullptr;",
                "}", ""]
        level_id = {"global": 0, "instance": 1, "device": 2}
        cpp += ["struct LevelEntry { const char* name; int level; };", "static const LevelEntry kLevels[] = {"]
        for cmd in sorted(cmds, key=lambda c: c.name):
            guard(cpp, cmd.protect, [f'    {{ "{cmd.name}", {level_id[cmd.level]} }},'])
        cpp += ["    { nullptr, -1 },", "};", "",
                "int VulkanFunctionLevel(std::string_view name) {",
                "    size_t lo = 0, hi = sizeof(kLevels) / sizeof(kLevels[0]) - 1;",
                "    while (lo < hi) {",
                "        size_t mid = (lo + hi) / 2;",
                "        int cmp = name.compare(kLevels[mid].name);",
                "        if (cmp == 0) return kLevels[mid].level;",
                "        if (cmp > 0) lo = mid + 1; else hi = mid;",
                "    }",
                "    return -1;",
                "}", ""]


def emit(reg, cmds, out, enum_tables, flag_types):
    """`enum_tables` and `flag_types` are the decoder's: the names with a kEnum_ table and a DecodeFlags_ function."""
    em = SourceEmitter(reg)
    em.enum_tables = set(enum_tables)
    em.flag_types = set(flag_types)
    h = [HEADER, "#pragma once", "#include <vulkan/vulkan.h>", "#include <string>", "#include <string_view>",
         '#include "source_writer.h"', '#include "vk_decode.gen.h"', "", "namespace vkreplay {", ""]
    cpp = [HEADER, '#include "vk_emit.gen.h"', "", "namespace vkreplay {", ""]
    em.emit_structs(h, cpp)
    h.append("")
    em.emit_pnext(h, cpp)
    h.append("")
    em.emit_commands(cmds, h, cpp)
    h += ["} // namespace vkreplay", ""]
    cpp += ["} // namespace vkreplay", ""]
    with open(os.path.join(out, "vk_emit.gen.h"), "w", newline="\n") as f:
        f.write("\n".join(h))
    with open(os.path.join(out, "vk_emit.gen.cpp"), "w", newline="\n") as f:
        f.write("\n".join(cpp))
