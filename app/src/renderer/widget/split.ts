// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Div } from './div.js';
import { SplitBar } from './split_bar.js';
import type { Widget, WidgetOptions } from './widget.js';

export interface SplitOptions extends WidgetOptions {
  /** Split.Horizontal or Split.Vertical. */
  direction?: number;
  /** The split position: a fraction (< 1) of the size, or a pixel size (>= 1). */
  position?: number;
}

/**
 * The original assigned `element.width = '0'` / `element.height = '0'` on the child elements,
 * which are not standard properties of generic elements; the assignments are kept as expandos.
 */
type SizedElement = HTMLElement & { width?: string; height?: string };

/**
 * The children of this widget are arranged horizontally or vertically and separated by a
 * draggable SplitBar.
 */
export class Split extends Div {
  static Horizontal = 0;
  static Vertical = 1;
  static Percentage = 0;
  static Pixel = 1;

  private _direction: number = Split.Horizontal;
  private _position = 0.5;
  /** Split.Percentage or Split.Pixel; how `position` is interpreted. */
  mode: number = Split.Percentage;

  constructor(parent?: Widget | HTMLElement | null, options?: SplitOptions) {
    super(parent);
    this.classList.add('split', 'disable-selection');

    if (options) {
      this.configure(options);
    }

    if (this._direction === Split.Horizontal) {
      this.classList.add('hsplit');
    } else {
      this.classList.add('vsplit');
    }
  }

  override configure(options: SplitOptions): void {
    if (options.direction !== undefined) {
      this._direction = options.direction;
    }

    super.configure(options);

    if (options.position !== undefined) {
      this.position = options.position;
      if (this.position > 1) {
        this.mode = Split.Pixel;
      }
    }
  }

  get direction(): number {
    return this._direction;
  }

  get position(): number {
    return this._position;
  }

  set position(pos: number) {
    this._position = pos;
    this.updatePosition();
  }

  updatePosition(): void {
    if (this.children.length < 3) {
      return;
    }

    const numSplitBars = this.children.length - 2;
    const splitBarSize = numSplitBars * SplitBar.size;

    let splitPos: string;
    let splitPos2: string;
    if (this._position < 1) {
      const pct = this._position * 100;
      splitPos = `${pct}%`;
      splitPos2 = `calc(${100 - pct}% - ${splitBarSize}px)`;
    } else {
      splitPos = `${this._position}px`;
      splitPos2 = `calc(100% - ${this._position}px - ${splitBarSize}px)`;
    }

    const first = this.children[0];
    const third = this.children[2];
    if (this._direction == Split.Horizontal) {
      first.style.width = splitPos;
      (first.element as SizedElement).width = '0';

      third.style.width = splitPos2;
      (third.element as SizedElement).width = '0';
    } else {
      (first.element as SizedElement).height = '0';
      first.style.height = splitPos;

      third.style.height = splitPos2;
      (third.element as SizedElement).height = '0';
    }

    this.onResize();
  }

  override appendChild(child: Widget): void {
    if (this.direction == Split.Horizontal)
      child.style.display = 'inline-block';

    if (this.children.length == 0 || child instanceof SplitBar) {
      if (this.children.length == 0) {
        child.style.width = '100%';
        child.style.height = '100%';
      } else {
        if (this._direction == Split.Horizontal) {
          child.style.height = '100%';
        } else {
          child.style.width = '100%';
        }
      }
      super.appendChild(child);
      return;
    }

    const percent = (1 / (this.children.length + 1)) * 100;

    new SplitBar(
      this._direction == Split.Horizontal
        ? SplitBar.Vertical
        : SplitBar.Horizontal,
      this
    );

    super.appendChild(child);

    const numSplitBars = this.children.length - 2;
    const splitBarSize = numSplitBars * SplitBar.size;

    for (const c of this.children) {
      if (!(c instanceof SplitBar)) {
        if (c === this.children[this.children.length - 1]) {
          if (this._direction == Split.Horizontal) {
            (c.element as SizedElement).width = '0';
            c.style.height = '100%';
            c.style.width = `calc(${100 - percent}% - ${splitBarSize}px)`;
          } else {
            (c.element as SizedElement).height = '0';
            c.style.width = '100%';
            c.style.height = `calc(${100 - percent}% - ${splitBarSize}px)`;
          }
        } else {
          if (this._direction == Split.Horizontal) {
            c.style.width = `${percent}%`;
          } else {
            c.style.height = `${percent}%`;
          }
        }
      }

      c.onResize();
    }

    if (this._position != 0.5) {
      this.updatePosition();
    }
  }
}
