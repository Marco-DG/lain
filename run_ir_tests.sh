#!/usr/bin/env bash
# run_ir_tests.sh — build and run the sovereign IR-analysis unit tests (hand-built IR, so
# they exercise the DETECTION direction the corpus can't: the old sema exit()s on the
# fail-tests before the new passes could run). Companion to phase3_differential.sh, which
# covers the no-over-rejection direction over the real corpus.
#
#   bash run_ir_tests.sh          # exit 0 = all IR analysis unit tests pass
set -u
cd "$(dirname "$0")"
fail=0
for t in test_vra test_linearity test_borrow; do
    if ! gcc -std=c99 -o "/tmp/$t" "src/analysis/$t.c" -I src 2>/dev/null; then
        echo "BUILD FAILED: $t"; fail=1; continue
    fi
    echo "── $t ──"
    if ! "/tmp/$t"; then echo "  ^^ $t FAILED"; fail=1; fi
done
echo "=================================================================="
[ $fail -eq 0 ] && echo "IR analysis unit tests: ALL PASSED" || echo "IR analysis unit tests: FAILURES"
exit $fail
