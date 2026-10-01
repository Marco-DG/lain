#!/usr/bin/env bash
# fuzz_modconst.sh — the MODULE-CONSTANT fuzzer.
#
# Every other generator writes its programs inside functions, and module scope is where four
# defects sat unseen until 2026-10-01: constants never checked against their type, tables
# accepted with uninitialised elements, the declared type ignored where a constant is folded
# into a use, and comprehensions there not modelled. fuzz_modconst.py writes only module
# constants (literals, references by name, shifts, explicit tables, comprehensions) and a
# `main` that reads every one; it computes each value exactly and says whether the program is
# valid. Four verdicts:
#
#   MISSED-REJECTION  an invalid constant (a value its type cannot hold, a table of the wrong
#                     length) was ACCEPTED — the soundness verdict
#   WRONG-REJECT      a valid program was refused
#   BROKEN-C          the emitted C does not compile
#   MISCOMPILE        it compiled and a constant read back as the wrong value
#
#   bash fuzz_modconst.sh [N]              # default 300 programs
#   RANDOM_SEED=123 bash fuzz_modconst.sh 50   # reproducible
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/../.."
LAIN=./lain
GEN="$HERE/fuzz_modconst.py"
CC="${CC:-gcc}"
SC="$(mktemp -d)"; trap 'rm -rf "$SC"' EXIT
N="${1:-300}"
BASE=${RANDOM_SEED:-$$}
[ -x "$LAIN" ] || { echo "build first: make"; exit 2; }

acc_ok=0 rej_ok=0 missed=0 wrongrej=0 brokenc=0 miscomp=0 genfail=0
for ((k=0; k<N; k++)); do
    seed=$((BASE * 100000 + k))
    if ! python3 "$GEN" "$seed" > "$SC/t.ln" 2>"$SC/gerr"; then
        genfail=$((genfail+1))
        [ $genfail -le 3 ] && { echo "── GENERATOR-FAIL seed=$seed"; tail -3 "$SC/gerr"; }
        continue
    fi
    hdr=$(head -1 "$SC/t.ln")
    expect=accept; grep -q "EXPECT reject" "$SC/t.ln" && expect=reject
    "$LAIN" "$SC/t.ln" -o "$SC/t.c" >"$SC/err" 2>&1; rc=$?
    if [ $expect = reject ]; then
        if [ $rc -ne 0 ]; then rej_ok=$((rej_ok+1)); continue; fi
        missed=$((missed+1)); echo "── MISSED-REJECTION  $hdr"
        [ $missed -le 3 ] && sed -n '2,$p' "$SC/t.ln" | grep -v "^    if" | head -12
        continue
    fi
    if [ $rc -ne 0 ]; then
        wrongrej=$((wrongrej+1)); echo "── WRONG-REJECT $(grep -oE '^\[E[0-9]+\]' "$SC/err" | head -1)  $hdr"
        [ $wrongrej -le 3 ] && { sed -n '2,$p' "$SC/t.ln" | grep -v "^    if" | head -12; grep -m2 -E '^\[E' "$SC/err"; }
        continue
    fi
    if ! $CC -fsanitize=undefined -o "$SC/t" "$SC/t.c" -w 2>"$SC/cerr"; then
        brokenc=$((brokenc+1)); echo "── BROKEN-C  $hdr"
        [ $brokenc -le 3 ] && { sed -n '2,$p' "$SC/t.ln" | head -12; grep -m2 error "$SC/cerr"; }
        continue
    fi
    if "$SC/t" 2>"$SC/rerr" && ! grep -q "runtime error" "$SC/rerr"; then acc_ok=$((acc_ok+1)); else
        miscomp=$((miscomp+1)); echo "── MISCOMPILE (check $?)  $hdr"
        [ $miscomp -le 3 ] && sed -n '2,$p' "$SC/t.ln" | head -20
    fi
done

echo "=================================================================="
echo "fuzz_modconst: $N programs   valid accepted+correct=$acc_ok   invalid refused=$rej_ok"
echo "  bugs:  MISSED-REJECTION=$missed  WRONG-REJECT=$wrongrej  BROKEN-C=$brokenc  MISCOMPILE=$miscomp"
[ $genfail -gt 0 ] && echo "  GENERATOR-FAIL=$genfail   (harness bug — these tested NOTHING)"
echo "=================================================================="
[ $((missed + wrongrej + brokenc + miscomp + genfail)) -eq 0 ]
