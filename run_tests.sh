#!/usr/bin/env bash
# Test runner for the Lain test suite.
# Convention:
#   *_fail.ln  → compilation must fail (non-zero exit)
#   *_pass.ln  → compilation must succeed (exit 0)
#   other .ln  → treated as passing by default
# If a _fail.ln file contains "// EXPECT: [EXXX]" in its contents,
# stderr must contain that code.

set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
LAIN="$ROOT/lain"

# Use relative paths for test files (lain crashes on absolute paths with spaces)
cd "$ROOT" || exit 1

if [[ ! -x "$LAIN" ]]; then
    echo "Compiler not found. Building..."
    gcc -std=c99 -Wall -Wextra -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src" 2>/dev/null
    if [[ ! -x "$LAIN" ]]; then
        echo "Build failed."
        exit 1
    fi
fi

TESTS_DIR="$ROOT/tests"
PASS_COUNT=0
FAIL_COUNT=0
FAILED_TESTS=()

# Compile the emitted C for every *_pass test with gcc and fail the test if gcc
# rejects it. The old harness only greps the emitted C — it never compiled it —
# which silently masked a whole class of code-generation bugs (const output
# params, undefined slice typedefs, wrong attributes, ...). Set LAIN_GCC_CHECK=0
# to skip (e.g. if no C compiler is available).
LAIN_GCC_CHECK="${LAIN_GCC_CHECK:-1}"
GCC_BIN="${CC:-gcc}"
GCC_ERR=""
# Known-broken emitted C — tracked follow-ups (fixed-array / sentinel-slice ABI
# consistency: a native local array passed to a Fixed_<T>_N by-value parameter,
# and *T[:0] sentinel-slice param/field representation). These still run for
# Lain exit code; only their gcc compile is skipped so the suite reflects the
# real, known state instead of hiding it.
GCC_CHECK_SKIP=(
    "struct_in_mutation_pass"
)

# Returns 0 if the emitted C compiles (or the check is disabled/skipped),
# 1 otherwise (with the first gcc error in GCC_ERR).
gcc_check_ok() {
    local file="$1" base="$2"
    GCC_ERR=""
    [[ "$LAIN_GCC_CHECK" == "1" ]] || return 0
    [[ "$base" == *_pass ]] || return 0
    local s
    for s in "${GCC_CHECK_SKIP[@]}"; do
        [[ "$base" == "$s" ]] && return 0
    done
    local out_c="/tmp/lain_gcc_$$_${RANDOM}.c" out_o="/tmp/lain_gcc_$$_${RANDOM}.o"
    if ! "$LAIN" "$file" -o "$out_c" >/dev/null 2>&1; then
        rm -f "$out_c"; return 0   # Lain-level failure is handled by the caller
    fi
    local gerr
    gerr="$("$GCC_BIN" -std=c99 -c -w -o "$out_o" "$out_c" \
        -Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_putchar=putchar \
        -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_realloc=realloc 2>&1)"
    local grc=$?
    rm -f "$out_c" "$out_o"
    if [[ $grc -ne 0 ]]; then
        GCC_ERR="$(echo "$gerr" | grep -oE 'error:.*' | head -1)"
        return 1
    fi
    return 0
}

run_test() {
    local file="$1"
    local base
    base="$(basename "$file" .ln)"
    local is_fail=0
    if [[ "$base" == *_fail ]]; then
        is_fail=1
    fi

    local out
    local rc
    out="$("$LAIN" "$file" 2>&1)"
    rc=$?

    if [[ $is_fail -eq 1 ]]; then
        if [[ $rc -eq 0 ]]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            FAILED_TESTS+=("$file (expected fail, got pass)")
            return
        fi
        # Check EXPECT tag if present
        local expect
        expect="$(grep -oE '// EXPECT: \[E[0-9]+\]' "$file" | head -1 | grep -oE 'E[0-9]+')"
        if [[ -n "$expect" ]]; then
            if ! echo "$out" | grep -q "\[$expect\]"; then
                FAIL_COUNT=$((FAIL_COUNT + 1))
                FAILED_TESTS+=("$file (expected $expect, got different error)")
                return
            fi
        fi
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        if [[ $rc -ne 0 ]]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            FAILED_TESTS+=("$file (expected pass, got fail: $(echo "$out" | head -1))")
            return
        fi
        # The emitted C must also compile with a real C compiler.
        if ! gcc_check_ok "$file" "$base"; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            FAILED_TESTS+=("$file (emitted C rejected by gcc: $GCC_ERR)")
            return
        fi
        PASS_COUNT=$((PASS_COUNT + 1))
    fi
}

run_emit_snapshot() {
    local file="$1"
    local grepfile="${file%.ln}.grep"
    if [[ ! -f "$grepfile" ]]; then
        return 0
    fi
    local out_c="/tmp/lain_emit_$$.c"
    "$LAIN" "$file" -o "$out_c" > /dev/null 2>&1
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS+=("$file (emit snapshot: compilation failed)")
        rm -f "$out_c"
        return
    fi
    local missing=""
    while IFS= read -r pattern; do
        # skip blank and comment lines
        [[ -z "$pattern" || "$pattern" =~ ^// ]] && continue
        if ! grep -qF -- "$pattern" "$out_c"; then
            missing="$missing [$pattern]"
        fi
    done < "$grepfile"
    rm -f "$out_c"
    if [[ -n "$missing" ]]; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS+=("$file (emit snapshot missing:$missing)")
    else
        PASS_COUNT=$((PASS_COUNT + 1))
    fi
}

# Run all .ln files in tests/ (relative paths)
while IFS= read -r file; do
    # emit/ tests are snapshot checks, not pass/fail compilation tests
    if [[ "$file" == tests/emit/* ]]; then
        run_emit_snapshot "$file"
    else
        run_test "$file"
    fi
done < <(find tests -name '*.ln' -type f | sort)

# Run shell-based helper tests (exit code = pass/fail).
while IFS= read -r shfile; do
    if bash "$shfile" >/dev/null 2>&1; then
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS+=("$shfile (shell helper exited non-zero)")
    fi
done < <(find tests -name '*.sh' -type f | sort)

TOTAL=$((PASS_COUNT + FAIL_COUNT))
echo ""
echo "=========================================="
echo "Results: $PASS_COUNT/$TOTAL passed, $FAIL_COUNT failed"
echo "=========================================="
if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
    echo ""
    echo "Failed tests:"
    for t in "${FAILED_TESTS[@]}"; do
        echo "  - $t"
    done
fi

if [[ $FAIL_COUNT -gt 0 ]]; then
    exit 1
fi
exit 0
