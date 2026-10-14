#!/usr/bin/env bash
# judge_finder_guarded_case.sh — a sum over an array a comprehension filled. The fill keeps
# `each(a) <= 4 upto(i)`: each element below the fill's counter is bounded. The sum loop's header
# relates its own counter j to the finished fill's i (`j <= i`, `i + j >= 5`, ordinary candidates),
# and on the sum's paths the finder DECIDES each guarded instance: where the path's facts refute the
# guard case `i <= k`, the instance is its bound alone, the relation search reads the element's
# range, and `s <= 4*j` follows. The twin's elements reach 2e9: the sum passes i32, refused (E086).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/pass.ln" <<'LN'
func main() i32 {
    var a i32[5] = [i as i32 for i in 0..5]
    var s i32 = 0
    for v in a {
        s = s + v
    }
    return s - 10
}
LN
cat > "$T/fail.ln" <<'LN'
func main() i32 {
    var a i32[5] = [(i as i32) * 500000000 for i in 0..5]
    var s i32 = 0
    for v in a {
        s = s + v
    }
    return s - 10
}
LN
out=$("$ROOT/lain" --judge=authority "$T/pass.ln" -o "$T/pass.c" 2>&1) \
    || { echo "the comprehension's sum is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/fail.ln" -o "$T/fail.c" 2>&1) \
    && { echo "a sum past i32 is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E086 " ] || { echo "a sum past i32 is refused with [$codes], not E086 alone:"; echo "$out" | head -8; exit 1; }
exit 0
