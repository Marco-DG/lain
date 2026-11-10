#!/usr/bin/env bash
# judge_karr_late.sh — at a header nothing else ranked, the finder's late Karr pass (f88) finds an
# affine equality over the loop's plain paths whose new values read quotients: `d = d / 2` and
# `p = p / 2` under d = p, each quotient fixed only up to its remainder. termination-nla's hard2
# second loop, alone: r and q keep q + r = A (r - d and q + p, with d' = p'), and under it the
# loop's test is p != 1, with which p >= 1 holds and p falls. Before, every version that read d'
# or p' was left out of the equalities, no ranking was proposed, and the loop was not proved to
# end. A compile cannot show the stage, so this runs the judge's driver with --rank-stages.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
gcc -std=c99 -O2 -w -o "$T/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "judge_vc_driver does not build"; exit 1; }
cat > "$T/divide.ln" <<'LN'
func divide(A i32, p0 i32) i32 effects raises {
    if A < 0 or A >= 1073741824 or p0 < 1 or p0 >= 1073741824 {
        return 0
    }
    var r i32 = A
    var d i32 = p0
    var p i32 = p0
    var q i32 = 0
    while (((q +? r else panic("overflow")) -? A else panic("overflow")) +? p else panic("overflow")) != 1 {
        d = d / 2
        p = p / 2
        if r >= d {
            r = r -? d else panic("overflow")
            q = q +? p else panic("overflow")
        }
    }
    return q
}
LN
out=$(cd "$T" && timeout 300 ./jvc divide.ln --judge --rank-stages 2>&1)
n=$(echo "$out" | grep -c ' rank\.fall .* proved')
bad=$(echo "$out" | grep '^v1 ' | grep -v ' proved')
if [ "$n" -lt 1 ] || [ -n "$bad" ] || ! echo "$out" | grep -q '^# rank-stage divide_divide bb[0-9]* karr-late$'; then
    echo "divide: want its loop ranked by the late Karr pass and every VC proved"
    echo "$bad"; echo "$out" | grep '^# rank-stage\|^# judge'
    exit 1
fi
echo "ok: a loop whose versions read quotients is ranked under the late Karr pass's equality"
