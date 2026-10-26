#!/usr/bin/env bash
# s17: two `var` arguments of one call naming the same storage (E139 refuses it; the emitted C
# passes two restrict pointers, undefined behaviour) stop the interpreter at the call, exit 99,
# as at an obligation that does not hold: the callee's facts model the two as distinct, so its body
# must not run on aliased cells (judge_gate checked mix's post VC there and found it false). Two
# var arguments that do not overlap, a[1] and a[2], run on.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
cat > "$D/two.ln" <<'LN'
func mix(var x i32, var y i32) {
    x = x +% 10
    y = y +% 100
}
func main() i32 {
    var a i32[4] = [1, 2, 3, 4]
    mix(var a[1], var a[2])
    if a[1] != 12 or a[2] != 103 { return 1 }
    return 0
}
LN
out=$(cd "$D" && "$ROOT/lain" --interpret two.ln 2>&1); rc=$?
[ $rc -eq 0 ] || { echo "two distinct elements: expected exit 0, got $rc: $out"; exit 1; }
# the compiler refuses one element twice (E139) before it runs; judge_gate's --check-hyps runs it
gcc -std=c99 -O2 -w -o "$D/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "the driver does not build"; exit 1; }
out=$(cd "$ROOT" && "$D/jvc" tests/ownership/coarg_alias_element_fail.ln --check-hyps 2>&1)
echo "$out" | grep -q "status=99" || { echo "one element twice: expected the run to stop, status 99: $out"; exit 1; }
echo "$out" | grep -q "two var arguments of one call name the same storage" || { echo "wrong message: $out"; exit 1; }
echo "$out" | grep -q "false=0" || { echo "a false hypothesis: $out"; exit 1; }
exit 0
