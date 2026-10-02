#!/usr/bin/env bash
# An enumeration an extern's signature reaches donates no niche (layout.h, ir_pool_for). The niche
# of `type K u8 { A, B, C }` is the values 3..255, sound only while no K holds one, and Lain can
# make none: no cast reaches an enum. C can. Here C's `getk` returns 3, the value Opt would use
# for None if K donated its niche, and `Opt.Some(getk())` would silently read as None. Linked
# against a C definition and run: it must read Some, and Opt must carry its tag.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/m.ln" <<'LN'
type K u8 {
    A
    B
    C
}
type Opt {
    Some { k K }
    None
}
extern func getk() K effects
func main() i32 {
    o = Opt.Some(getk())
    case o {
        Some(k): return 1
        None: return 0
    }
}
LN
"$LAIN" "$D/m.ln" -o "$D/m.c" > "$D/out" 2>&1 || { echo "lain refused the program"; cat "$D/out"; exit 1; }
grep -qE 'struct Opt \{ u?int[0-9]+_t tag;' "$D/m.c" || { echo "Opt donated K's niche although C can hand back any K"; grep -n 'Opt' "$D/m.c" | head -3; exit 1; }
grep -q "an extern's signature reaches" "$D/out" || { echo "W120 does not say why Opt keeps its tag"; cat "$D/out"; exit 1; }
printf '\nK getk(void) { return (K)3; }\n' >> "$D/m.c"
gcc -w -o "$D/m" "$D/m.c" -Dlibc_printf=printf -Dlibc_puts=puts || { echo "gcc failed"; exit 1; }
"$D/m"; rc=$?
[ $rc -eq 1 ] || { echo "Some(getk()) read as None (exit $rc): a K from C held the niche value"; exit 1; }
exit 0
