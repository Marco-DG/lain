#!/usr/bin/env bash
# judge_dump_measures_shape.sh — --dump-measures says the measure the judge proved, in its shape.
# A MAX group, `max(x, y)`, is one component: printed as `<x, y>` "lexicographically" it described
# a ranking this loop does not have (x may be negative while y >= 0). A WRITTEN measure is the one the
# judge's VCs prove: `fib ... decreasing n` printed the finder's `n + 2147483648` (Documentation,
# 2026-10-26).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
cat > "$D/maxg.ln" <<'LN'
func h(a i64, b i64) {
    var x = a
    var y = b
    while x >= 0 or y >= 0 {
        x = x - 1
        y = y - 1
    }
}
func main() i32 { h(3, 5); return 0 }
LN
cat > "$D/fib.ln" <<'LN'
func fib(n i32) i32 decreasing n {
    if n < 2 { return n }
    return fib(n - 1) +% fib(n - 2)
}
func main() i32 { return fib(10) }
LN
out=$(cd "$D" && "$ROOT/lain" maxg.ln --dump-measures -o maxg.c 2>&1)
echo "$out" | grep -qF '[measure] maxg_h: loop at line 4: `max(x, y)` decreases (the judge'"'"'s)' \
    || { echo "maxg: the MAX group is not printed as max(x, y):"; echo "$out" | grep measure; exit 1; }
out=$(cd "$D" && "$ROOT/lain" fib.ln --dump-measures -o fib.c 2>&1)
echo "$out" | grep -qF '[measure] fib_fib: recursion at line 3: `n` decreases at every call that may close the cycle (the judge'"'"'s)' \
    || { echo "fib: the written measure is not the one printed:"; echo "$out" | grep measure; exit 1; }
exit 0
