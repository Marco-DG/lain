#!/usr/bin/env bash
# check_build_warnings.sh — fail on the CORRECTNESS-class compiler warnings.
#
# Not a style gate. These specific warnings mean the program has undefined behaviour or a
# guaranteed-wrong branch, and each one has already cost this project a real bug:
#
#   -Wreturn-type   a `break` in ir_lower_expr's switch ran off the end of a non-void
#                   function, returning a GARBAGE IrValue* that the VRA dereferenced —
#                   a segfault on every @movemask/@load/@store program. gcc warned; the
#                   build output was being grepped for `error` only, so nobody saw it.
#
# The codebase has other, benign warnings (indentation, unused parameters), so a blanket
# "zero warnings" gate is not achievable today. This checks the subset that is never benign.
#
#   bash check_build_warnings.sh
set -u
cd "$(dirname "$0")/../.."
FATAL='-Wreturn-type -Wuninitialized -Wimplicit-function-declaration -Wint-conversion -Wincompatible-pointer-types'
UNITS="src/frontends/lain/main.c src/tools/vra_driver.c src/tools/linearity_driver.c
       src/tools/effects_driver.c src/tools/incomplete_driver.c src/tools/lower_driver.c
       src/tools/test_vra.c src/tools/test_linearity.c src/tools/test_borrow.c
       src/tools/test_definite_init.c src/tools/test_place.c src/tools/test_ir.c
       src/tools/test_octagon.c"
bad=0
for u in $UNITS; do
    [ -f "$u" ] || continue
    # -O2, as the Makefile builds: -Wmaybe-uninitialized needs the optimiser's dataflow, so
    # checking at -O0 checked a build nobody runs and saw fewer of the bugs it exists for.
    all=$(gcc -std=c99 -O2 -Wall -Wextra $FATAL -c -o /dev/null "$u" -I src 2>&1)
    rc=$?
    # A compile ERROR must fail the gate too. Grepping only for the warning list made
    # errors INVISIBLE — a unit that does not build reports "no warnings", which is the
    # same silent-skip flaw this script exists to prevent elsewhere.
    if [ $rc -ne 0 ]; then
        echo "── $u  DOES NOT COMPILE"; echo "$all" | grep -E "error" | head -4
        bad=$((bad+1)); continue
    fi
    out=$(echo "$all" | grep -E "\[-W(return-type|uninitialized|maybe-uninitialized|implicit-function-declaration|int-conversion|incompatible-pointer-types)\]")
    if [ -n "$out" ]; then
        echo "── $u"; echo "$out" | head -8
        bad=$((bad + $(echo "$out" | wc -l)))
    fi
done
# ── A REMOVED KEYWORD IN DIAGNOSTIC PROSE ────────────────────────────────────────────────
# Diagnostic text is the documentation a user reads when stuck, and no other gate reads it:
# readme_gate and spec_gate check documents and diagnostic CODES. E127 told users to write
# `extern proc` for a week after `proc` was removed (2026-09-25), and three other strings named
# it too. A string literal that names a removed keyword must be the message saying it was
# removed. Exempt: the lexer, which recognises the keyword in order to say so, and the IR dump,
# where `proc` is the IR's own word for an impure function, not Lain syntax.
REMOVED_KEYWORDS='proc'
prose=$(grep -rnE '"[^"]*\b('"$REMOVED_KEYWORDS"')\b[^"]*"' src --include=*.h --include=*.c \
        | grep -vE '^[^:]+:[0-9]+:\s*//' | grep -vE 'removed|used to' \
        | grep -vE '^src/ir/dump\.h:|^src/frontends/lain/token\.h:')
nprose=0
if [ -n "$prose" ]; then
    echo "── diagnostic prose names a removed keyword ($REMOVED_KEYWORDS):"; echo "$prose" | head -8
    nprose=$(echo "$prose" | wc -l)
fi
echo "=================================================================="
if [ $bad -eq 0 ]; then echo "build warnings (correctness class): NONE"; else
    echo "build warnings (correctness class): $bad  ← each of these is undefined behaviour"; fi
if [ $nprose -eq 0 ]; then echo "removed keywords in diagnostic prose: NONE"; else
    echo "removed keywords in diagnostic prose: $nprose  ← a message tells the user to write it"; fi
echo "=================================================================="
[ $bad -eq 0 ] && [ $nprose -eq 0 ]
