#!/usr/bin/env bash
# `restrict` on a slice parameter is dropped exactly when another parameter carries a raw pointer
# that could alias its elements (tests/trust/restrict_raw_alias_pass.ln is the miscompile), and
# kept otherwise, since it is what lets gcc vectorise `out[i] = a[i] + b[i]`.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/m.ln" <<'LN'
type H {
    p *var u8
}
func raw_param(a var u8[], q *var u8) u8 {
    if a.len == 0 { return 0 }
    a[0] = 1
    unsafe { *q = 2 }
    return a[0]
}
func raw_field(a var u8[], h H) u8 {
    if a.len == 0 { return 0 }
    a[0] = 1
    unsafe { *h.p = 2 }
    return a[0]
}
func two_slices(out var u32[], a u32[]) {
    var i usize = 0
    while i < out.len and i < a.len {
        out[i] = a[i] & 255
        i = i + 1
    }
}
func unrelated(a var u32[], q *var u16) u32 {
    if a.len == 0 { return 0 }
    a[0] = 1
    unsafe { *q = 2 }
    return a[0]
}
func main() i32 { return 0 }
LN
"$LAIN" "$D/m.ln" -o "$D/m.c" >/dev/null 2>&1 || { echo "lain refused the program"; exit 1; }
# The SLICE's data pointer is what carries the qualifier (`size_t, uint8_t* restrict, ...`); a
# struct parameter's own `H* restrict` speaks for the H object, not for what `h.p` points to.
proto() { grep -m1 -E "^[^ ].*m_$1\(" "$D/m.c"; }
for fn in raw_param raw_field; do
    proto $fn | grep -q 'uint8_t\* restrict' && { echo "$fn kept restrict beside a raw pointer"; exit 1; }
done
for fn in two_slices unrelated; do
    proto $fn | grep -q 'uint32_t\* restrict' || { echo "$fn lost restrict with nothing to alias it"; exit 1; }
done
exit 0
