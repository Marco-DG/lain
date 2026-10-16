#!/usr/bin/env bash
# judge_finder_return_range.sh — a recursion whose measure rests on its OWN return values
# (NestedRecursion_1b/1c/1d of PE.11): rec1(i) is 0 for i <= 0 and 1 above, and calls
# rec1(rec1(rec1(i - 2) - 1)): the outer argument is rec1 of something at most 0, so 0, below i.
# The finder proposes the small-range claims 0 <= ret <= 1 and the threshold (2^31 + 1)*ret - i <=
# 2^31 (ret <= 0 where i <= 0) as fixed candidates for Houdini to keep together, and the measure i
# then falls at all three calls. Accepted under --judge=authority; the twin whose base case is i < 0
# recurses for ever from i = 0 (rec1(0) calls rec1(0) through rec1(-1) = 0), refused with E011.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
rec() {   # $1: rec1's base-case guard
cat <<LN
func rec1(i i32) i32 effects raises {
    if $1 {
        return 0
    }
    return rec1(rec1((rec1((i -? 2 else panic("overflow"))) -? 1 else panic("overflow")))) +? 1 else panic("overflow")
}
func rec2(j i32) i32 effects raises {
    if j <= 0 {
        return 0
    }
    return rec2(rec1((j -? 1 else panic("overflow")))) -? 1 else panic("overflow")
}
func main() i32 effects raises {
    return rec2(5)
}
LN
}
rec 'i <= 0' > "$T/pass.ln"
rec 'i < 0' > "$T/twin.ln"
out=$("$ROOT/lain" --judge=authority "$T/pass.ln" -o "$T/pass.c" 2>&1) \
    || { echo "NestedRecursion_1b is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/twin.ln" -o "$T/twin.c" 2>&1) \
    && { echo "the twin, which recurses for ever from i = 0, is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E011 " ] || { echo "the twin is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
exit 0
