"""
vk.xml parser producing a simple model of the Vulkan registry: commands, structs/unions,
enums, bitmasks, handles and their platform protection.
"""
import re
import sys
import xml.etree.ElementTree as ET

DISPATCHABLE_INSTANCE = {"VkInstance", "VkPhysicalDevice"}
DISPATCHABLE_DEVICE = {"VkDevice", "VkQueue", "VkCommandBuffer"}

SCALAR_TYPES = {
    "uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t", "uint64_t", "int64_t",
    "float", "double", "size_t", "char", "void", "int",
}
SIGNED_SCALARS = {"int8_t", "int16_t", "int32_t", "int64_t", "int"}
FLOAT_SCALARS = {"float", "double"}


def api_ok(elem):
    api = elem.get("api")
    if api is None:
        return True
    return "vulkan" in api.split(",")


def element_text(elem, skip_tags=("comment",)):
    """Reconstructs the flattened declaration text of a <param>/<member>/<proto> element."""
    parts = [elem.text or ""]
    for child in elem:
        if child.tag in skip_tags:
            parts.append(child.tail or "")
            continue
        parts.append(child.text or "")
        parts.append(child.tail or "")
    text = "".join(parts)
    text = re.sub(r"\s+", " ", text).strip()
    text = text.replace(" *", "*").replace("* ", "* ").replace(" [", "[")
    return text


class Field:
    """A struct member or a command parameter."""

    def __init__(self, elem):
        self.decl = element_text(elem)
        self.type = elem.find("type").text
        self.name = elem.find("name").text
        self.ptr_depth = self.decl.count("*")
        self.is_const = self.decl.startswith("const ") or " const" in self.decl
        len_attr = elem.get("altlen") or elem.get("len")
        self.len = [x.strip() for x in len_attr.split(",")] if len_attr else []
        self.optional = (elem.get("optional") or "").split(",")
        self.values = elem.get("values")          # sType value for struct members
        self.selector = elem.get("selector")      # member that selects a union
        self.selection = elem.get("selection")    # union member: active for this selector value
        self.noautovalidity = elem.get("noautovalidity") == "true"
        # Fixed array dimensions, e.g. "[4]" or "[VK_UUID_SIZE]" or "[3][4]".
        self.dims = re.findall(r"\[([A-Za-z0-9_]+)\]", self.decl)
        # Bitfield width, e.g. "uint32_t x:24".
        m = re.search(r":\s*(\d+)\s*$", self.decl.split(self.name)[-1]) if self.name in self.decl else None
        self.bitfield = int(m.group(1)) if m else None
        if self.bitfield is not None:
            self.decl = re.sub(r":\s*\d+\s*$", "", self.decl).strip()

    @property
    def is_pointer(self):
        return self.ptr_depth > 0

    @property
    def is_string(self):
        return self.type == "char" and (self.ptr_depth == 1 and (not self.len or self.len[0] == "null-terminated") or (self.ptr_depth == 0 and self.dims))


class Command:
    def __init__(self, name, ret, params, alias_of=None):
        self.name = name
        self.ret = ret
        self.params = params
        self.alias_of = alias_of
        self.protect = None
        self.level = None  # 'global' | 'instance' | 'device'

    @property
    def dispatch_param(self):
        return self.params[0] if self.params else None


class Struct:
    def __init__(self, name, members, is_union, extends):
        self.name = name
        self.members = members
        self.is_union = is_union
        self.extends = extends
        self.protect = None
        self.stype = None
        for m in members:
            if m.name == "sType" and m.values:
                self.stype = m.values.split(",")[0]


class EnumValue:
    def __init__(self, name, value, protect=None):
        self.name = name
        self.value = value
        self.protect = protect


class Enum:
    def __init__(self, name, is_bitmask, bitwidth):
        self.name = name
        self.is_bitmask = is_bitmask
        self.bitwidth = bitwidth
        self.values = []      # EnumValue (ordinary values)
        self.bits = []        # EnumValue (bitpos values) for bitmasks
        self.protect = None


class Registry:
    def __init__(self, path):
        self.commands = []
        self.structs = {}          # name -> Struct (aliases resolved away)
        self.struct_aliases = {}   # alias -> real
        self.enums = {}            # name -> Enum
        self.enum_aliases = {}
        self.bitmask_bits = {}     # VkXxxFlags -> VkXxxFlagBits (or None)
        self.bitmask_64 = set()    # flags types that are VkFlags64
        self.handles = {}          # name -> dispatchable bool
        self.handle_aliases = {}
        self.handle_objtype = {}   # name -> VK_OBJECT_TYPE_* enum name
        self.basetypes = {}        # name -> underlying C type
        self.funcpointers = set()
        self.external = set()
        self.platforms = {}
        self.type_protect = {}     # type name -> protect macro
        self._parse(path)

    # ------------------------------------------------------------------ parsing
    def _parse(self, path):
        root = ET.parse(path).getroot()
        for p in root.find("platforms"):
            self.platforms[p.get("name")] = p.get("protect")

        self.ext_protect = {}
        for ext in root.find("extensions"):
            protect = None
            if ext.get("platform"):
                protect = self.platforms.get(ext.get("platform"))
            elif ext.get("provisional") == "true":
                protect = "VK_ENABLE_BETA_EXTENSIONS"
            self.ext_protect[ext.get("name")] = protect

        self._parse_types(root)
        self._parse_enums(root)
        self._parse_commands(root)
        self._parse_requirements(root)

    def _parse_types(self, root):
        struct_aliases = []
        for t in root.find("types"):
            if not api_ok(t):
                continue
            cat = t.get("category")
            name = t.get("name") or (t.find("name").text if t.find("name") is not None else None)
            if cat == "handle":
                if t.get("alias"):
                    self.handle_aliases[name] = t.get("alias")
                else:
                    self.handles[name] = t.find("type").text == "VK_DEFINE_HANDLE"
                    if t.get("objtypeenum"):
                        self.handle_objtype[name] = t.get("objtypeenum")
            elif cat in ("struct", "union"):
                if t.get("alias"):
                    struct_aliases.append((name, t.get("alias")))
                    continue
                members = [Field(m) for m in t.findall("member") if api_ok(m)]
                extends = (t.get("structextends") or "").split(",") if t.get("structextends") else []
                self.structs[name] = Struct(name, members, cat == "union", extends)
            elif cat == "basetype":
                inner = t.find("type")
                self.basetypes[name] = inner.text if inner is not None else None
            elif cat == "bitmask":
                if t.get("alias"):
                    self.enum_aliases[name] = t.get("alias")
                    continue
                bits = t.get("requires") or t.get("bitvalues")
                self.bitmask_bits[name] = bits
                if t.find("type") is not None and t.find("type").text == "VkFlags64":
                    self.bitmask_64.add(name)
            elif cat == "enum":
                if t.get("alias"):
                    self.enum_aliases[name] = t.get("alias")
            elif cat == "funcpointer":
                self.funcpointers.add(name)
            elif cat is None and t.get("requires") and t.get("requires") != "vk_platform":
                self.external.add(name)
        for alias, real in struct_aliases:
            while real in dict(struct_aliases):
                real = dict(struct_aliases)[real]
            self.struct_aliases[alias] = real
        # Resolve handle aliases.
        for alias, real in self.handle_aliases.items():
            self.handles[alias] = self.handles.get(real, False)

    def require_protect(self, req, default):
        """Protect for a <require> block: its own depends= may pull in a protected extension."""
        deps = req.get("depends")
        if not deps:
            return default
        for name in re.split(r"[+,()]", deps):
            p = self.ext_protect.get(name.strip())
            if p:
                return p
        return default

    def _parse_enums(self, root):
        for e in root.findall("enums"):
            name = e.get("name")
            kind = e.get("type")
            if kind not in ("enum", "bitmask"):
                continue
            en = Enum(name, kind == "bitmask", int(e.get("bitwidth") or 32))
            for v in e.findall("enum"):
                if not api_ok(v) or v.get("alias"):
                    continue
                if v.get("bitpos") is not None:
                    en.bits.append(EnumValue(v.get("name"), 1 << int(v.get("bitpos"))))
                else:
                    en.values.append(EnumValue(v.get("name"), int(v.get("value"), 0)))
            self.enums[name] = en

        # Extension / feature added values.
        def add_ext_enums(container, ext_number, ext_protect=None):
            for req in container.findall("require"):
                if not api_ok(req):
                    continue
                protect = self.require_protect(req, ext_protect)
                for v in req.findall("enum"):
                    ext = v.get("extends")
                    if not ext or v.get("alias") or not api_ok(v):
                        continue
                    en = self.enums.get(ext)
                    if en is None:
                        continue
                    if v.get("bitpos") is not None:
                        en.bits.append(EnumValue(v.get("name"), 1 << int(v.get("bitpos")), protect))
                    elif v.get("value") is not None:
                        en.values.append(EnumValue(v.get("name"), int(v.get("value"), 0), protect))
                    elif v.get("offset") is not None:
                        num = int(v.get("extnumber") or ext_number)
                        val = 1000000000 + (num - 1) * 1000 + int(v.get("offset"))
                        if v.get("dir") == "-":
                            val = -val
                        en.values.append(EnumValue(v.get("name"), val, protect))

        for f in root.findall("feature"):
            if api_ok(f):
                add_ext_enums(f, 0)
        for ext in root.find("extensions"):
            if "vulkan" in ext.get("supported", "").split(","):
                add_ext_enums(ext, int(ext.get("number")), self.ext_protect.get(ext.get("name")))

        # Deduplicate by value (keep the first, core name).
        for en in self.enums.values():
            seen = set()
            vals = []
            for v in en.values:
                if v.value not in seen:
                    seen.add(v.value)
                    vals.append(v)
            en.values = vals
            seen = set()
            bits = []
            for b in en.bits:
                if b.value not in seen:
                    seen.add(b.value)
                    bits.append(b)
            en.bits = bits

    def _parse_commands(self, root):
        commands = {}
        aliases = []
        for c in root.find("commands"):
            if not api_ok(c):
                continue
            if c.get("alias"):
                aliases.append((c.get("name"), c.get("alias")))
                continue
            proto = c.find("proto")
            name = proto.find("name").text
            ret = proto.find("type").text
            params = [Field(p) for p in c.findall("param") if api_ok(p)]
            commands[name] = Command(name, ret, params)
        for alias_name, target in aliases:
            if target in commands:
                base = commands[target]
                commands[alias_name] = Command(alias_name, base.ret, base.params, alias_of=target)
        self._all_commands = commands

    def _parse_requirements(self, root):
        required_cmds = {}   # name -> set(protect)
        required_types = {}  # name -> set(protect)

        def add(d, name, protect):
            d.setdefault(name, set()).add(protect)

        def walk(container, ext_protect):
            for req in container.findall("require"):
                if not api_ok(req):
                    continue
                protect = self.require_protect(req, ext_protect)
                for cmd in req.findall("command"):
                    add(required_cmds, cmd.get("name"), protect)
                for t in req.findall("type"):
                    add(required_types, t.get("name"), protect)

        for f in root.findall("feature"):
            if api_ok(f):
                walk(f, None)
        for ext in root.find("extensions"):
            if "vulkan" not in ext.get("supported", "").split(","):
                continue
            walk(ext, self.ext_protect.get(ext.get("name")))

        def resolve_protect(protects):
            return None if None in protects else sorted(p for p in protects if p)[0]

        # Types: struct protection propagates to structs only reachable from protected types.
        for name, protects in required_types.items():
            self.type_protect[name] = resolve_protect(protects)
        for name, st in self.structs.items():
            st.protect = self.type_protect.get(name, "VKINSP_UNUSED_TYPE")
        for name, en in self.enums.items():
            en.protect = self.type_protect.get(name)
            if en.protect is None and name not in required_types:
                # Bit enums are often only required via their Flags type.
                flags = [f for f, b in self.bitmask_bits.items() if b == name]
                if flags:
                    en.protect = self.type_protect.get(flags[0], "VKINSP_UNUSED_TYPE")
                else:
                    en.protect = "VKINSP_UNUSED_TYPE"

        for name, cmd in self._all_commands.items():
            if name not in required_cmds:
                continue
            cmd.protect = resolve_protect(required_cmds[name])
            first = cmd.dispatch_param
            if first is None or not self.handles.get(first.type, False):
                cmd.level = "global"
            elif first.type in DISPATCHABLE_INSTANCE:
                cmd.level = "instance"
            elif first.type in DISPATCHABLE_DEVICE:
                cmd.level = "device"
            else:
                print(f"note: skipping {name} (dispatch type {first.type})", file=sys.stderr)
                continue
            self.commands.append(cmd)
        self.commands.sort(key=lambda c: c.name)

    # ------------------------------------------------------------------ queries
    def category(self, type_name):
        """One of: struct, union, enum, bitmask, handle, basetype, funcpointer, external, scalar, unknown."""
        t = self.struct_aliases.get(type_name, type_name)
        if t in self.structs:
            return "union" if self.structs[t].is_union else "struct"
        t = self.enum_aliases.get(type_name, type_name)
        if t in self.bitmask_bits:
            return "bitmask"
        if t in self.enums or t.endswith("FlagBits") or "FlagBits" in t:
            return "enum"
        if type_name in self.handles:
            return "handle"
        if type_name in self.basetypes:
            return "basetype"
        if type_name in self.funcpointers:
            return "funcpointer"
        if type_name in self.external:
            return "external"
        if type_name in SCALAR_TYPES:
            return "scalar"
        return "unknown"

    def real_struct(self, type_name):
        return self.structs.get(self.struct_aliases.get(type_name, type_name))

    def real_enum_name(self, type_name):
        return self.enum_aliases.get(type_name, type_name)
