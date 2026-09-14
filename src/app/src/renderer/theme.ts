// UI theme selection. The palettes live in css/theme.css; this only picks which one applies by
// stamping data-theme on the document element. Applied from the window's ?theme= query parameter
// before any widget is built (so the first paint is right), and again whenever the setting changes.
import { THEMES, type ThemeName } from "../shared/protocol.js";

export const DEFAULT_THEME: ThemeName = "dark";

export function isTheme(name: unknown): name is ThemeName {
  return typeof name === "string" && (THEMES as readonly string[]).includes(name);
}

export function applyTheme(name: string | null | undefined): ThemeName {
  const theme: ThemeName = isTheme(name) ? name : DEFAULT_THEME;
  document.documentElement.dataset.theme = theme;
  return theme;
}

/** Display label for a theme ("dark" -> "Dark"). */
export function themeLabel(theme: ThemeName): string {
  return theme.charAt(0).toUpperCase() + theme.slice(1);
}

export function currentTheme(): ThemeName {
  return document.documentElement.dataset.theme === "light" ? "light" : "dark";
}
