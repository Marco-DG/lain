#!/usr/bin/env bash
# fuzz_window.sh — start-and-length windows and the textbook substring search, each guard, bound and
# index off by a small constant at random, judged against ground truth for every input on a small
# domain (scripts/fuzz/fuzz_window.py has the model). An accepted program that reads out of bounds
# or underflows a subtraction from some input is UNSOUND. This is the generator for I.144's rule,
# a sum bounded through an exact difference that dominates it.
#
# TEETH (RANDOM_SEED=1, 300 programs, 74 of them safe on the domain): HEAD before I.144 accepted 0,
# I.144 accepts those 74 and nothing else. The rule's bound one too strong, in the transfer and in
# the overflow check, is caught: 107 accepted, 33 UNSOUND. Its first bracket read with the wrong
# sign (ub(a' - a) for ub(a - a')) is NOT caught: every operand generated here sits below its
# subtrahend, where that mistake only loses facts (27 accepted, 0 UNSOUND).
#
#   bash fuzz_window.sh [N]        (RANDOM_SEED fixes the programs; it is printed either way)
set -u
cd "$(dirname "$0")/../.."
[ -x ./lain ] || { echo "build first: make"; exit 2; }
N="${1:-300}"
SEED=${RANDOM_SEED:-$$}
echo "fuzz_window: RANDOM_SEED=$SEED"
python3 scripts/fuzz/fuzz_window.py ./lain "$N" "$SEED"
