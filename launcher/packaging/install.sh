#!/bin/sh
# Install the launcher for this user: copies it to ~/.local/share/forza-reimagined/app, adds
# an application-menu entry and an icon. No root needed. Run from the extracted folder.
#   ./install.sh [--prefix DIR]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dest="${XDG_DATA_HOME:-$HOME/.local/share}/forza-reimagined/app"
[ "${1:-}" = "--prefix" ] && dest="$2"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
rm -rf "$dest"
mkdir -p "$dest" "$apps"
cp -a "$here/." "$dest/"
rm -f "$dest/install.sh"
sed -e "s|@EXEC@|$dest/forza-launcher|" -e "s|@ICON@|$dest/forza-reimagined.png|" \
    "$here/forza-reimagined.desktop" > "$apps/forza-reimagined.desktop"
rm -f "$dest/forza-reimagined.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1 || true
echo "Installed to $dest"
echo "Start it from your application menu (Forza Horizon 1: ReImagined) or run $dest/forza-launcher"
echo "If the window does not open on X11, install your distribution's libxcb-cursor package."
