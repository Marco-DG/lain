#!/usr/bin/env bash
# judge_finder_entry_bound.sh — a bound a loop's own step keeps from its entry value (the PE.11 port
# HeizmannHoenickeLeikePodelski-ATVA2013-Fig5): y = 2 before the loop, `y = (y + 1) / 2` keeps
# y >= 1, and `x = x - y` then falls. No comparison in the program states y >= 1: the finder proposes
# c - 1 <= y <= c + 1 around a version's one constant entry value c, and Houdini keeps the inductive
# side. Accepted under --judge=authority; its twin, y = 0 before the loop, keeps y = 0 and x never
# moves: refused with E011 alone.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
prog() {   # $1: y's value before the loop
cat <<LN
func task(nd0 i32) i32 effects raises {
    var x i32 = nd0
    var y i32 = $1
    while x >= 0 {
        x = x -? y else panic("overflow")
        y = (y +? 1 else panic("overflow")) / 2
    }
    return 0
}
LN
}
prog 2 > "$T/f.ln"; prog 0 > "$T/f_twin.ln"
out=$("$ROOT/lain" --judge=authority "$T/f.ln" -o "$T/f.c" 2>&1) \
    || { echo "Fig5 is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/f_twin.ln" -o "$T/f_twin.c" 2>&1) \
    && { echo "the twin, which loops for ever at y = 0, is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E011 " ] || { echo "the twin is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
exit 0
