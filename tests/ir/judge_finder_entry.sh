#!/usr/bin/env bash
# judge_finder_entry.sh — loop facts relative to a loop's ENTRY (the judge's c40 term entry(x): a
# cell's value on the loop's entry edge, fixed while it runs). Two PE.11 ports the finder now ranks:
# - nestedLoop-1 (AliasDarteFeautrierGonnord SAS2010): the middle loop sets i = k, and the loop in
#   it keeps k >= i, so i never falls there: `i - entry(i) >= 0` at the middle header, whence the
#   outer `i = i + 1` lowers n - i. Its twin sets i = k - 1, which falls when the inner loop does
#   not run, and loops for ever from m >= 2.
# - LarrazOliveras Fig1 (FMCAD 2013): the inner loop moves x up and z down together, so it keeps
#   x + z = entry(x) + entry(z) (Karr's equality, relative to the entry), and the outer x = x - 1
#   lowers x + z, bounded below by the types. Its twin drops x = x - 1, and from x = 0 the outer
#   loop keeps y for ever.
# Each port is accepted under --judge=authority; each twin refused with E011 alone.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
nested() {   # $1: what the middle loop assigns to i
cat <<LN
func task(n i32, m i32, N i32) effects raises {
    var i i32 = 0
    var j i32 = 0
    var k i32 = 0
    if (0 <= n) and (0 <= m) and (0 <= N) {
        while i < n {
            j = 0
            while j < m {
                j = j +? 1 else panic("overflow")
                k = i
                while k < (N -? 1 else panic("overflow")) {
                    k = k +? 1 else panic("overflow")
                }
                i = $1
            }
            i = i +? 1 else panic("overflow")
        }
    }
}
func main() i32 effects raises {
    task(3, 2, 1)
    return 0
}
LN
}
larraz() {   # $1: the outer loop's first statement
cat <<LN
func task(x0 i32, y0 i32, z0 i32) effects raises {
    var x = x0
    var y = y0
    var z = z0
    if (x <= 10000) and (x >= -10000) and (y <= 10000) and (z <= 10000) {
        while y >= 1 {
            $1
            while y < z {
                x = x +? 1 else panic("overflow")
                z = z -? 1 else panic("overflow")
            }
            y = x +? y else panic("overflow")
        }
    }
}
func main() i32 effects raises {
    task(0, 5, 0)
    return 0
}
LN
}
nested 'k' > "$T/nested.ln"
nested 'k -? 1 else panic("overflow")' > "$T/nested_twin.ln"
larraz 'x = x -? 1 else panic("overflow")' > "$T/larraz.ln"
larraz 'x = x' > "$T/larraz_twin.ln"
for p in nested larraz; do
    out=$("$ROOT/lain" --judge=authority "$T/$p.ln" -o "$T/$p.c" 2>&1) \
        || { echo "the $p port is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
    out=$("$ROOT/lain" --judge=authority "$T/${p}_twin.ln" -o "$T/${p}_twin.c" 2>&1) \
        && { echo "the $p twin, which can run for ever, is ACCEPTED under --judge=authority"; exit 1; }
    codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
    [ "$codes" = "E011 " ] || { echo "the $p twin is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
done
exit 0
