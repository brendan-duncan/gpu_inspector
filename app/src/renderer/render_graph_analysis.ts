// Performance rules over the render graph (render_graph.ts), the counterpart of the per-command
// rules in vulkan/frame_analysis.ts and metal/frame_analysis.ts.
//
// Those rules read one pass, or a pass and the one before it, and answer "does anything read
// this?" with a proxy: the Vulkan pass rules read the image's *usage flags* (a sampled bit means
// something could read it, not that anything did), the Metal ones keep a read set per whole
// texture and unversioned, which a mip chain or a target written twice in a frame defeats. The
// graph knows the answer exactly — a version of a subresource with no readers — so these rules
// state it instead of approximating it, and the proxies they replace are listed in
// SUPERSEDED_RULES so a finding is not reported twice in two wordings.
//
// The rest are things the graph makes expressible at all: a write thrown away before anything
// read it, a resource that never has to leave the pass that produced it, two passes that are one
// pass, and a barrier synchronizing a resource the frame does not use on both sides of it.
//
// These rules are API-neutral, because the graph is. Vulkan and Metal share them; only the advice
// each finding gives names an API's own spelling of the fix.
import { usageClass } from "./render_graph.js";
import { SEVERITY_RANK, type Confidence, type Severity } from "./vulkan/spirv_analysis.js";
import type { FrameFinding } from "./vulkan/frame_analysis.js";
import type { GraphNode, GraphUse, RenderGraph } from "./render_graph.js";

/**
 * Rules of the per-command analyses that these replace: the graph answers the same question
 * exactly, so the capture panel drops these when a graph is available.
 *
 *   depth-store / color-store   "stored although nothing reads it" — from usage flags (Vulkan) or
 *                               a per-texture read set (Metal); unread-store below is per version.
 *   mergeable-passes            Metal's adjacent-pass check, which the graph does for both APIs
 *                               and against what the second pass actually loads.
 */
export const SUPERSEDED_RULES = new Set(["depth-store", "color-store", "mergeable-passes"]);

/** Sort order of the graph rules within a severity, most actionable first. */
const RULE_ORDER = ["overwritten-before-read", "mergeable-passes", "unread-store", "transient-candidate", "oversynchronized-barrier"];

/** One finding per rule, naming the first case and counting the rest (as the other analyses fold). */
class Folded {
  first: GraphNode | null = null;
  count = 0;
  nodes: GraphNode[] = [];
  /** What the finding names: the first few resources involved. */
  subjects: string[] = [];

  add(node: GraphNode, subject: string): void {
    if (!this.first) this.first = node;
    this.count++;
    if (this.nodes.length < 64) this.nodes.push(node);
    if (this.subjects.length < 3 && !this.subjects.includes(subject)) this.subjects.push(subject);
  }

  /** "Bloom mip 1, Bloom mip 2 and 4 more". */
  get subjectText(): string {
    const rest = this.count - this.subjects.length;
    const names = this.subjects.join(", ");
    return rest > 0 ? `${names} and ${rest} more` : names;
  }
}

class GraphAnalysis {
  private _graph: RenderGraph;
  private _findings: FrameFinding[] = [];
  private _byCommand = new Map<number, FrameFinding[]>();
  /**
   * Something in the frame reads through a binding the capture could not resolve, so "nothing
   * reads this" is a statement about what the graph can see. Every rule that rests on it drops a
   * confidence level and says so.
   */
  private _blind = false;
  /** Which API's spelling of a fix the advice should name. */
  private _metal = false;

  constructor(graph: RenderGraph) {
    this._graph = graph;
    this._blind = graph.nodes.some((n) => n.unresolvedReads > 0);
    this._metal = graph.api === "metal";
  }

  analyze(): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } {
    this._unreadStores();
    const merged = this._mergeablePasses();
    this._overwrittenBeforeRead();
    this._transientCandidates(merged);
    this._oversynchronizedBarriers();
    this._findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity] || RULE_ORDER.indexOf(a.rule) - RULE_ORDER.indexOf(b.rule));
    return { findings: this._findings, byCommand: this._byCommand };
  }

  // -------------------------------------------------------------------------------- the rules

  /**
   * A pass stores an attachment to memory that no later pass reads, and that is not the image
   * being presented. On a tiled GPU the store is the expensive part of a pass, so a store nothing
   * consumes is pure bandwidth, and the fix is a store op away.
   *
   * Attachments only. A storage buffer or image nothing reads is also worth knowing, but the
   * advice would be different (drop the work, not the store), the graph counts every storage
   * binding as written whether the shader writes it or not, and the frame summary already reports
   * those passes as ones nothing reads.
   */
  private _unreadStores(): void {
    const folded = new Folded();
    for (const node of this._graph.nodes) {
      for (const write of node.writes) {
        if (usageClass(write.usage) !== "attachment") continue;
        if (write.dropped || write.resource.presented) continue;      // discarded, or the frame's output
        if (write.resolved) continue;                                 // the msaa-store rule's case
        if (write.version.readers.length) continue;
        if (node.unresolvedReads && node.writes.length === 1) continue;
        folded.add(node, write.resource.label);
      }
    }
    if (!folded.count) return;
    this._add("unread-store", "medium", this._blind ? "medium" : "high",
      `${count(folded.count, "write")} in the frame ${folded.count === 1 ? "reaches" : "reach"} memory that no later pass reads: ${folded.subjectText}. ` +
      `Discarding instead (${this._metal ? "MTLStoreActionDontCare" : "store op DONT_CARE"}) keeps the result in tile memory and skips the write. ` +
      `The graph only sees this capture, so a result the host reads back or the next frame consumes will look unread here${this._blindClause()}.`, folded);
  }

  /**
   * A version is replaced by a write that keeps nothing of it, and nothing read it in between:
   * the work that produced it was thrown away. A clear over a pass' output, a target written
   * twice, a compute pass whose result the next dispatch overwrites.
   */
  private _overwrittenBeforeRead(): void {
    const folded = new Folded();
    for (const resource of this._graph.resources) {
      for (let i = 0; i < resource.versions.length - 1; i++) {
        const version = resource.versions[i];
        const next = resource.versions[i + 1];
        if (!version.producer || version.readers.length || version.dropped) continue;
        // Only when the next write keeps nothing: a partial write (a copy into a region, an
        // attachment loaded and added to) still depends on what was there.
        const replaces = next.producer?.writes.find((w) => w.version === next)?.discards;
        if (!replaces) continue;
        folded.add(version.producer, `${resource.label} (${next.producer?.label ?? "a later pass"} replaces it)`);
      }
    }
    if (!folded.count) return;
    this._add("overwritten-before-read", "high", this._blind ? "medium" : "high",
      `${count(folded.count, "pass", "passes")} ${folded.count === 1 ? "writes a result that is" : "write results that are"} replaced before anything reads ${folded.count === 1 ? "it" : "them"}: ${folded.subjectText}. ` +
      `The work is done and thrown away — the pass can be dropped, or the write that replaces it can be dropped and the two merged${this._blindClause()}.`, folded);
  }

  /**
   * Two adjacent render passes where the second loads what the first stored: one pass keeps the
   * attachment in tile memory instead of storing and re-loading it. Returns the resources it
   * reported, so the transient rule does not say the same thing about them in other words.
   */
  private _mergeablePasses(): Set<string> {
    const folded = new Folded();
    const reported = new Set<string>();
    const nodes = this._graph.nodes;
    for (let i = 1; i < nodes.length; i++) {
      const before = nodes[i - 1];
      const after = nodes[i];
      if (before.kind !== "render" || after.kind !== "render") continue;
      // Attachments the second pass loads that the first pass stored, and that nothing between
      // them touched — they are adjacent, so there is nothing between them by construction.
      const carried = after.writes.filter((w) =>
        usageClass(w.usage) === "attachment" && !w.discards &&
        w.version.index > 1 && this._producedBy(w, before));
      if (!carried.length) continue;
      // Both passes must render to the same targets, or the merge changes what is drawn where.
      if (!sameTargets(before, after)) continue;
      for (const w of carried) reported.add(w.resource.key);
      folded.add(after, carried.map((w) => w.resource.label).join(", "));
    }
    if (!folded.count) return reported;
    this._add("mergeable-passes", "medium", "medium",
      `${count(folded.count, "pass", "passes")} load exactly what the pass immediately before stored, to the same targets: ${folded.subjectText}. ` +
      `Recorded as one pass (a second subpass, or simply more draws) the attachment stays in tile memory and the store and load both go away.`, folded);
    return reported;
  }

  /**
   * A resource that is written and then consumed only by the very next pass, never presented and
   * never copied out: it never needs to exist in memory at all. On Vulkan that is
   * TRANSIENT_ATTACHMENT with LAZILY_ALLOCATED memory or an input attachment; on Metal,
   * MTLStorageModeMemoryless.
   */
  private _transientCandidates(merged: Set<string>): void {
    const folded = new Folded();
    for (const resource of this._graph.resources) {
      if (resource.type !== "image" || resource.presented || merged.has(resource.key)) continue;
      if (resource.externalInput) continue;             // it carries something in from before the frame
      let stored = false;
      let consumed = false;
      let local = true;
      for (const version of resource.versions) {
        if (!version.producer) continue;
        const write = version.producer.writes.find((w) => w.version === version);
        if (write && !write.dropped) stored = true;
        for (const reader of version.readers) {
          consumed = true;
          // "Local" means the consumer runs immediately after the producer, so nothing else could
          // have needed the contents in between.
          if (reader.ordinal !== version.producer.ordinal + 1) local = false;
        }
      }
      // Never consumed is unread-store's finding, and never stored is already what the transient
      // and memoryless rules of the per-API analyses look for.
      if (!stored || !consumed || !local) continue;
      if (resource.uses.some((u) => usageClass(u.usage) === "transfer")) continue;   // copied somewhere
      const producer = resource.versions.find((v) => v.producer)?.producer;
      if (producer) folded.add(producer, resource.label);
    }
    if (!folded.count) return;
    this._add("transient-candidate", "medium", this._blind ? "low" : "medium",
      `${count(folded.count, "image")} ${folded.count === 1 ? "is" : "are"} written and then read only by the pass that follows, and never presented or copied: ${folded.subjectText}. ` +
      `A target used that way never has to reach memory: ${this._metal ? "MTLStorageModeMemoryless, or an imageblock read in the second pass" : "TRANSIENT_ATTACHMENT usage with LAZILY_ALLOCATED memory, or an input attachment in a second subpass"}${this._blindClause()}.`, folded);
  }

  /**
   * A barrier that names resources the frame does not use on both sides of it. Only barriers that
   * do nothing else are considered: a layout transition or a queue-family transfer is required
   * whatever the data does, and a global memory barrier names nothing to check.
   */
  private _oversynchronizedBarriers(): void {
    const graph = this._graph;
    if (!graph.syncPoints.length) return;
    // Which node ordinals touched each resource, for "used before" and "used after".
    const touched = new Map<string, number[]>();
    for (const resource of graph.resources) {
      touched.set(resource.key, resource.uses.map((u) => u.node.ordinal));
    }
    const findings: { commandIndex: number; resources: string[] }[] = [];
    for (const sync of graph.syncPoints) {
      if (sync.structural || !sync.resources.length) continue;
      const idle = sync.resources.filter((key) => {
        const ordinals = touched.get(key);
        if (!ordinals || !ordinals.length) return true;      // named but never used in the frame
        return !ordinals.some((o) => o <= sync.after) || !ordinals.some((o) => o > sync.after);
      });
      if (idle.length === sync.resources.length) findings.push({ commandIndex: sync.commandIndex, resources: idle });
    }
    if (!findings.length) return;
    const first = findings[0];
    const f: FrameFinding = {
      rule: "oversynchronized-barrier", severity: "low", confidence: "medium",
      message: `${count(findings.length, "barrier")} synchronize only resources the frame does not both write before and use after them, and change no image layout or queue family. ` +
        `A barrier that guards nothing still costs a pipeline stall. The graph sees this capture only, so a barrier making a host write visible, or ordering against another frame or queue, will look idle here${this._blindClause()}.`,
      commandIndex: first.commandIndex, count: findings.length,
    };
    this._findings.push(f);
    for (const entry of findings) this._attach(entry.commandIndex, f);
  }

  // ------------------------------------------------------------------------------- mechanics

  /** True when `use`'s version was produced by `node` (its immediately preceding version). */
  private _producedBy(use: GraphUse, node: GraphNode): boolean {
    const previous = use.resource.versions[use.version.index - 1];
    return !!previous && previous.producer === node;
  }

  private _blindClause(): string {
    return this._blind ? ", and some of the frame's bindings could not be resolved to a resource at all" : "";
  }

  private _add(rule: string, severity: Severity, confidence: Confidence, message: string, folded: Folded): void {
    const f: FrameFinding = { rule, severity, confidence, message, commandIndex: folded.first?.commandIndex, count: folded.count };
    this._findings.push(f);
    for (const node of folded.nodes) this._attach(node.commandIndex, f);
  }

  private _attach(commandIndex: number, f: FrameFinding): void {
    const list = this._byCommand.get(commandIndex);
    if (list) list.push(f); else this._byCommand.set(commandIndex, [f]);
  }
}

/** The attachments a pass renders to, as a comparable key. */
function targetKey(node: GraphNode): string {
  return node.writes.filter((w) => usageClass(w.usage) === "attachment").map((w) => w.resource.key).sort().join("|");
}

function sameTargets(a: GraphNode, b: GraphNode): boolean {
  const key = targetKey(a);
  return key.length > 0 && key === targetKey(b);
}

function count(n: number, one: string, many = ""): string {
  return `${n} ${n === 1 ? one : many || `${one}s`}`;
}

/** The graph rules over a capture's render graph. */
export function analyzeRenderGraph(graph: RenderGraph): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } {
  return new GraphAnalysis(graph).analyze();
}
