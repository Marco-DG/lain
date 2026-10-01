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
# ORACLES, three, because UBSan alone was watching almost nothing (corrected 2026-10-01):
#   UBSan              C-level UB only. The backend widens a narrow arithmetic op one step
#                      (i8 -> int16_t, i16 -> int32_t, i32 -> int64_t), so for those widths the C
#                      add cannot overflow and UBSan CANNOT FIRE. i64 is emitted at its own width
#                      and is the only width where UBSan means anything here.
#   lain --interpret   every obligation the analysis discharged, at the line that owes it. This is
#                      what covers i8/i16/i32.
#   the differential   C stdout vs interpreter stdout. Catches a wrong value even where the C
#                      wraps deterministically instead of committing UB.
# Before this, `UNSOUND=0` for i8/i16/i32 meant only that nothing was watching.
#
# TEETH: making the new engine's narrowing check unconditionally pass turns the `narrow` shape
# into a stream of UNSOUND reports. Verified before this was trusted.
#
# CAVEAT on LAIN=: it overrides the INTERPRETER leg only. vradrv and lowerdrv are built from
# src/tools/*.c in the tree this script runs in, so the `proven` verdict and the emitted C always
# come from THIS tree's source. To test another build's analysis, run the script from inside that
# tree; LAIN= alone will leave the analysis unchanged and the oracles inert.
#
#   bash fuzz_overflow.sh [N]
set -u
cd "$(dirname "$0")/../.."
SC="${TMPDIR:-/tmp}/fuzz_ovf.$$"; mkdir -p "$SC"; trap 'rm -rf "$SC"' EXIT
N="${1:-200}"
SEED=${RANDOM_SEED:-$$}
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"
LAIN="${LAIN:-./lain}"      # the interpreter leg; skipped with a warning if absent

gcc -std=c99 -O2 -o "$SC/vradrv"   src/tools/vra_driver.c -I src 2>/dev/null || { echo "vradrv build failed"; exit 2; }
gcc -std=c99 -O2 -o "$SC/lowerdrv" src/tools/lower_driver.c     -I src 2>/dev/null || { echo "lowerdrv build failed"; exit 2; }

proven=0; notproven=0; ran=0; unsound=0; skipped=0; brokenc=0; interp_bug=0; differ=0; nointerp=0
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
    err=$(timeout 10 "$SC/o" 2>"$SC/c.err" >"$SC/c.out"; echo $? > "$SC/c.st"; cat "$SC/c.err"); rc=$(cat "$SC/c.st")
    if [ "$rc" -ne 0 ] || grep -q 'runtime error' "$SC/c.err"; then
        unsound=$((unsound+1))
        echo "  ★ UNSOUND (UBSan): proved check-free, OVERFLOWS at runtime"
        grep -m1 'runtime error' "$SC/c.err" | sed 's/^/      /'
        cp "$prog" "$SC/unsound.$k.ln"; sed 's/^/      /' "$prog"
        continue
    fi

    # ── oracle 2+3: the interpreter, and the two legs' printed values ───────────────────────
    # These cover i8/i16/i32, where UBSan structurally cannot fire.
    if [ ! -x "$LAIN" ]; then nointerp=$((nointerp+1)); continue; fi
    LAIN_INTERP_STEPS=50000000 timeout 60 "$LAIN" "$prog" --interpret >"$SC/i.out" 2>"$SC/i.err"; ist=$?
    m=$(grep -m1 'lain --interpret' "$SC/i.err")
    if echo "$m" | grep -q 'PROOF FAILED\|UNDEFINED BEHAVIOUR'; then
        interp_bug=$((interp_bug+1))
        echo "  ★ UNSOUND (interpreter): $m"
        cp "$prog" "$SC/interp.$k.ln"; sed 's/^/      /' "$prog"
    elif echo "$m" | grep -q 'NOT MODELLED\|STEP BUDGET'; then
        :
    elif ! cmp -s "$SC/c.out" "$SC/i.out"; then
        differ=$((differ+1))
        echo "  ★ UNSOUND (differential): C printed '$(head -c 48 "$SC/c.out" | tr '\n' ' ')' but the interpreter printed '$(head -c 48 "$SC/i.out" | tr '\n' ' ')'"
        cp "$prog" "$SC/differ.$k.ln"; sed 's/^/      /' "$prog"
    fi
done

echo "=============================================================="
echo "fuzz_overflow: gens=$N  proven=$proven  not-proven=$notproven  skipped=$skipped"
echo "  proven AND EXECUTED : $ran   <- the ones actually held to account"
echo "  bugs:  UNSOUND via UBSan=$unsound (i64 only — see the oracle note)"
echo "         UNSOUND via interpreter=$interp_bug   UNSOUND via differential=$differ"
echo "         broken-C=$brokenc"
[ $nointerp -gt 0 ] && echo "  NO INTERPRETER=$nointerp  (build lain, or set LAIN=; i8/i16/i32 went UNORACLED)"
echo "=============================================================="
[ $unsound -eq 0 ] && [ $brokenc -eq 0 ] && [ $interp_bug -eq 0 ] && [ $differ -eq 0 ] && exit 0 || exit 1
