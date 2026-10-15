#!/usr/bin/env bash
# judge_finder_regions.sh — a PIECEWISE ranking with regions (the judge's c38). UrbanMine-ESOP2014
# Fig3: in x >= 1 the pair <x, y> falls, in x <= -1 the pair <y, -x>, and one path may jump from
# x <= -1 to x >= 1, never back. No tuple, max or min ranks the whole loop; the finder proposes two
# regions, each guarded by a sign case the paths state, after asking every obligation the judge will
# owe (coverage, no move up, each region's tuple) on the flattened paths. Two twins that run for
# ever must be refused with E011 alone: one whose x >= 1 path steps by 2, so x = 1 jumps to -1 and
# the other path sets x back to 1 (a move up), and one whose x <= -1 path leaves x unchanged. A
# third, the GAP: the first side starts at x > 1 and x == 1 takes a branch that changes nothing, so
# the only regions that rank the two sides, x >= 2 and x <= -1, leave x = 1 uncovered (rank.cover).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
fig3() {   # $1: the x >= 1 path's step on x, $2: the x <= -1 path's update of x
cat <<LN
extern func nondet() i32 effects io
func task(x0 i32, y0 i32) effects io, raises {
    var x = x0
    var y = y0
    while (x != 0) and (y > 0) {
        if x > 0 {
            if nondet() != 0 {
                x = x -? $1 else panic("overflow")
                y = nondet()
            } else {
                y = y -? 1 else panic("overflow")
            }
        } else {
            if nondet() != 0 {
                $2
            } else {
                y = y -? 1 else panic("overflow")
                x = nondet()
            }
        }
    }
}
func main() i32 effects io, raises {
    task(-3, 5)
    return 0
}
LN
}
fig3 1 'x = x +? 1 else panic("overflow")' > "$T/pass.ln"
fig3 2 'x = x +? 1 else panic("overflow")' > "$T/up.ln"
fig3 1 'x = x' > "$T/stall.ln"
cat > "$T/gap.ln" <<'LN'
extern func nondet() i32 effects io
func task(x0 i32, y0 i32) effects io, raises {
    var x = x0
    var y = y0
    while (x != 0) and (y > 0) {
        if x > 1 {
            if nondet() != 0 {
                x = x -? 1 else panic("overflow")
                y = nondet()
            } else {
                y = y -? 1 else panic("overflow")
            }
        } else {
            if x < 0 {
                if nondet() != 0 {
                    x = x +? 1 else panic("overflow")
                } else {
                    y = y -? 1 else panic("overflow")
                    x = nondet()
                }
            } else {
                y = y
            }
        }
    }
}
func main() i32 effects io, raises {
    task(-3, 5)
    return 0
}
LN
out=$("$ROOT/lain" --judge=authority "$T/pass.ln" -o "$T/pass.c" 2>&1) \
    || { echo "Fig3 is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
for tw in up stall gap; do
    out=$("$ROOT/lain" --judge=authority "$T/$tw.ln" -o "$T/$tw.c" 2>&1) \
        && { echo "Fig3's twin '$tw', which can run for ever, is ACCEPTED under --judge=authority"; exit 1; }
    codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
    [ "$codes" = "E011 " ] || { echo "Fig3's twin '$tw' is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
done
exit 0
