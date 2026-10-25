#!/usr/bin/env bash
# judge_witness_note.sh — PE.12's note under E011: an input the refused loop never ends for, printed
# only after running it brought the loop's header back to a state it was in (ir_interpret_witness).
# The Zune clock's convert_days(366, t): the call as a user types it, a struct argument by its name
# and said zeroed, the repeated state by source names, the period. A loop in a function with no
# parameters: no call, one variable ("comes back"). `while flag {}` names no variable: g(true), a
# value tried because no path's facts constrain `flag`, and "no lap changes anything". A loop fed by
# an extern: no note (its input is not the run's to decide), the E011 alone. A lap counter nothing in
# the loop depends on (src/ir/lap_only.h) is left out of the repeated state, and the note says it changes.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
cat > "$D/zune.ln" <<'LN'
type SystemTime { year i32, day u32 }
func is_leap_year(year i32) bool {
    var leap bool = false
    if year % 4 == 0 {
        leap = true
        if year % 100 == 0 {
            leap = year % 400 == 0
        }
    }
    return leap
}
func convert_days(days0 u32, var t SystemTime) bool {
    var days u32 = days0
    var year i32 = 1980
    while days > 365 {
        if is_leap_year(year) {
            if days > 366 {
                days = days - 366
                year = year + 1
            }
        } else {
            days = days - 365
            year = year + 1
        }
    }
    t.day = days
    t.year = year
    return true
}
func main() i32 {
    var t SystemTime = SystemTime(0, 0)
    convert_days(10593, var t)
    return 0
}
LN
cat > "$D/spin.ln" <<'LN'
func main() i32 {
    var x i32 = 3
    while x > 0 {
        x = x - 0
    }
    return 0
}
LN
cat > "$D/extern.ln" <<'LN'
extern func nondet() i32 effects io
func main() i32 effects io {
    var x i32 = 1
    while x > 0 {
        x = nondet()
    }
    return 0
}
LN
out=$(cd "$D" && "$ROOT/lain" zune.ln -o zune.c 2>&1) && { echo "zune: accepted"; exit 1; }
want='       this loop never ends for convert_days(366, t), with t zeroed: at line 15, days = 366 and year = 1980 come back after every lap'
echo "$out" | grep -qxF "$want" || { echo "zune: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
echo "$out" | grep -q '^\[E011\]' || { echo "zune: no E011"; echo "$out"; exit 1; }
out=$(cd "$D" && "$ROOT/lain" spin.ln -o spin.c 2>&1) && { echo "spin: accepted"; exit 1; }
want='       this loop never ends: at line 3, x = 3 comes back after every lap'
echo "$out" | grep -qxF "$want" || { echo "spin: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
cat > "$D/flag.ln" <<'LN'
func g(flag bool) i32 {
    while flag {}
    return 0
}
func main() i32 {
    return g(false)
}
LN
out=$(cd "$D" && "$ROOT/lain" flag.ln -o flag.c 2>&1) && { echo "flag: accepted"; exit 1; }
want='       this loop never ends for g(true): at line 2, no lap changes anything'
echo "$out" | grep -qxF "$want" || { echo "flag: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
cat > "$D/counted.ln" <<'LN'
func run(x0 i32, flag i32) i32 {
    var i i32 = x0
    var n i32 = 0
    while 0 < i {
        n = n +% 1
        if flag > 0 {
            i = i -% 1
        }
    }
    return n
}
func main() i32 {
    return run(4, 1)
}
LN
out=$(cd "$D" && "$ROOT/lain" counted.ln -o counted.c 2>&1) && { echo "counted: accepted"; exit 1; }
want='       this loop never ends for run(1, 0): at line 4, i = 1 comes back after every lap; n changes too, but nothing in the loop depends on it'
echo "$out" | grep -qxF "$want" || { echo "counted: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
cat > "$D/count.ln" <<'LN'
func g(flag bool) i32 {
    var n i32 = 0
    while flag {
        n = n +% 1
    }
    return n
}
func main() i32 {
    return g(false)
}
LN
out=$(cd "$D" && "$ROOT/lain" count.ln -o count.c 2>&1) && { echo "count: accepted"; exit 1; }
want='       this loop never ends for g(true): at line 3, no lap changes anything but n, and nothing in the loop depends on it'
echo "$out" | grep -qxF "$want" || { echo "count: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
out=$(cd "$D" && "$ROOT/lain" extern.ln -o extern.c 2>&1) && { echo "extern: accepted"; exit 1; }
echo "$out" | grep -q '^\[E011\]' || { echo "extern: no E011"; echo "$out"; exit 1; }
echo "$out" | grep -q 'never ends' && { echo "extern: a note for a loop fed by an extern:"; echo "$out"; exit 1; }
exit 0
