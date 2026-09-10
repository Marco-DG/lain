#!/usr/bin/env bash
# fuzz_init.sh — teeth for DEFINITE ASSIGNMENT, the last headline guarantee with no
# generative test.
#
# "Uninitialised read | UB | Prevented | Prevented" is a row in the README's table. The
# sovereign definite-init pass runs by DEFAULT (`--engine=ir`), so this asks the shipping
# compiler, not a driver.
#
# ORACLE: MemorySanitizer, via clang. MSan tracks uninitialised BITS through computation rather
# than hunting a sentinel, so a value that is added to, branched on or narrowed before it is
# used is still caught — which `-ftrivial-auto-var-init=pattern` cannot promise. A program the
# compiler ACCEPTED promises no uninitialised read; an MSan report on one is unsound.
#
# Unsafe variants are generated on purpose: a generator that can only produce initialised
# programs proves nothing about the checker. Programs the compiler REJECTS are counted, and the
# count is split by whether the shape was meant to be safe, so over-strictness is visible as a
# cost rather than hidden among the refusals.
#
# Each shape is labelled SAFE or UNSAFE by construction, so the run reports BOTH directions:
# an UNSAFE shape accepted and MSan-confirmed is unsound; a SAFE shape rejected is
# over-strictness — not a bug, and it does not fail the gate, but it is the number that says
# what the pass charges in expressiveness, and nothing was printing it.
#
# ★ CORRECTS THE RECORD. Session 65 noted a fail-OPEN here — "a store at an UNKNOWN index marks
# the whole array initialised, so a half-filling loop is not caught". Measured 2026-09-10: the
# half-filling loop is REJECTED ([E005]), so that fail-open does not reproduce. What is true is
# the opposite and it is a COST, not a hole: a loop that fills EVERY element is rejected too.
# `for k in 0..8 { a[k] = 7 }` then reading `a[7]` needs a proof that the loop covers 0..len,
# which the pass does not attempt — the comprehension form carries IR_INIT instead and is
# accepted. Both are tracked below as loop_fill_all (over-strict) and loop_fill_half (correct).
#
#   bash fuzz_init.sh [N]
set -u
cd "$(dirname "$0")/../.."
SC="${TMPDIR:-/tmp}/fuzz_init.$$"; mkdir -p "$SC"; trap 'rm -rf "$SC"' EXIT
N="${1:-200}"
SEED=${RANDOM_SEED:-$$}
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"
CLANG="${CLANG:-clang}"
command -v "$CLANG" >/dev/null 2>&1 || { echo "fuzz_init: needs clang for MSan — SKIPPED"; exit 0; }
[ -x ./lain ] || { echo "build first"; exit 2; }

accepted=0; rejected=0; ran=0; unsound=0; brokenc=0; overstrict=0; refused_unsafe=0
# SAFE by construction: every byte the program reads was written first.
declare -A safe=([whole_init]=1 [elem_then_same]=1 [comprehension]=1 [loop_fill_all]=1
                 [branch_both]=1 [struct_full]=1)
for ((k=0; k<N; k++)); do
    prog="$SC/i.ln"
    python3 scripts/fuzz/fuzz_init.py $((SEED + k)) > "$prog" || continue
    shape=$(grep -m1 '// SHAPE:' "$prog" | awk '{print $3}')

    if ! ./lain "$prog" -o "$SC/i.c" >/dev/null 2>&1; then
        rejected=$((rejected+1))
        if [ -n "${safe[$shape]:-}" ]; then
            overstrict=$((overstrict+1))
            [ -n "${VERBOSE:-}" ] && { echo "  · over-strict [$shape]"; sed 's/^/      /' "$prog"; }
        else
            refused_unsafe=$((refused_unsafe+1))
        fi
        continue
    fi
    accepted=$((accepted+1))

    # MSan needs every byte it judges to come from instrumented code; the emitted C is
    # self-contained apart from libc, which MSan models.
    "$CLANG" -std=c99 -w -fsanitize=memory -fno-omit-frame-pointer \
        -o "$SC/i" "$SC/i.c" $DEFS 2>/dev/null || { brokenc=$((brokenc+1)); continue; }

    ran=$((ran+1))
    err=$(MSAN_OPTIONS=exitcode=86 timeout 20 "$SC/i" 2>&1 >/dev/null || true)
    if echo "$err" | grep -q 'MemorySanitizer: use-of-uninitialized-value'; then
        unsound=$((unsound+1))
        echo "  ★ UNSOUND: accepted, yet MSan reports an uninitialised read  [$shape]"
        cp "$prog" "$SC/unsound.$k.ln"; sed 's/^/      /' "$prog"
    fi
done

echo "=============================================================="
echo "fuzz_init: gens=$N  accepted=$accepted  rejected=$rejected"
echo "  accepted AND EXECUTED : $ran   <- the ones MSan actually judged"
echo "  correctly refused unsafe : $refused_unsafe   <- the guarantee working"
echo "  refused a SAFE program   : $overstrict   <- over-strictness (a cost, not a bug)"
echo "  bugs:  UNSOUND=$unsound  broken-C=$brokenc"
echo "=============================================================="
[ $unsound -eq 0 ] && [ $brokenc -eq 0 ] && exit 0 || exit 1
