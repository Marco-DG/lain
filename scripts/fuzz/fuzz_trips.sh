#!/usr/bin/env bash
# fuzz_trips.sh — random loops over a falling measure with a running total beside it, judged against
# ground truth for EVERY input (scripts/fuzz/fuzz_trips.py has the oracle). An accepted program that
# underflows the measure, takes the total out of its type, or runs for ever from some input is UNSOUND.
#
# TEETH, verified before this was trusted (RANDOM_SEED=1, 300 programs; the engine as it is: 0 of 88
# accepted). Each sabotage of vra.h is caught: the falling trip count one too small, 12 UNSOUND; the
# every-trip-decrements test skipped, 19; two updates of the total in one trip allowed when they sit
# in different blocks, 9; and in one block, 10.
#
#   bash fuzz_trips.sh [N]        (RANDOM_SEED fixes the programs; it is printed either way)
set -u
cd "$(dirname "$0")/../.."
[ -x ./lain ] || { echo "build first: make"; exit 2; }
N="${1:-300}"
SEED=${RANDOM_SEED:-$$}
echo "fuzz_trips: RANDOM_SEED=$SEED"
python3 scripts/fuzz/fuzz_trips.py ./lain "$N" "$SEED"
