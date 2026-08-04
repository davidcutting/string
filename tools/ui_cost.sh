#!/usr/bin/env bash
# Per-phase UI CPU cost (brief 12c M0) — the measurement every 12c milestone reports against.
#
# Runs the ui-dev scene headless at one or more nameplate counts and prints UIPass's [ui-cpu] rolling
# average, which since 12c M0 carries a PHASE BREAKDOWN: author / layout / pack / upload, plus node,
# surface and skipped-surface counts. The combined number can say the UI costs N ms; only the
# breakdown says which of the four to attack.
#
# A REPORT, not a gate — same discipline as string-ui/test/ui_bench.cpp. Absolute numbers depend on
# the machine and on what else is running; compare phases WITHIN a run, and re-derive both sides when
# comparing across a change.
#
# STRING_FIXED_DT is forced so the projected nameplate positions are identical run to run. Note the
# skipped-surface count still swings, because the demo's plates scale font_px with camera distance —
# that is real, and it is the app-side precondition recorded in brief 12c M0.
#
# Usage:
#   tools/ui_cost.sh 0 500 2000        # one run per count, SERIALLY
#   tools/ui_cost.sh 0                 # just the realistic shell
#
# NOTE: numbers below were taken on a -O0 build (packages.demo used mesonBuildType="debug" until
# 2026-08-04). The flake now builds debugoptimized; an optimized run is ~7x faster than these.
# Baseline at time of writing (2026-08-03, 12b complete / 12c not started, -O0):
#   plates=0     172 nodes  0.236ms  author 56%  layout  9%  pack 33%  upload 0.4%
#   plates=500  1402 nodes  1.545ms  author 52%  layout 11%  pack 36%  upload 0.4%
#   plates=2000 5097 nodes  6.16ms   author 48%  layout 16%  pack 36%  upload 0.5%
set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
store="$(nix build --no-link --print-out-paths "$repo#demo" 2>/dev/null)"

res="$(mktemp -d -t string-uicost.XXXXXX)"
trap 'rm -rf "$res"' EXIT
mkdir -p "$res/shaders"
for f in "$store/include/string/shaders/"*.spv; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
for f in "$repo/string-core/shaders/"*.slang "$repo/string-render-forward/shaders/"*.slang; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
ln -sfn "$repo/sandbox/assets" "$res/assets"

counts=("$@")
[ ${#counts[@]} -eq 0 ] && counts=(0 500 2000)

# One run at a time, never a batch: stacked GPU runs are how the phase-2 device-lost crash compounded.
for np in "${counts[@]}"; do
    log="$(mktemp -t string-uicost-log.XXXXXX)"
    env STRING_RESOURCES_DIR="$res" \
        STRING_SCENE=ui \
        STRING_UI_SCREEN=all \
        STRING_UI_NAMEPLATES="$np" \
        STRING_FIXED_DT=0.016 \
        timeout 35 "$store/bin/string_demo" >"$log" 2>&1

    echo "=== nameplates=$np ==="
    if grep -q '\[ui-cpu\]' "$log"; then
        grep '\[ui-cpu\]' "$log" | tail -3
    else
        echo "  NO [ui-cpu] lines — the run died or did not reach 300 frames:"
        tail -15 "$log"
    fi
    rm -f "$log"
done
