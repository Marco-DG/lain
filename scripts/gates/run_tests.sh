#!/usr/bin/env bash
# Test runner for the Lain test suite.
# Convention:
#   *_fail.ln  → compilation must fail (non-zero exit)
#   *_pass.ln  → compilation must succeed (exit 0)
#   other .ln  → treated as passing by default
# If a _fail.ln file contains "// EXPECT: [EXXX]" in its contents,
# stderr must contain that code.
#
#   bash scripts/gates/run_tests.sh                     the whole suite
#   bash scripts/gates/run_tests.sh tests/a.ln ...      only these tests (.ln or .sh), in this order

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"

# A test named on the command line is resolved from the caller's directory, then run by its path
# from the tree root like every other test (see below).
ARGS=()
for a in "$@"; do
    [[ -f "$a" ]] || { echo "run_tests: no such test file: $a" >&2; exit 2; }
    rel="$(realpath --relative-to="$ROOT" "$a")"
    [[ "$rel" != ../* ]] || { echo "run_tests: not inside the tree: $a" >&2; exit 2; }
    ARGS+=("$rel")
done

# Test files are compiled by RELATIVE path from the tree root. Given an absolute path, lain changes
# to the file's directory, so `import std.io` looks for std/io.ln there and is refused (E106).
# Spaces have nothing to do with it (this line used to blame them): an absolute path fails with or
# without one, and a relative path works inside a directory whose name has one.
cd "$ROOT" || exit 1

if [[ ! -x "$LAIN" ]]; then
    echo "Compiler not found. Building..."
    gcc -std=c99 -O2 -Wall -Wextra -o "$LAIN" "$ROOT/src/frontends/lain/main.c" -I "$ROOT/src" 2>/dev/null
    if [[ ! -x "$LAIN" ]]; then
        echo "Build failed."
        exit 1
    fi
fi

TESTS_DIR="$ROOT/tests"
# Every temporary of this run lives here (I.173): parallel jobs, and a concurrent run in another
# tree, cannot meet on one name. The gcc verdict cache is shared, keyed by content.
RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/lain_run_tests.XXXXXX")"; trap 'rm -rf "$RUN_DIR"' EXIT
GCC_CACHE="${LAIN_GATE_CACHE:-$HOME/.cache/lain-gates}/gcc"; mkdir -p "$GCC_CACHE" 2>/dev/null
# One emission of the corpus is about 650 entries (2.6 MB); an entry no run has used for 14 days
# is deleted, so the store stays near the size of the emissions in current use.
find "$GCC_CACHE" -type f -mtime +14 -delete 2>/dev/null

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
# The flags gcc_check_ok compiles with, ONCE: the cached verdict's key is built from this same text
# and gcc's version, so a change to either is a different key, never a stale hit. (Each flag is one
# word; see gcc_check_ok's note for why each is there.)
GCC_FLAGS="-std=c99 -c -Wno-discarded-qualifiers -Wno-format-security -Werror=int-conversion -Werror=implicit-int -Werror=incompatible-pointer-types -Werror=return-type -Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_putchar=putchar -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_realloc=realloc"
GCC_FLAGS_KEY="$("$GCC_BIN" --version 2>/dev/null | head -1) | $GCC_FLAGS"
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
    local file="$1" base="$2" out_c="$3"
    GCC_ERR=""
    [[ "$LAIN_GCC_CHECK" == "1" ]] || return 0
    [[ "$base" == *_pass ]] || return 0
    local s
    for s in "${GCC_CHECK_SKIP[@]}"; do
        [[ "$base" == "$s" ]] && return 0
    done
    [[ -f "$out_c" ]] || return 0             # Lain-level failure is handled by the caller
    local out_o="${out_c%.c}.o"
    # ★ THE VERDICT IS CACHED BY CONTENT (I.173). gcc's answer is a function of the C, the flags and
    # gcc itself, and most of a chain's links change few programs' C: the same C was recompiled on
    # every link. The key is all three; a stored entry is written to a temporary name and renamed,
    # so a concurrent run reads a whole entry or misses. LAIN_GATE_NOCACHE=1 compiles every time.
    local key="" entry=""
    if [[ "${LAIN_GATE_NOCACHE:-0}" != "1" ]]; then
        key="$( { cat "$out_c"; echo "$GCC_FLAGS_KEY"; } | sha256sum | cut -c1-40)"
        entry="$GCC_CACHE/$key"
        # A hit refreshes the entry's age (the store keeps 14 days of use, below). An entry that
        # vanished between the test and the read, or reads as neither verdict, is compiled again.
        local verdict; verdict="$(cat "$entry" 2>/dev/null)"
        if [[ "$verdict" == "ok" ]]; then touch -c "$entry" 2>/dev/null; return 0; fi
        if [[ "$verdict" == "err "* ]]; then touch -c "$entry" 2>/dev/null; GCC_ERR="${verdict#err }"; return 1; fi
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
    gerr="$("$GCC_BIN" $GCC_FLAGS -o "$out_o" "$out_c" 2>&1)"
    local grc=$?
    rm -f "$out_o"
    if [[ $grc -ne 0 ]]; then
        GCC_ERR="$(echo "$gerr" | grep -oE 'error:.*' | head -1)"
        [[ -n "$entry" ]] && { printf 'err %s' "$GCC_ERR" > "$entry.$$" && mv -f "$entry.$$" "$entry"; }
        return 1
    fi
    [[ -n "$entry" ]] && { printf 'ok' > "$entry.$$" && mv -f "$entry.$$" "$entry"; }
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
    # ★ ONE RUN GIVES THE VERDICT AND THE C (I.173). The verdict ran without `-o` (so every test
    # wrote the shared out.c at the tree root, which two parallel jobs would race on), and a _pass
    # test was then compiled a second time to get its C for gcc.
    local out_c="$RUN_DIR/$(printf '%s' "$file" | md5sum | cut -c1-16).c"
    out="$("$LAIN" $(lain_flags_for "$file") "$file" -o "$out_c" 2>&1)"
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
        # An optional `// EXPECT-TEXT: <text>` must appear in the output too. The code says which
        # rule refused the program; the text says the message EXPLAINS it, and a message is the
        # documentation a user reads when stuck. Nothing tested one before.
        local etext
        etext="$(grep -m1 -oE '// EXPECT-TEXT: .*' "$file" | sed 's|^// EXPECT-TEXT: ||')"
        if [[ -n "$etext" ]] && ! echo "$out" | grep -qF -- "$etext"; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            FAILED_TESTS+=("$file (expected the message to say: $etext)")
            return
        fi
        # ★ THE FIRST CODED ERROR SAYS WHERE IT IS: a line, or at least the function (I.68). This
        # MEASURES what check_build_warnings can only grep for: that gate looked for a literal
        # `"[E086] Error:"`, while report.h's ir_diag passes the code as a format argument, so six
        # analysis errors printed no position and no rule saw them (Handwriting's audit). The one
        # exempt code has no source construct to point at: E064, an anonymous union's layout. (E124
        # in a type position needed the same exemption until 4db07a1 placed it.)
        local first
        first="$(echo "$out" | grep -m1 -E '^\[E[0-9]+\] Error')"
        if [[ -n "$first" ]] && ! echo "$first" | grep -qE ' Ln [0-9]+, Col [0-9]+| in '"'" \
           && ! echo "$first" | grep -qE '^\[E064\]'; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            FAILED_TESTS+=("$file (the first error has no position: $first)")
            return
        fi
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        if [[ $rc -ne 0 ]]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            FAILED_TESTS+=("$file (expected pass, got fail: $(echo "$out" | head -1))")
            return
        fi
        # The emitted C must also compile with a real C compiler.
        if ! gcc_check_ok "$file" "$base" "$out_c"; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            FAILED_TESTS+=("$file (emitted C rejected by gcc: $GCC_ERR)")
            return
        fi
        PASS_COUNT=$((PASS_COUNT + 1))
    fi
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
# ★ BOTH ARE CHECKED, AFTER THE TEST'S OWN VERDICT (I.175). This runner used to look for a
# `.expected` only beside a `.grep`, and the commit that replaced 22 greps with oracles deleted the
# greps: from then on 22 oracles ran nowhere, among them payload_marker_try's `line=99`, which HEAD
# printed as `line=0` for days (I.120). Where both existed, the oracle ended the test and the grep
# was never read. Now every program gets the verdict and gcc check of run_test, then its oracle,
# then its grep, all on the one C that run_test emitted. tests/trust/ is the exception for the
# oracle: run_trust.sh executes those programs under ASan and UBSan.
run_sidecars() {
    local file="$1"
    local exp="${file%.ln}.expected" grepfile="${file%.ln}.grep"
    local out_c="$RUN_DIR/$(printf '%s' "$file" | md5sum | cut -c1-16).c"
    local why=""
    if [[ -f "$exp" && "$file" != tests/trust/* ]]; then
        local bin="${out_c%.c}.bin"
        if ! gcc -o "$bin" "$out_c" -Dlibc_printf=printf -Dlibc_puts=puts -w 2>/dev/null; then
            why="oracle: emitted C rejected by gcc"
        else
            # a program that does not finish in 30 s fails its oracle instead of stalling the suite
            local got; got="$(timeout 30 "$bin" 2>/dev/null)"
            diff -q <(printf '%s\n' "$got") "$exp" >/dev/null 2>&1 \
                || why="oracle: output differs from $(basename "$exp")"
        fi
        rm -f "$bin"
    fi
    if [[ -z "$why" && -f "$grepfile" ]]; then
        local missing="" pattern
        while IFS= read -r pattern; do
            # skip blank and comment lines
            [[ -z "$pattern" || "$pattern" =~ ^// ]] && continue
            grep -qF -- "$pattern" "$out_c" || missing="$missing [$pattern]"
        done < "$grepfile"
        [[ -n "$missing" ]] && why="emit snapshot missing:$missing"
    fi
    if [[ -n "$why" ]]; then
        PASS_COUNT=0; FAIL_COUNT=1; FAILED_TESTS=("$file ($why)")
    fi
}

# ── THE TESTS RUN IN PARALLEL (I.173) ─────────────────────────────────────────────────────
# Each test is one job that prints ONE line: its index in the old order, P or F, and for a failure
# the exact text the serial loop appended. The lines are sorted back into that order, so the counts
# and the failure list read as they always did. LAIN_GATE_JOBS sets the width.
one_test() {
    local idx="$1" file="$2"
    PASS_COUNT=0; FAIL_COUNT=0; FAILED_TESTS=()
    if [[ "$file" == *.sh ]]; then
        if bash "$file" >/dev/null 2>&1; then PASS_COUNT=1
        else FAIL_COUNT=1; FAILED_TESTS+=("$file (shell helper exited non-zero)"); fi
    else
        run_test "$file"
        (( FAIL_COUNT == 0 )) && [[ "$file" != *_fail.ln ]] && run_sidecars "$file"
        # its C is in the shared tmpfs; one run of the suite would otherwise hold all of it
        rm -f "$RUN_DIR/$(printf '%s' "$file" | md5sum | cut -c1-16).c"
    fi
    local i
    for (( i = 0; i < PASS_COUNT; i++ )); do printf '%06d\tP\n' "$idx"; done
    for t in "${FAILED_TESTS[@]}"; do printf '%06d\tF\t%s\n' "$idx" "$t"; done
}
export -f one_test run_test run_sidecars gcc_check_ok lain_flags_for
export LAIN LAIN_GCC_CHECK GCC_BIN RUN_DIR GCC_CACHE GCC_FLAGS GCC_FLAGS_KEY
# The default width is 4: on a 6-core machine shared with other sessions, 4 jobs cut the wall time
# about 3x and cost about 20% more CPU than one (the cores slow each other); more jobs buy a few
# seconds for much more CPU.
JOBS="${LAIN_GATE_JOBS:-4}"
RESULTS="$RUN_DIR/results.txt"
# Each test keeps its index in the serial order (.ln files, then the shell helpers, each sorted;
# or the order of the arguments), but the shell helpers START first: a few take 5-9 s and would
# otherwise be the last jobs running.
if (( ${#ARGS[@]} > 0 )); then printf '%s\n' "${ARGS[@]}"
else find tests -name '*.ln' -type f | sort; find tests -name '*.sh' -type f | sort; fi \
    | awk '{ line = sprintf("%d\t%s", NR, $0); if ($0 ~ /\.sh$/) print line; else ln[++n] = line }
           END { for (i = 1; i <= n; i++) print ln[i] }' \
    | xargs -P "$JOBS" -d '\n' -n 1 bash -c 'IFS=$(printf "\t") read -r i f <<< "$1"; one_test "$i" "$f"' _ \
    > "$RESULTS"
sort -s -n -k1,1 "$RESULTS" > "$RESULTS.sorted"
PASS_COUNT=$(awk -F'\t' '$2 == "P"' "$RESULTS.sorted" | wc -l)
FAIL_COUNT=$(awk -F'\t' '$2 == "F"' "$RESULTS.sorted" | wc -l)
FAILED_TESTS=()
while IFS= read -r t; do FAILED_TESTS+=("$t"); done < <(awk -F'\t' '$2 == "F" { sub(/^[^\t]*\tF\t/, ""); print }' "$RESULTS.sorted")
# A sidecar without its program is a test that runs nothing (I.175: payload_marker_try's oracle
# stayed in tests/errors when the program moved to tests/trust). Checked on a whole-suite run.
if (( ${#ARGS[@]} == 0 )); then
    while IFS= read -r t; do
        [[ -f "${t%.*}.ln" ]] && continue
        FAIL_COUNT=$((FAIL_COUNT + 1)); FAILED_TESTS+=("$t (a sidecar without its program: nothing runs it)")
    done < <(find tests \( -name '*.expected' -o -name '*.grep' \) -type f | sort)
fi

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
