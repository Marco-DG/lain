#!/usr/bin/env bash
# `--dump-measures` prints the measure each termination rule FOUND, in the program's names: the
# first half of a termination certificate (local/internal/design/certificates.md, C.1), and the
# answer to "why does Lain think this loop ends?". Three canonical loops (counting up, counting
# down, a pair of endpoints), a field counter, an offset guard, and a recursion; each line exact.
# And a loop with none, named with its line beside its E011.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/m.ln" <<'LN'
type Lx {
    src u8[]
    pos usize
}
func up(n u32) u32 {
    var i u32 = 0
    while i < n {
        i = i + 1
    }
    return i
}
func down(n u32) u32 {
    var k u32 = n
    while k > 0 {
        k = k - 1
    }
    return k
}
func bsearch(a i32[], x i32) usize {
    var lo usize = 0
    var hi usize = a.len
    while lo < hi {
        mid = lo + (hi - lo) / 2
        if a[mid] < x { lo = mid + 1 } else { hi = mid }
    }
    return lo
}
func scan(l var Lx) {
    while l.pos < l.src.len {
        l.pos = l.pos + 1
    }
}
func pairs(a u8[]) usize {
    var i usize = 0
    while i + 1 < a.len {
        i = i + 1
    }
    return i
}
func fact(n u32) u32 {
    if n == 0 { return 1 }
    return fact(n - 1) +% n
}
func main() i32 {
    return 0
}
LN
"$LAIN" --dump-measures "$D/m.ln" -o "$D/m.c" > "$D/out" 2>&1 || { echo "lain refused the program"; cat "$D/out"; exit 1; }
cat > "$D/want" <<'WANT'
[measure] m_up: loop at line 7: `n - i` decreases: i rises by at least 1 each iteration and stays below n
[measure] m_down: loop at line 14: `k - 0` decreases: k falls by at least 1 each iteration and stays above 0
[measure] m_bsearch: loop at line 22: `hi - lo` decreases: each iteration raises lo or lowers hi, and the loop runs while lo < hi
[measure] m_scan: loop at line 29: `l.src.len - l.pos` decreases: l.pos rises by at least 1 each iteration and stays below l.src.len
[measure] m_pairs: loop at line 35: `a.len - (i + 1)` decreases: i + 1 rises by at least 1 each iteration and stays below a.len
[measure] m_fact: recursion at line 42: `n` decreases at every self-call and stays at least 0
WANT
grep '^\[measure\]' "$D/out" | diff "$D/want" - || { echo "the measures differ from the expected ones"; exit 1; }
# A loop with NO measure is named with its line, beside the E011 it explains: the counter moves
# on one path only, so no measure holds.
cat > "$D/n.ln" <<'LN'
func f(n u32) u32 {
    var i u32 = 0
    while i < n {
        if n > 5 { i = i + 1 }
    }
    return i
}
func main() i32 {
    return 0
}
LN
"$LAIN" --dump-measures "$D/n.ln" -o "$D/n.c" > "$D/nout" 2>&1 && { echo "the loop with no measure was accepted"; exit 1; }
grep -qxF '[measure] n_f: loop at line 3: no measure found' "$D/nout" || { echo "no 'no measure found' line for the loop"; cat "$D/nout"; exit 1; }
grep -q '\[E011\] Error Ln 3,' "$D/nout" || { echo "the E011 is not at the same line"; exit 1; }
exit 0
