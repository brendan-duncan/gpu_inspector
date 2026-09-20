// What the threads were doing during a timing capture: call stacks sampled by the capture library
// a few hundred times a second, each filed under the frame it fell in (src/vulkan/src/cpu_sampler.h).
//
// frame_timing.ts finds the hitch and names the timed call that explains it, when one does. When
// none does — "the application's own work between them" — this is where the answer is: for the
// frames asked about, which thread was busy and in what, and which was blocked and on what. The
// second is as useful as the first. A render thread that spent the hitch waiting is not the cause
// of it; the thread it was waiting for is, and its stack in the same frames says what it was doing.
//
// No DOM here.
import type { TimingSamplesMessage } from "../shared/protocol.js";

export interface SampledThread {
  id: number;
  name?: string;
}

/** [frame, thread (an index into threads), stack id, running, samples] as the library sends them. */
export type SampleRecord = [number, number, number, number, number];

export interface TimingSamples {
  periodMs: number;
  threads: SampledThread[];
  /** Stack id -> addresses ("0x..."), innermost first. */
  stacks: Map<number, string[]>;
  samples: SampleRecord[];
  dropped: number;
}

export function emptyTimingSamples(): TimingSamples {
  return { periodMs: 0, threads: [], stacks: new Map(), samples: [], dropped: 0 };
}

/** Records kept: at 250 Hz and a dozen threads, around twenty minutes, which is what the frames keep. */
export const MAX_SAMPLE_RECORDS = 1 << 21;

export function appendTimingSamples(store: TimingSamples, msg: TimingSamplesMessage): void {
  if (msg.periodMs > 0) store.periodMs = msg.periodMs;
  // The whole list each time, so that an index means the same thread in every batch.
  if (msg.threads?.length) store.threads = msg.threads.map((t) => ({ id: t.id, ...(t.name ? { name: t.name } : {}) }));
  for (const s of msg.stacks ?? []) store.stacks.set(s.id, s.addresses);
  for (const s of msg.samples ?? []) store.samples.push(s);
  if (typeof msg.dropped === "number") store.dropped = msg.dropped;
  if (store.samples.length > MAX_SAMPLE_RECORDS) store.samples.splice(0, store.samples.length - MAX_SAMPLE_RECORDS);
}

/** A stack a thread was sampled under, with how often. */
export interface StackShare {
  stack: number;
  addresses: string[];
  samples: number;
  /** Of them, how many found the thread running rather than blocked. */
  running: number;
}

export interface ThreadActivity {
  thread: SampledThread;
  samples: number;
  running: number;
  /** Milliseconds the samples stand for: their count times the sampling period. */
  runningMs: number;
  waitingMs: number;
  /** Where it ran, most first; and where it waited. */
  ranIn: StackShare[];
  waitedIn: StackShare[];
}

export interface SampleSummary {
  fromFrame: number;
  toFrame: number;
  samples: number;
  /** Busiest first: the threads that ran in the stretch, then the ones that were blocked through it, longest first. */
  threads: ThreadActivity[];
  /** Threads that never ran in the whole capture, which the view folds into one line. */
  idle: number;
}

/**
 * What each thread did over the frames `fromFrame`..`toFrame` (inclusive, the application's own
 * frame numbers). Null when no sample fell in them.
 */
export function summarizeSamples(store: TimingSamples, fromFrame: number, toFrame: number, top = 3): SampleSummary | null {
  const perThread = new Map<number, Map<string, StackShare>>();
  // A process has dozens of threads parked in a pool or a driver, and they are nobody's answer.
  // What tells them from a render thread that spent this hitch blocked — which is very much the
  // answer — is the rest of the capture: the one works in other frames, the others never do.
  const everRan = new Set<number>();
  let total = 0;
  for (const [frame, thread, stack, running, count] of store.samples) {
    if (running) everRan.add(thread);
    if (frame < fromFrame || frame > toFrame) continue;
    total += count;
    let stacks = perThread.get(thread);
    if (!stacks) perThread.set(thread, (stacks = new Map()));
    // Running and waiting under the same stack are different findings, so they are kept apart.
    const key = `${stack}:${running}`;
    let share = stacks.get(key);
    if (!share) stacks.set(key, (share = { stack, addresses: store.stacks.get(stack) ?? [], samples: 0, running: 0 }));
    share.samples += count;
    if (running) share.running += count;
  }
  if (!total) return null;
  const period = store.periodMs || 4;
  const threads: ThreadActivity[] = [];
  let idle = 0;
  for (const [index, stacks] of perThread) {
    if (!everRan.has(index)) {
      idle++;
      continue;
    }
    const all = [...stacks.values()];
    const samples = all.reduce((n, s) => n + s.samples, 0);
    const running = all.reduce((n, s) => n + s.running, 0);
    const by = (s: StackShare[]): StackShare[] => s.sort((a, b) => b.samples - a.samples).slice(0, top);
    threads.push({
      thread: store.threads[index] ?? { id: 0 },
      samples, running, runningMs: running * period, waitingMs: (samples - running) * period,
      ranIn: by(all.filter((s) => s.running > 0)), waitedIn: by(all.filter((s) => s.running === 0)),
    });
  }
  threads.sort((a, b) => b.running - a.running || b.samples - a.samples);
  return { fromFrame, toFrame, samples: total, threads, idle };
}

/** The addresses a summary's stacks need symbols for, innermost `depth` frames of each. */
export function summaryAddresses(summary: SampleSummary, depth = 12): string[] {
  const out = new Set<string>();
  for (const t of summary.threads) for (const s of [...t.ranIn, ...t.waitedIn]) for (const a of s.addresses.slice(0, depth)) out.add(a);
  return [...out];
}

/** What is known of an address, as far as naming it goes. */
export interface NamedFrame {
  function?: string;
  module?: string;
  file?: string;
  line?: number;
  internal?: boolean;
}

/**
 * A stack in a line: the innermost frames that say something, innermost first. The capture
 * library's own frames are left out (a hooked wait shows the hook), and so are the frames under the
 * thread's start, which every stack of the process shares.
 */
export function stackText(addresses: string[], symbolOf: (address: string) => NamedFrame | undefined, frames = 5): string {
  const names: string[] = [];
  for (const a of addresses) {
    const f = symbolOf(a);
    if (f?.internal) continue;
    const fn = f?.function ?? "";
    if (/^(BaseThreadInitThunk|RtlUserThreadStart|invoke_main|__scrt_common_main(_seh)?|mainCRTStartup|WinMainCRTStartup)$/.test(fn)) continue;
    const where = f?.file && f.line ? ` (${f.file.replace(/^.*[\\/]/, "")}:${f.line})` : "";
    names.push(fn ? `${fn}${where}` : f?.module ? `${f.module}` : a);
    if (names.length >= frames) break;
  }
  // A run of frames with no symbols is one module named once.
  const folded = names.filter((n, i) => i === 0 || n !== names[i - 1]);
  return folded.join(" ← ") || "(no frames)";
}
