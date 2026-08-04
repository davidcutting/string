#!/usr/bin/env bash
# Headless capture+log harness for 03b A/B. Usage: cap.sh <tag> <extra-env...> -- <capture_frame>
# Builds once (cached), stitches resources like run.sh, runs the demo with the given env, writes
# /tmp/cap_<tag>.bmp and /tmp/cap_<tag>.log. Set STRING_CAP_SECS to change the timeout.
set -uo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tag="$1"; shift
secs="${STRING_CAP_SECS:-40}"
frame="${STRING_CAP_FRAME:-300}"

out="$(nix build --no-link --print-out-paths "$repo#demo" 2>/dev/null)"
res="$(mktemp -d -t string-resources.XXXXXX)"
mkdir -p "$res/shaders"
for f in "$out/include/string/shaders/"*.spv; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
for f in "$repo/string-core/shaders/"*.slang "$repo/string-render-forward/shaders/"*.slang; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
ln -sfn "$repo/sandbox/assets" "$res/assets"

env STRING_RESOURCES_DIR="$res" \
    STRING_CAPTURE_FRAME="$frame" STRING_CAPTURE_PATH="/tmp/cap_${tag}.bmp" \
    "$@" \
    timeout "$secs" "$out/bin/string_demo" >"/tmp/cap_${tag}.log" 2>&1
rm -rf "$res"
echo "=== $tag: cull/frametime lines ==="
grep -E '\[mesh-cull\]|\[frametime\]|\[crowd\]|VUID|Validation' "/tmp/cap_${tag}.log" | head -40
