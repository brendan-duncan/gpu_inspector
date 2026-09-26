// The invocations of one tessellation control patch, for the shader debugger. A GPU runs a patch's
// control shader invocations together: they write one set of outputs (each its own gl_out[] element,
// the patch's outputs and tessellation levels shared), and a barrier() makes each wait until every
// other has reached it, so what one wrote before the barrier is there for the others to read after.
//
// InvocationGroup steps the debugged invocation; when it reaches a barrier, the others are run up to
// that barrier (or to their end) first, and then all go on, the way PixelQuad (debug/quad.ts) brings
// a pixel quad to a derivative point.
import { Invocation, type BarrierSource } from "./interpreter.js";
import type { InvocationStatus, Stepper } from "../debug/program.js";

const MAX_CATCH_UP = 10_000_000;

export class InvocationGroup implements Stepper, BarrierSource {
  readonly lanes: Invocation[] = [];
  /** The lane being debugged. */
  readonly target: number;
  /** Lanes that reached the barrier and wait there. */
  private readonly _waiting = new Set<Invocation>();
  /** Lanes let through the barrier they wait at, until they take the release. */
  private readonly _released = new Set<Invocation>();

  /**
   * `create(index, group, shared)` makes lane `index`, with the group as its barrier and, for every lane
   * but the first, the first lane's output cells to write into.
   */
  constructor(count: number, target: number, create: (index: number, group: InvocationGroup, shared: Invocation | null) => Invocation) {
    for (let i = 0; i < count; i++) this.lanes.push(create(i, this, this.lanes[0] ?? null));
    this.target = Math.min(Math.max(0, target), count - 1);
  }

  get invocation(): Invocation {
    return this.lanes[this.target];
  }

  arrive(invocation: Invocation): boolean {
    if (this._released.delete(invocation)) return true;
    this._waiting.add(invocation);
    return false;
  }

  /**
   * Steps the debugged lane; a barrier brings the others up to it first. When it finishes, the others
   * run to their end too, so the outputs it shows are the whole patch's.
   */
  step(): InvocationStatus {
    const target = this.invocation;
    let status = target.step();
    if (status === "blocked") {
      this._catchUp(target);
      status = target.step();
    }
    if (target.finished) this.runAll();
    return status;
  }

  /** Runs the debugged lane to its end. */
  run(): InvocationStatus {
    while (!this.invocation.finished) this.step();
    return this.invocation.status;
  }

  /** Runs every lane to its end: what a tessellation evaluation shader's inputs are taken from. */
  runAll(): void {
    for (let rounds = 0; rounds < 1000 && this.lanes.some((l) => !l.finished); rounds++) {
      const lead = this.lanes.find((l) => !l.finished)!;
      let guard = 0;
      while (!lead.finished && guard++ < MAX_CATCH_UP) {
        if (lead.step() === "blocked") {
          this._catchUp(lead);
          break;
        }
      }
    }
  }

  /** Every lane but `waiting` to the barrier or its end, then all of them through it. */
  private _catchUp(waiting: Invocation): void {
    for (const lane of this.lanes) {
      if (lane === waiting) continue;
      let guard = 0;
      while (!lane.finished && !this._waiting.has(lane) && guard++ < MAX_CATCH_UP) {
        if (lane.step() === "blocked" && !this._waiting.has(lane)) break;
      }
    }
    for (const lane of this._waiting) this._released.add(lane);
    this._waiting.clear();
  }
}
