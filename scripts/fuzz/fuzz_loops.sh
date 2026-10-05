#!/usr/bin/env bash
# fuzz_loops.sh — random `while` loops over two u8 variables, judged against ground truth for EVERY
# input (scripts/fuzz/fuzz_loops.py has the oracle). An accepted loop that can run for ever from some
# input is UNSOUND.
#
# TEETH, verified before this was trusted: with vra_progress_on_every_path made to return true (the
# state before the multi-path fix, fuzz_termination's own teeth check), 35 of the 300 loops of
# RANDOM_SEED=1 are accepted and can run for ever; with the engine as it is, 0, and it accepts 97 of
# the 208 that end from every input.
#
#   bash fuzz_loops.sh [N]        (RANDOM_SEED fixes the programs; it is printed either way)
set -u
cd "$(dirname "$0")/../.."
[ -x ./lain ] || { echo "build first: make"; exit 2; }
N="${1:-300}"
SEED=${RANDOM_SEED:-$$}
echo "fuzz_loops: RANDOM_SEED=$SEED"
python3 scripts/fuzz/fuzz_loops.py ./lain "$N" "$SEED"
