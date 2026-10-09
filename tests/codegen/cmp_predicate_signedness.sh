#!/usr/bin/env bash
# I.182: the emitted C compares as the IR's predicate says, never as C's conversions decide. The
# interpreter (the IR's meaning) reads an unsigned predicate as uint64 and a signed one as the
# mathematical values (ii_cmp_scalar). The emitter printed a bare `a < b`, so C's usual conversions
# decided, and they differ exactly when an operand is negative. `for i in -3..2` lowered to an
# unsigned compare: C ran it, the interpreter did not, and the analysis sided with the interpreter.
#   1. that loop counts 5, natively and with --interpret (the parent's interpreter counts 0);
#   2. an unsigned predicate with a signed operand is printed with uint64 casts, and a signed
#      predicate with operands of mixed signedness with the sign test first;
#   3. those printed forms are exact: a C harness compares each, over boundary values, with the
#      mathematical comparison in __int128.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
fail=0
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"
cat > "$D/count.ln" <<'LN'
extern func libc_printf(fmt *u8, ...) i32 effects io
func main() i32 effects io {
    var c i32 = 0
    for i in -3..2 {
        c = c +% 1
    }
    libc_printf("%d\n", c)
    return 0
}
LN
"$LAIN" "$D/count.ln" -o "$D/count.c" > "$D/out" 2>&1 || { echo "count: lain refused it"; head -2 "$D/out"; fail=1; }
if [ -f "$D/count.c" ]; then
    gcc -w -o "$D/count" "$D/count.c" $DEFS && n=$("$D/count") || n="(build failed)"
    i=$("$LAIN" "$D/count.ln" --interpret 2>/dev/null)
    [ "$n" = 5 ] || { echo "count: native printed '$n', not 5"; fail=1; }
    [ "$i" = 5 ] || { echo "count: --interpret printed '$i', not 5"; fail=1; }
fi
# 2. the printed forms: an unsigned predicate with a signed operand, and a mixed signed predicate
cat > "$D/forms.ln" <<'LN'
func get(arr i32[10], i i32 in 0..arr.len) i32 {
    return arr[i]
}
func main() i32 {
    var a i32[10] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
    var s usize = 0
    while s < 10 {
        s = s + 1
    }
    if get(a, 5) != 5 { return 1 }
    return 0
}
LN
"$LAIN" "$D/forms.ln" -o "$D/forms.c" > "$D/out" 2>&1 || { echo "forms: lain refused it"; head -2 "$D/out"; fail=1; }
grep -qE '\(\(uint64_t\)v[0-9]+ (<|<=|>|>=) \(uint64_t\)v[0-9]+\)' "$D/forms.c" \
    || { echo "forms: no unsigned predicate printed with uint64 casts"; fail=1; }
grep -qE '\(\(v[0-9]+ (<|>=) 0\) (\|\||&&) \(\(uint64_t\)v[0-9]+ (<|<=|>|>=) \(uint64_t\)v[0-9]+\)\)' "$D/forms.c" \
    || { echo "forms: no mixed signed predicate printed with its sign test"; fail=1; }
# 3. the forms are exact
cat > "$D/exact.c" <<'C'
#include <stdint.h>
#include <stdio.h>
static const int64_t S[] = { INT64_MIN, INT64_MIN + 1, -4294967296LL, -2147483649LL, -2147483648LL, -1, 0, 1, 2147483647LL, 4294967295LL, INT64_MAX };
static const uint64_t U[] = { 0, 1, 2147483647ULL, 2147483648ULL, 4294967295ULL, 9223372036854775807ULL, 9223372036854775808ULL, UINT64_MAX };
int main(void) {
    int bad = 0;
    for (unsigned a = 0; a < sizeof S / sizeof *S; a++) for (unsigned b = 0; b < sizeof U / sizeof *U; b++) {
        int64_t s = S[a]; uint64_t u = U[b]; __int128 ms = s, mu = u;
        /* signed on the left, as the emitter prints it */
        bad += ((s < 0) || ((uint64_t)s <  (uint64_t)u)) != (ms <  mu);
        bad += ((s < 0) || ((uint64_t)s <= (uint64_t)u)) != (ms <= mu);
        bad += ((s >= 0) && ((uint64_t)s >  (uint64_t)u)) != (ms >  mu);
        bad += ((s >= 0) && ((uint64_t)s >= (uint64_t)u)) != (ms >= mu);
        /* signed on the right */
        bad += ((s >= 0) && ((uint64_t)u <  (uint64_t)s)) != (mu <  ms);
        bad += ((s >= 0) && ((uint64_t)u <= (uint64_t)s)) != (mu <= ms);
        bad += ((s < 0) || ((uint64_t)u >  (uint64_t)s)) != (mu >  ms);
        bad += ((s < 0) || ((uint64_t)u >= (uint64_t)s)) != (mu >= ms);
        /* an unsigned predicate reads both as uint64, as the interpreter does */
        bad += ((uint64_t)s < (uint64_t)u) != ((unsigned __int128)(uint64_t)s < (unsigned __int128)u);
    }
    printf("%d\n", bad);
    return bad != 0;
}
C
gcc -w -o "$D/exact" "$D/exact.c" && r=$("$D/exact") || r="(failed)"
[ "$r" = 0 ] || { echo "exact: $r comparisons differ from the mathematical ones"; fail=1; }
exit $fail
