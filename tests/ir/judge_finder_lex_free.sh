#!/usr/bin/env bash
# judge_finder_lex_free.sh — a lexicographic ranking whose falling component differs by PATH of one
# back edge (CookSeeZuleger TACAS 2013 Fig7b): <y, z, x>, each path lowering one counter and setting
# another arbitrarily. The finder leaves the edge's component free (fall -1) and the judge owes the
# lexicographic decrease itself (rank.lex). Two finder traps on the way: under i32 the tuple also
# exists linearized by powers of two (about 2^63*y + 2^32*z + x), found first and past what a
# ranking claim states, so the search retries with bounded coefficients; and only the counters, not
# the loop's nondet temporaries, together. The twin's last path lowers nothing: E011.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/pass.ln" <<'LN'
extern func nondet() i32 effects io
func task(x0 i32, y0 i32, z0 i32) effects io, raises {
    var x = x0
    var y = y0
    var z = z0
    while x > 0 and y > 0 and z > 0 {
        if nondet() != 0 {
            x = x -? 1 else panic("overflow")
        } else {
            if nondet() != 0 {
                y = y -? 1 else panic("overflow")
                z = nondet()
            } else {
                z = z -? 1 else panic("overflow")
                x = nondet()
            }
        }
    }
}
func main() i32 effects io, raises {
    task(3, 2, 1)
    return 0
}
LN
cat > "$T/fail.ln" <<'LN'
extern func nondet() i32 effects io
func task(x0 i32, y0 i32, z0 i32) effects io, raises {
    var x = x0
    var y = y0
    var z = z0
    while x > 0 and y > 0 and z > 0 {
        if nondet() != 0 {
            x = x -? 1 else panic("overflow")
        } else {
            if nondet() != 0 {
                y = y -? 1 else panic("overflow")
                z = nondet()
            } else {
                z = z
                x = nondet()
            }
        }
    }
}
func main() i32 effects io, raises {
    task(3, 2, 1)
    return 0
}
LN
out=$("$ROOT/lain" --judge=authority "$T/pass.ln" -o "$T/pass.c" 2>&1) \
    || { echo "the Fig7b loop is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/fail.ln" -o "$T/fail.c" 2>&1) \
    && { echo "a loop whose third path lowers nothing is ACCEPTED under --judge=authority"; exit 1; }
codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
[ "$codes" = "E011 " ] || { echo "a loop whose third path lowers nothing is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
exit 0
