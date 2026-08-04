#!/usr/bin/env bash
# Headless capture helper for brief-04 A/B verification. Mirrors run.sh's resource-dir
# stitching (store shaders + on-disk sponza), then runs the demo with STRING_CAPTURE_* and
# a timeout so it exits after writing the BMP.
#
# Usage: tools/capture.sh <out.bmp> "<STRING_CAM value>" [extra env KEY=VAL ...]
#   e.g. tools/capture.sh /tmp/a.bmp "9,4.5,0,3.1416,0.05" STRING_HIZ=1
set -euo pipefail

out_bmp="$1"; shift
cam="$1"; shift

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
store="$(nix build --no-link --print-out-paths "$repo#demo" 2>/dev/null)"

res="$(mktemp -d -t string-cap.XXXXXX)"
trap 'rm -rf "$res"' EXIT
mkdir -p "$res/shaders"
for f in "$store/include/string/shaders/"*.spv; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
for f in "$repo/string-core/shaders/"*.slang "$repo/string-render-forward/shaders/"*.slang; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
ln -sfn "$repo/sandbox/assets" "$res/assets"

env STRING_RESOURCES_DIR="$res" \
    STRING_CAPTURE_FRAME=300 \
    STRING_CAPTURE_PATH="$out_bmp" \
    STRING_CAM="$cam" \
    STRING_LIGHTS=0 \
    "$@" \
    timeout 40 "$store/bin/string_demo" >"${out_bmp}.log" 2>&1 || true

if [ -f "$out_bmp" ]; then echo "captured $out_bmp"; else echo "NO CAPTURE — see ${out_bmp}.log"; tail -20 "${out_bmp}.log"; fi
