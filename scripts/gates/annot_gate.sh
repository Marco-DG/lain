#!/usr/bin/env bash
# annot_gate.sh — WHAT DOES EACH BACKEND TELL THE C COMPILER?
#
# ★ WHY THIS EXISTS, AND WHY emit_gate IS NOT ENOUGH.
# `emit_gate` compares the BEHAVIOUR of the two backends: it builds each corpus program with
# both and checks that the programs print the same thing and exit the same way. It reached
# 399 agree / 0 differ / 0 build-fail, and that was read — by me, twice — as "the old emitter
# can go". It is not: two programs can behave identically while one tells the C compiler far
# less about them, and TELLING THE C COMPILER is the entire product. README §8 is a table of
# five facts derived from proofs — restrict, access, const, pure/const, nonnull — and a
# backend that drops them emits correct code that has thrown the proofs away.
#
# Measured the day this script was written:
#
#     annotation                 OLD      NEW
#     restrict                    98      123
#     __attribute__((pure))      144      108
#     __attribute__((const))     122      204
#     __attribute__((nonnull))   200        0      <-- entirely absent
#     returns_nonnull             16        0      <-- entirely absent
#     access(read_only, n, m)      4        0      <-- entirely absent
#
# So this is a PROGRESS METER like emit_gate, not a pass/fail gate: it reports a distance, and
# the distance must reach zero-or-better on every row before src/emit/ can be deleted. "Or
# better" is deliberate — the new backend emits MORE restrict and MORE const, which is a win
# and must not be read as a regression.
#
#   bash scripts/gates/annot_gate.sh [N]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
[ -x ./lain ] || { echo "build first: make"; exit 2; }
LOWER=/tmp/annot_lowerdrv
gcc -std=c99 -o "$LOWER" src/tools/lower_driver.c -I src 2>/dev/null || { echo "build lowerdrv failed"; exit 2; }
N="${1:-100000}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

KEYS=( 'restrict' '__attribute__((pure))' '__attribute__((const))' '__attribute__((nonnull))' 'returns_nonnull' 'access(read' )
# Counted and reported, but NOT counted as short — see the note at the bottom of this script.
CONST_PTR_RE='const [A-Za-z_][A-Za-z0-9_]* ?\*'
declare -A O N_
for k in "${KEYS[@]}"; do O[$k]=0; N_[$k]=0; done
prog=0; crash=0; oconst=0; nconst=0
for f in $(find tests -name '*_pass.ln' -type f | sort | head -"$N"); do
  ./lain "$f" -o "$TMP/o.c" >/dev/null 2>&1 || continue      # old engine refused: not our question
  if ! "$LOWER" "$f" --emit-c > "$TMP/n.c" 2>/dev/null; then crash=$((crash+1)); continue; fi
  prog=$((prog+1))
  for k in "${KEYS[@]}"; do
    O[$k]=$((  ${O[$k]}  + $(grep -c -F -- "$k" "$TMP/o.c") ))
    N_[$k]=$(( ${N_[$k]} + $(grep -c -F -- "$k" "$TMP/n.c") ))
  done
  oconst=$(( oconst + $(grep -cE "$CONST_PTR_RE" "$TMP/o.c") ))
  nconst=$(( nconst + $(grep -cE "$CONST_PTR_RE" "$TMP/n.c") ))
done

echo "=================================================================="
echo "What each backend TELLS THE C COMPILER   ($prog programs)"
printf "  %-28s %8s %8s   %s\n" "annotation" "OLD" "NEW" ""
short=0
for k in "${KEYS[@]}"; do
  mark=""
  if [ "${N_[$k]}" -lt "${O[$k]}" ]; then mark="<- SHORT by $(( ${O[$k]} - ${N_[$k]} ))"; short=$((short+1)); fi
  printf "  %-28s %8d %8d   %s\n" "$k" "${O[$k]}" "${N_[$k]}" "$mark"
done
printf "  %-28s %8d %8d   %s\n" "const T* (pointee)" "$oconst" "$nconst" \
       "$( [ "$nconst" -lt "$oconst" ] && echo '<- DELIBERATE, see below' )"
echo "  rows where the new backend says LESS : $short   (must be 0 before src/emit/ can go)"
[ "$crash" -gt 0 ] && echo "  new backend could not emit           : $crash"
echo "=================================================================="
echo "A meter, not a verdict: it always exits 0. The number is a distance."
cat <<'NOTE'

★ `const T*` ON THE POINTEE IS COUNTED BUT NOT COUNTED AGAINST — a DELIBERATE divergence, and
  it is listed here because an invisible one is worse than a stated one. This meter read "0 rows
  short" for a day while the new backend emitted NONE of the old one's 355, simply because it
  was not looking: the same blind spot it was built to expose, one level up.

  The argument for not emitting it is in src/ir/annot.h and it is sound: C's `const` is not an
  aliasing guarantee — it can be cast away, so no optimiser may rely on it — and qualifying a
  parameter cascades into every pointer derived from it (`&p[i]` is not const), which is real
  emitter complexity bought for nothing an optimiser can use. Its value is DOCUMENTATION: the
  emitted C says which parameters are read-only.

  So the row is informational. If it is ever decided that the emitted C should read as the old
  one did, the write footprint (`ir_param_writes`) is already the honest source for it.
NOTE

# Same contract as backend_corpus: in `make gates` since 2026-09-19, so the exit code is a
# verdict. A row going SHORT means the new backend has stopped telling the C compiler
# something the old one proved — a silent loss of exactly the proofs this project exists to
# deliver. The `const T*` row is excluded above as a stated, deliberate divergence.
if [ "$short" -ne 0 ]; then
  echo "ANNOT GATE FAILS — $short row(s) short"
  exit 1
fi
echo "ANNOT GATE HOLDS — the new backend states at least as much on every row"
