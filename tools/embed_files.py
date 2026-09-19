#!/usr/bin/env python3
"""
Embeds text files into a C++ source as string pieces, for code that writes them out again: the
exported project's hand-written files (src/replay/export_template, see src/replay/src/exporter.h).

Usage: python embed_files.py --out <file.cpp> --symbol kExportTemplates <file | name=file>...

Each file becomes raw string literal pieces cut at line boundaries, none longer than a compiler's
single-literal limit, listed under the file's base name, or under `name` (which may hold
directories: vulkan_headers/vulkan/vulkan.h) when given as name=file.
"""
import argparse
import os

PIECE_BYTES = 8000
DELIMITER = "~~"


def pieces(text):
    out = []
    current = ""
    for line in text.splitlines(keepends=True):
        if current and len(current) + len(line) > PIECE_BYTES:
            out.append(current)
            current = ""
        current += line
    if current or not out:
        out.append(current)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--symbol", default="kEmbeddedFiles")
    ap.add_argument("--namespace", default="vkreplay")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    lines = ["// GENERATED FILE - do not edit. Produced by tools/embed_files.py from an export_template directory.",
             "#include <cstddef>", "", f"namespace {args.namespace} {{", "",
             "struct EmbeddedFile { const char* name; const char* const* pieces; size_t count; };", ""]
    entries = []
    for i, spec in enumerate(args.files):
        # name=file; a Windows path's drive colon is no separator, and no path here holds an equals sign.
        listed, _, path = spec.partition("=") if "=" in spec else (os.path.basename(spec), "", spec)
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read().replace("\r\n", "\n")
        close = ")" + DELIMITER + '"'
        if close in text:
            raise SystemExit(f"{path} contains the raw string delimiter {close}")
        name = f"kPieces_{i}"
        lines.append(f"static const char* const {name}[] = {{")
        for piece in pieces(text):
            lines.append(f'    R"{DELIMITER}({piece}){DELIMITER}",')
        lines.append("};")
        entries.append(f'    {{ "{listed}", {name}, sizeof({name}) / sizeof({name}[0]) }},')
    lines += ["", f"extern const EmbeddedFile {args.symbol}[];", f"extern const size_t {args.symbol}Count;",
              f"const EmbeddedFile {args.symbol}[] = {{"] + entries + ["};",
              f"const size_t {args.symbol}Count = sizeof({args.symbol}) / sizeof({args.symbol}[0]);", "",
              f"}} // namespace {args.namespace}", ""]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
