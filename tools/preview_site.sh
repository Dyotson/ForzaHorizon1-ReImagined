#!/usr/bin/env bash
# Assemble site/ the way .github/workflows/pages.yml does (the page uses docs/images) and serve it.
#   tools/preview_site.sh [PORT]     -> http://127.0.0.1:8000/   (the assembled copy is in ${TMPDIR:-/tmp}/forza-site)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/forza-site"
rm -rf "$OUT"; mkdir -p "$OUT/images"
cp -r "$ROOT/site/." "$OUT/"
cp "$ROOT"/docs/images/*.jpg "$ROOT/docs/images/logo.svg" "$ROOT/docs/images/logo.png" "$ROOT/docs/images/social-preview.png" "$OUT/images/"
echo "serving $OUT on http://127.0.0.1:${1:-8000}/"
exec python3 -m http.server "${1:-8000}" --bind 127.0.0.1 --directory "$OUT"
