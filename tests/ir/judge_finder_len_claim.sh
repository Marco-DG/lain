#!/usr/bin/env bash
# judge_finder_len_claim.sh — a relation VRA keeps between a loop's cell and a SLICE'S LENGTH reaches
# the judge as a claim with a JTERM_LEN term. VRA names the length by the value an IR_SLICE_LEN
# computed before the loop (`hi <= %3`); the generator names it len(<slice>), so the finder maps one
# to the other. A binary search's `a[mid]` needs `hi <= a.len` at the header: without the claim it is
# not implied, and --judge=authority refuses a correct program (E085).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cd "$T" || exit 1
cat > bs.ln <<'LN'
func lower_bound(a i32[], t i32) usize {
    var lo usize = 0
    var hi usize = a.len
    while lo < hi {
        var mid usize = lo + (hi - lo) / 2
        if a[mid] < t { lo = mid + 1 } else { hi = mid }
    }
    return lo
}
func main() i32 {
    var seven i32[7] = [1, 3, 5, 7, 9, 11, 13]
    r = lower_bound(seven, 6)
    return 0
}
LN
out=$("$ROOT/lain" bs.ln --judge=authority -o /dev/null 2>&1) || { echo "a binary search refused under authority: $out"; exit 1; }
exit 0
