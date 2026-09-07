// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Input, InputOptions } from './input.js';
import type { Widget } from './widget.js';

export interface TextInputOptions extends InputOptions<string> {
  placeholder?: string;
}

export class TextInput extends Input<string> {
  constructor(parent?: Widget | HTMLElement | null, options?: TextInputOptions) {
    super(parent, options);
    this.classList.add('text-input');
    this.type = 'text';

    this.enableKeyPressEvent();
  }

  override configure(options?: TextInputOptions): void {
    if (!options) return;
    super.configure(options);
    if (options.placeholder) {
      this.placeholder = options.placeholder;
    }
  }

  get placeholder(): string {
    return this.element.placeholder;
  }

  set placeholder(v: string) {
    this.element.placeholder = v;
  }

  override keyPressEvent(e: KeyboardEvent): boolean {
    if (e.keyCode === 27) {
      // Escape
      if (e.target instanceof HTMLElement) {
        e.target.blur();
      }
    }
    return true;
  }
}
