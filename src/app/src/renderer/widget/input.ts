// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';
import { Label } from './label.js';
import { Signal } from '../utils/signal.js';

/** The value an Input reports to its listeners: `checked` for checkboxes, `value` otherwise. */
export type InputValue = string | boolean;

export interface InputOptions<V extends InputValue = InputValue> extends WidgetOptions {
  type?: string;
  checked?: boolean;
  value?: string;
  /** A label text (a Label widget is created next to the input) or an existing Label widget. */
  label?: string | Label;
  readOnly?: boolean;
  onChange?: (value: V) => void;
  onEdit?: (value: V) => void;
}

/**
 * An <input> element widget. `V` is the type of value reported by onChange/onEdit
 * (string for text inputs, boolean for checkboxes).
 */
export class Input<V extends InputValue = InputValue> extends Widget<HTMLInputElement> {
  onChange: Signal<(value: V) => void>;
  onEdit: Signal<(value: V) => void>;

  // These are assigned by configure(), which the Widget constructor calls before the field
  // initializers of this class run; `declare` keeps them from being reset to undefined.
  declare label: Label | undefined;
  private declare _onChange: ((value: V) => void) | undefined;
  private declare _onEdit: ((value: V) => void) | undefined;

  constructor(parent?: Widget | HTMLElement | null, options?: InputOptions<V>) {
    super('input', parent, options);
    this.onChange = new Signal<(value: V) => void>();
    this.onEdit = new Signal<(value: V) => void>();
    const self = this;

    this.element.addEventListener('change', function () {
      const v = (self.type === 'checkbox' ? self.checked : self.value) as V;
      self.onChange.emit(v);
      if (self._onChange) {
        self._onChange(v);
      }
    });

    this.element.addEventListener('input', function () {
      const v = (self.type === 'checkbox' ? self.checked : self.value) as V;
      self.onEdit.emit(v);
      if (self._onEdit) {
        self._onEdit(v);
      }
    });
  }

  override configure(options?: InputOptions<V>): void {
    if (!options) {
      return;
    }
    super.configure(options);

    if (options.type !== undefined) {
      this.type = options.type;
    }

    if (options.checked !== undefined) {
      this.checked = options.checked;
    }

    if (options.value !== undefined) {
      this.value = options.value;
    }

    if (options.label !== undefined) {
      if (typeof options.label === 'string') {
        this.label = new Label(options.label, this.parent, {
          for: this,
        });
      } else {
        this.label = options.label;
        this.label.for = this.id;
      }
    }

    if (options.readOnly !== undefined) {
      this.readOnly = options.readOnly;
    }

    if (options.onChange !== undefined) {
      this._onChange = options.onChange;
    }

    if (options.onEdit !== undefined) {
      this._onEdit = options.onEdit;
    }
  }

  get type(): string {
    return this._element.type;
  }

  set type(v: string) {
    this._element.type = v;
  }

  get checked(): boolean {
    return this._element.checked;
  }

  set checked(v: boolean) {
    this._element.checked = v;
  }

  get indeterminate(): boolean {
    return this._element.indeterminate;
  }

  set indeterminate(v: boolean) {
    this._element.indeterminate = v;
  }

  get value(): string {
    return this._element.value;
  }

  set value(v: string) {
    this._element.value = v;
  }

  get readOnly(): boolean {
    return this._element.readOnly;
  }

  set readOnly(v: boolean) {
    this._element.readOnly = v;
  }

  focus(): void {
    this._element.focus();
  }

  blur(): void {
    this._element.blur();
  }

  select(): void {
    this._element.select();
  }
}
