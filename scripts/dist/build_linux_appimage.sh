#!/usr/bin/env bash
# Build a fat AppImage from the self-contained Linux dist directory.
# Must run on Linux (or inside the olive-build-qt68 container).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST_DIR="${DIST_DIR:-$ROOT/dist/linux-x86_64}"
SRC="${SRC:-$ROOT}"
APPDIR="${APPDIR:-$ROOT/dist/AppDir}"
ARCH="${ARCH:-x86_64}"
TOOL="${APPIMAGETOOL:-$ROOT/build/tools/appimagetool-$ARCH.AppImage}"
OUT="${OUT:-$ROOT/dist/Olive-linux-$ARCH.AppImage}"

command -v mksquashfs >/dev/null 2>&1 || {
  echo "ERROR: mksquashfs is required (squashfs-tools)." >&2
  exit 1
}
command -v file >/dev/null 2>&1 || {
  echo "ERROR: file is required." >&2
  exit 1
}

[ -x "$DIST_DIR/bin/olive-editor" ] || {
  echo "ERROR: missing $DIST_DIR/bin/olive-editor; run build_linux_dist.sh first." >&2
  exit 1
}

if [ ! -x "$TOOL" ]; then
  mkdir -p "$(dirname "$TOOL")"
  curl -fL --retry 3 --max-time 300 \
    -o "$TOOL" \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
  chmod +x "$TOOL"
fi

rm -rf "$APPDIR"
cp -a "$DIST_DIR" "$APPDIR"
cp "$APPDIR/run-olive.sh" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
cp "$SRC/app/packaging/linux/org.olivelan.Olivelan.desktop" "$APPDIR/"
cp "$SRC/app/packaging/linux/icons/256x256/org.olivelan.Olivelan.png" "$APPDIR/"

APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$ARCH" \
  "$TOOL" --no-appstream "$APPDIR" "$OUT"

echo "[OK] $OUT"
