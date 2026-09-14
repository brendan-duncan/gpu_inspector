// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';
import { Signal } from '../utils/signal.js';

export interface TextAreaOptions extends WidgetOptions {
  value?: string;
  placeholder?: string;
  readOnly?: boolean;
  onChange?: (value: string) => void;
  onEdit?: (value: string) => void;
}

export class TextArea extends Widget<HTMLTextAreaElement> {
  onChange: Signal<(value: string) => void>;
  onEdit: Signal<(value: string) => void>;

  // Assigned by configure() from within the Widget constructor; see Input for why `declare`.
  private declare _onChange: ((value: string) => void) | undefined;
  private declare _onEdit: ((value: string) => void) | undefined;

  constructor(parent?: Widget | HTMLElement | null, options?: TextAreaOptions) {
    super('textArea', parent, options);
    this.element.spellcheck = false;
    this.classList.add('text-area');

    this.onChange = new Signal<(value: string) => void>();
    this.onEdit = new Signal<(value: string) => void>();

    const self = this;
    this.element.addEventListener('change', function () {
      const v = self.value;
      self.onChange.emit(v);
      if (self._onChange) {
        self._onChange(v);
      }
    });

    this.element.addEventListener('input', function () {
      const v = self.value;
      self.onEdit.emit(v);
      if (self._onEdit) {
        self._onEdit(v);
      }
    });
  }

  override configure(options?: TextAreaOptions): void {
    if (!options) {
      return;
    }
    super.configure(options);

    if (options.value) {
      this.value = options.value;
    }

    if (options.placeholder) {
      this.placeholder = options.placeholder;
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

  get value(): string {
    return this._element.value;
  }

  set value(t: string) {
    this._element.value = t;
  }

  get placeholder(): string {
    return this._element.placeholder;
  }

  set placeholder(v: string) {
    this._element.placeholder = v;
  }

  get readOnly(): boolean {
    return this._element.readOnly;
  }

  set readOnly(v: boolean) {
    this._element.readOnly = v;
  }
}
