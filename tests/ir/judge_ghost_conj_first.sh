#!/usr/bin/env bash
# judge_ghost_conj_first.sh — a loop ranked over its products followed as ghosts (fd_rank_ghost) is
# tried first on each path's CONJUNCTIVE facts alone (f85): a ghost's paths carry every checked
# product's sign cases, and deciding them, a satisfiability question per case on a path of hundreds
# of facts, spent termination-nla's ps5 its termination account (343e6 of the 500e6 left it) before
# the ranking was searched; its ranking reads none of them. A compile cannot show an account, so
# this runs the judge's driver on ps2's loop (tests/functions/ghost_product_rank_pass.ln) with the
# finder's work capped (--finder-work=N, every account): at 59e6 the loop is ranked by its ghost;
# before f85 it needed 66e6, the cases' questions 7.4e6 of it (measured at 01e57448 + I.69 C: ranked
# from 55e6 with f85, from 66e6 without). Where the finder's other stages grow, re-measure N.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
gcc -std=c99 -O2 -w -o "$T/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "judge_vc_driver does not build"; exit 1; }
cp "$ROOT/tests/functions/ghost_product_rank_pass.ln" "$T/ps2.ln"
out=$(cd "$T" && timeout 120 ./jvc ps2.ln --judge --rank-stages --finder-work=59000000 2>&1)
n=$(echo "$out" | grep -c ' rank\.fall .* proved')
bad=$(echo "$out" | grep ' rank\.' | grep -v ' proved')
if [ "$n" -lt 1 ] || [ -n "$bad" ] || ! echo "$out" | grep -q '^# rank-stage ps2_steps bb[0-9]* ghost$'; then
    echo "ps2's loop under a 59e6 cap: want it ranked by its ghost, got $n rank.fall proved"
    echo "$bad"; echo "$out" | grep '^# rank-stage'
    exit 1
fi
echo "ok: ps2's loop ranked by its ghost under a 59e6 cap"
