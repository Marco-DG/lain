#!/usr/bin/env bash
# phase3_differential.sh — the Phase 3 parity gate. Runs the new sovereign IR analyses
# (effects, linearity, borrow) against the old AST engine over the corpus and asserts the
# two soundness directions that make them safe to eventually make authoritative (3.5):
#
#   A. no-over-rejection : on every program the OLD engine ACCEPTS (a *_pass file that loads
#      cleanly), the new linearity+borrow passes must raise 0 findings. A single finding
#      here is a false positive — the new engine rejecting valid code.
#   B. effect soundness  : the new effect row must never DROP an observable effect
#      (Write/IO/Raises) the old engine reports (Diverge may differ — the new VRA is more
#      precise). effects_driver.c already encodes this; we just tally UNSOUND lines.
#
# Detection (the passes actually FIRE on violations) is proven by the hand-built IR unit
# tests test_linearity.c / test_borrow.c, because the old sema exit()s on the fail-tests
# before the new passes could run — so the corpus can only measure direction A here.
#
#   bash phase3_differential.sh        # exit 0 = parity holds
set -u
cd "$(dirname "$0")"
LINDRV=/tmp/lindrv EFFDRV=/tmp/effdrv
gcc -std=c99 -o "$LINDRV" src/analysis/linearity_driver.c -I src 2>/dev/null || { echo "build lindrv failed"; exit 2; }
gcc -std=c99 -o "$EFFDRV" src/analysis/effects_driver.c   -I src 2>/dev/null || { echo "build effdrv failed"; exit 2; }

fp=0 fp_files="" scanned=0 uns=0
while IFS= read -r f; do
    case "$f" in *_fail.ln) continue;; esac
    "$LINDRV" "$f" 2>&1 >/dev/null | grep -q "Cannot open" && continue     # skip driver module-path misses
    scanned=$((scanned+1))
    "$LINDRV" "$f" --quiet >/dev/null 2>/dev/null || { fp=$((fp+1)); fp_files="$fp_files $f"; }
    u=$("$EFFDRV" "$f" 2>&1 | grep -cE "^UNSOUND ")
    uns=$((uns+u))
done < <(find tests -name '*_pass.ln' | sort)

echo "=================================================================="
echo "Phase 3 differential (new sovereign IR analyses vs old AST engine)"
echo "  A. linearity+borrow false positives (must be 0): $fp   over $scanned pass-programs"
echo "  B. effect observable-effect drops   (must be 0): $uns"
echo "=================================================================="
[ -n "$fp_files" ] && { echo "false-positive files:"; for x in $fp_files; do echo "  $x"; done; }
[ "$fp" -eq 0 ] && [ "$uns" -eq 0 ] && { echo "PARITY HOLDS (0 false positives, 0 dropped effects)"; exit 0; }
echo "PARITY BROKEN"; exit 1
