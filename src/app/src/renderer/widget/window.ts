// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';
import { Signal } from '../utils/signal.js';

/**
 * A window widget fills the entire browser window. It will resize with the
 * browser. A Window can have an Overlay, which is a [Widget] that will be
 * resized to fill the entire window, and can be used to create full screen
 * modal editors.
 */
export class Window extends Widget {
  static override isWindow = true;
  static override _idPrefix = 'WINDOW';

  _overlay: Widget | null;
  _onResizeCB: () => void;
  onWindowResized: Signal<() => void>;

  constructor(options?: WidgetOptions) {
    super(document.body, options);
    this._overlay = null;
    this._onResizeCB = this.windowResized.bind(this);
    window.addEventListener('resize', this._onResizeCB);
    this.onWindowResized = new Signal();
    Widget.window = this;
  }

  windowResized(): void {
    this._onResize(window.innerWidth, window.innerHeight);
  }

  /**
   * The width of the widget.
   */
  override get width(): number {
    return window.innerWidth;
  }

  /**
   * The height of the widget.
   */
  override get height(): number {
    return window.innerHeight;
  }

  /**
   * The active overlay widget, which covers the entire window temporarily.
   */
  get overlay(): Widget | null {
    return this._overlay;
  }

  set overlay(v: Widget | null) {
    if (this._overlay === v) {
      return;
    }

    if (this._overlay !== null) {
      this._element.removeChild(this._overlay._element);
    }

    this._overlay = v;

    if (this._overlay) {
      this._element.appendChild(this._overlay._element);
      this._overlay.setPosition(0, 0, 'absolute');
      this._overlay.resize(window.innerWidth, window.innerHeight);
    }
  }

  /**
   * The widget has been resized.
   */
  _onResize(width: number, height: number): void {
    this.onWindowResized.emit();
    this.repaint();
    if (this._element) {
      if (this._overlay) {
        this._overlay.resize(width, height);
      }
    }
    this.onResize();
  }
}
