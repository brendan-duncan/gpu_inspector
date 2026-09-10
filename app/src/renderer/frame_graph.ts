// Turning a capture into the passes the render graph is built from.
//
// This is the API-neutral half of the extraction: it segments the command stream into passes the
// same way the capture panel's command tree does (so a graph node and a tree block are the same
// pass, with the same pass key and so the same GPU timing), and asks a per-API ResourceSource
// what each command touches. The Vulkan and Metal sources are in vulkan/frame_resources.ts and
// metal/frame_resources.ts; the graph model they feed is render_graph.ts.
import { buildRenderGraph, type NodeKind, type RawAccess, type RawPass, type RenderGraph } from "./render_graph.js";
import { isAction, type CommandSets } from "./command_sets.js";
import { passKey } from "./capture_data.js";
import type { CaptureData } from "./capture_data.js";
import { MetalResourceSource } from "./metal/frame_resources.js";
import { VulkanResourceSource } from "./vulkan/frame_resources.js";
import type { ObjectLookup } from "./vulkan/vulkan_object.js";
import type { CaptureCommand } from "../shared/protocol.js";

/**
 * What one graphics API contributes: the resources its commands name. The walk below feeds it
 * every command in order so it can track what is bound, then asks it what a pass or a draw
 * actually touches.
 */
export interface ResourceSource {
  /**
   * Every command, in execution order, before it is asked about. Sources track bound descriptor
   * sets or stage bindings here. `stream` identifies the command buffer (and secondary) the
   * command was recorded into: bindings do not carry across streams.
   */
  observe(cmd: CaptureCommand, stream: string): void;
  /**
   * The kind, label and attachment accesses of the pass `cmd` begins. Vulkan only ever begins a
   * render pass here; Metal begins compute and blit passes with a command of the same class, so
   * the kind comes from the source rather than from the walk.
   */
  passAccesses(cmd: CaptureCommand, ordinal: number): { kind: NodeKind; label: string; accesses: RawAccess[] } | null;
  /** What a draw, dispatch or trace reads through the state bound at it. */
  actionAccesses(cmd: CaptureCommand, stream: string): { accesses: RawAccess[]; unresolved: number };
  /** A transfer or clear command outside any pass, which becomes a node of its own; null if it is not one. */
  transferAccesses(cmd: CaptureCommand): { label: string; accesses: RawAccess[] } | null;
  /** Label for a compute pass (a run of dispatches) that the API does not name itself. */
  computePassLabel(ordinal: number): string;
}

/**
 * Folds the accesses collected for one pass: a pass with 500 draws that all sample the same
 * shadow map is one edge, not 500. The strongest mode wins, so a resource both read and written
 * by a pass ends up read-write, and the usages seen are joined ("sampled, storage").
 */
class AccessSet {
  private _byKey = new Map<string, RawAccess>();
  private _usages = new Map<string, Set<string>>();

  add(access: RawAccess): void {
    const key = access.resource.key;
    const existing = this._byKey.get(key);
    let usages = this._usages.get(key);
    if (!usages) this._usages.set(key, (usages = new Set()));
    usages.add(access.usage);
    if (!existing) {
      this._byKey.set(key, { ...access });
      return;
    }
    if (existing.mode !== access.mode) existing.mode = "readwrite";
    // A write that preserves the previous contents anywhere in the pass means the pass depends on
    // them, whatever the other accesses said; the same for a store that is not thrown away.
    if (!access.discards) existing.discards = false;
    if (!access.dropped) existing.dropped = false;
  }

  list(): RawAccess[] {
    const out: RawAccess[] = [];
    for (const [key, access] of this._byKey) {
      const usages = [...(this._usages.get(key) ?? [])].sort();
      out.push({ ...access, usage: usages.join(", ") });
    }
    return out;
  }
}

/** One pass being accumulated by the walk. */
interface OpenPass {
  pass: RawPass;
  accesses: AccessSet;
}

/**
 * Splits the frame into passes and collects what each touches.
 *
 * The segmentation mirrors CaptureView._renderFrame(): render passes are counted per command
 * buffer, compute passes are runs of dispatches outside a render pass (Vulkan has no command that
 * begins one, so barriers, debug labels, secondary execution and the end of the command buffer
 * close the run), and both are keyed with passKey() so a node can look its GPU timing up. Any
 * change to the grouping there belongs here too.
 */
export function collectPasses(data: CaptureData, sets: CommandSets, source: ResourceSource): RawPass[] {
  const passes: RawPass[] = [];
  const renderCounters = new Map<number, number>();
  const computeCounters = new Map<number, number>();
  // Held in an object rather than a plain local: the helpers below assign it, and a local would
  // be narrowed to null by the compiler across those calls.
  const state: { open: OpenPass | null } = { open: null };
  let inPass = false;
  let stream = "";
  let currentCb = -1;

  // Passes join the list when they open, not when they close, so that a transfer command
  // recorded between two dispatches of the same compute pass does not have to close it to keep
  // the list in execution order. Empty ones are dropped at the end.
  const finish = (): void => {
    if (!state.open) return;
    state.open.pass.accesses = state.open.accesses.list();
    state.open = null;
  };
  const begin = (pass: RawPass, accesses: AccessSet): void => {
    passes.push(pass);
    state.open = { pass, accesses };
  };

  let currentFrame = -1;
  for (const cmd of data.commands) {
    const objId = cmd.object?.__id ?? 0;
    // Pass indices count from zero again in each captured frame, because the capture library
    // resets them when a command buffer is re-begun and the command tree counts them the same way
    // (CaptureView._renderFrame). Counting on across frames would key a multi-frame capture's
    // later passes to timings that do not exist.
    if (cmd.frame !== currentFrame) {
      currentFrame = cmd.frame;
      renderCounters.clear();
      computeCounters.clear();
    }
    if (sets.SUBMIT.has(cmd.method)) {
      finish();
      inPass = false;
      currentCb = -1;
      continue;
    }
    if (objId !== currentCb) {
      finish();
      inPass = false;
      currentCb = objId;
    }
    stream = `${objId}:${cmd.secondary ?? 0}`;
    source.observe(cmd, stream);

    if (sets.PASS_BEGIN.has(cmd.method)) {
      finish();
      const ordinal = renderCounters.get(objId) ?? 0;
      renderCounters.set(objId, ordinal + 1);
      const decoded = source.passAccesses(cmd, ordinal);
      inPass = true;
      const accesses = new AccessSet();
      for (const a of decoded?.accesses ?? []) accesses.add(a);
      begin({
        kind: decoded?.kind ?? "render", label: decoded?.label ?? `Pass ${ordinal}`, commandIndex: cmd.index,
        passKey: passKey(cmd.frame, objId, ordinal), frame: cmd.frame, draws: 0, accesses: [], unresolvedReads: 0,
      }, accesses);
      continue;
    }
    if (sets.PASS_END.has(cmd.method)) {
      finish();
      inPass = false;
      continue;
    }
    // What ends a run of dispatches, matching the command tree's bracketing.
    if (!inPass && state.open?.pass.kind === "compute" &&
        (sets.COMPUTE_PASS_END.has(cmd.method) || cmd.method === "vkEndCommandBuffer" ||
         sets.LABEL_BEGIN.has(cmd.method) || sets.LABEL_END.has(cmd.method))) {
      finish();
    }

    if (sets.DISPATCH.has(cmd.method) && !inPass && !state.open) {
      const cbKey = cmd.secondary || objId;
      const ordinal = computeCounters.get(cbKey) ?? 0;
      computeCounters.set(cbKey, ordinal + 1);
      begin({
        kind: "compute", label: source.computePassLabel(ordinal), commandIndex: cmd.index,
        passKey: passKey(cmd.frame, cbKey, ordinal, true), frame: cmd.frame, draws: 0, accesses: [], unresolvedReads: 0,
      }, new AccessSet());
    }

    if (isAction(sets, cmd.method)) {
      const open = state.open;
      if (!open) continue;   // a draw outside any pass: nothing to attribute it to
      open.pass.draws++;
      const { accesses, unresolved } = source.actionAccesses(cmd, stream);
      for (const a of accesses) open.accesses.add(a);
      open.pass.unresolvedReads = (open.pass.unresolvedReads ?? 0) + unresolved;
      continue;
    }

    const transfer = source.transferAccesses(cmd);
    if (transfer && transfer.accesses.length) {
      // Transfers outside a pass are nodes of their own; inside a render pass (a clear of an
      // attachment) they belong to the pass that is open.
      if (state.open && inPass) {
        for (const a of transfer.accesses) state.open.accesses.add(a);
      } else {
        // A copy outside a render pass is a node of its own. It does not close an open run of
        // dispatches: the command tree does not either, and the pass keys have to agree.
        passes.push({
          kind: "transfer", label: transfer.label, commandIndex: cmd.index, passKey: null,
          frame: cmd.frame, draws: 0, accesses: transfer.accesses, unresolvedReads: 0,
        });
      }
    }
  }
  finish();
  // A pass that touched nothing the capture can name is noise in the graph, not information.
  return passes.filter((p) => p.accesses.length > 0 || p.draws > 0);
}

/** The render graph of a capture, with GPU pass durations attached when the frame was profiled. */
export function buildFrameGraph(data: CaptureData, sets: CommandSets, source: ResourceSource): RenderGraph {
  const passes = collectPasses(data, sets, source);
  return buildRenderGraph(passes, {
    durationOf: (key) => data.passTimings.get(key)?.durationMs ?? null,
  });
}

/** The render graph of a capture, using the resource source of the API the capture came from. */
export function frameRenderGraph(data: CaptureData, db: ObjectLookup): RenderGraph {
  const source = data.api === "metal" ? new MetalResourceSource(db) : new VulkanResourceSource(db);
  return buildFrameGraph(data, data.sets, source);
}
