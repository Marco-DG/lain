#!/usr/bin/env bash
# Each tests/borrow/escape_*_fail.ln is a program whose main READS the escaped reference after
# another call has reused the stack, so each is refused (E010) for a dangling read that would
# happen, not one that might. Should one ever compile, this runs it under `lain --interpret`
# and prints the read ("use of storage that is no longer live"), so the regression arrives with
# its witness. Every one of them compiled before the check was made total, and every one fails
# under the interpreter there.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
n=0; bad=0
for f in "$ROOT"/tests/borrow/escape_*_fail.ln; do
    n=$((n+1))
    if "$LAIN" "$f" -o "$D/x.c" > "$D/out" 2>&1; then
        echo "$(basename "$f"): compiled; under the interpreter:"
        "$LAIN" --interpret "$f" 2>&1 | grep -m1 'PROOF FAILED\|NOT MODELLED' | sed 's/^/    /'
        bad=$((bad+1)); continue
    fi
    grep -q 'E010' "$D/out" || { echo "$(basename "$f"): refused, but not with E010"; bad=$((bad+1)); }
done
[ "$n" -ge 13 ] || { echo "only $n escape programs found"; exit 1; }
[ "$bad" -eq 0 ]
