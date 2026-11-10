#!/usr/bin/env bash
# judge_checked_cast_owed.sh — `v as? T else e` lowers to two range tests and a CHECKED cast past
# them (I.69 C): the cast owes its narrowing like any other, and VRA and the judge discharge it
# from the two tests. Before, the lowering flagged that cast unchecked, the readers took it as a
# wrap, and nothing was owed: the proof the tests make was never checked. `f` below has no other
# narrowing (its `else` arm panics: no value lands), so its narrow.lo and narrow.hi records are the
# cast's, and each must be proved.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
gcc -std=c99 -O2 -w -o "$T/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "judge_vc_driver does not build"; exit 1; }
cat > "$T/asq.ln" <<'LN'
func f(v i32) u8 effects raises {
    return v as? u8 else panic("out of range")
}
func main() i32 effects raises {
    return f(200)
}
LN
out=$(cd "$T" && "$T/jvc" asq.ln --judge 2>&1)
lo=$(echo "$out" | grep -c '^v1 asq_f .* narrow\.lo [0-9]* proved')
hi=$(echo "$out" | grep -c '^v1 asq_f .* narrow\.hi [0-9]* proved')
bad=$(echo "$out" | grep '^v1 ' | grep -v ' proved')
if [ "$lo" -ne 1 ] || [ "$hi" -ne 1 ] || [ -n "$bad" ]; then
    echo "as? u8 from an i32: want its cast's narrow.lo and narrow.hi owed and proved, got $lo and $hi"
    echo "$out" | grep '^v1 '
    exit 1
fi
echo "ok: the checked cast past as?'s two tests owes its narrowing, and the judge proves it"
