#!/usr/bin/env bash
# judge_finder_split_loop.sh — a loop ranked by the sign of a value it only reads (the PE.11 ports
# Toulouse-BranchesToLoop-2 and Toulouse-MultiBranchesToLoop-2): x is 1 or -1 (then stepped away
# from 0 eight times) before the loop, and `y = y + x; z = z - x` falls in 99 - y where x >= 1 and in
# 99 - z where x <= -1; no linear ranking fits both. The finder splits on x's sign once every back
# edge, asked with its join facts whole (the flattening drops some of MultiBranches' nine), rules
# out x = 0; the regions are what c38 owes. Both accepted under --judge=authority; each twin, whose
# else arm sets x to 0, loops for ever there and is refused with E011 alone.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
branches() {   # $1: the else arm's x
cat <<LN
extern func nondet() i32 effects io
func task(nd0 i32, nd1 i32) effects io, raises {
    var x i32 = 0
    var y i32 = nd0
    var z i32 = nd1
    if !(-268435455 <= y) {
        return
    }
    if !(-268435455 <= z) {
        return
    }
    if nondet() != 0 {
        x = 1
    } else {
        x = $1
    }
    while (y < 100) and (z < 100) {
        y = y +? x else panic("overflow")
        z = z -? x else panic("overflow")
    }
}
LN
}
multi() {   # $1: the last step's else arm
cat <<LN
extern func nondet() i32 effects io
func task(nd0 i32, nd1 i32) effects io, raises {
    var x i32 = 0
    var y i32 = nd0
    var z i32 = nd1
    if !(-268435455 <= y) {
        return
    }
    if !(-268435455 <= z) {
        return
    }
    if nondet() != 0 {
        x = 1
    } else {
        x = -1
    }
LN
for k in 1 2 3 4 5 6 7; do cat <<LN
    if x > 0 {
        x = x +? 1 else panic("overflow")
    } else {
        x = x -? 1 else panic("overflow")
    }
LN
done
cat <<LN
    if x > 0 {
        x = x +? 1 else panic("overflow")
    } else {
        $1
    }
    while (y < 100) and (z < 100) {
        y = y +? x else panic("overflow")
        z = z -? x else panic("overflow")
    }
}
LN
}
branches '-1' > "$T/b.ln"; branches '0' > "$T/b_twin.ln"
multi 'x = x -? 1 else panic("overflow")' > "$T/m.ln"; multi 'x = 0' > "$T/m_twin.ln"
for p in b m; do
    out=$("$ROOT/lain" --judge=authority "$T/$p.ln" -o "$T/$p.c" 2>&1) \
        || { echo "$p is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
    out=$("$ROOT/lain" --judge=authority "$T/${p}_twin.ln" -o "$T/${p}_twin.c" 2>&1) \
        && { echo "the $p twin, which loops for ever at x = 0, is ACCEPTED under --judge=authority"; exit 1; }
    codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
    [ "$codes" = "E011 " ] || { echo "the $p twin is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
done
exit 0
