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
# MUST-REFUSE. A dropped obligation leaves no false hypothesis: the interpreter stops at the point
# it was owed, before any fact rests on it. So a program the judge must refuse is solved too: one
# fail test per LANDING site kind (P2.3: a store, the result, a call argument, a struct field, a
# sum payload), and one per ASSERT kind (P2.6: a precondition, a return refinement, a sized slice,
# a struct invariant), each with a value that breaks the rule of its kind. Each must leave a VC of
# that kind not implied (a model), or the judge proves a program that traps. A subslice's end, of a
# slice and of an array, owed at its MAKE_SLICE; TYPE_MIN / -1 at 64 bits, by division and by
# remainder (guards: they held before the subslice rows were added); a vector's shift amount (I.202).
#
# COVERAGE (Main Compiler Work's census): the judge can take VRA's place only where it owes what VRA
# checks. Every test program and probe goes through the driver's --cover: each check VRA makes
# (termination aside) must have an obligation of its class at its instruction. One that has none
# fails the gate unless scripts/gates/judge_cover_allow.txt lists it with its reason, and a listed
# one that is covered fails too, since an exemption expires with its bug. A crash fails the gate:
# that is the harness, not a verdict.
#
# PRECISION, the other half: a hypothesis check cannot see a generator that stops stating a TRUE
# fact. So every program is also JUDGED, through the entry point the compiler calls (judge.h's
# judge_module, by the driver's --judge): the untrusted solver answers each VC, and a VC counts as
# implied only when the trusted KERNEL (src/judge/kernel.h) accepts its certificate. A certificate
# it refuses, a model that fails, or a claim set that is not certified fails the gate (a solver,
# proposer or kernel bug). Two canaries keep the wiring honest: judge_check must refuse a bad
# certificate, and --judge-corrupt, which damages every certificate, must prove nothing. The gate reports how many are implied, failing below a
# floor. scripts/gates/solver_harness.py checks the solver against a second, independent kernel.
# A floor over the corpus has slack, so the judge's OWN programs, tests/trust/judge_*_pass.ln,
# each written for one rule, must have EVERY VC implied: one that is not is a rule that stopped
# working. They must be, too, from the judge's own candidates alone (`--no-finder`: no proof may
# need VRA's precision), except a program written for the finder (judge_*finder*_pass.ln).
#
#   bash scripts/gates/judge_gate.sh
set -u
cd "$(dirname "$0")/../.."
REACHED_FLOOR=100000         # obligation events; 287,380 when the gate was written (19473f7)
IMPLIED_FLOOR=10650      # obligations' VCs implied over the corpus; 10,722 of 10,940 when the floor was set (the ASSERT obligations). Lower it only with the reason
EDGE_FLOOR=50000         # claims checked at run time on the edges they are owed on; 260,682 when the floor was set
RETURN_FLOOR=4000        # return claims checked at run time where the program returns; 9,065 when the floor was set
CONTRACT_FLOOR=2000      # return claims checked at run time where a caller uses them; 4,096 when the floor was set
USE_FLOOR=100000         # claims checked at run time where they are used; 1,215,712 when the floor was set
MUST_REFUSE="tests/vra/overflow/field_assign_overflow_fail.ln:narrow tests/vra/overflow/return_overflow_fail.ln:narrow
             tests/vra/overflow/arg_literal_overflow_fail.ln:narrow tests/vra/overflow/struct_field_literal_overflow_fail.ln:narrow
             tests/vra/overflow/enum_payload_overflow_fail.ln:narrow tests/functions/fixed_array_len_precondition_fail.ln:assert.0
             tests/errors/return_refinement_param_fail.ln:assert.86 tests/vra/sized_param_constant_length_fail.ln:assert.87
             tests/vra/invariant/slice_len_bound_store_fail.ln:assert.121
             tests/vra/bounds/subslice_oob_fail.ln:bounds tests/types/sub_slice_bounds_fail.ln:bounds
             tests/types/div_i64_min_by_minus_one_fail.ln:divovf tests/types/rem_i64_min_by_minus_one_fail.ln:divovf
             tests/vra/overflow/simd_shift_amount_fail.ln:shift"
REFUSED_FLOOR=150        # programs the prover refuses that ran and reached an obligation; 197 when the floor was set
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
if ! gcc -std=c99 -O2 -w -o "$T/jvc" src/tools/judge_vc_driver.c -I src 2> "$T/build.err" ||
   ! gcc -std=c99 -O2 -w -o "$T/judge_check" src/tools/judge_check.c -I src 2>> "$T/build.err"; then
    echo "judge_gate: the driver or the solver does not build:"; head -5 "$T/build.err"; exit 1
fi
# the kernel's wiring, checked where it runs: judge_check must refuse a certificate the kernel
# refuses (a negative multiplier on an inequality), or the gate would count a refused one implied
canary=0
"$T/judge_check" scripts/gates/judge_canary/negative_multiplier.vc scripts/gates/judge_canary/negative_multiplier.ans > "$T/canary.jc" 2>&1
if [ $? -ne 1 ] || ! grep -q '^REFUSED .*negative multiplier' "$T/canary.jc"; then
    canary=1; echo "  THE KERNEL'S CANARY WAS NOT REFUSED (judge_check accepted a certificate it must refuse):"; sed 's/^/      /' "$T/canary.jc"
fi
# and the entry point's: every certificate damaged before the judge reads it, nothing proved
"$T/jvc" tests/trust/judge_callee_contracts_pass.ln --judge-corrupt --no-claims > "$T/canary2.rec" 2>&1
if ! grep -qE '^# judge proved=0 .*solver-bug=[1-9]' "$T/canary2.rec"; then
    canary=1; echo "  THE JUDGE'S CANARY: judge_module counted a damaged certificate:"; tail -1 "$T/canary2.rec" | sed 's/^/      /'
fi
find tests -name '*_pass.ln' | sort > "$T/files.txt"
declared=$(wc -l < "$T/files.txt")
# judge_module's records (the driver's --judge) as one verdict line per VC, named by its id:
# `implied`, `not-implied`, `budget`, `capacity`, or `REFUSED` for what the judge cannot show (a
# solver bug, unstable claims), and for a function it refuses or a proposer that failed
recs() {
    awk '/^v1 / && $3 != "-" { v = $7; if (v == "proved") v = "implied"; else if (v == "solver-bug" || v == "uncertified") v = "REFUSED"
                                print v " " $2 " " $3 " " $4 " " $5 " " $6 }
         /^(refused|no-claims)/ { print "REFUSED " $0 }' "$1"
}
one() {
    local f="$1" o rc; o=$(echo "$f" | tr '/' '_')
    LAIN_INTERP_STEPS=50000000 timeout 120 "$T/jvc" "$f" --check-hyps > "$T/$o.out" 2> "$T/$o.err"
    echo $? > "$T/$o.rc"
    # the judge's own verdicts, from the entry point the compiler calls (judge.h's judge_module): the
    # claims certified first, the solver's certificates through the KERNEL, its models evaluated.
    # Exit 1 is the judge reporting (a solver bug, unstable claims, a refusal); 2 and up, a failure
    timeout 300 "$T/jvc" "$f" --judge > "$T/$o.rec" 2>&1; rc=$?
    echo $rc > "$T/$o.jcrc"; [ $rc -ge 2 ] && echo $rc > "$T/$o.src" || echo 0 > "$T/$o.src"
    recs "$T/$o.rec" > "$T/$o.jc"
    case "$f" in tests/trust/judge_*finder*_pass.ln) ;; tests/trust/judge_*_pass.ln)
        timeout 300 "$T/jvc" "$f" --judge --no-finder > "$T/$o.nf.rec" 2>&1
        recs "$T/$o.nf.rec" > "$T/$o.nf.jc";;
    esac
}
export -f one recs; export T
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
cover_one() {
    local f="$1" o; o=c_$(echo "$f" | tr '/' '_')
    timeout 120 "$T/jvc" "$f" --cover > "$T/$o.cov" 2> /dev/null; echo $? > "$T/$o.crc"
}
export -f cover_one
{ find tests -name '*.ln' | sort; ls scripts/gates/judge_runnable/*.ln; } > "$T/cover.txt"
xargs -P "${LAIN_GATE_JOBS:-4}" -I{} bash -c 'cover_one "{}"' < "$T/cover.txt"
judged=0; nosum=0; refused=0; falsec=0; reached=0; checked=0; solverr=0; implied=0; notimp=0; outbudget=0; krefused=0
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
        if [ "$(cat "$T/$o.src")" != 0 ]; then solverr=$((solverr+1)); echo "  THE JUDGE FAILED TO RUN (rc $(cat "$T/$o.src")): $f"; tail -2 "$T/$o.rec" | sed 's/^/      /'; fi
        # a verdict names its VC: `implied FN IRHASH POINT KIND ORDINAL`; a claim's kind is `hull`,
        # `inv.entry`, `inv.keep` or `post`, and every other kind is an obligation's. What the judge
        # cannot show (a certificate the kernel refuses, a model that fails, unstable claims) is a
        # solver, proposer or kernel bug, never noise
        if [ "$(cat "$T/$o.jcrc")" = 1 ]; then
            n=$(grep -c '^REFUSED ' "$T/$o.jc"); krefused=$((krefused + (n > 0 ? n : 1)))
            echo "  THE JUDGE COULD NOT SHOW AN ANSWER ($n): $f"; grep -m2 '^REFUSED ' "$T/$o.jc" | sed 's/^/      /'
        fi
        h=$(grep -cE '^implied [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post) ' "$T/$o.jc"); hullok=$((hullok + h))
        implied=$((implied + $(grep -c '^implied ' "$T/$o.jc") - h)); notimp=$((notimp + $(grep -c '^not-implied ' "$T/$o.jc") - $(grep -cE '^not-implied [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post) ' "$T/$o.jc")))
        outbudget=$((outbudget + $(grep -cE '^(budget|capacity) ' "$T/$o.jc") - $(grep -cE '^(budget|capacity) [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post) ' "$T/$o.jc")))
        n=$(grep -cE '^(not-implied|budget|capacity|REFUSED) [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post)[ :]' "$T/$o.jc")
        if [ "$n" -gt 0 ]; then hullbad=$((hullbad+n)); echo "  CLAIM NOT IMPLIED ($n): $f"; grep -m2 -E '^(not-implied|budget|capacity|REFUSED) [^ ]+ [^ ]+ [^ ]+ (hull|inv\.entry|inv\.keep|post)[ :]' "$T/$o.jc" | sed 's/^/      /'; fi
        case "$f" in tests/trust/judge_*_pass.ln)
            n=$(grep -cE '^(not-implied|budget|capacity|REFUSED) ' "$T/$o.jc")
            if [ "$n" -gt 0 ] || ! grep -q '^implied ' "$T/$o.jc"; then mustfail=$((mustfail+1)); echo "  MUST-PROVE, $n NOT IMPLIED: $f"; grep -m2 -E '^(not-implied|budget|capacity|REFUSED) ' "$T/$o.jc" | sed 's/^/      /'; fi
            if [ -f "$T/$o.nf.jc" ]; then
                n=$(grep -cE '^(not-implied|budget|capacity|REFUSED) ' "$T/$o.nf.jc")
                if [ "$n" -gt 0 ] || ! grep -q '^implied ' "$T/$o.nf.jc"; then mustfail=$((mustfail+1)); echo "  MUST-PROVE WITHOUT THE FINDER, $n NOT IMPLIED: $f"; grep -m2 -E '^(not-implied|budget|capacity|REFUSED) ' "$T/$o.nf.jc" | sed 's/^/      /'; fi
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
mustrefuse=0
for fk in $MUST_REFUSE; do
    f=${fk%%:*}; k=${fk#*:}; o=m_$(echo "$f" | tr '/' '_')
    timeout 300 "$T/jvc" "$f" --judge > "$T/$o.rec" 2>&1
    kre=${k//./\\.}
    if ! grep -qE "^v1 [^ ]+ [^ ]+ [^ ]+ (${kre}|${kre}\.[^ ]+) [^ ]+ not-implied" "$T/$o.rec"; then
        mustrefuse=$((mustrefuse+1)); echo "  MUST-REFUSE, EVERY $k VC IMPLIED (the judge proves what breaks the rule): $f"
    fi
done
cran=0; ccrash=0; cchecks=0; cstated=0; cout=0; : > "$T/uncovered.txt"
while read -r f; do
    o=c_$(echo "$f" | tr '/' '_')
    if [ "$(cat "$T/$o.crc")" -ge 2 ]; then
        ccrash=$((ccrash+1)); echo "  THE COVERAGE RUN FAILED (rc $(cat "$T/$o.crc"); the harness, not a verdict): $f"; continue
    fi
    s=$(grep -m1 '^COVER' "$T/$o.cov"); [ -n "$s" ] || continue      # refused before lowering: no IR to cover
    cran=$((cran+1))
    c=$(echo "$s" | sed -nE 's/.*checks ([0-9]+).*/\1/p'); cchecks=$((cchecks + ${c:-0}))
    c=$(echo "$s" | sed -nE 's/.*stated ([0-9]+).*/\1/p'); cstated=$((cstated + ${c:-0}))
    c=$(echo "$s" | sed -nE 's/.*outside ([0-9]+).*/\1/p'); cout=$((cout + ${c:-0}))
    awk -F'\t' -v f="$f" '$1 == "UNCOVERED" { print f "\t" $2 "\t" $3 "\t" $5 }' "$T/$o.cov" >> "$T/uncovered.txt"
done < "$T/cover.txt"
grep -v '^#' scripts/gates/judge_cover_allow.txt | cut -f1-4 | LC_ALL=C sort -u > "$T/allow.txt"
LC_ALL=C sort -u "$T/uncovered.txt" > "$T/unc.txt"
cnew=$(LC_ALL=C comm -23 "$T/unc.txt" "$T/allow.txt" | wc -l); cstale=$(LC_ALL=C comm -13 "$T/unc.txt" "$T/allow.txt" | wc -l)
if [ "$cnew" -gt 0 ]; then echo "  VRA CHECKS THE JUDGE OWES NOTHING FOR ($cnew):"; LC_ALL=C comm -23 "$T/unc.txt" "$T/allow.txt" | head -10 | sed 's/^/      /'; fi
if [ "$cstale" -gt 0 ]; then echo "  ALLOWED, AND COVERED NOW: remove from judge_cover_allow.txt ($cstale):"; LC_ALL=C comm -13 "$T/unc.txt" "$T/allow.txt" | sed 's/^/      /'; fi
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
echo "VCs implied $implied (each certificate accepted by the kernel), not implied $notimp, budget or capacity $outbudget, solver failures $solverr, certificates the kernel refused $krefused (floor $IMPLIED_FLOOR)"
echo "claims $claims: their VCs implied $hullok, not implied $hullbad; at run time, claims checked on $claimgoals edges taken (floor $EDGE_FLOOR) and $claimchk times where used (floor $USE_FLOOR)"
echo "return claims checked at run time at $retgoals returns (floor $RETURN_FLOOR) and $contractchk times at a call (floor $CONTRACT_FLOOR)"
echo "the judge's own programs (tests/trust/judge_*_pass.ln) with a VC not implied, with or without the finder: $mustfail"
echo "fail tests the judge must refuse (a landing, an assert, a subslice, a division, a vector shift): $(echo $MUST_REFUSE | wc -w), with every VC of their kind implied: $mustrefuse"
echo "coverage: $(wc -l < "$T/cover.txt") programs, $cran lowered, $cchecks VRA checks: $cstated with an obligation, $cout outside, $(wc -l < "$T/unc.txt") uncovered ($(wc -l < "$T/allow.txt") allowed), $cnew not allowed, $cstale stale, $ccrash runs failed"
echo "programs the prover refuses: $rdecl; ran $rran (floor $REFUSED_FLOOR), skipped $((rnosum + rnothing)) ($rnosum cannot run, $rnothing reach no obligation), crashed $rcrash; hypotheses checked $rchecked, false $rfalse, generator refusals $rrefused"
if [ "$nosum" -gt 0 ] || [ "$refused" -gt 0 ] || [ "$falsec" -gt 0 ] || [ "$judged" -ne "$declared" ] || [ "$solverr" -gt 0 ] || [ "$krefused" -gt 0 ] || [ "$canary" -gt 0 ] || [ "$hullbad" -gt 0 ] || [ "$mustfail" -gt 0 ] || [ "$mustrefuse" -gt 0 ] || [ "$rfalse" -gt 0 ] || [ "$rrefused" -gt 0 ] || [ "$cnew" -gt 0 ] || [ "$cstale" -gt 0 ] || [ "$ccrash" -gt 0 ]; then
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
