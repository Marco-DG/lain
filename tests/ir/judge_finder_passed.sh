#!/usr/bin/env bash
# judge_finder_passed.sh — a value one pair has before the trip that is ANOTHER pair's after it: a
# recursive call passing a parameter on as a different argument (LeeJonesBen-Amram POPL 2001 Ex4,
# p(m, r - 1, n) and p(r, n - 1, m): m + n + r falls by one at both calls, bounded by the types), or
# a loop's `x = y`. The finder's path once gave such a value to whichever pair came last, so one side
# lost it and no measure was found. Ex4, Parts (p + q at three calls; parts(p, p) makes p's value
# one pair's pre and two posts) and dup (g(a - 1, a - 1): one argument both pairs' post; the finder
# before this proved it another way, so it pins the shape, not the fix: Ex4 and Parts do that) must be
# accepted under --judge=authority; each twin,
# whose call passes its parameters on unchanged and so recurses for ever, refused with its code alone.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
ex4() {   # $1: call A's middle argument
cat <<LN
func p(m i32, n i32, r i32) i32 effects raises {
    if r > 0 {
        return p(m, $1, n)
    } else {
        if n > 0 {
            return p(r, (n -? 1 else panic("overflow")), m)
        } else {
            return m
        }
    }
}
func main() i32 effects raises {
    return p(1, 2, 3)
}
LN
}
parts() {   # $1: the call for q > p
cat <<LN
func parts(p i32, q i32) i32 effects raises {
    if p <= 0 {
        return 1
    } else {
        if q <= 0 {
            return 0
        } else {
            if q > p {
                return $1
            } else {
                return parts((p -? q else panic("overflow")), q) +? parts(p, (q -? 1 else panic("overflow"))) else panic("overflow")
            }
        }
    }
}
func main() i32 effects raises {
    return parts(4, 3)
}
LN
}
dup() {   # $1: the call's argument, both places
cat <<LN
func g(a i32, b i32) i32 effects raises {
    if a > 0 {
        return g($1, $1)
    }
    return b
}
func main() i32 effects raises {
    return g(3, 7)
}
LN
}
dup '(a -? 1 else panic("overflow"))' > "$T/dup.ln"
dup 'a' > "$T/dup_twin.ln"
ex4 '(r -? 1 else panic("overflow"))' > "$T/ex4.ln"
ex4 'r' > "$T/ex4_twin.ln"
parts 'parts(p, p)' > "$T/parts.ln"
parts 'parts(p, q)' > "$T/parts_twin.ln"
for p in ex4 parts dup; do
    out=$("$ROOT/lain" --judge=authority "$T/$p.ln" -o "$T/$p.c" 2>&1) \
        || { echo "$p is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
    out=$("$ROOT/lain" --judge=authority "$T/${p}_twin.ln" -o "$T/${p}_twin.c" 2>&1) \
        && { echo "the $p twin, which recurses for ever, is ACCEPTED under --judge=authority"; exit 1; }
    codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
    [ "$codes" = "E011 " ] || { echo "the $p twin is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
done
exit 0
