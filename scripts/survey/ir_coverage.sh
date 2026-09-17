#!/usr/bin/env bash
# ir_coverage.sh — how much of the corpus does the IR actually MODEL? (corrective C3)
#
# `IrFunc.incomplete` is set when lowering drops or placeholders a construct. It is SOUND
# (all proofs over that function are suppressed) but it silently EXCLUDES the function from
# every metric. So "523/563 bounds proven" and "0 false positives over 340 programs" are
# both implicitly conditioned on "...among the functions we actually modelled".
#
# This reports that denominator. Every survey number should be read next to it, and the
# number should climb to 100% as B3 (`opaque`/havoc with declared footprints) retires
# `incomplete` altogether — see REBUILD.md Stage II.
#
#   bash ir_coverage.sh            # corpus-wide
#   bash ir_coverage.sh --worst    # also list the files with the most unmodelled functions
set -u
cd "$(dirname "$0")/../.."
DRV=/tmp/lowerdrv
gcc -std=c99 -o "$DRV" src/tools/lower_driver.c -I src 2>/dev/null || { echo "build lowerdrv failed"; exit 2; }
WORST=0; [ "${1:-}" = "--worst" ] && WORST=1

inc=0 tot=0 files=0 rows=""
while IFS= read -r f; do
    out=$("$DRV" "$f" --coverage 2>/dev/null | grep '^coverage') || true
    [ -z "$out" ] && continue
    i=$(echo "$out" | awk '{print $2}'); t=$(echo "$out" | awk '{print $3}')
    inc=$((inc+i)); tot=$((tot+t)); files=$((files+1))
    [ "$i" -gt 0 ] && rows="$rows$i $f\n"
done < <(find tests -name '*_pass.ln' | sort)

echo "=================================================================="
echo "IR modelling coverage (the denominator under every other metric)"
echo "  files            : $files"
echo "  functions        : $tot"
echo "  modelled         : $((tot-inc))"
echo "  incomplete       : $inc"
[ "$tot" -gt 0 ] && echo "  COVERAGE         : $(( (tot-inc)*100/tot ))%"
echo "=================================================================="
if [ $WORST -eq 1 ] && [ -n "$rows" ]; then
    echo "files with unmodelled functions (count, path):"
    printf '%b' "$rows" | sort -rn | head -20
fi
