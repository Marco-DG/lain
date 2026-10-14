#!/usr/bin/env bash
# judge_finder_each_upto.sh — a loop that fills storage with no value yet (`var a i32[8]`): no bound
# holds for every element on entry, but each element BELOW the counter is 7 once the loop has passed
# it, `each(a) == 7 upto(k)` (a guarded EACH claim: per element k, `k' >= k + 1` or the bound). The
# finder iterates the bound from the first trip (on entry every instance is off), and after the
# loop k = 8 covers a[7]. Under --judge=authority `a[7] - 7` is accepted; with -2147483642 stored,
# `a[7] - 7` passes i32's minimum and is refused with E086 and nothing else.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/fill.ln" <<'LN'
func main() i32 {
    var a i32[8]
    for k in 0..8 { a[k] = 7 }
    return a[7] - 7
}
LN
sed 's/a\[k\] = 7 /a[k] = -2147483642 /' "$T/fill.ln" > "$T/fill_fail.ln"
grep -q 2147483642 "$T/fill_fail.ln" || { echo "the twin was not made"; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/fill.ln" -o "$T/fill.c" 2>&1) \
    || { echo "the filled array is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/fill_fail.ln" -o "$T/fill_fail.c" 2>&1) \
    && { echo "a[7] - 7 below i32's minimum is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E086 " ] || { echo "the overflowing read is refused with [$codes], not E086 alone:"; echo "$out" | head -8; exit 1; }
exit 0
