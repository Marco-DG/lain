#!/usr/bin/env bash
# judge_finder_affine_equality.sh — an affine EQUALITY a loop keeps (Karr), the one its ranking needs
# (SV-COMP MenloPark): `x -= y; y -= z; z = -z` from y = 100, z = 1 keeps 2y - z = 199, so with
# -1 <= z <= 1 y stays in [99, 100] and x falls by at least 99 a trip. The finder row-reduces each
# back-edge path's definitions to new = affine(old), solves for the vectors c with c.(new - old) = 0
# on every path, and proposes c.x at its value on entry. The twin starts at y = 0, z = 0: x never
# moves, and the loop runs forever: E011.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/pass.ln" <<'LN'
func task(x0 i32) effects raises {
    var x = x0
    var y i32 = 100
    var z i32 = 1
    while x >= 0 {
        x = x -? y else panic("overflow")
        y = y -? z else panic("overflow")
        z = 0 -? z else panic("overflow")
    }
}
func main() i32 effects raises {
    task(1000)
    return 0
}
LN
cat > "$T/fail.ln" <<'LN'
func task(x0 i32) effects raises {
    var x = x0
    var y i32 = 0
    var z i32 = 0
    while x >= 0 {
        x = x -? y else panic("overflow")
        y = y -? z else panic("overflow")
        z = 0 -? z else panic("overflow")
    }
}
func main() i32 effects raises {
    task(1000)
    return 0
}
LN
out=$("$ROOT/lain" --judge=authority "$T/pass.ln" -o "$T/pass.c" 2>&1) \
    || { echo "the MenloPark loop is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/fail.ln" -o "$T/fail.c" 2>&1) \
    && { echo "a loop whose x never moves is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E011 " ] || { echo "a loop whose x never moves is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
exit 0
