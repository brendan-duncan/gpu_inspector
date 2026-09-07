// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Div } from './div.js';
import type { Widget, WidgetOptions } from './widget.js';

/**
 * A single content area with multiple panels, each associated with a header in a list.
 */
export class TabPage extends Div {
  static override _idPrefix = 'TABPAGE';
  static isTabPage = true;

  panel: Widget | null;

  constructor(panel: Widget | null, parent?: Widget | HTMLElement | null, options?: WidgetOptions) {
    super(parent, options);
    this.classList.add('tab-page');
    this.style.display = 'none';
    this.panel = panel;
    if (panel) {
      panel.parent = this;
      //panel.style.width = '100%';
    }
  }
}
