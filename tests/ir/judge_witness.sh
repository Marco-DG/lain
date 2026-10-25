#!/usr/bin/env bash
# judge_witness.sh — PE.12's first link: for a loop the finder does not rank, fd_witness proposes
# inputs and ir_interpret_witness runs each; HANG only when the loop's header came back to a state
# it was in. The Zune clock's convert_days(366, ...) (days = 366 in a leap year, no branch moves it)
# is found and confirmed; a loop that ends for every input (Collatz from n > 1 under checked
# arithmetic, which traps on 3n + 1 past the type) never shows HANG; a loop fed by nondet() is
# confirmed with nondet() returning 1 every time (w6: an extern no pointer of the program's reaches
# can stand on a tape, when it is declared `effects io`), and one that calls an extern taking a
# pointer, or a pure one, stops at the call (`extern`);
# a ranked loop is not searched.
# A lap counter (`n = n +% 1`) that nothing in the loop depends on is left out of the state (src/ir/lap_only.h),
# so the loop around it is confirmed; the same counter checked (`n + 1`, whose overflow ends the run),
# read by a test, or handed to a call is state, and no HANG is shown.
# A recursion (w5) is run on small inputs until a call is made while a call with the same state is
# still running: rec(x, y) from rec(0, 0), a stem away from rec(0, 1); walk(var c) when c.i stays,
# and not when it rises (the object an argument reaches is part of the state); g(g(x - 1) + 1)
# from g(1), and r(0) when nondet() returns 1 every time. A recursion that ends on the inputs tried
# (Collatz from 0..4) shows none.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
gcc -std=c99 -O2 -w -o "$D/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "the driver does not build"; exit 1; }
cat > "$D/zune.ln" <<'LN'
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
func convert_days(days0 u32) i32 {
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
    return year
}
func main() i32 {
    return convert_days(10593)
}
LN
cat > "$D/collatz.ln" <<'LN'
func steps(n0 u32) u32 {
    var n u32 = n0
    var k u32 = 0
    while n > 1 {
        if n % 2 == 0 {
            n = n / 2
        } else {
            n = 3 * n + 1
        }
        k = k +% 1
    }
    return k
}
func main() i32 {
    return steps(27) as i32
}
LN
cat > "$D/extern.ln" <<'LN'
extern func nondet() i32 effects io
func spin() effects io {
    var x i32 = 1
    while x > 0 {
        x = nondet()
    }
}
func main() i32 effects io {
    spin()
    return 0
}
LN
cat > "$D/ranked.ln" <<'LN'
func count(n u32) u32 {
    var i u32 = 0
    while i < n {
        i = i + 1
    }
    return i
}
func main() i32 {
    return count(5) as i32
}
LN
for c in counted checked tested passed; do
    case $c in
        counted) step='n = n +% 1' ;;
        checked) step='n = n + 1' ;;
        tested)  step='n = n +% 1
        if n == 7 { i = 0 }' ;;
        passed)  step='n = n +% 1
        if stop(n) { i = 0 }' ;;
    esac
    cat > "$D/$c.ln" <<LN
func stop(n i32) bool { return n == 7 }
func run(x0 i32, flag i32) i32 {
    var i i32 = x0
    var n i32 = 0
    while 0 < i {
        $step
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
done
out=$(cd "$D" && ./jvc counted.ln --witness 2>&1)
echo "$out" | grep -qE '^witness counted_run bb[0-9]* \(stem 0, cycle 1\) .*: HANG$' \
    || { echo "counted: the loop around a lap counter was not confirmed:"; echo "$out"; exit 1; }
for c in checked tested passed; do
    out=$(cd "$D" && ./jvc $c.ln --witness 2>&1)
    echo "$out" | grep -q 'HANG' && { echo "$c: a counter the run depends on was left out of the state:"; echo "$out"; exit 1; }
done
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
for c in fixed:fixed_rec walk0:walk0_walk nested:nested_g fed:fed_r; do
    out=$(cd "$D" && ./jvc ${c%%:*}.ln --witness 2>&1)
    echo "$out" | grep -qE "^witness ${c#*:} recursion.*: HANG, ${c#*:} called again$" \
        || { echo "${c%%:*}: the recursion was not confirmed:"; echo "$out"; exit 1; }
done
for c in collatz_rec:collatz_rec_c walk1:walk1_walk; do
    out=$(cd "$D" && ./jvc ${c%%:*}.ln --witness 2>&1)
    echo "$out" | grep -q 'HANG' && { echo "${c%%:*}: a recursion that ends was shown to hang:"; echo "$out"; exit 1; }
    echo "$out" | grep -qx "no witness ${c#*:} recursion" || { echo "${c%%:*}: the recursion was not searched:"; echo "$out"; exit 1; }
done
out=$(cd "$D" && ./jvc zune.ln --witness 2>&1)
echo "$out" | grep -q '^witness zune_convert_days bb[0-9]* (stem 0, cycle 1) days0 = 366: HANG$' \
    || { echo "zune: convert_days(366) not found and confirmed:"; echo "$out"; exit 1; }
out=$(cd "$D" && ./jvc collatz.ln --witness 2>&1)
echo "$out" | grep -q 'HANG' && { echo "collatz: a loop that ends for every input was shown to hang:"; echo "$out"; exit 1; }
echo "$out" | grep -qE '^(no witness|witness) collatz_steps ' || { echo "collatz: the unranked loop was not searched:"; echo "$out"; exit 1; }
out=$(cd "$D" && ./jvc extern.ln --witness 2>&1)
echo "$out" | grep -qE '^witness extern_spin bb[0-9]* .* \[nondet = 1\]: HANG$' \
    || { echo "extern: the loop fed by nondet() was not confirmed on a tape:"; echo "$out"; exit 1; }
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
out=$(cd "$D" && ./jvc puts.ln --witness 2>&1)
echo "$out" | grep -q 'HANG' && { echo "puts: a loop calling an extern that takes a pointer was shown to hang:"; echo "$out"; exit 1; }
echo "$out" | grep -qE '^witness main bb[0-9]* .*: extern$' || { echo "puts: the run did not stop at the extern call:"; echo "$out"; exit 1; }
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
out=$(cd "$D" && ./jvc pure.ln --witness 2>&1)
echo "$out" | grep -q 'HANG' && { echo "pure: a loop calling a pure extern was shown to hang on a tape:"; echo "$out"; exit 1; }
echo "$out" | grep -qE '^witness main bb[0-9]* .*: extern$' || { echo "pure: the run did not stop at the pure extern:"; echo "$out"; exit 1; }
out=$(cd "$D" && ./jvc ranked.ln --witness 2>&1)
[ -z "$out" ] || { echo "ranked: a loop the finder ranks was searched for a witness:"; echo "$out"; exit 1; }
exit 0
