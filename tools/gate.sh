#!/usr/bin/env bash
# Phase-2 byte-parity gate: capture frame-300 exterior and diff against the committed baseline.
# AE=0 => byte-identical. Usage: tools/gate.sh [label]
set -euo pipefail
label="${1:-gate}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="/tmp/gp_refactor/p2/${label}_ext.png"
"$repo/tools/capture.sh" "$out" "35,30,25,-2.52,-0.51" >/dev/null 2>&1 || true
if [ ! -f "$out" ]; then echo "NO CAPTURE ($label) — see ${out}.log"; tail -20 "${out}.log"; exit 2; fi
nix-shell -p imagemagick --run \
  "compare -metric AE /tmp/gp_refactor/p2/base_exterior.png '$out' null:" 2>&1 | tail -1
echo " <- AE for '$label' (expect 0)"
