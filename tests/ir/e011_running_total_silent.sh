#!/usr/bin/env bash
# I.205: a running total that has no bound only because its loop may not end (no trip count, and the
# loop's own E011 or E082) is not reported at all: the program is refused by that error, and once
# the loop is shown to end the total is proven or comes back as its own E086. EXPECT-TEXT asserts
# what a message says; this asserts what it does NOT: no E086 and no `note:` beside the E011.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
for t in tests/vra/running_total_under_unbounded_loop_fail.ln tests/vra/field_bound_accumulator_grows_fail.ln; do
    out=$(cd "$ROOT" && ./lain "$t" -o /dev/null 2>&1)
    echo "$out" | grep -q '\[E011\]' || { echo "$t: no E011: $out"; exit 1; }
    if echo "$out" | grep -qE '\[E086\]|note:'; then echo "$t: the running total is reported: $out"; exit 1; fi
done
exit 0
