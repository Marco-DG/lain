#!/usr/bin/env bash
# A sum returned from the function that built it from a local array must not read that dead
# frame: its array payload is stored inline (tests/trust/sum_array_payload_pass.ln). run_trust
# compiles at -O1 without ASan's use-after-return check, so this runs the shape at -O0 and -O2
# with it, and under `lain --interpret`, which reported "use of storage that is no longer live".
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/m.ln" <<'LN'
type Sh {
    Arr { a u8, xs u32[3] }
    Flat
}
func mk(k u32 <= 100) Sh {
    xs u32[3] = [k, k + 1, k + 2]
    return Sh.Arr(1, xs)
}
func clobber(k u32) u32 {
    ys u32[8] = [k, k, k, k, k, k, k, k]
    return ys[3]
}
func main() i32 {
    s = mk(10)
    c = clobber(77)
    case s {
        Arr(a, xs):
            if xs[0] != 10 or xs[2] != 12 { return 1 }
            return 0
        Flat: return 2
    }
}
LN
"$LAIN" "$D/m.ln" -o "$D/m.c" >/dev/null 2>&1 || { echo "lain refused the program"; exit 1; }
for o in -O0 -O2; do
    gcc $o -fsanitize=address -fno-omit-frame-pointer -w -o "$D/m$o" "$D/m.c" -Dlibc_printf=printf -Dlibc_puts=puts \
        || { echo "gcc $o failed"; exit 1; }
    ASAN_OPTIONS=detect_stack_use_after_return=1 "$D/m$o" > "$D/out$o" 2>&1; rc=$?
    [ $rc -eq 0 ] || { echo "gcc $o: exit $rc"; grep -m1 'ERROR' "$D/out$o"; exit 1; }
done
"$LAIN" --interpret "$D/m.ln" > "$D/interp" 2>&1; rc=$?
[ $rc -eq 0 ] || { echo "lain --interpret: exit $rc"; grep -m1 'PROOF\|NOT MODELLED' "$D/interp"; exit 1; }
exit 0
