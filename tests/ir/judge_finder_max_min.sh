#!/usr/bin/env bash
# judge_finder_max_min.sh — a ranking component that is the MAX or the MIN of rows (the judge's
# c36/c37). `while x != 0`, x stepping toward 0 from either side (CookSeeZuleger Fig8a), ranks by
# max(x, -x); Piecewise's p and q, the smaller one falling and the other set arbitrarily, by
# min(p, q). The finder tries both shapes over the versions that step by a constant, checking each
# on every back-edge path with the judge's own VC shapes before proposing it. Here the |x| loop is
# the pass program; its twin leaves x unchanged when negative and loops forever: E011. The min case
# is checked the same way in the second pair.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/pass.ln" <<'LN'
func task(x0 i32) effects raises {
    var x = x0
    while x != 0 {
        if x > 0 {
            x = x -? 1 else panic("overflow")
        } else {
            x = x +? 1 else panic("overflow")
        }
    }
}
func main() i32 effects raises {
    task(-3)
    return 0
}
LN
cat > "$T/fail.ln" <<'LN'
func task(x0 i32) effects raises {
    var x = x0
    while x != 0 {
        if x > 0 {
            x = x -? 1 else panic("overflow")
        } else {
            x = x
        }
    }
}
func main() i32 effects raises {
    task(-3)
    return 0
}
LN
out=$("$ROOT/lain" --judge=authority "$T/pass.ln" -o "$T/pass.c" 2>&1) \
    || { echo "the |x| loop is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/fail.ln" -o "$T/fail.c" 2>&1) \
    && { echo "an |x| loop that stalls below zero is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E011 " ] || { echo "an |x| loop that stalls below zero is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
cat > "$T/minp.ln" <<'LN'
extern func nondet() i32 effects io
func task(p0 i32, q0 i32) effects io, raises {
    var p = p0
    var q = q0
    while q > 0 and p > 0 and p != q {
        if q < p {
            q = q -? 1 else panic("overflow")
            p = nondet()
        } else {
            if p < q {
                p = p -? 1 else panic("overflow")
                q = nondet()
            }
        }
    }
}
func main() i32 effects io, raises {
    task(3, 5)
    return 0
}
LN
cat > "$T/minf.ln" <<'LN'
extern func nondet() i32 effects io
func task(p0 i32, q0 i32) effects io, raises {
    var p = p0
    var q = q0
    while q > 0 and p > 0 and p != q {
        if q < p {
            q = q
            p = nondet()
        } else {
            if p < q {
                p = p -? 1 else panic("overflow")
                q = nondet()
            }
        }
    }
}
func main() i32 effects io, raises {
    task(3, 5)
    return 0
}
LN
out=$("$ROOT/lain" --judge=authority "$T/minp.ln" -o "$T/minp.c" 2>&1) \
    || { echo "Piecewise's min(p, q) loop is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/minf.ln" -o "$T/minf.c" 2>&1) \
    && { echo "a min loop whose q path stalls is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E011 " ] || { echo "the stalling min loop is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
exit 0
