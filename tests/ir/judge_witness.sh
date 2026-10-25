#!/usr/bin/env bash
# judge_witness.sh — PE.12's first link: for a loop the finder does not rank, fd_witness proposes
# inputs and ir_interpret_witness runs each; HANG only when the loop's header came back to a state
# it was in. The Zune clock's convert_days(366, ...) (days = 366 in a leap year, no branch moves it)
# is found and confirmed; a loop that ends for every input (Collatz from n > 1 under checked
# arithmetic, which traps on 3n + 1 past the type) never shows HANG; a loop fed by an extern stops
# at the call (`extern`), its input not the run's to decide; a ranked loop is not searched.
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
out=$(cd "$D" && ./jvc zune.ln --witness 2>&1)
echo "$out" | grep -q '^witness zune_convert_days bb[0-9]* (stem 0, cycle 1) days0 = 366: HANG$' \
    || { echo "zune: convert_days(366) not found and confirmed:"; echo "$out"; exit 1; }
out=$(cd "$D" && ./jvc collatz.ln --witness 2>&1)
echo "$out" | grep -q 'HANG' && { echo "collatz: a loop that ends for every input was shown to hang:"; echo "$out"; exit 1; }
echo "$out" | grep -qE '^(no witness|witness) collatz_steps ' || { echo "collatz: the unranked loop was not searched:"; echo "$out"; exit 1; }
out=$(cd "$D" && ./jvc extern.ln --witness 2>&1)
echo "$out" | grep -q 'HANG' && { echo "extern: a loop fed by an extern was shown to hang:"; echo "$out"; exit 1; }
echo "$out" | grep -qE '^witness extern_spin bb[0-9]* .*: extern$' || { echo "extern: the run did not stop at the extern call:"; echo "$out"; exit 1; }
out=$(cd "$D" && ./jvc ranked.ln --witness 2>&1)
[ -z "$out" ] || { echo "ranked: a loop the finder ranks was searched for a witness:"; echo "$out"; exit 1; }
exit 0
