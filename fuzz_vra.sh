#!/usr/bin/env bash
# fuzz_vra.sh — teeth for the NEW engine's BOUNDS PROOFS. A false proof is a removed bounds
# check, and until this existed nothing could falsify one: every other fuzzer validates the
# OLD engine, and the new VRA stood on 33 corpus cases alone.
#
# Method (the same "execute what was accepted" discipline as the other fuzzers, pointed at
# the new engine):
#     1. generate a program whose index safety rests on a NEW proof rule
#     2. ask the new VRA (vradrv --suppress) whether every bounds obligation is discharged
#     3. if it says YES, emit C through the NEW pipeline and RUN it under ASan
#     4. an out-of-bounds access in a program the engine PROVED check-free is a FALSE PROOF
#
# Programs the engine does not prove are counted, not run: over-strictness is not unsound.
# The generator emits unsafe variants on purpose — a fuzzer that can only produce safe
# programs proves nothing about the prover.
#
#   bash fuzz_vra.sh [N]        # default 300 programs
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
VRADRV="${VRADRV:-/tmp/vradrv}"; LOWERDRV="${LOWERDRV:-/tmp/lowerdrv_vra}"
CC="${CC:-gcc}"; N="${1:-300}"
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
gcc -std=c99 -o "$VRADRV"   src/analysis/vra_driver.c -I src 2>/dev/null || { echo "build vradrv failed"; exit 2; }
gcc -std=c99 -o "$LOWERDRV" src/ir/lower_driver.c     -I src 2>/dev/null || { echo "build lowerdrv failed"; exit 2; }

proven=0; unproven=0; skipped=0; falseproof=0; brokenc=0; miscompile=0
mkdir -p "$TMP/w"
for i in $(seq 1 "$N"); do
  f="$TMP/w/p$i.ln"
  python3 fuzz_vra.py "$i" > "$f" 2>/dev/null || { skipped=$((skipped+1)); continue; }

  out=$("$VRADRV" "$f" --suppress 2>/dev/null) || { skipped=$((skipped+1)); continue; }
  # every BOUNDS obligation must be discharged for the program to count as "proven"
  echo "$out" | grep -q "index bounds" || { skipped=$((skipped+1)); continue; }
  if echo "$out" | grep "index bounds" | grep -q "NOT proven"; then
    unproven=$((unproven+1)); continue
  fi
  proven=$((proven+1))

  "$LOWERDRV" "$f" --emit-c --reject --suppress-bounds > "$TMP/w/p$i.c" 2>/dev/null || { brokenc=$((brokenc+1)); continue; }
  if ! $CC -std=c99 -w -fsanitize=address,undefined -fno-sanitize-recover=all \
        -o "$TMP/w/p$i" "$TMP/w/p$i.c" $DEFS 2>/dev/null; then
    brokenc=$((brokenc+1)); echo "  BROKEN-C: p$i"; cp "$f" "$TMP/keep_brokenc_$i.ln" 2>/dev/null; continue
  fi
  if ! ASAN_OPTIONS=detect_leaks=0 "$TMP/w/p$i" >/dev/null 2>"$TMP/w/p$i.err"; then
    # Distinguish the two failure modes: an OUT-OF-BOUNDS in a program the engine proved
    # check-free is a FALSE PROOF (the prover is wrong); any other fault is a MISCOMPILE in
    # the emitter (the prover may be right about a program the backend then builds wrongly).
    # Conflating them sent the first run chasing a "false proof" that was a `void*` parameter
    # doing BYTE arithmetic.
    if grep -qE "out-of-bounds|buffer-overflow|stack-overflow|SEGV" "$TMP/w/p$i.err"; then
      falseproof=$((falseproof+1))
      echo "  ★ FALSE PROOF: p$i — proven check-free, reads out of bounds"
      sed -n '1,12p' "$f" | sed 's/^/      /'
      head -3 "$TMP/w/p$i.err" | sed 's/^/      /'
    elif grep -qE "AddressSanitizer|runtime error" "$TMP/w/p$i.err"; then
      miscompile=$((miscompile+1))
      echo "  ✗ MISCOMPILE: p$i — $(grep -oE "runtime error: [^,]*" "$TMP/w/p$i.err" | head -1)"
      cp "$f" "$TMP/keep_mis_$i.ln" 2>/dev/null
    fi
  fi
done

echo "=================================================================="
echo "fuzz_vra: $N programs   proven=$proven  unproven=$unproven  skipped=$skipped"
echo "  bugs:  FALSE-PROOF=$falseproof  MISCOMPILE=$miscompile  broken-C=$brokenc"
echo "=================================================================="
[ "$falseproof" -eq 0 ] && [ "$brokenc" -eq 0 ] && [ "$miscompile" -eq 0 ]
