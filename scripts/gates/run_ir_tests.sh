#!/usr/bin/env bash
# run_ir_tests.sh — build and run the sovereign IR-analysis unit tests (hand-built IR, so
# they exercise the DETECTION direction the corpus can't: the old sema exit()s on the
# fail-tests before the new passes could run). Companion to phase3_differential.sh, which
# covers the no-over-rejection direction over the real corpus.
#
# It also holds the proof engine's judge (src/judge/): the include rules between the trusted judge
# and the untrusted solver and finder (judge_includes.py), and the unit tests of the judge's text
# form (test_judge_format), of its VC generator over a hand-built IR (test_judge_gen), of its
# kernel against good and bad certificates (test_judge_kernel), and of its entry point with every
# kind of wrong answer stubbed in (test_judge_module). The generator's corpus check, every
# hypothesis true at run time, and the kernel's, every certificate of the corpus, are judge_gate.sh.
#
#   bash run_ir_tests.sh          # exit 0 = all IR analysis unit tests pass
set -u
cd "$(dirname "$0")/../.."
# Each binary is built in this run's temp dir. At /tmp/$t, two runs from two trees could each
# build test_octagon and run the OTHER's: its 24 s run is a wide window (see fuzz_vra.sh).
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0
echo "── judge include rules ──"
python3 scripts/gates/judge_includes.py . || fail=1
for t in test_octagon test_vra test_place test_linearity test_borrow test_definite_init test_ir test_callgraph test_judge_format test_rank test_judge_gen test_judge_kernel test_judge_module; do
    src="src/tools/$t.c"
    if [ ! -f "$src" ]; then echo "MISSING: $t"; fail=1; continue; fi
    # test_octagon enumerates a 13^3 box 40,000 times against gamma. Unoptimised that
    # is 2m07s; at -O2 it is 24s, so it gets the flag its own header documents.
    opt=""; [ "$t" = "test_octagon" ] && opt="-O2"
    if ! gcc -std=c99 $opt -o "$TMP/$t" "$src" -I src 2>/dev/null; then
        echo "BUILD FAILED: $t"; fail=1; continue
    fi
    echo "── $t ──"
    if ! "$TMP/$t"; then echo "  ^^ $t FAILED"; fail=1; fi
done
# The proof engine's UNTRUSTED solver (src/solver/lia.h), checked from outside: an independent
# kernel and a brute-force oracle written in Python, which share nothing with the C.
if gcc -std=c99 -O2 -o "$TMP/solve_vc" src/tools/solve_vc.c -I src 2>/dev/null; then
    echo "── solver ──"
    python3 scripts/gates/solver_harness.py "$TMP/solve_vc" || { echo "  ^^ solver FAILED"; fail=1; }
else echo "BUILD FAILED: solve_vc"; fail=1; fi
echo "=================================================================="
[ $fail -eq 0 ] && echo "IR analysis unit tests: ALL PASSED" || echo "IR analysis unit tests: FAILURES"
exit $fail
