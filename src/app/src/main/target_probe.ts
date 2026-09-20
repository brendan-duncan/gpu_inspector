// Finding the applications that can be attached to, for the attach list.
//
// A capture library listens on one port each, the first free one of a small range
// (src/vulkan/src/target_probe.h), so several applications started by hand are all reachable
// without anybody choosing port numbers. This walks that range and asks each listener what it is.
//
// The asking matters: these servers take one client at a time and replace the old connection, so a
// bare connect would throw a running session off its own socket. A probe therefore says what it is
// with its first frame, and the library answers it and closes without ever treating it as a
// client. Old capture libraries, which have no handshake, take the probe for a client and send a
// snapshot instead of an answer; they are reported as unknown rather than listed wrongly, and the
// probe closes at once.
import net from "node:net";

import { FrameReader, encodeRequest } from "./layer_protocol.js";
import { DEFAULT_PORT } from "./launch_env.js";

/** The ports a capture library may pick, matching kFirstPort/kPortCount in target_probe.h. */
export const FIRST_PORT = DEFAULT_PORT;
export const PORT_COUNT = 8;

/** How long one port has to answer, over loopback. */
const PROBE_TIMEOUT_MS = 500;

/** An application that a capture library is serving, as the attach list shows it. */
export interface InspectableTarget {
  port: number;
  /** "Vulkan", "D3D12" or "Metal"; "" from a library too old to answer a probe. */
  api: string;
  /** The application's own name for itself, empty when it has none (or has not started yet). */
  name: string;
  /** The executable's base name; the fallback identity, and never empty in practice. */
  exe: string;
  pid: number;
  /** Whether an inspector is attached already: attaching takes the connection from it. */
  busy: boolean;
}

/** What one port answered, or null when nothing there answers a probe. */
export function probePort(port: number, timeoutMs = PROBE_TIMEOUT_MS): Promise<InspectableTarget | null> {
  return new Promise((resolve) => {
    const sock = net.createConnection({ host: "127.0.0.1", port });
    let settled = false;
    const done = (target: InspectableTarget | null): void => {
      if (settled) return;
      settled = true;
      sock.destroy();
      resolve(target);
    };
    sock.setTimeout(timeoutMs, () => done(null));
    sock.once("error", () => done(null));
    sock.once("close", () => done(null));
    sock.once("connect", () => {
      sock.setNoDelay(true);
      sock.write(encodeRequest({ action: "Probe" }));
    });
    const reader = new FrameReader();
    sock.on("data", (chunk: Buffer) => {
      for (const msg of reader.push(chunk)) {
        const m = msg as unknown as Record<string, unknown>;
        // Anything other than the answer means a library that does not know the handshake and has
        // taken this for a client: say so, and get off the socket before it sends a whole snapshot.
        if (m.action !== "Target") {
          done({ port, api: "", name: "", exe: "", pid: 0, busy: false });
          return;
        }
        done({
          port,
          api: String(m.api ?? ""),
          name: String(m.name ?? ""),
          exe: String(m.exe ?? ""),
          pid: Number(m.pid ?? 0),
          busy: m.busy === true,
        });
        return;
      }
    });
  });
}

/**
 * Every application currently serving one of the ports, in port order. The whole range is probed
 * at once: eight loopback connections that nearly always refuse immediately.
 */
export async function listTargets(timeoutMs = PROBE_TIMEOUT_MS): Promise<InspectableTarget[]> {
  const ports = Array.from({ length: PORT_COUNT }, (_, i) => FIRST_PORT + i);
  const found = await Promise.all(ports.map((port) => probePort(port, timeoutMs)));
  return found.filter((t): t is InspectableTarget => t !== null);
}

/** How the attach list names a target: its own name when it has one, else the executable. */
export function targetDisplayName(t: InspectableTarget): string {
  const name = t.name || t.exe || `port ${t.port}`;
  return t.name && t.exe && t.exe !== t.name ? `${t.name} (${t.exe})` : name;
}
