// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Span } from './span.js';
import { TextInput } from './text_input.js';
import type { Widget, WidgetOptions } from './widget.js';

export interface NumberInputOptions extends WidgetOptions {
  /** The initial value. A non-numeric string is treated as an expression and shown as-is. */
  value?: number | string | null;
  /** Number of decimal places shown (default 3). */
  precision?: number;
  /** Units suffix appended to the displayed value. */
  units?: string;
  disabled?: boolean;
  /** Drag horizontally (instead of vertically) to change the value. */
  horizontal?: boolean;
  /** Apply drag deltas linearly instead of with an acceleration curve. */
  linear?: boolean;
  step?: number;
  min?: number | null;
  max?: number | null;
  /** Add the "full" class to the input container. */
  full?: boolean;
  /** Extra class for the text input (default "full"). */
  inputClass?: string;
  onChange?: (value: string) => void;
  tabIndex?: number;
}

/** The modifier keys innerInc() consults; satisfied by mouse, wheel and keyboard events. */
type ModifierEvent = Pick<MouseEvent, 'shiftKey' | 'ctrlKey'>;

/**
 * A text input for numbers that can also be adjusted by dragging, the mouse wheel, or the
 * up/down arrow keys.
 */
export class NumberInput extends Span {
  value: number | string;
  precision: number;
  units: string;
  horizontal: boolean;
  linear: boolean;
  step: number;
  min: number | null;
  max: number | null;
  input: TextInput;
  dragger: Span;

  constructor(parent?: Widget | HTMLElement | null, options?: NumberInputOptions) {
    super(parent, options);
    this.classList.add('dragger');

    const opts: NumberInputOptions = options ?? {};

    let isExpr = false;

    const rawValue = opts.value;
    let value: number | string;
    if (rawValue === null || rawValue === undefined) {
      value = 0;
    } else if (typeof rawValue === 'string') {
      isExpr = isNaN(Number(rawValue));
      value = isExpr ? rawValue : parseFloat(rawValue);
    } else if (typeof rawValue === 'number') {
      value = rawValue;
    } else {
      value = 0;
    }

    const precision = opts.precision ?? 3;

    this.value = value;
    this.precision = precision;
    this.units = opts.units ?? '';
    this.disabled = !!opts.disabled;
    this.horizontal = !!opts.horizontal;
    this.linear = !!opts.linear;
    this.step = opts.step ?? 1;
    this.min = opts.min ?? null;
    this.max = opts.max ?? null;
    const container = new Span(this, { class: 'inputfield' });

    if (opts.full) {
      container.classList.add('full');
    }

    if (this.disabled) {
      container.classList.add('disabled');
    }

    const inputClass = opts.inputClass || 'full';
    const input = new TextInput(container, {
      class: ['text', 'number', inputClass],
      value:
        typeof value === 'string'
          ? value
          : value.toFixed(precision) + (opts.units ? opts.units : ''),
      onChange: opts.onChange,
    });
    this.input = input;
    // The document that receives the drag listeners (was stored as `input.ownerDocument`).
    const ownerDocument: Document = document;

    if (this.disabled) {
      input.disabled = true;
    }

    if (opts.tabIndex) {
      input.tabIndex = opts.tabIndex;
    }

    input.addEventListener('keydown', function (e) {
      if (e.keyCode == 38) {
        innerInc(1, e);
      } else if (e.keyCode == 40) {
        innerInc(-1, e);
      } else {
        return;
      }
      e.stopPropagation();
      e.preventDefault();
    });

    const dragger = new Span(container, { class: 'drag_widget' });
    if (this.disabled) {
      dragger.classList.add('disabled');
    }

    this.dragger = dragger;

    dragger.addEventListener('mousedown', innerDown);
    input.addEventListener('wheel', innerWheel, false);
    input.addEventListener('mousewheel', innerWheel, false);

    let docBinded: Document | null = null;
    // The last drag position, in screen coordinates (was stored as `dragger.data`).
    let dragPosition: [number, number] = [0, 0];

    const self = this;

    function innerDown(e: MouseEvent): void {
      if (isExpr) {
        return;
      }
      docBinded = ownerDocument;

      docBinded.removeEventListener('mousemove', innerMove);
      docBinded.removeEventListener('mouseup', innerUp);

      if (!self.disabled) {
        if (self.element.requestPointerLock) {
          self.element.requestPointerLock();
        }
        docBinded.addEventListener('mousemove', innerMove);
        docBinded.addEventListener('mouseup', innerUp);

        dragPosition = [e.screenX, e.screenY];

        self.trigger('startDragging');
      }

      e.stopPropagation();
      e.preventDefault();
    }

    function innerMove(e: MouseEvent): void {
      if (isExpr) {
        return;
      }
      const deltax = e.screenX - dragPosition[0];
      const deltay = dragPosition[1] - e.screenY;
      let diff: [number, number] = [deltax, deltay];
      // movementX is optional in older browsers.
      if ((e.movementX as number | undefined) !== undefined) {
        diff = [e.movementX, -e.movementY];
      }

      dragPosition = [e.screenX, e.screenY];
      const axis = self.horizontal ? 0 : 1;

      innerInc(diff[axis], e);

      e.stopPropagation();
      e.preventDefault();
    }

    function innerWheel(this: HTMLElement, e: Event): void {
      if (isExpr) {
        return;
      }
      if (document.activeElement !== this) {
        return;
      }
      // Legacy 'mousewheel' events carry wheelDelta; standard 'wheel' events carry deltaY.
      const we = e as WheelEvent & { readonly wheelDelta?: number };
      const delta =
        we.wheelDelta !== undefined
          ? we.wheelDelta
          : we.deltaY
          ? -we.deltaY / 3
          : 0;
      innerInc(delta > 0 ? 1 : -1, we);
      e.stopPropagation();
      e.preventDefault();
    }

    function innerUp(e: MouseEvent): void {
      if (isExpr) {
        return;
      }
      self.trigger('stopDragging');
      const doc = docBinded || document;
      docBinded = null;
      doc.removeEventListener('mousemove', innerMove);
      doc.removeEventListener('mouseup', innerUp);
      if (doc.exitPointerLock) {
        doc.exitPointerLock();
      }
      dragger.trigger('blur');
      e.stopPropagation();
      e.preventDefault();
    }

    function innerInc(v: number, e?: ModifierEvent): void {
      if (isExpr) {
        return;
      }
      if (!self.linear) {
        v = v > 0 ? Math.pow(v, 1.2) : Math.pow(Math.abs(v), 1.2) * -1;
      }

      let scale = self.step ? self.step : 1.0;
      if (e && e.shiftKey) {
        scale *= 10;
      } else if (e && e.ctrlKey) {
        scale *= 0.1;
      }

      let value = parseFloat(input.value) + v * scale;

      if (self.max !== null && value > self.max) {
        value = self.max;
      }

      if (self.min !== null && value < self.min) {
        value = self.min;
      }

      let text = value.toFixed(self.precision);
      if (self.units) {
        text += self.units;
      }
      input.value = text;

      input.trigger('change');
    }
  }

  setRange(min: number | null, max: number | null): void {
    this.min = min;
    this.max = max;
  }

  setValue(v: number | string, skipEvent?: boolean): void {
    const isExpr = isNaN(Number(v));
    let value: number | string = v;
    if (!isExpr) {
      let n = parseFloat(String(v));
      if (this.min !== null && n < this.min) {
        n = this.min;
      }
      if (this.max !== null && n > this.max) {
        n = this.max;
      }
      value = n;
    }
    if (this.value == value) {
      return;
    }
    this.value = value;
    let text = String(value);
    if (!isExpr && typeof value === 'number') {
      if (this.precision) {
        text = value.toFixed(this.precision);
      }
      if (this.units) {
        text += this.units;
      }
    }
    if (this.input.value != text) {
      this.input.value = text;
      if (!skipEvent) {
        this.input.onChange.emit(text);
      }
    }
  }

  getValue(): number | string {
    return this.value;
  }
}
