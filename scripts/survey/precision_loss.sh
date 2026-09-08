#!/usr/bin/env bash
# precision_loss.sh — A1. For every obligation the numeric engine cannot discharge, say WHICH
# CAPABILITY was missing, and tally it.
#
# An unproven obligation is not evidence for anything until you know why. Three precision
# questions in a row were answered by bisecting programs instead of reading the abstract state.
# The categories map 1:1 onto the Stage B items in REBUILD.md, so this ORDERS that stage
# instead of confirming a guess about it:
#
#   product   loop-carried, NON-constant step (`s = s + a[i]`). Needs s0 + T*delta, a PRODUCT,
#             unstatable in any relational domain      -> B1 loop summarisation
#   widen     loop-carried, CONSTANT step. The domain should reach this; widening lost it
#             -> a precision bug, not a missing capability
#   call      an operand comes from a call             -> B4 postcondition summaries
#   arity     three or more distinct symbolic values   -> B5 (octagons are binary)
#   join      defined at a merge: a disjunction hulled -> B2 partitioning
#   other     unclassified — if this dominates, the classifier is what needs work
#
#   bash scripts/survey/precision_loss.sh [N]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
N="${1:-100000}"
DRV="$(mktemp -d)/vradrv"; trap 'rm -rf "$(dirname "$DRV")"' EXIT
gcc -std=c99 -O2 -o "$DRV" src/analysis/vra_driver.c -I src 2>/dev/null || { echo "driver build failed"; exit 2; }

TALLY="$(mktemp)"; BYKIND="$(mktemp)"; trap 'rm -f "$TALLY" "$BYKIND"' EXIT
files=0
for f in $(find tests examples -name '*_pass.ln' -o -name '*.ln' 2>/dev/null | grep -v '/_tmp/' | sort -u | head -"$N"); do
    out=$("$DRV" "$f" --loss --suppress 2>/dev/null | grep '^LOSS') || true
    [ -z "$out" ] && continue
    files=$((files+1))
    echo "$out" | cut -f2 >> "$TALLY"
    echo "$out" | cut -f2,3 >> "$BYKIND"
done

tot=$(wc -l < "$TALLY")
echo "=================================================================="
echo "A1 — where the numeric engine loses a proof"
echo "  programs with at least one unproven obligation : $files"
echo "  unproven obligations                           : $tot"
echo "------------------------------------------------------------------"
if [ "$tot" -gt 0 ]; then
    sort "$TALLY" | uniq -c | sort -rn | while read -r n cat; do
        pct=$(( n * 100 / tot ))
        printf "  %-10s %5d  %3d%%  %s\n" "$cat" "$n" "$pct" "$(printf '#%.0s' $(seq 1 $(( pct / 2 + 1 ))))"
    done
    echo "------------------------------------------------------------------"
    echo "  by obligation kind:"
    sort "$BYKIND" | uniq -c | sort -rn | head -12 | while read -r n cat kind; do
        printf "    %-10s %-16s %5d\n" "$cat" "$kind" "$n"
    done
fi
echo "=================================================================="
