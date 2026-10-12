#!/usr/bin/env bash
# I.202: a vector shift's amount is owed as a scalar's (spec 07: a scalar in [0, w-1], E086), and
# the C shifts lane by lane, undefined past the width. The interpreter read a lane shifted past its
# width as 0 and ran on, so the oracle found nothing where the native program is undefined, and the
# judge's generator, which takes its rows from the interpreter, owed nothing. Inside `unsafe` it
# must now stop as undefined behaviour (exit 96), worded as a scalar's shift is.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
cat > "$D/lane.ln" <<'LN'
func f(a u32x4, s u32) u32x4 {
    var r u32x4 = a
    unsafe {
        r = a << s
    }
    return r
}
func main() i32 {
    var a u32x4 = [1, 2, 3, 4]
    r = f(a, 40)
    return (r[0] & 1) as i32
}
LN
out=$(cd "$D" && "$ROOT/lain" --interpret lane.ln 2>&1); rc=$?
[ $rc -eq 96 ] || { echo "expected exit 96 (undefined behaviour), got $rc: $out"; exit 1; }
echo "$out" | grep -q "shift by 40, outside \[0, 32)" || { echo "wrong message: $out"; exit 1; }
exit 0
