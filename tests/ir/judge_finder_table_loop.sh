#!/usr/bin/env bash
# judge_finder_table_loop.sh — a loop that READS a table keeps its ranking. Once the generator states
# a local array's elements as cells, the finder's claims about convert_days' 12-entry month table
# ride into the month loop's ranking paths (16 pairs, 736 hypotheses) and the Farkas system passes
# its capacity. The per-quantity retry then tries the counter ALONE, its paths cut to the facts that
# touch it (its guard and its step), and finds `12 - month`. Checked through --judge=shadow: both
# loops' termination proved by both engines; before the retry the month loop was `outside`.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cd "$T" || exit 1
cat > cd.ln <<'LN'
type SystemTime { year i32, month i32, day u32 }
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
func convert_days(days0 u32, var t SystemTime) {
    var days u32 = days0
    var year i32 = 1980
    while days > 365 {
        if is_leap_year(year) {
            if days > 366 {
                days = days - 366
                year = year + 1
            } else {
                break
            }
        } else {
            days = days - 365
            year = year + 1
        }
    }
    var month_tab u8[12] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    if is_leap_year(year) { month_tab[1] = 29 }
    var month usize = 0
    while month < 12 {
        if days <= month_tab[month] { break }
        days = days - month_tab[month]
        month = month + 1
    }
    t.day = days
    t.month = (month + 1) as i32
    t.year = year
}
func main() i32 {
    var t SystemTime = SystemTime(0, 0, 0)
    convert_days(10593, var t)
    return 0
}
LN
out=$("$ROOT/lain" cd.ln --judge=shadow -o /dev/null 2>&1)
echo "$out" | grep -qE "2 loops' termination among them \(2 proved by both, 0 by the judge only\)" \
    || { echo "the month loop's ranking is lost: $(echo "$out" | grep -E '^judge-shadow: [0-9]+ VRA checks')"; exit 1; }
exit 0
