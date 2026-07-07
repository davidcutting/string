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
out="$(nix build --no-link --print-out-paths "$repo#")"

res="$(mktemp -d -t string-resources.XXXXXX)"
trap 'rm -rf "$res"' EXIT
ln -sfn "$out/include/string/shaders" "$res/shaders"
ln -sfn "$repo/sandbox/assets" "$res/assets"

export STRING_RESOURCES_DIR="$res"
echo "STRING_RESOURCES_DIR=$res"
echo "  shaders -> $out/include/string/shaders"
echo "  assets  -> $repo/sandbox/assets"

exec "$out/bin/string_demo"
