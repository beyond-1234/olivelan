#!/usr/bin/env bash
# Run a mixed HDR/SDR project and assert that the HDR view is selected once at
# load time and does not churn during playback.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP="${OLIVE_APP:-$ROOT/build/app/Olivelan.app/Contents/MacOS/Olivelan}"
PROJECT="${1:?usage: verify_mixed_hdr_sdr.sh <mixed.ovexml>}"
SECONDS_TO_PLAY="${OLIVE_SECONDS:-3}"
LOG="$(mktemp "${TMPDIR:-/tmp}/olive-mixed-hdr.XXXXXX")"
trap 'rm -f "$LOG"' EXIT

python3 - "$APP" "$PROJECT" "$SECONDS_TO_PLAY" "$LOG" <<'PY'
import subprocess, sys, os
app, project, seconds, log = sys.argv[1:5]
with open(log, "w") as f:
    p = subprocess.Popen([app, "--autoplay", seconds, project],
                         stdout=f, stderr=subprocess.STDOUT, text=True,
                         env=os.environ.copy())
    try:
        p.wait(timeout=max(30, int(seconds) + 20))
    except subprocess.TimeoutExpired:
        p.terminate(); p.wait(timeout=8)
        sys.exit(2)
PY

python3 - "$LOG" <<'PY'
import re, sys
text = open(sys.argv[1], errors="replace").read()

def stats(prefix):
    m = re.search(rf"\[Autoplay\] color stats {prefix}transform_changes (\d+) hdr_view_switches (\d+) shader_compiles (\d+) shader_cache_hits (\d+)", text)
    if not m:
        return None
    return tuple(map(int, m.groups()))

baseline = stats("baseline ")
final = stats("")
if baseline is None or final is None:
    print("FAIL: missing color stats baseline/final lines", file=sys.stderr)
    print(text[-2000:], file=sys.stderr)
    sys.exit(1)

bt, bh, bc, bch = baseline
ft, fh, fc, fch = final
print(f"baseline: transform_changes={bt} hdr_view_switches={bh} shader_compiles={bc}")
print(f"final:    transform_changes={ft} hdr_view_switches={fh} shader_compiles={fc}")
print(f"playback delta: transforms={ft-bt} hdr_switches={fh-bh} shader_compiles={fc-bc} cache_hits={fch-bch}")

if bh <= 0:
    print("FAIL: no HDR view was selected at load time", file=sys.stderr)
    sys.exit(1)
if ft != bt or fh != bh:
    print("FAIL: HDR view churned during playback", file=sys.stderr)
    sys.exit(1)
if fc > bc + 2:
    print("FAIL: too many shader compiles during playback", file=sys.stderr)
    sys.exit(1)
print("PASS: HDR view is sticky and did not switch at edit points")
PY
