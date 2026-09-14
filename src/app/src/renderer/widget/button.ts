// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';

/** A Button callback; invoked with the Button as `this` and the originating mouse event. */
export type ButtonCallback = (this: Button, event: MouseEvent) => void;

export interface ButtonOptions extends WidgetOptions {
  label?: string;
  callback?: ButtonCallback;
  mouseDown?: ButtonCallback;
  mouseUp?: ButtonCallback;
}

export class Button extends Widget<HTMLButtonElement> {
  callback: ButtonCallback | null = null;
  onMouseDown: ButtonCallback | null = null;
  onMouseUp: ButtonCallback | null = null;

  private readonly _click: (event: MouseEvent) => void;
  private readonly _mouseDown: (event: MouseEvent) => void;
  private readonly _mouseUp: (event: MouseEvent) => void;

  constructor(parent?: Widget | HTMLElement | null, options?: ButtonOptions) {
    super('button', parent);
    this.classList.add('button');

    this._click = this.click.bind(this);
    this._mouseDown = this.mouseDown.bind(this);
    this._mouseUp = this.mouseUp.bind(this);

    this.element.addEventListener('click', this._click);
    this.element.addEventListener('mousedown', this._mouseDown);
    this.element.addEventListener('mouseup', this._mouseUp);

    if (options) {
      this.configure(options);
    }
  }

  override configure(options: ButtonOptions): void {
    super.configure(options);
    if (options.callback) {
      this.callback = options.callback;
    }
    if (options.mouseDown) {
      this.onMouseDown = options.mouseDown;
    }
    if (options.mouseUp) {
      this.onMouseUp = options.mouseUp;
    }
    if (options.label) {
      this.text = options.label;
    }
  }

  click(event: MouseEvent): void {
    if (this.callback) {
      this.callback.call(this, event);
    }
  }

  mouseDown(event: MouseEvent): void {
    if (this.onMouseDown) {
      this.onMouseDown.call(this, event);
    }
  }

  mouseUp(event: MouseEvent): void {
    if (this.onMouseUp) {
      this.onMouseUp.call(this, event);
    }
  }
}
