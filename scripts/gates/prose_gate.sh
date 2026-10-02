#!/usr/bin/env bash
# run.sh — compile every prose probe and check it against the claim in its header.
#
# The gates check EXAMPLES and CODES. Nothing checks a SENTENCE, and every false claim found in
# these documents on 2026-10-01/02 lived in a sentence that carried no example. Each probe here
# turns one such sentence into a program that can falsify it.
#
# A probe's header gives:
#   // CLAIM:  the sentence, and the document it lives in — so a failure names what to fix
#   // EXPECT: COMPILES, or a diagnostic code like [E100]
#
# A refusal probe MUST name its code. A program refused for an unrelated reason would otherwise
# look like a passing probe, which is the failure mode that makes a green suite worthless.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
LAIN=./lain

[ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
# Same refusal the gates use: a stale binary makes every verdict here meaningless.
if stale=$(find src -type f -newer "$LAIN" -print -quit 2>/dev/null); [ -n "$stale" ]; then
    echo "REFUSING TO RUN: ./lain is older than $stale"
    echo "  Run: make"
    exit 2
fi

DIR="scripts/gates/prose_probes"
pass=0; fail=0
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

for f in "$DIR"/p*.ln; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .ln)
    expect=$(grep -m1 '^// EXPECT:' "$f" | sed 's|^// EXPECT: *||' | tr -d ' ')
    claim=$(grep -m1 '^// CLAIM:' "$f" | sed 's|^// CLAIM: *||')
    out=$("$LAIN" "$f" -o "$TMP/out.c" 2>&1)
    code=$(printf '%s' "$out" | grep -oE '^\[E[0-9]+\]' | head -1)

    if [ "$expect" = "COMPILES" ]; then
        if [ -z "$code" ]; then pass=$((pass+1))
        else
            fail=$((fail+1))
            echo "  ✗ $name — expected it to COMPILE, got $code"
            echo "      claim: $claim"
        fi
    else
        if [ "$code" = "$expect" ]; then pass=$((pass+1))
        elif [ -z "$code" ]; then
            fail=$((fail+1))
            echo "  ✗ $name — expected $expect, but it COMPILED"
            echo "      claim: $claim"
            echo "      the documented rule no longer holds; the document is now wrong"
        else
            fail=$((fail+1))
            echo "  ✗ $name — expected $expect, got $code"
            echo "      claim: $claim"
            echo "      refused, but for a different reason — the probe may have rotted"
        fi
    fi
done

echo "=================================================================="
echo "prose probes: $pass hold, $fail broken  (of $((pass+fail)))"
if [ $fail -gt 0 ]; then
    echo "A broken probe means a SENTENCE is wrong, not a program. Fix the document it names."
    exit 1
fi
echo "Every claim with a probe beside it still holds."
echo "A sentence with no probe is still a sentence nobody is testing."
