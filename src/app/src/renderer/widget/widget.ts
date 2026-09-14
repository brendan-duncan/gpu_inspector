// Ported from WebGPU Inspector (MIT License) - https://github.com/brendan-duncan/webgpu_inspector
import { Pointer } from './pointer.js';

// DOM back-pointer: the original JS assigned `element.widget = this` as an expando, and other
// widgets look it up the same way (e.g. `node.parentNode.widget` in tree_widget). To keep those
// lookups working without casts or a lookup helper, the expando is declared via a global
// augmentation of HTMLElement rather than moved into a WeakMap.
declare global {
  interface HTMLElement {
    /** The Widget wrapping this element, if any. Set by the Widget constructor. */
    widget?: Widget;
  }
}

/**
 * Options understood by Widget.configure(). Subclasses extend this interface with their own
 * options; the index signature lets extra options pass through the base constructor.
 */
export interface WidgetOptions {
  id?: string;
  class?: string | string[];
  text?: string;
  html?: string;
  style?: string;
  title?: string;
  backgroundColor?: string;
  color?: string;
  type?: string;
  children?: Widget[];
  disabled?: boolean;
  tabIndex?: number;
  zIndex?: number;
  draggable?: boolean;
  onClick?: (e: MouseEvent) => void;
  data?: unknown;
  tooltip?: string;
  href?: string;
  target?: string;
  /** Stretch factor used when the parent is a layout (`parent.constructor.isLayout`). */
  stretch?: number;
  [key: string]: unknown;
}

/**
 * A MouseEvent that Widget.updatePositionFromEvent() has annotated with the mouse position
 * relative to the widget.
 */
export interface WidgetMouseEvent extends MouseEvent {
  targetX?: number;
  targetY?: number;
}

/**
 * A wheel event as delivered to mouseWheelEvent(): the legacy 'mousewheel' fields that some
 * browsers provide, plus the normalized `wheel` and `delta` values that Widget computes.
 */
export interface WidgetWheelEvent extends WheelEvent {
  readonly wheelDelta?: number;
  readonly wheelDeltaY?: number;
  wheel: number;
  delta: number;
}

/** A parent whose constructor has `isLayout` set: children are added through `add()`. */
interface LayoutLike extends Widget {
  add(child: Widget, stretch: number): void;
}

/** Element properties that Widget forwards but which only exist on some element types. */
type FormLikeElement = HTMLInputElement;
type AnchorLikeElement = HTMLAnchorElement;

/**
 * A Widget is a wrapper for a DOM element.
 */
export class Widget<E extends HTMLElement = HTMLElement> {
  static window: Widget | null = null;
  static currentPointers: Pointer[] = [];
  static disablePaintingOnResize = false;
  static id = 0;
  /** Set to true on layout subclasses; such parents receive children through `add()`. */
  static isLayout = false;
  /** Set to true on the Window subclass. */
  static isWindow = false;
  static _idPrefix = 'WIDGET';

  id: string;
  _element: E;
  _parent: Widget | null = null;
  readonly children: Widget[] = [];
  /** Optional window reference searched by the `window` getter (never set by Widget itself). */
  _window: Widget | null = null;
  /** Arbitrary user data (`options.data`). Subclasses may narrow the type with `declare data: T`. */
  data: unknown = undefined;

  /** Subclasses may define paintEvent(); repaint() calls it when present. */
  paintEvent?(): void;

  hasFocus = false;
  mouseX = 0;
  mouseY = 0;
  mousePageX = 0;
  mousePageY = 0;
  lastMouseX = 0;
  lastMouseY = 0;
  startMouseX = 0;
  startMouseY = 0;
  startMouseEvent: MouseEvent | null = null;
  mouseButton = -1;
  _isMouseDown = false;

  _mouseDownEnabled = false;
  _mouseMoveEnabled = false;
  _mouseUpEnabled = false;
  _contextMenuEnabled = false;
  _clickEnabled = false;
  _doubleClickEnabled = false;
  _mouseWheelEnabled = false;
  _mouseOverEnabled = false;
  _mouseOutEnabled = false;
  _touchEventsEnabled = false;
  _pointerEventsEnabled = false;
  _pointerEventsBoundToWindow = false;
  _keyPressEnabled = false;
  _keyReleaseEnabled = false;

  _boundMouseDown: ((e: MouseEvent) => void) | null = null;
  _boundMouseMove: ((e: MouseEvent) => void) | null = null;
  _boundMouseUp: ((e: MouseEvent) => void) | null = null;
  _boundContextMenu: ((e: MouseEvent) => void) | null = null;
  _boundClick: ((e: MouseEvent) => void) | null = null;
  _boundDoubleClick: ((e: MouseEvent) => void) | null = null;
  _boundMouseWheel: ((e: WheelEvent) => void) | null = null;
  _boundMouseOver: ((e: MouseEvent) => void) | null = null;
  _boundMouseOut: ((e: MouseEvent) => void) | null = null;
  _boundTouchStart: ((e: TouchEvent) => void) | null = null;
  _boundTouchEnd: ((e: TouchEvent) => void) | null = null;
  _boundTouchCancel: ((e: TouchEvent) => void) | null = null;
  _boundTouchMove: ((e: TouchEvent) => void) | null = null;
  _boundPointerDown: ((e: PointerEvent) => void) | null = null;
  _boundPointerMove: ((e: PointerEvent) => void) | null = null;
  _boundPointerUp: ((e: PointerEvent) => void) | null = null;
  _boundDrag: ((e: DragEvent) => void) | null = null;
  _boundDragStart: ((e: DragEvent) => void) | null = null;
  _boundDragEnd: ((e: DragEvent) => void) | null = null;
  _boundKeyPress: ((e: KeyboardEvent) => void) | null = null;
  _boundKeyRelease: ((e: KeyboardEvent) => void) | null = null;
  _onDragEvent: ((e: DragEvent) => void) | null = null;
  _onDropEvent: ((e: DragEvent) => void) | null = null;

  /**
   * @param element A tag name to create, or an existing element to wrap.
   * @param parent The parent widget (or an options object, in which case there is no parent).
   * @param options Configuration options, see WidgetOptions.
   */
  constructor(
    element: string | E,
    parent?: Widget | HTMLElement | WidgetOptions | null,
    options?: WidgetOptions
  ) {
    this.id = `widget_${Widget.id++}`;
    if (typeof element === 'string') {
      element = document.createElement(element) as E;
    }

    this._element = element;
    if (element) {
      this._element.id = this.id;
      this._element.title = '';
    }

    if (parent && parent.constructor === Object) {
      options = parent as WidgetOptions;
      parent = null;
    }

    if (parent) {
      if (parent instanceof HTMLElement) {
        parent.appendChild(this._element);
      } else if ((parent.constructor as typeof Widget).isLayout) {
        const stretch = options && options.stretch ? options.stretch : 0;
        (parent as LayoutLike).add(this, stretch);
      } else {
        this.parent = parent as Widget;
      }
    }

    if (options) {
      this.configure(options);
    }

    if (this._element) {
      this._element.widget = this;
    }
  }

  configure(options: WidgetOptions): void {
    if (options.id) {
      this._element.id = options.id;
    }

    if (options.class) {
      if (typeof options.class === 'string') {
        const classes = options.class.split(' ').filter((c) => c.trim());
        this.classList.add(...classes);
      } else {
        this.classList.add(...options.class);
      }
    }

    if (options.text !== undefined) {
      this.text = options.text;
    }

    if (options.html !== undefined) {
      this.html = options.html;
    }

    if (options.style !== undefined) {
      this._element.style = options.style;
    }

    if (options.title !== undefined) {
      this._element.title = options.title;
    }

    if (options.backgroundColor !== undefined) {
      this._element.style.backgroundColor = options.backgroundColor;
    }

    if (options.color !== undefined) {
      this._element.style.color = options.color;
    }

    if (options.type !== undefined) {
      (this._element as HTMLElement as FormLikeElement).type = options.type;
    }

    if (options.children !== undefined) {
      for (const c of options.children) {
        this.appendChild(c);
      }
    }

    if (options.disabled !== undefined) {
      (this._element as HTMLElement as FormLikeElement).disabled = options.disabled;
    }

    if (options.tabIndex !== undefined) {
      this._element.tabIndex = options.tabIndex;
    }

    if (options.zIndex !== undefined) {
      this._element.style.zIndex = String(options.zIndex);
    }

    if (options.draggable !== undefined) {
      this.draggable = options.draggable;
    }

    if (options.onClick !== undefined) {
      this.addEventListener('click', options.onClick);
    }

    if (options.data !== undefined) {
      this.data = options.data;
    }

    if (options.tooltip !== undefined) {
      this.tooltip = options.tooltip;
    }

    if (options.href !== undefined) {
      (this._element as HTMLElement as AnchorLikeElement).href = options.href;
    }

    if (options.target !== undefined) {
      (this._element as HTMLElement as AnchorLikeElement).target = options.target;
    }
  }

  /**
   * The HTML DOM element.
   */
  get element(): E {
    return this._element;
  }

  /**
   * The parent widget of this widget.
   */
  get parent(): Widget | null {
    return this._parent;
  }

  /**
   * Set the parent widget of this widget. If the widget already has a parent, it will be removed from the current parent before being added to the new parent.
   * @param p The new parent widget. If null, the widget will be removed from its current parent.
   */
  set parent(p: Widget | null) {
    if (!p) {
      if (this._parent) {
        this._parent.removeChild(this);
        return;
      }
    } else {
      p.appendChild(this);
    }

    this.onResize();
  }

  /**
   * The last child widget of this widget, or undefined if there are no children.
   */
  get lastChild(): Widget | undefined {
    return this.children[this.children.length - 1];
  }

  /**
   * Insert a child widget before the given child widget.
   * @param newChild The new child widget to insert.
   * @param refChild The reference child widget before which the new child will be inserted. If refChild is not a child of this widget, newChild will be appended to the end of the children list.
   */
  insertBefore(newChild: Widget, refChild: Widget): void {
    const index = this.children.indexOf(refChild);
    if (index === -1) {
      this.appendChild(newChild);
      return;
    }
    this.children.splice(index, 0, newChild);
    this._element.insertBefore(newChild._element, refChild._element);
    newChild._parent = this;
  }

  /**
   * Insert a child widget after the given child widget.
   * @param newChild The new child widget to insert.
   * @param refChild The reference child widget after which the new child will be inserted. If refChild is not a child of this widget, newChild will be appended to the end of the children list.
   */
  insertAfter(newChild: Widget, refChild: Widget): void {
    let index = this.children.indexOf(refChild);
    if (index === -1) {
      this.appendChild(newChild);
      return;
    }
    index++;
    if (index >= this.children.length) {
      this.appendChild(newChild);
      return;
    }
    const refWidget = this.children[index];
    this.children.splice(index, 0, newChild);
    this._element.insertBefore(newChild._element, refWidget._element);
    newChild._parent = this;
  }

  /**
   * Add a child widget to this widget.
   * @param child The child widget to add.
   */
  appendChild(child: Widget): void {
    if (child.parent === this) {
      return;
    }

    // Remove the widget from its current parent.
    if (child.parent) {
      child.parent.removeChild(child);
    }

    // Add the widget to the children list.
    child._parent = this;
    this.children.push(child);
    this._element.appendChild(child._element);

    const w = this.window;
    if (w) {
      child._addedToWindow(w);
    }

    child.onResize();
  }

  remove(): void {
    if (this._parent) {
      this._parent.removeChild(this);
    }
    this._removeEventListeners();
    this.element.remove();
  }

  _removeEventListeners(): void {
    if (this._element) {
      if (this._boundMouseDown) {
        this._element.removeEventListener('mousedown', this._boundMouseDown);
      }
      if (this._boundMouseMove) {
        this._element.removeEventListener('mousemove', this._boundMouseMove);
      }
      if (this._boundMouseUp) {
        this._element.removeEventListener('mouseup', this._boundMouseUp);
      }
      if (this._boundContextMenu) {
        this._element.removeEventListener('contextmenu', this._boundContextMenu);
      }
      if (this._boundClick) {
        this._element.removeEventListener('click', this._boundClick);
      }
      if (this._boundDoubleClick) {
        this._element.removeEventListener('dblclick', this._boundDoubleClick);
      }
      if (this._boundMouseWheel) {
        this._element.removeEventListener('mousewheel', this._boundMouseWheel as EventListener);
      }
      if (this._boundMouseOver) {
        this._element.removeEventListener('mouseover', this._boundMouseOver);
      }
      if (this._boundMouseOut) {
        this._element.removeEventListener('mouseout', this._boundMouseOut);
      }
      if (this._boundTouchStart) {
        this._element.removeEventListener('touchstart', this._boundTouchStart);
      }
      if (this._boundTouchEnd) {
        this._element.removeEventListener('touchend', this._boundTouchEnd);
      }
      if (this._boundTouchCancel) {
        this._element.removeEventListener('touchcancel', this._boundTouchCancel);
      }
      if (this._boundTouchMove) {
        this._element.removeEventListener('touchmove', this._boundTouchMove);
      }
      if (this._boundPointerDown) {
        this._element.removeEventListener('pointerdown', this._boundPointerDown);
      }
      if (this._boundPointerMove) {
        if (this._pointerEventsBoundToWindow) {
          window.removeEventListener('pointermove', this._boundPointerMove);
        } else {
          this._element.removeEventListener('pointermove', this._boundPointerMove);
        }
      }
      if (this._boundPointerUp) {
        if (this._pointerEventsBoundToWindow) {
          window.removeEventListener('pointerup', this._boundPointerUp);
        } else {
          this._element.removeEventListener('pointerup', this._boundPointerUp);
        }
      }
      if (this._boundDrag) {
        this._element.removeEventListener('drag', this._boundDrag);
      }
      if (this._boundDragStart) {
        this._element.removeEventListener('dragstart', this._boundDragStart);
      }
      if (this._boundDragEnd) {
        this._element.removeEventListener('dragend', this._boundDragEnd);
      }
    }
    if (this._boundKeyPress) {
      document.removeEventListener('keydown', this._boundKeyPress);
    }
    if (this._boundKeyRelease) {
      document.removeEventListener('keyup', this._boundKeyRelease);
    }
  }

  /**
   * Remove a child widget.
   * @param child The child widget to remove.
   */
  removeChild(child: Widget): void {
    const index = this.children.indexOf(child);
    if (index !== -1) {
      this.children.splice(index, 1);
    }
    child._parent = null;
    this._element.removeChild(child._element);
  }

  /**
   * Remove all children from this widget.
   */
  removeAllChildren(): void {
    for (const child of this.children) {
      child._parent = null;
    }
    this.children.length = 0;
    while (this._element.firstChild) {
      const last = this._element.lastChild;
      if (!last) {
        break;
      }
      this._element.removeChild(last);
    }
  }

  /**
   * Get the position of the element on the page.
   * @return [x, y]
   */
  getPagePosition(): [number, number] {
    let lx = 0;
    let ly = 0;
    for (let el: HTMLElement | null = this._element; el != null; el = el.offsetParent as HTMLElement | null) {
      lx += el.offsetLeft;
      ly += el.offsetTop;
    }
    return [lx, ly];
  }

  /**
   * Parse out the value from a CSS string
   */
  static getCssValue(cssValue: string | null | undefined): number {
    if (!cssValue) {
      cssValue = '0px';
    }
    if (cssValue.endsWith('%')) {
      cssValue = cssValue.substring(0, cssValue.length - 1);
    } else {
      cssValue = cssValue.substring(0, cssValue.length - 2);
    }
    if (cssValue.includes('.')) {
      return parseFloat(cssValue);
    }
    return parseInt(cssValue);
  }

  /**
   * Return the size of a CSS property, like "padding", "Left", "Right"
   */
  static getStyleSize(style: CSSStyleDeclaration, property: string, d1: string, d2: string): number {
    const values = style as unknown as Record<string, string | undefined>;
    const s1 = Widget.getCssValue(values[`${property}${d1}`]);
    const s2 = Widget.getCssValue(values[`${property}${d2}`]);
    return s1 + s2;
  }

  /**
   * The width of the widget.
   */
  get width(): number {
    return this._element.offsetWidth;
  }

  /**
   * The height of the widget.
   */
  get height(): number {
    return this._element.offsetHeight;
  }

  /**
   * Get the bounding rect of the widget.
   */
  getBoundingClientRect(): DOMRect {
    return this._element.getBoundingClientRect();
  }

  /**
   * Is the element visible?
   */
  get visible(): boolean {
    let e: Widget | null = this;
    while (e) {
      if (e._element.style.display === 'none') {
        return false;
      }
      e = e.parent;
    }
    return true;
  }

  /**
   * Called when the DOM of the widget has changed.
   * This can be used to trigger any updates that need to happen when the DOM changes,
   * like updating the size of the widget.
   * This method can be overridden by subclasses to implement custom behavior when the DOM changes.
   */
  onDomChanged(): void {}

  /**
   * Called when the DOM of the widget or any of its children has changed.
   * This can be used to trigger any updates that need to happen when the DOM changes,
   * like updating the size of the widget.
   */
  domChanged(): void {
    this.onDomChanged();
    for (const c of this.children) {
      c.domChanged();
    }
  }

  /**
   * The x position of the element.
   */
  get left(): number {
    return this._element ? this._element.offsetLeft : 0;
  }

  /**
   * The y position of the element.
   */
  get top(): number {
    return this._element ? this._element.offsetTop : 0;
  }

  /**
   * Set the position of the element.
   * @param x The x position of the element.
   * @param y The y position of the element.
   * @param type The CSS position type of the element. Defaults to "absolute".
   */
  setPosition(x: number, y: number, type?: string): void {
    type = type || 'absolute';
    this._element.style.position = type;
    this._element.style.left = `${x}px`;
    this._element.style.top = `${y}px`;
  }

  /**
   * Resize the element.
   * @param w The new width of the element.
   * @param h The new height of the element.
   */
  resize(w: number, h: number): void {
    // style.width/height is only for the inner contents of the widget,
    // not the full size of the widget including border and padding.
    // Since the resize function wants to encompass the entire widget,
    // we need to subtract the border and padding sizes from the size set
    // to the style.
    const rect = this.getBoundingClientRect();
    const dx = this._element.offsetWidth - rect.width;
    const dy = this._element.offsetHeight - rect.height;
    this._element.style.width = `${w - dx}px`;
    this._element.style.height = `${h - dy}px`;
  }

  onResize(): void {
    for (const c of this.children) {
      c.onResize();
    }
  }

  /**
   * The CSS style of the element. Assigning a string sets the element's inline style text.
   */
  get style(): CSSStyleDeclaration {
    return this._element.style;
  }

  set style(v: string) {
    this._element.style = v;
  }

  /**
   * The CSS class set of the element.
   */
  get classList(): DOMTokenList {
    return this._element.classList;
  }

  /**
   * The inner text of the element.
   */
  get text(): string {
    return this._element.innerText;
  }

  set text(s: string) {
    this._dropChildren();
    this._element.innerText = s;
  }

  get textContent(): string | null {
    return this._element.textContent;
  }

  set textContent(s: string | null) {
    this._element.textContent = s;
  }

  get html(): string {
    return this._element.innerHTML;
  }

  set html(v: string) {
    this._dropChildren();
    this._element.innerHTML = v;
  }

  /**
   * Replacing the element's content (text or html) discards the child elements, so the child
   * widgets are forgotten as well; otherwise later insertBefore()/insertAfter() calls would refer
   * to elements that are no longer in the DOM.
   */
  private _dropChildren(): void {
    for (const child of this.children) {
      child._parent = null;
    }
    this.children.length = 0;
  }

  get title(): string {
    return this._element.title;
  }

  set title(v: string) {
    this._element.title = v;
  }

  get tooltip(): string {
    return this._element.title;
  }

  set tooltip(v: string) {
    this._element.title = v;
  }

  get disabled(): boolean {
    return (this._element as HTMLElement as FormLikeElement).disabled;
  }

  set disabled(v: boolean) {
    (this._element as HTMLElement as FormLikeElement).disabled = v;
  }

  get dataset(): DOMStringMap {
    return this._element.dataset;
  }

  get tabIndex(): number {
    return this._element.tabIndex;
  }

  set tabIndex(v: number) {
    this._element.tabIndex = v;
  }

  get zIndex(): number {
    return parseInt(this._element.style.zIndex) || 0;
  }

  set zIndex(v: number) {
    this._element.style.zIndex = String(v);
  }

  get draggable(): boolean {
    return this._element.draggable;
  }

  set draggable(v: boolean) {
    this._element.draggable = v;
    if (v) {
      this._boundDrag = this.dragEvent.bind(this);
      this._boundDragStart = this.dragStartEvent.bind(this);
      this._boundDragEnd = this.dragEndEvent.bind(this);
      this.addEventListener('drag', this._boundDrag);
      this.addEventListener('dragstart', this._boundDragStart);
      this.addEventListener('dragend', this._boundDragEnd);
    } else {
      if (this._boundDrag) {
        this.removeEventListener('drag', this._boundDrag);
        if (this._boundDragStart) {
          this.removeEventListener('dragstart', this._boundDragStart);
        }
        if (this._boundDragEnd) {
          this.removeEventListener('dragend', this._boundDragEnd);
        }
      }
    }
  }

  querySelector<T extends Element = Element>(selectors: string): T | null {
    return this._element.querySelector<T>(selectors);
  }

  addEventListener<K extends keyof HTMLElementEventMap>(
    type: K,
    listener: (this: HTMLElement, ev: HTMLElementEventMap[K]) => unknown,
    options?: boolean | AddEventListenerOptions
  ): void;
  addEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | AddEventListenerOptions
  ): void;
  addEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | AddEventListenerOptions
  ): void {
    return this._element.addEventListener(type, listener, options);
  }

  removeEventListener<K extends keyof HTMLElementEventMap>(
    type: K,
    listener: (this: HTMLElement, ev: HTMLElementEventMap[K]) => unknown,
    options?: boolean | EventListenerOptions
  ): void;
  removeEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | EventListenerOptions
  ): void;
  removeEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | EventListenerOptions
  ): void {
    return this._element.removeEventListener(type, listener, options);
  }

  dispatchEvent(event: Event): boolean {
    return this._element.dispatchEvent(event);
  }

  /**
   * Repaint the widget.
   * @param allDecendents Also repaint all descendant widgets.
   */
  repaint(allDecendents = true): void {
    if (this.paintEvent) this.paintEvent();
    if (allDecendents) {
      for (const c of this.children) {
        c.repaint(allDecendents);
      }
    }
  }

  startResize(): void {}

  _startResize(): void {
    this.startResize();
    for (const c of this.children) {
      c._startResize();
    }
  }

  onAddedToWindow(w: Widget): void {}

  _addedToWindow(w: Widget): void {
    this.onAddedToWindow(w);
    for (const c of this.children) {
      c._addedToWindow(w);
    }
  }

  get window(): Widget | null {
    let w = Widget.window;
    if (!w) {
      let p = this._parent;
      while (p) {
        if (p._window) {
          w = p._window;
          break;
        }
        p = p._parent;
      }
    }
    return w;
  }

  /**
   * Start listening for mousePressEvent, mouseMoveEvent, and mouseReleaseEvent.
   */
  enableMouseEvents(): void {
    if (!this._mouseDownEnabled && this._element) {
      this._mouseDownEnabled = true;
      this._boundMouseDown = this._onMouseDown.bind(this);
      this._element.addEventListener('mousedown', this._boundMouseDown);
    }
    if (!this._mouseMoveEnabled && this._element) {
      this._mouseMoveEnabled = true;
      this._boundMouseMove = this._onMouseMove.bind(this);
      this._element.addEventListener('mousemove', this._boundMouseMove);
    }
    if (!this._mouseUpEnabled && this._element) {
      this._mouseUpEnabled = true;
      this._boundMouseUp = this._onMouseUp.bind(this);
      this._element.addEventListener('mouseup', this._boundMouseUp);
    }
  }

  /**
   * Start listening for mouseMoveEvent.
   */
  enableMouseMoveEvent(): void {
    if (!this._mouseMoveEnabled && this._element) {
      this._mouseMoveEnabled = true;
      this._boundMouseMove = this._onMouseMove.bind(this);
      this._element.addEventListener('mousemove', this._boundMouseMove);
    }
  }

  /**
   * Start listening for ContextMenu events.
   */
  enableContextMenuEvent(): void {
    this.enableMouseMoveEvent();
    if (!this._contextMenuEnabled && this._element) {
      this._contextMenuEnabled = true;
      this._boundContextMenu = this._onContextMenu.bind(this);
      this._element.addEventListener('contextmenu', this._boundContextMenu);
    }
  }

  /**
   * Start listenening for Click events.
   */
  enableClickEvent(): void {
    if (!this._clickEnabled && this._element) {
      this._clickEnabled = true;
      this._boundClick = this._onClick.bind(this);
      this._element.addEventListener('click', this._boundClick);
    }
  }

  /**
   * Start listening for DoubleClick events.
   */
  enableDoubleClickEvent(): void {
    if (!this._doubleClickEnabled && this._element) {
      this._doubleClickEnabled = true;
      this._boundDoubleClick = this._onDoubleClick.bind(this);
      this._element.addEventListener('dblclick', this._boundDoubleClick);
    }
  }

  /**
   * Start listening for MouseWheel events.
   */
  enableMouseWheelEvent(): void {
    if (!this._mouseWheelEnabled && this._element) {
      this._mouseWheelEnabled = true;
      this._boundMouseWheel = this._onMouseWheel.bind(this);
      this._element.addEventListener('mousewheel', this._boundMouseWheel as EventListener);
    }
  }

  /**
   * Start listening for when the mouse enters the widget.
   */
  enableEnterEvent(): void {
    this.enableMouseMoveEvent();
    if (!this._mouseOverEnabled && this._element) {
      this._mouseOverEnabled = true;
      this._boundMouseOver = this._onMouseOver.bind(this);
      this._element.addEventListener('mouseover', this._boundMouseOver);
    }
  }

  /**
   * Start listening for when the mouse leaves the widget.
   */
  enableLeaveEvent(): void {
    this.enableMouseMoveEvent();
    if (!this._mouseOutEnabled && this._element) {
      this._mouseOutEnabled = true;
      this._boundMouseOut = this._onMouseOut.bind(this);
      this._element.addEventListener('mouseout', this._boundMouseOut);
    }
  }

  /**
   * Enable listening for touch events.
   */
  enableTouchEvents(): void {
    if (!this._touchEventsEnabled) {
      this._touchEventsEnabled = true;
      this._boundTouchStart = this._onTouchStart.bind(this);
      this._boundTouchEnd = this._onTouchEnd.bind(this);
      this._boundTouchCancel = this._onTouchCancel.bind(this);
      this._boundTouchMove = this._onTouchMove.bind(this);
      this._element.addEventListener('touchstart', this._boundTouchStart);
      this._element.addEventListener('touchend', this._boundTouchEnd);
      this._element.addEventListener('touchcancel', this._boundTouchCancel);
      this._element.addEventListener('touchmove', this._boundTouchMove);
      this.style.touchAction = 'none';
    }
  }

  enablePointerEvents(bindToWindow?: boolean): void {
    if (!this._pointerEventsEnabled) {
      this._pointerEventsEnabled = true;
      this._pointerEventsBoundToWindow = !!bindToWindow;
      this._boundPointerDown = this._onPointerDown.bind(this);
      this._boundPointerMove = this._onPointerMove.bind(this);
      this._boundPointerUp = this._onPointerUp.bind(this);
      this._element.addEventListener('pointerdown', this._boundPointerDown);
      if (bindToWindow) {
        window.addEventListener('pointermove', this._boundPointerMove);
        window.addEventListener('pointerup', this._boundPointerUp);
      } else {
        this._element.addEventListener('pointermove', this._boundPointerMove);
        this._element.addEventListener('pointerup', this._boundPointerUp);
      }
      this.style.touchAction = 'none';
    }
  }

  _onPointerDown(e: PointerEvent): void {
    this.hasFocus = true;
    const pointer = new Pointer(e);
    if (Widget.currentPointers.some((p) => p.id === pointer.id)) {
      return;
    }
    Widget.currentPointers.push(pointer);
    //this.element.setPointerCapture(e.pointerId);
    const res = this.pointerDownEvent(e, Widget.currentPointers, pointer);
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  releasePointers(): void {
    //for (let p of Widget.currentPointers)
    //this.element.releasePointerCapture(p.id);
    Widget.currentPointers.length = 0;
  }

  _onPointerMove(e: PointerEvent): void {
    const pointer = new Pointer(e);

    const index = Widget.currentPointers.findIndex((p) => p.id === pointer.id);
    if (index !== -1) {
      Widget.currentPointers[index] = pointer;
    }

    this.hasFocus = true;
    const res = this.pointerMoveEvent(e, Widget.currentPointers, pointer);
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  _onPointerUp(e: PointerEvent): void {
    const pointer = new Pointer(e);
    //if (Widget.currentPointers.some((p) => p.id === pointer.id))
    //this.element.releasePointerCapture(e.pointerId);
    const index = Widget.currentPointers.findIndex((p) => p.id === pointer.id);
    if (index !== -1) {
      Widget.currentPointers.splice(index, 1);
    }

    this.hasFocus = true;
    const res = this.pointerUpEvent(e, Widget.currentPointers, pointer);
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Event called when a pointer is pressed on the widget. Return false to stop propagation
   * and prevent the default action.
   */
  pointerDownEvent(e: PointerEvent, pointers: Pointer[], pointer: Pointer): boolean {
    return true;
  }

  pointerMoveEvent(e: PointerEvent, pointers: Pointer[], pointer: Pointer): boolean {
    return true;
  }

  pointerUpEvent(e: PointerEvent, pointers: Pointer[], pointer: Pointer): boolean {
    return true;
  }

  /**
   * Start listening for KeyPress events.
   */
  enableKeyPressEvent(): void {
    this.enableEnterEvent();
    this.enableLeaveEvent();
    this.enableMouseMoveEvent();
    if (!this._keyPressEnabled) {
      this._keyPressEnabled = true;
      this._boundKeyPress = this._onKeyPress.bind(this);
      document.addEventListener('keydown', this._boundKeyPress);
    }
  }

  /**
   * Start listening for KeyRelease events.
   */
  enableKeyReleaseEvent(): void {
    this.enableEnterEvent();
    this.enableLeaveEvent();
    this.enableMouseMoveEvent();
    if (!this._keyReleaseEnabled) {
      this._keyReleaseEnabled = true;
      this._boundKeyRelease = this._onKeyRelease.bind(this);
      document.addEventListener('keyup', this._boundKeyRelease);
    }
  }

  /**
   * Event called when the widget is to be drawn
   */
  //paintEvent() { }

  /**
   * Event called when a mouse button is pressed on the wdiget.
   */
  mousePressEvent(e: MouseEvent): boolean {
    return false;
  }

  /**
   * Event called when the mouse is moved over the widget.
   */
  mouseMoveEvent(e: MouseEvent): boolean {
    //this.updatePositionFromEvent(e);
    return false;
  }

  /**
   * Event called when a mouse button is released over the widget.
   */
  mouseReleaseEvent(e: MouseEvent): boolean {
    return false;
  }

  /**
   * Event called when the widget receives a ContextMenu event, usually from
   * the right mouse button.
   */
  contextMenuEvent(e: MouseEvent): boolean {
    return true;
  }

  /**
   * Event called when a mouse button is clicked.
   */
  clickEvent(e: MouseEvent): boolean {
    return true;
  }

  /**
   * Event called when a mouse button is double clicked.
   */
  doubleClickEvent(e: MouseEvent): boolean {
    return true;
  }

  /**
   * Event called when a mouse wheel is scrolled.
   */
  mouseWheelEvent(e: WidgetWheelEvent): boolean {
    return true;
  }

  /**
   * Event called when the mouse enters the widget.
   */
  enterEvent(e: MouseEvent): boolean {
    return true;
  }

  /**
   * Event called when the mouse leaves the widget.
   */
  leaveEvent(e: MouseEvent): boolean {
    return true;
  }

  /**
   * Event called when a key is pressed on the widget.
   */
  keyPressEvent(e: KeyboardEvent): boolean {
    return true;
  }

  /**
   * Event called when a key is released on the widget.
   */
  keyReleaseEvent(e: KeyboardEvent): boolean {
    return true;
  }

  /**
   * Event called when a touch has started. Returning a falsy value stops propagation and
   * prevents the default action.
   */
  touchStartEvent(e: TouchEvent): boolean | undefined {
    return undefined;
  }

  /**
   * Event called when a touch has ended.
   */
  touchEndEvent(e: TouchEvent): boolean | undefined {
    return undefined;
  }

  /**
   * Event called when a touch has been canceled.
   */
  touchCancelEvent(e: TouchEvent): boolean | undefined {
    return undefined;
  }

  /**
   * Event called when a touch has moved.
   */
  touchMoveEvent(e: TouchEvent): boolean | undefined {
    return undefined;
  }

  /**
   * Event called when the element starts dragging.
   */
  dragStartEvent(e: DragEvent): void {}

  /**
   * Event called when the element ends dragging.
   */
  dragEndEvent(e: DragEvent): void {}

  /**
   * Event called when the element is dragging.
   */
  dragEvent(e: DragEvent): void {}

  /**
   * Called to update the current tracked mouse position on the widget.
   */
  updatePositionFromEvent(e: WidgetMouseEvent): void {
    if (!this._element) {
      return;
    }

    if (this.startMouseEvent) {
      e.targetX = Math.max(
        0,
        Math.min(
          this.element.clientWidth,
          this.startMouseX + e.pageX - this.startMouseEvent.pageX
        )
      );

      e.targetY = Math.max(
        0,
        Math.min(
          this.element.clientHeight,
          this.startMouseY + e.pageY - this.startMouseEvent.pageY
        )
      );
    } else {
      e.targetX = e.offsetX;
      e.targetY = e.offsetY;
    }

    this.mouseX = e.offsetX;
    this.mouseY = e.offsetY;
    this.mousePageX = e.clientX;
    this.mousePageY = e.clientY;

    if (e.movementX === undefined) {
      // Older browsers without movementX/Y: synthesize them (the DOM typings mark these readonly).
      const movable = e as { movementX: number; movementY: number };
      movable.movementX = e.clientX - this.lastMouseX;
      movable.movementY = e.clientY - this.lastMouseY;
    }

    this.lastMouseX = e.clientX;
    this.lastMouseY = e.clientY;
  }

  /**
   * Event called when the mouse is pressed on the widget.
   */
  _onMouseDown(e: MouseEvent): boolean {
    this.startMouseEvent = e;
    this.startMouseX = e.offsetX;
    this.startMouseY = e.offsetY;
    this.lastMouseX = e.clientX;
    this.lastMouseY = e.clientY;
    //this.updatePositionFromEvent(e);
    this._isMouseDown = true;
    this.mouseButton = e.button;
    const res = this.mousePressEvent(e);
    // If true is returned, prevent the event from propagating up and capture the mouse.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
      //this.beginMouseCapture();
    }
    return res;
  }

  /**
   * Event called when the mouse moves on the widget.
   */
  _onMouseMove(e: MouseEvent): boolean {
    //this.updatePositionFromEvent(e);
    return this.mouseMoveEvent(e);
  }

  /**
   * Event called when the mosue is released on the widget.
   */
  _onMouseUp(e: MouseEvent): boolean {
    //this.updatePositionFromEvent(e);
    this.startMouseEvent = null;
    if (!this._isMouseDown) {
      return true;
    }

    this._isMouseDown = false;
    const res = this.mouseReleaseEvent(e);

    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }

    //this.endMouseCapture();
    return res;
  }

  /**
   * Called for a ContextMenu event.
   */
  _onContextMenu(e: MouseEvent): boolean {
    const res = this.contextMenuEvent(e);
    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
    return false;
  }

  /**
   * Called fora  Click event.
   */
  _onClick(e: MouseEvent): void {
    const res = this.clickEvent(e);
    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a DoubleClick event.
   */
  _onDoubleClick(e: MouseEvent): void {
    const res = this.doubleClickEvent(e);
    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for mouseWheel event.
   */
  _onMouseWheel(event: WheelEvent): void {
    const e = event as WidgetWheelEvent;
    if (e.type === 'wheel') {
      e.wheel = -e.deltaY;
    } else {
      // in firefox deltaY is 1 while in Chrome is 120
      e.wheel = e.wheelDeltaY != null ? e.wheelDeltaY : e.detail * -60;
    }

    // from stack overflow
    // firefox doesnt have wheelDelta
    e.delta =
      e.wheelDelta !== undefined
        ? e.wheelDelta / 40
        : e.deltaY
        ? -e.deltaY / 3
        : 0;

    const res = this.mouseWheelEvent(e);

    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a MouseOver event.
   */
  _onMouseOver(e: MouseEvent): void {
    this.hasFocus = true;
    const res = this.enterEvent(e);
    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a MouseOut event.
   */
  _onMouseOut(e: MouseEvent): void {
    this.hasFocus = false;
    const res = this.leaveEvent(e);
    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a KeyPress event.
   */
  _onKeyPress(e: KeyboardEvent): void {
    if (!this.hasFocus) {
      return;
    }
    const res = this.keyPressEvent(e);
    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a KeyRelease event.
   */
  _onKeyRelease(e: KeyboardEvent): void {
    if (!this.hasFocus) {
      return;
    }
    const res = this.keyReleaseEvent(e);
    // if false is returned, prevent the event from propagating up.
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a touchstart event.
   */
  _onTouchStart(e: TouchEvent): void {
    this.hasFocus = true;
    const res = this.touchStartEvent(e);
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a touchend event.
   */
  _onTouchEnd(e: TouchEvent): void {
    this.hasFocus = true;
    const res = this.touchEndEvent(e);
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a touchcancel event.
   */
  _onTouchCancel(e: TouchEvent): void {
    this.hasFocus = true;
    const res = this.touchCancelEvent(e);
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Called for a touchmove event.
   */
  _onTouchMove(e: TouchEvent): void {
    this.hasFocus = true;
    const res = this.touchMoveEvent(e);
    if (!res) {
      e.stopPropagation();
      e.preventDefault();
    }
  }

  /**
   * Dispatch a bubbling, cancelable CustomEvent from the widget's element.
   * @param eventName The event type.
   * @param params Passed as the event's `detail`.
   */
  trigger(eventName: string, params?: unknown): CustomEvent {
    const event = new CustomEvent(eventName, {
      bubbles: true,
      cancelable: true,
      detail: params,
    });

    if (this.dispatchEvent) {
      this.dispatchEvent(event);
    }

    return event;
  }

  disableDropEvents(): void {
    if (!this._onDragEvent) {
      return;
    }

    this.removeEventListener('dragenter', this._onDragEvent);
    if (this._onDropEvent) {
      this.removeEventListener('drop', this._onDropEvent);
    }
    this.addEventListener('dragleave', this._onDragEvent);
    this.addEventListener('dragover', this._onDragEvent);
    if (this._onDropEvent) {
      this.addEventListener('drop', this._onDropEvent);
    }

    this._onDragEvent = null;
    this._onDropEvent = null;
  }

  enableDropEvents(): void {
    if (this._onDragEvent) {
      return;
    }

    this._onDragEvent = this.onDragEvent.bind(this);
    this._onDropEvent = this.onDropEvent.bind(this);

    this.addEventListener('dragenter', this._onDragEvent);
  }

  dragEnterEvent(event: DragEvent): void {}

  dragLeaveEvent(event: DragEvent): void {}

  dragOverEvent(event: DragEvent): void {}

  onDragEvent(event: DragEvent): void {
    const element = this.element;

    if (event.type === 'dragenter') {
      // A null listener is a no-op for addEventListener, so the guards preserve behavior.
      const onDrag = this._onDragEvent;
      const onDrop = this._onDropEvent;
      if (onDrag) {
        element.addEventListener('dragleave', onDrag);
        element.addEventListener('dragover', onDrag);
      }
      if (onDrop) {
        element.addEventListener('drop', onDrop);
      }
    }
    if (event.type === 'dragenter') {
      this.dragEnterEvent(event);
    }
    if (event.type === 'dragleave') {
      this.dragLeaveEvent(event);
    }
    if (event.type === 'dragover') {
      this.dragOverEvent(event);
    }
  }

  dropEvent(event: DragEvent): void {}

  onDropEvent(event: DragEvent): void {
    const onDrag = this._onDragEvent;
    const onDrop = this._onDropEvent;
    if (onDrag) {
      this.removeEventListener('dragleave', onDrag);
      this.removeEventListener('dragover', onDrag);
    }
    if (onDrop) {
      this.removeEventListener('drop', onDrop);
    }

    this.dropEvent(event);
  }
}
