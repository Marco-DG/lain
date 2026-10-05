#!/usr/bin/env bash
# The interpreter recurses on lain's own stack, one ii_call per interpreted call, and nothing bounded
# it: a recursion a few hundred calls deep crashed lain itself (SIGSEGV, exit 139), both while
# COMPILING a module constant (`K = f(300)`) and under `lain --interpret`, where the native binary
# went 10000 deep. Spec 04 promises a compile-time evaluation depth of at least 256. Now a recursion
# within the stack runs, and one past it stops with a diagnostic: E136 for a constant, status 97
# under --interpret, for a never-ending mutual pair too (Main Compiler Worker's I.130 reproducers).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cd "$D"
fn='func f(n i32 >= 0 and <= 100000) i32 {
    if n == 0 { return 0 }
    return f(n - 1)
}'
fail=0
konst() { printf '%s\n\nK = f(%s)\n\nfunc main() i32 {\n    return K\n}\n' "$fn" "$1" > k.ln; }
konst 1000
"$LAIN" k.ln -o k.c > out 2>&1; rc=$?
[ $rc -eq 0 ] || { echo "K = f(1000): exit $rc"; head -2 out; fail=1; }
konst 100000
"$LAIN" k.ln -o k.c > out 2>&1; rc=$?
{ [ $rc -eq 1 ] && grep -q "^\[E136\].*constant 'K' recursed [0-9]* calls deep" out; } || { echo "K = f(100000): exit $rc, not E136 for its depth"; head -2 out; fail=1; }
printf '%s\nfunc main() i32 {\n    return f(1000)\n}\n' "$fn" > r.ln
"$LAIN" r.ln --interpret > out 2>&1; rc=$?
[ $rc -eq 0 ] || { echo "--interpret f(1000): exit $rc"; head -2 out; fail=1; }
printf '%s\nfunc main() i32 {\n    return f(100000)\n}\n' "$fn" > r.ln
"$LAIN" r.ln --interpret > out 2>&1; rc=$?
{ [ $rc -eq 97 ] && grep -q 'RECURSION TOO DEEP' out; } || { echo "--interpret f(100000): exit $rc, not 97 RECURSION TOO DEEP"; head -2 out; fail=1; }
cat > m.ln <<'LN'
func ping(n i32) i32 effects diverge {
    return pong(n)
}
func pong(n i32) i32 effects diverge {
    return ping(n)
}
func main() i32 effects diverge {
    return ping(1)
}
LN
"$LAIN" m.ln --interpret > out 2>&1; rc=$?
{ [ $rc -eq 97 ] && grep -q 'RECURSION TOO DEEP' out; } || { echo "--interpret ping/pong: exit $rc, not 97 RECURSION TOO DEEP"; head -2 out; fail=1; }
[ $fail -eq 0 ] && echo "interpreter recursion: deep calls run, deeper ones stop with E136 or 97"
exit $fail
