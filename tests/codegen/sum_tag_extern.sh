#!/usr/bin/env bash
# A tagged sum's tag is the smallest integer that holds its variants (tests/trust/sum_tag_width_pass.ln),
# except where C reads the sum: one an extern's signature reaches keeps the `int32_t tag` and the
# payload after it that C was written against (layout.h, ir_sum_tag_bits). Here C's `fill` writes
# B(42) into a Lain S through that layout. Narrowed, the tag would read 1 and `y` would read byte 1,
# a padding byte of the C struct (0), so the program would return 0.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/m.ln" <<'LN'
type S {
    A { x u8 }
    B { y u8 }
    C
}
extern func fill(p *var S) effects
func main() i32 {
    var s = S.C
    unsafe {
        fill(&s)
    }
    case s {
        A(x): return 10
        B(y): return y as i32
        C: return 20
    }
}
LN
"$LAIN" "$D/m.ln" -o "$D/m.c" > "$D/out" 2>&1 || { echo "lain refused the program"; cat "$D/out"; exit 1; }
grep -q 'struct S { int32_t tag;' "$D/m.c" || { echo "S lost the int32_t tag C reads"; grep -n 'struct S {' "$D/m.c"; exit 1; }
grep -q "carries an int32_t tag (4 bytes)" "$D/out" || { echo "W120 does not name the tag S carries"; cat "$D/out"; exit 1; }
cat >> "$D/m.c" <<'C'
struct S_c { int32_t tag; union { struct { uint8_t x; } A; struct { uint8_t y; } B; } data; };
void fill(S *p) { struct S_c *q = (struct S_c *)p; q->tag = 1; q->data.B.y = 42; }
C
gcc -w -o "$D/m" "$D/m.c" -Dlibc_printf=printf -Dlibc_puts=puts || { echo "gcc failed"; exit 1; }
"$D/m"; rc=$?
[ $rc -eq 42 ] || { echo "C wrote B(42) and Lain read exit $rc: the tag C writes is not the one Lain reads"; exit 1; }
exit 0
