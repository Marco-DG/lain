#!/usr/bin/env bash
# judge_finder_regions_fixed.sh — regions whose guard compares a version with a value fixed at the
# header, and a region no trip stays in (CookSeeZuleger-TACAS2013 Fig8b). `while x != M`: below M, x
# rises by 1 and <M - x> falls; above M, every trip sets x = 0, so no trip stays above M and that
# region owes its tuple under guards no path meets. Neither side alone, nor a max or min, ranks it.
# Two twins that can run for ever must be refused with E011 alone: 'stall', whose x > M path leaves x
# (it stays in the region it should leave), and 'over', whose x < M path steps by 2 and can jump from
# M - 1 to M + 1 (a move up), whence x = 0 starts again.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
fig8b() {   # $1: the x > M path, $2: the x < M path's step
cat <<LN
func task(x0 i32, M i32) effects raises {
    var x = x0
    if M > 0 {
        while x != M {
            if x > M {
                $1
            } else {
                x = x +? $2 else panic("overflow")
            }
        }
    }
}
func main() i32 effects raises {
    task(7, 3)
    return 0
}
LN
}
fig8b 'x = 0' 1 > "$T/pass.ln"
fig8b 'x = x' 1 > "$T/stall.ln"
fig8b 'x = 0' 2 > "$T/over.ln"
out=$("$ROOT/lain" --judge=authority "$T/pass.ln" -o "$T/pass.c" 2>&1) \
    || { echo "Fig8b is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
for tw in stall over; do
    out=$("$ROOT/lain" --judge=authority "$T/$tw.ln" -o "$T/$tw.c" 2>&1) \
        && { echo "Fig8b's twin '$tw', which can run for ever, is ACCEPTED under --judge=authority"; exit 1; }
    codes=$(echo "$out" | grep -o 'E[0-9][0-9][0-9]' | sort -u | tr '\n' ' ')
    [ "$codes" = "E011 " ] || { echo "Fig8b's twin '$tw' is refused with [$codes], not E011 alone:"; echo "$out" | head -8; exit 1; }
done
exit 0
