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
#   CRASH/BUDGET  the interpreter died, or ran out of steps where the C program finished in under 1 s
#                 both at -O2 and at -O0 (-O2 C does about 1e9 simple operations a second, so such
#                 a program fits in 5e7 steps: running out points at the interpreter, e.g. a wrong
#                 loop exit) or died of a signal
#   INTERNAL      the compiler failed on a generated program (exit 70, a signal): no verdict at all.
#                 Until 2026-10-21 every non-zero exit counted as `rejected`, so an internal error
#                 read as the analysis refusing the program
#   LONG          (not a bug) an accepted program that runs too long to compare. Either:
#                 - the C program ran past its 20 s AND the interpreter stopped on its own limit, its
#                   step budget or its recursion depth (RECURSION TOO DEEP: fuzz_cycles seed
#                   20261021012, until 2026-10-21 a DIFFER);
#                 - or the C program FINISHED after at least 1 s, the interpreter ran out of steps,
#                   and all it printed is the start of what the C printed (fuzz_cycles seed
#                   2062896004, until 2026-10-25 a BUDGET: 3.9e9 calls, 4.4 s of -O2 C, past the 5e7
#                   steps the interpreter makes in about 4 s). The budget cannot simply grow: every
#                   call keeps its frame's object shells, so that a dangling pointer is caught (1.2 GB
#                   at 1e8 steps on that program);
#                 - or the same where the -O2 C ended in under 1 s but, built again at -O0, ran 1 s
#                   or more (or past 20 s). -O2 can fold the work away: fuzz_cycles seed 3776866001,
#                   an exponential recursion whose every result is 0, takes 0.03 s at -O2 and 1.2 s
#                   (1.5e8 calls) at -O0; until 2026-10-25 a BUDGET. -O0 cannot fold it, so a C
#                   that is quick even at -O0 still points at the interpreter.
#                 Out of steps with output that is NOT the start of the C's is a DIFFER, and so is the
#                 depth limit where the C FINISHED. Under the judge (PE.9) the default
#                 accepts recursions VRA refused, some exponential: fuzz_cycles' `T(x, y) = T(x, y+1)
#                 + T(x-1, y-1)` ends (ranked 56x - y + 147) after binomially many calls. Whether an
#                 accepted program ENDS is fuzz_cycles' and fuzz_loops' question, with ground truth.
#
# Since PE.9's flip (2426d11) the judge decides by default, and its compile is slow on loop-heavy
# programs (a 37-line fuzz_window program: 66 s, against 0.26 s under --judge=vra, all of it finder
# work). Two consequences here:
#   - the acceptance compile (the judge, the decider) has 300 s; a compile past that is counted as
#     a COMPILE TIMEOUT, not as rejected;
#   - --check-invariants checks the RANGE ANALYSIS's states, so it runs under --judge=vra: the
#     check it means, without paying the judge's compile a second time. Under VRA it also checks
#     every proof VRA discharged. A program VRA refuses (the judge proves more) is run by the
#     judge's own `--interpret`, every proof the JUDGE discharged checked as it is used, and is
#     counted. For a program both accept, the interpreter checks VRA's proofs, not the judge's;
#     the judge's are then held only to the C leg's output.
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
agree=0 diff=0 proof=0 inv=0 ub=0 crash=0 unsup=0 rej=0 cfail=0 genfail=0 long=0 ctime=0 internal=0 jonly=0
# The C built again at -O0 and timed (o0ms, o0st): true when it exited on its own after 1 s or more,
# or ran past 20 s. Called only where the -O2 C ended in under 1 s.
slow_at_O0() {
  gcc -O0 -o "$SC/x0" "$SC/x.c" $DEFS -w -lm 2>/dev/null || return 1
  local t0=$(date +%s%N)
  ( cd "$SC" && timeout 20 ./x0 > /dev/null 2>&1; echo $? > c0.st ) 2>/dev/null
  o0ms=$(( ($(date +%s%N) - t0) / 1000000 )); o0st=$(cat "$SC/c0.st")
  [ "$o0st" -le 124 ] && [ $o0ms -ge 1000 ]
}
for g in "$HERE"/fuzz_*.py; do
  gn=$(basename "$g" .py)
  # Not every fuzz_*.py is a program generator. fuzz_malformed.py MUTATES a corpus program (a source,
  # an operator and a seed) and is run by its own driver, as fuzz_metamorphic.py will be; taken for a
  # generator it failed every call and counted 30 GENERATOR-FAIL. Named, not detected by failure: a
  # generator that breaks must still be reported.
  # fuzz_regions.py's programs call externs (its choices), so they do not link.
  case "$gn" in fuzz_malformed|fuzz_metamorphic|fuzz_regions) continue ;; esac
  for ((k=0; k<N; k++)); do
    seed=$((BASE * 1000 + k)); f="$SC/${gn}_$k.ln"
    python3 "$g" "$seed" > "$f" 2>/dev/null || { genfail=$((genfail+1)); continue; }
    timeout -s KILL 300 "$LAIN" "$f" -o "$SC/x.c" >/dev/null 2>"$SC/x.err"; cr=$?
    if [ $cr -eq 1 ]; then rej=$((rej+1)); continue; fi
    if [ $cr -eq 137 ]; then ctime=$((ctime+1)); echo "── COMPILE TIMEOUT  $gn seed=$seed (past 300 s)"; continue; fi
    if [ $cr -ne 0 ]; then internal=$((internal+1)); echo "── INTERNAL($cr)  $gn seed=$seed :: $(grep -v '^\s*$' "$SC/x.err" | head -1 | cut -c1-120)"; continue; fi
    gcc -O2 -o "$SC/x" "$SC/x.c" $DEFS -w -lm 2>/dev/null || { cfail=$((cfail+1)); continue; }
    t0=$(date +%s%N)
    ( cd "$SC" && timeout 20 ./x > c.out 2>/dev/null; echo $? > c.st ) 2>/dev/null
    cms=$(( ($(date +%s%N) - t0) / 1000000 )); o0ms=
    LAIN_INTERP_STEPS=50000000 timeout 60 "$LAIN" "$f" --check-invariants --judge=vra > "$SC/i.out" 2>"$SC/i.err"; is=$?
    if grep -q '^\[E[0-9]*\] Error' "$SC/i.err"; then      # VRA refuses what the judge accepted
      jonly=$((jonly+1))
      LAIN_INTERP_STEPS=50000000 timeout 360 "$LAIN" "$f" --interpret > "$SC/i.out" 2>"$SC/i.err"; is=$?
    fi
    m=$(grep -m1 'lain --interpret' "$SC/i.err")
    if   echo "$m" | grep -q "INVARIANT VIOLATED";   then inv=$((inv+1)); echo "── INVARIANT  $gn seed=$seed :: $m"
    elif echo "$m" | grep -q "PROOF FAILED\|TRUSTED"; then proof=$((proof+1)); echo "── PROOF  $gn seed=$seed :: $m"
    elif echo "$m" | grep -q "UNDEFINED BEHAVIOUR";  then ub=$((ub+1));       echo "── UB  $gn seed=$seed :: $m"
    elif echo "$m" | grep -q "NOT MODELLED";         then unsup=$((unsup+1))
    elif echo "$m" | grep -q "STEP BUDGET\|RECURSION TOO DEEP" && [ "$(cat "$SC/c.st")" = 124 ]; then
      long=$((long+1)); echo "── LONG  $gn seed=$seed (C past 20 s, interpreter past its own limit: $(echo "$m" | grep -oE 'STEP BUDGET|RECURSION TOO DEEP'))"
    elif echo "$m" | grep -q "STEP BUDGET" && ! cmp -s -n "$(wc -c < "$SC/i.out")" "$SC/i.out" "$SC/c.out"; then
      diff=$((diff+1)); echo "── DIFFER  $gn seed=$seed :: the interpreter ran out of steps, and what it printed is not the start of the C's output"
    elif echo "$m" | grep -q "STEP BUDGET" && [ "$(cat "$SC/c.st")" -lt 124 ] && [ $cms -ge 1000 ]; then
      long=$((long+1)); echo "── LONG  $gn seed=$seed (C finished in $((cms/1000)).$((cms%1000/100)) s; the interpreter ran out of steps, all it printed the C's)"
    elif echo "$m" | grep -q "STEP BUDGET" && [ "$(cat "$SC/c.st")" -lt 124 ] && slow_at_O0; then
      long=$((long+1)); echo "── LONG  $gn seed=$seed (C finished in $((cms/1000)).$((cms%1000/100)) s at -O2, exit $o0st in $((o0ms/1000)).$((o0ms%1000/100)) s at -O0; the interpreter ran out of steps, all it printed the C's)"
    elif echo "$m" | grep -q "STEP BUDGET";          then crash=$((crash+1)); echo "── BUDGET  $gn seed=$seed (C exit $(cat "$SC/c.st") in $((cms/1000)).$((cms%1000/100)) s${o0ms:+, exit $o0st in $((o0ms/1000)).$((o0ms%1000/100)) s at -O0})"
    elif [ "$is" = "$(cat "$SC/c.st")" ] && cmp -s "$SC/c.out" "$SC/i.out"; then agree=$((agree+1))
    elif [ "$is" -ge 124 ] && [ "$is" != "$(cat "$SC/c.st")" ]; then crash=$((crash+1)); echo "── CRASH($is)  $gn seed=$seed"
    else diff=$((diff+1)); echo "── DIFFER  $gn seed=$seed :: C exit $(cat "$SC/c.st"), interpreter exit $is"; fi
  done
done
echo "=================================================================="
echo "fuzz_interp: $N programs per generator   agree=$agree   (rejected=$rej, compile timeout=$ctime, not modelled=$unsup, C link failed=$cfail, too long to compare=$long)"
echo "  run by the judge's interpreter (VRA refuses them): $jonly"
echo "  bugs:  DIFFER=$diff  PROOF=$proof  INVARIANT=$inv  UB=$ub  CRASH=$crash  INTERNAL=$internal"
[ $genfail -gt 0 ] && echo "  GENERATOR-FAIL=$genfail   (harness bug — these tested NOTHING)"
echo "=================================================================="
[ $((diff + proof + inv + ub + crash + genfail + internal)) -eq 0 ]
