#!/usr/bin/env bash
# judge_finder_guarded.sh — the finder's GUARDED return claims (s7's form): at a call whose integer
# arguments are constants, the callee's least and greatest return under its parameters at those
# constants. `t[dbl(3)]` with `dbl(a) = a +% a` has no bound that holds for every a, and
# `t[pick(1, 2)]` none for max(a, b); with the guarded claims both index checks are proved under
# --judge=authority. The soundness pair keeps one guard from reaching another call: in
# callsite_return_range_fail.ln, `t[dbl(2)]` is proved and `t[dbl(k)]`, k anything, stays E085.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cd "$ROOT" || exit 1
out=$(./lain --judge=authority tests/vra/callsite_return_range_pass.ln -o "$T/p.c" 2>&1) \
    || { echo "callsite_return_range_pass is refused under --judge=authority:"; echo "$out" | head -6; exit 1; }
out=$(./lain --judge=authority tests/vra/callsite_return_range_fail.ln -o "$T/f.c" 2>&1) \
    && { echo "callsite_return_range_fail is ACCEPTED under --judge=authority"; exit 1; }
errs=$(echo "$out" | grep -oE '^\[E[0-9]+\] Error Ln [0-9]+' | tr '\n' ' ')
[ "$errs" = "[E085] Error Ln 18 " ] || { echo "callsite_return_range_fail: expected E085 at line 18 alone, got [$errs]"; echo "$out" | head -8; exit 1; }
exit 0
