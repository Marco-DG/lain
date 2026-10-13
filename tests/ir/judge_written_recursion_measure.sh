#!/usr/bin/env bash
# judge_written_recursion_measure.sh — I.206 under --judge=authority: a function's WRITTEN `decreasing`
# is what its recursion owes (the generator reads the ghost IrFunc.measure_val), so:
#   1. `decreasing k` with k passed on unchanged is refused, E082, saying the measure is not smaller
#      at the call (it was accepted for as long as the engine ranked by a parameter it found, n here);
#   2. `decreasing k` counting k down is accepted;
#   3. `decreasing hi - lo` on u64, whose base case `lo >= hi` returns first, is accepted: the
#      measure owes well-definedness where the recursion happens, past the guard, not at the entry.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cd "$T" || exit 1
A="--judge=authority"
printf 'func f(n i32, k i32 >= 0) i32 decreasing k {\n    if n <= 0 { return 0 }\n    return f(n - 1, k)\n}\nfunc main() i32 {\n    a = f(3, 1)\n    return 0\n}\n' > bad.ln
out=$("$ROOT/lain" bad.ln $A -o /dev/null 2>&1); rc=$?
[ $rc -eq 1 ] && echo "$out" | grep -q '^\[E082\] .*the `decreasing` measure written is not provably smaller at this call' \
    || { echo "1: an unchanged written measure not refused with E082 ($rc): $out"; exit 1; }
printf 'func down(k i32 >= 0) i32 decreasing k {\n    if k == 0 { return 0 }\n    return down(k - 1)\n}\nfunc main() i32 {\n    b = down(3)\n    return 0\n}\n' > good.ln
out=$("$ROOT/lain" good.ln $A -o /dev/null 2>&1) || { echo "2: a falling written measure refused: $out"; exit 1; }
printf 'func span(lo u64, hi u64) u64 decreasing hi - lo {\n    if lo >= hi { return 0 }\n    return span(lo + 1, hi)\n}\nfunc main() i32 {\n    b = span(1, 4)\n    return 0\n}\n' > span.ln
out=$("$ROOT/lain" span.ln $A -o /dev/null 2>&1) || { echo "3: hi - lo past its guard refused: $out"; exit 1; }
exit 0
