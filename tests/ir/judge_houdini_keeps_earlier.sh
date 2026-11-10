#!/usr/bin/env bash
# judge_houdini_keeps_earlier.sh — a round whose Houdini loses a claim an earlier round verified
# gives way to that round's set (f86). Once the module's meter is out, every VC of a round answers
# budget and Houdini drops all it was given: termination-nla's dijkstra3 kept 31 claims in round 0
# and none in round 1, so `while q <= n { q = 4 *? q }`, which ranks under q >= 1, was left with no
# ranking proposed. A compile cannot show the meter, so this runs the judge's driver on Dijkstra's
# integer square root with the finder's work capped (--finder-work=N, every account) where round
# 0 keeps its claims and round 1 runs out: at 3.5e6 the first loop must be ranked (measured at
# 4ddca9a4: ranked at every cap from 2.5e6 to 7e6 with f86; without it from 7e6, and at 6e6 and
# below no loop was).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
gcc -std=c99 -O2 -w -o "$T/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "judge_vc_driver does not build"; exit 1; }
cat > "$T/isqrt.ln" <<'LN'
func isqrt_steps(n i32) i32 effects raises {
    if n < 0 or n >= 1073741824 {
        return 0
    }
    var q i32 = 1
    while q <= n {
        q = 4 *? q else panic("overflow")
    }
    var p i32 = 0
    var r i32 = n
    var h i32 = 0
    while r >= ((2 *? p else panic("overflow")) +? q else panic("overflow")) {
        q = q / 4
        h = p +? q else panic("overflow")
        p = p / 2
        if r >= h {
            p = p +? q else panic("overflow")
            r = r -? h else panic("overflow")
        }
    }
    return q
}
LN
out=$(cd "$T" && timeout 120 ./jvc isqrt.ln --judge --finder-work=3500000 2>&1)
n=$(echo "$out" | grep -c ' rank\.fall .* proved')
if [ "$n" -lt 1 ]; then
    echo "isqrt under a 3.5e6 cap: want its first loop ranked under the claims round 0 kept, got none"
    echo "$out" | grep ' rank\.'
    exit 1
fi
echo "ok: the claims an earlier round verified are kept ($n loop ranked under a 3.5e6 cap)"
