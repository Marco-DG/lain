#!/usr/bin/env bash
# ghost_measure.sh — a function's WRITTEN `decreasing` measure is lowered as a GHOST tree over its
# parameters (IrFunc.measure_val, I.206): the judge reads it, nothing executes it. Checked here:
#   1. the lowering driver shows it for a parameter (`decreasing n`) and a difference
#      (`decreasing hi - lo`), as an instruction outside every block of the CFG, and none for a
#      measure the front end inferred;
#   2. it emits nothing: the C of a function with the clause equals the C without it (the front end
#      infers a measure for the self-call in its place), byte for byte, so the ghost's values are
#      numbered after the body's and no C variable moves;
#   3. a program whose base case is the only path where the measure is ill-defined (u64 `hi - lo`
#      with `lo >= hi` returning first) compiles: the ghost owes nothing where it stands.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cd "$T" || exit 1
gcc -std=c99 -O2 -o lower_driver "$ROOT/src/tools/lower_driver.c" -I "$ROOT/src" 2>/dev/null || { echo "lower_driver does not build"; exit 1; }
cat > m.ln <<'LN'
func down(n u32) u32 decreasing n {
    if n == 0 { return 0 }
    return down(n - 1)
}
func span(lo u64, hi u64) u64 decreasing hi - lo {
    if lo >= hi { return 0 }
    return span(lo + 1, hi)
}
func main() i32 {
    a = down(3)
    b = span(1, 4)
    return 0
}
LN
mkdir p && sed 's/ decreasing n {/ {/; s/ decreasing hi - lo {/ {/' m.ln > p/m.ln     # one name: one module prefix
d=$(./lower_driver m.ln 2>&1)
echo "$d" | grep -A2 'func m_down' > /dev/null || { echo "1: no dump: $d"; exit 1; }
n=$(echo "$d" | grep -c '^decreasing (ghost, not in the CFG):')
[ "$n" = 2 ] || { echo "1: $n ghost measures, not 2: $d"; exit 1; }
echo "$d" | grep -qE '^  %[0-9]+ = sub %1, %0 : u64$' || { echo "1: hi - lo not stated over the parameters: $d"; exit 1; }
# an INFERRED measure (the front end installs one for the twin's self-calls in the same field) is
# no claim: no ghost
dp=$(cd p && ../lower_driver m.ln 2>&1)
echo "$dp" | grep -q 'func ' || { echo "1: no dump of the twin: $dp"; exit 1; }
echo "$dp" | grep -q '^decreasing' && { echo "1: a ghost for an inferred measure: $dp"; exit 1; }
"$ROOT/lain" m.ln -o m.c > m.err 2>&1 || { echo "3: refused: $(cat m.err)"; exit 1; }
(cd p && "$ROOT/lain" m.ln -o m.c) > p.err 2>&1 || { echo "2: the clause-free twin is refused: $(cat p.err)"; exit 1; }
grep -q 'decreasing' p/m.ln && { echo "2: the twin still has a clause"; exit 1; }
cmp -s m.c p/m.c || { echo "2: the ghost moved the emitted C"; diff m.c p/m.c | head; exit 1; }
exit 0
