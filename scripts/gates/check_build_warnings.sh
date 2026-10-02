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
# The attributes `@io` and `@diverges`, a second spelling of the row, were removed the same way
# (I.67); `\b` cannot anchor before `@`, so they have their own alternative.
REMOVED_KEYWORDS='proc'
REMOVED_ATTRS='io|diverges'
prose=$(grep -rnE '"[^"]*(\b('"$REMOVED_KEYWORDS"')\b|@('"$REMOVED_ATTRS"')\b)[^"]*"' src --include=*.h --include=*.c \
        | grep -vE '^[^:]+:[0-9]+:\s*//' | grep -vE 'removed|used to' \
        | grep -vE '^src/ir/dump\.h:|^src/frontends/lain/token\.h:')
nprose=0
if [ -n "$prose" ]; then
    echo "── diagnostic prose names a removed keyword ($REMOVED_KEYWORDS, @$REMOVED_ATTRS):"; echo "$prose" | head -8
    nprose=$(echo "$prose" | wc -l)
fi
# ── AN ERROR WITHOUT A CODE ──────────────────────────────────────────────────────────────
# spec_gate checks that every code the compiler prints is in Annex B; an error that prints NO
# code is invisible to it. Twelve did ("sema error: struct 'S' has no field 'zzz'", most without
# a line either), found by Handwriting's M11. A front-end error starts with its [E###]; an
# internal one says "internal error"; one about the command line or a file the driver cannot
# open starts with "lain:". The rule matched only "sema error" at first, and 16 more spellings
# hid behind it ("Error: Field 'zz' not found in struct", "Compile Error: ...", "Error Ln 4,
# Col 1: ..."), five of them reachable from a program.
uncoded=$(grep -rnE 'fprintf\(stderr, *"(sema error|[Ee]rror|Compile [Ee]rror|[Ff]atal|FATAL)' src --include=*.h --include=*.c | grep -vE '^[^:]+:[0-9]+:\s*//')
nuncoded=0
if [ -n "$uncoded" ]; then
    echo "── an error message with no diagnostic code:"; echo "$uncoded" | head -8
    nuncoded=$(echo "$uncoded" | wc -l)
fi
# ── AN ERROR WITHOUT A POSITION ──────────────────────────────────────────────────────────
# A coded error says where: `[E###] Error Ln N, Col M: ...`, or `[E###] Error` followed by a
# position printed when the construct has one. A literal `[E###] Error:` has none by
# construction. Allowed, each for its reason: an anonymous union's layout refusal (E064,
# ir/emit_c.h: no source construct to point at), a function beyond an analysis's capacity (E100 in
# analysis/definite_init.h and analysis/borrow.h: an IrFunc has no line, so it names the
# function), and the --emit-llvm summary (main.c). M13 (Handwriting) measures the dynamic side.
noloc=$(grep -rnE 'fprintf\(stderr, *"\[E[0-9]+\] Error:' src --include=*.h --include=*.c \
        | grep -vE '^[^:]+:[0-9]+:\s*//' \
        | grep -vE '^src/ir/emit_c\.h:[0-9]+:.*\[E064\]' \
        | grep -vE '^src/analysis/(definite_init|borrow)\.h:[0-9]+:.*\[E100\]' \
        | grep -vE '^src/frontends/lain/main\.c:[0-9]+:.*\[E100\] Error: the LLVM path')
# Two shapes the literal rule above cannot see (Handwriting, from a count of every coded literal):
# a code spelled otherwise ("[E101] Comptime purity error", "[E085] bounds error" skipped a rule
# keyed on "Error"), and the position printed by a separate call, `"[E012] Error"` then
# `if (line) fprintf(" Ln ...")`, which printed "[E012] Error: ..." with no place when line was 0,
# a shape neither this gate nor a runtime grep for "Ln 0" could see. The idiom's next line must be
# ir_diag_locus (ir/ir.h), which names the function when there is no line.
spelled=$(grep -rnE 'fprintf\(stderr, *"\[E[0-9]+\]' src --include=*.h --include=*.c \
          | grep -vE '^[^:]+:[0-9]+:\s*//' | grep -vE '"\[E[0-9]+\] Error( Ln |:|")')
[ -n "$spelled" ] && noloc="$noloc${noloc:+
}$spelled"
idiom=$(for f in $(grep -rlE 'fprintf\(stderr, *"\[E[0-9]+\] Error"' src --include=*.h --include=*.c); do
          awk -v F="$f" '/fprintf\(stderr, *"\[E[0-9]+\] Error"/ { m = NR; next }
                         m && NR == m + 1 { if ($0 !~ /ir_diag_locus\(/) print F ":" m ": the position after it is not ir_diag_locus"; m = 0 }' "$f"
        done)
[ -n "$idiom" ] && noloc="$noloc${noloc:+
}$idiom"
nnoloc=0
if [ -n "$noloc" ]; then
    echo "── a coded error with no position (or not spelled [E###] Error):"; echo "$noloc" | head -8
    nnoloc=$(echo "$noloc" | wc -l)
fi
echo "=================================================================="
if [ $bad -eq 0 ]; then echo "build warnings (correctness class): NONE"; else
    echo "build warnings (correctness class): $bad  ← each of these is undefined behaviour"; fi
if [ $nprose -eq 0 ]; then echo "removed keywords in diagnostic prose: NONE"; else
    echo "removed keywords in diagnostic prose: $nprose  ← a message tells the user to write it"; fi
if [ $nuncoded -eq 0 ]; then echo "errors without a diagnostic code: NONE"; else
    echo "errors without a diagnostic code: $nuncoded  ← spec_gate cannot see them"; fi
if [ $nnoloc -eq 0 ]; then echo "coded errors without a position: NONE"; else
    echo "coded errors without a position: $nnoloc  ← the user is not told where"; fi
echo "=================================================================="
[ $bad -eq 0 ] && [ $nprose -eq 0 ] && [ $nuncoded -eq 0 ] && [ $nnoloc -eq 0 ]
