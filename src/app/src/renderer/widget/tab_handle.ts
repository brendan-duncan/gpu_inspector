// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Div } from './div.js';
import type { Widget, WidgetOptions } from './widget.js';
import type { TabPage } from './tab_page.js';
import type { TabWidget } from './tab_widget.js';

export interface TabHandleOptions extends WidgetOptions {
  displayCloseButton?: boolean;
}

/** Drag events in Chromium carry the non-standard layerX offset. */
type LayerDragEvent = DragEvent & { readonly layerX: number };

/**
 * The handle widget for a tab panel.
 */
export class TabHandle extends Div {
  static override _idPrefix = 'TAB';
  /** The handle currently being dragged, if any. */
  static DragWidget: TabHandle | null = null;

  page: TabPage;
  parentWidget: TabWidget;
  textElement: Div;
  closeButton: Div | undefined;

  constructor(
    title: string,
    page: TabPage,
    parentWidget: TabWidget,
    parent?: Widget | HTMLElement | null,
    options?: TabHandleOptions
  ) {
    super(parent);

    this.title = title;
    this.page = page;
    this.parentWidget = parentWidget;

    this.classList.add('tab-handle', 'disable-selection');

    this.textElement = new Div(this, {
      class: 'tab-handle-text',
      text: title,
    });

    this.draggable = true;

    this.enableMouseEvents();
    this.enableDoubleClickEvent();

    this.configure(options);

    this.enableDropEvents();
  }

  override dragStartEvent(): void {
    TabHandle.DragWidget = this;
  }

  override dragEndEvent(): void {
    TabHandle.DragWidget = null;
  }

  override dragOverEvent(e: DragEvent): void {
    if (!TabHandle.DragWidget) {
      return;
    }

    const src = e.srcElement as Element | null;
    if (src && src.classList.contains('tab-handle') && this !== TabHandle.DragWidget) {
      if ((e as LayerDragEvent).layerX < this.width * 0.5) {
        e.preventDefault();
        this.style.borderRight = '';
        this.style.borderLeft = '4px solid #fff';
      } else {
        e.preventDefault();
        this.style.borderLeft = '';
        this.style.borderRight = '4px solid #fff';
      }
    }
  }

  override dropEvent(e: DragEvent): void {
    this.style.borderLeft = '';
    this.style.borderRight = '';
    const src = e.srcElement as Element | null;
    if (src && src.classList.contains('tab-handle')) {
      if ((e as LayerDragEvent).layerX < this.width * 0.5) {
        console.log('Insert Before');
      } else {
        console.log('Insert After');
      }
    }
  }

  override dragEnterEvent(): void {
    this.style.borderLeft = '';
    this.style.borderRight = '';
  }

  override dragLeaveEvent(): void {
    this.style.borderLeft = '';
    this.style.borderRight = '';
  }

  override configure(options?: TabHandleOptions): void {
    if (!options) {
      return;
    }
    super.configure(options);
    if (options.displayCloseButton) {
      this.closeButton = new Div(this, {
        class: 'tab-handle-close-button',
      });
      this.closeButton.title = 'Close Tab';

      // Set the close button text
      const closeIcon = 'icon-remove-sign';
      this.closeButton.element.innerHTML = `<span class="${closeIcon}">x</span>`;
      this.closeButton.addEventListener('click', () => {
        this.parentWidget.closeTabHandle(this);
      });
    }
  }

  /**
   * Is this tab currently active?
   */
  get isActive(): boolean {
    return this.classList.contains('tab-handle-selected');
  }

  /**
   * Set the active state of the tab (does not affect other tabs, which should
   * be set as inactive).
   */
  set isActive(a: boolean) {
    if (a == this.isActive) {
      return;
    }

    if (a) {
      this.classList.add('tab-handle-selected');
      this.page.style.display = 'block';
      this.style.zIndex = '10';
    } else {
      this.classList.remove('tab-handle-selected');
      this.page.style.display = 'none';
      this.style.zIndex = '0';
    }
  }

  override mousePressEvent(): boolean {
    this.parentWidget.setHandleActive(this);
    return false;
  }

  override doubleClickEvent(): boolean {
    //this.maximizePanel();
    return false;
  }

  maximizePanel(): void {
    //Widget.window.maximizePanelToggle(this.title, this.page.panel);
  }
}
