#!/usr/bin/env bash
# Local build and install of the Flatpak (user installation). Needs flatpak-builder
# (flatpak install --user flathub org.flatpak.Builder), the freedesktop Sdk 26.08 and its
# llvm22 extension. The big files go to ~/.cache/forza-flatpak/ (FORZA_FLATPAK_CACHE).
#
#   flatpak/rebuild.sh [OVERLAY.tar.gz] [SDK-LICENSE]
#
# OVERLAY: the patched ReXGlue SDK pieces (CI builds rexglue-overlay-linux-amd64.tar.gz; locally
# tools/package_sdk_overlay.sh makes one from tools/build_rexglue_sdk.sh's output).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
C="${FORZA_FLATPAK_CACHE:-$HOME/.cache/forza-flatpak}"
mkdir -p "$C"
OVERLAY="${1:-$C/rexglue-overlay-linux-amd64.tar.gz}"
[ -f "$OVERLAY" ] || "$HERE/../tools/package_sdk_overlay.sh" "$OVERLAY"
LIC=()
[ -n "${2:-}" ] && LIC=(--sdk-license "$2")
python3 "$HERE/stage.py" --overlay "$OVERLAY" "${LIC[@]}"
BUILDER=(flatpak-builder)
command -v flatpak-builder >/dev/null || BUILDER=(flatpak run org.flatpak.Builder)
"${BUILDER[@]}" --user --install --force-clean --disable-rofiles-fuse --state-dir="$C/state" \
    --repo="$C/repo" "$C/build" "$HERE/io.github.dyotson.ForzaHorizon1ReImagined.yml"
