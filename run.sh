#!/usr/bin/env bash
#
# Run the String demo (Sponza scene).
#
# Sponza is gitignored (~280 MB — too large to track), so it isn't baked into the Nix store and
# the wrapped binary's default STRING_RESOURCES_DIR (which points at the store) doesn't contain
# it. This script stitches the store's built shaders together with the on-disk sandbox/assets
# tree (which *does* have sponza) into one resources dir, and points the binary at it.
#
# The binary wraps STRING_RESOURCES_DIR with --set-default (see flake.nix), so the value we
# export here wins. Validation layers + loader paths still come from the wrapper.
#
# Usage: ./run.sh
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Building..."
out="$(nix build --no-link --print-out-paths "$repo#demo")"

res="$(mktemp -d -t string-resources.XXXXXX)"
trap 'rm -rf "$res"' EXIT

# Shaders: the engine now compiles Slang in-process and hot-reloads on save (brief 01). So the
# runtime shaders/ dir must expose the *editable* source tree, not the read-only Nix store — edit
# a .slang, save, see it swap. We build a writable shaders/ dir that symlinks:
#   - the store's build-time GLSL .spv (the passes not yet ported: 3d/shadow/cull/grid), and
#   - the on-disk .slang SOURCES (engine + sandbox) so saves are picked up by the watcher.
# The .slang sources shadow the store's copies; editing sandbox/shaders or string-engine/shaders
# is what the hot-reload loop watches.
mkdir -p "$res/shaders"
for f in "$out/include/string/shaders/"*.spv; do
    ln -sfn "$f" "$res/shaders/$(basename "$f")"
done
for f in "$repo/string-engine/shaders/"*.slang "$repo/sandbox/shaders/"*.slang; do
    ln -sfn "$f" "$res/shaders/$(basename "$f")"
done
ln -sfn "$repo/sandbox/assets" "$res/assets"

export STRING_RESOURCES_DIR="$res"
echo "STRING_RESOURCES_DIR=$res"
echo "  shaders(.spv)   -> $out/include/string/shaders"
echo "  shaders(.slang) -> $repo/{string-engine,sandbox}/shaders  (editable; hot-reloaded)"
echo "  assets          -> $repo/sandbox/assets"

exec "$out/bin/string_demo"
