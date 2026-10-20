#!/usr/bin/env bash
# judge_finder_sum_monotone.sh — a SUM of two loop versions that never rises against its entry value
# (Euclid's `t = b; b = a % b; a = t`: neither a nor b is monotone, but a % b <= a, so a + b never
# rises). With the header claim a + b <= entry(a) + entry(b), gcd's return is at most a0 + b0, and
# the guarded claim of gcd(48, 36) at most 84, so `gcd(48, 36) as i32` is proved under
# --judge=authority (tests/functions/gcd_modulo_termination_pass.ln). Its twin returns the sum's
# PARTNER grown each trip (`a = t +% 1`), which the claim no longer bounds: still refused.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cd "$ROOT" || exit 1
out=$(./lain --judge=authority tests/functions/gcd_modulo_termination_pass.ln -o "$T/g.c" 2>&1) \
    || { echo "gcd_modulo_termination_pass is refused under --judge=authority:"; echo "$out" | head -6; exit 1; }
cat > "$T/twin.ln" <<'LN'
func gcd(a0 usize, b0 usize) usize {
    var a usize = a0
    var b usize = b0
    while b > 0 decreasing b {
        var t usize = b
        b = a % b
        a = t +% 1
    }
    return a
}
func main() i32 {
    return gcd(48, 36) as i32
}
LN
out=$(./lain --judge=authority "$T/twin.ln" -o "$T/t.c" 2>&1) \
    && { echo "the twin, whose return the sum no longer bounds, is ACCEPTED under --judge=authority"; exit 1; }
echo "$out" | grep -q '\[E086\]' || { echo "the twin is refused, but not with E086:"; echo "$out" | head -6; exit 1; }
exit 0
