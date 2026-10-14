#!/usr/bin/env bash
# judge_finder_body_comparison.sh — a comparison in a loop's BODY gives the header its bound. In the
# tokenizer, `if n < m { n = n + 1 }` keeps `n <= m`, which the return refinement `usize <= m`
# needs; nothing maximized finds it (the claim is its own back edge's hypothesis), and the joins
# after each `if` must carry it too, or the back edge from them knows nothing of n. The finder
# proposes the compared value give or take one, fixed, at the header and the loop's joins.
# Under --judge=authority the tokenizer is accepted; with `n <= m` (n can reach m + 1) it is
# refused with E086, as in the default mode.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cp "$ROOT/tests/trust/tokenizer_pass.ln" "$T/tok.ln"
grep -q 'if n < m {' "$T/tok.ln" || { echo "tokenizer_pass.ln no longer has \`if n < m {\`: update this test"; exit 1; }
sed 's/if n < m {/if n <= m {/' "$T/tok.ln" > "$T/tok_fail.ln"
out=$("$ROOT/lain" --judge=authority "$T/tok.ln" -o "$T/tok.c" 2>&1) \
    || { echo "the tokenizer is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/tok_fail.ln" -o "$T/tok_fail.c" 2>&1) \
    && { echo "the tokenizer with \`n <= m\` is ACCEPTED under --judge=authority"; exit 1; }
echo "$out" | grep -q 'E086' || { echo "the tokenizer with \`n <= m\` is refused without E086:"; echo "$out" | head -8; exit 1; }
exit 0
