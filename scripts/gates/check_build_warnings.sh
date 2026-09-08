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
UNITS="src/main.c src/analysis/vra_driver.c src/analysis/linearity_driver.c
       src/analysis/effects_driver.c src/analysis/incomplete_driver.c src/ir/lower_driver.c
       src/analysis/test_vra.c src/analysis/test_linearity.c src/analysis/test_borrow.c
       src/analysis/test_definite_init.c src/analysis/test_place.c src/ir/test_ir.c"
bad=0
for u in $UNITS; do
    [ -f "$u" ] || continue
    all=$(gcc -std=c99 -Wall -Wextra $FATAL -c -o /dev/null "$u" -I src 2>&1)
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
echo "=================================================================="
if [ $bad -eq 0 ]; then echo "build warnings (correctness class): NONE"; else
    echo "build warnings (correctness class): $bad  ← each of these is undefined behaviour"; fi
echo "=================================================================="
[ $bad -eq 0 ]
