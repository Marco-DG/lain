#!/usr/bin/env bash
# A module constant whose initialiser calls a function is computed at COMPILE time (DECIDE-W step 2,
# src/frontends/lain/static_eval.h): the C holds the value, and nothing calls the function at run
# time. The run-time values are checked by tests/trust/module_const_from_call_pass.ln; this checks
# WHERE they are computed, which no output can show: a compiler that emitted the call would print
# the same numbers.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/a.ln" <<'LN'
func fib(n u32 <= 40) u32 {
    var a u32 = 0
    var b u32 = 1
    var i u32 = 0
    while i < n {
        t = a +% b
        a = b
        b = t
        i = i + 1
    }
    return a
}
func sq(x u32 <= 1000) u32 { return x * x }
F20 u32 = fib(20)
SQ u32[3] = [sq(i as u32 + 11) for i in 0..3]
func main() i32 {
    if F20 != 6765 or SQ[2] != 169 { return 1 }
    return 0
}
LN
out=$( LAIN_STATIC_EVAL_TRACE=1 "$LAIN" "$D/a.ln" -o "$D/a.c" 2>&1 ) || { echo "lain refused a.ln"; echo "$out"; exit 1; }
echo "$out" | grep -q "static-eval: 2 constant(s) computed at compile time" || { echo "expected 2 constants folded:"; echo "$out"; exit 1; }
main=$(awk '/^int main\(void\) \{/,/^}/' "$D/a.c")
[ -n "$main" ] || { echo "no main in the C"; exit 1; }
echo "$main" | grep -q "6765" || { echo "main does not hold the value 6765"; exit 1; }
for v in 121 144 169; do echo "$main" | grep -q "\b$v\b" || { echo "main does not hold the table value $v"; exit 1; }; done
if echo "$main" | grep -q "_fib(\|_sq("; then echo "main calls the function at run time"; exit 1; fi
gcc -w -o "$D/a" "$D/a.c" && "$D/a" || { echo "the program does not run to 0"; exit 1; }
exit 0
