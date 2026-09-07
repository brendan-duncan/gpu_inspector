#!/usr/bin/env python3
"""
Generates the Vulkan layer's dispatch tables, forwarding entry points and JSON serializers
from vk.xml. See tools/vkgen/*.py for the individual emitters.

Usage: python gen_vulkan.py --xml third_party/Vulkan-Headers/registry/vk.xml --out layer/gen
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vkgen.registry import Registry
from vkgen import dispatch, serialize


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xml", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    reg = Registry(args.xml)
    cmds = dispatch.emit(reg, args.out)
    serialize.emit(reg, cmds, args.out)

    n = {lvl: sum(1 for c in cmds if c.level == lvl) for lvl in ("global", "instance", "device")}
    print(f"generated {len(cmds)} commands ({n['global']} global, {n['instance']} instance, "
          f"{n['device']} device), {len(reg.structs)} structs, {len(reg.enums)} enums -> {args.out}")


if __name__ == "__main__":
    main()
