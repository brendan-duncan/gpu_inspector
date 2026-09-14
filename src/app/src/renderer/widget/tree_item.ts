// Ported from WebGPU Inspector (https://github.com/brendan-duncan/webgpu_inspector), MIT license.
import { Signal } from '../utils/signal.js';
import { Widget, WidgetOptions } from "./widget.js";
import { Div } from "./div.js";
import { Span } from "./span.js";
import type { CollapseButton } from "./collapse_button.js";

/** Callback that fills one of a tree item's slots (icon, content, precontent, postcontent). */
export type TreeItemRenderer = (target: Span, data: TreeItemData) => void;

/** Value returned by `TreeItemData.onDragData`: extra DataTransfer entries for a drag. */
export type TreeItemDragData = Record<string, string>;

/**
 * The data record backing a tree item. Consumers routinely attach their own
 * fields to it, so it carries an open index signature.
 */
export interface TreeItemData {
  id?: string;
  className?: string;
  icon?: string | TreeItemRenderer;
  content?: string | Span | TreeItemRenderer;
  precontent?: string | TreeItemRenderer;
  postcontent?: string | TreeItemRenderer;
  visible?: boolean;
  children?: TreeItemData[];
  collapsible?: boolean;
  collapsed?: boolean;
  skipDrag?: boolean;
  /** Set by TreeItem: the widget created for this record. */
  item?: TreeItem;
  /** Return truthy to stop the tree's own `itemSelected` hook from running. */
  callback?: (this: unknown, data: TreeItemData, addToSelection: boolean) => unknown;
  onCollapseChange?: (item: TreeItem, data: TreeItemData, value: string) => void;
  onDragData?: () => TreeItemDragData | null | undefined;
  [key: string]: unknown;
}

export interface TreeItemOptions extends WidgetOptions {
  data?: TreeItemData;
  level?: number;
}

function isTreeItemOptions(p: Widget | HTMLElement | TreeItemOptions): p is TreeItemOptions {
  return p.constructor === Object;
}

export class TreeItem extends Widget {
  static isTreeItem = true;

  override data: TreeItemData;
  itemId: string;
  level: number;
  /** Id of the parent item; assigned by TreeWidget when the item is inserted. */
  parentId: string | null | undefined = undefined;
  /** Created lazily by TreeWidget once the item has (or may have) children. */
  collapseButton: CollapseButton | null;
  onClick: Signal<() => void>;

  titleElement: Div;
  preContent: Span;
  indent: Span;
  collapseButtonArea: Span;
  icon: Span;
  content: Span;
  postContent: Span;

  constructor(parent?: Widget | HTMLElement | TreeItemOptions | null, options?: TreeItemOptions) {
    let parentWidget: Widget | HTMLElement | null | undefined = parent as Widget | HTMLElement | null | undefined;
    if (parent && isTreeItemOptions(parent)) {
      options = parent;
      parentWidget = null;
    }

    super("li", parentWidget, options);

    options = options || {};
    this.data = options.data || {};
    this.itemId = this.data.id || "";
    this.level = options.level || 0;
    this.collapseButton = null;
    this.onClick = new Signal<() => void>();
    this.data.item = this;

    this.titleElement = new Div(this, {class: "tree-item-title"});
    this.preContent = new Span(this.titleElement, {class:"precontent"});
    this.indent = new Span(this.titleElement, {class:"indent"});
    this.collapseButtonArea = new Span(this.titleElement, {class:"tree-collapse"});
    this.icon = new Span(this.titleElement, {class:"icon"});
    this.content = new Span(this.titleElement, {class:"content"});
    this.postContent = new Span(this.titleElement, {class:"postcontent"});

    if (this.data.className) {
      this.titleElement.classList.add(this.data.className);
    }

    if (this.data.icon) {
      if (typeof this.data.icon === "function") {
        this.data.icon(this.icon, this.data);
      } else {
        this.icon.element.innerHTML = this.data.icon;
      }
    }

    if (this.data.content) {
      if (typeof this.data.content === "function") {
        this.data.content(this.content, this.data);
      } else if (this.data.content instanceof Span) {
        this.content.appendChild(this.data.content);
      } else {
        this.content.text = this.data.content || this.data.id || "";
      }
    } else {
      // Upstream quirk kept as-is: HTMLSpanElement has no `text` property, so this
      // only sets an expando on the element and renders nothing.
      (this.content.element as HTMLElement & { text?: string }).text = this.data.id || "";
    }

    if (this.data.precontent) {
      if (typeof this.data.precontent === "function") {
        this.data.precontent(this.preContent, this.data);
      } else {
        this.preContent.element.innerHTML = this.data.precontent;
      }
    }

    if (this.data.postcontent) {
      if (typeof this.data.postcontent === "function") {
        this.data.postcontent(this.postContent, this.data);
      } else {
        this.postContent.element.innerHTML = this.data.postcontent;
      }
    }

    if (this.data.visible === false) {
      this.style.display = "none";
    }
  }
}
