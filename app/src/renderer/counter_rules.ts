// Frame Issues rules over the GPU counters, for both APIs.
//
// The per-command rules read what the application asked for; these read what the GPU actually
// did. Each is a threshold on one of the figures pass_metrics.ts divides out of the counters the
// capture backend sampled around every pass, and each is silent when the counter behind it is
// absent — a GPU without a statistics counter set, or a capture taken without "Profile passes".
//
//   high-overdraw          a pass shading each pixel far more than once
//   microtriangles         triangles too small for the rasterizer's 2x2 quad
//   late-depth-rejection   a pass that overdraws while its depth test rejects almost nothing
//
// The last needs the count of fragments that survived the depth test, which Metal's statistic
// set has and Vulkan's pipeline statistics do not, so it is quiet on a Vulkan capture.
import { LOW_REJECTION_RATE, MICROTRIANGLE_LIMIT, OVERDRAW_LIMIT, collectPassMetrics, formatRatio } from "./pass_metrics.js";
import type { FrameFinding } from "./vulkan/frame_analysis.js";
import type { ObjectLookup } from "./vulkan/vulkan_object.js";
import type { CaptureData } from "./capture_data.js";

export const COUNTER_RULES = ["high-overdraw", "microtriangles", "late-depth-rejection"];

/** One finding per rule, naming the first pass it applies to and counting the rest. */
class Folded {
  first: number | null = null;
  count = 0;
  commands: number[] = [];
  add(commandIndex: number): void {
    if (this.first === null) this.first = commandIndex;
    this.count++;
    if (this.commands.length < 64) this.commands.push(commandIndex);
  }
}

export function analyzeCounters(data: CaptureData, db: ObjectLookup): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } {
  const findings: FrameFinding[] = [];
  const byCommand = new Map<number, FrameFinding[]>();
  const metrics = collectPassMetrics(data, db);

  const overdrawn = new Folded();
  const micro = new Folded();
  const shadedThenDropped = new Folded();
  let worstOverdraw = 0;
  let worstFragments = Infinity;
  for (const p of metrics.passes) {
    if (p.overdraw !== null && p.overdraw > OVERDRAW_LIMIT) {
      overdrawn.add(p.commandIndex);
      worstOverdraw = Math.max(worstOverdraw, p.overdraw);
    }
    if (p.fragmentsPerPrimitive !== null && p.fragmentsPerPrimitive < MICROTRIANGLE_LIMIT) {
      micro.add(p.commandIndex);
      worstFragments = Math.min(worstFragments, p.fragmentsPerPrimitive);
    }
    if (p.depthRejectRate !== null && p.overdraw !== null && p.overdraw > 1.5 && p.depthRejectRate < LOW_REJECTION_RATE) {
      shadedThenDropped.add(p.commandIndex);
    }
  }

  const add = (rule: string, severity: FrameFinding["severity"], confidence: FrameFinding["confidence"],
               message: string, folded: Folded): void => {
    if (!folded.count) return;
    const f: FrameFinding = { rule, severity, confidence, message, commandIndex: folded.first ?? undefined, count: folded.count };
    findings.push(f);
    for (const index of folded.commands) {
      const list = byCommand.get(index);
      if (list) list.push(f); else byCommand.set(index, [f]);
    }
  };

  const passWord = (n: number): string => `${n} pass${n === 1 ? "" : "es"}`;
  add("high-overdraw", "high", "high",
    `${passWord(overdrawn.count)} shade each pixel more than ${OVERDRAW_LIMIT} times over (worst ${formatRatio(worstOverdraw)}): stacked transparency, a full-screen effect drawn more than once, or opaque geometry drawn back to front.`,
    overdrawn);
  add("microtriangles", "high", "high",
    `${passWord(micro.count)} rasterize triangles covering fewer than ${MICROTRIANGLE_LIMIT} fragments each (worst ${formatRatio(worstFragments, 1)}): the 2x2 rasterization quad shades lanes that are then thrown away. Mesh level of detail at distance is the usual answer.`,
    micro);
  add("late-depth-rejection", "medium", "medium",
    `${passWord(shadedThenDropped.count)} overdraw while the depth test rejects little: fragments are shaded and then replaced. Drawing opaque geometry front to back, or a depth prepass, rejects that work before the fragment shader runs.`,
    shadedThenDropped);
  return { findings, byCommand };
}
