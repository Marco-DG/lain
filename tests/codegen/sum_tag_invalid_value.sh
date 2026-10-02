#!/usr/bin/env bash
# A tagged sum's tag must be one of its variants (spec 19). Lain makes no other, and `unsafe` can:
# `*(&s as *var u8) = 9` writes the tag of a three-variant S. That is undefined, and the C shows
# it (`case` takes no arm). The interpreter is the oracle that names it, at the load of an S
# holding 9; it had answered "not modelled" and skipped the program. A raw write of a VALID tag
# (2, C) over payload bytes the interpreter does not model stays "not modelled", never a report.
# The interpreter does not know the write's width: `*(&s as *var u32) = 257` is reported though a
# one-byte tag reads its low byte, 1. An unsafe write that depends on the layout is judged on the
# value written, which errs on the side of reporting.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
prog() {
cat <<LN
type S {
    A { x u32 }
    B { y u16 }
    C
}
func f() i32 {
    var s = S.B(3)
    unsafe {
        p = &s as *var u8
        *p = $1
    }
    case s {
        A(x): return 1
        B(y): return 2
        C: return 3
    }
    return 0
}
func main() i32 {
    return f()
}
LN
}
prog 9 > "$D/bad.ln"; prog 2 > "$D/ok.ln"
"$LAIN" --interpret "$D/bad.ln" > "$D/bad.out" 2>&1; rc=$?
[ $rc -eq 96 ] || { echo "the interpreter did not report the S holding tag 9 (status $rc)"; cat "$D/bad.out"; exit 1; }
grep -q 'none of its 3 variants' "$D/bad.out" || { echo "the report does not name the invalid tag"; cat "$D/bad.out"; exit 1; }
"$LAIN" --interpret "$D/ok.ln" > "$D/ok.out" 2>&1; ri=$?
[ $ri -ne 96 ] || { echo "a valid raw tag was reported as undefined"; cat "$D/ok.out"; exit 1; }
"$LAIN" "$D/ok.ln" -o "$D/ok.c" > /dev/null 2>&1 || { echo "lain refused the valid program"; exit 1; }
gcc -w -o "$D/ok" "$D/ok.c" -Dlibc_printf=printf -Dlibc_puts=puts || { echo "gcc failed"; exit 1; }
"$D/ok"; rc=$?
[ $rc -eq 3 ] || { echo "a raw tag 2 read as exit $rc in the C, not C (3)"; exit 1; }
exit 0
