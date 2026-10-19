#!/usr/bin/env bash
# fuzz_loops.sh — random `while` loops over two u8 variables, judged against ground truth for EVERY
# input (scripts/fuzz/fuzz_loops.py has the oracle). An accepted loop that can run for ever from some
# input is UNSOUND.
#
# TEETH, verified before this was trusted: with vra_progress_on_every_path made to return true (the
# state before the multi-path fix, fuzz_termination's own teeth check), 40 of the 300 loops of
# RANDOM_SEED=1 are accepted and can run for ever; with the engine as it is, 0, and it accepts 84 of
# the 174 that end from every input. (The generator gained `!= 0` guards and the `x & (x - 1)`
# step with H4, which an unsigned `x != 0` guard now reaches: the engine before H4 accepts 72.)
#
#   bash fuzz_loops.sh [N]        (RANDOM_SEED fixes the programs; it is printed either way)
set -u
cd "$(dirname "$0")/../.."
[ -x ./lain ] || { echo "build first: make"; exit 2; }
N="${1:-300}"
SEED=${RANDOM_SEED:-$$}
echo "fuzz_loops: RANDOM_SEED=$SEED"
# Two legs over the same programs and the same ground truth: the verdict as compiled by default (the
# range analysis decides), and under --judge=authority (the judge decides). Until 2026-10-14 only the
# first ran, so the judge's ranking rules (c35..c37) had no ground-truth oracle at all.
python3 scripts/fuzz/fuzz_loops.py ./lain "$N" "$SEED"; r1=$?
LAIN_FLAGS=--judge=authority python3 scripts/fuzz/fuzz_loops.py ./lain "$N" "$SEED"; r2=$?
# and every update wrapping (`+%`, `-%`) on if arms, the oracle modulo 256 (s5), judge leg, on N/3
# programs: with all N the three legs ran past the full suite's 1500 s per fuzzer (seed 2490187)
M=$(( N / 3 > 0 ? N / 3 : 1 ))
FUZZ_LOOPS_WRAP=1 LAIN_FLAGS=--judge=authority python3 scripts/fuzz/fuzz_loops.py ./lain "$M" "$SEED"; r3=$?
[ $r1 -eq 0 ] && [ $r2 -eq 0 ] && [ $r3 -eq 0 ]
