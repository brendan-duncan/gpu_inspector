#!/usr/bin/env bash
# Installs a desktop entry for this checkout, so the GNOME/KDE dock and application list show
# GPU Inspector with its own icon and name instead of a generic placeholder. Desktops identify a
# window by its WM_CLASS ("gpu-inspector") and take the icon from the matching .desktop file; the
# icon in the window's own _NET_WM_ICON property is only a fallback, and GNOME ignores it.
#
#   tools/install_desktop_entry.sh              install for the current user
#   tools/install_desktop_entry.sh --uninstall  remove it again
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
FILE="$DIR/gpu-inspector.desktop"

if [[ "${1:-}" == "--uninstall" ]]; then
    rm -f "$FILE"
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DIR" || true
    echo "removed $FILE"
    exit 0
fi

mkdir -p "$DIR"
cat > "$FILE" <<DESKTOP
[Desktop Entry]
Type=Application
Name=GPU Inspector
Comment=Graphics inspector for native Vulkan applications
Exec=$ROOT/tools/gpu-inspector
Icon=$ROOT/app/assets/icon.png
Terminal=false
Categories=Development;
Keywords=Vulkan;GPU;Graphics;Debugger;Profiler;
StartupWMClass=gpu-inspector
DESKTOP

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DIR" || true
echo "installed $FILE"
echo "  launches: $ROOT/tools/gpu-inspector"
echo "  icon:     $ROOT/app/assets/icon.png"
