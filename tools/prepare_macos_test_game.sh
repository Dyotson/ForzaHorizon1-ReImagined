#!/usr/bin/env bash
# APFS clone with single-frame startup movies for local rendering tests.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="${FORZA_DATA:-$ROOT/extracted/game}"
DEST="${FORZA_MACOS_TEST_GAME:-$ROOT/extracted/macos-test-game}"
MARKER="$DEST/.forza-macos-short-intros"
if [ -f "$MARKER" ] && [ "$(cat "$MARKER")" = "$SOURCE" ]; then
    printf '%s\n' "$DEST"
    exit 0
fi
if [ -e "$DEST" ]; then
    echo "Test data destination already exists without a matching marker: $DEST" >&2
    exit 1
fi
[ "$(uname -s)" = Darwin ] || { echo "This helper requires macOS APFS cloning." >&2; exit 1; }
[ -f "$SOURCE/default.xex" ] || { echo "Missing game files: $SOURCE" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "Install ffmpeg for movie-free test launches: brew install ffmpeg" >&2; exit 1; }
STAGING="$DEST.tmp.$$"
trap 'rm -rf "$STAGING"' EXIT
cp -cR "$SOURCE" "$STAGING"
for name in Dolby_Corona_Intro Forza_Intro Forza_Tone MSGS_Logo T10_MS_Combined; do
    movie="$STAGING/media/ui/videos/splash_intros/$name.wmv"
    [ -f "$movie" ] || continue
    ffmpeg -hide_banner -loglevel error -y -i "$movie" -map 0 -t 0.05 \
        -c copy -f asf "$movie.short.wmv"
    mv "$movie.short.wmv" "$movie"
done
printf '%s\n' "$SOURCE" > "$STAGING/.forza-macos-short-intros"
mv "$STAGING" "$DEST"
printf '%s\n' "$DEST"
