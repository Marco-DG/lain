#!/usr/bin/env bash
# judge_gate.sh — every hypothesis the proof engine's TRUSTED generator states holds at run time.
#
# The judge (src/judge/) decides an obligation from the facts its generator writes (gen.h). A
# fact that is false where the program runs is a false proof waiting for a solver to use it, and
# no solver is needed to find one: run each accepted program on the interpreter, and each time an
# obligation's instruction is reached, evaluate every hypothesis of its VC at the values the
# program has there (judge_vc_driver.c --check-hyps, through interp.h's ii_on_instr hook).
#
# FAILS on any of:
#   - a false hypothesis (a generator or interpreter bug, never noise);
#   - a function the generator refuses;
#   - a program whose run left no SUMMARY line (the harness died: that is not a verdict);
#   - fewer obligation events than the floor (a run that judged almost nothing is not a hold).
# A rule no program exercises is not seen here; src/tools/test_judge_gen.c pins those over a
# hand-built IR (run_ir_tests.sh).
#
# PROGRAMS THE PROVER REFUSES, run too: the judge's facts must hold on every program the
# interpreter can execute, not only on those today's prover accepts. A refused program is exactly
# one the judge will decide at phase 4, and its false hypothesis would be a false proof there.
# Two sources: every fail test whose expected codes are all VRA's obligation codes (E085, E086,
# E087, E015, E011, E082, E121, the precondition E012, or [VRA]; such a refusal comes after the IR
# exists), and the probes in scripts/gates/judge_runnable/, which no test holds. Each must leave
# 0 false hypotheses and no generator refusal; any exit status is accepted, since such a program
# may trap, and runs on a 1,000,000-step budget (an E011 test is a loop that does not end). One
# that cannot run (its error stops lowering: no summary) or that reaches no obligation is SKIPPED
# and counted, and a floor on those that ran keeps the coverage honest. A run that CRASHES is the
# harness's failure, not a verdict, and is listed by name.
#
# CLAIMS. The driver proposes claims at forward joins (hulls) and loop headers (invariants),
# untrusted: the solver's lia_maximize over the paths in, and a header's guards, filtered by
# Houdini. The generator states each where its block's versions are named and owes it on every
# edge in (`hull`, `inv.entry`, `inv.keep` VCs). So the gate also FAILS on a claim's VC that is
# not implied (a claim stated without its proof), and below either floor of claims checked at run
# time (a claim never evaluated is not a hold): each time the program takes an edge a claim is owed
# on, the driver evaluates the claim there, with its obligation's hypotheses, and where the claim
# is used after its block, as any hypothesis. The two are floored apart: the uses outnumber the
# edges, and would hide a run that checks no edge. RETURN claims (callee contracts) are owed at
# every return (`post` VCs) and stated at each call in the same module. They are floored apart
# too: checked where the program returns, and where a caller uses one.
#
# PRECISION, the other half: a hypothesis check cannot see a generator that stops stating a TRUE
# fact. So every program's VCs are also solved by the untrusted solver (solve_vc), and the gate
# reports how many are implied, failing below a floor. These are phase 1a's numbers; no kernel
# checks the certificates yet (scripts/gates/solver_harness.py checks the solver itself). A floor
# over the corpus has slack, so the judge's OWN programs, tests/trust/judge_*_pass.ln, each written
# for one rule, must have EVERY VC implied: one that is not is a rule that stopped working. They
# must be, too, from the judge's own candidates alone (`--no-finder`: no proof may need VRA's
# precision), except a program written for the finder (judge_*finder*_pass.ln).
#
#   bash scripts/gates/judge_gate.sh
set -u
cd "$(dirname "$0")/../.."
REACHED_FLOOR=100000         # obligation events; 287,380 when the gate was written (19473f7)
IMPLIED_FLOOR=7450       # obligations' VCs implied over the corpus; 7,470 of 7,567 when the floor was set (102f71f and the field cells). Lower it only with the reason
EDGE_FLOOR=50000         # claims checked at run time on the edges they are owed on; 260,682 when the floor was set
RETURN_FLOOR=4000        # return claims checked at run time where the program returns; 9,065 when the floor was set
CONTRACT_FLOOR=2000      # return claims checked at run time where a caller uses them; 4,096 when the floor was set
USE_FLOOR=100000         # claims checked at run time where they are used; 1,215,712 when the floor was set
REFUSED_FLOOR=150        # programs the prover refuses that ran and reached an obligation; 197 when the floor was set
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
if ! gcc -std=c99 -O2 -w -o "$T/jvc" src/tools/judge_vc_driver.c -I src 2> "$T/build.err" ||
   ! gcc -std=c99 -O2 -w -o "$T/solve_vc" src/tools/solve_vc.c -I src 2>> "$T/build.err"; then
    echo "judge_gate: the driver or the solver does not build:"; head -5 "$T/build.err"; exit 1
fi
find tests -name '*_pass.ln' | sort > "$T/files.txt"
declared=$(wc -l < "$T/files.txt")
one() {
    local f="$1" o; o=$(echo "$f" | tr '/' '_')
    LAIN_INTERP_STEPS=50000000 timeout 120 "$T/jvc" "$f" --check-hyps > "$T/$o.out" 2> "$T/$o.err"
    echo $? > "$T/$o.rc"
    # the same VCs, solved: the driver's VC text without its comment lines
    "$T/jvc" "$f" 2>/dev/null | grep -v '^#' > "$T/$o.vc"
    if [ -s "$T/$o.vc" ]; then timeout 120 "$T/solve_vc" "$T/$o.vc" > "$T/$o.ans" 2>&1; echo $? > "$T/$o.src"; fi
    case "$f" in tests/trust/judge_*finder*_pass.ln) ;; tests/trust/judge_*_pass.ln)
        "$T/jvc" "$f" --no-finder 2>/dev/null | grep -v '^#' > "$T/$o.nf.vc"
        timeout 120 "$T/solve_vc" "$T/$o.nf.vc" > "$T/$o.nf.ans" 2>&1;;
    esac
}
export -f one; export T
# the refused programs: fail tests whose every EXPECT code is VRA's, and the probes
for f in $(find tests -name '*_fail.ln' | sort); do
    codes=$(grep -oE '^// EXPECT: \[[A-Z0-9]+\]' "$f" | grep -oE '\[[A-Z0-9]+\]' | sort -u)
    [ -n "$codes" ] || continue
    echo "$codes" | grep -qvE '^\[(E085|E086|E087|E015|E011|E082|E121|E012|VRA)\]$' && continue
    echo "$f"
done > "$T/refused.txt"
ls scripts/gates/judge_runnable/*.ln >> "$T/refused.txt"
refused_one() {
    local f="$1" o; o=r_$(echo "$f" | tr '/' '_')
    LAIN_INTERP_STEPS=1000000 timeout 120 "$T/jvc" "$f" --check-hyps > "$T/$o.out" 2> "$T/$o.err"
    echo $? > "$T/$o.rc"
}
export -f refused_one
xargs -P "${LAIN_GATE_JOBS:-4}" -I{} bash -c 'one "{}"' < "$T/files.txt"
xargs -P "${LAIN_GATE_JOBS:-4}" -I{} bash -c 'refused_one "{}"' < "$T/refused.txt"
judged=0; nosum=0; refused=0; falsec=0; reached=0; checked=0; solverr=0; implied=0; notimp=0; outbudget=0
claims=0; hullok=0; hullbad=0; edges=0; claimchk=0; claimgoals=0; mustfail=0; retgoals=0; contractchk=0
while read -r f; do
    o=$(echo "$f" | tr '/' '_')
    s=$(grep -m1 '^SUMMARY ' "$T/$o.err")
    if [ -z "$s" ]; then
        nosum=$((nosum+1)); echo "  NO SUMMARY (the harness died, rc $(cat "$T/$o.rc")): $f"; tail -2 "$T/$o.err" | sed 's/^/      /'
        continue
    fi
    judged=$((judged+1))
    # the checker writes to stderr; the program's own output is on stdout and need not end a line
    if grep -q '^ERROR ' "$T/$o.err"; then refused=$((refused+1)); echo "  GENERATOR REFUSED: $f"; grep -m2 '^ERROR ' "$T/$o.err" | sed 's/^/      /'; fi
    n=$(grep -c '^FALSE ' "$T/$o.err")
    if [ "$n" -gt 0 ]; then falsec=$((falsec+n)); echo "  FALSE HYPOTHESES ($n): $f"; grep -m3 '^FALSE ' "$T/$o.err" | sed 's/^/      /'; fi
    if [ -f "$T/$o.src" ]; then
        if [ "$(cat "$T/$o.src")" != 0 ]; then solverr=$((solverr+1)); echo "  SOLVER FAILED (rc $(cat "$T/$o.src")): $f"; tail -2 "$T/$o.ans" | sed 's/^/      /'; fi
        # an answer line names its VC: `cert FN IRHASH POINT KIND ORDINAL`; a claim's kind is `hull`,
        # `inv.entry`, `inv.keep` or `post`, and every other kind is an obligation's
        h=$(grep -cE '^cert [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post) ' "$T/$o.ans"); hullok=$((hullok + h))
        implied=$((implied + $(grep -c '^cert ' "$T/$o.ans") - h)); notimp=$((notimp + $(grep -c '^model ' "$T/$o.ans") - $(grep -cE '^model [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post) ' "$T/$o.ans")))
        outbudget=$((outbudget + $(grep -cE '^(budget|capacity) ' "$T/$o.ans") - $(grep -cE '^(budget|capacity) [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post) ' "$T/$o.ans")))
        n=$(grep -cE '^(model|budget|capacity) [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post) ' "$T/$o.ans")
        if [ "$n" -gt 0 ]; then hullbad=$((hullbad+n)); echo "  CLAIM NOT IMPLIED ($n): $f"; grep -m2 -E '^(model|budget|capacity) [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post) ' "$T/$o.ans" | sed 's/^/      /'; fi
        case "$f" in tests/trust/judge_*_pass.ln)
            n=$(grep -cE '^(model|budget|capacity) ' "$T/$o.ans")
            if [ "$n" -gt 0 ] || [ ! -s "$T/$o.ans" ]; then mustfail=$((mustfail+1)); echo "  MUST-PROVE, $n NOT IMPLIED: $f"; grep -m2 -E '^(model|budget|capacity) ' "$T/$o.ans" | sed 's/^/      /'; fi
            if [ -f "$T/$o.nf.ans" ]; then
                n=$(grep -cE '^(model|budget|capacity) ' "$T/$o.nf.ans")
                if [ "$n" -gt 0 ] || [ ! -s "$T/$o.nf.ans" ]; then mustfail=$((mustfail+1)); echo "  MUST-PROVE WITHOUT THE FINDER, $n NOT IMPLIED: $f"; grep -m2 -E '^(model|budget|capacity) ' "$T/$o.nf.ans" | sed 's/^/      /'; fi
            fi;;
        esac
    fi
    r=$(echo "$s" | sed -nE 's/.* reached=([0-9]+) .*/\1/p'); c=$(echo "$s" | sed -nE 's/.* hyps-checked=([0-9]+) .*/\1/p')
    reached=$((reached + ${r:-0})); checked=$((checked + ${c:-0}))
    r=$(echo "$s" | sed -nE 's/.* claims=([0-9]+) .*/\1/p'); c=$(echo "$s" | sed -nE 's/.* edges=([0-9]+) .*/\1/p')
    claims=$((claims + ${r:-0})); edges=$((edges + ${c:-0}))
    c=$(echo "$s" | sed -nE 's/.* claim-hyps-checked=([0-9]+) .*/\1/p'); claimchk=$((claimchk + ${c:-0}))
    c=$(echo "$s" | sed -nE 's/.* claim-goals-checked=([0-9]+).*/\1/p'); claimgoals=$((claimgoals + ${c:-0}))
    c=$(echo "$s" | sed -nE 's/.* return-goals-checked=([0-9]+).*/\1/p'); retgoals=$((retgoals + ${c:-0}))
    c=$(echo "$s" | sed -nE 's/.* contract-hyps-checked=([0-9]+).*/\1/p'); contractchk=$((contractchk + ${c:-0}))
done < "$T/files.txt"
rdecl=$(wc -l < "$T/refused.txt"); rran=0; rnosum=0; rnothing=0; rfalse=0; rrefused=0; rchecked=0; rcrash=0
while read -r f; do
    o=r_$(echo "$f" | tr '/' '_')
    s=$(grep -m1 '^SUMMARY ' "$T/$o.err")
    if [ -z "$s" ] && [ "$(cat "$T/$o.rc")" -ge 128 ]; then
        rcrash=$((rcrash+1)); echo "  CRASHED (rc $(cat "$T/$o.rc"); the harness, not a verdict): $f"; continue
    fi
    if [ -z "$s" ]; then rnosum=$((rnosum+1)); continue; fi              # cannot run: skipped
    if grep -q '^ERROR ' "$T/$o.err"; then rrefused=$((rrefused+1)); echo "  GENERATOR REFUSED (a refused program): $f"; grep -m2 '^ERROR ' "$T/$o.err" | sed 's/^/      /'; fi
    n=$(grep -c '^FALSE ' "$T/$o.err")
    if [ "$n" -gt 0 ]; then rfalse=$((rfalse+n)); echo "  FALSE HYPOTHESES ($n), in a program the prover refuses: $f"; grep -m3 '^FALSE ' "$T/$o.err" | sed 's/^/      /'; fi
    r=$(echo "$s" | sed -nE 's/.* reached=([0-9]+) .*/\1/p'); c=$(echo "$s" | sed -nE 's/.* hyps-checked=([0-9]+) .*/\1/p')
    if [ "${r:-0}" -eq 0 ]; then rnothing=$((rnothing+1)); continue; fi  # reached no obligation: skipped
    rran=$((rran+1)); rchecked=$((rchecked + ${c:-0}))
done < "$T/refused.txt"
echo "=================================================================="
echo "programs declared $declared, judged $judged, no summary $nosum"
echo "obligation events $reached, hypotheses checked at run time $checked, false $falsec, generator refusals $refused"
echo "VCs implied $implied, not implied $notimp, budget or capacity $outbudget, solver failures $solverr (floor $IMPLIED_FLOOR)"
echo "claims $claims: their VCs implied $hullok, not implied $hullbad; at run time, claims checked on $claimgoals edges taken (floor $EDGE_FLOOR) and $claimchk times where used (floor $USE_FLOOR)"
echo "return claims checked at run time at $retgoals returns (floor $RETURN_FLOOR) and $contractchk times at a call (floor $CONTRACT_FLOOR)"
echo "the judge's own programs (tests/trust/judge_*_pass.ln) with a VC not implied, with or without the finder: $mustfail"
echo "programs the prover refuses: $rdecl; ran $rran (floor $REFUSED_FLOOR), skipped $((rnosum + rnothing)) ($rnosum cannot run, $rnothing reach no obligation), crashed $rcrash; hypotheses checked $rchecked, false $rfalse, generator refusals $rrefused"
if [ "$nosum" -gt 0 ] || [ "$refused" -gt 0 ] || [ "$falsec" -gt 0 ] || [ "$judged" -ne "$declared" ] || [ "$solverr" -gt 0 ] || [ "$hullbad" -gt 0 ] || [ "$mustfail" -gt 0 ] || [ "$rfalse" -gt 0 ] || [ "$rrefused" -gt 0 ]; then
    echo "judge_gate: FAIL"; exit 1
fi
if [ "$reached" -lt "$REACHED_FLOOR" ]; then
    echo "judge_gate: FAIL — $reached obligation events is below the floor of $REACHED_FLOOR: a run that judged almost nothing is not a hold"; exit 1
fi
if [ "$claimgoals" -lt "$EDGE_FLOOR" ] || [ "$claimchk" -lt "$USE_FLOOR" ]; then
    echo "judge_gate: FAIL — claims checked at run time, $claimgoals on edges and $claimchk where used, below the floors ($EDGE_FLOOR, $USE_FLOOR): claims are stated where no run evaluates them"; exit 1
fi
if [ "$retgoals" -lt "$RETURN_FLOOR" ] || [ "$contractchk" -lt "$CONTRACT_FLOOR" ]; then
    echo "judge_gate: FAIL — return claims checked at run time, $retgoals at returns and $contractchk at calls, below the floors ($RETURN_FLOOR, $CONTRACT_FLOOR): contracts are stated where no run evaluates them"; exit 1
fi
if [ "$rran" -lt "$REFUSED_FLOOR" ]; then
    echo "judge_gate: FAIL — $rran programs the prover refuses ran, below the floor of $REFUSED_FLOOR: the refused set lost its coverage"; exit 1
fi
if [ "$implied" -lt "$IMPLIED_FLOOR" ]; then
    echo "judge_gate: FAIL — $implied VCs implied is below the floor of $IMPLIED_FLOOR: the generator stopped stating a fact it stated"; exit 1
fi
echo "judge_gate: hold — every hypothesis the generator states is true where the program runs, and $implied VCs are implied"
