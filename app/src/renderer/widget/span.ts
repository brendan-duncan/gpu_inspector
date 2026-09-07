// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';

/**
 * A SPAN element widget.
 */
export class Span extends Widget {
  static override _idPrefix = 'SPAN';

  constructor(parent?: Widget | HTMLElement | WidgetOptions | null, options?: WidgetOptions) {
    super('span', parent, options);
    if (options?.text && !options?.tooltip) {
      this.tooltip = options.text;
    }
  }
}
