#!/usr/bin/env bash
# Brief 04c/06 per-pass GPU table capture. Runs .#demo-tracy headless (Tracy client broadcasts on
# :8086), records ~10s with tracy-capture, exports the per-frame GPU zones, and prints the mean ms
# per zone. Mirrors run.sh/capture.sh resource stitching. STRING_LIGHTS=0, settled window.
#
# Usage: tools/tracy_pass_table.sh <tag> "<STRING_CAM>" [extra env KEY=VAL ...]
set -uo pipefail

tag="$1"; shift
cam="$1"; shift

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
store="$(nix build --no-link --print-out-paths "$repo#demo-tracy" 2>/dev/null)"

res="$(mktemp -d -t string-tracy.XXXXXX)"
mkdir -p "$res/shaders"
for f in "$store/include/string/shaders/"*.spv; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
for f in "$repo/string-core/shaders/"*.slang "$repo/string-render-forward/shaders/"*.slang; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
ln -sfn "$repo/sandbox/assets" "$res/assets"

out="/tmp/tracy_${tag}.tracy"
rm -f "$out"

# Start the tracy capture listener FIRST (waits for the client to connect), then launch the demo.
nix shell nixpkgs#tracy -c tracy-capture -o "$out" -s 12 -f >"/tmp/tracy_${tag}_cap.log" 2>&1 &
cap_pid=$!
sleep 1

env STRING_RESOURCES_DIR="$res" \
    STRING_CAM="$cam" \
    STRING_LIGHTS=0 \
    "$@" \
    timeout 30 "$store/bin/string_demo" >"/tmp/tracy_${tag}_run.log" 2>&1 &
run_pid=$!

wait $cap_pid 2>/dev/null
kill $run_pid 2>/dev/null
wait $run_pid 2>/dev/null
rm -rf "$res"

if [ ! -f "$out" ]; then echo "NO TRACY CAPTURE ($tag) — see /tmp/tracy_${tag}_cap.log"; tail -5 "/tmp/tracy_${tag}_cap.log"; exit 1; fi

# Export GPU zones and print per-zone mean ms.
csv="/tmp/tracy_${tag}.csv"
nix shell nixpkgs#tracy -c tracy-csvexport -g "$out" >"$csv" 2>/dev/null
echo "=== $tag: per-pass GPU mean ms (zone: mean_ms count) ==="
python3 - "$csv" <<'PY'
import csv, sys, collections
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    print("  (no GPU zones)"); sys.exit()
# tracy-csvexport columns include "name" and a duration in ns (column varies by version).
dur_col = next((c for c in rows[0] if c.lower() in ("gpu_ns","gpu_time","duration_ns","ns","time_ns")), None)
if dur_col is None:
    # fall back: find a numeric column that isn't a count/id
    for c in rows[0]:
        try:
            float(rows[0][c]); dur_col = c; break
        except: pass
agg = collections.defaultdict(list)
for r in rows:
    try: agg[r.get("name","?")].append(float(r[dur_col]))
    except: pass
for name in sorted(agg):
    v = agg[name]
    print(f"  {name:20s} {sum(v)/len(v)/1e6:8.3f}  (n={len(v)})")
print(f"  [duration column: {dur_col}]")
PY
