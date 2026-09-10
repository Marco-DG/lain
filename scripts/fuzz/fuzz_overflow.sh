#!/usr/bin/env bash
# fuzz_overflow.sh — teeth for prove-or-reject INTEGER OVERFLOW, on the programs that were
# invisible.
#
# The E086 family stops 47 corpus programs in the OLD front end before the sovereign pass runs
# — the largest blocked bucket in phase3's breakdown — so for every one of them the new
# engine's overflow verdict could not be observed at all. B1's false proof of 2026-09-09 (a
# trip count computed from a counter a callee can reset, bounding a total that has no bound)
# lived in exactly that shadow and had to be found by reading code. `g_suppress_overflow`
# opens it; this executes what comes through.
#
# ORACLE: UBSan, and for SIGNED arithmetic it is exact — a signed overflow at runtime in a
# program the engine proved check-free is unsound, with no interpretation needed. Every
# generated function is called with the EXTREMES of its declared parameter ranges, so a proof
# that is wrong at the boundary is exercised rather than merely possible.
#
# HONEST LIMIT: UNSIGNED wraparound is defined behaviour in C, so UBSan does not report it and
# this fuzzer does not cover it. Lain rejects unsigned overflow too, and testing that needs a
# self-checking generator (compute the expected value in a wider type and compare), which is a
# separate build. Only signed types are generated, so the gap is not silently mixed in.
#
# TEETH: making the new engine's narrowing check unconditionally pass turns the `narrow` shape
# into a stream of UNSOUND reports. Verified before this was trusted.
#
#   bash fuzz_overflow.sh [N]
set -u
cd "$(dirname "$0")/../.."
SC="${TMPDIR:-/tmp}/fuzz_ovf.$$"; mkdir -p "$SC"; trap 'rm -rf "$SC"' EXIT
N="${1:-200}"
SEED=${RANDOM_SEED:-$$}
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"

gcc -std=c99 -O2 -o "$SC/vradrv"   src/analysis/vra_driver.c -I src 2>/dev/null || { echo "vradrv build failed"; exit 2; }
gcc -std=c99 -O2 -o "$SC/lowerdrv" src/ir/lower_driver.c     -I src 2>/dev/null || { echo "lowerdrv build failed"; exit 2; }

proven=0; notproven=0; ran=0; unsound=0; skipped=0; brokenc=0
for ((k=0; k<N; k++)); do
    prog="$SC/o.ln"
    python3 scripts/fuzz/fuzz_overflow.py $((SEED + k)) > "$prog" || { skipped=$((skipped+1)); continue; }

    out=$("$SC/vradrv" "$prog" --suppress 2>/dev/null) || { skipped=$((skipped+1)); continue; }
    ovf=$(echo "$out" | grep -c 'arith overflow')
    [ "$ovf" -eq 0 ] && { skipped=$((skipped+1)); continue; }
    # Only a program whose EVERY overflow obligation is discharged makes a promise to check.
    if echo "$out" | grep 'arith overflow' | grep -q 'NOT proven'; then
        notproven=$((notproven+1)); continue
    fi
    proven=$((proven+1))

    "$SC/lowerdrv" "$prog" --emit-c --suppress-ovf --suppress-term > "$SC/o.c" 2>/dev/null || { skipped=$((skipped+1)); continue; }
    gcc -std=c99 -w -fsanitize=undefined -fno-sanitize-recover=undefined \
        -o "$SC/o" "$SC/o.c" $DEFS 2>/dev/null || { brokenc=$((brokenc+1)); cp "$prog" "$SC/brokenc.$k.ln"; continue; }

    ran=$((ran+1))
    err=$(timeout 10 "$SC/o" 2>&1 >/dev/null); rc=$?
    if [ $rc -ne 0 ] || echo "$err" | grep -q 'runtime error'; then
        unsound=$((unsound+1))
        echo "  ★ UNSOUND: proved check-free, OVERFLOWS at runtime"
        echo "$err" | grep -m1 'runtime error' | sed 's/^/      /'
        cp "$prog" "$SC/unsound.$k.ln"; sed 's/^/      /' "$prog"
    fi
done

echo "=============================================================="
echo "fuzz_overflow: gens=$N  proven=$proven  not-proven=$notproven  skipped=$skipped"
echo "  proven AND EXECUTED : $ran   <- the ones actually held to account"
echo "  bugs:  UNSOUND(overflow on a proven program)=$unsound  broken-C=$brokenc"
echo "=============================================================="
[ $unsound -eq 0 ] && [ $brokenc -eq 0 ] && exit 0 || exit 1
