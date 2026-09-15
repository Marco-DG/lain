#!/usr/bin/env bash
# reverse_differential.sh — the measurement nobody had run (plan MEASURE-0).
#
# Every divergence study so far asked: which programs does LEGACY accept and SOVEREIGN refuse?
# That direction finds OVER-REJECTION. This asks the opposite:
#
#     legacy REJECTS  ∧  sovereign ACCEPTS      =  a guarantee the new engine LOST
#
# A lost guarantee is a silent unsoundness; an over-rejection is a visible refusal. No gate can
# see this class, because both engines pass the corpus: the old engine's verdict is only
# consulted where the new one declines to speak. D-31 was found in it by accident.
set -u
cd "$(dirname "$0")/../.."
LAIN=./lain
[ -x "$LAIN" ] || { echo "build first: make"; exit 1; }

lost=0 both_rej=0 both_acc=0 over=0 total=0
> /tmp/revdiff_lost.txt
for f in $(find tests -name '*.ln' | sort); do
    total=$((total+1))
    "$LAIN" --engine=legacy "$f" >/dev/null 2>&1; L=$?
    "$LAIN" --engine=ir     "$f" >/dev/null 2>&1; S=$?
    if   [ $L -ne 0 ] && [ $S -eq 0 ]; then lost=$((lost+1));  echo "$f" >> /tmp/revdiff_lost.txt
    elif [ $L -eq 0 ] && [ $S -ne 0 ]; then over=$((over+1))
    elif [ $L -ne 0 ];                 then both_rej=$((both_rej+1))
    else                                    both_acc=$((both_acc+1)); fi
done

echo "=========================================================="
echo "REVERSE DIFFERENTIAL over $total corpus programs"
echo "=========================================================="
echo "  both accept .................. $both_acc"
echo "  both reject .................. $both_rej"
echo "  legacy accepts, sovereign NOT  $over    (over-rejection — the OLD study)"
echo "  ** legacy REJECTS, sovereign ACCEPTS ** $lost    (LOST GUARANTEES)"
echo
if [ $lost -gt 0 ]; then
    echo "The $lost programs where the sovereign engine may have lost a guarantee:"
    echo "  (a _fail test here is DAMNING: the corpus asserts it must be refused)"
    while read -r p; do
        tag=""; case "$p" in *_fail.ln) tag="  <-- ASSERTED-REJECT";; esac
        printf "  %s%s\n" "$p" "$tag"
        "$LAIN" --engine=legacy "$p" 2>&1 | grep -oE '^\[E[0-9]+\]' | head -1 | sed 's/^/        legacy says: /'
    done < /tmp/revdiff_lost.txt
fi

# ── the corpus is the WRONG population for this question ─────────────────────────
# Both engines were tuned against it, so a lost guarantee that no corpus program exercises is
# invisible there — which is exactly how D-31 survived (no corpus program held a resource in an
# array until one was added ON FINDING it). The population that matters is programs neither
# engine has seen. Pass `fuzz N` to run the same question over N generated programs per
# generator.
if [ "${1:-}" = "fuzz" ]; then
    N=${2:-200}
    echo; echo "=========================================================="
    echo "REVERSE DIFFERENTIAL over GENERATED programs ($N per generator)"
    echo "=========================================================="
    tmp=$(mktemp -d); glost=0; gtot=0; > /tmp/revdiff_fuzz_lost.txt
    for gen in fuzz_combo fuzz_linear fuzz_borrow; do
        [ -f "scripts/fuzz/$gen.py" ] || continue
        glost_g=0
        for i in $(seq 1 "$N"); do
            p="$tmp/$gen-$i.ln"
            python3 "scripts/fuzz/$gen.py" "$i" > "$p" 2>/dev/null || continue
            [ -s "$p" ] || continue
            gtot=$((gtot+1))
            "$LAIN" --engine=legacy "$p" >/dev/null 2>&1; L=$?
            "$LAIN" --engine=ir     "$p" >/dev/null 2>&1; S=$?
            if [ $L -ne 0 ] && [ $S -eq 0 ]; then
                glost=$((glost+1)); glost_g=$((glost_g+1)); echo "$p" >> /tmp/revdiff_fuzz_lost.txt
            fi
        done
        printf "  %-14s %4d programs, %d where legacy rejects and sovereign accepts\n" "$gen" "$N" "$glost_g"
    done
    echo
    echo "  total generated: $gtot    LOST-GUARANTEE candidates: $glost"
    # A zero here means NOTHING without its coverage. Print the cost beside the count: if the
    # generators never produce the shape a defect lives in, "0 found" is a statement about the
    # generators. Measured 2026-09-15: of 180 programs, 65 declared a linear type and 69 declared
    # an array, and ZERO crossed the two -- which is precisely D-31's shape.
    lin_n=0; arr_n=0; cross_n=0
    for f in "$tmp"/*.ln; do
        [ -f "$f" ] || continue
        grep -q 'mov ' "$f" && lin_n=$((lin_n+1))
        grep -qE '[A-Za-z_]+\[[0-9]+\]' "$f" && arr_n=$((arr_n+1))
        lt=$(grep -oE '^type ([A-Za-z_]+)' "$f" 2>/dev/null | awk '{print $2}' | head -1)
        if [ -n "$lt" ] && grep -q 'mov ' "$f" && grep -qE "$lt\[[0-9]+\]" "$f"; then cross_n=$((cross_n+1)); fi
    done
    echo
    echo "  COVERAGE -- read this before believing the number above:"
    echo "    programs declaring a linear type ......... $lin_n"
    echo "    programs declaring an array .............. $arr_n"
    echo "    programs crossing the two ................ $cross_n   <-- D-31's shape"
    [ "$cross_n" -eq 0 ] && echo "    ** the generators do not produce this shape, so a defect in it CANNOT be found here **"
    if [ $glost -gt 0 ]; then
        echo; echo "  first 10, with what legacy says:"
        head -10 /tmp/revdiff_fuzz_lost.txt | while read -r p; do
            printf "    %s\n" "$(basename "$p")"
            "$LAIN" --engine=legacy "$p" 2>&1 | grep -oE '^\[E[0-9]+\][^|]{0,60}' | head -1 | sed 's/^/        /'
        done
        echo; echo "  kept in $tmp"
    else
        rm -rf "$tmp"
    fi
fi
