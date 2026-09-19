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
  # Every sum the program declares, by name, with how each backend spelled it.
  for nm in $(grep -ohE '\b__U_[A-Za-z0-9_]+' "$TMP/old.c" | sort -u); do
    o_niche=0; n_niche=0
    grep -qE "^struct $nm \{ int32_t tag;" "$TMP/old.c" || o_niche=1
    grep -qE "^struct $nm \{ int32_t tag;" "$TMP/new.c" || n_niche=1
    if [ "$o_niche" = "$n_niche" ]; then agree=$((agree+1));
    else
      lost=$((lost+1))
      echo "NICHE-LOST $f  $nm  old=$( [ $o_niche = 1 ] && echo packed || echo tagged )  new=$( [ $n_niche = 1 ] && echo packed || echo tagged )" >> "$TMP/lost.txt"
    fi
  done
done
echo "=================================================="
echo "LAYOUT — do both backends represent a sum the same way?"
echo "  sums agreeing    : $agree"
echo "  NICHE LOST       : $lost   <- the new backend tagged what the old one packed"
echo "  programs skipped : $skipped  (one backend refused it)"
echo "=================================================="
sort -u "$TMP/lost.txt" | head -20
[ "$lost" -eq 0 ]
