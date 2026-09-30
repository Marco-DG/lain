#!/usr/bin/env bash
# A fact the compiler ESTABLISHED reaches gcc; a fact the programmer TRUSTS does not.
#   1. `n usize >= 4` (checked at every call) and `a i32[n]` become ONE conjoined
#      `if (!(...)) __builtin_unreachable();` in the callee: one guard, because gcc 13 ignores an
#      unreachable-guard on a variable an earlier guard already used.
#   2. The program runs clean with -fsanitize=unreachable: the hint is TRUE, or this traps.
#   3. `assume(i < 16)` in `unsafe` is the programmer's word and emits no hint.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/dot.ln" <<'LN'
func dot(a i32[n], b i32[n], n usize >= 4) i32 {
    var s i32 = 0
    var i usize = 0
    while i < n {
        s = s +% (a[i] *% b[i])
        i = i + 1
    }
    return s
}
func main() i32 {
    var a i32[4] = [1, 2, 3, 4]
    return dot(a, a, 4) -% 30
}
LN
"$LAIN" "$D/dot.ln" -o "$D/dot.c" >/dev/null 2>&1 || { echo "lain refused dot.ln"; exit 1; }
body=$(awk '/int32_t dot_dot\(size_t __len/,/^}/' "$D/dot.c")
n=$(printf '%s\n' "$body" | grep -c '__builtin_unreachable')
[ "$n" = 1 ] || { echo "dot: expected ONE conjoined hint, found $n"; exit 1; }
printf '%s\n' "$body" | grep -q 'if (!(v[0-9]* & ' || { echo "dot: the hint is not conjoined"; exit 1; }
gcc -std=c11 -w -O2 -fsanitize=unreachable -fno-sanitize-recover=all -Dlibc_printf=printf \
    -o "$D/dot" "$D/dot.c" || { echo "dot: build failed"; exit 1; }
"$D/dot" 2> "$D/dot.err"; rc=$?
[ "$rc" = 0 ] || { echo "dot: exit $rc $(head -1 "$D/dot.err")"; exit 1; }
"$LAIN" "$ROOT/tests/verification/assume_in_unsafe_pass.ln" -o "$D/au.c" >/dev/null 2>&1 \
    || { echo "lain refused assume_in_unsafe_pass"; exit 1; }
t=$(grep -c '__builtin_unreachable' "$D/au.c")
[ "$t" = 0 ] || { echo "a trusted assume reached gcc ($t hints)"; exit 1; }
exit 0
