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
# CLAIMS. The driver proposes a hull claim at each forward join (untrusted: the solver's
# lia_maximize over each incoming path's VC), and the generator states it after the join and owes
# it on every edge in (`hull` VCs). So the gate also FAILS on a `hull` VC that is not implied (a
# claim stated without its proof), and below a floor of claims checked at run time (a claim never
# evaluated is not a hold): each time the program takes an edge a claim is owed on, the driver
# evaluates the claim there, with its obligation's hypotheses, and where the claim is used after
# its join, as any hypothesis.
#
# PRECISION, the other half: a hypothesis check cannot see a generator that stops stating a TRUE
# fact. So every program's VCs are also solved by the untrusted solver (solve_vc), and the gate
# reports how many are implied, failing below a floor. These are phase 1a's numbers; no kernel
# checks the certificates yet (scripts/gates/solver_harness.py checks the solver itself). A floor
# over the corpus has slack, so the judge's OWN programs, tests/trust/judge_*_pass.ln, each written
# for one rule, must have EVERY VC implied: one that is not is a rule that stopped working.
#
#   bash scripts/gates/judge_gate.sh
set -u
cd "$(dirname "$0")/../.."
REACHED_FLOOR=100000         # obligation events; 287,380 when the gate was written (19473f7)
IMPLIED_FLOOR=7120       # obligations' VCs implied over the corpus; 7,133 of 7,321 when the floor was set (ccdd5d0 and the hull claims). Lower it only with the reason
CLAIM_FLOOR=5000         # claims checked at run time, on their edges and where used; 17,074 when the floor was set
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
}
export -f one; export T
xargs -P "${LAIN_GATE_JOBS:-4}" -I{} bash -c 'one "{}"' < "$T/files.txt"
judged=0; nosum=0; refused=0; falsec=0; reached=0; checked=0; solverr=0; implied=0; notimp=0; outbudget=0
claims=0; hullok=0; hullbad=0; edges=0; claimchk=0; claimgoals=0; mustfail=0
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
        # an answer line names its VC: `cert FN IRHASH POINT KIND ORDINAL`; a claim's kind is `hull`
        h=$(grep -cE '^cert [^ ]+ [^ ]+ [^ ]+ hull ' "$T/$o.ans"); hullok=$((hullok + h))
        implied=$((implied + $(grep -c '^cert ' "$T/$o.ans") - h)); notimp=$((notimp + $(grep -cE '^model [^ ]+ [^ ]+ [^ ]+ [^h]' "$T/$o.ans")))
        outbudget=$((outbudget + $(grep -cE '^(budget|capacity) [^ ]+ [^ ]+ [^ ]+ [^h]' "$T/$o.ans")))
        n=$(grep -cE '^(model|budget|capacity) [^ ]+ [^ ]+ [^ ]+ hull ' "$T/$o.ans")
        if [ "$n" -gt 0 ]; then hullbad=$((hullbad+n)); echo "  CLAIM NOT IMPLIED ($n): $f"; grep -m2 -E '^(model|budget|capacity) [^ ]+ [^ ]+ [^ ]+ hull ' "$T/$o.ans" | sed 's/^/      /'; fi
        case "$f" in tests/trust/judge_*_pass.ln)
            n=$(grep -cE '^(model|budget|capacity) ' "$T/$o.ans")
            if [ "$n" -gt 0 ] || [ ! -s "$T/$o.ans" ]; then mustfail=$((mustfail+1)); echo "  MUST-PROVE, $n NOT IMPLIED: $f"; grep -m2 -E '^(model|budget|capacity) ' "$T/$o.ans" | sed 's/^/      /'; fi;;
        esac
    fi
    r=$(echo "$s" | sed -nE 's/.* reached=([0-9]+) .*/\1/p'); c=$(echo "$s" | sed -nE 's/.* hyps-checked=([0-9]+) .*/\1/p')
    reached=$((reached + ${r:-0})); checked=$((checked + ${c:-0}))
    r=$(echo "$s" | sed -nE 's/.* claims=([0-9]+) .*/\1/p'); c=$(echo "$s" | sed -nE 's/.* edges=([0-9]+) .*/\1/p')
    claims=$((claims + ${r:-0})); edges=$((edges + ${c:-0}))
    c=$(echo "$s" | sed -nE 's/.* claim-hyps-checked=([0-9]+) .*/\1/p'); claimchk=$((claimchk + ${c:-0}))
    c=$(echo "$s" | sed -nE 's/.* claim-goals-checked=([0-9]+).*/\1/p'); claimgoals=$((claimgoals + ${c:-0}))
done < "$T/files.txt"
echo "=================================================================="
echo "programs declared $declared, judged $judged, no summary $nosum"
echo "obligation events $reached, hypotheses checked at run time $checked, false $falsec, generator refusals $refused"
echo "VCs implied $implied, not implied $notimp, budget or capacity $outbudget, solver failures $solverr (floor $IMPLIED_FLOOR)"
echo "claims $claims: hull VCs implied $hullok, not implied $hullbad; at run time, claims checked on $claimgoals edges taken and $claimchk times where used (floor $CLAIM_FLOOR)"
echo "the judge's own programs (tests/trust/judge_*_pass.ln) with a VC not implied: $mustfail"
if [ "$nosum" -gt 0 ] || [ "$refused" -gt 0 ] || [ "$falsec" -gt 0 ] || [ "$judged" -ne "$declared" ] || [ "$solverr" -gt 0 ] || [ "$hullbad" -gt 0 ] || [ "$mustfail" -gt 0 ]; then
    echo "judge_gate: FAIL"; exit 1
fi
if [ "$reached" -lt "$REACHED_FLOOR" ]; then
    echo "judge_gate: FAIL — $reached obligation events is below the floor of $REACHED_FLOOR: a run that judged almost nothing is not a hold"; exit 1
fi
if [ $((claimgoals + claimchk)) -lt "$CLAIM_FLOOR" ]; then
    echo "judge_gate: FAIL — $((claimgoals + claimchk)) claims checked at run time is below the floor of $CLAIM_FLOOR: claims are stated where no run evaluates them"; exit 1
fi
if [ "$implied" -lt "$IMPLIED_FLOOR" ]; then
    echo "judge_gate: FAIL — $implied VCs implied is below the floor of $IMPLIED_FLOOR: the generator stopped stating a fact it stated"; exit 1
fi
echo "judge_gate: hold — every hypothesis the generator states is true where the program runs, and $implied VCs are implied"
