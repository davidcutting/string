#!/usr/bin/env bash
# Generate architecture diagrams with clang-uml + plantuml.
#
#   tools/uml.sh                 # every diagram in .clang-uml
#   tools/uml.sh geometry_scene  # just one (see .clang-uml for names)
#   tools/uml.sh --setup         # (re)configure the native build the tooling needs
#
# Output: docs/diagrams/<name>.puml + .svg
#
# TWO THINGS THIS SCRIPT EXISTS TO HANDLE:
#  1. compile_commands.json must come from a NATIVE build. The build-win-* dirs are mingw cross
#     builds; libclang cannot parse x86_64-w64-mingw32-g++ flags or find its headers. build-uml is a
#     throwaway configure-only meson dir (nothing is compiled) that exists purely for the DB.
#  2. clang-uml must run INSIDE `nix develop`. The devShell's clang wrapper supplies the system
#     include paths through env vars rather than the compile DB, so `query_driver: clang++` in
#     .clang-uml has to be able to find that exact driver — outside the shell it cannot, and every
#     TU fails with "'vector' file not found".
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

setup_db() {
    echo "==> configuring build-uml (compile_commands.json only; nothing is compiled)"
    rm -rf build-uml
    nix develop --command meson setup build-uml sandbox -Dstring-core:wsi=sdl >/dev/null

    # Normalize the symlinked subproject paths to real ones.
    #
    # meson sees each library through sandbox/subprojects/<lib>, which is a symlink to ../../<lib>.
    # clang-uml resolves that target one directory too high — it reports
    # /home/dcutting/code/cpp/sandbox/... (note: no "string/") — and the TU dies with a FATAL
    # "file not found" that aborts the ENTIRE run, not just that file. ui_pass.cpp was the casualty,
    # which silently removed the whole UI library from every diagram.
    #
    # From build-uml, ../sandbox/subprojects/<lib> and ../<lib> name the same directory, so
    # rewriting to the latter keeps the DB valid and sidesteps the symlink entirely.
    sed -i 's|\.\./sandbox/subprojects/string-|../string-|g' build-uml/compile_commands.json
    echo "    build-uml/compile_commands.json ready (subproject symlinks normalized)"
}

if [ "${1:-}" = "--setup" ]; then setup_db; exit 0; fi

# The DB goes stale when meson.build files change or a source file is added/removed.
if [ ! -f build-uml/compile_commands.json ]; then
    echo "==> build-uml/compile_commands.json missing"
    setup_db
fi

mkdir -p docs/diagrams

# --thread-count 1 is REQUIRED, not a tuning knob. Generating the diagrams in parallel makes
# render_passes fail with "due to following issues:" followed by an unrelated list of compiler
# warnings from another diagram's translation units — the diagnostics get attributed across threads.
# The same diagram builds fine on its own, and fine in a serial batch.
UML=(clang-uml -c .clang-uml --thread-count 1)

if [ $# -gt 0 ]; then
    echo "==> clang-uml: $*"
    nix develop --command "${UML[@]}" $(printf -- '-n %s ' "$@") 2>&1 | grep -vE "^\[info\].*Processing diagram" || true
    targets=("$@")
else
    echo "==> clang-uml: all diagrams (parses the render library serially — takes a couple of minutes)"
    nix develop --command "${UML[@]}" 2>&1 | grep -vE "^\[info\].*Processing diagram" || true
    targets=()
fi

echo "==> plantuml -> svg"
if [ ${#targets[@]} -gt 0 ]; then
    for t in "${targets[@]}"; do
        [ -f "docs/diagrams/$t.puml" ] && plantuml -tsvg "docs/diagrams/$t.puml"
    done
else
    plantuml -tsvg docs/diagrams/*.puml
fi

echo
echo "Diagrams in docs/diagrams:"
for f in docs/diagrams/*.svg; do
    [ -e "$f" ] || continue
    printf "  %-44s %s\n" "$f" "$(du -h "$f" | cut -f1)"
done
