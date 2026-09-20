#!/usr/bin/env bash
# layout_gate.sh — do the two backends REPRESENT a type the same way?
#
# ★ WHY THIS EXISTS, AND WHY EVERY OTHER GATE IS BLIND TO IT. `backend_corpus` compares what
# programs PRINT, and two layouts that are each internally consistent print the same thing. So
# when the IR backend became the default it silently replaced
#
#     typedef const uint8_t * __U_ptr_u8_none;            // one pointer, zero cost
#     struct __U_ptr_u8_none { int32_t tag; union { ... } data; };   // 16 bytes
#
# for every `T | markers` in the corpus — the niche optimization, which is the project's P2
# claim and the thing the README showcases — and all ten gates stayed green. The design doc
# predicted exactly this (plan item 2.2): "if two backends chose differently, the same program
# would have different runtime semantics under each, and nothing would catch it: there is no
# cross-backend layout test because there has never been a second backend."
#
# This is that test. It compares the SUM TYPEDEFS the two backends emit, per program:
#
#   NICHE-LOST   the old backend packed a sum into its payload's spare bit-patterns and the
#                new one emitted a tag+union struct. Correct, and not zero-cost.
#   AGREE        both chose the same representation
#
# It is a MEASURE until the IR backend can niche-pack (plan item 2.2, the layout pass below
# the IR); on that day it becomes a gate, and the deletion of src/emit/ waits for it.
set -u
cd "$(cd "$(dirname "$0")/../.." && pwd)"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
[ -x ./lain ] || { echo "build first"; exit 2; }

agree=0; lost=0; skipped=0
: > "$TMP/lost.txt"
for f in $(find tests std -name '*.ln' -type f | sort); do
  case "$f" in *_fail.ln|*/_tmp/*) continue;; esac
  flags=$(grep -o 'LAINFLAGS:.*' "$f" | sed 's/LAINFLAGS: *//' | head -1)
  ./lain $flags --backend=legacy "$f" -o "$TMP/old.c" >/dev/null 2>&1 || { skipped=$((skipped+1)); continue; }
  ./lain $flags --backend=ir     "$f" -o "$TMP/new.c" >/dev/null 2>&1 || { skipped=$((skipped+1)); continue; }
  # ★ EVERY TYPE BOTH BACKENDS NAME, not just the `__U_` sums. The first version of this gate
  # asked only about sums, which is the case that prompted it — and a gate that asks only
  # about the defect you already found is how the defect got in. A `[packed]` struct emitted
  # as a scalar typedef by one backend and a struct by the other is the same class of silent
  # divergence, and so is a plain enum.
  #
  # Only names present in BOTH files are compared: if the two backends do not even call a type
  # the same thing, that is a naming difference, not a layout one, and scoring it here would
  # bury the signal this gate exists for.
  # Names come from BOTH typedef spellings and BOTH files. Taking them only from
  # `^typedef ... X;` lines missed every multi-line `typedef struct X { ... } X;`, so the gate
  # could see "old scalar, new struct" and was BLIND to "old struct, new scalar" — a bias
  # toward the answer being looked for, which is the one bias a gate must not have.
  names=$( { grep -ohE '^(typedef .*|\}) *[A-Za-z_][A-Za-z0-9_]*;' "$TMP/old.c" "$TMP/new.c" \
             | sed -E 's/.*[^A-Za-z0-9_]([A-Za-z_][A-Za-z0-9_]*);/\1/'; } | sort -u)
  for nm in $names; do
    grep -qE "(^|[^A-Za-z0-9_])$nm;" "$TMP/old.c" || continue      # not named by both
    grep -qE "(^|[^A-Za-z0-9_])$nm;" "$TMP/new.c" || continue
    o_struct=0; n_struct=0
    grep -qE "struct $nm[[:space:]]*\{" "$TMP/old.c" && o_struct=1
    grep -qE "struct $nm[[:space:]]*\{" "$TMP/new.c" && n_struct=1
    if [ "$o_struct" = "$n_struct" ]; then agree=$((agree+1));
    else
      lost=$((lost+1))
      echo "LAYOUT-DIFFERS $f  $nm  old=$( [ $o_struct = 1 ] && echo struct || echo scalar )  new=$( [ $n_struct = 1 ] && echo struct || echo scalar )" >> "$TMP/lost.txt"
    fi
  done
done
echo "=================================================="
echo "LAYOUT — do both backends represent a sum the same way?"
echo "  types agreeing   : $agree"
echo "  LAYOUT DIFFERS   : $lost   <- one backend boxed what the other left flat"
echo "  programs skipped : $skipped  (one backend refused it)"
echo "=================================================="
sort -u "$TMP/lost.txt" | head -20
[ "$lost" -eq 0 ]
