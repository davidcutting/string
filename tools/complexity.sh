#!/usr/bin/env bash
# Cognitive-complexity report and RATCHET GATE.
#
#   tools/complexity.sh                      # whole engine, ranked worst-first
#   tools/complexity.sh string-render-forward     # one library / path fragment
#   tools/complexity.sh --raw                # per-construct breakdown (where the score accumulates)
#   tools/complexity.sh --baseline           # record docs/complexity-baseline.txt
#   tools/complexity.sh --gate               # fail if anything got worse   <-- the gate
#
# THE GATE'S RULE IS A ONE-WAY RATCHET: no tracked function may score higher than its baseline, and
# no NEW function may appear above the threshold. Complexity can fall freely; it cannot rise.
#
# Why a ratchet rather than a fixed ceiling: the worst function is currently 230, so any ceiling that
# passes today would bless everything below it. A ratchet makes the existing debt visible without
# blocking work, and makes each new increment a deliberate choice rather than an accident.
#
# Uses build-uml/compile_commands.json (see tools/uml.sh — the build-win-* dirs are mingw cross
# builds clang cannot parse). Runs inside `nix develop` for the system include paths.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

BASELINE="docs/complexity-baseline.txt"
THRESHOLD=25          # matches tools/complexity.clang-tidy

mode="report"
filter=""
for a in "$@"; do
    case "$a" in
        --raw)      mode="raw" ;;
        --baseline) mode="baseline" ;;
        --gate)     mode="gate" ;;
        *)          filter="$a" ;;
    esac
done

if [ ! -f build-uml/compile_commands.json ]; then
    echo "==> build-uml/compile_commands.json missing; running tools/uml.sh --setup"
    tools/uml.sh --setup
fi

# --- analyse ------------------------------------------------------------------------------------
# NOT run-clang-tidy: it is a Python script and the devShell has no python3. Driving clang-tidy
# directly over the TU list and parallelising with xargs -P gets the same result with one less
# dependency. The TU list comes from the compile DB via sed (jq is also absent).
out=$(mktemp); files=$(mktemp); cur=$(mktemp)
trap 'rm -f "$out" "$files" "$cur"' EXIT

sed -nE 's/^[[:space:]]*"file":[[:space:]]*"(.*)".*/\1/p' build-uml/compile_commands.json \
  | sed -E 's|^\.\./|'"$repo"'/|' \
  | { if [ -n "$filter" ]; then grep -- "$filter"; else cat; fi; } \
  | sort -u > "$files"

count=$(wc -l < "$files")
[ "$count" -gt 0 ] || { echo "no translation units matched '${filter}'"; exit 1; }
echo "==> analysing ${filter:-whole engine} ($count translation units) — expect a few minutes" >&2

nix develop --command bash -c '
    xargs -P "$(nproc)" -I{} clang-tidy --config-file=tools/complexity.clang-tidy \
        -p build-uml --quiet {} 2>/dev/null < '"$files"'
' > "$out" || true

if [ "$mode" = "raw" ]; then cat "$out"; exit 0; fi

# --- canonical form: "score<TAB>file::function" --------------------------------------------------
# Keyed on file+function, NOT file+line: line numbers shift on every edit, which would make the
# baseline churn constantly. Where one file has several same-named functions (widgets.cpp has four
# `emit`), the HIGHEST score for that name wins — stable, and it is the one that matters.
# third_party/ is analysed but never ranked or gated: vendored code we will not refactor.
grep -oE "[^ ]+:[0-9]+:[0-9]+: warning: function '[^']+' has cognitive complexity of [0-9]+" "$out" \
  | grep -v "third_party/" \
  | sed -E "s|^(.*):[0-9]+:[0-9]+: warning: function '([^']+)' has cognitive complexity of ([0-9]+)$|\3\t\1\t\2|" \
  | awk -F'\t' -v repo="$repo/" '{ i = index($2, repo); if (i) $2 = substr($2, i + length(repo));
                                   sub(/^\.\.\//, "", $2);
                                   key = $2 "::" $3;
                                   if ($1 > best[key]) best[key] = $1 }
                                 END { for (k in best) printf "%d\t%s\n", best[k], k }' \
  | sort -rn -k1,1 -k2,2 > "$cur"

case "$mode" in
  report)
    printf "\n%6s  %s\n%6s  %s\n" "SCORE" "FUNCTION" "-----" "--------"
    awk -F'\t' '{ printf "%6d  %s\n", $1, $2 }' "$cur" | head -40
    printf "\n%s functions over threshold %s. Baseline: %s\n" "$(wc -l < "$cur")" "$THRESHOLD" \
        "$([ -f "$BASELINE" ] && echo "$BASELINE" || echo "(none — run --baseline)")"
    ;;

  baseline)
    [ -n "$filter" ] && { echo "refusing: --baseline must cover the WHOLE engine (drop '$filter')"; exit 1; }
    { echo "# Cognitive-complexity baseline for tools/complexity.sh --gate"
      echo "# Regenerate ONLY when intentionally accepting new debt: tools/complexity.sh --baseline"
      echo "# Format: <score>\t<file>::<function>   (threshold $THRESHOLD, third_party excluded)"
      echo "# Recorded: $(git rev-parse --short HEAD 2>/dev/null || echo unknown) (working tree may differ)"
      cat "$cur"
    } > "$BASELINE"
    echo "wrote $BASELINE ($(wc -l < "$cur") functions)"
    ;;

  gate)
    [ -f "$BASELINE" ] || { echo "no $BASELINE — run: tools/complexity.sh --baseline"; exit 2; }
    [ -n "$filter" ] && { echo "refusing: --gate must cover the WHOLE engine (drop '$filter')"; exit 1; }
    grep -v '^#' "$BASELINE" | grep -v '^$' > "$cur.base"

    fail=0
    # Regressions: a tracked function scoring higher than baseline.
    while IFS=$'\t' read -r score key; do
        base=$(awk -F'\t' -v k="$key" '$2 == k { print $1 }' "$cur.base" | head -1)
        if [ -n "$base" ] && [ "$score" -gt "$base" ]; then
            printf "  REGRESSION  %-58s %s -> %s\n" "$key" "$base" "$score"; fail=1
        elif [ -z "$base" ]; then
            printf "  NEW         %-58s %s (threshold %s)\n" "$key" "$score" "$THRESHOLD"; fail=1
        fi
    done < "$cur"
    rm -f "$cur.base"

    if [ "$fail" -eq 0 ]; then
        improved=$(awk -F'\t' 'NR==FNR { b[$2]=$1; next } ($2 in b) && $1 < b[$2] { n++ } END { print n+0 }' \
                   <(grep -v '^#' "$BASELINE" | grep -v '^$') "$cur")
        echo "COMPLEXITY GATE: PASS ($(wc -l < "$cur") functions tracked, $improved improved)"
        exit 0
    fi
    echo
    echo "COMPLEXITY GATE: FAIL"
    echo "Either simplify, or accept the debt deliberately with: tools/complexity.sh --baseline"
    exit 1
    ;;
esac
