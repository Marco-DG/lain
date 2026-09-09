#!/usr/bin/env bash
# fuzz_termination.sh — teeth for the ONE guarantee that had no fuzzer.
#
# "Non-termination: Rejected in `func`" is a headline row in the README's table, and until now
# nothing generated programs to test it. Three FALSE PROOFS were found in it by hand on
# 2026-09-09 — a rising counter read as falling because the guard's predicate was not reversed
# when the counter sat on the right, a counter a callee resets, and B1's trip count built on the
# same recovery. None was reachable through the corpus, because the old front end refuses those
# shapes with [E082]/[E011] before the new pass ever runs.
#
# ORACLE, and it is exact: a loop the engine PROVES terminating must terminate. Run it under a
# timeout; a hang on a proven-terminating program is an UNSOUND result, not a slow test.
#
# The seam is what makes this possible at all: `--suppress` now also opens the legacy
# TERMINATION diagnostics, so the new engine's verdict is observable on exactly the programs
# that used to be invisible. Emission goes through the NEW backend for the same reason — the old
# one refuses to emit for a program it rejected.
#
# TEETH, verified before this was trusted: making `vra_progress_on_every_path` return true
# unconditionally (the state before the multi-path fix) takes not-proven to 0 and produces
# **8 UNSOUND in 40 programs** — the `one_arm` shape, `while i <= 1 { if flag > 0 { i = i+2 } }`,
# which is proven terminating and hangs on `run(0, 0)`. Restore that line to re-verify.
#
# That check also caught a bug in THIS script: `if ! timeout ...; then rc=$?` reads the status
# of the NEGATION, always 0, so the 124 the whole harness exists to see was invisible and it
# reported "all clean" over a program that provably hangs. Take the status directly.
#
#   bash fuzz_termination.sh [N]
set -u
cd "$(dirname "$0")/../.."
SC="${TMPDIR:-/tmp}/fuzz_term.$$"; mkdir -p "$SC"; trap 'rm -rf "$SC"' EXIT
N="${1:-200}"
SEED=${RANDOM_SEED:-$$}
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"

gcc -std=c99 -O2 -o "$SC/vradrv"  src/analysis/vra_driver.c -I src 2>/dev/null || { echo "vradrv build failed"; exit 2; }
gcc -std=c99 -O2 -o "$SC/lowerdrv" src/ir/lower_driver.c    -I src 2>/dev/null || { echo "lowerdrv build failed"; exit 2; }

proven=0; notproven=0; unsound=0; skipped=0; brokenc=0; ran=0
for ((k=0; k<N; k++)); do
    prog="$SC/t.ln"
    python3 scripts/fuzz/fuzz_termination.py $((SEED + k)) > "$prog" || { skipped=$((skipped+1)); continue; }

    # The new engine's verdict for `run`, with the legacy termination diagnostics stood down.
    v=$("$SC/vradrv" "$prog" --suppress 2>/dev/null | grep -E '^\s+run\s+termination' | head -1)
    [ -z "$v" ] && { skipped=$((skipped+1)); continue; }
    case "$v" in
        *"PROVEN check-free"*) ;;
        *) notproven=$((notproven+1)); continue ;;
    esac
    proven=$((proven+1))

    "$SC/lowerdrv" "$prog" --emit-c --suppress-term > "$SC/t.c" 2>/dev/null || { skipped=$((skipped+1)); continue; }
    gcc -std=c99 -w -o "$SC/t" "$SC/t.c" $DEFS 2>/dev/null || { brokenc=$((brokenc+1)); cp "$prog" "$SC/brokenc.$k.ln"; continue; }

    # A loop proven to terminate must terminate. 5s is many orders of magnitude over any
    # bound these programs can legitimately need (the largest generated limit is 255).
    ran=$((ran+1))
    # Take the status DIRECTLY. `if ! cmd; then rc=$?` reads the status of the NEGATION, which
    # is always 0, so the 124 this whole harness exists to see was invisible — it reported
    # "all clean" over a program that provably hangs. Verified by opening a hole in the
    # analysis on purpose and watching the count stay at zero.
    timeout 5 "$SC/t" >/dev/null 2>&1; rc=$?
    if [ $rc -eq 124 ]; then
        unsound=$((unsound+1))
        echo "  ★ UNSOUND: proven terminating, HANGS"
        cp "$prog" "$SC/unsound.$k.ln"; sed 's/^/      /' "$prog"
    fi
done

echo "=============================================================="
echo "fuzz_termination: gens=$N  proven=$proven  not-proven=$notproven  skipped=$skipped"
# EXECUTED is the number the teeth depend on: a verdict nothing ran is a verdict
# nothing checked. A run where proven is high and ran is low has no oracle at all.
echo "  proven AND EXECUTED : $ran   <- the ones actually held to account"
echo "  bugs:  UNSOUND(hang on a proven loop)=$unsound  broken-C=$brokenc"
echo "=============================================================="
[ $unsound -eq 0 ] && [ $brokenc -eq 0 ] && exit 0 || exit 1
