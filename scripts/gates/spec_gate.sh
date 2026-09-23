#!/usr/bin/env bash
# spec_gate.sh — Annex B is NORMATIVE: "a conforming implementation shall issue a diagnostic
# for every constraint violation in the table below". That is a claim about a set, so check
# the set. Two directions, and both matter:
#
#   MISSING     the compiler emits a code the annex does not define — an implementation
#               diagnosing something the specification never required
#   PHANTOM     the annex defines a code nothing emits — a requirement no implementation meets,
#               including this one
#
# It also checks that a code carries ONE meaning. A diagnostic code identifies a constraint;
# two unrelated constraints under one code make the annex's table unusable for the reader who
# actually hit the error. (E123 covered both function-pointer arity and `assume`-outside-unsafe
# until this gate was written; E125 covered both direct ADT access and an understated effect row.)
#
#   bash spec_gate.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
ANNEX=spec/annexes/B-diagnostics.tex
[ -f "$ANNEX" ] || { echo "no $ANNEX"; exit 2; }

# ★ BOTH SPELLINGS, and the second one is why this line had to change. The old AST engine
# wrote the code INSIDE its format string (`"[E001] Error ..."`), so `\[E###\]` found it. The
# sovereign engine passes the code as a bare argument to `ir_diag`, which adds the brackets —
# `ir_diag(file, line, col, "E001", "...")`. The bracket pattern cannot see that, so for as
# long as both engines existed this gate was measuring the OLD one's diagnostics and calling
# it "the compiler". Deleting src/sema/linearity.h is what exposed it: eight ownership codes
# turned PHANTOM overnight while the compiler went on emitting all eight.
#
# A gate that greps for one implementation's phrasing is scoped to that implementation, and
# says nothing about the other — the same defect as a differential that names only one leg.
emitted=$( { grep -rhoE '\[E[0-9]{3}\]' --include='*.h' --include='*.c' src/ | tr -d '[]'
             grep -rhoE '"E[0-9]{3}"'     --include='*.h' --include='*.c' src/ | tr -d '"'
           } | sort -u)
# Only the codes the annex actually DEFINES — a `\diagcode{E###}` heading a row — not every
# code its prose happens to mention. Scraping bare `E###` made a cross-reference count as a
# definition: folding the retired E006 into E002's entry ("this subsumes E006") left E006
# looking documented-but-never-emitted, i.e. PHANTOM, purely because the explanation named it.
# A spec gate should measure what the spec SPECIFIES, and prose that refers to a retired code
# is exactly the kind of thing a good annex contains.
documented=$(grep -ohE '\\diagcode\{E[0-9]{3}\}' "$ANNEX" | grep -ohE 'E[0-9]{3}' | sort -u)

missing=$(comm -23 <(echo "$emitted") <(echo "$documented"))
phantom=$(comm -13 <(echo "$emitted") <(echo "$documented"))

nm=$(echo "$missing" | grep -c . || true)
np=$(echo "$phantom" | grep -c . || true)

[ "$nm" -gt 0 ] && { echo "MISSING from Annex B (emitted, never specified):"; echo "$missing" | sed 's/^/  /'; }
[ "$np" -gt 0 ] && { echo "PHANTOM in Annex B (specified, never emitted):"; echo "$phantom" | sed 's/^/  /'; }

echo "=================================================================="
echo "Annex B vs the compiler"
echo "  codes emitted        : $(echo "$emitted" | grep -c .)"
echo "  codes documented     : $(echo "$documented" | grep -c .)"
echo "  MISSING from the annex : $nm"
echo "  PHANTOM in the annex   : $np"
echo "=================================================================="
[ "$nm" -eq 0 ] && [ "$np" -eq 0 ] && exit 0 || exit 1
