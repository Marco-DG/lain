#!/usr/bin/env bash
# judge_finder_refuted_paths.sh — a loop whose every trip the finder's solver refutes only through a
# fact the judge does not state for a measure is ranked, not left to the constant measure 0.
# for_signed_range_negative_index_fail.ln's `for i in -3..2` reads a[i] at -3: `a[i]` is stated as an
# element only at an index in bounds, the claims put i at -3, and the solver refuted the trip. The
# finder proposed 0, the judge was left 0 <= -1 to prove, and the loop, which ends, was a second
# error beside the true E085 (Documentation, 2026-10-26). This asserts the E085 and NO E011.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
t=tests/vra/bounds/for_signed_range_negative_index_fail.ln
out=$(cd "$ROOT" && ./lain "$t" -o /dev/null 2>&1)
echo "$out" | grep -q '\[E085\]' || { echo "$t: no E085: $out"; exit 1; }
if echo "$out" | grep -q '\[E011\]'; then echo "$t: an E011 for a loop that ends: $out"; exit 1; fi
exit 0
