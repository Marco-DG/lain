#!/usr/bin/env bash
# Test runner for the Lain test suite.
# Convention:
#   *_fail.ln  → compilation must fail (non-zero exit)
#   *_pass.ln  → compilation must succeed (exit 0)
#   other .ln  → treated as passing by default
# If a _fail.ln file contains "// EXPECT: [EXXX]" in its contents,
# stderr must contain that code.

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"

# Use relative paths for test files (lain crashes on absolute paths with spaces)
cd "$ROOT" || exit 1

if [[ ! -x "$LAIN" ]]; then
    echo "Compiler not found. Building..."
    gcc -std=c99 -Wall -Wextra -o "$LAIN" "$ROOT/src/frontends/lain/main.c" -I "$ROOT/src" 2>/dev/null
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
# Emitted C that gcc must reject-list. EMPTY — every _pass test's emitted C now
# compiles with gcc. (Keep it empty: a new entry means a real codegen/interop bug
# to fix, not to skip.)
GCC_CHECK_SKIP=(
)

# Returns 0 if the emitted C compiles (or the check is disabled/skipped),
# 1 otherwise (with the first gcc error in GCC_ERR).
# Per-test compiler flags. A test may pin behaviour that is authoritative only under the
# sovereign engine — `// LAINFLAGS: --engine=ir-full` — which is exactly the Stage 3.5 split:
# the new analyses decide some questions before the whole switchover happens. Without this
# the corpus could only ever record what the OLD engine does.
lain_flags_for() {
    grep -oE '^// LAINFLAGS:.*' "$1" 2>/dev/null | head -1 | sed 's|^// LAINFLAGS:[[:space:]]*||'
}

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
    if ! "$LAIN" $(lain_flags_for "$file") "$file" -o "$out_c" >/dev/null 2>&1; then
        rm -f "$out_c"; return 0   # Lain-level failure is handled by the caller
    fi
    local gerr
    # `-w` silences warnings, and gcc classifies some C CONSTRAINT VIOLATIONS as warnings —
    # cases where the emitted program does something other than what the Lain program said. The
    # gate could not see them, so whether a codegen defect was caught came down to whether gcc
    # had chosen "error" or "warning", which is not a property of Lain at all. 31 programs
    # carried one (D-38).
    #
    # Two of the three classes are now clean and are promoted to errors, so they cannot return:
    #   int-conversion   -- a pointer stored in an integer. This was a LIVE MISCOMPILE: Lain
    #                       could not write through a mutable borrow (D-38 tier 1).
    #   implicit-int     -- a declaration with no type at all. C99 removed implicit int, so
    #                       `static const <nothing> MAX = 100;` is not valid C99.
    #
    # incompatible-pointer-types joined them 2026-09-16, once D-39 (a string literal reaching a
    # `u8[]`) and D-40 (an extern declaring a slice parameter, which has no C type) were closed.
    # All three classes are now errors; what remains tolerated is listed explicitly above, and
    # each -Wno- is a claim that the class does not change behaviour.
    # `-w` had to GO, not be supplemented: it beats -Werror= in every flag position, so the
    # promotions below were inert while it was present (verified twice). What remains is an
    # explicit list of what is tolerated, which is the honest form — each -Wno- is a claim that
    # the class does not change behaviour, and each can be argued.
    #
    # Measured over all emitted C: discarded-qualifiers 15, format-security 4,
    # incompatible-pointer-types 1. None is int-conversion or implicit-int any more.
    gerr="$("$GCC_BIN" -std=c99 -c -o "$out_o" "$out_c" \
        -Wno-discarded-qualifiers -Wno-format-security \
        -Werror=int-conversion -Werror=implicit-int \
        -Werror=incompatible-pointer-types \
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
    out="$("$LAIN" $(lain_flags_for "$file") "$file" 2>&1)"
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


# Build the program and diff its stdout against the oracle. Returns 0 when a `.grep` should
# ALSO be applied, 1 when this was the whole test.
run_output_oracle() {
    local file="$1" exp="$2"
    local base; base="$(basename "${file%.ln}")"
    local c="/tmp/lain_oracle_$$.c" bin="/tmp/lain_oracle_$$"
    if ! "$LAIN" $(lain_flags_for "$file") "$file" -o "$c" >/dev/null 2>&1; then
        FAIL_COUNT=$((FAIL_COUNT + 1)); FAILED_TESTS+=("$file (oracle: compilation failed)")
        rm -f "$c"; return 1
    fi
    if ! gcc -o "$bin" "$c" -Dlibc_printf=printf -Dlibc_puts=puts -w 2>/dev/null; then
        FAIL_COUNT=$((FAIL_COUNT + 1)); FAILED_TESTS+=("$file (oracle: emitted C rejected by gcc)")
        rm -f "$c" "$bin"; return 1
    fi
    local got; got="$("$bin" 2>/dev/null)"
    rm -f "$c" "$bin"
    if ! diff -q <(printf '%s\n' "$got") "$exp" >/dev/null 2>&1; then
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS+=("$file (oracle: output differs from $(basename "$exp"))")
        return 1
    fi
    PASS_COUNT=$((PASS_COUNT + 1))
    return 1
}

# ── AN OUTPUT ORACLE BEATS A TEXT GREP, AND IT IS BACKEND-NEUTRAL ────────────────────────
# A `.grep` sidecar asserts that the emitted C CONTAINS some text, which pins an IMPLEMENTATION.
# The project now has two backends, and they are two implementations of one semantics: every
# one of these tests grepped for the old emitter's private spellings — `__match`, `__try`,
# `__elsev`, `_Tag_ParseErr` — so all 22 failed against the new backend while the programs
# behaved identically (emit_gate: 399 agree).
#
# A `.expected` sidecar asserts the program's OUTPUT instead. That is a property of the program
# rather than of the compiler that emitted it, so it holds whichever backend runs — and it is
# strictly stronger than a grep, because it catches a wrong ANSWER where a grep only catches a
# renamed temporary. It is the same mechanism `run_trust.sh` already uses for its oracles.
#
# A test may carry either. `.expected` is preferred when present; `.grep` remains for the few
# assertions that are genuinely about the emitted INTERFACE rather than the behaviour.
run_emit_snapshot() {
    local file="$1"
    local grepfile="${file%.ln}.grep"
    local expfile="${file%.ln}.expected"
    if [[ -f "$expfile" ]]; then
        run_output_oracle "$file" "$expfile" || return 0
        [[ -f "$grepfile" ]] || return 0
    fi
    if [[ ! -f "$grepfile" ]]; then
        return 0
    fi
    local out_c="/tmp/lain_emit_$$.c"
    # ★ The file's own LAINFLAGS apply HERE too. This path ran the compiler bare, so a test
    # pinned to an engine was snapshotted under a different one — and a test pinned because the
    # DEFAULT cannot compile it failed as "compilation failed" with nothing saying why.
    "$LAIN" $(lain_flags_for "$file") "$file" -o "$out_c" > /dev/null 2>&1
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
    if [[ -f "${file%.ln}.grep" ]]; then
        # any test with a .grep sidecar is an emit snapshot, wherever it lives
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
