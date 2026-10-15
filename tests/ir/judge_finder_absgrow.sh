#!/usr/bin/env bash
# judge_finder_absgrow.sh — a ranking whose first component is a MIN of sign rows on a free edge
# (the judge's c41): the PE.11 port of 4NestedWith3Variables, (a, b) <- (3a - 4b, 4a + 3b), which
# multiplies |a| + |b| by at least 3.5 unless a = b = 0, where q = q + a - 1 falls. The finder
# proposes <min(C - a - b, C - a + b, C + a - b, C + a + b), the rest>, C a bound on |a| + |b| read
# from the types, after checking each trip per sign case: |a| + |b| rises by one, or stays and the
# rest (the finder's own search over those cases) falls. The port is refused without it. Two twins
# that run for ever are refused with E011 alone: 'swap' moves (a, b) to (b, a), keeping |a| + |b| and,
# from a = b = 1, q; 'same' leaves (a, b), and at a = 1 q too.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
nest() {   # $1: a's new value, $2: b's (olda holds a's old value)
cat <<LN
func task(nd0 i32, nd1 i32, nd2 i32) i32 effects raises {
    var a i32
    var b i32
    var q i32
    var olda i32
    q = nd0
    a = nd1
    b = nd2
    if !((-524287 <= q) and (q <= 524287)) {
        return 0
    }
    if !((-524287 <= a) and (a <= 524287)) {
        return 0
    }
    if !((-524287 <= b) and (b <= 524287)) {
        return 0
    }
    while q > 0 {
        q = (q +? a else panic("overflow")) -? 1 else panic("overflow")
        olda = a
        a = $1
        b = $2
    }
    return 0
}
func main() i32 effects raises {
    task(3, 0, 0)
    return 0
}
LN
}
nest '(3 *? olda else panic("overflow")) -? (4 *? b else panic("overflow")) else panic("overflow")' '(4 *? olda else panic("overflow")) +? (3 *? b else panic("overflow")) else panic("overflow")' > "$T/pass.ln"
nest 'b' 'olda' > "$T/swap.ln"
nest 'olda' 'b' > "$T/same.ln"
out=$("$ROOT/lain" --judge=authority "$T/pass.ln" -o "$T/pass.c" 2>&1) \
    || { echo "4NestedWith3Variables is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
for tw in swap same; do
    out=$("$ROOT/lain" --judge=authority "$T/$tw.ln" -o "$T/$tw.c" 2>&1) \
        && { echo "the twin '$tw', which can run for ever, is ACCEPTED under --judge=authority"; exit 1; }
    codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
    [ "$codes" = "E011 " ] || { echo "the twin '$tw' is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
done
exit 0
