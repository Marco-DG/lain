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
#   termination  a loop the termination checker could not discharge. Not a numeric-domain
#             capability, so it is reported apart rather than counted against the domain.
#   other     unclassified — if this dominates, the classifier is what needs work
#
# ── WHICH PROGRAMS COUNT ────────────────────────────────────────────────────────────────────
# A `_fail.ln` program is one the corpus asserts must be REJECTED. Its unproven obligations are
# the engine doing exactly what the test demands, so putting them in the same tally as a
# `_pass.ln` program's is measuring a success as a failure. The first version of this survey did
# that and inflated every bucket: 50 of 134 obligations came from asserted-reject programs, and
# `other` — the bucket the number was steering work toward — was 52% of the fail side against 32%
# of the rest. The two sides are now tallied SEPARATELY. The fail side is still printed, because
# it is the audit trail for "the refusal is for the reason the test intends" and because a shift
# there is how an over-strict change announces itself.
#
#   bash scripts/survey/precision_loss.sh [N]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
N="${1:-100000}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
DRV="$TMP/vradrv"
gcc -std=c99 -O2 -o "$DRV" src/tools/vra_driver.c -I src 2>/dev/null || { echo "driver build failed"; exit 2; }

PASS="$TMP/pass"; FAIL="$TMP/fail"; : > "$PASS"; : > "$FAIL"
pfiles=0; ffiles=0
for f in $(find tests examples -name '*.ln' 2>/dev/null | grep -v '/_tmp/' | sort -u | head -"$N"); do
    out=$("$DRV" "$f" --loss --suppress 2>/dev/null | grep '^LOSS') || true
    [ -z "$out" ] && continue
    case "$f" in
        *_fail.ln) ffiles=$((ffiles+1)); echo "$out" | awk -v F="$f" '{print F"\t"$2"\t"$3" "$4}' >> "$FAIL" ;;
        *)         pfiles=$((pfiles+1)); echo "$out" | awk -v F="$f" '{print F"\t"$2"\t"$3" "$4}' >> "$PASS" ;;
    esac
done

report() {              # $1 = file, $2 = heading, $3 = program count
    local tot; tot=$(wc -l < "$1")
    echo "------------------------------------------------------------------"
    printf "  %s\n" "$2"
    printf "    programs: %-6s obligations: %s\n" "$3" "$tot"
    [ "$tot" -eq 0 ] && return
    cut -f2 "$1" | sort | uniq -c | sort -rn | while read -r n cat; do
        pct=$(( n * 100 / tot ))
        printf "    %-15s %5d  %3d%%  %s\n" "$cat" "$n" "$pct" "$(printf '#%.0s' $(seq 1 $(( pct / 2 + 1 ))))"
    done
    echo "    by obligation kind:"
    cut -f2,3 "$1" | sort | uniq -c | sort -rn | head -10 | while read -r n cat kind; do
        printf "      %-15s %-16s %5d\n" "$cat" "$kind" "$n"
    done
}

echo "=================================================================="
echo "A1 — where the numeric engine loses a proof"
report "$PASS" "★ PRECISION QUESTIONS — programs the corpus asserts COMPILE" "$pfiles"
report "$FAIL" "CORRECT REFUSALS — programs the corpus asserts are REJECTED" "$ffiles"
echo "=================================================================="
echo "Only the first block orders Stage B. The second is an audit trail: every line in it is a"
echo "test that WANTS a refusal, so a bucket growing there is not a regression by itself."
