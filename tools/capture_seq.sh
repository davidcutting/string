#!/usr/bin/env bash
# Headless capture-SEQUENCE helper (brief 04d motion-ghosting verification). Mirrors capture.sh's
# resource-dir stitching, but drives a MOTION lever (STRING_ORBIT) + STRING_CAPTURE_EVERY_N to write
# a numbered PNG sequence, so the two-phase phase-1/phase-2 interleaved path (only reached under
# per-frame disocclusion) can be inspected frame-by-frame.
#
# Usage: tools/capture_seq.sh <out_prefix.png> "<STRING_CAM value>" <orbit_speed> <every_n> [extra ENV=VAL ...]
#   e.g. tools/capture_seq.sh /tmp/ghost/f.png "9,4.5,0,3.1416,0.05" 0.5 5 STRING_HIZ=1
# Writes /tmp/ghost/f_<frame>.png for frames that are multiples of <every_n> (default capture
# lifetime ~ the timeout). Set STRING_CAPTURE_FRAME=0 disables the single-shot capture.
set -euo pipefail

out="$1"; shift
cam="$1"; shift
orbit="$1"; shift
every="$1"; shift

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
store="$(nix build --no-link --print-out-paths "$repo#demo" 2>/dev/null)"

res="$(mktemp -d -t string-cap.XXXXXX)"
trap 'rm -rf "$res"' EXIT
mkdir -p "$res/shaders"
for f in "$store/include/string/shaders/"*.spv; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
for f in "$repo/string-core/shaders/"*.slang "$repo/string-render-forward/shaders/"*.slang; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
ln -sfn "$repo/sandbox/assets" "$res/assets"

mkdir -p "$(dirname "$out")"

env STRING_RESOURCES_DIR="$res" \
    STRING_CAPTURE_FRAME=0 \
    STRING_CAPTURE_EVERY_N="$every" \
    STRING_CAPTURE_PATH="$out" \
    STRING_ORBIT="$orbit" \
    STRING_CAM="$cam" \
    STRING_LIGHTS=0 \
    "$@" \
    timeout 40 "$store/bin/string_demo" >"${out}.log" 2>&1 || true

ls -1 "$(dirname "$out")"/*.png 2>/dev/null | head -40 || { echo "NO CAPTURES — see ${out}.log"; tail -30 "${out}.log"; }
