#!/usr/bin/env bash
# I.195: an object whose ELEMENT is too big to shape. The interpreter capped an object's element
# count, not the cells under each element, so a local struct of two `u64[2147483647]` fields was
# shaped field by field until a calloc failed unchecked and the shaping wrote through NULL: the
# interpreter (and every harness built on it, judge_gate's --check-hyps among them) died with
# SIGSEGV on a program the compiler accepts. It must answer NOT MODELLED (exit 98) instead.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
cat > "$D/huge.ln" <<'LN'
type B {
    f0 u64[2147483647]
    f1 u64[2147483647]
}
func f() u64 {
    var b B
    b.f0[0] = 7
    return b.f0[0]
}
func main() i32 {
    if f() != 7 { return 1 }
    return 0
}
LN
out=$(cd "$D" && timeout 60 "$ROOT/lain" --interpret huge.ln 2>&1); rc=$?
[ $rc -eq 98 ] || { echo "expected exit 98 (not modelled), got $rc: $out"; exit 1; }
echo "$out" | grep -q "NOT MODELLED" || { echo "wrong message: $out"; exit 1; }
exit 0
