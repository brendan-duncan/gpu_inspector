// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Div } from './div.js';
import { Widget, WidgetOptions } from './widget.js';
import type { Split } from './split.js';

/**
 * A draggable bar to adjust sizes of elements in a splitter.
 */
export class SplitBar extends Div {
  static isSplitBar = true;
  static Horizontal = 0;
  static Vertical = 1;
  /** The thickness of the bar, in pixels. */
  static size = 6;

  /** SplitBar.Horizontal or SplitBar.Vertical. */
  orientation: number;
  private _mousePressed = false;
  private _mouseX = 0;
  private _mouseY = 0;
  private _prevWidget: Widget | null = null;
  private _nextWidget: Widget | null = null;
  private _splitIndex = 0;

  constructor(orientation: number, parent?: Widget | HTMLElement | null, options?: WidgetOptions) {
    super(parent, options);

    this.orientation = orientation;

    this._element.classList.add('splitbar');

    if (this.orientation == SplitBar.Horizontal) {
      this._element.style.height = `${SplitBar.size}px`;
      this._element.style.width = '100%';
      this._element.style.cursor = 'n-resize';
    } else {
      this._element.style.width = `${SplitBar.size}px`;
      this._element.style.height = '100%';
      this._element.style.cursor = 'e-resize';
    }

    this.enablePointerEvents();
  }

  override pointerDownEvent(e: PointerEvent): boolean {
    this._mousePressed = true;
    this._mouseX = e.clientX;
    this._mouseY = e.clientY;
    const parent = this.parent;
    if (parent) {
      for (let i = 0; i < parent.children.length; ++i) {
        const w = parent.children[i];
        if (w === this) {
          this._splitIndex = i;
          this._prevWidget = parent.children[i - 1] ?? null;
          this._nextWidget = parent.children[i + 1] ?? null;
          break;
        }
      }
    }
    if (this._prevWidget) {
      this._prevWidget._startResize();
    }
    if (this._nextWidget) {
      this._nextWidget._startResize();
    }

    this.element.setPointerCapture(e.pointerId);
    return false;
  }

  override pointerMoveEvent(e: PointerEvent): boolean {
    if (!this._mousePressed) {
      return false;
    }

    const split = this.parent as Split | null;
    if (split) {
      if (this.orientation === SplitBar.Horizontal) {
        const dy = e.clientY - this._mouseY;
        if (dy != 0) {
          if (split.mode === 0) {
            const pct = dy / split.height;
            split.position += pct;
          } else {
            split.position += dy;
          }
        }
      } else {
        const dx = e.clientX - this._mouseX;
        if (dx != 0) {
          if (split.mode === 0) {
            const pct = dx / split.width;
            split.position += pct;
          } else {
            split.position += dx;
          }
        }
      }
    }

    this._mouseX = e.clientX;
    this._mouseY = e.clientY;

    return false;
  }

  override pointerUpEvent(): boolean {
    this._prevWidget = null;
    this._nextWidget = null;
    this._mousePressed = false;
    Widget.disablePaintingOnResize = false;
    const parent = this.parent;
    if (parent) {
      for (const w of parent.children) {
        w.repaint(true);
      }
    }
    return false;
  }
}
