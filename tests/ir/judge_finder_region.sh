#!/usr/bin/env bash
# judge_finder_region.sh — a loop whose paths do not fit the finder's first region is built again
# in a larger one (f84, fd_loop_region: 4 MB, then 32 MB), never left with no ranking proposed.
# termination-nla's ps4 flattened to 23 paths past 4 MB and was refused (E011) for that alone. A
# compile cannot show the region, so this runs the judge's driver with its first region set small
# (--finder-region=4096): Dijkstra's integer square root below, whose two loops only the finder
# ranks, must keep no rank.* record outside (without the retry both loops had none proposed).
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
out=$(cd "$T" && "$T/jvc" isqrt.ln --judge --finder-region=4096 2>&1)
n=$(echo "$out" | grep -c ' rank\.fall .* proved')
bad=$(echo "$out" | grep ' rank\.' | grep -v ' proved')
if [ "$n" -lt 2 ] || [ -n "$bad" ]; then
    echo "a loop's paths past the first region: want both loops ranked, got $n rank.fall proved"
    echo "$bad"
    exit 1
fi
echo "ok: both loops ranked from a 4 KB first region"
