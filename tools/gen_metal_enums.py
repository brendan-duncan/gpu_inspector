#!/usr/bin/env python3
"""Generates the Metal replay's enum name tables from the Metal SDK headers.

The Metal counterpart of the enum half of gen_d3d12.py, and it exists for the same reason: the
capture writes a descriptor's enums into JSON, sometimes by name ("MTLPixelFormatBGRA8Unorm", from
the hand-written switches in src/metal/src/formats.mm) and sometimes as a bare number, and the
replay (src/metal/replay/) has to read both back. A switch cannot be read backwards, so what is
generated here is a table:

  metal_enums.gen.h / .cpp   {value, name} for every enum and option set of the Metal headers, with
                             EnumName (value -> name), EnumValue (name -> value) and WriteFlags.

The tables hold names and numbers only, never the SDK's own constants, so they compile against any
SDK — the same property the D3D12 tables have, and the reason both are committed rather than
generated into the build tree.

Metal 4 (MTL4*.h) is skipped: src/metal/ does not hook it, so nothing reaches a capture.

Usage: gen_metal_enums.py --out src/metal/gen [--sdk <MacOSX.sdk>]
"""
import argparse
import glob
import os
import re
import subprocess
import sys

# An enumerator's name may be followed by any of these before its "=", and they carry commas and
# parentheses of their own ("macos(10.11, 27.0)"), so they are cut depth-aware rather than by regex.
ATTRIBUTE_MACROS = ("API_AVAILABLE", "API_DEPRECATED", "API_DEPRECATED_WITH_REPLACEMENT",
                    "API_UNAVAILABLE", "NS_SWIFT_NAME", "NS_SWIFT_UNAVAILABLE", "NS_REFINED_FOR_SWIFT",
                    "API_OBSOLETED", "MTL_EXPORT")
CASTS = ("NSUInteger", "NSInteger", "uint64_t", "int64_t", "uint32_t", "int32_t", "unsigned long",
         "unsigned int", "unsigned", "long", "int")

# Foundation's limits, which a few enums use for their "unspecified" member. Not in the Metal
# headers, and the tables are for a 64-bit Mac.
SEED_SYMBOLS = {"NSUIntegerMax": (1 << 64) - 1, "NSIntegerMax": (1 << 63) - 1, "NSIntegerMin": -(1 << 63),
                "UINT_MAX": (1 << 32) - 1, "INT_MAX": (1 << 31) - 1, "INT_MIN": -(1 << 31)}


def as_int64(value):
    """The value as it lands in an int64_t field: NSUIntegerMax reads back as -1, which is what C does."""
    value &= (1 << 64) - 1
    return value - (1 << 64) if value >= (1 << 63) else value


def sdk_path(given):
    if given:
        return given
    try:
        return subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def split_top_level(text, separator=","):
    """Splits on `separator` at parenthesis depth 0, outside string literals."""
    out, current, depth, quote = [], "", 0, ""
    i = 0
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\":
                current += text[i:i + 2]
                i += 2
                continue
            if c == quote:
                quote = ""
        elif c in "\"'":
            quote = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == separator and depth == 0:
            out.append(current)
            current = ""
            i += 1
            continue
        current += c
        i += 1
    out.append(current)
    return out


def cut_attributes(text):
    """Removes API_AVAILABLE(...) and its relatives, with whatever they hold."""
    for macro in ATTRIBUTE_MACROS:
        while True:
            m = re.search(r"\b" + macro + r"\b", text)
            if not m:
                break
            rest = text[m.end():].lstrip()
            if not rest.startswith("("):
                text = text[:m.start()] + text[m.end():]
                continue
            start = m.end() + (len(text[m.end():]) - len(rest))
            depth, i, quote = 0, start, ""
            while i < len(text):
                c = text[i]
                if quote:
                    if c == "\\":
                        i += 2
                        continue
                    if c == quote:
                        quote = ""
                elif c in "\"'":
                    quote = c
                elif c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            text = text[:m.start()] + text[i + 1:]
    return text


def clean_expression(text):
    text = cut_attributes(text).strip()
    for cast in CASTS:
        text = text.replace("(" + cast + ")", "")
    # 0x20UL, 1ULL, 8u -> the number alone; Python has no integer suffixes.
    text = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", text)
    return text.strip()


def collect_defines(text, symbols):
    for name, value in re.findall(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+([^\n\\]+)$", text, flags=re.M):
        expression = clean_expression(value)
        if not expression or re.search(r"[A-Za-z_]\w*\s*\(", expression):
            continue
        try:
            symbols[name] = int(eval(expression, {"__builtins__": {}}, dict(symbols)))
        except Exception:
            pass


def find_enums(text):
    """Every `typedef NS_ENUM/NS_OPTIONS(type, Name) { body }`, as (name, [(enumerator, expression)])."""
    out = []
    for m in re.finditer(r"\bNS_(?:ENUM|OPTIONS)\s*\(\s*[A-Za-z_]\w*\s*,\s*([A-Za-z_]\w*)\s*\)", text):
        brace = text.find("{", m.end())
        if brace < 0:
            continue
        depth, i = 0, brace
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        items = []
        for item in split_top_level(text[brace + 1:i]):
            item = item.strip()
            if not item:
                continue
            halves = split_top_level(item, "=")
            name_part = cut_attributes(halves[0]).strip()
            name = re.match(r"^([A-Za-z_]\w*)", name_part)
            if not name:
                continue
            items.append((name.group(1), clean_expression("=".join(halves[1:])) if len(halves) > 1 else None))
        if items:
            out.append((m.group(1), items))
    return out


def resolve(enums, symbols):
    """Evaluates every enumerator, repeating while anything still resolves (values cross headers)."""
    pending = [(name, list(items)) for name, items in enums]
    resolved = {}
    while True:
        progress = False
        for name, items in pending:
            if name in resolved:
                continue
            values, previous, complete = [], -1, True
            # An enum's later entries name its earlier ones — every deprecated spelling in the
            # Metal headers is `MTLArgumentAccessReadOnly = MTLBindingAccessReadOnly` — so the
            # scope grows as the body is walked rather than only between enums.
            scope = dict(symbols)
            for enumerator, expression in items:
                if expression is None:
                    value = previous + 1
                else:
                    try:
                        value = int(eval(expression, {"__builtins__": {}}, scope))
                    except Exception:
                        complete = False
                        break
                scope[enumerator] = value
                values.append((enumerator, value))
                previous = value
            if not complete:
                continue
            resolved[name] = values
            for enumerator, value in values:
                symbols[enumerator] = value
            progress = True
        if not progress:
            break
    return resolved, [name for name, _ in pending if name not in resolved]


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


HEADER_PREAMBLE = """// Generated by tools/gen_metal_enums.py from the Metal SDK headers: do not edit.
// Name tables of every Metal enum and option set, for the replay's decoder and its source emitter
// (src/metal/replay/). Values and names only, so they compile against any SDK.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mtlinsp {

struct EnumEntry { int64_t value; const char* name; };

/** The name of `value` in the table, or nullptr. */
const char* EnumName(const EnumEntry* entries, size_t count, int64_t value);
/**
 * The value of `name` in the table; false when it names nothing.
 *
 * Both spellings are accepted: Metal's own ("MTLTextureUsageShaderRead") and the short one the
 * capture writes for a flag set ("ShaderRead", UsageFlags in src/metal/src/hooks_descriptors.mm),
 * which is the table's name with the enum's own prefix taken off.
 */
bool EnumValue(const EnumEntry* entries, size_t count, std::string_view name, int64_t& value);

"""

SOURCE_PREAMBLE = """// Generated by tools/gen_metal_enums.py from the Metal SDK headers: do not edit.
#include "metal_enums.gen.h"

#include <cstring>

namespace mtlinsp {

const char* EnumName(const EnumEntry* entries, size_t count, int64_t value) {
    for (size_t i = 0; i < count; ++i)
        if (entries[i].value == value) return entries[i].name;
    return nullptr;
}

bool EnumValue(const EnumEntry* entries, size_t count, std::string_view name, int64_t& value) {
    for (size_t i = 0; i < count; ++i) {
        if (name == entries[i].name) {
            value = entries[i].value;
            return true;
        }
    }
    // The short spelling: the entry's name ends with it, and what comes before is the enum's
    // prefix, which every entry of the table shares.
    for (size_t i = 0; i < count; ++i) {
        const size_t length = std::strlen(entries[i].name);
        if (length <= name.size()) continue;
        if (std::string_view(entries[i].name + (length - name.size()), name.size()) != name) continue;
        value = entries[i].value;
        return true;
    }
    return false;
}

"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--sdk", default="")
    args = ap.parse_args()

    sdk = sdk_path(args.sdk)
    headers_dir = os.path.join(sdk, "System/Library/Frameworks/Metal.framework/Headers")
    if not os.path.isdir(headers_dir):
        sys.exit(f"no Metal headers in {headers_dir!r}: pass --sdk with a MacOSX.sdk")

    symbols, enums = dict(SEED_SYMBOLS), []
    # MTL4*.h is Metal 4, which src/metal/ does not hook.
    for path in sorted(glob.glob(os.path.join(headers_dir, "MTL*.h"))):
        if os.path.basename(path).startswith("MTL4"):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            text = strip_comments(f.read())
        collect_defines(text, symbols)
        enums += find_enums(text)

    resolved, unresolved = resolve(enums, symbols)
    order = [name for name, _ in enums if name in resolved]

    header = [HEADER_PREAMBLE]
    source = [SOURCE_PREAMBLE]
    for name in order:
        entries = resolved[name]
        header.append(f"extern const EnumEntry kEnum_{name}[{len(entries)}];\n"
                      f"inline const char* ToString_{name}(int64_t v) {{ return EnumName(kEnum_{name}, {len(entries)}, v); }}\n")
        source.append(f"const EnumEntry kEnum_{name}[{len(entries)}] = {{")
        source += [f"    {{ {as_int64(value)}, {c_string(enumerator)} }}," for enumerator, value in entries]
        source.append("};\n")
    header.append("} // namespace mtlinsp\n")
    source.append("} // namespace mtlinsp\n")

    os.makedirs(args.out, exist_ok=True)
    for path, lines in ((os.path.join(args.out, "metal_enums.gen.h"), header),
                        (os.path.join(args.out, "metal_enums.gen.cpp"), source)):
        with open(path, "w", newline="\n", encoding="utf-8") as f:
            f.write("\n".join(lines))

    print(f"{len(order)} enums, {sum(len(v) for v in resolved.values())} entries -> {args.out}")
    if unresolved:
        print(f"  unresolved (left out): {', '.join(sorted(unresolved))}")


if __name__ == "__main__":
    main()
