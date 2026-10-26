#!/usr/bin/env bash
# judge_vc_module_boundary.sh — judge_vc_driver keeps the module boundary as the compiler does
# (I.181). The driver left every IrFunc.src_file NULL, so a function of an imported module counted
# as the main file's own, the judge used facts across the boundary, and on
# constant_argument_range_stays_in_module_fail.ln it proved all 18 obligations where the compiler
# refuses three. judge_gate builds this driver: it checked a configuration the compiler does not
# compile (Documentation, 2026-10-26).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
gcc -std=c99 -O2 -w -o "$D/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "the driver does not build"; exit 1; }
out=$(cd "$ROOT" && "$D/jvc" tests/modules/constant_argument_range_stays_in_module_fail.ln --judge 2>&1)
line=$(echo "$out" | grep '^# judge')
[ -n "$line" ] || { echo "no judge summary:"; echo "$out" | tail -3; exit 1; }
echo "$line" | grep -q ' not-implied=0 ' && { echo "the driver proves across the module boundary: $line"; exit 1; }
exit 0
