// Ported from WebGPU Inspector (https://github.com/brendan-duncan/webgpu_inspector), MIT license.
import { Widget, WidgetOptions } from "./widget.js";
import { Div } from "./div.js";
import { Span } from "./span.js";
import { TreeItem, TreeItemData } from "./tree_item.js";
import { CollapseButton } from "./collapse_button.js";
import { TextInput } from "./text_input.js";
import { Signal } from '../utils/signal.js';

export interface TreeWidgetOptions extends WidgetOptions {
  /** The root item record; its `children` populate the tree. */
  data?: TreeItemData;
  /** Double-click renames an item inline. Default false. */
  allowRename?: boolean;
  /** Items can be dragged onto other items. Default true. */
  allowDrag?: boolean;
  /** Ctrl/shift click extends the selection. Default false. */
  allowMultiSelection?: boolean;
  /** Extra indent levels applied to every item. Default 0. */
  indentOffset?: number;
  /** Items at or below this depth start collapsed. Default 3. */
  collapsedDepth?: number;
  /** Mark newly created items as selected. */
  selected?: boolean;
  /** Collapse an item when its collapse button is created. */
  collapsed?: boolean;
}

/** Payload of `TreeWidget.onItemDropped`. */
export interface TreeItemDropInfo {
  item: TreeItem;
  event: DragEvent;
}

/** Payload of `TreeWidget.onItemMoved`. */
export interface TreeItemMoveInfo {
  item: TreeItem | null;
  parentItem: TreeItem | null;
}

/** Payload of `TreeWidget.onItemContextMenu`. */
export interface TreeItemContextInfo {
  item: TreeItem;
  data: TreeItemData;
}

/** Predicate for `TreeWidget.filterByRule`. */
export type TreeFilterRule<P> = (data: TreeItemData, content: Span, param: P | undefined) => boolean;

/** An item, or the id of one. */
export type TreeItemRef = string | TreeItem;

function isTreeWidgetOptions(p: Widget | HTMLElement | TreeWidgetOptions): p is TreeWidgetOptions {
  return p.constructor === Object;
}

// Inline-rename state, keyed by the content span being edited (was an ad-hoc
// `_editing`/`_oldName` pair on the widget).
const renameState = new WeakMap<Widget, { oldName: string }>();

/**
 * Provides a hierarchical view of items.
 */
export class TreeWidget extends Div {
  static Indent = 20;

  options: TreeWidgetOptions;
  override data: TreeItemData;
  rootItem: TreeItem | null;
  selection: TreeItem[];

  // These are assigned by configure(), which the Widget constructor calls before
  // this class's field initializers would run, so they must not be re-initialized.
  declare allowRename: boolean;
  declare allowDrag: boolean;
  declare allowMultiSelection: boolean;
  declare indentOffset: number;
  declare collapsedDepth: number;

  onBackgroundClicked: Signal<(e: MouseEvent) => void>;
  onContextMenu: Signal<(e: MouseEvent) => void>;
  onItemDeselected: Signal<(data: TreeItemData) => void>;
  onDeselectAll: Signal<() => void>;
  onItemSelected: Signal<(data: TreeItemData, addToSelection: boolean) => void>;
  onItemDoubleClicked: Signal<(data: TreeItemData) => void>;
  onItemRenamed: Signal<(oldName: string | undefined, newName: string, data: TreeItemData) => void>;
  onItemDropped: Signal<(info: TreeItemDropInfo) => void>;
  onItemMoved: Signal<(info: TreeItemMoveInfo) => void>;
  onItemContextMenu: Signal<(e: MouseEvent, info: TreeItemContextInfo) => void>;

  /** Optional hook: called after a selection when the item's own `callback` didn't handle it. */
  itemSelected?: (data: TreeItemData, addToSelection: boolean) => void;
  /** Optional hook: return false to veto moving `dragItem` under `dropItem`. */
  onMoveItem?: (dragItem: TreeItem | null, dropItem: TreeItem | null) => boolean | void;
  /** Optional hook: called after any drop on an item. */
  onDropItem?: (e: DragEvent, data: TreeItemData) => void;

  private _semiSelected: TreeItem[];
  private _skipScroll = false;

  constructor(parent?: Widget | HTMLElement | TreeWidgetOptions | null, options?: TreeWidgetOptions) {
    let parentWidget: Widget | HTMLElement | null | undefined = parent as Widget | HTMLElement | null | undefined;
    if (parent && isTreeWidgetOptions(parent)) {
      options = parent;
      parentWidget = null;
    }

    super(parentWidget, options);

    this.options = options || {};

    this.classList.add("tree-widget");
    this.data = this.options.data || {};

    this.onBackgroundClicked = new Signal();
    this.onContextMenu = new Signal();
    this.onItemDeselected = new Signal();
    this.onDeselectAll = new Signal();
    this.onItemSelected = new Signal();
    this.onItemDoubleClicked = new Signal();
    this.onItemRenamed = new Signal();
    this.onItemDropped = new Signal();
    this.onItemMoved = new Signal();
    this.onItemContextMenu = new Signal();

    const self = this;

    this.addEventListener("click", function (e: MouseEvent) {
      if (e.srcElement !== self.element) {
        return;
      }
      self.onBackgroundClicked.emit(e);
    });

    self.addEventListener("contextmenu", function (e: MouseEvent) {
      if (e.button != 2) {
        return false;
      }
      self.onContextMenu.emit(e);
      e.preventDefault();
      return false;
    });

    const rootItem = this.createAndInsert(this.data, this.options, null);
    if (rootItem) {
      rootItem.classList.add("tree-root-item");
    }

    this.selection = [];
    this._semiSelected = [];

    this.rootItem = rootItem;
  }

  override configure(options: TreeWidgetOptions): void {
    super.configure(options);

    this.allowRename =
      options.allowRename !== undefined ? options.allowRename : false;

    this.allowDrag = options.allowDrag !== undefined ? options.allowDrag : true;

    this.allowMultiSelection =
      options.allowMultiSelection !== undefined
        ? options.allowMultiSelection
        : false;

    this.indentOffset =
      options.indentOffset !== undefined ? options.indentOffset : 0;

    this.collapsedDepth =
      options.collapsedDepth !== undefined ? options.collapsedDepth : 3;
  }

  /**
   * Update the tree with new data. The old data will be discarded.
   */
  setData(data: TreeItemData | null | undefined): void {
    this.clear(false);
    this.data = data || {};
    if (data) {
      const rootItem = this.createAndInsert(data, this.options, null);
      if (rootItem) {
        rootItem.classList.add("tree-root-item");
        this.rootItem = rootItem;
      } else {
        this.rootItem = null;
      }
    } else {
      this.rootItem = null;
    }
  }

  /**
   * Insert an item into the tree.
   */
  insertItem(data: TreeItemData, parentId?: string | null, position?: number, options?: TreeWidgetOptions): TreeItem | null {
    if (!parentId) {
      const root = this.children[0] as TreeItem | undefined;
      if (root) {
        parentId = root.itemId;
      }
    }

    const element = this.createAndInsert(data, options, parentId, position);
    return element;
  }

  createAndInsert(data: TreeItemData, options: TreeWidgetOptions | undefined, parentId: string | null | undefined, elementIndex?: number): TreeItem | null {
    // Find the parent
    let parentElementIndex = -1;
    if (parentId) {
      parentElementIndex = this._findElementIndex(parentId);
    } else if (parentId === undefined) {
      parentElementIndex = 0; // Root
    }

    let parent: TreeItem | null = null;
    let childLevel = 0;

    // Find the level
    if (parentElementIndex != -1) {
      parent = this.children[parentElementIndex] as TreeItem;
      childLevel = parent.level + 1;
    }

    // Create
    const element = this.createTreeItem(data, options, childLevel);
    if (!element) {
      return null;
    }

    element.parentId = parentId;

    // Check
    const existingItem = this.getItem(element.itemId);
    if (existingItem) {
      //Log.warning("There is another item with the same ID in this tree.", existingItem.id, element.id);
    }

    // Insert
    if (parentElementIndex == -1) {
      this.appendChild(element);
    } else {
      this._insertInside(element, parentElementIndex, elementIndex);
    }

    // Compute visibility according to parents
    if (parent && !this._isNodeChildrenVisible(parentId)) {
      element.classList.add("hidden");
    }

    // Children
    if (data.children) {
      for (const c of data.children) {
        this.createAndInsert(c, options, data.id);
      }
    }

    // Update collapse button
    if (parentId) {
      this._updateCollapseButton(this._findElement(parentId));
    }

    if (data?.collapsible) {
        this._updateCollapseButton(element);
    }

    if (options?.selected) {
      this._markAsSelected(element, true, false);
    }

    if (data.collapsed) {
      this.collapseItem(data.id);
    }

    return element;
  }

  private _insertInside(element: TreeItem, parentIndex: number, offsetIndex?: number, level?: number): void {
    const parent = this.children[parentIndex] as TreeItem | undefined;
    if (!parent) {
      throw `No parent node found. Index: ${parentIndex}, nodes: ${this.children.length}`;
    }

    const parentLevel = parent.level;
    const childLevel = level !== undefined ? level : parentLevel + 1;

    const indent = element.indent;
    if (indent) {
      indent.style.paddingLeft =
        (childLevel + this.indentOffset) * TreeWidget.Indent + "px";
    }

    element.level = childLevel;

    // Under level nodes
    for (let j = parentIndex + 1; j < this.children.length; ++j) {
      const newChildNode = this.children[j] as TreeItem;
      if (
        !newChildNode.classList ||
        !newChildNode.classList.contains("tree-item")
      ) {
        continue;
      }

      const currentLevel = newChildNode.level;

      if (currentLevel == childLevel && offsetIndex) {
        offsetIndex--;
        continue;
      }

      // Last position
      if (currentLevel < childLevel || (offsetIndex === 0 && currentLevel === childLevel)) {
        this.insertBefore(element, newChildNode);
        return;
      }
    }

    this.appendChild(element);
  }

  private _isNodeChildrenVisible(id: TreeItemRef | null | undefined): boolean {
    const node = this.getItem(id);
    if (!node) {
      return false;
    }

    if (node.classList.contains("hidden")) {
      return false;
    }

    // Check CollapseButton
    const collapseButton = node.collapseButton;
    if (!collapseButton) {
      return true;
    }

    if (collapseButton.value === "closed") {
      return false;
    }

    return true;
  }

  private _findElement(id: string | null | undefined): TreeItem | null {
    if (!id || typeof id !== "string") {
      throw "_findElement param must be a string with item id";
    }

    for (const c of this.children as TreeItem[]) {
      if (c.itemId == id) {
        return c;
      }
    }
    return null;
  }

  private _findElementIndex(id: TreeItemRef): number {
    for (let i = 0, l = this.children.length; i < l; ++i) {
      const childNode = this.children[i] as TreeItem;
      if (!childNode.classList || !childNode.classList.contains("tree-item")) {
        continue;
      }

      if (typeof id === "string") {
        if (childNode.itemId === id) {
          return i;
        }
      } else if (childNode === id) {
        return i;
      }
    }

    return -1;
  }

  private _findElementLastChildIndex(startIndex: number): number {
    if (startIndex == -1) {
      return -1;
    }

    const level = (this.children[startIndex] as TreeItem).level;

    for (let i = startIndex + 1, l = this.children.length; i < l; ++i) {
      const childNode = this.children[i] as TreeItem;
      if (!childNode.classList || !childNode.classList.contains("tree-item")) {
        continue;
      }

      const currentLevel = childNode.level;
      if (currentLevel == level) {
        return i;
      }
    }

    return -1;
  }

  private _findChildElements(id: TreeItemRef, onlyDirect?: boolean): TreeItem[] | undefined {
    const parentIndex = this._findElementIndex(id);
    if (parentIndex == -1) {
      return;
    }

    const parent = this.children[parentIndex] as TreeItem;
    const parentLevel = parent.level;

    const result: TreeItem[] = [];

    for (let i = parentIndex + 1, l = this.children.length; i < l; ++i) {
      const childNode = this.children[i] as TreeItem;
      if (!childNode.classList || !childNode.classList.contains("tree-item")) {
        continue;
      }

      const currentLevel = childNode.level;
      if (onlyDirect && currentLevel > parentLevel + 1) {
        continue;
      }

      if (currentLevel <= parentLevel) {
        return result;
      }

      result.push(childNode);
    }

    return result;
  }

  createTreeItem(data: TreeItemData | null | undefined, options: TreeWidgetOptions | undefined, level: number): TreeItem | undefined {
    if (data === null || data === undefined) {
      //Log.error("Tree item cannot be null");
      return;
    }

    options = options || this.options;

    const item = new TreeItem({ data, level, class: "tree-item" });

    const self = this;

    const row = item.element;
    row.addEventListener("click", _onItemSelected);
    row.addEventListener("dblclick", _onItemDoubleClicked);
    row.addEventListener("mousedown", _onItemContextMenu);

    function _onItemContextMenu(e: MouseEvent): boolean | undefined {
      // Right button
      if (e.button != 2) {
        return;
      }

      e.preventDefault();
      e.stopPropagation();

      _onItemSelected(e);
      self.onItemContextMenu.emit(e, { item, data: item.data });

      return false;
    }

    function _onItemSelected(e: MouseEvent): void {
      e.preventDefault();
      e.stopPropagation();

      if (item.collapseButton && e.target === item.collapseButton.element) {
        return;
      }

      // Upstream checks the title element here while renaming marks the content
      // span, so this guard never fires; kept for fidelity.
      const title = item.titleElement;
      if (renameState.has(title)) {
        return;
      }

      if (e.ctrlKey && self.options.allowMultiSelection) {
        // Check if selected
        if (self._isNodeSelected(item)) {
          self._unmarkAsSelected(item);
          return;
        }

        // Mark as selected
        self._markAsSelected(item, true, true);
      } else if (e.shiftKey && self.options.allowMultiSelection) {
        // select from current selection till here
        const lastItem = self.getSelectedItem();
        if (!lastItem) {
          return;
        }

        if (lastItem === item) {
          return;
        }

        // lastItem is in the selection, so its parent is this tree.
        const nodeList = self.children as TreeItem[];
        const lastIndex = nodeList.indexOf(lastItem);
        const currentIndex = nodeList.indexOf(item);

        const items = currentIndex > lastIndex
            ? nodeList.slice(lastIndex, currentIndex + 1)
            : nodeList.slice(currentIndex, lastIndex + 1);

        for (const item of items) {
          // mark as selected
          self._markAsSelected(item, true, true);
        }
      } else {
        self._skipScroll = true; // avoid scrolling while user clicks something

        // mark as selected
        self._markAsSelected(item, false, true);

        self._skipScroll = false;
      }
    }

    function _onItemDoubleClicked(e: MouseEvent): void {
      e.preventDefault();
      e.stopPropagation();

      const title = item.content;

      self.onItemDoubleClicked.emit(item.data);

      if (!renameState.has(title) && self.options.allowRename) {
        renameState.set(title, { oldName: title.text });

        const itemTitle = title;
        const itemName = title.text;

        itemTitle.removeAllChildren();

        const input = new TextInput(title, {
          value: itemName,
          style: "width: 100%;",
        });

        // Loose focus when renaming
        input.element.addEventListener("blur", function (e: FocusEvent) {
          const newName = (e.target as HTMLInputElement).value;
          // Use a timeout to avoid NotFoundError
          setTimeout(function () {
            itemTitle.removeAllChildren();
            itemTitle.text = newName;
          }, 1);
          const oldName = renameState.get(itemTitle)?.oldName;
          renameState.delete(itemTitle);
          self.onItemRenamed.emit(oldName, newName, item.data);
        });

        // Finishes renaming
        input.element.addEventListener("keydown", function (e: KeyboardEvent) {
          if (e.keyCode != 13) {
            return;
          }
          this.blur();
        });

        // set on focus
        input.element.focus();
        (input.element as HTMLInputElement).select();

        e.preventDefault();
      }
    }

    // Draggin an element on the tree.
    const draggableElement = item.titleElement;
    if (this.options.allowDrag) {
      draggableElement.draggable = true;

      // Start dragging this element
      draggableElement.addEventListener("dragstart", function (e: DragEvent) {
        const dt = e.dataTransfer;
        dt?.setData("item_id", item.itemId);
        if (data.onDragData) {
          const dragData = data.onDragData();
          if (dragData) {
            for (const i in dragData) {
              dt?.setData(i, dragData[i]);
            }
          }
        }
      });
    }

    draggableElement.addEventListener("dragenter", function (e: DragEvent) {
      e.preventDefault();
      if (data.skipDrag) {
        return false;
      }
      item.classList.add("dragover");
    });

    draggableElement.addEventListener("dragleave", function (e: DragEvent) {
      e.preventDefault();
      item.classList.remove("dragover");
    });

    draggableElement.addEventListener("dragover", function (e: DragEvent) {
      e.preventDefault();
    });

    // The listener's `this.parentNode.widget` in the original is the item's own
    // <li> widget, i.e. `item`.
    draggableElement.addEventListener("drop", function (de: DragEvent) {
      item.classList.remove("dragover");
      de.preventDefault();
      if (data.skipDrag) {
        return false;
      }

      const dragItemId = de.dataTransfer?.getData("item_id");
      if (!dragItemId) {
        self.onItemDropped.emit({ item, event: de });
        if (self.onDropItem) {
          const dragData = item.data;
          self.onDropItem(de, dragData);
        }
        return;
      }

      const dropItemId = item.itemId;
      if (!self.onMoveItem || (self.onMoveItem &&
          self.onMoveItem(self.getItem(dragItemId), self.getItem(dropItemId)) !== false)) {
        if (self.moveItem(dragItemId, dropItemId)) {
          self.onItemMoved.emit({
            item: self.getItem(dragItemId),
            parentItem: self.getItem(dropItemId),
          });
        }
      }

      if (self.onDropItem) {
        const dropData = item.data;
        self.onDropItem(de, dropData);
      }
    });

    return item;
  }

  // Remove from the tree the items that do not have a name that matches the string.
  filterByName(name?: string | null): void {
    for (let i = 0; i < this.children.length; ++i) {
      const childNode = this.children[i] as TreeItem;
      // `isTreeItem` is a static on TreeItem, so instances never carry it and
      // this loop body has never run upstream; kept for fidelity.
      if (!(childNode as Partial<Record<"isTreeItem", boolean>>).isTreeItem) {
        continue;
      }

      const content = childNode.content;
      if (!content) {
        continue;
      }

      const str = content.text.toLowerCase();

      if (!name || str.indexOf(name.toLowerCase()) != -1) {
        if (childNode.data && childNode.data.visible !== false) {
          childNode.classList.remove("filtered");
        }

        const indent = childNode.indent;
        if (indent) {
          if (name) {
            indent.style.paddingLeft = "0";
          } else {
            const level = childNode.level;
            indent.style.paddingLeft = `${(level + this.indentOffset) * TreeWidget.Indent}px`;
          }
        }
      } else {
        childNode.classList.add("filtered");
      }
    }
  }

  // Remove from the tree the items that do not have a name that matches the rule.
  filterByRule<P>(callbackToFilter: TreeFilterRule<P>, param?: P): void {
    for (let i = 0; i < this.children.length; ++i) {
      const childNode = this.children[i] as TreeItem;
      if (!childNode.classList || !childNode.classList.contains("tree-item")) {
        continue;
      }

      const content = childNode.content;
      if (!content) {
        continue;
      }

      if (callbackToFilter(childNode.data, content, param)) {
        if (childNode.data && childNode.data.visible !== false) {
          childNode.classList.remove("filtered");
        }

        const indent = childNode.indent;
        if (indent) {
          if (param) {
            indent.style.paddingLeft = "0";
          } else {
            const level = childNode.level;
            indent.style.paddingLeft = `${(level + this.indentOffset) * TreeWidget.Indent}px`;
          }
        }
      } else {
        childNode.classList.add("filtered");
      }
    }
  }

  getItem(id: TreeItemRef | null | undefined): TreeItem | null {
    if (!id) {
      return null;
    }

    if (typeof id !== "string") {
      return id;
    }

    for (const c of this.children as TreeItem[]) {
      if (!c.classList || !c.classList.contains("tree-item")) {
        continue;
      }

      if (c.itemId === id) {
        return c;
      }
    }

    return null;
  }

  /**
   * Expand the item to show its children.
   * @param parents Also expand every ancestor.
   */
  expandItem(id: TreeItemRef | null | undefined, parents?: boolean): void {
    const item = this.getItem(id);
    if (!item) {
      return;
    }

    if (!item.collapseButton) {
      return;
    }

    item.collapseButton.value = true;

    if (!parents) {
      return;
    }

    const parent = this.getParent(item);
    if (parent) {
      this.expandItem(parent, parents);
    }
  }

  /**
   * Collapse the item to hide its children.
   */
  collapseItem(id: TreeItemRef | null | undefined): void {
    const item = this.getItem(id);
    if (!item) {
      return;
    }

    if (!item.collapseButton) {
      return;
    }

    item.collapseButton.value = false;
  }

  /**
   * Checks if the item is out of the view due to scrolling.
   */
  isInsideArea(id: TreeItemRef): boolean {
    const item = typeof id === "string" ? this.getItem(id) : id;
    if (!item) {
      return false;
    }

    const rects = this.element.getClientRects();
    if (!rects.length) {
      return false;
    }

    const r = rects[0];
    const h = r.height;
    const y = item.top;

    return this.element.scrollTop < y && y < this.element.scrollTop + h;
  }

  /**
   * Scrolls to center the given item.
   */
  scrollToItem(id: TreeItemRef): boolean | undefined {
    const item = typeof id === "string" ? this.getItem(id) : id;
    if (!item) {
      return;
    }

    const rects = this.element.getClientRects();
    if (!rects.length) {
      return false;
    }

    const r = rects[0];
    const h = r.height;
    const x = (item.level + this.indentOffset) * TreeWidget.Indent + 50;

    this.element.scrollTop = (item.top - h * 0.5) | 0;
    if (r.width * 0.75 < x) {
      this.element.scrollLeft = x;
    } else {
      this.element.scrollLeft = 0;
    }
  }

  /**
   * Mark the item as selected
   */
  setSelectedItem(id: TreeItemRef | null | undefined, scroll?: boolean, sendEvent?: boolean): TreeItem | null | undefined {
    if (!id) {
      this.deselectAll();
      return;
    }

    const node = this.getItem(id);
    if (!node) {
      return null;
    }

    if (node.classList.contains("itemselected")) {
      return;
    }

    this._markAsSelected(node, true, false);

    if (scroll && !this._skipScroll) {
      this.scrollToItem(node);
    }

    this.expandItem(node, true);

    if (sendEvent) {
      node.onClick.emit();
    }

    return node;
  }

  /**
   * Adds an item to the selection for multiple selection
   */
  addItemToSelection(id: TreeItemRef | null | undefined): TreeItem | null | undefined {
    if (!id) {
      return;
    }

    const node = this.getItem(id);
    if (!node) {
      return null;
    }

    this._markAsSelected(node, true, true);

    return node;
  }

  /**
   * Remove an item from selection for multiple selection
   */
  removeItemFromSelection(id: TreeItemRef | null | undefined): null | undefined {
    if (!id) {
      return;
    }
    const node = this.getItem(id);
    if (!node) {
      return null;
    }
    node.classList.remove("itemselected");
  }

  /**
   * Returns the first selected item.
   */
  getSelectedItem(): TreeItem | undefined {
    if (!this.selection.length) {
      return;
    }
    return this.selection[this.selection.length - 1];
  }

  /**
   * Returns an array with the selected items.
   */
  getSelectedItems(): TreeItem[] {
    return this.selection;
  }

  /**
   * Returns true if an item is selected.
   */
  isItemSelected(id: TreeItemRef | null | undefined): boolean {
    const node = this.getItem(id);
    if (!node) {
      return false;
    }
    return this._isNodeSelected(node);
  }

  /**
   * Returns the children of an item.
   */
  getChildren(id: TreeItemRef, onlyDirect?: boolean): TreeItem[] | undefined {
    if (id && typeof id !== "string" && id.itemId !== undefined) {
      id = id.itemId;
    }
    return this._findChildElements(id, onlyDirect);
  }

  /**
   * Returns the parent of an item.
   */
  getParent(idOrNode: TreeItemRef | null | undefined): TreeItem | null {
    const element = this.getItem(idOrNode);
    if (element) {
      return this.getItem(element.parentId);
    }
    return null;
  }

  /**
   * Returns an array with all of the ancestors.
   */
  getAncestors(idOrNode: TreeItemRef | null | undefined, result?: TreeItem[]): TreeItem[] {
    result = result || [];
    const element = this.getItem(idOrNode);
    if (element) {
      result.push(element);
      return this.getAncestors(element.parentId, result);
    }
    return result;
  }

  /**
   * Returns true if the given node is an ancestor of the child.
   */
  isAncestor(child: TreeItemRef | null | undefined, node: TreeItemRef | null | undefined): boolean {
    const element = this.getItem(child);
    if (!element) {
      return false;
    }
    const dest = this.getItem(node);
    const parent = this.getItem(element.parentId);
    if (!parent) {
      return false;
    }
    if (parent === dest) {
      return true;
    }
    return this.isAncestor(parent, node);
  }

  /**
   * Move an item to a new parent.
   */
  moveItem(id: TreeItemRef, parentId: TreeItemRef): boolean {
    if (id === parentId) {
      return false;
    }

    const node = this.getItem(id);
    const parent = this.getItem(parentId);

    if (this.isAncestor(parent, node)) {
      return false;
    }

    if (!parent || !node) {
      return false;
    }

    let parentIndex = this._findElementIndex(parent);
    const parentLevel = parent.level;
    const oldParent = this.getParent(node);
    if (!oldParent) {
      //Log.error("node parent not found by id, maybe id has changed");
      return false;
    }

    const oldParentLevel = oldParent.level;
    const levelOffset = parentLevel - oldParentLevel;

    if (parent == oldParent) {
      return false;
    }

    // replace parent info
    node.parentId = parent.itemId;

    // get all children and subchildren and reinsert them in the new level
    const children = this.getChildren(node);
    if (children) {
      children.unshift(node); // add the node at the beginning

      // remove all children
      for (let i = 0; i < children.length; i++) {
        children[i].parent?.removeChild(children[i]);
      }

      // update levels
      for (let i = 0; i < children.length; i++) {
        const child = children[i];
        const newLevel = child.level + levelOffset;
        child.level = newLevel;
      }

      // reinsert
      parentIndex = this._findElementIndex(parent); // update parent index
      let lastIndex = this._findElementLastChildIndex(parentIndex);
      if (lastIndex == -1) {
        lastIndex = 0;
      }

      for (let i = 0; i < children.length; i++) {
        const child = children[i];
        this._insertInside(child, parentIndex, lastIndex + i - 1, child.level);
      }
    }

    // update collapse button
    this._updateCollapseButton(parent);
    if (oldParent) {
      this._updateCollapseButton(oldParent);
    }

    return true;
  }

  /**
   * Remove an item from the tree.
   */
  removeItem(idOrNode: TreeItemRef | null | undefined, removeChildren?: boolean): boolean {
    const node = this.getItem(idOrNode);
    if (!node) {
      return false;
    }

    // get parent
    const parent = this.getParent(node);

    // get all descendants
    let childNodes: TreeItem[] | undefined = undefined;
    if (removeChildren) {
      childNodes = this.getChildren(node);
    }

    // remove html element
    this.removeChild(node);

    // remove all children
    if (childNodes) {
      for (let i = 0; i < childNodes.length; i++) {
        this.removeChild(childNodes[i]);
      }
    }

    // update parent collapse button
    if (parent) {
      this._updateCollapseButton(parent);
    }

    return true;
  }

  /**
   * Remove an item's children from the tree.
   */
  removeItemChildren(idOrNode: TreeItemRef | null | undefined): boolean {
    const node = this.getItem(idOrNode);
    if (!node) {
      return false;
    }

    // get all descendants
    const childNodes = this.getChildren(node);

    // remove all children
    if (childNodes) {
      for (let i = 0; i < childNodes.length; i++) {
        this.removeChild(childNodes[i]);
      }
    }

    return true;
  }

  /**
   * Update the item with new data.
   */
  updateItem(id: TreeItemRef | null | undefined, data: TreeItemData): boolean {
    const node = this.getItem(id);
    if (!node) {
      return false;
    }

    node.data = data;
    if (data.id && node.id != data.id) {
      this.updateItemId(node.id, data.id);
    }

    if (data.content) {
      node.content.element.innerHTML = String(data.content);
    }

    return true;
  }

  /**
   * Update a given item id and the link with its children.
   */
  updateItemId(oldId: string, newId: string): boolean {
    const node = this.getItem(oldId);
    if (!node) {
      return false;
    }

    const children = this.getChildren(oldId, true) ?? [];
    node.id = newId;

    for (let i = 0; i < children.length; ++i) {
      const child = children[i];
      child.parentId = newId;
    }

    return true;
  }

  /**
   * Clears all of the items from the tree.
   */
  clear(keepRoot?: boolean): void {
    if (!keepRoot) {
      this.selection.length = 0;
      this.removeAllChildren();
      return;
    }

    this.selection.length = 0;
    for (const i of this.children as TreeItem[]) {
      i.removeAllChildren();
      if (i.classList.contains("itemselected")) {
        this.selection.push(i);
      }
    }
  }

  getNodeByIndex(index: number): TreeItem | undefined {
    return this.children[index] as TreeItem | undefined;
  }

  deselectAll(notify?: boolean): void {
    if (notify === undefined) {
      notify = true;
    }
    this.selection.length = 0;
    this._semiSelected.length = 0;
    this.classList.remove("itemselected");
    for (const i of this.children) {
      i.classList.remove("itemselected");
      i.classList.remove("semiselected");
    }
    if (notify) this.onDeselectAll.emit();
  }

  private _isNodeSelected(node: TreeItem): boolean {
    if (node.classList.contains("itemselected")) {
      return true;
    }
    return false;
  }

  private _markAsSelected(node: TreeItem, addToSelection: boolean, doCallback: boolean): void {
    // Already selected
    if (node.classList.contains("itemselected")) {
      if (addToSelection || this.selection.length == 1) {
        return;
      }
    }

    // Clear old selection
    if (!addToSelection) {
      this.deselectAll(false);
    }

    // Mark as selected (it was node.title_element?)
    if (!node.classList.contains("itemselected")) {
      node.classList.add("itemselected");
      this.selection.push(node);
    }

    if (node.classList.contains("semiselected")) {
      node.classList.remove("semiselected");
      const i = this._semiSelected.indexOf(node);
      if (i != -1) {
        this._semiSelected.splice(i, 1);
      }
    }

    // Go up and semiselect
    let parent = this.getParent(node);
    while (parent && !parent.classList.contains("semiselected")) {
      parent.classList.add("semiselected");
      this._semiSelected.push(parent);
      parent = this.getParent(parent);
    }

    if (doCallback) {
      this.onItemSelected.emit(node.data, addToSelection);

      let r: unknown = false;
      if (node.data.callback) {
        // Upstream passes the global `self` (window) as `this`; kept as-is.
        r = node.data.callback.call(self, node.data, addToSelection);
      }

      if (!r && this.itemSelected) {
        this.itemSelected(node.data, addToSelection);
      }
    }
  }

  private _unmarkAsSelected(node: TreeItem): void {
    if (!node.classList.contains("itemselected")) {
      return;
    }

    node.classList.remove("itemselected");

    const i = this.selection.indexOf(node);
    if (i != -1) {
      this.selection.splice(i, 1);
    }

    for (const c of this._semiSelected) {
      c.classList.remove("semiselected");
    }

    this._semiSelected.length = 0;
    for (const c of this.selection) {
      let parent = this.getParent(c);
      while (parent && !parent.classList.contains("semiselected")) {
        parent.classList.add("semiselected");
        this._semiSelected.push(parent);
        parent = this.getParent(parent);
      }
    }

    this.onItemDeselected.emit(node.data);
  }

  /**
   * Update the widget to collapse
   */
  private _updateCollapseButton(node: TreeItem | null, options?: { collapsed?: boolean }, currentLevel?: number): void {
    if (!node) {
      return;
    }

    const self = this;
    if (!node.collapseButton) {
      const container = node.collapseButtonArea;

      const collapseButton = new CollapseButton(true,
        function (e: string) {
          self._onClickExpand(e, node);
          node.data.collapsed = collapseButton.value == "closed" ? true : false;
          if (node.data.onCollapseChange) {
            node.data.onCollapseChange(node, node.data, collapseButton.value);
          }
        }, container);

      collapseButton.stopPropagation = true;
      collapseButton.setEmpty(true);
      node.collapseButton = collapseButton;
    }

    if ((options && options.collapsed) || (currentLevel !== undefined && currentLevel >= this.collapsedDepth)) {
      node.collapseButton.collapse();
    }

    const childElements = this.getChildren(node.itemId);
    if (!childElements) {
      return; //null
    }

    node.collapseButton.setEmpty(false);
  }

  private _onClickExpand(_e: string, node: TreeItem): void {
    const children = this.getChildren(node);

    if (!children) {
      return;
    }

    // Update children visibility
    for (const child of children) {
      const childParent = this.getParent(child);
      let visible = true;
      if (childParent) {
        visible = this._isNodeChildrenVisible(childParent);
      }
      if (visible) {
        child.classList.remove("hidden");
      } else {
        child.classList.add("hidden");
      }
    }
  }
}
