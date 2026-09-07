// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Div } from './div.js';
import { Span } from './span.js';
import { Widget, WidgetOptions } from './widget.js';
import { Signal } from '../utils/signal.js';

export interface CollapsibleOptions extends WidgetOptions {
  label?: string;
  collapsed?: boolean;
}

/**
 * A collapsible widget with a header and a body.
 */
export class collapsible extends Widget<HTMLDivElement> {
  titleBar: Div;
  collapseButton: Span;
  label: Span;
  body: Div;
  onExpanded: Signal<() => void>;
  onCollapsed: Signal<() => void>;

  constructor(parent?: Widget | HTMLElement | null, options?: CollapsibleOptions) {
    super('div', parent, options);

    const collapsed = options?.collapsed ?? false;

    this.titleBar = new Div(this, { class: 'title_bar' });
    this.collapseButton = new Span(this.titleBar, {
      class: 'collapsible_button',
      text: collapsed ? '+' : '-',
      style: 'margin-right: 10px;',
    });
    this.label = new Span(this.titleBar, { class: 'object_type', text: options?.label ?? '' });
    this.onExpanded = new Signal<() => void>();
    this.onCollapsed = new Signal<() => void>();

    this.body = new Div(this, { class: ['collapsible_body'] });
    if (collapsed) {
      this.body.element.className = 'collapsible_body collapsed';
    }

    const self = this;

    this.titleBar.element.onclick = function () {
      if (self.collapseButton.text == '-') {
        self.collapseButton.text = '+';
        self.body.element.className = 'collapsible_body collapsed';
        self.onCollapsed.emit();
      } else {
        self.collapseButton.text = '-';
        self.body.element.className = 'collapsible_body';
        self.onExpanded.emit();
      }
    };
  }

  expand(): void {
    this.collapsed = false;
  }

  get collapsed(): boolean {
    return this.collapseButton.text == '+';
  }

  set collapsed(value: boolean) {
    if (this.collapsed == value) {
      return;
    }
    if (value) {
      this.collapseButton.text = '+';
      this.body.element.className = 'collapsible_body collapsed';
      this.onCollapsed.emit();
    } else {
      this.collapseButton.text = '-';
      this.body.element.className = 'collapsible_body';
      this.onExpanded.emit();
    }
  }
}
