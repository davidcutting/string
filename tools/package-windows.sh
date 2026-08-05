#!/usr/bin/env bash
# Assemble a shippable Windows package from a cross build tree.
#
#   tools/package-windows.sh <build-dir> <out-dir>
#   e.g. tools/package-windows.sh build-win-rel /tmp/pkg/string-windows-x64
#
# Hand-assembling this folder twice produced two shipped-and-broken packages: a missing
# assets/fonts/ (null deref in stb_truetype) and missing shaders/*.spv (lookdev failed to open
# debug_line.vert.spv). Both were "I forgot a file", not build problems. So this script derives what
# to copy from the build output and the exe's OWN IMPORT TABLE, then verifies the result before
# claiming success.
#
# Shader files come from TWO places and both are required:
#   *.slang  source, compiled at runtime by Slang (and the cache key is their content hash)
#   *.spv    prebuilt by glslangValidator at build time (debug_line, grid_2d_shader)
set -euo pipefail

build="${1:?usage: package-windows.sh <build-dir> <out-dir>}"
out="${2:?usage: package-windows.sh <build-dir> <out-dir>}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[ -f "$build/string_demo.exe" ] || { echo "no string_demo.exe in $build" >&2; exit 1; }

objdump() { nix-shell -p pkgsCross.mingwW64.buildPackages.binutils --run "x86_64-w64-mingw32-objdump $*" 2>/dev/null; }
strip_exe() { nix-shell -p pkgsCross.mingwW64.buildPackages.binutils --run "x86_64-w64-mingw32-strip -o '$1' '$2'" 2>/dev/null; }

rm -rf "$out"; mkdir -p "$out/shaders" "$out/assets"

# --- exe (stripped: 172MB -> ~7MB, and it drops the build-host paths in the debug info) ---
strip_exe "$out/string_demo.exe" "$build/string_demo.exe"

# --- DLLs, resolved from the import table rather than a list someone maintains by hand ---
# KERNEL32/msvcrt/etc. are Windows' own. Everything else must be found and copied.
mapfile -t imports < <(objdump -p "$out/string_demo.exe" | sed -n 's/.*DLL Name: //p' | sort -u)
missing=()
for dll in "${imports[@]}"; do
    case "${dll,,}" in
        kernel32.dll|msvcrt.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|\
        oleaut32.dll|comdlg32.dll|winspool.drv|uuid.dll|ucrtbase.dll|ws2_32.dll|version.dll|\
        imm32.dll|setupapi.dll|winmm.dll|opengl32.dll|vulkan-1.dll) continue ;;   # OS / driver
    esac
    # Search the build tree first (SDL3), then the nix store (mcfgthread), then the slang wrap.
    src="$(find "$build" -name "$dll" -print -quit 2>/dev/null || true)"
    [ -n "$src" ] || src="$(find /nix/store -maxdepth 3 -name "$dll" -print -quit 2>/dev/null || true)"
    [ -n "$src" ] || src="$(find "$repo" -path '*slang*/bin/*' -name "$dll" -print -quit 2>/dev/null || true)"
    if [ -n "$src" ]; then cp -f "$src" "$out/"; else missing+=("$dll"); fi
done

# Slang dlopen()s siblings at RUNTIME, so they are invisible to the import table above. Only
# slang-compiler.dll is actually loaded (the other five - llvm, glslang, glsl-module, rt, gfx -
# are never touched; see docs/off-nix-build.md). Skipped entirely for a -Dslang=disabled build,
# which has no slang.dll import at all.
if [ -f "$out/slang.dll" ]; then
    sc="$(find "$repo" -path '*slang*/bin/slang-compiler.dll' -print -quit 2>/dev/null || true)"
    [ -n "$sc" ] && cp -f "$sc" "$out/" || missing+=("slang-compiler.dll")
fi

# --- shaders: sources AND the glslang-built .spv ---
for d in "$repo"/string-core/shaders "$repo"/string-render-forward/shaders; do
    [ -d "$d" ] && cp -f "$d"/*.slang "$out/shaders/" 2>/dev/null || true
done
find "$build" -name '*.spv' -exec cp -f {} "$out/shaders/" \;

# --- assets the engine loads unconditionally ---
cp -r "$repo/sandbox/assets/fonts" "$out/assets/"

# --- prebuilt shader cache (optional 3rd arg; REQUIRED for a -Dslang=disabled build) ---
cache_src="${3:-}"
if [ -n "$cache_src" ] && [ -d "$cache_src" ]; then
    mkdir -p "$out/shadercache"
    cp -f "$cache_src"/*.program "$out/shadercache/" 2>/dev/null || true
fi

cp -f "$repo/tools/package-readme.txt" "$out/README.txt" 2>/dev/null || true

# --- verify before declaring success ---
fail=0
for f in string_demo.exe assets/fonts/DejaVuSans.ttf; do
    [ -e "$out/$f" ] || { echo "MISSING: $f"; fail=1; }
done
# Every .spv OUR code opens by name must be present. FIRST-PARTY SOURCE ONLY: sandbox/subprojects
# holds vendored SDL3/KTX sample code that references skybox.vert.spv and friends we never build.
while read -r spv; do
    [ -e "$out/shaders/$spv" ] || { echo "MISSING shader: $spv"; fail=1; }
# Match the BASENAME anywhere, not anchored to a quote: the call sites write
# "shaders/debug_line.vert.spv", so a quote-anchored pattern matched nothing and the loop silently
# never ran — a check that cannot fail.
done < <(grep -rhoE '[a-z0-9_]+\.(vert|frag|comp)\.spv' \
             --exclude-dir=subprojects "$repo"/string-*/src "$repo"/sandbox 2>/dev/null \
         | sort -u)
[ "${#missing[@]}" -eq 0 ] || { echo "MISSING DLLs: ${missing[*]}"; fail=1; }
[ "$(ls "$out"/shaders/*.slang 2>/dev/null | wc -l)" -gt 0 ] || { echo "MISSING: shaders/*.slang"; fail=1; }

# A slang-ENABLED package with no cache still WORKS, but the first visit to each scene compiles its
# shaders on the main thread — seconds of frozen window, which on Windows looks like a hang (the
# compositor blanks a window that stops pumping messages). Ship a warm cache.
if [ -f "$out/slang.dll" ] && [ ! -d "$out/shadercache" ]; then
    echo "NOTE: no prebuilt shadercache/ — the first switch to each scene will freeze while its"
    echo "      shaders compile. Pass a warmed cache dir as arg 3."
fi

# A build with no slang.dll import is -Dslang=disabled: it CANNOT compile, so a package without a
# prebuilt cache is broken by construction and would fail on the first shader.
if [ ! -f "$out/slang.dll" ]; then
    n_cached="$(ls "$out"/shadercache/*.program 2>/dev/null | wc -l)"
    if [ "$n_cached" -eq 0 ]; then
        echo "MISSING: shadercache/ — this is a -Dslang=disabled build with no compiler, so it"
        echo "         cannot render anything without a prebuilt cache. Pass one as arg 3."
        fail=1
    else
        # Coverage is per-shader, and warming by running ONE scene only caches that scene's shaders.
        n_slang="$(ls "$out"/shaders/*.slang 2>/dev/null | wc -l)"
        echo "NOTE: $n_cached cached programs for $n_slang .slang sources. Any shader without an"
        echo "      entry is a HARD ERROR at runtime in this build — warm the cache across every"
        echo "      scene you ship, not just the default one (docs/off-nix-build.md)."
    fi
fi

[ "$fail" -eq 0 ] || { echo "PACKAGE INCOMPLETE"; exit 1; }
echo "packaged $out ($(du -sh "$out" | cut -f1))"
ls "$out"
