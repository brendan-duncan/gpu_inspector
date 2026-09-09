"""
Removes the embedded source text from a SPIR-V module while keeping its file name and line
information, the shape most shipped shaders have (dxc with -Zi, glslang with -g and a stripped
build): OpSource loses its optional text operand and OpSourceContinued goes away; OpString,
OpLine and the NonSemantic debug lines stay. The inspector then needs a source root to show the
text. Used by the triangle test application's build for one of its shaders.

    python tools/strip_shader_source.py in.spv out.spv [--relative-to <dir>]

--relative-to makes the OpString file names under <dir> relative to it (a shipped shader names
its file relative to the project, not by the build machine's absolute path).
"""
import struct
import sys

OP_SOURCE_CONTINUED = 2
OP_SOURCE = 3
OP_STRING = 7


def encode_string(text):
    data = text.encode("utf-8") + b"\x00"
    data += b"\x00" * (-len(data) % 4)
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def decode_string(words):
    data = struct.pack(f"<{len(words)}I", *words)
    return data.split(b"\x00", 1)[0].decode("utf-8", "replace")


def strip(words, relative_to=None):
    out = words[:5]
    i = 5
    while i < len(words):
        wc = words[i] >> 16
        op = words[i] & 0xFFFF
        if wc == 0:
            raise ValueError("bad instruction word count")
        if op == OP_SOURCE_CONTINUED:
            i += wc
            continue
        if op == OP_STRING and relative_to:
            name = decode_string(words[i + 2:i + wc]).replace("\\", "/")
            base = relative_to.replace("\\", "/").rstrip("/") + "/"
            if name.lower().startswith(base.lower()):
                body = encode_string(name[len(base):])
                out.append(((2 + len(body)) << 16) | OP_STRING)
                out.append(words[i + 1])
                out.extend(body)
                i += wc
                continue
        if op == OP_SOURCE and wc > 4:
            # OpSource <language> <version> [file id] [text]: keep up to the file id.
            out.append((4 << 16) | OP_SOURCE)
            out.extend(words[i + 1:i + 4])
        else:
            out.extend(words[i:i + wc])
        i += wc
    return out


def main():
    args = sys.argv[1:]
    relative_to = None
    if "--relative-to" in args:
        at = args.index("--relative-to")
        relative_to = args[at + 1]
        del args[at:at + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    with open(args[0], "rb") as f:
        data = f.read()
    if len(data) % 4 or len(data) < 20:
        print("not a SPIR-V module", file=sys.stderr)
        return 1
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    if words[0] != 0x07230203:
        print("not a SPIR-V module (bad magic)", file=sys.stderr)
        return 1
    stripped = strip(words, relative_to)
    with open(args[1], "wb") as f:
        f.write(struct.pack(f"<{len(stripped)}I", *stripped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
