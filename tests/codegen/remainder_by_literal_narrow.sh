#!/usr/bin/env bash
# I.166 (Documentation, timing the Zune driver's ConvertDays against C): a signed `%` below 64 bits
# is computed in i64, so that INT_MIN % -1, undefined in C, is an ordinary 0 there. A literal
# divisor other than -1 cannot meet that case, and the widening cost a 64-bit magic multiply where
# C's own `y % 4` gets a 32-bit one. `y % 4`, `y % 100` and `y % 400` stay at the operand's width;
# a divisor that may be -1 is still widened, and INT_MIN % -1 through it is still 0.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cat > "$W/rm.ln" <<'LN'
extern func libc_printf(fmt *u8, ...) i32 effects io

func leap(y i32) bool {
    return (y % 4 == 0 and y % 100 != 0) or y % 400 == 0
}

func any_divisor(a i32, b i32 != 0) i32 {
    return a % b
}

func main() i32 effects io {
    libc_printf("%d %d %d %d %d\n", leap(2000) as i32, leap(1900) as i32, leap(2008) as i32, -7 % 4, any_divisor(-2147483647 - 1, -1))
    return 0
}
LN
( cd "$W" && "$LAIN" rm.ln -o rm.c >"$W/err" 2>&1 ) || { echo "lain refused the program"; cat "$W/err"; exit 1; }
leap="$(sed -n '/rm_leap(int32_t v0) {/,/^}/p' "$W/rm.c")"
echo "$leap" | grep -q 'int64_t' && { echo "leap's remainders are still computed in 64 bits:"; echo "$leap" | grep -n 'int64_t\|%'; exit 1; }
anyd="$(sed -n '/rm_any_divisor(int32_t v0, int32_t v1) {/,/^}/p' "$W/rm.c")"
echo "$anyd" | grep -q 'int64_t' || { echo "a divisor that may be -1 is no longer widened:"; echo "$anyd"; exit 1; }
gcc -std=c99 -fsanitize=undefined -fno-sanitize-recover=all -o "$W/rm" "$W/rm.c" -Dlibc_printf=printf -w 2>"$W/gcc" || { echo "gcc refused the C:"; head -5 "$W/gcc"; exit 1; }
out="$("$W/rm")" || { echo "the program failed"; exit 1; }
[ "$out" = "1 0 1 -3 0" ] || { echo "wrong result: $out"; exit 1; }
echo "remainder by a literal: ok"
