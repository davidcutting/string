#!/usr/bin/env bash
# Set up a PERSISTENT resources dir (like run.sh, but without the auto-cleanup trap) and print the
# executable + env RenderDoc needs. Run this, then paste the printed values into qrenderdoc's
# "Launch Application" dialog. Clean up the printed dir yourself when done.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Building..."
out="$(nix build --no-link --print-out-paths "$repo#demo")"

res="$(mktemp -d -t string-res-rdoc.XXXXXX)"   # NOT auto-deleted (RenderDoc needs it to persist)
mkdir -p "$res/shaders"
for f in "$out/include/string/shaders/"*.spv; do ln -sfn "$f" "$res/shaders/$(basename "$f")"; done
for f in "$repo/string-engine/shaders/"*.slang "$repo/sandbox/shaders/"*.slang; do
    ln -sfn "$f" "$res/shaders/$(basename "$f")"
done
ln -sfn "$repo/sandbox/assets" "$res/assets"

echo
echo "======================================================================"
echo " Plug these into qrenderdoc  ->  File > Launch Application:"
echo "----------------------------------------------------------------------"
echo " Executable Path : $out/bin/string_demo"
echo " Working Dir     : $repo"
echo " Environment     : STRING_RESOURCES_DIR = $res"
echo "                   SDL_VIDEODRIVER      = x11        (force XWayland; RenderDoc + Wayland is flaky)"
echo "                   STRING_LIGHTS        = 0          (optional: kill the 384 test lights)"
echo " Options         : tick 'Capture Child Processes'   (string_demo is a wrapper that execs the real ELF)"
echo "======================================================================"
echo " Then: in the app, aim at the arcade floor, press F12 to capture, close the app."
echo " Clean up when done:  rm -rf $res"
