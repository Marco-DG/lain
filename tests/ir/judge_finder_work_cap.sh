#!/usr/bin/env bash
# judge_finder_work_cap.sh — the finder's work is metered (src/solver/lia.h's meter, FD_WORK in
# finder.h): once a module's proposal has done its cap of rational operations, every query of it
# answers budget and the judge decides on what was proposed. A u8 loop with two `if`s of wrapping
# steps (fuzz_loops' wrap mode, seed 2490187, program 216) spent 2.1e9 operations, 146 s, and still
# had no ranking. Here the cap is set small (judge_vc_driver --finder-work=N) so the test is quick:
# that loop stops at it and is judged (its loop outside, as with no cap), and a plain counting loop
# stays under it and is proved whole. The cap is PER FUNCTION (an account for its facts, one for its
# termination) and one for the module's work: the two in ONE module, the heavy one first, and the
# light one is still proved whole (with one meter for the module it lost its claims, as isqrt.ln's
# `main` lost its `for` loop's ranking to its search: TCW's real programs, 2026-10-20).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
gcc -std=c99 -O2 -w -o "$T/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "judge_vc_driver does not build"; exit 1; }
cat > "$T/heavy.ln" <<'LN'
func run(x0 u8, y0 u8) u8 {
    var x = x0
    var y = y0
    while x > 0 and y > 0 {
        if x % 2 == 0 {
            y = x
        } else {
            x = x +% 1
        }
        if x % 2 == 0 {
            y = y -% 1
        } else {
            y = y +% 1
        }
    }
    return x
}

func main() i32 {
    return run(5, 7) as i32
}
LN
cat > "$T/light.ln" <<'LN'
func main() i32 {
    var s i32 = 0
    var i i32 = 0
    while i < 10 {
        s = s + i
        i = i + 1
    }
    return s - 45
}
LN
{ sed '/^func main/,$d' "$T/heavy.ln"; sed 's/^func main() i32 {/func light() i32 {/' "$T/light.ln"
  printf 'func main() i32 {\n    return run(5, 7) as i32 + light()\n}\n'; } > "$T/both.ln"
out=$(cd "$T" && timeout 60 ./jvc heavy.ln --judge --finder-work=10000000 2>&1); rc=$?
[ $rc -ne 124 ] || { echo "the heavy loop ran past 60 s with a cap of 1e7 operations"; exit 1; }
echo "$out" | grep -q '^# judge proved=' || { echo "heavy: no judge summary (rc $rc):"; echo "$out" | tail -3; exit 1; }
echo "$out" | grep -q '^# finder work=[0-9]* out=1$' || { echo "heavy: the meter did not run out:"; echo "$out" | grep '^#'; exit 1; }
echo "$out" | grep -qE '^v1 .* rank\.fall 0 outside' || { echo "heavy: its loop is not outside:"; echo "$out" | grep -v ' proved ' | head -5; exit 1; }
out=$(cd "$T" && timeout 60 ./jvc light.ln --judge --finder-work=10000000 2>&1)
echo "$out" | grep -q '^# finder work=[0-9]* out=0$' || { echo "light: the meter ran out on a plain loop:"; echo "$out" | grep '^#'; exit 1; }
echo "$out" | grep -q 'not-implied=0 outside=0 budget=0 capacity=0 solver-bug=0 uncertified=0 refused=0' \
    || { echo "light: not proved whole:"; echo "$out" | grep '^#'; exit 1; }
out=$(cd "$T" && timeout 120 ./jvc both.ln --judge --finder-work=10000000 2>&1)
echo "$out" | grep -q '^# finder work=[0-9]* out=1$' || { echo "both: the heavy function's account did not run out:"; echo "$out" | grep '^#'; exit 1; }
bad=$(echo "$out" | grep -E '^v1 both_light ' | grep -v ' proved ')
[ -z "$bad" ] && echo "$out" | grep -qE '^v1 both_light .* rank\.fall 0 proved' \
    || { echo "both: the light function lost to the heavy one's search:"; echo "${bad:-no rank.fall proved for light}" | head -5; exit 1; }
exit 0
