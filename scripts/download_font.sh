#!/bin/sh
FONT_TARGET="$1"
FONT_URL="https://raw.githubusercontent.com/adobe-fonts/source-han-sans/release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf"

if [ -f "${FONT_TARGET}" ] && [ $(wc -c < "${FONT_TARGET}") -gt 10000000 ]; then
  echo "[CMake Build Hook] Source Han Sans font exists and valid."
  exit 0
fi

echo "[CMake Build Hook] Downloading Source Han Sans font for Metro UI..."
curl -L --retry 3 --connect-timeout 10 -o "${FONT_TARGET}" "${FONT_URL}" || true
