#!/usr/bin/env bash
#
# Cook a glTF model's source textures (PNG/JPG) into KTX2/BC7 with a full mip chain, ready for the
# runtime loader to upload directly with no transcode (see sandbox/passes/texture_streamer.cpp).
# This is what removes the ~15s stb_image decode floor on large scenes (Sponza).
#
# `ktx create --encode` can only produce Basis (UASTC/ETC1S), not raw BC7 — so we cook in two
# stages: PNG -> UASTC (a temp), then `ktx transcode --target bc7` -> the shipped BC7 file. Baking
# BC7 straight to disk means the loader never transcodes (it just uploads BC7 blocks), which is why
# the streamer needs no RAM cache / background-transcode machinery. UASTC is a *universal*
# intermediate meant for runtime transcode to any GPU format; String targets desktop Vulkan + BC7
# only, so baking BC7 is the right trade. The loader keeps a UASTC->BC7 transcode fallback for any
# .ktx2 that still needs it, so an un-baked UASTC file (or a hand-supplied one) still loads.
#
# Each source image gets a `.ktx2` sibling next to it; the loader prefers that sibling when
# present and falls back to stb_image otherwise, so cooking is incremental and optional.
#
# Transfer function matters for correctness: base-color maps are sRGB, data maps (normal /
# metallic-roughness / occlusion) are linear. glTF knows each image's role, but a shell script
# doesn't, so we use a filename heuristic (overridable): names hinting a data map are cooked
# linear, everything else sRGB. A glTF-aware cook is a sensible follow-up if the heuristic misses.
#
# Requires the `ktx` CLI (KTX-Software). On nix it's provided by the devShell / `nix develop`.
#
# Usage:
#   tools/cook_textures.sh <dir>                 # cook every .png/.jpg under <dir>
#   tools/cook_textures.sh sandbox/assets/sponza
set -euo pipefail

if ! command -v ktx >/dev/null 2>&1; then
    echo "error: 'ktx' CLI not found (KTX-Software). Enter the dev shell: nix develop" >&2
    exit 1
fi

dir="${1:-}"
if [[ -z "$dir" || ! -d "$dir" ]]; then
    echo "usage: $0 <directory-of-source-textures>" >&2
    exit 2
fi

# Case-insensitive substrings that mark a *linear* (non-color) data map.
linear_re='normal|_nrm|_ddna|metallic|roughness|_orm|_arm|_mr|occlusion|_ao'

cooked=0
skipped=0
while IFS= read -r -d '' src; do
    out="${src%.*}.ktx2"
    # Skip if already cooked and up to date.
    if [[ -f "$out" && "$out" -nt "$src" ]]; then
        skipped=$((skipped + 1))
        continue
    fi

    lower="${src,,}"
    if [[ "$lower" =~ $linear_re ]]; then
        fmt=R8G8B8A8_UNORM
        tf=linear
    else
        fmt=R8G8B8A8_SRGB
        tf=srgb
    fi

    # Stage 1: PNG -> UASTC + full mip chain (a throwaway temp; UASTC is the only thing `ktx create`
    # can encode, and `ktx transcode` needs a Basis input to transcode from).
    # Stage 2: UASTC -> BC7, zstd-supercompressed. BC7 is ~8bpp; KTX2 deflates each mip level
    # independently, so the runtime reads only the levels it streams and inflates them fast (a
    # decompress, not a transcode). The GPU-resident BC7 size is the same either way.
    echo "cook  ($tf)  $src -> $out"
    tmp="$(mktemp --suffix=.ktx2)"
    trap 'rm -f "$tmp"' EXIT
    ktx create \
        --encode uastc \
        --generate-mipmap \
        --format "$fmt" \
        --assign-tf "$tf" \
        "$src" "$tmp"
    ktx transcode \
        --target bc7 \
        --zstd 18 \
        "$tmp" "$out"
    rm -f "$tmp"
    trap - EXIT
    cooked=$((cooked + 1))
done < <(find "$dir" -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' \) -print0)

echo "done: $cooked cooked, $skipped up-to-date"
