// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';

/**
 * A generic DIV element, usually used as a container for other widgets.
 */
export class Div extends Widget {
  static override _idPrefix = 'DIV';

  constructor(parent?: Widget | HTMLElement | WidgetOptions | null, options?: WidgetOptions) {
    super('div', parent, options);
  }
}
