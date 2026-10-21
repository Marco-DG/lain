# judge_leg.sh — sourced by fuzz_vra.sh, fuzz_overflow.sh and fuzz_termination.sh: their JUDGE leg.
#
# Those three decide through vra_driver and lower_driver, never through `lain`. Since PE.9's flip
# (2426d11) the judge decides by default, so a program only the judge proves was never executed by
# any of them, and the one judge exposure left (fuzz_overflow's `lain --interpret`) ran only
# programs VRA had proved already. Same seed, same programs, before and after the flip: identical
# reports by construction.
#
# This leg asks the shipping compiler. Every generated program is compiled by `lain` (the judge,
# by default), whatever the VRA leg concluded. A program it accepts makes the promise the VRA leg's
# proven programs make: its C is held to the fuzzer's own oracle. `lain --interpret` checks every
# proof the judge discharged as the program uses it. A program accepted here that the VRA leg did
# not prove is counted, and named, as ONLY THE JUDGE's.
#
# The VRA legs are unchanged: their counts and verdicts are what they were.

jl_acc=0; jl_only=0; jl_refused=0; jl_internal=0; jl_ran=0; jl_brokenc=0; jl_interp=0; jl_onlyids=""
JL_LAIN="${LAIN:-./lain}"
[ -x "$JL_LAIN" ] || { echo "judge leg: no $JL_LAIN (build lain first)"; exit 2; }

# jl_compile PROG VRA ID OUT.c: 0 when `lain` accepts PROG (C in OUT.c), 1 otherwise. VRA is the
# VRA leg's verdict: 1 proven, 0 not proven, - no verdict (the VRA leg skipped the program).
# A refusal is exit 1; any other exit (70, a signal, the kill) is an INTERNAL failure, a bug.
jl_compile() {
    local prog=$1 vra=$2 id=$3 out=$4 rc
    timeout -s KILL 120 "$JL_LAIN" "$prog" -o "$out" >/dev/null 2>"$out.err"; rc=$?
    if [ $rc -eq 1 ]; then jl_refused=$((jl_refused+1)); return 1; fi
    if [ $rc -ne 0 ]; then
        jl_internal=$((jl_internal+1))
        echo "  ★ JUDGE INTERNAL: $id — lain exit $rc: $(grep -v '^\s*$' "$out.err" | head -1 | cut -c1-140)"
        return 1
    fi
    jl_acc=$((jl_acc+1))
    if [ "$vra" != 1 ]; then jl_only=$((jl_only+1)); jl_onlyids="$jl_onlyids $id"; fi
    return 0
}

# jl_interpret PROG ID OUT: run PROG on the interpreter under the judge's decisions (stdout to OUT).
# 0 clean; 1 a discharged proof failed or UB (counted, reported); 2 not modelled or the step budget.
jl_interpret() {
    local prog=$1 id=$2 out=$3 m
    LAIN_INTERP_STEPS=50000000 timeout -s KILL 60 "$JL_LAIN" "$prog" --interpret >"$out" 2>"$out.err"
    m=$(grep -m1 'lain --interpret' "$out.err")
    if echo "$m" | grep -q 'PROOF FAILED\|UNDEFINED BEHAVIOUR'; then
        jl_interp=$((jl_interp+1))
        echo "  ★ JUDGE UNSOUND (interpreter): $id — $m"
        return 1
    fi
    echo "$m" | grep -q 'NOT MODELLED\|STEP BUDGET' && return 2
    return 0
}

# jl_report BUGS...: the leg's summary lines (the first 20 judge-only programs named)
jl_report() {
    echo "  judge leg (lain, the default): accepted=$jl_acc  refused=$jl_refused  executed=$jl_ran"
    local ids; ids=$(echo $jl_onlyids | tr ' ' '\n' | head -20 | tr '\n' ' ')
    [ "$jl_only" -gt 20 ] && ids="$ids…"
    echo "    only the judge proves: $jl_only${ids:+  (${ids% })}"
    echo "    bugs:  $*  interpreter=$jl_interp  internal=$jl_internal  broken-C=$jl_brokenc"
}
jl_bugs() { [ $jl_interp -eq 0 ] && [ $jl_internal -eq 0 ] && [ $jl_brokenc -eq 0 ]; }
