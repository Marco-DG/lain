#!/usr/bin/env bash
# fuzz_soundness.sh — the PER-OPERATION SOUNDNESS HARNESS (src/tools/soundness_driver.c).
#
# The range analysis is an abstraction of the IR's semantics, and the interpreter (ir/interp.h)
# is that semantics, executable; so soundness is a relation between the two, tested one IR
# operation at a time: on random abstract inputs, the range the analysis gives a result must
# contain every value the interpreter computes, every difference bound it holds must hold, and
# every obligation it discharged must not fail when the interpreter checks it. See the driver's
# header for the four verdicts.
#
# Every other fuzzer here tests WHOLE PROGRAMS a generator can write, and its zero is about its
# generator: on its first run this one found four false proofs that all of them passed (an
# unsigned quotient by a non-constant divisor, a signed remainder by a negative divisor, a
# same-width unsigned-to-signed cast, @ctz/@clz of zero), and a gap in the interpreter's own
# checks (a narrowing where a value LANDS, which it wrapped exactly as C does).
#
# The driver is built under AddressSanitizer: a query that reads outside the analysis's state is
# a bug too, and it found one in the driver itself.
#
#   bash fuzz_soundness.sh [SEEDS] [TRIALS]      default 5 seeds x 4000 trials
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
SEEDS="${1:-5}"; TRIALS="${2:-4000}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
DRV="$TMP/soundness"          # in this run's own directory: never a fixed /tmp path
gcc -std=c99 -O1 -fsanitize=address -fno-omit-frame-pointer -w -o "$DRV" src/tools/soundness_driver.c -I src \
  || { echo "build soundness_driver failed"; exit 2; }
bugs=0; trials=0; samples=0; cov=""
for seed in $(seq 1 "$SEEDS"); do
  ASAN_OPTIONS=detect_leaks=0 "$DRV" "$seed" "$TRIALS" > "$TMP/s$seed.txt" 2>&1; rc=$?
  if grep -q "AddressSanitizer" "$TMP/s$seed.txt"; then
    echo "seed $seed: AddressSanitizer report"; grep -A8 "ERROR: AddressSanitizer" "$TMP/s$seed.txt" | head -12; bugs=$((bugs+1)); continue
  fi
  b=$(grep -o '^bugs: [0-9]*' "$TMP/s$seed.txt" | grep -o '[0-9]*$')
  [ -n "$b" ] || { echo "seed $seed: no summary (rc=$rc)"; tail -3 "$TMP/s$seed.txt"; bugs=$((bugs+1)); continue; }
  if [ "$b" -gt 0 ]; then
    echo "seed $seed: $b finding(s)"; grep -m6 -A2 '^FINDING' "$TMP/s$seed.txt"; tail -2 "$TMP/s$seed.txt"
  fi
  bugs=$((bugs+b))
  t=$(grep -o '^soundness: seed [0-9]*, [0-9]* trials, [0-9]* samples' "$TMP/s$seed.txt" | grep -o '[0-9]* trials, [0-9]* samples')
  trials=$((trials + $(echo "$t" | cut -d' ' -f1))); samples=$((samples + $(echo "$t" | cut -d' ' -f3)))
  cov="$cov $(grep -o '[0-9]* of [0-9]* results' "$TMP/s$seed.txt" | cut -d' ' -f1)/$(grep -o '[0-9]* of [0-9]* results' "$TMP/s$seed.txt" | cut -d' ' -f3)"
done
echo "=================================================================="
echo "per-operation soundness: $SEEDS seeds, $trials trials, $samples concrete samples"
echo "  results with a range narrower than their type, per seed:$cov"
echo "  bugs: $bugs (a finding is an unsound transfer, a discharged obligation that fails, or a missing one)"
echo "=================================================================="
[ "$bugs" -eq 0 ]
