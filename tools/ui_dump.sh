#!/usr/bin/env bash
# Layout-tree dump helper (brief 12, M0a) — the MECHANICAL gate for the UI facade migration.
#
# Runs the ui-dev scene, lets it settle to a fixed frame, and writes the positioned layout tree
# (ids, resolved boxes, sizing resolution, format, visuals, text) as diffable text. Diff two dumps
# across a change to catch silently-changed sizing resolution / id assignment / tree structure —
# the bug class that is murder to spot by eye. Motion and docking feel are NOT covered; those stay
# eyes-on.
#
# STRING_FIXED_DT is forced: the brief-05 authors show a frame counter and tick dt-driven cooldowns,
# so without it two runs of the SAME code differ. Same discipline as tools/capture.sh's fixed cam.
#
# Usage:
#   tools/ui_dump.sh <outdir> [screen ...]     # default: all five brief-05 screens
#     e.g. tools/ui_dump.sh /tmp/ui/base
#          tools/ui_dump.sh /tmp/ui/after inventory
#   Compare: diff -u /tmp/ui/base/inventory.txt /tmp/ui/after/inventory.txt
set -euo pipefail

outdir="$1"; shift
screens=("$@")
if [ ${#screens[@]} -eq 0 ]; then
    screens=(nameplates inventory actionbar chat all)
fi

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
store="$(nix build --no-link --print-out-paths "$repo#demo" 2>/dev/null)"

res="$(mktemp -d -t string-uidump.XXXXXX)"
trap 'rm -rf "$res"' EXIT
mkdir -p "$res/shaders" "$outdir"
for f in "$store/include/string/shaders/"*.spv; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
for f in "$repo/string-engine/shaders/"*.slang "$repo/sandbox/shaders/"*.slang; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
ln -sfn "$repo/sandbox/assets" "$res/assets"

# One run at a time, never a batch: stacked GPU runs are how the phase-2 device-lost crash compounded.
for screen in "${screens[@]}"; do
    out="$outdir/$screen.txt"
    env STRING_RESOURCES_DIR="$res" \
        STRING_SCENE=ui \
        STRING_UI_SCREEN="$screen" \
        STRING_FIXED_DT=0.016 \
        STRING_UI_DUMP="$out" \
        STRING_UI_DUMP_FRAME="${STRING_UI_DUMP_FRAME:-120}" \
        timeout 40 "$store/bin/string_demo" >"$out.log" 2>&1 || true

    if [ -f "$out" ]; then
        echo "dumped $screen -> $out ($(grep -c '^ ' "$out" || true) nodes)"
    else
        echo "NO DUMP for $screen — see $out.log"
        tail -20 "$out.log"
    fi
done
