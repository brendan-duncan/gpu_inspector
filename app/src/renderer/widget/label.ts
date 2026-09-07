// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';

export interface LabelOptions extends WidgetOptions {
  /** The element (or element id) the label is for. */
  for?: string | Widget | HTMLElement | null;
}

export class Label extends Widget<HTMLLabelElement> {
  constructor(text: string, parent?: Widget | HTMLElement | LabelOptions | null, options?: LabelOptions) {
    super('label', parent, options);
    this.classList.add('label');
    this.text = text;
  }

  override configure(options: LabelOptions): void {
    if (!options) {
      return;
    }
    super.configure(options);
    if (options.for) {
      this.for = options.for;
    }
  }

  get for(): string {
    return this._element.htmlFor;
  }

  set for(v: string | Widget | HTMLElement | null | undefined) {
    if (!v) {
      this._element.htmlFor = '';
    } else if (typeof v === 'string') {
      this._element.htmlFor = v;
    } else {
      this._element.htmlFor = v.id;
    }
  }
}
