#!/usr/bin/env bash
# judge_witness_note.sh — PE.12's note under E011: an input the refused loop never ends for, printed
# only after running it brought the loop's header back to a state it was in (ir_interpret_witness).
# The Zune clock's convert_days(366, t): the call as a user types it, a struct argument by its name
# and said zeroed, the repeated state by source names, the period. A loop in a function with no
# parameters: no call, one variable ("comes back"). `while flag {}` names no variable: g(true), a
# value tried because no path's facts constrain `flag`, and "no lap changes anything". A loop fed by
# nondet(): the note says it holds if nondet() returns 1 every time (w6); a loop calling an extern
# that takes a pointer, or a pure one: no note, the E011 alone. A lap counter nothing in
# the loop depends on (src/ir/lap_only.h) is left out of the repeated state, and the note says it changes.
# A recursion's E011 (w5) names an input on which a call is made while the same call is still
# running: the call made again by its arguments, or, when one is not an integer, by its name.
# A value or a cell dead at the header (not live on entry: its slot is written again before any read)
# is no part of the state: a checked `x -? y` stored in the block after its overflow test, and a
# temporary cell the lap writes first (`oldx = x`), each made the repeat read one lap late, "from the
# second lap on", and the cell was named as state (Documentation, ch4 finding 6).
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
cat > "$D/fixed.ln" <<'LN'
func rec(x i32, y i32) {
    if x < 10 {
        rec(2 *% y -% 2, x +% 1)
    }
}
func main() i32 {
    rec(5, 5)
    return 0
}
LN
cat > "$D/collatz_rec.ln" <<'LN'
func c(n u32) u32 {
    if n <= 1 {
        return 0
    }
    if n % 2 == 0 {
        return c(n / 2) +% 1
    }
    return c(3 *% n +% 1) +% 1
}
func main() i32 {
    return c(27) as% i32
}
LN
cat > "$D/fed.ln" <<'LN'
extern func nondet() i32 effects io
func r(x i32) effects io {
    if nondet() > 0 {
        r(x)
    }
}
func main() i32 effects io {
    r(0)
    return 0
}
LN
for step in 0 1; do cat > "$D/walk$step.ln" <<LN
type C { i i32, pad i32 }
func walk(var c C) {
    if c.i < 5 {
        c.i = c.i +% $step
        walk(var c)
    }
}
func main() i32 {
    var c = C(0, 0)
    walk(var c)
    return 0
}
LN
done
cat > "$D/nested.ln" <<'LN'
func g(x i32) i32 {
    if x == 0 {
        return 1
    }
    return g(g(x -% 1) +% 1)
}
func main() i32 {
    return g(0)
}
LN
for c in 'fixed|this recursion never returns for rec(0, 0): rec(0, 1) is called again before it returns' \
         'nested|this recursion never returns for g(1): g(1) is called again before it returns' \
         'walk0|this recursion never returns for walk(c), with c zeroed: walk is called again, with the same arguments, before it returns' \
         'fed|this recursion never returns for r(0) if nondet() returns 1 every time: r(0) is called again before it returns'; do
    f=${c%%|*}; want="       ${c#*|}"
    out=$(cd "$D" && "$ROOT/lain" $f.ln -o $f.c 2>&1) && { echo "$f: accepted"; exit 1; }
    echo "$out" | grep -qxF "$want" || { echo "$f: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
done
for f in collatz_rec walk1; do
    out=$(cd "$D" && "$ROOT/lain" $f.ln -o $f.c 2>&1) && { echo "$f: accepted"; exit 1; }
    echo "$out" | grep -q '^\[E011\]' || { echo "$f: no E011"; echo "$out"; exit 1; }
    echo "$out" | grep -qE 'never (ends|returns)' && { echo "$f: a note for a recursion that ends:"; echo "$out"; exit 1; }
done
# The period and stem are the RUN's: the finder's proposal assumed step(5) returns 5 (an unclaimed
# call), and the note said "after every lap" for y alternating 5, 6 (Documentation, 2026-10-26).
# From g(1), y climbs to 5 by the fifth lap, then cycles every two (three) laps.
for c in '2|6|y = 5 comes back every two laps, from the fifth lap on' '3|7|y = 5 comes back every three laps, from the fifth lap on'; do
    p=${c%%|*}; r=${c#*|}; top=${r%%|*}; want_tail=${r#*|}
    cat > "$D/cycle$p.ln" <<LN
func step(x i32) i32 {
    if x == $top {
        return 5
    }
    return x + 1
}
func g(y0 i32) i32 {
    var y = y0
    while y > 0 {
        y = step(y)
    }
    return y
}
func main() i32 {
    return g(1)
}
LN
    out=$(cd "$D" && "$ROOT/lain" cycle$p.ln -o cycle$p.c 2>&1) && { echo "cycle$p: accepted"; exit 1; }
    want="       this loop never ends for g(1): at line 9, $want_tail"
    echo "$out" | grep -qxF "$want" || { echo "cycle$p: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
done
out=$(cd "$D" && "$ROOT/lain" extern.ln -o extern.c 2>&1) && { echo "extern: accepted"; exit 1; }
want='       this loop never ends if nondet() returns 1 every time: at line 4, x = 1 comes back after every lap'
echo "$out" | grep -qxF "$want" || { echo "extern: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
cat > "$D/puts.ln" <<'LN'
extern func libc_puts(s *u8) i32 effects io
func main() i32 effects io {
    var x i32 = 1
    while x > 0 {
        libc_puts("again")
    }
    return 0
}
LN
out=$(cd "$D" && "$ROOT/lain" puts.ln -o puts.c 2>&1) && { echo "puts: accepted"; exit 1; }
echo "$out" | grep -q '^\[E011\]' || { echo "puts: no E011"; echo "$out"; exit 1; }
echo "$out" | grep -qE 'never (ends|returns)' && { echo "puts: a note for a loop calling an extern that takes a pointer:"; echo "$out"; exit 1; }
cat > "$D/pure.ln" <<'LN'
extern func pure_id(x i32) i32 effects
func main() i32 {
    var y i32 = 1
    while y > 0 {
        y = pure_id(y)
    }
    return 0
}
LN
out=$(cd "$D" && "$ROOT/lain" pure.ln -o pure.c 2>&1) && { echo "pure: accepted"; exit 1; }
echo "$out" | grep -qE 'never (ends|returns)' && { echo "pure: a note resting on a pure extern:"; echo "$out"; exit 1; }
cat > "$D/checked.ln" <<'LN'
func task(x0 i32, y0 i32) i32 effects raises {
    var x i32 = x0
    if y0 >= 0 {
        while x >= 0 {
            x = x -? y0 else panic("overflow")
        }
    }
    return 0
}
func main() i32 effects raises { return task(5, 1) }
LN
out=$(cd "$D" && "$ROOT/lain" checked.ln -o checked.c 2>&1) && { echo "checked: accepted"; exit 1; }
want='       this loop never ends for task(0, 0): at line 4, x = 0 comes back after every lap'
echo "$out" | grep -qxF "$want" || { echo "checked: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
cat > "$D/rotate.ln" <<'LN'
func spin(x0 i32, y0 i32) i32 {
    var oldx i32
    var x i32 = x0
    var y i32 = y0
    if !(x >= -2147483647) { return 0 }
    if !(y >= -2147483647) { return 0 }
    while 1 != 0 {
        oldx = x
        x = -y
        y = oldx
    }
    return 0
}
func main() i32 { return spin(1, 2) }
LN
out=$(cd "$D" && "$ROOT/lain" rotate.ln -o rotate.c 2>&1) && { echo "rotate: accepted"; exit 1; }
want='       this loop never ends for spin(0, 0): at line 7, x = 0 and y = 0 come back after every lap'
echo "$out" | grep -qxF "$want" || { echo "rotate: the note is not"; echo "$want"; echo "-- got:"; echo "$out"; exit 1; }
exit 0
