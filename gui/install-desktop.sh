#!/usr/bin/env bash
# Installs a desktop entry + themed icons for FDM so the dock / alt-tab / app
# grid show the FDM icon. Needed on GNOME (and good practice elsewhere): the
# dock icon comes from a .desktop file matched to the window's WM_CLASS, not
# from the in-app setWindowIcon.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # gui/
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Prefer the launcher wrapper (strips snap-leaked lib paths); fall back to the
# raw binary.
EXEC=""
for c in "$REPO_ROOT/build/gui/fdm-gui.sh" "$REPO_ROOT/build/gui/fdm-gui"; do
  [[ -x "$c" ]] && EXEC="$c" && break
done
if [[ -z "$EXEC" ]]; then
  echo "error: build fdm-gui first (cmake --build build --target fdm-gui)" >&2
  exit 1
fi

# 1) Icons into the hicolor theme so Icon=fdm resolves.
for s in 16 32 48 128 256; do
  dest="$HOME/.local/share/icons/hicolor/${s}x${s}/apps"
  mkdir -p "$dest"
  cp "$SCRIPT_DIR/icons/fdm-${s}.png" "$dest/fdm.png"
done

# 2) Desktop entry. StartupWMClass must match the window's WM_CLASS (Qt derives
#    it from setDesktopFileName("fdm") / applicationName "fdm").
APPS="$HOME/.local/share/applications"
mkdir -p "$APPS"
cat > "$APPS/fdm.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=FDM
Comment=Download manager
Exec=$EXEC %u
Icon=fdm
Terminal=false
Categories=Network;FileTransfer;
StartupWMClass=fdm
EOF

# 3) Refresh caches (best effort; safe if the tools are missing).
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
  gtk-update-icon-cache -f "$HOME/.local/share/icons/hicolor" >/dev/null 2>&1 || true
command -v update-desktop-database >/dev/null 2>&1 && \
  update-desktop-database "$APPS" >/dev/null 2>&1 || true

echo "Installed:"
echo "  $APPS/fdm.desktop  (Exec=$EXEC)"
echo "  hicolor icons -> fdm.png (16..256)"
echo
echo "Relaunch fdm-gui. The dock / alt-tab icon should now be the FDM mark."
echo "If it doesn't match, run 'xprop WM_CLASS', click the FDM window, and set"
echo "StartupWMClass in the .desktop to the value shown."
