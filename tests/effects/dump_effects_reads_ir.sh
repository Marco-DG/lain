#!/usr/bin/env bash
# --dump-effects prints the IR's effect row (I.143), the row that decides gcc's `pure`/`const` and
# what static evaluation may run. It printed the front end's own inference instead, which called a
# `while A and B` loop {Diverge} even where the analysis proves it ends (Haystack H2), and passed
# that on to every caller. The `break` spelling of the same loop was {} in both.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
[ -x "$LAIN" ] || { echo "no compiler at $LAIN"; exit 1; }
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/e.ln" <<'LN'
func count(s u8[]) usize {
    var j usize = 0
    while j < s.len and j < 100 { j = j + 1 }
    return j
}
func count_break(s u8[]) usize {
    var j usize = 0
    while j < s.len {
        if j >= 100 { break }
        j = j + 1
    }
    return j
}
func spin(n u8) u8 effects diverge {
    var x = n
    while x != 0 { x = x +% 1 }
    return x
}
func main() i32 {
    var a u8[4] = [1, 2, 3, 4]
    return (count(a) + count_break(a)) as i32
}
LN
out=$("$LAIN" --dump-effects "$D/e.ln" -o "$D/e.c" 2>&1) || { echo "the program was refused"; echo "$out"; exit 1; }
want() { echo "$out" | grep -qxF "$1" || { echo "missing: $1"; echo "$out" | grep '^\[effects\]'; exit 1; }; }
want '[effects] func count : {}  (pure & total)'
want '[effects] func count_break : {}  (pure & total)'
want '[effects] func spin : {Diverge}'
want '[effects] func main : {}  (pure & total)'
exit 0
