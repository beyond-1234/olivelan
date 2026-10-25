#!/usr/bin/env bash
# ==============================================================================
# Olivelan Icon Generator
# Renders the vector master (app/ui/icons/olivelan.svg) into all platform assets:
#   - macOS: app/packaging/macos/olive.icns (16px .. 1024px @2x)
#   - Linux: app/packaging/linux/icons/<size>x<size>/{app,mime} PNGs
#   - In-app About banner: app/ui/graphics/olive-splash.png
#
# Requirements: rsvg-convert (librsvg), iconutil (macOS), sips (macOS)
# Usage: bash scripts/generate_app_icons.sh
# ==============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SVG="$ROOT/app/ui/icons/olivelan_macos.svg"
LINUX_ICONS="$ROOT/app/packaging/linux/icons"
MACOS_DIR="$ROOT/app/packaging/macos"
ABOUT_BANNER="$ROOT/app/ui/graphics/olive-splash.png"

APP_ICON_NAME="org.olivelan.Olivelan.png"
MIME_ICON_NAME="application-vnd.olivelan-project.png"

command -v rsvg-convert >/dev/null 2>&1 || {
  echo "ERROR: rsvg-convert is required (librsvg / librsvg2-bin)." >&2
  exit 1
}

[ -f "$SVG" ] || {
  echo "ERROR: vector master not found: $SVG" >&2
  exit 1
}

echo "[1/3] Linux icon set..."
for size in 16 32 48 64 128 256 512; do
  dir="$LINUX_ICONS/${size}x${size}"
  mkdir -p "$dir"
  rsvg-convert -w "$size" -h "$size" "$SVG" -o "$dir/$APP_ICON_NAME"
  rsvg-convert -w "$size" -h "$size" "$SVG" -o "$dir/$MIME_ICON_NAME"
  echo "      ${size}x${size} OK"
done

echo "[2/3] macOS .icns..."
if command -v iconutil >/dev/null 2>&1; then
  iconset="$(mktemp -d "${TMPDIR:-/tmp}/olivelan_iconset.XXXXXX")"
  trap 'rm -rf "$iconset"' EXIT
  mkdir -p "$iconset/icon.iconset"

  rsvg-convert -w 16   -h 16   "$SVG" -o "$iconset/icon.iconset/icon_16x16.png"
  rsvg-convert -w 32   -h 32   "$SVG" -o "$iconset/icon.iconset/icon_16x16@2x.png"
  rsvg-convert -w 32   -h 32   "$SVG" -o "$iconset/icon.iconset/icon_32x32.png"
  rsvg-convert -w 64   -h 64   "$SVG" -o "$iconset/icon.iconset/icon_32x32@2x.png"
  rsvg-convert -w 128  -h 128  "$SVG" -o "$iconset/icon.iconset/icon_128x128.png"
  rsvg-convert -w 256  -h 256  "$SVG" -o "$iconset/icon.iconset/icon_128x128@2x.png"
  rsvg-convert -w 256  -h 256  "$SVG" -o "$iconset/icon.iconset/icon_256x256.png"
  rsvg-convert -w 512  -h 512  "$SVG" -o "$iconset/icon.iconset/icon_256x256@2x.png"
  rsvg-convert -w 512  -h 512  "$SVG" -o "$iconset/icon.iconset/icon_512x512.png"
  rsvg-convert -w 1024 -h 1024 "$SVG" -o "$iconset/icon.iconset/icon_512x512@2x.png"

  iconutil -c icns "$iconset/icon.iconset" -o "$MACOS_DIR/olive.icns"
  echo "      $MACOS_DIR/olive.icns OK"
else
  echo "      SKIP: iconutil not available (not macOS)"
fi

echo "[3/3] About dialog banner..."
rsvg-convert -w 512 -h 512 "$SVG" -o "$ABOUT_BANNER"
echo "      $ABOUT_BANNER OK"

echo "[OK] Icon assets regenerated from $SVG"
