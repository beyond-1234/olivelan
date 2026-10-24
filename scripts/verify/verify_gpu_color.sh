#!/usr/bin/env bash
# Run the offscreen GPU-vs-CPU OCIO color test with a real platform plugin.
# QT_QPA_PLATFORM is deliberately not forced to offscreen: the test skips when
# no GL context is available, which would make a green result meaningless.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST_BIN="${GPU_COLOR_TEST_BIN:-$ROOT/build/tests/general/gpu-color-tests}"

if [ ! -x "$TEST_BIN" ]; then
  echo "ERROR: gpu-color-tests not found: $TEST_BIN" >&2
  exit 1
fi

if [ -n "${QT_QPA_PLATFORM:-}" ]; then
  PLATFORM="$QT_QPA_PLATFORM"
elif [ "$(uname -s)" = "Darwin" ]; then
  PLATFORM="cocoa"
elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
  PLATFORM="wayland"
else
  PLATFORM="xcb"
fi

echo "[INFO] running GPU color test with QT_QPA_PLATFORM=$PLATFORM"
QT_QPA_PLATFORM="$PLATFORM" "$TEST_BIN"
