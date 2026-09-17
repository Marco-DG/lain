#!/usr/bin/env bash
# fuzz_unsigned.sh — prove-or-reject for UNSIGNED overflow, which no sanitizer can see.
#
# Unsigned wraparound is DEFINED behaviour in C, so UBSan reports nothing and
# `fuzz_overflow.sh` deliberately generates signed types only. Lain rejects unsigned overflow
# just the same, and the class is not academic: the memory-safety bug of 2026-09-09 was an
# unsigned UNDERFLOW — `n - 1` at n == 0 becoming SIZE_MAX and then serving as a loop bound,
# where no downstream bounds check could see it.
#
# ORACLE: exact integer arithmetic, computed by the generator and carried in the program as
# `// EXPECT_OUT:`. Two verdicts, and the first needs no execution at all:
#
#   FITS: no   the exact result leaves the type, so the program is UNSAFE. The engine PROVING
#              it check-free is unsound, full stop.
#   FITS: yes  the program is safe. If the engine proved it, running it must print exactly the
#              exact results — a differing number IS the wrap, caught without a sanitizer.
#
# TEETH, verified before this was trusted: forcing `c.ok = true` in vra_check_narrow takes
# not-proven to 0 and produces **38 UNSOUND in 60 programs** — 16 of them caught without
# running anything, because the exact result plainly leaves the type, and the rest caught
# by the printed value differing from exact arithmetic. Restore that line to re-verify.
#
#   bash fuzz_unsigned.sh [N]
set -u
cd "$(dirname "$0")/../.."
SC="${TMPDIR:-/tmp}/fuzz_uns.$$"; mkdir -p "$SC"; trap 'rm -rf "$SC"' EXIT
N="${1:-200}"
SEED=${RANDOM_SEED:-$$}
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"

gcc -std=c99 -O2 -o "$SC/vradrv"   src/tools/vra_driver.c -I src 2>/dev/null || { echo "vradrv build failed"; exit 2; }
gcc -std=c99 -O2 -o "$SC/lowerdrv" src/tools/lower_driver.c     -I src 2>/dev/null || { echo "lowerdrv build failed"; exit 2; }

proven=0; notproven=0; ran=0; unsound=0; skipped=0; brokenc=0; refused_unsafe=0; refused_safe=0
for ((k=0; k<N; k++)); do
    prog="$SC/u.ln"
    python3 scripts/fuzz/fuzz_unsigned.py $((SEED + k)) > "$prog" || { skipped=$((skipped+1)); continue; }
    fits=$(grep -m1 '// FITS:' "$prog" | awk '{print $3}')
    want=$(grep -m1 '// EXPECT_OUT:' "$prog" | cut -d: -f2- | xargs)

    out=$("$SC/vradrv" "$prog" --suppress 2>/dev/null) || { skipped=$((skipped+1)); continue; }
    echo "$out" | grep -q 'arith overflow' || { skipped=$((skipped+1)); continue; }
    if echo "$out" | grep 'arith overflow' | grep -q 'NOT proven'; then
        notproven=$((notproven+1))
        if [ "$fits" = "no" ]; then refused_unsafe=$((refused_unsafe+1))
        else                        refused_safe=$((refused_safe+1))
             [ -n "${VERBOSE:-}" ] && { echo "  · over-strict (safe, refused):"; sed 's/^/      /' "$prog"; }
        fi
        continue
    fi
    proven=$((proven+1))

    # An unsafe program the engine PROVED needs no run: the exact result leaves the type.
    if [ "$fits" = "no" ]; then
        unsound=$((unsound+1))
        echo "  ★ UNSOUND: exact result leaves the type, yet PROVEN check-free"
        cp "$prog" "$SC/unsound.$k.ln"; sed 's/^/      /' "$prog"
        continue
    fi

    "$SC/lowerdrv" "$prog" --emit-c --suppress-ovf --suppress-term > "$SC/u.c" 2>/dev/null || { skipped=$((skipped+1)); continue; }
    gcc -std=c99 -w -o "$SC/u" "$SC/u.c" $DEFS 2>/dev/null || { brokenc=$((brokenc+1)); cp "$prog" "$SC/brokenc.$k.ln"; continue; }

    ran=$((ran+1))
    got=$(timeout 10 "$SC/u" 2>/dev/null | xargs)
    if [ "$got" != "$want" ]; then
        unsound=$((unsound+1))
        echo "  ★ UNSOUND: proven check-free, but the value WRAPPED"
        echo "      expected: $want"
        echo "      got     : $got"
        cp "$prog" "$SC/unsound.$k.ln"; sed 's/^/      /' "$prog"
    fi
done

echo "=============================================================="
echo "fuzz_unsigned: gens=$N  proven=$proven  not-proven=$notproven  skipped=$skipped"
echo "  proven AND EXECUTED : $ran   <- the ones held to the exact answer"
echo "  correctly refused unsafe : $refused_unsafe   <- prove-or-reject working"
# Over-rejection is not a bug and does not fail the gate, but it is the number that says how
# much the engine costs in expressiveness. Reported so a precision change can be seen moving,
# instead of only its soundness being watched. VERBOSE=1 prints the programs.
echo "  refused a SAFE program   : $refused_safe   <- over-strictness (not a bug; a cost)"
echo "  bugs:  UNSOUND=$unsound  broken-C=$brokenc"
echo "=============================================================="
[ $unsound -eq 0 ] && [ $brokenc -eq 0 ] && exit 0 || exit 1
