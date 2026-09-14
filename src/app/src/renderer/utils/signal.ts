// Ported from WebGPU Inspector (MIT): src/utils/signal.js

/** Constraint for the callback shape a Signal carries. */
export type SignalCallback = (...args: never[]) => void;

/** Anything that can be connected to a Signal: a callback, or another Signal with the same shape. */
export type SignalListener<F extends SignalCallback> = F | Signal<F>;

/** A slot is the connected listener plus the optional `this` object it should be invoked on. */
export type SignalSlot<F extends SignalCallback> = [SignalListener<F>, unknown];

/**
 * A Signal is like a proxy function that can have multiple "listeners" assigned to it, such that
 * when the Signal is executed (or "emitted"), it executes each of its associated listeners.
 * A listener is a callback function, object method, or another Signal.
 */
export class Signal<F extends SignalCallback = (...args: unknown[]) => void> {
  static _disableSignals = 0;

  _lastSlotId: number;
  slots: Map<number, SignalSlot<F>>;
  name?: string;

  /**
   * @param name Optional name for the signal, usually used for debugging purposes.
   */
  constructor(name?: string) {
    this._lastSlotId = 0;
    this.slots = new Map();
    if (name) {
      this.name = name;
    }
  }

  /**
   * Returns true if signals are allowed to be emitted. If false,
   * calling the Signal's emit method will do nothing.
   */
  static get enabled(): boolean {
    return Signal._disableSignals == 0;
  }

  /**
   * Returns true if signals are disabled from being emitted. If true,
   * calling the Signal's emit method will do nothing.
   */
  static get disabled(): boolean {
    return Signal._disableSignals > 0;
  }

  /**
   * Disables all signals from being emitted. This can be called multiple times, but an equal
   * number of calls to enable should be used to re-enable signals. This is often used to disable
   * any callbacks while doing heavy operations, like file loading, so a single signal will be
   * emitted at the end.
   */
  static disable(): number {
    return Signal._disableSignals++;
  }

  /**
   * Enable signals to be emitted, having been previously disabled.
   * @param force If true, signals will be forced to the enabled state,
   * even if there were an unbalanced number of calls to disable..
   */
  static enable(force?: boolean): number {
    if (force) {
      Signal._disableSignals = 0;
      return 0;
    }
    return Signal._disableSignals > 0 ? Signal._disableSignals-- : 0;
  }

  /**
   * Disconnect the listener from all signals of the given object.
   * @param object The object to disconnect from.
   * @param callback The listener to disconnect
   * @param instance The optional listener instance that owns callback.
   */
  static disconnect(object: object, callback?: unknown, instance?: unknown): void {
    const record = object as Record<string, unknown>;
    for (const i in record) {
      const p = record[i] as { constructor: unknown };
      if (p.constructor === Signal) {
        (p as Signal).disconnect(callback, instance);
      }
    }
  }

  /**
   * Return all signals that belong to the object.
   * @param object The object to get the signals from.
   * @param out Optional storage for the results. A new array will be created if null.
   * @return The list of signals that belong to the object.
   */
  static getSignals(object: object, out?: Signal[] | null): Signal[] {
    out = out || [];
    const record = object as Record<string, unknown>;
    for (const i in record) {
      const p = record[i] as { constructor: unknown };
      if (p.constructor === Signal) {
        out.push(p as Signal);
      }
    }
    return out;
  }

  /**
   * True if this signal has at least one listener.
   */
  get hasListeners(): boolean {
    return this.slots.size > 0;
  }

  /**
   * Emit a signal, calling all listeners.
   * @param args Optional arguments to call the listeners with.
   * @returns The first truthy value returned by a listener (which stops emission), else null.
   */
  emit(...args: Parameters<F>): unknown {
    if (Signal.disabled) {
      return null;
    }

    for (const k of this.slots) {
      const s = k[1][0];
      const o = k[1][1] || s;
      if (!s) {
        continue;
      }

      if (s.constructor === Signal) {
        (s as Signal<F>).emit.apply(o, args);
      } else {
        const res: unknown = (s as (...a: Parameters<F>) => unknown).apply(o, args);
        if (res) {
          return res;
        }
      }
    }
    return null;
  }

  /**
   * Connect a listener to the signal. This can be a function, object method,
   * class static method, or another signal. There is no type-checking to
   * ensure the listener function can successfully receive the arguments that
   * will be emitted by the signal, which will result in an exception if you
   * connect an incompatible listener and emit the signal.
   * To have an object method listen to a signal, pass in the object, too.
   * @returns A handle that can be used to disconnect the listener. Returns -1 if the listener was already connected.
   * @example
   * listen(Function)
   * listen(Signal)
   * listen(method, object)
   */
  addListener(callback: SignalListener<F>, object?: unknown): number {
    // Don't add the same listener multiple times.
    if (this.isListening(callback, object)) {
      return -1;
    }

    this.slots.set(this._lastSlotId++, [callback, object]);
    return this._lastSlotId - 1;
  }

  /**
   * Checks if there is a binded listener that matches the criteria.
   * @example
   * isListening(Signal)
   * isListening(callback)
   * isListening(object)
   * isListening(method, object)
   */
  isListening(callback?: unknown, object?: unknown): boolean {
    for (const slot of this.slots) {
      const slotInfo = slot[1];

      if (callback && !object) {
        if (slotInfo[0] === callback || slotInfo[1] === callback) {
          return true;
        }
      } else if (!callback && object) {
        if (slotInfo[1] === object) {
          return true;
        }
      } else {
        if (slotInfo[0] === callback && slotInfo[1] === object) {
          return true;
        }
      }
    }
    return false;
  }

  /**
   * Disconnect a listener from the signal.
   * @example
   * disconnect(Object) -- Disconnect all method listeners of the given object.
   * disconnect(Function) -- Disconnect the function listener.
   * disconnect(Signal) -- Disconnect the signal listener.
   * disconnect(method, object) -- Disconnect the method listener.
   * disconnect(handle) -- Disconnect the listener registered under the numeric handle.
   * disconnect() -- Disconnect all listeners from the signal.
   */
  disconnect(callback?: unknown, object?: unknown): boolean {
    if ((callback === null || callback === undefined) &&
      (object === null || object === undefined)) {
      this.slots.clear();
      return true;
    }

    if (typeof callback === 'number') {
      const handle = callback;
      if (!this.slots.has(handle)) {
        return false;
      }
      this.slots.delete(handle);
      return true;
    }

    let found = false;
    for (const slot of this.slots) {
      const slotHandle = slot[0];
      const slotInfo = slot[1];

      if (callback && !object) {
        if (slotInfo[0] === callback || slotInfo[1] === callback) {
          this.slots.delete(slotHandle);
          found = true;
        }
      } else if (!callback && object) {
        if (slotInfo[1] === object) {
          this.slots.delete(slotHandle);
          found = true;
        }
      } else {
        if (slotInfo[0] === callback && slotInfo[1] === object) {
          this.slots.delete(slotHandle);
          found = true;
        }
      }
    }

    return found;
  }

  /**
   * Alias of disconnect(callback, object); the name the TypeScript Signal contract uses.
   */
  removeListener(callback: SignalListener<F>, object?: unknown): void {
    this.disconnect(callback, object);
  }
}
