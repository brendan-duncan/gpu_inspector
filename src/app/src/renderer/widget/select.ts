// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Widget, WidgetOptions } from './widget.js';
import { Label } from './label.js';
import { TextInput } from './text_input.js';
import { Signal } from '../utils/signal.js';

export type SelectChangeCallback = (value: string, index: number) => void;

export interface SelectOptions extends WidgetOptions {
  /** The option texts to populate the select with. */
  options?: string[];
  /** If true, a text input overlays the select so the value can also be typed. */
  editable?: boolean;
  /** A label text (a Label widget is created next to the select) or an existing Label widget. */
  label?: string | Label;
  value?: string;
  index?: number;
  onChange?: SelectChangeCallback;
}

export class Select extends Widget<HTMLSpanElement> {
  select: Widget<HTMLSelectElement>;
  selectEdit: TextInput | undefined;
  label: Label | undefined;
  /** Emitted with (value, index); the index is omitted when an editable select's text changes. */
  onChange: Signal<(value: string, index?: number) => void>;
  private _onChange: SelectChangeCallback | undefined;

  constructor(parent?: Widget | HTMLElement | null, options?: SelectOptions) {
    super('span', parent);
    this.classList.add('select');

    this.select = new Widget<HTMLSelectElement>('select', this);
    this.select.style.width = '100%';
    this.select.style.height = '20px';
    this.select.style.border = 'none';
    this.select.style.display = 'inline-block';
    this.select.classList.add('select');
    this.onChange = new Signal<(value: string, index?: number) => void>();

    const self = this;
    this.select.element.addEventListener('change', function () {
      if (self.selectEdit) {
        self.selectEdit.value = self.select.element.value;
      } else {
        self.onChange.emit(self.value, self.index);
        if (self._onChange) {
          self._onChange(self.value, self.index);
        }
      }
    });

    if (options && options.editable) {
      this.selectEdit = new TextInput(this, {
        value: options.options?.[0],
        style:
          'position: absolute; top: 2px; left: 0px; width: calc(100% - 20px); height: 20px; border: none;',
      });
      this.selectEdit.onChange.addListener(function () {
        self.onChange.emit(self.value);
        if (self._onChange) {
          self._onChange(self.value, self.index);
        }
      });
    }

    if (options) {
      this.configure(options);
    }

    //this.style.height = '20px';
    this.style.position = 'relative';
    this.style.minWidth = '50px';
  }

  override get disabled(): boolean {
    return super.disabled;
  }

  override set disabled(v: boolean) {
    super.disabled = v;
    this.select.disabled = v;
    if (this.selectEdit) {
      this.selectEdit.disabled = v;
    }
  }

  override configure(options?: SelectOptions): void {
    if (!options) {
      return;
    }
    super.configure(options);

    if (options.options) {
      for (const o of options.options) {
        this.addOption(o);
      }
    }

    if (options.label !== undefined) {
      if (typeof options.label === 'string') {
        this.label = new Label(options.label, this.parent, {
          fixedSize: 0,
          for: this,
        });
      } else {
        this.label = options.label;
        this.label.for = this.id;
        if (!this.label.parent) {
          this.label.parent = this.parent;
        }
      }
    }

    if (options.value !== undefined) {
      this.select.element.value = options.value;
    }

    if (options.index !== undefined) {
      this.select.element.selectedIndex = options.index;
      if (this.selectEdit) {
        this.selectEdit.value = this.select.element.value;
      }
    }

    if (options.onChange !== undefined) {
      this._onChange = options.onChange;
    }
  }

  get index(): number {
    return this.select.element.selectedIndex;
  }

  set index(v: number) {
    this.select.element.selectedIndex = v;
  }

  get value(): string {
    if (this.selectEdit) {
      return this.selectEdit.value;
    }
    return this.select.element.value;
  }

  set value(v: string) {
    if (this.selectEdit) {
      this.selectEdit.value = v;
    } else {
      this.select.element.value = v;
    }
  }

  addOption(text: string): void {
    const o = document.createElement('option');
    o.innerText = text;
    this.select.element.add(o);
  }

  override resize(width: number, height: number): void {
    if (!this._element) {
      return;
    }

    // SELECT elements behave differently than other elements with resizing.
    this.select.element.style.width = `${width}px`;
    this.select.element.style.height = `${height}px`;

    this.onResize();

    if (!Widget.disablePaintingOnResize) {
      if (this.paintEvent) {
        this.paintEvent();
      }
    }
  }
}
