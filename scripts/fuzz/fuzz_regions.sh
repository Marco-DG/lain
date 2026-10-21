#!/usr/bin/env bash
# fuzz_regions.sh — nondeterministic loops over two i8 variables whose paths test a sign case (the
# loops a piecewise, region ranking is for: UrbanMine-ESOP2014-Fig3), judged against ground truth for
# EVERY input and every choice (scripts/fuzz/fuzz_regions.py has the oracle, and checks its own teeth
# on Fig3 and a variant that cycles at every run). An accepted program that can run for ever from
# some input is UNSOUND. Two legs, as fuzz_loops: by default and under --judge=authority. Then both
# legs again on N/3 programs in FUZZ_REGIONS_SPLIT mode: loops split by the sign of a value they only
# read, set before the loop through up to nine joins (Toulouse-BranchesToLoop-2), d = 0 the near miss.
#
#   bash fuzz_regions.sh [N]        (RANDOM_SEED fixes the programs; it is printed either way)
set -u
cd "$(dirname "$0")/../.."
[ -x ./lain ] || { echo "build first: make"; exit 2; }
N="${1:-150}"
SEED=${RANDOM_SEED:-$$}
echo "fuzz_regions: RANDOM_SEED=$SEED"
LAIN_FLAGS=--judge=vra python3 scripts/fuzz/fuzz_regions.py ./lain "$N" "$SEED"; r1=$?
LAIN_FLAGS=--judge=authority python3 scripts/fuzz/fuzz_regions.py ./lain "$N" "$SEED"; r2=$?
M=$(( N / 3 > 0 ? N / 3 : 1 ))
FUZZ_REGIONS_SPLIT=1 LAIN_FLAGS=--judge=vra python3 scripts/fuzz/fuzz_regions.py ./lain "$M" "$SEED"; r3=$?
FUZZ_REGIONS_SPLIT=1 LAIN_FLAGS=--judge=authority python3 scripts/fuzz/fuzz_regions.py ./lain "$M" "$SEED"; r4=$?
# and the split mode with every step wrapping (s5: a wrap's cases inside a join's disjuncts), judge leg
FUZZ_REGIONS_SPLIT=1 FUZZ_REGIONS_WRAP=1 LAIN_FLAGS=--judge=authority python3 scripts/fuzz/fuzz_regions.py ./lain "$M" "$SEED"; r5=$?
[ $r1 -eq 0 ] && [ $r2 -eq 0 ] && [ $r3 -eq 0 ] && [ $r4 -eq 0 ] && [ $r5 -eq 0 ]
