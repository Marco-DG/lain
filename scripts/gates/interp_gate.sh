#!/usr/bin/env bash
# interp_gate.sh — THE IR'S OWN SEMANTICS AGAINST THE C IT IS COMPILED TO.
#
# The two legs, named, because a differential whose legs become one thing passes meaninglessly
# (D-60):
#   leg 1  the emitted C, built by gcc -O2 and RUN (optimised: an aliasing claim such as
#          `restrict` only bites when gcc acts on it, and at -O0 it never does)
#   leg 2  `lain --interpret`: the same accepted IR executed by src/ir/interp.h, which models
#          values abstractly (a sum is a tag and a payload, never a niche; a struct its fields,
#          never an order) and knows nothing of the backend's representation choices
# For every corpus program the compiler accepts, both must print the same bytes and exit with
# the same status. And the interpreter checks every proof the analyses discharged as it is used
# (bounds, overflow, division, asserts, established assumes, initialisation, live storage), so a
# PROOF verdict is a false proof caught at its line.
#
# It restores what the 2026-09-23 deletions took: a differential against an INDEPENDENT
# implementation (the seven that compared two backends or two engines went with the second one).
#
# And the analysis's own STATES are checked against the run (--check-invariants,
# src/analysis/containment.h): at every block entry each value the interpreter holds must lie in
# the range analysis's invariant there, intervals and difference bounds. A discharged obligation
# is one consumer of those states; this checks the states themselves, so a wrong fact that no
# obligation in the corpus happens to read still fails the gate (INVARIANT). It found `x as bool`
# copied as if it kept the value (`7 as bool` held to be 7) on its first corpus programs.
#
#   bash scripts/gates/interp_gate.sh [-v]     exit 0 = no disagreement, no failed proof, no UB
set -u
cd "$(dirname "$0")/../.."
LAIN="$(pwd)/lain"; [ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
VERBOSE=0; [ "${1:-}" = "-v" ] && VERBOSE=1
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_calloc=calloc -Dlibc_realloc=realloc"
one() { f="$1"; d="$OUT/$(echo "$f" | md5sum | cut -c1-12)"; mkdir -p "$d"
  timeout 60 "$LAIN" "$f" -o "$d/x.c" >"$d/lain.err" 2>&1 || { echo "REJ $f"; return; }
  gcc -O2 -o "$d/x" "$d/x.c" $DEFS -w -lm 2>"$d/cc.err" || { echo "CFAIL $f"; return; }
  ( cd "$d" && timeout 20 ./x > c.out 2>/dev/null; echo $? > c.st ) 2>/dev/null
  LAIN_INTERP_STEPS=50000000 timeout 60 "$LAIN" "$f" --check-invariants > "$d/i.out" 2>"$d/i.err"; echo $? > "$d/i.st"
  # Classified by the interpreter's own message: a program may exit 99 by itself.
  if grep -q "lain --interpret: INVARIANT VIOLATED" "$d/i.err"; then echo "INVARIANT $f :: $(grep -m1 'lain --interpret' "$d/i.err")"; return; fi
  if grep -q "lain --interpret: PROOF FAILED\|lain --interpret: TRUSTED" "$d/i.err"; then echo "PROOF $f :: $(grep -m1 'lain --interpret' "$d/i.err")"; return; fi
  if grep -q "lain --interpret: UNDEFINED BEHAVIOUR" "$d/i.err"; then echo "UB $f :: $(grep -m1 'lain --interpret' "$d/i.err")"; return; fi
  if grep -q "lain --interpret: NOT MODELLED" "$d/i.err"; then echo "UNSUP $f :: $(grep -m1 'lain --interpret' "$d/i.err" | sed 's/.*NOT MODELLED[^:]*: //')"; return; fi
  if grep -q "lain --interpret: STEP BUDGET" "$d/i.err"; then echo "BUDGET $f"; return; fi
  cs=$(cat "$d/c.st"); is=$(cat "$d/i.st")
  case $is in 124|139|136) [ "$is" = "$cs" ] || { echo "CRASH($is) $f"; return; };; esac
  if [ "$cs" = "$is" ] && cmp -s "$d/c.out" "$d/i.out"; then echo "AGREE $f"; else echo "DIFF $f :: C exit $cs, interpreter exit $is"; fi; }
export -f one; export LAIN OUT DEFS
find tests -name '*_pass.ln' | sort | xargs -P 10 -I{} bash -c 'one "$@"' _ {} > "$OUT/res.txt" 2>/dev/null
n() { grep -c "^$1 " "$OUT/res.txt"; }
bad=$(( $(n DIFF) + $(n PROOF) + $(n INVARIANT) + $(n UB) + $(n BUDGET) + $(grep -c '^CRASH' "$OUT/res.txt") ))
echo "=================================================================="
echo "The IR interpreter against the emitted C (corpus _pass programs)"
echo "  agree (same output, same status) : $(n AGREE)"
echo "  DIFFER                           : $(n DIFF)   <- a backend or a semantics bug"
echo "  PROOF FAILED at run time         : $(n PROOF)   <- a false proof (or a lying contract)"
echo "  INVARIANT VIOLATED               : $(n INVARIANT)   <- the analysis's state at a block excludes the run"
echo "  undefined behaviour in unsafe    : $(n UB)"
echo "  interpreter crashed / budget     : $(( $(grep -c '^CRASH' "$OUT/res.txt") + $(n BUDGET) ))"
echo "  not modelled yet                 : $(n UNSUP)   (coverage, not a verdict)"
echo "  C did not link                   : $(n CFAIL)   (externs the harness does not define)"
echo "  rejected by the compiler         : $(n REJ)"
echo "=================================================================="
if [ $bad -gt 0 ] || [ $VERBOSE -eq 1 ]; then grep -E "^(DIFF|PROOF|INVARIANT|UB|BUDGET|CRASH|UNSUP)" "$OUT/res.txt" | sed 's/^/  /'; fi
[ $bad -eq 0 ]
