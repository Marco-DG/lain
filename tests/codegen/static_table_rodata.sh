#!/usr/bin/env bash
# A module constant table READ through an index is one read-only static object, not a copy made
# at every use. As a local, `CTYPE u8[256]` in `is_space(c) = (CTYPE[c] & 1) != 0` compiled at
# gcc -O2 to 16 vector loads from .rodata, 16 stores to the stack and a stack protector on every
# call, and a lexer calls it per character. Now the C names a `static const` table and is_space
# declares no 256-byte slot; on x86-64 its -O2 body is a handful of instructions and never
# touches the stack.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/t.ln" <<'LN'
CTYPE u8[256] = [(i == 32 or i == 9 or i == 10) as u8 for i in 0..256]
func is_space(c u8) bool {
    return (CTYPE[c] & 1) != 0
}
func main() i32 {
    if !is_space(32) or is_space(65) { return 1 }
    return 0
}
LN
"$LAIN" "$D/t.ln" -o "$D/t.c" >/dev/null 2>&1 || { echo "lain refused the program"; exit 1; }
grep -q 'static const uint8_t lain_ro_t_CTYPE\[256\]' "$D/t.c" || { echo "no static const table"; exit 1; }
awk '/t_is_space\(uint8_t v0\) \{/{f=1} f&&/^}/{exit} f' "$D/t.c" | grep -q '\[256\]' && { echo "is_space still declares a 256-byte slot"; exit 1; }
gcc -O2 -w -o "$D/t" "$D/t.c" && "$D/t" || { echo "the program does not run correctly"; exit 1; }
if [ "$(uname -m)" = "x86_64" ]; then
    gcc -O2 -S -w -o "$D/t.s" "$D/t.c"
    body=$(awk '/^t_is_space:/{f=1} f{print} f&&/\.cfi_endproc/{exit}' "$D/t.s")
    echo "$body" | grep -q '%rsp' && { echo "is_space touches the stack at -O2"; exit 1; }
    n=$(echo "$body" | grep -cE '^\s+[a-z]')
    [ "$n" -le 10 ] || { echo "is_space is $n instructions at -O2"; exit 1; }
fi
exit 0
