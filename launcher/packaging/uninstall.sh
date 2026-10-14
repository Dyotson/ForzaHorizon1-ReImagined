#!/bin/sh
# Remove the launcher. Your saves, the built game and the extracted game files stay in
# ~/.local/share/forza (delete that folder yourself to free the space).
set -eu
dest="${XDG_DATA_HOME:-$HOME/.local/share}/forza-reimagined"
rm -rf "$dest"
rm -f "${XDG_DATA_HOME:-$HOME/.local/share}/applications/forza-reimagined.desktop"
echo "Launcher removed. Game data is kept in ${XDG_DATA_HOME:-$HOME/.local/share}/forza"
