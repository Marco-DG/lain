#!/usr/bin/env bash
# A value of an enumeration type must be one of its variants (spec 07). Lain makes no other (no cast
# reaches an enum), C cannot reach one whose niche is used (tests/codegen/enum_niche_extern.sh), and
# `unsafe` can: `*(&k as *var u8) = 3` writes a K that is none of A, B, C. That is undefined, and it
# is visible: `Opt` stores None in K's spare value 3, so the C reads `Some(k)` as None. The
# interpreter is the oracle that names it: it reports the load of a K holding 3. A raw write of a
# VALID ordinal (2, C) is defined, and the C and the interpreter agree on it.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
prog() {
cat <<LN
type K u8 {
    A
    B
    C
}
type Opt {
    Some { k K }
    None
}
func f() i32 {
    var k = K.A
    unsafe {
        p = &k as *var u8
        *p = $1
    }
    o = Opt.Some(k)
    case o {
        Some(x): return 1
        None: return 0
    }
}
func main() i32 {
    return f()
}
LN
}
prog 3 > "$D/bad.ln"; prog 2 > "$D/ok.ln"
"$LAIN" --interpret "$D/bad.ln" > "$D/bad.out" 2>&1; rc=$?
[ $rc -eq 96 ] || { echo "the interpreter did not report the K holding 3 (status $rc)"; cat "$D/bad.out"; exit 1; }
grep -q 'none of its 3 variants' "$D/bad.out" || { echo "the report does not name the invalid enumeration value"; cat "$D/bad.out"; exit 1; }
"$LAIN" --interpret "$D/ok.ln" > /dev/null 2>&1; ri=$?
"$LAIN" "$D/ok.ln" -o "$D/ok.c" > /dev/null 2>&1 || { echo "lain refused the valid program"; exit 1; }
gcc -w -o "$D/ok" "$D/ok.c" -Dlibc_printf=printf -Dlibc_puts=puts || { echo "gcc failed"; exit 1; }
"$D/ok"; rc=$?
[ $ri -eq 1 ] && [ $rc -eq 1 ] || { echo "a valid raw ordinal: interpreter $ri, C $rc, both should read Some (1)"; exit 1; }
exit 0
