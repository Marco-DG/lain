#!/usr/bin/env bash
# fuzz_regions.sh — nondeterministic loops over two i8 variables whose paths test a sign case (the
# loops a piecewise, region ranking is for: UrbanMine-ESOP2014-Fig3), judged against ground truth for
# EVERY input and every choice (scripts/fuzz/fuzz_regions.py has the oracle, and checks its own teeth
# on Fig3 and a variant that cycles at every run). An accepted program that can run for ever from
# some input is UNSOUND. Two legs, as fuzz_loops: by default and under --judge=authority.
#
#   bash fuzz_regions.sh [N]        (RANDOM_SEED fixes the programs; it is printed either way)
set -u
cd "$(dirname "$0")/../.."
[ -x ./lain ] || { echo "build first: make"; exit 2; }
N="${1:-150}"
SEED=${RANDOM_SEED:-$$}
echo "fuzz_regions: RANDOM_SEED=$SEED"
python3 scripts/fuzz/fuzz_regions.py ./lain "$N" "$SEED"; r1=$?
LAIN_FLAGS=--judge=authority python3 scripts/fuzz/fuzz_regions.py ./lain "$N" "$SEED"; r2=$?
[ $r1 -eq 0 ] && [ $r2 -eq 0 ]
