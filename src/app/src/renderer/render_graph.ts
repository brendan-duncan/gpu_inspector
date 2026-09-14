// The render graph of a captured frame: the frame's passes as nodes and the resources they read
// and write as the edges between them.
//
// A capture is a flat list of commands, which says what the frame *did* but not what depends on
// what. The dependencies are there, spread across three places: a pass' attachments say what it
// renders to and whether it loads what was there before, the descriptor sets bound at each draw
// and dispatch say what it reads, and the transfer commands name a source and a destination. Roll
// those up per pass and the frame becomes a directed graph, which is how engines think about it
// and how it is easiest to see that a pass writes something nothing reads, that a chain of passes
// is serial when it did not have to be, or where the frame's real critical path runs.
//
// Two things make it a graph and not just "which passes touched image 12":
//
//   Subresources. Edges are keyed on the mip level and array layer a pass actually touched, not
//   on the image. A bloom chain writes mip N and reads mip N-1 of one image; keyed on the image
//   alone it would collapse into a single node with a self-loop.
//
//   Versions. A pass that loads an attachment and stores it again both reads and writes the same
//   resource, so a graph over resources has cycles. Each write instead starts a new *version* of
//   the resource, and edges run from the version's producer to its readers, which is the same
//   SSA-shaped model a render graph compiler uses. Version 0 is what the resource held on entry
//   to the capture: reads of it come from a previous frame, a host upload, or a pass outside the
//   captured range, and are reported as external inputs rather than as edges.
//
// Everything above is API-neutral: this module is fed RawPass/RawAccess by a per-API extractor
// (vulkan/frame_resources.ts, metal/frame_resources.ts), the same split as command_sets.ts.

import type { CaptureApi } from "../shared/protocol.js";

export type ResourceType = "image" | "buffer";
export type AccessMode = "read" | "write" | "readwrite";
export type NodeKind = "render" | "compute" | "transfer";

/** A resource subresource as the graph identifies it: one row of the chart, one lane of the DAG. */
export interface RawResource {
  /**
   * Identity of the subresource an access touches: the object id plus the mip and layer for an
   * image ("image:12:m0:l0"). Accesses with the same key are accesses to the same thing.
   */
  key: string;
  objectId: number;
  type: ResourceType;
  /** "Bloom mip 2", "VkImage 12"; the object's label when it has one. */
  label: string;
  /** "1920x1080 R8G8B8A8_UNORM", "4.0 MB": the second line of the row header. */
  detail: string;
  /** Size in bytes when known, for sorting the heaviest resources first. */
  bytes: number;
  /** A swapchain / drawable image: what the frame is for, so writes to it are never unread. */
  presented: boolean;
}

export interface RawAccess {
  resource: RawResource;
  mode: AccessMode;
  /** "color attachment", "sampled", "storage", "copy dst": what the pass did with it. */
  usage: string;
  /**
   * The write replaces the subresource entirely (a CLEAR or DONT_CARE load op, a full copy), so
   * it does not depend on what was there before and starts a chain rather than continuing one.
   */
  discards?: boolean;
  /** The API throws the result away (store op DONT_CARE): nothing can read this version. */
  dropped?: boolean;
  /**
   * A multisampled attachment written alongside its resolve target. Nothing reads such an
   * attachment, by design, so the rules leave it to the per-API msaa-store rule, which knows the
   * resolve-only store ops to suggest.
   */
  resolved?: boolean;
}

/** One pass (or standalone transfer command) of the frame, in execution order. */
export interface RawPass {
  kind: NodeKind;
  label: string;
  /** The command that begins the pass, for jumping to it in the command list. */
  commandIndex: number;
  /** passKey() of the pass for GPU timings; null for a transfer command, which has no timing. */
  passKey: string | null;
  frame: number;
  draws: number;
  accesses: RawAccess[];
  /**
   * Reads the capture could not resolve (descriptor buffers, bindless indexing): the graph is a
   * lower bound on the edges, and the view says so rather than implying the pass reads nothing.
   */
  unresolvedReads?: number;
}

/** One version of a resource: what a pass left in it, and which passes then read that. */
export interface GraphVersion {
  resource: GraphResource;
  /** 0 is the contents on entry to the capture; version N is the result of the Nth write. */
  index: number;
  /** The pass that wrote this version; null for version 0. */
  producer: GraphNode | null;
  readers: GraphNode[];
  /** The producer declared the contents dead (store op DONT_CARE), so nothing may read them. */
  dropped: boolean;
}

export interface GraphResource extends RawResource {
  versions: GraphVersion[];
  /** Ordinals of the first and last node that touched it: the span of its lifetime bar. */
  first: number;
  last: number;
  uses: GraphUse[];
  /** Version 0 was read: the contents come from before the capture. */
  externalInput: boolean;
}

/** One pass' access to one resource, from both ends: the chart's cells and the node's lists. */
export interface GraphUse {
  node: GraphNode;
  resource: GraphResource;
  mode: AccessMode;
  usage: string;
  /** The version read, for a read; the version produced, for a write. */
  version: GraphVersion;
  /** A write that replaced the subresource rather than adding to what was there (see RawAccess). */
  discards: boolean;
  /** A write the API throws away (store op DONT_CARE), so it never reaches memory. */
  dropped: boolean;
  /** A multisampled attachment written beside its resolve target (see RawAccess). */
  resolved: boolean;
}

export interface GraphNode {
  ordinal: number;
  kind: NodeKind;
  label: string;
  commandIndex: number;
  passKey: string | null;
  frame: number;
  draws: number;
  reads: GraphUse[];
  writes: GraphUse[];
  inputs: GraphEdge[];
  outputs: GraphEdge[];
  /** GPU duration from the capture's pass timings, when the frame was profiled. */
  durationMs: number | null;
  unresolvedReads: number;
  /**
   * Nothing later in the capture reads anything this pass wrote, and none of it is presented.
   * A weaker claim than "dead": the host may read the result back, or the next frame may.
   */
  unread: boolean;
  /** Longest-duration path from this node to the end of the frame, in ms (critical path). */
  pathMs: number;
}

export interface GraphEdge {
  from: GraphNode;
  to: GraphNode;
  version: GraphVersion;
  /** What the consumer did with it: "sampled", "color attachment (load)", ... */
  usage: string;
}

/**
 * A synchronization command between two passes (a Vulkan pipeline barrier or event wait), kept
 * beside the graph so the rules can compare what a frame declares it depends on with what it
 * actually does. Not a node: it produces and consumes nothing.
 */
export interface SyncPoint {
  commandIndex: number;
  method: string;
  /** Ordinal of the last pass that began before it, -1 when it precedes every pass. */
  after: number;
  /** The subresources the barrier names, by resource key. Empty for a global memory barrier. */
  resources: string[];
  /**
   * The barrier also changes an image layout or moves a resource between queue families. Those
   * are required whether or not any data depends on them, so such a barrier is never questioned.
   */
  structural: boolean;
}

export interface RenderGraph {
  /** The API the capture came from, so a rule's advice can name that API's own spelling of a fix. */
  api: CaptureApi;
  nodes: GraphNode[];
  resources: GraphResource[];
  edges: GraphEdge[];
  /** Barriers and event waits, in command order (see SyncPoint). */
  syncPoints: SyncPoint[];
  /** Resources whose first access reads contents from before the capture. */
  externalInputs: GraphResource[];
  /** Passes nothing downstream consumes; see GraphNode.unread for what that does and does not mean. */
  unreadNodes: GraphNode[];
  /** The longest-duration chain of dependent passes, when the capture has timings. */
  criticalPath: GraphNode[];
  criticalPathMs: number;
  /** Things the graph could not see, shown above the chart so it is not read as complete. */
  warnings: string[];
}

export interface BuildOptions {
  api?: CaptureApi;
  /** GPU duration of a pass, by its passKey; absent when the frame was not profiled. */
  durationOf?: (passKey: string) => number | null;
  /** The frame's barriers and event waits, in command order. */
  syncPoints?: SyncPoint[];
}

/** Builds the graph from a frame's passes in execution order. */
export function buildRenderGraph(passes: RawPass[], options: BuildOptions = {}): RenderGraph {
  const resources = new Map<string, GraphResource>();
  const nodes: GraphNode[] = [];
  const edges: GraphEdge[] = [];
  /** The version each resource currently holds, as the walk moves through the frame. */
  const current = new Map<string, GraphVersion>();

  const resourceOf = (raw: RawResource): GraphResource => {
    let r = resources.get(raw.key);
    if (!r) {
      r = { ...raw, versions: [], uses: [], first: Infinity, last: -1, externalInput: false };
      // Version 0: whatever the resource held when the capture started.
      r.versions.push({ resource: r, index: 0, producer: null, readers: [], dropped: false });
      resources.set(raw.key, r);
      current.set(raw.key, r.versions[0]);
    }
    return r;
  };

  for (const pass of passes) {
    const node: GraphNode = {
      ordinal: nodes.length, kind: pass.kind, label: pass.label, commandIndex: pass.commandIndex,
      passKey: pass.passKey, frame: pass.frame, draws: pass.draws, reads: [], writes: [], inputs: [], outputs: [],
      durationMs: pass.passKey && options.durationOf ? options.durationOf(pass.passKey) : null,
      unresolvedReads: pass.unresolvedReads ?? 0, unread: false, pathMs: 0,
    };
    nodes.push(node);

    // A pass' accesses are applied in two rounds: every read against the versions the pass began
    // with, then every write. Applied one at a time, a pass that reads image A and writes image A
    // through two different bindings would read its own output.
    // A write that keeps what was there (an attachment loaded, a copy into part of an image) is a
    // read of the previous version too: that is the edge an accumulating target hangs on.
    for (const access of pass.accesses) {
      if (access.mode === "write" && access.discards) continue;
      const resource = resourceOf(access.resource);
      const version = current.get(resource.key)!;
      // Such a write depends on the previous version but is not itself listed as a read: the pass'
      // access to the resource is the write, recorded in the round below.
      const isRead = access.mode !== "write";
      const use: GraphUse = { node, resource, mode: access.mode, usage: access.usage, version, discards: !!access.discards, dropped: !!access.dropped, resolved: !!access.resolved };
      if (isRead) {
        node.reads.push(use);
        resource.uses.push(use);
        touch(resource, node);
      }
      if (!version.readers.includes(node)) version.readers.push(node);
      if (version.producer && version.producer !== node) {
        const edge: GraphEdge = { from: version.producer, to: node, version, usage: access.usage };
        edges.push(edge);
        version.producer.outputs.push(edge);
        node.inputs.push(edge);
      } else if (!version.producer) {
        resource.externalInput = true;
      }
    }

    for (const access of pass.accesses) {
      if (access.mode === "read") continue;
      const resource = resourceOf(access.resource);
      const version: GraphVersion = {
        resource, index: resource.versions.length, producer: node, readers: [], dropped: !!access.dropped,
      };
      resource.versions.push(version);
      current.set(resource.key, version);
      const use: GraphUse = { node, resource, mode: access.mode, usage: access.usage, version, discards: !!access.discards, dropped: !!access.dropped, resolved: !!access.resolved };
      node.writes.push(use);
      resource.uses.push(use);
      touch(resource, node);
    }
  }

  for (const node of nodes) {
    node.unread = node.writes.length > 0 &&
      node.writes.every((w) => w.version.readers.length === 0 && !w.resource.presented);
  }

  const list = [...resources.values()];
  const graph: RenderGraph = {
    api: options.api ?? "vulkan",
    nodes, resources: list, edges, syncPoints: options.syncPoints ?? [],
    externalInputs: list.filter((r) => r.externalInput),
    unreadNodes: nodes.filter((n) => n.unread),
    criticalPath: [], criticalPathMs: 0, warnings: [],
  };
  computeCriticalPath(graph);

  if (nodes.some((n) => [...n.reads, ...n.writes].some((u) => u.usage.includes("storage")))) {
    graph.warnings.push("Storage buffers and storage images are counted as read-write: without shader reflection the capture cannot tell a binding the shader only reads from one it writes, so some write edges may be dependencies that are not really there.");
  }
  const unresolved = nodes.reduce((n, p) => n + p.unresolvedReads, 0);
  if (unresolved) {
    graph.warnings.push(`${unresolved} binding${unresolved === 1 ? "" : "s"} could not be resolved to a resource (bindless / descriptor buffers), so some read edges are missing: the graph is a lower bound on the frame's dependencies.`);
  }
  if (!nodes.some((n) => n.durationMs !== null)) {
    graph.warnings.push("The capture has no GPU pass timings, so there is no critical path. Capture with \"Profile passes\" to get one.");
  }
  return graph;
}

function touch(resource: GraphResource, node: GraphNode): void {
  resource.first = Math.min(resource.first, node.ordinal);
  resource.last = Math.max(resource.last, node.ordinal);
}

/**
 * The longest chain of dependent passes by GPU time. Nodes are already in execution order and
 * every edge runs forward, so one backwards sweep is enough: a node's path is its own duration
 * plus the longest path of anything that consumes it.
 */
function computeCriticalPath(graph: RenderGraph): void {
  const next = new Map<GraphNode, GraphNode | null>();
  let head: GraphNode | null = null;
  let best = 0;
  for (let i = graph.nodes.length - 1; i >= 0; i--) {
    const node = graph.nodes[i];
    let bestChild: GraphNode | null = null;
    let bestChildMs = 0;
    for (const edge of node.outputs) {
      // Nodes are in execution order and all but a handful of edges run forward with them; the
      // exceptions (a copy recorded between two dispatches of one compute pass, which the command
      // tree keeps in that pass) are left out of the path rather than allowed to make it circular.
      if (edge.to.ordinal <= node.ordinal) continue;
      if (edge.to.pathMs > bestChildMs) {
        bestChildMs = edge.to.pathMs;
        bestChild = edge.to;
      }
    }
    node.pathMs = (node.durationMs ?? 0) + bestChildMs;
    next.set(node, bestChild);
    if (node.pathMs > best) {
      best = node.pathMs;
      head = node;
    }
  }
  if (!head || best <= 0) return;
  const path: GraphNode[] = [];
  for (let n: GraphNode | null = head; n; n = next.get(n) ?? null) path.push(n);
  graph.criticalPath = path;
  graph.criticalPathMs = best;
}

/**
 * Row order for the lifetime chart: by the node that first touched the resource, then by the one
 * that last did, longest-lived first. That puts a resource's row next to the pass that created it
 * and draws the frame as a staircase, the way a memory-lifetime chart reads.
 */
export function orderResources(resources: GraphResource[]): GraphResource[] {
  return [...resources].sort((a, b) => a.first - b.first || b.last - a.last || b.bytes - a.bytes || a.label.localeCompare(b.label));
}

/** Classes of usage, for coloring an access in the chart and keying the legend. */
export function usageClass(usage: string): string {
  // Every transfer names its ends "<verb> src" / "<verb> dst", which catches the verbs (update,
  // fill, resolve copy, generate mipmaps) that a prefix test would not.
  if (usage.endsWith(" src") || usage.endsWith(" dst")) return "transfer";
  if (usage.startsWith("color") || usage.startsWith("depth") || usage.startsWith("stencil") || usage.startsWith("resolve")) return "attachment";
  if (usage.startsWith("copy") || usage.startsWith("clear") || usage.startsWith("blit")) return "transfer";
  if (usage.startsWith("storage")) return "storage";
  if (usage.startsWith("sampled")) return "sampled";
  if (usage.startsWith("vertex") || usage.startsWith("index") || usage.startsWith("indirect") || usage.startsWith("uniform")) return "input";
  return "other";
}
