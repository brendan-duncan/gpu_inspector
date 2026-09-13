// Derivatives for the shader debugger's fragment invocations: a GPU runs a fragment shader on 2x2
// pixel quads and takes dFdx / dFdy (and the level of detail of implicit sampling) as differences
// across the quad. PixelQuad runs the four invocations in lockstep: when one reaches a derivative
// point it waits, the others are run to the same point, and each gets the differences of its row
// and column. An invocation that finishes before reaching the point (it branched differently)
// lends it the debugged pixel's value, as a GPU's result there is undefined anyway.
//
// Nothing here knows which language the invocations are of: SPIR-V's OpDPdx and MSL's dfdx block
// the same way, so a Vulkan and a Metal fragment share this.
import { zipScalars, type Value } from "./values.js";
import type { DebugInvocation, DebugStep, InvocationStatus, Stepper } from "./program.js";

/** An invocation that can be blocked at a derivative point: what PixelQuad drives. */
export interface DerivativeInvocation extends DebugInvocation {
  step(): InvocationStatus;
}

export interface DerivativeSource {
  /** The screen-space derivatives of a value at a derivative point; "blocked" while the other invocations catch up. */
  derivative(invocation: DerivativeInvocation, inst: DebugStep, operand: Value): { dx: Value; dy: Value } | "blocked";
}

interface LaneState {
  /** The value at the derivative point it waits at, until the results are in. */
  operand: Value | undefined;
  result: { dx: Value; dy: Value } | undefined;
  /** Derivative points it has passed. */
  points: number;
}

const MAX_CATCH_UP = 10_000_000;

export class PixelQuad implements DerivativeSource, Stepper {
  /** Top-left, top-right, bottom-left, bottom-right. */
  readonly lanes: DerivativeInvocation[];
  /** The lane of the pixel being debugged. */
  readonly target: number;
  private readonly _state = new Map<DerivativeInvocation, LaneState>();

  /**
   * `create(dx, dy)` makes the invocation of the pixel at that offset from the quad's top-left
   * corner; `target` is the debugged pixel's position in the quad (0 to 3).
   */
  constructor(create: (dx: number, dy: number, source: DerivativeSource) => DerivativeInvocation, target: number) {
    this.lanes = [create(0, 0, this), create(1, 0, this), create(0, 1, this), create(1, 1, this)];
    this.target = target;
    for (const lane of this.lanes) this._state.set(lane, { operand: undefined, result: undefined, points: 0 });
  }

  /** The quad's corner and the debugged pixel's lane for a pixel. */
  static place(x: number, y: number): { x0: number; y0: number; target: number } {
    const x0 = x - (x & 1);
    const y0 = y - (y & 1);
    return { x0, y0, target: (y & 1) * 2 + (x & 1) };
  }

  get invocation(): DerivativeInvocation {
    return this.lanes[this.target];
  }

  derivative(invocation: DerivativeInvocation, _inst: DebugStep, operand: Value): { dx: Value; dy: Value } | "blocked" {
    const state = this._state.get(invocation);
    if (!state) return { dx: zipScalars(operand, operand, () => 0), dy: zipScalars(operand, operand, () => 0) };
    if (state.result) {
      const result = state.result;
      state.result = undefined;
      state.operand = undefined;
      state.points++;
      return result;
    }
    state.operand = operand;
    return "blocked";
  }

  /** Steps the debugged invocation; a derivative point brings the other three up to it first. */
  step(): InvocationStatus {
    const target = this.invocation;
    let status = target.step();
    if (status === "blocked") {
      this._resolve();
      status = target.step();
    }
    return status;
  }

  /** Runs every lane to the end. */
  run(): InvocationStatus {
    while (!this.invocation.finished) this.step();
    return this.invocation.status;
  }

  private _resolve(): void {
    // Everyone else to the same derivative point, or to their end.
    for (const lane of this.lanes) {
      if (lane === this.invocation) continue;
      let guard = 0;
      while (!lane.finished && this._state.get(lane)!.operand === undefined && guard++ < MAX_CATCH_UP) {
        if (lane.step() === "blocked") break;
      }
    }
    const fallback = this._state.get(this.invocation)!.operand as Value;
    const at = (i: number): Value => this._state.get(this.lanes[i])!.operand ?? fallback;
    const diff = (a: number, b: number): Value => zipScalars(at(b), at(a), (x, y) => Number(x) - Number(y));
    const rowDx = [diff(0, 1), diff(0, 1), diff(2, 3), diff(2, 3)];
    const colDy = [diff(0, 2), diff(1, 3), diff(0, 2), diff(1, 3)];
    this.lanes.forEach((lane, i) => {
      const state = this._state.get(lane)!;
      if (state.operand !== undefined) state.result = { dx: rowDx[i], dy: colDy[i] };
    });
    // Lanes that were waiting take their result now, so they are ready for the next point.
    for (const lane of this.lanes) {
      if (lane === this.invocation) continue;
      const state = this._state.get(lane)!;
      if (state.result) lane.step();
    }
  }
}
