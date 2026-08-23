#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Lain TRUST harness (Phase 0 — the gate).
#
# run_tests.sh only *compiles* the emitted C. That proves it's valid C; it does
# NOT prove Lain's safety PROOFS are sound. This harness does the missing half:
#
#   Every program here is ACCEPTED by Lain — i.e. Lain PROVED it memory-safe and
#   overflow-free (it elided bounds checks, proved arithmetic can't overflow,
#   enforced single-ownership, laid out niches). We compile the emitted C under
#   AddressSanitizer + UndefinedBehaviorSanitizer and EXECUTE it against edge/
#   adversarial values. A sanitizer trap means Lain's proof was UNSOUND — it
#   accepted code that is actually unsafe. That is a TRUST VIOLATION: a proof in
#   this state must never be allowed to drive an `assume` in the LLVM backend.
#
#   *_pass.ln  → must compile, run clean under sanitizers, exit 0, and (if a
#                sibling .expected exists) match stdout exactly (correctness oracle
#                — catches miscompiles like a mis-encoded niche or a wrong wrap).
#
# Usage: bash run_trust.sh
# ─────────────────────────────────────────────────────────────────────────────
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
LAIN="$ROOT/lain"
CC="${CC:-gcc}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1"
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_putchar=putchar \
      -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_realloc=realloc -Dlibc_calloc=calloc"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:halt_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

cd "$ROOT" || exit 1
if [[ ! -x "$LAIN" ]]; then
    echo "building lain…"; gcc -std=c99 -Wall -Wextra -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src" 2>/dev/null || { echo "build failed"; exit 1; }
fi

PASS=0; FAIL=0; VIOL=(); TDIR="$(mktemp -d)"
trap 'rm -rf "$TDIR"' EXIT

for f in "$ROOT"/tests/trust/*_pass.ln; do
    [[ -e "$f" ]] || continue
    base="$(basename "$f" .ln)"
    c="$TDIR/$base.c"; bin="$TDIR/$base"

    # 1) Lain must ACCEPT it (proof succeeded).
    if ! "$LAIN" "$f" -o "$c" >/dev/null 2>"$TDIR/lain.err"; then
        FAIL=$((FAIL+1)); VIOL+=("$base: Lain rejected a trust program:  $(head -1 "$TDIR/lain.err")"); continue
    fi
    # 2) Compile the emitted C with sanitizers.
    if ! $CC $SAN $DEFS -o "$bin" "$c" 2>"$TDIR/gcc.err"; then
        FAIL=$((FAIL+1)); VIOL+=("$base: emitted C did not compile under sanitizers:  $(grep -m1 error: "$TDIR/gcc.err")"); continue
    fi
    # 3) EXECUTE it. A sanitizer trap = an unsound proof.
    out="$("$bin" 2>"$TDIR/san.err")"; rc=$?
    if grep -qE "runtime error|AddressSanitizer|UndefinedBehaviorSanitizer|SUMMARY:" "$TDIR/san.err"; then
        FAIL=$((FAIL+1)); VIOL+=("$base: ⚠ SANITIZER TRAP (unsound proof):  $(grep -m1 -E 'runtime error|ERROR' "$TDIR/san.err")"); continue
    fi
    if [[ $rc -ne 0 ]]; then
        FAIL=$((FAIL+1)); VIOL+=("$base: nonzero exit ($rc) — logic/trap failure"); continue
    fi
    # 4) Correctness oracle (optional).
    if [[ -f "${f%.ln}.expected" ]]; then
        if ! diff -q <(printf '%s\n' "$out") "${f%.ln}.expected" >/dev/null; then
            FAIL=$((FAIL+1)); VIOL+=("$base: output mismatch (miscompile):  got '$out'"); continue
        fi
    fi
    PASS=$((PASS+1))
done

echo "=========================================="
echo "TRUST: $PASS trusted, $FAIL violations"
echo "=========================================="
if (( FAIL > 0 )); then
    printf '  %s\n' "${VIOL[@]}"
    exit 1
fi
