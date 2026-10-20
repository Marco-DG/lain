#!/usr/bin/env bash
# vra_measure_inclusive.sh — the default mode's --dump-measures text follows the guard: `while i <= n`
# lets i reach n, so i "stays at most n", not "below n"; `while k >= 0`, k "stays at least 0"
# (Documentation's engines experiment, turing1949_factorial's `while s <= r`). A strict guard keeps
# "below" and "above" (tests/codegen/dump_measures.sh).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/le.ln" <<'LN'
func up(n u32) u32 {
    var i u32 = 0
    while i <= n {
        if i == 4000000000 { return i }
        i = i + 1
    }
    return i
}
func down(n i32) i32 {
    var k i32 = n
    while k >= 0 {
        k = k - 1
    }
    return k
}
func main() i32 {
    return 0
}
LN
out=$(cd "$T" && "$ROOT/lain" --dump-measures le.ln -o le.c 2>&1)
echo "$out" | grep -qF 'i rises by at least 1 each iteration and stays at most n' || { echo "the <= loop's text:"; echo "$out" | grep measure; exit 1; }
echo "$out" | grep -qF 'k falls by at least 1 each iteration and stays at least 0' || { echo "the >= loop's text:"; echo "$out" | grep measure; exit 1; }
exit 0
