#!/usr/bin/env bash
# fuzz_cycles.sh — recursion cycles of 2 to 4 functions, several calls each, judged against ground
# truth for EVERY input (scripts/fuzz/fuzz_cycles.py has the oracle). An accepted program that can
# run for ever from some input is UNSOUND.
#
# TEETH, verified before this was trusted, over the 3000 programs of RANDOM_SEED=1:
#   - the compiler before I.130, which ranked one pair of calls per cycle: 251 accepted that can
#     run for ever (the generator without pointer calls found 25: 16 of two functions, through a
#     second call or a self-call, 4 of three, 5 of four);
#   - after I.130, whose cycle walk followed direct calls only: 222, every one through a pointer;
#   - after I.136, which makes an indirect call an edge to every address-taken function: 0, and it
#     accepts 112 of the 869 that end from every input.
#
#   bash fuzz_cycles.sh [N]        (RANDOM_SEED fixes the programs; it is printed either way)
set -u
cd "$(dirname "$0")/../.."
[ -x ./lain ] || { echo "build first: make"; exit 2; }
N="${1:-400}"
SEED=${RANDOM_SEED:-$$}
echo "fuzz_cycles: RANDOM_SEED=$SEED"
python3 scripts/fuzz/fuzz_cycles.py ./lain "$N" "$SEED"
