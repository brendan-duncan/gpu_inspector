// What an acceleration structure is made of, as a tree with its costs rolled up, and where its
// instances overlap: the two questions a scene's traversal cost comes down to.
//
// The tree is top level → instances → bottom level → geometries, each row with its primitives,
// surface area and memory, summed upward, so a heavy instance or a bottom level placed a thousand
// times stands out. Everything is measured from what is drawn (acceleration_scene.ts,
// structureDrawing): the areas are in world space, after the instance's transform.
//
// Overlap is between the instances' world-space bounding boxes. A ray through a region where several
// instances' boxes overlap has to descend into every one of them, so the more instances a box shares
// its space with, the more a ray there costs — what Nsight Graphics calls the instance overlap
// heatmap. A driver's own boxes are tighter than these, but not by the amount that matters: two
// instances whose geometry boxes overlap overlap in the driver's structure too.
import { unresolvedReference, type SceneGroup } from "./acceleration_structure.js";
import type { StructureDrawing } from "./acceleration_scene.js";

export interface Bounds {
  min: [number, number, number];
  max: [number, number, number];
}

/** What one run of a drawing's vertices amounts to. */
export interface GroupStats {
  primitives: number;
  /** World-space surface area: of the triangles, or of a procedural geometry's boxes. Null for a stand-in. */
  area: number | null;
  bounds: Bounds | null;
}

export type TreeKind = "tlas" | "instance" | "blas" | "geometry";

export interface TreeNode {
  /** Unique within the tree, stable across rebuilds of the same drawing: what visibility is kept by. */
  key: string;
  kind: TreeKind;
  label: string;
  /** The structure object a row stands for (a top or bottom level), for opening it. */
  objectId?: number;
  /** The instance's position in the drawing's instance list. */
  instance?: number;
  /** The drawing's groups this row covers: what hiding it hides and what selecting it frames. */
  groups: number[];
  primitives: number;
  area: number | null;
  /** Bytes, when the capture says: a top level's own plus every distinct bottom level it places. */
  memory: number | null;
  bounds: Bounds | null;
  children: TreeNode[];
  /** Why a row has no geometry of its own. */
  note?: string;
}

/** What the tree needs to know of a structure object that the drawing does not say. */
export interface StructureFacts {
  name: string;
  /** Bytes the structure takes, or null when the capture does not say. */
  memory: number | null;
  /** Primitives its build holds, for a bottom level whose geometry was not read back. */
  primitives: number | null;
}

// ---------------------------------------------------------------------------------------------
// Measuring

function extend(b: Bounds | null, x: number, y: number, z: number): Bounds {
  if (!b) return { min: [x, y, z], max: [x, y, z] };
  if (x < b.min[0]) b.min[0] = x; if (x > b.max[0]) b.max[0] = x;
  if (y < b.min[1]) b.min[1] = y; if (y > b.max[1]) b.max[1] = y;
  if (z < b.min[2]) b.min[2] = z; if (z > b.max[2]) b.max[2] = z;
  return b;
}

export function union(a: Bounds | null, b: Bounds | null): Bounds | null {
  if (!a) return b ? { min: [...b.min], max: [...b.max] } : null;
  if (!b) return a;
  return extend(extend({ min: [...a.min], max: [...a.max] }, ...b.min), ...b.max);
}

function boxArea(b: Bounds): number {
  const dx = b.max[0] - b.min[0], dy = b.max[1] - b.min[1], dz = b.max[2] - b.min[2];
  return 2 * (dx * dy + dy * dz + dz * dx);
}

/** A procedural box is drawn as its twelve edges, two endpoints each. */
const BOX_VERTICES = 24;

/** Primitives, area and bounds of one group of a drawing. */
export function groupStats(drawing: StructureDrawing, group: SceneGroup): GroupStats {
  const p = group.lines ? drawing.lines : drawing.triangles;
  let bounds: Bounds | null = null;
  const end = Math.min(group.first + group.count, p.length / 3);
  for (let v = group.first; v < end; v++) bounds = extend(bounds, p[v * 3], p[v * 3 + 1], p[v * 3 + 2]);
  if (group.geometry < 0) return { primitives: 0, area: null, bounds };
  if (group.lines) {
    let area = 0;
    let boxes = 0;
    for (let v = group.first; v + BOX_VERTICES <= end; v += BOX_VERTICES) {
      let box: Bounds | null = null;
      for (let k = v; k < v + BOX_VERTICES; k++) box = extend(box, p[k * 3], p[k * 3 + 1], p[k * 3 + 2]);
      if (box) area += boxArea(box);
      boxes++;
    }
    return { primitives: boxes, area, bounds };
  }
  let area = 0;
  let triangles = 0;
  for (let v = group.first; v + 3 <= end; v += 3) {
    const ax = p[v * 3], ay = p[v * 3 + 1], az = p[v * 3 + 2];
    const ux = p[v * 3 + 3] - ax, uy = p[v * 3 + 4] - ay, uz = p[v * 3 + 5] - az;
    const wx = p[v * 3 + 6] - ax, wy = p[v * 3 + 7] - ay, wz = p[v * 3 + 8] - az;
    const cx = uy * wz - uz * wy, cy = uz * wx - ux * wz, cz = ux * wy - uy * wx;
    const a = Math.hypot(cx, cy, cz) / 2;
    if (Number.isFinite(a)) area += a;
    triangles++;
  }
  return { primitives: triangles, area, bounds };
}

// ---------------------------------------------------------------------------------------------
// The tree

const sumArea = (nodes: TreeNode[]): number | null =>
  nodes.some((n) => n.area !== null) ? nodes.reduce((s, n) => s + (n.area ?? 0), 0) : null;

function geometryNode(key: string, drawing: StructureDrawing, index: number, stats: GroupStats): TreeNode {
  const g = drawing.groups[index];
  return {
    key, kind: "geometry", label: `Geometry ${g.geometry} · ${g.lines ? "procedural boxes" : "triangles"}`,
    groups: [index], primitives: stats.primitives, area: stats.area, memory: null, bounds: stats.bounds, children: [],
  };
}

/**
 * The tree of `structure` as `drawing` shows it. `facts` answers for any structure object id, the
 * one drawn and every bottom level its instances place.
 */
export function structureTree(drawing: StructureDrawing, structure: number,
                              facts: (id: number) => StructureFacts | null): TreeNode {
  const own = facts(structure);
  const stats = drawing.groups.map((g) => groupStats(drawing, g));
  if (!drawing.instances.length) {
    // A bottom level: its geometries.
    const children = drawing.groups.map((_g, i) => geometryNode(`g${i}`, drawing, i, stats[i]));
    return {
      key: "root", kind: "blas", label: own?.name ?? `Structure ${structure}`, objectId: structure,
      groups: children.flatMap((c) => c.groups),
      primitives: children.length ? children.reduce((s, c) => s + c.primitives, 0) : own?.primitives ?? 0,
      area: sumArea(children), memory: own?.memory ?? null,
      bounds: children.reduce<Bounds | null>((b, c) => union(b, c.bounds), null), children,
    };
  }

  const byInstance = new Map<number, number[]>();
  drawing.groups.forEach((g, i) => {
    const list = byInstance.get(g.instance) ?? [];
    list.push(i);
    byInstance.set(g.instance, list);
  });
  const instances: TreeNode[] = drawing.instances.map((instance, at) => {
    const groups = byInstance.get(at) ?? [];
    const blasFacts = instance.blas !== undefined ? facts(instance.blas) : null;
    const standIn = groups.every((i) => drawing.groups[i].geometry < 0);
    const geometries = standIn ? [] : groups.map((i) => geometryNode(`i${at}g${i}`, drawing, i, stats[i]));
    const bounds = groups.reduce<Bounds | null>((b, i) => union(b, stats[i].bounds), null);
    const primitives = standIn ? blasFacts?.primitives ?? 0 : geometries.reduce((s, c) => s + c.primitives, 0);
    const note = instance.blas === undefined
      ? "Names no bottom level this capture knows"
      : standIn ? "Its bottom level's geometry is not in this capture: drawn as a box where the transform puts it" : undefined;
    const blas: TreeNode = {
      key: `i${at}b`, kind: "blas", label: blasFacts?.name ?? (instance.blas !== undefined ? `Structure ${instance.blas}` : unresolvedReference(instance)),
      objectId: instance.blas, groups, primitives, area: standIn ? null : sumArea(geometries),
      memory: blasFacts?.memory ?? null, bounds, children: geometries, note,
    };
    return {
      key: `i${at}`, kind: "instance", label: `Instance ${instance.index}`, instance: at, groups,
      primitives, area: blas.area, memory: null, bounds, children: [blas], note,
    };
  });
  // A bottom level placed many times costs its memory once.
  const distinct = new Map<number, number | null>();
  for (const i of drawing.instances) if (i.blas !== undefined && !distinct.has(i.blas)) distinct.set(i.blas, facts(i.blas)?.memory ?? null);
  const known = [own?.memory ?? null, ...distinct.values()].filter((m): m is number => m !== null);
  return {
    key: "root", kind: "tlas", label: own?.name ?? `Structure ${structure}`, objectId: structure,
    groups: drawing.groups.map((_g, i) => i),
    primitives: instances.reduce((s, n) => s + n.primitives, 0),
    area: sumArea(instances),
    memory: known.length ? known.reduce((s, m) => s + m, 0) : null,
    bounds: instances.reduce<Bounds | null>((b, n) => union(b, n.bounds), null),
    children: instances,
  };
}

/** Every node of the tree, depth first. */
export function walkTree(node: TreeNode, visit: (n: TreeNode, depth: number) => void, depth = 0): void {
  visit(node, depth);
  for (const c of node.children) walkTree(c, visit, depth + 1);
}

/**
 * The rows a search keeps: those whose label matches, with every ancestor so they stay reachable,
 * and every descendant of a match so a matched instance still shows what it holds. All when the
 * search is empty.
 */
export function matchingKeys(root: TreeNode, search: string): Set<string> {
  const keep = new Set<string>();
  const needle = search.trim().toLowerCase();
  const all = (n: TreeNode): void => { keep.add(n.key); n.children.forEach(all); };
  if (!needle) {
    all(root);
    return keep;
  }
  const visit = (n: TreeNode, path: TreeNode[]): void => {
    if (n.label.toLowerCase().includes(needle)) {
      for (const p of path) keep.add(p.key);
      all(n);
      return;
    }
    for (const c of n.children) visit(c, [...path, n]);
  };
  visit(root, []);
  return keep;
}

// ---------------------------------------------------------------------------------------------
// Instance overlap

export interface InstanceOverlap {
  /** Positions in the drawing's instance list, a < b. */
  a: number;
  b: number;
  /** Volume of the two boxes' intersection. */
  volume: number;
  /** That volume over the smaller box's: 1 when one box lies wholly inside the other. */
  fraction: number;
}

export interface OverlapReport {
  /** The overlapping pairs, the most overlapped first, at most `limit` of them. */
  pairs: InstanceOverlap[];
  /** Every overlapping pair, however many were kept. */
  total: number;
  /** For each instance, how many others its box overlaps: the heat. */
  counts: number[];
  /** Instances left out because nothing of their bottom level is known, so their box is not either. */
  unknown: number;
}

/** The volume of a box, with its flat sides given a sliver of thickness so a plane still has one. */
function volumeOf(min: number[], max: number[], sliver: number): number {
  return Math.max(max[0] - min[0], sliver) * Math.max(max[1] - min[1], sliver) * Math.max(max[2] - min[2], sliver);
}

/**
 * Which instances' world-space boxes overlap, by sweep and prune along x: the boxes sorted by their
 * lower x, each tested only against those still open. `bounds` is one entry per instance, null for
 * an instance whose box is not known.
 */
export function instanceOverlaps(bounds: (Bounds | null)[], limit = 1000): OverlapReport {
  const counts = new Array<number>(bounds.length).fill(0);
  const known = bounds.map((b, i) => ({ b, i })).filter((e): e is { b: Bounds; i: number } => !!e.b);
  let scene: Bounds | null = null;
  for (const e of known) scene = union(scene, e.b);
  const sliver = scene ? Math.max(1e-9, Math.hypot(scene.max[0] - scene.min[0], scene.max[1] - scene.min[1], scene.max[2] - scene.min[2]) * 1e-5) : 1e-9;
  known.sort((x, y) => x.b.min[0] - y.b.min[0]);
  const pairs: InstanceOverlap[] = [];
  let total = 0;
  const open: { b: Bounds; i: number }[] = [];
  for (const e of known) {
    // Boxes that end before this one begins can meet nothing further along.
    for (let k = open.length - 1; k >= 0; k--) if (open[k].b.max[0] < e.b.min[0]) open.splice(k, 1);
    for (const o of open) {
      const lo = [Math.max(o.b.min[0], e.b.min[0]), Math.max(o.b.min[1], e.b.min[1]), Math.max(o.b.min[2], e.b.min[2])];
      const hi = [Math.min(o.b.max[0], e.b.max[0]), Math.min(o.b.max[1], e.b.max[1]), Math.min(o.b.max[2], e.b.max[2])];
      if (hi[0] < lo[0] || hi[1] < lo[1] || hi[2] < lo[2]) continue;
      // Boxes that only touch share a face and nothing a ray has to test twice.
      const volume = volumeOf(lo, hi, 0);
      const touching = [0, 1, 2].filter((k) => hi[k] - lo[k] <= 0).length;
      const flatBoxes = [o.b, e.b].some((b) => [0, 1, 2].some((k) => b.max[k] - b.min[k] <= sliver));
      if (touching && !flatBoxes) continue;
      total++;
      counts[o.i]++;
      counts[e.i]++;
      const smaller = Math.min(volumeOf(o.b.min, o.b.max, sliver), volumeOf(e.b.min, e.b.max, sliver));
      const fraction = Math.min(1, volumeOf(lo, hi, sliver) / smaller);
      pairs.push({ a: Math.min(o.i, e.i), b: Math.max(o.i, e.i), volume, fraction });
    }
    open.push(e);
  }
  pairs.sort((x, y) => y.fraction - x.fraction || y.volume - x.volume || x.a - y.a || x.b - y.b);
  return { pairs: pairs.slice(0, limit), total, counts, unknown: bounds.length - known.length };
}

/** The world boxes of a drawing's instances, from what is drawn of them; null where only a stand-in is. */
export function instanceBounds(drawing: StructureDrawing): (Bounds | null)[] {
  const out: (Bounds | null)[] = drawing.instances.map(() => null);
  drawing.groups.forEach((g) => {
    if (g.instance < 0 || g.geometry < 0) return;
    out[g.instance] = union(out[g.instance], groupStats(drawing, g).bounds);
  });
  return out;
}

/** A heat from 0 to 1 as a colour, from a cold blue through green and yellow to red. */
export function heatColor(t: number): [number, number, number] {
  const x = Math.max(0, Math.min(1, t));
  const stops: [number, number, number][] = [[0.15, 0.3, 0.75], [0.2, 0.75, 0.45], [0.95, 0.85, 0.2], [0.9, 0.2, 0.15]];
  const f = x * (stops.length - 1);
  const i = Math.min(stops.length - 2, Math.floor(f));
  const u = f - i;
  return [0, 1, 2].map((k) => stops[i][k] + (stops[i + 1][k] - stops[i][k]) * u) as [number, number, number];
}

/** One colour per vertex of the drawing's triangles and of its lines, by the heat of the instance each came from. */
export function heatColors(drawing: StructureDrawing, counts: number[]): { triangles: Float32Array; lines: Float32Array } {
  const most = Math.max(1, ...counts);
  const triangles = new Float32Array(drawing.triangles.length);
  const lines = new Float32Array(drawing.lines.length);
  for (const g of drawing.groups) {
    const out = g.lines ? lines : triangles;
    const heat = g.instance >= 0 ? (counts[g.instance] ?? 0) / most : 0;
    const [r, gr, b] = heatColor(heat);
    for (let v = g.first; v < g.first + g.count; v++) {
      out[v * 3] = r;
      out[v * 3 + 1] = gr;
      out[v * 3 + 2] = b;
    }
  }
  return { triangles, lines };
}

/** The bounding boxes of the given instances as line pairs, twelve edges each: the instance box overlay. */
export function boundsLines(bounds: (Bounds | null)[]): { positions: Float32Array; owners: number[] } {
  const out: number[] = [];
  const owners: number[] = [];
  bounds.forEach((b, i) => {
    if (!b) return;
    const corner = (c: number): number[] => [(c & 1) ? b.max[0] : b.min[0], (c & 2) ? b.max[1] : b.min[1], (c & 4) ? b.max[2] : b.min[2]];
    for (let a = 0; a < 8; a++) {
      for (const bit of [1, 2, 4]) {
        const c = a ^ bit;
        if (c <= a) continue;
        out.push(...corner(a), ...corner(c));
      }
    }
    owners.push(i);
  });
  return { positions: new Float32Array(out), owners };
}
