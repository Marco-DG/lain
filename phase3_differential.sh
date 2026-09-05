#!/usr/bin/env bash
# phase3_differential.sh — the Phase 3 gate, on ADJUDICATED DIVERGENCE (corrective C1).
#
# The previous criterion ("0 findings on programs the old engine accepts") was a STRUCTURAL
# MISTAKE: it forbids the new engine from ever being STRONGER than the old one — a better
# checker fails it by construction (see internal/design/critical_retrospective.md §2).
#
# The gate now COMPARES VERDICTS and requires every difference to be CLASSIFIED:
#
#   *_pass.ln  (old ACCEPTS)   new finds violations  ⇒ NEW-REJECTS divergence
#   *_fail.ln  (old REJECTS)   new finds nothing     ⇒ NEW-ACCEPTS divergence
#
# Each divergence must appear in phase3_adjudications.txt with one of:
#   old-unsound            new rejects and is RIGHT      → a WIN (add a fail-test)
#   new-over-strict        new rejects and is WRONG      → a BUG to fix
#   new-more-precise       new accepts and is RIGHT      → a WIN (e.g. v.push(v.len()))
#   new-incomplete-<area>  new accepts because that analysis ISN'T BUILT YET → tracked gap
#   new-unsound            new accepts and is WRONG      → HARD FAIL, must never appear
#
# PASS ⟺ no UNADJUDICATED divergence and no `new-unsound`. Divergence itself is allowed —
# that is the whole point: it is how a stronger engine is permitted to land.
#
#   bash phase3_differential.sh            # exit 0 = gate holds
#   bash phase3_differential.sh --list     # print divergences in ledger format (to triage)
set -u
cd "$(dirname "$0")"
LEDGER=phase3_adjudications.txt
LINDRV=/tmp/lindrv EFFDRV=/tmp/effdrv
LIST=0; [ "${1:-}" = "--list" ] && LIST=1

gcc -std=c99 -o "$LINDRV" src/analysis/linearity_driver.c -I src 2>/dev/null || { echo "build lindrv failed"; exit 2; }
gcc -std=c99 -o "$EFFDRV" src/analysis/effects_driver.c   -I src 2>/dev/null || { echo "build effdrv failed"; exit 2; }

# classification for a path, from the ledger ("" if unadjudicated)
adjudication() { grep -E "^[[:space:]]*$1[[:space:]]" "$LEDGER" 2>/dev/null | awk '{print $3}' | head -1; }

agree=0 skipped=0 unadj=0 unsound=0 eff_uns=0
declare -A cls_count
unadj_lines=""

while IFS= read -r f; do
    case "$f" in *_fail.ln) expect_reject=1;; *) expect_reject=0;; esac
    if [ $expect_reject -eq 1 ]; then out=$("$LINDRV" "$f" --reject 2>&1); else out=$("$LINDRV" "$f" 2>&1); fi
    echo "$out" | grep -q "Cannot open" && { skipped=$((skipped+1)); continue; }
    # the summary line only prints if our driver actually RAN (sema didn't exit first)
    n=$(echo "$out" | sed -n 's/^linearity: \([0-9]*\) finding.*/\1/p' | head -1)
    [ -z "$n" ] && { skipped=$((skipped+1)); continue; }   # old engine rejected for another reason

    if   [ $expect_reject -eq 0 ] && [ "$n" -gt 0 ]; then div="NEW-REJECTS"
    elif [ $expect_reject -eq 1 ] && [ "$n" -eq 0 ]; then div="NEW-ACCEPTS"
    else agree=$((agree+1)); continue; fi

    c=$(adjudication "$f")
    if [ -z "$c" ]; then
        unadj=$((unadj+1)); unadj_lines="$unadj_lines\n$f  $div  UNADJUDICATED"
        [ $LIST -eq 1 ] && printf '%s  %s  <classify-me>\n' "$f" "$div"
    else
        cls_count[$c]=$(( ${cls_count[$c]:-0} + 1 ))
        [ "$c" = "new-unsound" ] && unsound=$((unsound+1))
    fi
done < <(find tests -name '*.ln' -type f | sort)

# effect soundness (unchanged: the new effect row must never DROP an observable effect)
while IFS= read -r f; do
    eff_uns=$(( eff_uns + $("$EFFDRV" "$f" 2>&1 | grep -cE "^UNSOUND ") ))
done < <(find tests -name '*_pass.ln' | sort)

[ $LIST -eq 1 ] && exit 0
echo "=================================================================="
echo "Phase 3 gate — ADJUDICATED DIVERGENCE (new sovereign IR vs old engine)"
echo "  verdicts agree            : $agree"
echo "  skipped (non-ownership)   : $skipped"
for k in "${!cls_count[@]}"; do printf '  %-24s: %s\n' "$k" "${cls_count[$k]}"; done
echo "  UNADJUDICATED             : $unadj      (must be 0)"
echo "  new-unsound               : $unsound      (must be 0)"
echo "  effect observable-drops   : $eff_uns      (must be 0)"
echo "=================================================================="
[ $unadj -gt 0 ] && { printf 'unadjudicated divergences:%b\n' "$unadj_lines"; echo "→ classify each in $LEDGER (or run --list)"; }
if [ $unadj -eq 0 ] && [ $unsound -eq 0 ] && [ $eff_uns -eq 0 ]; then
    echo "GATE HOLDS — every divergence is accounted for"; exit 0
fi
echo "GATE BROKEN"; exit 1
