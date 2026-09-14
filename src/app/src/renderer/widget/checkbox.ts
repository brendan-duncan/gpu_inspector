// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Input } from './input.js';
import { Label } from './label.js';
import { Span } from './span.js';
import type { Widget, WidgetOptions } from './widget.js';

export interface CheckboxOptions extends WidgetOptions {
  /** A label text (a Label widget is created next to the checkbox) or an existing Label widget. */
  label?: string | Label;
  checked?: boolean;
  onChange?: (checked: boolean) => void;
  onEdit?: (checked: boolean) => void;
}

export class Checkbox extends Span {
  /** The checkbox Input widget wrapping the <input type="checkbox"> element. */
  inputWidget: Input<boolean>;

  constructor(parent?: Widget | HTMLElement | null, options?: CheckboxOptions) {
    super(parent, options);
    this.classList.add('styled-checkbox-container');

    this.inputWidget = new Input<boolean>(this, options);
    this.inputWidget.type = 'checkbox';
    this.inputWidget.classList.add('styled-checkbox');

    let label = this.label;
    if (!label) {
      // The label must target the input (not this container span), so that
      // clicking the drawn box — the label's ::before — toggles the checkbox.
      label = new Label('', this, { for: this.inputWidget });
      this.label = label;
    }

    if (options?.tooltip) {
      this.inputWidget.title = options.tooltip;
      label.title = options.tooltip;
    }
  }

  /** The <input type="checkbox"> element. */
  get input(): HTMLInputElement {
    return this.inputWidget.element;
  }

  get checked(): boolean {
    return this.inputWidget.checked;
  }

  set checked(v: boolean) {
    this.inputWidget.checked = v;
  }

  get label(): Label | undefined {
    return this.inputWidget.label;
  }

  set label(v: Label | undefined) {
    this.inputWidget.label = v;
  }

  get indeterminate(): boolean {
    return this.inputWidget.indeterminate;
  }

  set indeterminate(v: boolean) {
    this.inputWidget.indeterminate = v;
  }
}
