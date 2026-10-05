#!/usr/bin/env bash
# fuzz_allinputs.sh — index, narrowing and division obligations judged on EVERY input of
# `run(x u8, y u8)`, and every accepted program RUN on all 65536 inputs and compared with the model
# (scripts/fuzz/fuzz_allinputs.py has both oracles).
#
# TEETH, verified before this was trusted: with report.h made to skip a failed bounds check, 15 of
# the 100 programs of RANDOM_SEED=5 are accepted and index out of bounds on some input; with the
# compiler as it is, 0, and 0 mismatches between the C and the model.
#
#   bash fuzz_allinputs.sh [N]        (RANDOM_SEED fixes the programs; it is printed either way)
set -u
cd "$(dirname "$0")/../.."
[ -x ./lain ] || { echo "build first: make"; exit 2; }
N="${1:-100}"
SEED=${RANDOM_SEED:-$$}
echo "fuzz_allinputs: RANDOM_SEED=$SEED"
python3 scripts/fuzz/fuzz_allinputs.py ./lain "$N" "$SEED"
