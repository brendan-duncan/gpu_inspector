// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Div } from './div.js';
import { TabHandle } from './tab_handle.js';
import { TabPage } from './tab_page.js';
import type { Widget, WidgetOptions } from './widget.js';
import { Signal } from '../utils/signal.js';

export interface TabDescriptor {
  label: string;
  contents: Widget;
}

export interface TabWidgetOptions extends WidgetOptions {
  displayCloseButton?: boolean;
  tabs?: TabDescriptor[];
}

/** The label each panel was added with (the original stored this as `panel._tabLabel`). */
const tabLabels = new WeakMap<Widget, string>();

/**
 * A TabWidget has multiple children widgets, only one of which is visible at a time. Selecting
 * the active child is done via a header of tab handles.
 */
export class TabWidget extends Div {
  static override _idPrefix = 'TABWIDGET';

  private _activeTab = -1;
  displayCloseButton = false;

  /** Emitted when the active tab changes. Receives (activeIndex, panel). */
  onActiveTabChanged: Signal<(index: number, panel: Widget | null) => void>;
  /** Emitted when a tab is closed. Receives the panel widget being removed. */
  onTabClosed: Signal<(panel: Widget) => void>;

  headerElement: Div;
  iconsElement: Div;
  tabListElement: Div;
  contentElement: Div;

  constructor(parent?: Widget | HTMLElement | null, options?: TabWidgetOptions) {
    super(parent);

    this.onActiveTabChanged = new Signal<(index: number, panel: Widget | null) => void>();
    this.onTabClosed = new Signal<(panel: Widget) => void>();

    this._element.classList.add('tab-widget');

    this.headerElement = new Div(this);
    this.headerElement.classList.add('tab-header');

    this.iconsElement = new Div(this.headerElement);
    this.iconsElement.classList.add('tab-icons');

    this.tabListElement = new Div(this.headerElement);
    this.tabListElement.classList.add('tab-handle-list-container');

    this.contentElement = new Div(this);
    this.contentElement.classList.add('tab-content');
    this.contentElement.style.height = `calc(100% - ${this.headerElement.height}px)`;

    if (options) {
      this.configure(options);
    }
  }

  override configure(options: TabWidgetOptions): void {
    super.configure(options);

    if (options.displayCloseButton !== undefined) {
      this.displayCloseButton = options.displayCloseButton;
    }

    if (options.tabs !== undefined) {
      for (const tab of options.tabs) {
        this.addTab(tab.label, tab.contents);
      }
    }
  }

  /**
   * Remove all of the icons.
   */
  clearIcons(): void {
    this.iconsElement.children.length = 0;
  }

  /**
   * The label the given panel was added with, if it was added through addTab().
   */
  static getTabLabel(panel: Widget): string | undefined {
    return tabLabels.get(panel);
  }

  /**
   * Add a tab.
   * @returns The handle widget for the new tab.
   */
  addTab(label: string, panel: Widget): TabHandle {
    tabLabels.set(panel, label);
    const page = new TabPage(panel, this.contentElement);
    const handle = new TabHandle(label, page, this, this.tabListElement, {
      displayCloseButton: this.displayCloseButton,
    });

    if (this.tabListElement.children.length == 1) {
      this._activeTab = 0;
      handle.isActive = true;
      if (page) {
        page.repaint(true);
      }
      this.onActiveTabChanged.emit(0, panel);
    }

    panel.domChanged();

    return handle;
  }

  /**
   * Close a tab.
   */
  closeTabHandle(handle: TabHandle): void {
    const index = this.tabListElement.children.indexOf(handle);
    if (index == -1) {
      return;
    }

    const page = this.contentElement.children[index];
    const closedPanel = page?.children?.[0];

    this.tabListElement.removeChild(handle);
    this.contentElement.removeChild(page);

    if (closedPanel) {
      this.onTabClosed.emit(closedPanel);
    }

    if (this._activeTab == index) {
      this._activeTab = -1;
    }

    if (this._activeTab == -1 && this.numTabs > 0) {
      this.activeTab = 0;
    } else if (this.numTabs === 0) {
      this.onActiveTabChanged.emit(-1, null);
    }
  }

  /**
   * The number of tabs.
   */
  get numTabs(): number {
    return this.tabListElement.children.length;
  }

  /**
   * The index of the active tab.
   */
  get activeTab(): number {
    return this._activeTab;
  }

  /**
   * Set the current active tab.
   */
  set activeTab(index: number) {
    if (index < 0 || index > this.tabListElement.children.length) {
      return;
    }

    const changed = index !== this._activeTab;

    for (let i = 0, l = this.tabListElement.children.length; i < l; ++i) {
      const handle = this.tabListElement.children[i] as TabHandle;
      handle.isActive = i == index;
    }

    this._activeTab = index;

    const page = this.contentElement.children[this._activeTab].children[0];
    if (page) {
      page.repaint(true);
    }

    if (changed) {
      this.onActiveTabChanged.emit(index, page);
    }
  }

  isPanelVisible(panel: Widget): boolean {
    for (let i = 0, l = this.numTabs; i < l; ++i) {
      const h = this.tabListElement.children[i] as TabHandle;
      const p = h.page.children[0];
      if (panel === p) return this._activeTab == i;
    }
    return false;
  }

  setActivePanel(panel: Widget): void {
    for (let i = 0, l = this.numTabs; i < l; ++i) {
      const h = this.tabListElement.children[i] as TabHandle;
      const p = h.page.children[0];
      if (panel === p) this.activeTab = i;
    }
  }

  /**
   * Set the tab with the given [handle] as active.
   */
  setHandleActive(handle: TabHandle): void {
    for (let i = 0, l = this.numTabs; i < l; ++i) {
      const h = this.tabListElement.children[i];
      if (h === handle) this.activeTab = i;
    }
  }

  /**
   * Find the TabWidget that contains the given widget, if any.
   * If a TabWidget is found, then an array with the tab widget and the actual tab page
   * is returned.
   */
  static findParentTabWidget(panel: Widget): [TabWidget | null, TabPage] | null {
    let p = panel.parent;
    while (p) {
      if (p instanceof TabPage) {
        const tabWidget = (p.parent?.parent ?? null) as TabWidget | null;
        return [tabWidget, p];
      }
      p = p.parent;
    }
    return null;
  }
}
