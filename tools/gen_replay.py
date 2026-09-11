#!/usr/bin/env python3
"""
Generates the replay tool's decoders from vk.xml: the JSON the capture layer writes, read back into
Vulkan structs and command calls (see tools/vkgen/deserialize.py).

Usage: python gen_replay.py --xml third_party/Vulkan-Headers/registry/vk.xml --out replay/gen
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vkgen.registry import Registry
from vkgen import deserialize
from vkgen.dispatch import SKIP_COMMANDS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xml", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    reg = Registry(args.xml)
    cmds = [c for c in reg.commands if c.name not in SKIP_COMMANDS]
    deserialize.emit(reg, cmds, args.out)
    print(f"generated decoders for {len(cmds)} commands, {len(reg.structs)} structs, {len(reg.enums)} enums -> {args.out}")


if __name__ == "__main__":
    main()
