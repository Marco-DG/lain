#!/usr/bin/env bash
# judge_dump_measures.sh — `--dump-measures` under --judge=authority prints the ranking the JUDGE
# certified, in the source's names, not the range analysis' search (which decides nothing in that
# mode, and printed "no measure found" for loops the judge had proved: Documentation, 2026-10-20).
# Dijkstra's gcd falls in x + y; the default mode still prints VRA's own line.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/gcd.ln" <<'LN'
func gcd(x0 u32, y0 u32) u32 {
    var x = x0
    var y = y0
    if x == 0 or y == 0 {
        return 0
    }
    while x != y {
        if x > y {
            x = x - y
        } else {
            y = y - x
        }
    }
    return x
}
func main() i32 {
    if gcd(12, 18) == 6 {
        return 0
    }
    return 1
}
LN
out=$(cd "$T" && "$ROOT/lain" --judge=authority --dump-measures gcd.ln -o gcd.c 2>&1) \
    || { echo "gcd is refused under --judge=authority:"; echo "$out" | head -6; exit 1; }
echo "$out" | grep -qF '[measure] gcd: loop at line 7: `x + y` decreases (the judge'"'"'s)' \
    || { echo "the judge's ranking is not printed:"; echo "$out" | grep measure; exit 1; }
echo "$out" | grep -q 'no measure found' && { echo "VRA's search is printed under authority:"; echo "$out" | grep measure; exit 1; }
out=$(cd "$T" && "$ROOT/lain" --dump-measures gcd.ln -o gcd.c 2>&1)
echo "$out" | grep -qE '^\[measure\] [a-z_]+: loop at line 7' || { echo "the default mode no longer prints VRA's measure:"; echo "$out" | head -4; exit 1; }
echo "$out" | grep -q "(the judge's)" && { echo "the default mode prints the judge's ranking"; exit 1; }
exit 0
