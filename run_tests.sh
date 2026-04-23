#!/usr/bin/env bash
# Test runner per la suite Lain.
# Convention:
#   *_fail.ln  → compilazione deve fallire (exit non-zero)
#   *_pass.ln  → compilazione deve riuscire (exit 0)
#   altri .ln  → trattati come pass per default
# Se un _fail.ln contiene "// EXPECT: [EXXX]" nel testo, stderr deve contenere quel codice.

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
