// A transient popup menu at a screen position (right-click menus). Closes on a click outside,
// Escape, window blur, or when an item is chosen.

export interface ContextMenuItem {
  label?: string;
  disabled?: boolean;
  /** Renders a separator line instead of an item. */
  separator?: boolean;
  callback?: () => void;
}

let current: HTMLElement | null = null;

export function closeContextMenu(): void {
  if (current) {
    current.remove();
    current = null;
  }
  document.removeEventListener("mousedown", onDocumentMouseDown, true);
  document.removeEventListener("keydown", onKeyDown, true);
  window.removeEventListener("blur", closeContextMenu);
}

function onDocumentMouseDown(e: MouseEvent): void {
  if (current && !current.contains(e.target as Node)) closeContextMenu();
}

function onKeyDown(e: KeyboardEvent): void {
  if (e.key === "Escape") closeContextMenu();
}

export function showContextMenu(x: number, y: number, items: ContextMenuItem[]): void {
  closeContextMenu();
  const menu = document.createElement("div");
  menu.className = "menu-dropdown open context-menu";
  for (const item of items) {
    if (item.separator) {
      const sep = document.createElement("div");
      sep.className = "menu-separator";
      menu.appendChild(sep);
      continue;
    }
    const el = document.createElement("div");
    el.className = item.disabled ? "menu-item disabled" : "menu-item";
    el.textContent = item.label ?? "";
    if (!item.disabled) {
      el.onclick = () => {
        closeContextMenu();
        item.callback?.();
      };
    }
    menu.appendChild(el);
  }
  document.body.appendChild(menu);
  current = menu;
  // Keep the menu inside the window.
  const rect = menu.getBoundingClientRect();
  const left = Math.max(0, Math.min(x, window.innerWidth - rect.width - 2));
  const top = Math.max(0, Math.min(y, window.innerHeight - rect.height - 2));
  menu.style.left = `${left}px`;
  menu.style.top = `${top}px`;
  document.addEventListener("mousedown", onDocumentMouseDown, true);
  document.addEventListener("keydown", onKeyDown, true);
  window.addEventListener("blur", closeContextMenu);
}
