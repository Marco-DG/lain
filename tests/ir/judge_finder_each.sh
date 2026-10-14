#!/usr/bin/env bash
# judge_finder_each.sh — every element of an array a loop fills, bounded at its header (an EACH
# claim): `a[i] = 25` over zeros keeps each(a) in [0, 25] (the bound iterated from the entry's 0 to
# the stored 25, which a trip keeps), so the loop that sums the array keeps `s <= 25*j` and five
# elements fit an i8. The summing loop must keep its ranking although the per-element claims ride
# into its paths (it once stopped on capacity: E011). Under --judge=authority it is accepted; with
# 26 (130 > 127) refused with E086 and nothing else.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/each.ln" <<'LN'
func main() i32 {
    var a i8[5] = [0, 0, 0, 0, 0]
    for i in 0..5 { a[i] = 25 }
    var s i8 = 0
    for v in a { s = s + v }
    return s as i32 - 125
}
LN
sed 's/a\[i\] = 25/a[i] = 26/; s/125/130/' "$T/each.ln" > "$T/each_fail.ln"
out=$("$ROOT/lain" --judge=authority "$T/each.ln" -o "$T/each.c" 2>&1) \
    || { echo "the filled array's sum is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/each_fail.ln" -o "$T/each_fail.c" 2>&1) \
    && { echo "a sum past 127 is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E086 " ] || { echo "the overflowing sum is refused with [$codes], not E086 alone:"; echo "$out" | head -8; exit 1; }
exit 0
