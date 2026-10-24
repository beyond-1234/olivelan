#!/usr/bin/env bash
# Capture the viewer framebuffer after a short playback. This validates the
# GPU render/readback hook and can compare an explicit expected SHA256 when a
# reference frame is supplied. It is not a zero-copy test.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP="${OLIVE_APP:-$ROOT/build/app/Olive.app/Contents/MacOS/Olive}"
PROJECT="${1:-}"
if [ -z "$PROJECT" ]; then
  echo "Usage: $0 <project.ove>" >&2
  exit 1
fi
SECONDS_TO_PLAY="${OLIVE_SECONDS:-1}"
OUT="${OLIVE_DUMP_FRAME:-$(mktemp "${TMPDIR:-/tmp}/olive-frame.XXXXXX")}"
LOG="$(mktemp "${TMPDIR:-/tmp}/olive-frame.XXXXXX")"
trap 'rm -f "$LOG"' EXIT

python3 - "$APP" "$PROJECT" "$SECONDS_TO_PLAY" "$OUT" "$LOG" <<'PY'
import subprocess, sys, os
app, project, seconds, out, log = sys.argv[1:6]
env = os.environ.copy()
env['OLIVE_DUMP_FRAME'] = out
with open(log, 'w') as f:
    p = subprocess.Popen([app, '--autoplay', seconds, project], stdout=f, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        p.wait(timeout=max(30, int(seconds) + 20))
    except subprocess.TimeoutExpired:
        p.terminate()
        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        print(f'FAIL: process did not finish within {max(30, int(seconds) + 20)}s', file=sys.stderr)
        sys.exit(2)
PY

if [ ! -s "$OUT" ]; then
  echo "FAIL: frame dump was not written: $OUT" >&2
  tail -40 "$LOG" >&2
  exit 1
fi

python3 - "$OUT" <<'PY'
import struct, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    header = f.read(24)
if header[:8] != b'\x89PNG\r\n\x1a\n':
    print(f'FAIL: not a PNG: {p}', file=sys.stderr); sys.exit(1)
w, h = struct.unpack('>II', header[16:24])
print(f'PNG {w}x{h} {p}')
PY

sha="$(shasum -a 256 "$OUT" | awk '{print $1}')"
echo "SHA256 $sha"
if [ -n "${OLIVE_EXPECTED_SHA256:-}" ] && [ "$sha" != "$OLIVE_EXPECTED_SHA256" ]; then
  echo "FAIL: frame SHA256 mismatch (expected $OLIVE_EXPECTED_SHA256)" >&2
  exit 1
fi
echo "PASS: frame dump captured"
