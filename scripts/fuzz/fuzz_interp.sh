#!/usr/bin/env bash
# fuzz_interp.sh — EVERY generator's programs, run two ways: the emitted C, and the IR interpreter.
#
# Each other fuzzer has an oracle for its own property (ownership, overflow, termination...). This
# one gives all of them a second, SEMANTIC oracle: for every program a generator produces and the
# compiler accepts, `lain --interpret` (src/ir/interp.h: abstract values, every discharged proof
# checked as it is used) must print the same bytes and exit with the same status as the C. Verdicts:
#
#   DIFFER        the two legs disagree: a backend representation bug or a semantics bug
#   PROOF         the interpreter caught a false proof (or a violated trusted assumption)
#   INVARIANT     the range analysis's state at some block excludes the running program's
#                 (--check-invariants): a wrong fact, even one no obligation happened to read
#   UB            undefined behaviour inside `unsafe` (a generator bug: its programs should be
#                 defined, or the oracle of that generator means nothing)
#   CRASH/BUDGET  the interpreter died or ran out of steps
#
#   bash fuzz_interp.sh [N]                    # N programs per generator (default 30)
#   RANDOM_SEED=123 bash fuzz_interp.sh 10     # reproducible
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/../.."
LAIN="$(pwd)/lain"; [ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
N="${1:-30}"; BASE=${RANDOM_SEED:-$$}
SC="$(mktemp -d)"; trap 'rm -rf "$SC"' EXIT
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_calloc=calloc -Dlibc_realloc=realloc"
agree=0 diff=0 proof=0 inv=0 ub=0 crash=0 unsup=0 rej=0 cfail=0 genfail=0
for g in "$HERE"/fuzz_*.py; do
  gn=$(basename "$g" .py)
  for ((k=0; k<N; k++)); do
    seed=$((BASE * 1000 + k)); f="$SC/${gn}_$k.ln"
    python3 "$g" "$seed" > "$f" 2>/dev/null || { genfail=$((genfail+1)); continue; }
    timeout 60 "$LAIN" "$f" -o "$SC/x.c" >/dev/null 2>&1 || { rej=$((rej+1)); continue; }
    gcc -O2 -o "$SC/x" "$SC/x.c" $DEFS -w -lm 2>/dev/null || { cfail=$((cfail+1)); continue; }
    ( cd "$SC" && timeout 20 ./x > c.out 2>/dev/null; echo $? > c.st ) 2>/dev/null
    LAIN_INTERP_STEPS=50000000 timeout 60 "$LAIN" "$f" --check-invariants > "$SC/i.out" 2>"$SC/i.err"; is=$?
    m=$(grep -m1 'lain --interpret' "$SC/i.err")
    if   echo "$m" | grep -q "INVARIANT VIOLATED";   then inv=$((inv+1)); echo "── INVARIANT  $gn seed=$seed :: $m"
    elif echo "$m" | grep -q "PROOF FAILED\|TRUSTED"; then proof=$((proof+1)); echo "── PROOF  $gn seed=$seed :: $m"
    elif echo "$m" | grep -q "UNDEFINED BEHAVIOUR";  then ub=$((ub+1));       echo "── UB  $gn seed=$seed :: $m"
    elif echo "$m" | grep -q "NOT MODELLED";         then unsup=$((unsup+1))
    elif echo "$m" | grep -q "STEP BUDGET";          then crash=$((crash+1)); echo "── BUDGET  $gn seed=$seed"
    elif [ "$is" = "$(cat "$SC/c.st")" ] && cmp -s "$SC/c.out" "$SC/i.out"; then agree=$((agree+1))
    elif [ "$is" -ge 124 ] && [ "$is" != "$(cat "$SC/c.st")" ]; then crash=$((crash+1)); echo "── CRASH($is)  $gn seed=$seed"
    else diff=$((diff+1)); echo "── DIFFER  $gn seed=$seed :: C exit $(cat "$SC/c.st"), interpreter exit $is"; fi
  done
done
echo "=================================================================="
echo "fuzz_interp: $N programs per generator   agree=$agree   (rejected=$rej, not modelled=$unsup, C link failed=$cfail)"
echo "  bugs:  DIFFER=$diff  PROOF=$proof  INVARIANT=$inv  UB=$ub  CRASH=$crash"
[ $genfail -gt 0 ] && echo "  GENERATOR-FAIL=$genfail   (harness bug — these tested NOTHING)"
echo "=================================================================="
[ $((diff + proof + inv + ub + crash + genfail)) -eq 0 ]
