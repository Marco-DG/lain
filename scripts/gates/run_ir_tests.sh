#!/usr/bin/env bash
# run_ir_tests.sh — build and run the sovereign IR-analysis unit tests (hand-built IR, so
# they exercise the DETECTION direction the corpus can't: the old sema exit()s on the
# fail-tests before the new passes could run). Companion to phase3_differential.sh, which
# covers the no-over-rejection direction over the real corpus.
#
#   bash run_ir_tests.sh          # exit 0 = all IR analysis unit tests pass
set -u
cd "$(dirname "$0")/../.."
fail=0
for t in test_octagon test_vra test_place test_linearity test_borrow test_definite_init test_ir; do
    src="src/tools/$t.c"
    if [ ! -f "$src" ]; then echo "MISSING: $t"; fail=1; continue; fi
    # test_octagon enumerates a 13^3 box 40,000 times against gamma. Unoptimised that
    # is 2m07s; at -O2 it is 24s, so it gets the flag its own header documents.
    opt=""; [ "$t" = "test_octagon" ] && opt="-O2"
    if ! gcc -std=c99 $opt -o "/tmp/$t" "$src" -I src 2>/dev/null; then
        echo "BUILD FAILED: $t"; fail=1; continue
    fi
    echo "── $t ──"
    if ! "/tmp/$t"; then echo "  ^^ $t FAILED"; fail=1; fi
done
echo "=================================================================="
[ $fail -eq 0 ] && echo "IR analysis unit tests: ALL PASSED" || echo "IR analysis unit tests: FAILURES"
exit $fail
