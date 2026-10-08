#!/usr/bin/env bash
# The accumulator explanation ("this running total can overflow ... bound the count, the element,
# or the total") is for a RUNNING TOTAL: a value stored back into the cell it read, `s = s + d`.
# It was printed for any loop-carried `x + d` whose check failed — here the index `i + w - 1` of a
# sliding window, whose missing fact is a relation between i, w and a.len, not a bound on a sum —
# sending the reader to change the wrong thing (O-5 in the Octagon session's ISSUES.md). The
# window walks `i < a.len`: under `i <= a.len - w` it is proven since I.144, and `i + w` here can
# leave u64 (two lengths' worth), so the add still fails and the wording is still asked.
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
LAIN="$ROOT/lain"
[ -x "$LAIN" ] || { echo "no compiler at $LAIN"; exit 1; }
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/window.ln" <<'LN'
func f(a i32[], w usize) i32 {
    if w == 0 or w > a.len { return 0 }
    var s i32 = 0
    var i usize = 0
    while i < a.len {
        s = s +% (a[i + w - 1] -% a[i])
        i = i + 1
    }
    return s
}
func main() i32 { return 0 }
LN
cat > "$D/total.ln" <<'LN'
func total(a u8[], n usize) u8 {
    var s u8 = 0
    var i usize = 0
    while i < n and i < a.len {
        s = s + a[i]
        i = i + 1
    }
    return s
}
func main() i32 { return 0 }
LN
"$LAIN" "$D/window.ln" -o "$D/w.c" > "$D/w.err" 2>&1 && { echo "the window compiled"; exit 1; }
grep -q 'E086' "$D/w.err" || { echo "the window is not E086"; exit 1; }
grep -q 'running total' "$D/w.err" && { echo "the window was explained as a running total"; exit 1; }
"$LAIN" "$D/total.ln" -o "$D/t.c" > "$D/t.err" 2>&1 && { echo "the u8 total compiled"; exit 1; }
grep -q 'running total' "$D/t.err" || { echo "the real running total lost its explanation"; exit 1; }
exit 0
