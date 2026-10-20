#!/usr/bin/env bash
# judge_shadow_one_place.sh — --judge=shadow compares one place once. `return n + 1` on u32: VRA files
# two overflow checks at the add (the add's own, proved, and the return's landing, refused: E086 in
# the default mode), the judge one record there under another rule (the add's narrowing, not
# implied). Each VRA check took the judge's record, so the proved one was reported "vra-only: VRA
# proves this overflow check" where both engines refuse the place (Documentation's ackermann_checked,
# 2026-10-20). One comparison per instruction and class, VRA proving it when it proves every check
# there: both refuse, no vra-only.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/inc.ln" <<'LN'
func inc(n u32) u32 {
    return n + 1
}
func main() i32 {
    if inc(5) != 6 { return 1 }
    return 0
}
LN
out=$(cd "$T" && "$ROOT/lain" --judge=shadow inc.ln -o inc.c 2>&1)
echo "$out" | grep -q '\[E086\] Error Ln 2' || { echo "the default verdict is not E086 at line 2:"; echo "$out" | head -4; exit 1; }
echo "$out" | grep -q 'judge-shadow: vra-only' && { echo "a vra-only where both refuse:"; echo "$out" | grep judge-shadow; exit 1; }
echo "$out" | grep -qE 'judge-shadow: [0-9]+ VRA checks: [0-9]+ agree \([0-9]+ proved, [1-9][0-9]* refused\)' \
    || { echo "the refused place is not an agreement:"; echo "$out" | grep judge-shadow; exit 1; }
exit 0
