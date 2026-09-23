#!/usr/bin/env bash
# baseline_gate.sh — does the compiler still produce what it produced before?
#
# Checks every corpus program against tests/BASELINE.txt: its exit code, everything it prints,
# and ★ how every type it declares is REPRESENTED (a flat scalar, or a struct).
#
# That last column is why this gate exists and why it is not redundant with the corpus or the
# trust harness. A `T | markers` that stops being niche-packed prints exactly the same thing
# and passes every other instrument — it happened (D-62: 29 sums lost their packing while TEN
# gates stayed green) and it is the project's P2 "zero cost" claim quietly going away.
# Behaviour cannot see representation; this can.
#
# It replaces the two cross-backend differentials (`backend_corpus`, `layout_gate`) that asked
# these questions against src/emit/. ★ The baseline was RECORDED from that backend before it
# was deleted, so the reference outlived the implementation — and it is a STRONGER instrument
# than the differential it replaces, because a differential is silent when both sides are
# wrong together, while a baseline states the answer.
#
# If an intended change moves a line: READ THE DIFF, then regenerate with
#   bash scripts/gates/make_baseline.sh
# Regenerating without reading it turns the baseline into a record of whatever the code
# happens to do, which is worth nothing.
set -u
cd "$(cd "$(dirname "$0")/../.." && pwd)"
BASE="tests/BASELINE.txt"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
[ -x ./lain ] || { echo "build first"; exit 2; }
[ -f "$BASE" ] || { echo "no baseline: run scripts/gates/make_baseline.sh"; exit 2; }

bash scripts/gates/make_baseline.sh "$TMP/now.txt" >/dev/null 2>&1
if diff -q "$BASE" "$TMP/now.txt" >/dev/null 2>&1; then
  echo "=================================================="
  echo "BASELINE GATE HOLDS — $(grep -c '^### ' "$BASE") programs: same output, same exit code,"
  echo "                      same representation for every type they declare"
  echo "=================================================="
  exit 0
fi
echo "=================================================="
echo "BASELINE GATE FAILS — the compiler's output moved"
echo "=================================================="
diff "$BASE" "$TMP/now.txt" | head -40
echo "--- (a 'ty=' line means a TYPE changed shape: a niche packing gained or lost)"
exit 1
