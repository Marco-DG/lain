#!/usr/bin/env bash
# dead_code_gate.sh — no static function in src/ is unreachable from an entry point, unless
# dead_code_allow.txt names it with a reason.
#
# A dead cluster is invisible to -Wunused-function, because its functions name each other, and
# it is not free: it has to be kept compiling and kept correct, and it reads as live. W130's
# visitor outlived its warning, the front end's mutual-recursion walker outlived the obligation
# it served, and a slice-parameter table was still built for every function after its only
# reader had gone. 2026-10-02: 33 unreached static functions; 23 deleted, 10 on the allowlist.
#
# The detector (dead_code.py) judges by name; see its header for what that can and cannot see.
# The count of functions it judged is printed, and a run that judged fewer than 500 fails, so a
# parse that found nothing cannot pass.
#
#   bash scripts/gates/dead_code_gate.sh
set -u
cd "$(dirname "$0")/../.."
ALLOW=scripts/gates/dead_code_allow.txt
out=$(python3 scripts/gates/dead_code.py . 2> /tmp/dead_code_counts.$$) || { echo "the detector failed"; exit 2; }
counts=$(cat /tmp/dead_code_counts.$$); rm -f /tmp/dead_code_counts.$$
judged=$(echo "$counts" | grep -oE 'static functions: [0-9]+' | grep -oE '[0-9]+')
[ -n "$judged" ] && [ "$judged" -ge 500 ] || { echo "judged ${judged:-no} static functions: the detector read nothing"; exit 2; }
bad=0; allowed=0
while IFS= read -r line; do
    [ -z "$line" ] && continue
    file=${line%%:*}; name=${line##*: }
    ok=0
    while read -r p f _; do
        case "$p" in ''|'#'*) continue ;; esac
        if [ "$f" = "*" ] && [ "${p%/}/" = "$p" ] && [ "${file#$p}" != "$file" ]; then ok=1; break; fi
        if [ "$p" = "$file" ] && [ "$f" = "$name" ]; then ok=1; break; fi
    done < "$ALLOW"
    if [ $ok = 1 ]; then allowed=$((allowed+1)); else bad=$((bad+1)); echo "  unreached: $line"; fi
done <<< "$out"
echo "dead_code_gate: $judged static functions judged, $allowed unreached and allowed, $bad unreached and not"
[ $bad -eq 0 ] || { echo "A static function nothing reaches: delete it, or name it in $ALLOW with the reason."; exit 1; }
