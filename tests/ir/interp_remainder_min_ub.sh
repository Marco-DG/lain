#!/usr/bin/env bash
# I.185: C makes `a % b` undefined when `a / b` is not representable (C11 6.5.5p6), so
# INT64_MIN % -1 traps natively (SIGFPE on x86). The interpreter computed it in 128 bits, got 0,
# and ran on: the oracle missed undefined behaviour the native program dies on. Inside `unsafe` it
# must now stop as undefined behaviour (exit 96), naming the remainder.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
cat > "$D/rem.ln" <<'LN'
func f(x i64, y i64) i64 {
    var r i64 = 0
    unsafe {
        r = x % y
    }
    return r
}
func main() i32 {
    r = f(-9223372036854775807 - 1, -1)
    return 0
}
LN
out=$(cd "$D" && "$ROOT/lain" --interpret rem.ln 2>&1); rc=$?
[ $rc -eq 96 ] || { echo "expected exit 96 (undefined behaviour), got $rc: $out"; exit 1; }
echo "$out" | grep -q "remainder of the type's minimum by -1" || { echo "wrong message: $out"; exit 1; }
exit 0
