#!/usr/bin/env bash
# fuzz_combo.sh — the COMBINATION fuzzer for the shipping pipeline.
#
# Every other fuzzer here is VERTICAL: it drills one feature as deeply as it can. None
# CROSS two — and every compiler bug found by hand-probing in session 59 lived at an
# intersection (`case` on a by-reference parameter, whole-value assignment through a `var`
# aggregate, a fixed array OF A USER TYPE by reference, a field read off a CALL RESULT).
# Each axis alone was covered by the corpus; nothing crossed them.
#
# fuzz_combo.py composes three axes — SHAPE x CARRIER x OP — and every program is VALID BY
# CONSTRUCTION and SELF-CHECKING (it returns 0 iff it computes the value the generator
# predicted). That yields three distinct verdicts with no second pipeline needed:
#
#   WRONG-REJECT  a valid program the compiler refused
#   BROKEN-C      the emitted C does not compile
#   MISCOMPILE    it compiled and computed the WRONG ANSWER
#
#   bash fuzz_combo.sh [N]          # default 300 programs
#   RANDOM_SEED=123 bash fuzz_combo.sh 50     # reproducible
set -u
cd "$(dirname "$0")"
LAIN=./lain
GEN=fuzz_combo.py
CC="${CC:-gcc}"
DEFS="-Dlibc_printf=printf -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_calloc=calloc -Dlibc_realloc=realloc"
SC="${TMPDIR:-/tmp}/fuzz_combo.$$"; mkdir -p "$SC"
trap 'rm -rf "$SC"' EXIT
N="${1:-300}"
BASE=${RANDOM_SEED:-$$}
[ -x "$LAIN" ] || { echo "build first: gcc -std=c99 -o lain src/main.c -I src"; exit 2; }

ok=0 wrongrej=0 brokenc=0 miscomp=0 genfail=0
for ((k=0; k<N; k++)); do
    seed=$((BASE * 100000 + k))
    # A generator crash must be LOUD. Skipping it silently overstates coverage: the run
    # reports "all clean" over programs that were never produced, let alone compiled.
    if ! python3 "$GEN" "$seed" > "$SC/t.ln" 2>"$SC/gerr"; then
        genfail=$((genfail+1))
        [ $genfail -le 3 ] && { echo "── GENERATOR-FAIL seed=$seed"; tail -3 "$SC/gerr"; }
        continue
    fi
    hdr=$(head -1 "$SC/t.ln")

    if ! "$LAIN" "$SC/t.ln" -o "$SC/t.c" >"$SC/err" 2>&1; then
        code=$(grep -oE '^\[E[0-9]+\]' "$SC/err" | head -1)
        wrongrej=$((wrongrej+1))
        echo "── WRONG-REJECT ${code:-?}  $hdr"
        [ $wrongrej -le 3 ] && { sed -n '2,$p' "$SC/t.ln" | head -24; grep -m2 -E '^\[E' "$SC/err"; }
        continue
    fi
    if ! $CC -o "$SC/t" "$SC/t.c" $DEFS -w 2>"$SC/cerr"; then
        brokenc=$((brokenc+1))
        echo "── BROKEN-C  $hdr"
        [ $brokenc -le 3 ] && { sed -n '2,$p' "$SC/t.ln" | head -24; grep -m2 -E 'error' "$SC/cerr"; }
        continue
    fi
    if "$SC/t"; then ok=$((ok+1)); else
        miscomp=$((miscomp+1))
        echo "── MISCOMPILE (self-check failed)  $hdr"
        [ $miscomp -le 3 ] && sed -n '2,$p' "$SC/t.ln" | head -24
    fi
done

echo "=================================================================="
echo "fuzz_combo: $N programs   ok=$ok"
echo "  bugs:  WRONG-REJECT=$wrongrej  BROKEN-C=$brokenc  MISCOMPILE=$miscomp"
[ $genfail -gt 0 ] && echo "  GENERATOR-FAIL=$genfail   (harness bug — these tested NOTHING)"
echo "=================================================================="
[ $((wrongrej + brokenc + miscomp + genfail)) -eq 0 ]
