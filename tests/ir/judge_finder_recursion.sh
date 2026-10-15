#!/usr/bin/env bash
# judge_finder_recursion.sh — measures of several rows for a recursion (the judge's c42/c43), each
# proposed by the finder only for a function whose component is itself alone:
# - RecursiveMultiplication: mult(n, -m) for m < 0, then mult(n, m - 1): regions m <= -1 (left for
#   m >= 1 at once, never back) and m >= 1 <m>;
# - TwoWay: `terminate` passed on unchanged picks n falling or n rising to the type's bound: three
#   regions by its sign, z >= 1 <n>, z <= -1 <n>, z = 0 <2^31 - 1 - n> (fd_rank_split);
# - LeeJonesBen-Amram POPL 2001 Ex5: f(y, y - 1) and f(y, x - 1) fall in max(2x, 2y + 1)
#   (fd_rank_affmax).
# Each is accepted under --judge=authority; each twin, which recurses for ever, refused with E011.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
mult() {   # $1: the argument for m < 0
cat <<LN
func mult(n i32, m i32) i32 effects raises {
    if m < 0 {
        return -1 *? mult(n, $1) else panic("overflow")
    }
    if m == 0 {
        return 0
    }
    return n +? mult(n, (m -? 1 else panic("overflow"))) else panic("overflow")
}
func main() i32 effects raises {
    return mult(3, -4)
}
LN
}
twoway() {   # $1: the step when terminate != 0
cat <<LN
func twoWay(terminate i32, n i32) i32 effects raises {
    if n < 0 {
        return 1
    } else {
        var m i32 = n
        if terminate != 0 {
            m = $1
        } else {
            m = m +? 1 else panic("overflow")
        }
        return m *? twoWay(terminate, m) else panic("overflow")
    }
}
func main() i32 effects raises {
    return twoWay(1, 5)
}
LN
}
ex5() {   # $1: the second argument when x != 0
cat <<LN
func f(x i32, y i32) i32 effects raises {
    if y == 0 {
        return x
    } else {
        if x == 0 {
            return f(y, (y -? 1 else panic("overflow")))
        } else {
            return f(y, $1)
        }
    }
}
func main() i32 effects raises {
    return f(3, 2)
}
LN
}
mult '(0 -? m else panic("overflow"))' > "$T/mult.ln"
mult 'm' > "$T/mult_twin.ln"
twoway 'm -? 1 else panic("overflow")' > "$T/twoway.ln"
twoway 'm' > "$T/twoway_twin.ln"
ex5 '(x -? 1 else panic("overflow"))' > "$T/ex5.ln"
ex5 'x' > "$T/ex5_twin.ln"
for p in mult twoway ex5; do
    out=$("$ROOT/lain" --judge=authority "$T/$p.ln" -o "$T/$p.c" 2>&1) \
        || { echo "$p is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
    out=$("$ROOT/lain" --judge=authority "$T/${p}_twin.ln" -o "$T/${p}_twin.c" 2>&1) \
        && { echo "the $p twin, which recurses for ever, is ACCEPTED under --judge=authority"; exit 1; }
    codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
    [ "$codes" = "E011 " ] || { echo "the $p twin is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
done
exit 0
