#!/usr/bin/env python3
"""
Debug client for the layer's TCP protocol. Connects to a running layer, prints a summary of
every message, and validates that JSON payloads parse.

Usage: python inspector_client.py [--port 47531] [--seconds 5] [--dump]
"""
import argparse
import collections
import json
import socket
import struct
import sys
import time


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def send_json(sock, obj):
    payload = json.dumps(obj).encode("utf-8")
    sock.sendall(struct.pack("<IB", len(payload), 0) + payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=47531)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--dump", action="store_true", help="print full messages")
    ap.add_argument("--retry", type=float, default=10, help="seconds to keep trying to connect")
    ap.add_argument("--capture", action="store_true", help="request a frame capture after connecting")
    ap.add_argument("--save", help="write captured command list JSON to this file")
    ap.add_argument("--record-always", action="store_true", help="enable recording of all command buffers")
    ap.add_argument("--delay", type=float, default=0, help="seconds to wait before requesting the capture")
    args = ap.parse_args()

    deadline = time.time() + args.retry
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", args.port), timeout=1)
            break
        except OSError:
            time.sleep(0.2)
    if not sock:
        print("could not connect", file=sys.stderr)
        return 1
    print(f"connected to {args.port}")
    sock.settimeout(1.0)
    send_json(sock, {"action": "Ping"})
    if args.record_always:
        send_json(sock, {"action": "Settings", "recordAlways": True})
    if args.capture:
        if args.delay:
            time.sleep(args.delay)
        send_json(sock, {"action": "Capture", "frameCount": 1})
    commands = []
    textures = []

    counts = collections.Counter()
    types = collections.Counter()
    bad = 0
    end = time.time() + args.seconds
    while time.time() < end:
        try:
            hdr = read_exact(sock, 5)
        except socket.timeout:
            continue
        if hdr is None:
            print("disconnected")
            break
        length, kind = struct.unpack("<IB", hdr)
        payload = read_exact(sock, length)
        if payload is None:
            break
        if kind == 0:
            try:
                msg = json.loads(payload.decode("utf-8"))
            except Exception as e:
                bad += 1
                print("BAD JSON:", e, payload[:200])
                continue
            action = msg.get("action")
            counts[action] += 1
            if action == "AddObject":
                types[msg.get("type")] += 1
            elif action == "CaptureFrameCommands":
                commands.extend(msg.get("commands", []))
            elif action == "CaptureTextureFrames":
                textures = msg.get("textures", [])
            if args.dump:
                print(json.dumps(msg)[:2000])
        else:
            (hl,) = struct.unpack("<I", payload[:4])
            header = json.loads(payload[4:4 + hl])
            counts["binary:" + header.get("action", "?")] += 1
            if args.dump:
                print("BINARY", header, len(payload) - 4 - hl, "bytes")

    print("messages:", dict(counts))
    print("object types:", dict(types))
    print("bad json:", bad)
    if commands:
        methods = collections.Counter(c.get("method") for c in commands)
        print(f"captured commands: {len(commands)}", dict(methods))
        for t in textures:
            print("  texture:", t)
        if args.save:
            with open(args.save, "w") as f:
                json.dump({"commands": commands, "textures": textures}, f, indent=1)
            print("saved", args.save)
    return 0 if bad == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
