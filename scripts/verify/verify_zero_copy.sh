#!/usr/bin/env bash
# Verify that a playback session used hardware frames and did not fall back to
# CPU readback. This intentionally does NOT use OLIVE_DUMP_FRAME: a framebuffer
# readback is itself a CPU readback and cannot prove zero-copy.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP="${OLIVE_APP:-$ROOT/build/app/Olivelan.app/Contents/MacOS/Olivelan}"
PROJECT="${1:-}"
if [ -z "$PROJECT" ]; then
  echo "Usage: $0 <project.ove>" >&2
  exit 1
fi
SECONDS_TO_PLAY="${OLIVE_SECONDS:-2}"
LOG="$(mktemp "${TMPDIR:-/tmp}/olive-zero-copy.XXXXXX")"
trap 'rm -f "$LOG"' EXIT

python3 - "$APP" "$PROJECT" "$SECONDS_TO_PLAY" "$LOG" <<'PY'
import subprocess, sys, os
app, project, seconds, log = sys.argv[1:5]
cmd = [app, '--autoplay', seconds, project]
with open(log, 'w') as f:
    p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, text=True, env=os.environ.copy())
    try:
        p.wait(timeout=max(30, int(seconds) + 20))
    except subprocess.TimeoutExpired:
        p.terminate()
        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        print(f'FAIL: process did not finish within {max(30, int(seconds) + 20)}s', file=sys.stderr)
        sys.exit(2)
PY

LINE="$(grep '\[Autoplay\] HAL stats' "$LOG" | tail -1 || true)"
if [ -z "$LINE" ]; then
  echo "FAIL: no HAL stats line; playback did not start or crashed" >&2
  tail -40 "$LOG" >&2
  exit 1
fi

printf '%s\n' "$LINE"
read -r HW READBACK FAILURES < <(python3 - "$LINE" <<'PY'
import re, sys
s = sys.argv[1]
def n(key):
    m = re.search(rf'{key}\s+(\d+)', s)
    return int(m.group(1)) if m else -1
print(n('hardware_frames'), n('cpu_readback_frames'), n('import_failures'))
PY
)

if [ "$HW" -le 0 ]; then
  echo "FAIL: hardware_frames=$HW (expected > 0)" >&2
  exit 1
fi
if [ "$READBACK" -ne 0 ]; then
  echo "FAIL: cpu_readback_frames=$READBACK (expected 0)" >&2
  exit 1
fi
if [ "$FAILURES" -ne 0 ]; then
  echo "FAIL: import_failures=$FAILURES (expected 0)" >&2
  exit 1
fi

echo "PASS: hardware_frames=$HW, cpu_readback_frames=0, import_failures=0"
