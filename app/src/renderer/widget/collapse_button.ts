// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Span } from './span.js';
import type { Widget, WidgetOptions } from './widget.js';

/** The state string stored in the button's data-value attribute. */
export type CollapseButtonState = 'open' | 'closed';

export type CollapseButtonChangeCallback = (value: CollapseButtonState) => void;

/**
 * A small triangle button that toggles between an open and a closed state.
 */
export class CollapseButton extends Span {
  onChange: CollapseButtonChangeCallback | null | undefined;
  /** Set by owners (e.g. TreeWidget) that handle the click themselves. */
  stopPropagation = false;

  constructor(
    state: boolean,
    onChange?: CollapseButtonChangeCallback | null,
    parent?: Widget | HTMLElement | null,
    options?: WidgetOptions
  ) {
    super(parent, options);
    this.onChange = onChange;

    this.classList.add('collapse-button', state ? 'collapse-button-open' : 'collapse-button-closed');
    this.element.innerHTML = state ? '&#9660;' : '&#9658;';
    this.element.dataset['value'] = state ? 'open' : 'closed';

    this.addEventListener('click', onClick);
    const self = this;
    function onClick(this: HTMLElement, e: MouseEvent): void {
      self.value = this.dataset['value'] === 'open' ? false : true;
      if (self.stopPropagation) {
        // Preserved from the original: the method is referenced but never invoked, so the
        // click still propagates to the owner.
        void e.stopPropagation;
      }
    }
  }

  setEmpty(v: boolean): void {
    if (v) {
      this.classList.add('empty');
    } else {
      this.classList.remove('empty');
    }
  }

  expand(): void {
    this.value = true;
  }

  collapse(): void {
    this.value = false;
  }

  set value(v: boolean) {
    if (this.dataset['value'] == (v ? 'open' : 'closed')) {
      return;
    }

    if (!v) {
      this.dataset['value'] = 'closed';
      this.element.innerHTML = '&#9658;';
      this.classList.remove('collapse-button-open');
      this.classList.add('collapse-button-closed');
    } else {
      this.dataset['value'] = 'open';
      this.element.innerHTML = '&#9660;';
      this.classList.add('collapse-button-open');
      this.classList.remove('collapse-button-closed');
    }

    if (this.onChange) {
      this.onChange(this.dataset['value'] as CollapseButtonState);
    }
  }

  get value(): CollapseButtonState {
    // data-value is always set by the constructor and the setter.
    return this.dataset['value'] as CollapseButtonState;
  }
}
